#include "vo_sync.h"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <atomic>

namespace demo {
namespace {
constexpr off_t vo_base = 0x90840000ULL;
constexpr unsigned frame_irq = 0x3e0 / 4, vtth_irq = 0x3e4 / 4;
constexpr uint32_t enable_bit = 1U << 20;
void io_fence() {
#ifdef __riscv
    asm volatile("fence iorw, iorw" ::: "memory");
#else
    std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}
}
int VoSync::enable() {
    if (regs_) return 0;
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) return -errno;
    void *mapping = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, vo_base);
    int error = errno;
    close(fd);
    if (mapping == MAP_FAILED || !mapping) return error ? -error : -EIO;
    regs_ = static_cast<volatile uint32_t *>(mapping);
    saved_ = regs_[frame_irq] & enable_bit;
    // Equivalent to SDK kd_vo_set_frame_intr(1), without touching VTTH/timing.
    regs_[frame_irq] |= enable_bit;
    io_fence();
    if (!(regs_[frame_irq] & enable_bit)) return -EIO;
    std::printf("[vision-sync] frame-end IRQ=0x%08x VTTH=0x%08x; original frame bit=%u\n",
                regs_[frame_irq], regs_[vtth_irq], saved_ != 0);
    return 0;
}
int VoSync::stop() {
    if (!regs_) return 0;
    regs_[frame_irq] = (regs_[frame_irq] & ~enable_bit) | saved_;
    io_fence();
    if ((regs_[frame_irq] & enable_bit) != saved_) return -EIO;
    if (munmap(const_cast<uint32_t *>(regs_), 4096)) return -errno;
    regs_ = nullptr;
    return 0;
}
int VoSync::preview_address(uint64_t &y, uint64_t &uv) const {
    if (!regs_ || !(regs_[0x118 / 4] & (1U << 1))) return -EAGAIN;
    // Frozen SDK layer1 address registers; reading does not consume a frame.
    for (unsigned i = 0; i < 3; ++i) {
        y = regs_[0xa2c / 4]; uv = regs_[0xa30 / 4];
        io_fence();
        if (y == regs_[0xa2c / 4] && uv == regs_[0xa30 / 4])
            return y >= 0x10000000 && y < 0x20000000 ? 0 : -EIO;
    }
    return -EAGAIN;
}
}
