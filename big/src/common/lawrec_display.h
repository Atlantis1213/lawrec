#pragma once
#include <atomic>
#include <stdint.h>

/* Hardware methods run on the one IPC receiver. Busy also covers failed rollback. */
class LawrecDisplayController {
public:
    LawrecDisplayController() : busy_(false) {}
    int Enable(uint32_t width, uint32_t height);
    int Recover();
    bool Busy() const { return busy_.load(); }
private:
    std::atomic<bool> busy_;
};
