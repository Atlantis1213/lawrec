#include "vo_sync.h"
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

namespace {
uint32_t regs[1024];
constexpr unsigned frame = 0x3e0 / 4, vtth = 0x3e4 / 4;
constexpr uint32_t bit = 1U << 20;
int open_error, map_error, unmap_error, unmaps;
}
extern "C" {
int open(const char *path, int flags, ...) {
    assert(!std::strcmp(path, "/dev/mem") && flags == (O_RDWR | O_SYNC));
    errno = open_error; return open_error ? -1 : 7;
}
int close(int fd) noexcept { assert(fd == 7); return 0; }
void *mmap(void *, size_t length, int prot, int flags, int fd, off_t offset) noexcept {
    assert(length == 4096 && prot == (PROT_READ | PROT_WRITE) && flags == MAP_SHARED);
    assert(fd == 7 && offset == 0x90840000ULL);
    errno = map_error; return map_error ? MAP_FAILED : regs;
}
int munmap(void *ptr, size_t length) noexcept {
    assert(ptr == regs && length == 4096); ++unmaps; errno = unmap_error; return unmap_error ? -1 : 0;
}
}
int main() {
    demo::VoSync sync;
    uint64_t y = 0, uv = 0;
    assert(sync.preview_address(y, uv) == -EAGAIN);
    regs[vtth] = bit | 10; regs[frame] = 0x4040;
    open_error = EACCES; assert(sync.enable() == -EACCES && !sync.active());
    open_error = 0; map_error = ENOMEM; assert(sync.enable() == -ENOMEM && !sync.active());
    map_error = 0; assert(sync.enable() == 0 && sync.active());
    assert(regs[frame] == (bit | 0x4040) && regs[vtth] == (bit | 10));
    assert(sync.enable() == 0);
    assert(sync.preview_address(y, uv) == -EAGAIN);
    regs[0x118 / 4] = 2;
    regs[0xa2c / 4] = 0x10000000; regs[0xa30 / 4] = 0x1005dc00;
    assert(sync.preview_address(y, uv) == 0 && y == 0x10000000 && uv == 0x1005dc00);
    assert(regs[frame] == (bit | 0x4040) && regs[vtth] == (bit | 10));
    regs[frame] |= 0x80;
    unmap_error = EIO; assert(sync.stop() == -EIO && sync.active());
    unmap_error = 0; assert(sync.stop() == 0 && !sync.active() && regs[frame] == 0x40c0);
    assert(sync.stop() == 0 && unmaps == 2);
    regs[frame] = bit | 0x4040;
    assert(sync.enable() == 0 && sync.stop() == 0 && regs[frame] == (bit | 0x4040));
    std::puts("VO sync MOCK ONLY: one frame IRQ bit, unchanged VTTH, restore/retry and map failures passed");
}
