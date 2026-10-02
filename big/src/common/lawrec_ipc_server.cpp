#include "lawrec_ipc_server.h"
#include <cerrno>
#include <cstdio>
#include <unistd.h>

LawrecIpcServer::LawrecIpcServer(const char *service, k_ipcmsg_handle_fn_ptr callback,
                               bool (*running)())
    : service_(service), callback_(callback), running_(running), stopping_(false), handle_(-1)
{
    pthread_mutex_init(&lock_, nullptr);
}
LawrecIpcServer::~LawrecIpcServer() { pthread_mutex_destroy(&lock_); }
bool LawrecIpcServer::Running() const { return !stopping_.load() && running_(); }

int LawrecIpcServer::Init()
{
    k_ipcmsg_connect_t attr{};
    attr.u32RemoteId = 0; attr.u32Port = 101; attr.u32Priority = 0;
    int ret = kd_ipcmsg_add_service(service_, &attr);
    if (ret) printf("[lawrec-ipc] register result=%d\n", ret);
    return ret;
}

void LawrecIpcServer::Detach(int expected)
{
    pthread_mutex_lock(&lock_);
    int old = handle_;
    if (expected >= 0 && old != expected) old = -1;
    if (old >= 0) handle_ = -1;
    pthread_mutex_unlock(&lock_);
    // No future send can acquire old; do not hold the lock during SDK teardown.
    if (old >= 0) {
        int ret = kd_ipcmsg_disconnect(old);
        printf("[lawrec-ipc] disconnect handle=%d result=%d\n", old, ret);
    }
}

void LawrecIpcServer::Stop() { stopping_.store(true); Detach(-1); }

void LawrecIpcServer::Run()
{
    while (Running()) {
        int candidate = -1;
        int ret = kd_ipcmsg_try_connect(&candidate, service_, callback_);
        if (ret) {
            printf("[lawrec-ipc] connect result=%d; retry\n", ret);
            for (int i = 0; i < 10 && Running(); ++i) usleep(100000);
            continue;
        }
        pthread_mutex_lock(&lock_);
        bool publish = Running();
        if (publish) handle_ = candidate;
        pthread_mutex_unlock(&lock_);
        if (!publish) {
            kd_ipcmsg_disconnect(candidate);
            break;
        }
        // try_connect can return a valid handle before the peer is present.
        while (Running() && !kd_ipcmsg_is_connect(candidate)) usleep(100000);
        if (Running()) {
            printf("[lawrec-ipc] connected handle=%d\n", candidate);
            kd_ipcmsg_run(candidate);
            printf("[lawrec-ipc] receive loop ended handle=%d\n", candidate);
        }
        Detach(candidate);
        for (int i = 0; i < 10 && Running(); ++i) usleep(100000);
    }
}

int LawrecIpcServer::Send(uint32_t module, uint32_t command, const void *body, uint32_t bytes)
{
    pthread_mutex_lock(&lock_);
    int ret = -ENOTCONN;
    if (handle_ >= 0 && kd_ipcmsg_is_connect(handle_)) {
        auto *message = kd_ipcmsg_create_message(module, command, body, bytes);
        if (!message) ret = -ENOMEM;
        else {
            ret = kd_ipcmsg_send_only(handle_, message);
            kd_ipcmsg_destroy_message(message);
        }
    }
    pthread_mutex_unlock(&lock_);
    if (ret) printf("[lawrec-ipc] send cmd=%u result=%d\n", command, ret);
    return ret;
}

int LawrecIpcServer::Reply(int handle, k_ipcmsg_message_t *message)
{
    pthread_mutex_lock(&lock_);
    int ret = handle == handle_ && kd_ipcmsg_is_connect(handle) ?
        kd_ipcmsg_send_async(handle, message, nullptr) : -ENOTCONN;
    pthread_mutex_unlock(&lock_);
    return ret;
}
