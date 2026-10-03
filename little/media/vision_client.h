#pragma once
#include "protocol.h"
#include <atomic>
#include <thread>

namespace demo {
class VisionClient {
public:
    int start();
    int exchange(const Request &request, Status &status);
    int stop();
private:
    int handle_ = -1;
    bool registered_ = false;
    std::atomic<bool> running_{false};
    std::thread receiver_;
};
}
