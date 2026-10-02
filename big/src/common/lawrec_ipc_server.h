#pragma once
#include <atomic>
#include <stdint.h>
#include <pthread.h>
#include "k_ipcmsg.h"

/* One connection owner. Sends pin the handle; reconnect never initializes media. */
class LawrecIpcServer {
public:
    LawrecIpcServer(const char *service, k_ipcmsg_handle_fn_ptr callback, bool (*running)());
    ~LawrecIpcServer();
    int Init();
    void Run();
    void Stop();
    int Send(uint32_t module, uint32_t command, const void *body, uint32_t bytes);
    int Reply(int handle, k_ipcmsg_message_t *message);
private:
    void Detach(int expected);
    bool Running() const;
    const char *service_;
    k_ipcmsg_handle_fn_ptr callback_;
    bool (*running_)();
    std::atomic<bool> stopping_;
    pthread_mutex_t lock_;
    int handle_;
};
