#pragma once
#include <cstdint>

namespace demo {
// The frozen SDK exposes no MPI frame-end IRQ setter. Only vision owns this bit.
class VoSync {
public:
    int enable();
    int stop();
    bool active() const { return regs_ != nullptr; }
private:
    volatile uint32_t *regs_ = nullptr;
    uint32_t saved_ = 0;
};
}
