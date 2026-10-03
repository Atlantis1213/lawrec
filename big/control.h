#pragma once
#include "protocol.h"
#include "k_ipcmsg.h"
#include <atomic>
#include <mutex>
#include <thread>

namespace demo {
// SDK receive thread only snapshots/enqueues. Main owns camera/AI mutations.
class VisionControl {
public:
    int start();
    int stop();
    void publish(const Status &status);
    bool take(Request &request);
    void finish(int result, const Status &status);
private:
    static VisionControl *instance_;
    static void callback(int handle, k_ipcmsg_message_t *message);
    void receive(int handle, k_ipcmsg_message_t *message);
    std::mutex lock_;
    Status status_;
    Request request_;
    k_ipcmsg_message_t *reply_ = nullptr;
    bool pending_ = false, registered_ = false;
    int handle_ = -1;
    std::atomic<bool> running_{false};
    std::thread receiver_;
};
}
