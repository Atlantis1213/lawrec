#include <elf.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool pass, const char *message) { if (!pass) throw std::runtime_error(message); }
struct Image {
    std::vector<uint8_t> bytes;
    template<class T> T read(uint64_t offset) const {
        require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset, "truncated ELF structure");
        T value; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value;
    }
    void range(uint64_t offset, uint64_t length) const {
        require(offset <= bytes.size() && length <= bytes.size() - offset, "ELF file range exceeds file");
    }
};
struct Mapping {
    uint64_t first = UINT64_MAX, end = 0;
    void add(uint64_t address, uint64_t size) {
        if (!size) return;
        require(size <= UINT64_MAX - address, "ELF virtual range overflow");
        first = std::min<uint64_t>(first, address & ~4095ULL);
        end = std::max<uint64_t>(end, (address + size + 4095) & ~4095ULL);
    }
    bool contains(uint64_t address, uint64_t size) const {
        return first != UINT64_MAX && address >= first && address <= end && size <= end - address;
    }
};
void check(const Image &image, bool vision) {
    auto header = image.read<Elf64_Ehdr>(0);
    require(!std::memcmp(header.e_ident, ELFMAG, SELFMAG) && header.e_ident[EI_CLASS] == ELFCLASS64 &&
        header.e_ident[EI_DATA] == ELFDATA2LSB && header.e_ident[EI_VERSION] == EV_CURRENT &&
        header.e_machine == EM_RISCV && header.e_version == EV_CURRENT, "not little-endian ELF64 RISC-V");
    require(header.e_ehsize == sizeof(header) && header.e_phentsize == sizeof(Elf64_Phdr) &&
        header.e_shentsize == sizeof(Elf64_Shdr), "ELF table entry sizes");
    require(header.e_phnum > 0 && header.e_shnum > 0 && header.e_shstrndx < header.e_shnum, "missing ELF tables");
    image.range(header.e_phoff, uint64_t(header.e_phnum) * sizeof(Elf64_Phdr));
    image.range(header.e_shoff, uint64_t(header.e_shnum) * sizeof(Elf64_Shdr));
    if (vision) {
        require(header.e_type == ET_EXEC && header.e_entry == 0x200000000ULL, "RT-Smart static entry/type");
        require(header.e_phnum * sizeof(Elf64_Phdr) <= 4096 - 16, "RT-Smart aux program table too large");
        require((header.e_flags & EF_RISCV_FLOAT_ABI) == EF_RISCV_FLOAT_ABI_DOUBLE, "vision float ABI");
    } else require(header.e_type == ET_EXEC || header.e_type == ET_DYN, "Linux runtime must be EXEC or DYN");
    std::vector<Elf64_Shdr> sections;
    Mapping text, data;
    for (unsigned i = 0; i < header.e_shnum; ++i) {
        auto section = image.read<Elf64_Shdr>(header.e_shoff + uint64_t(i) * sizeof(Elf64_Shdr));
        sections.push_back(section);
        if (section.sh_type != SHT_NOBITS) image.range(section.sh_offset, section.sh_size);
        if (section.sh_type == SHT_SYMTAB || section.sh_type == SHT_DYNSYM)
            require(section.sh_entsize == sizeof(Elf64_Sym) && section.sh_size % section.sh_entsize == 0, "invalid symbol entry size");
        if (section.sh_type == SHT_RELA)
            require(section.sh_entsize == sizeof(Elf64_Rela) && section.sh_size % section.sh_entsize == 0, "invalid RELA entry size");
        if (section.sh_type == SHT_REL)
            require(section.sh_entsize == sizeof(Elf64_Rel) && section.sh_size % section.sh_entsize == 0, "invalid REL entry size");
        if (!(section.sh_flags & SHF_ALLOC) || !vision) continue;
        require(section.sh_addr >= 0x200000000ULL && section.sh_addr <= 0x270000000ULL &&
            section.sh_size <= 0x270000000ULL - section.sh_addr, "RT-Smart user address range");
        // Match this SDK load_elf(): only read-only PROGBITS belongs to text.
        if (section.sh_type == SHT_PROGBITS && !(section.sh_flags & SHF_WRITE)) text.add(section.sh_addr, section.sh_size);
        else data.add(section.sh_addr, section.sh_size);
        if (section.sh_type == SHT_RELA)
            for (uint64_t n = 0; n < section.sh_size; n += sizeof(Elf64_Rela)) {
                auto relocation = image.read<Elf64_Rela>(section.sh_offset + n);
                require(relocation.r_info == 0 && relocation.r_offset == 0 && relocation.r_addend == 0,
                    "static vision contains relocation requiring a dynamic loader");
            }
    }
    if (vision) {
        require(text.contains(header.e_entry, 1), "entry outside loader text mapping");
        require(text.end <= data.first || data.end <= text.first, "RT-Smart text/data page overlap");
    }
    bool entry_loaded = false;
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        auto segment = image.read<Elf64_Phdr>(header.e_phoff + uint64_t(i) * sizeof(Elf64_Phdr));
        image.range(segment.p_offset, segment.p_filesz);
        if (vision) require(segment.p_type != PT_INTERP && segment.p_type != PT_DYNAMIC, "vision needs Linux dynamic loader");
        if (segment.p_type != PT_LOAD) continue;
        require(segment.p_filesz <= segment.p_memsz, "PT_LOAD file size exceeds memory");
        require(segment.p_align && !(segment.p_align & (segment.p_align - 1)) &&
            segment.p_vaddr % segment.p_align == segment.p_offset % segment.p_align, "PT_LOAD alignment");
        if (vision) require(text.contains(segment.p_vaddr, segment.p_memsz) || data.contains(segment.p_vaddr, segment.p_memsz),
            "PT_LOAD outside RT-Smart section-derived maps");
        if (vision) require(segment.p_vaddr % 4096 == 0 && !((segment.p_flags & PF_W) && (segment.p_flags & PF_X)),
            "RT-Smart copy expects page-start segment; no RWX load");
        if ((segment.p_flags & PF_X) && header.e_entry >= segment.p_vaddr &&
            header.e_entry - segment.p_vaddr < segment.p_memsz) entry_loaded = true;
    }
    if (vision) require(entry_loaded, "entry not loaded executable");
    for (const auto &section : sections) {
        if (section.sh_type != SHT_SYMTAB && section.sh_type != SHT_DYNSYM) continue;
        require(section.sh_link < sections.size() && sections[section.sh_link].sh_type == SHT_STRTAB, "invalid symbol string table");
        if (!vision) continue;
        for (uint64_t n = sizeof(Elf64_Sym); n < section.sh_size; n += sizeof(Elf64_Sym)) {
            auto symbol = image.read<Elf64_Sym>(section.sh_offset + n);
            require(symbol.st_shndx != SHN_UNDEF || ELF64_ST_BIND(symbol.st_info) != STB_GLOBAL,
                "unresolved strong symbol in static vision");
        }
    }
}
}
int main(int argc, char **argv) {
    try {
        require(argc == 3, "Usage: elf_check ELF vision|linux");
        std::ifstream stream(argv[1], std::ios::binary);
        require(bool(stream), "cannot open ELF");
        Image image{std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {})};
        bool vision = std::string(argv[2]) == "vision";
        check(image, vision);
        if (vision) {
            auto header = image.read<Elf64_Ehdr>(0);
            bool tested = false;
            for (unsigned i = 0; i < header.e_shnum; ++i) {
                uint64_t offset = header.e_shoff + uint64_t(i) * sizeof(Elf64_Shdr);
                auto section = image.read<Elf64_Shdr>(offset);
                if (section.sh_type != SHT_RELA) continue;
                section.sh_entsize = 8;
                Image broken = image;
                std::memcpy(broken.bytes.data() + offset, &section, sizeof(section));
                try { check(broken, true); } catch (const std::runtime_error &) { tested = true; }
                break;
            }
            require(tested, "regression must reject old malformed RELA");
        }
        std::printf("ELF: %s structural checks%s passed; not hardware loader execution\n", argv[1],
            vision ? " + SDK static loader ranges/entry/relocations" : "");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "ELF rejected: %s\n", error.what()); return 1;
    }
}
