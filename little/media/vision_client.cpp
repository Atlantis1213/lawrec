#include "vision_client.h"
#include "config.h"
#include "k_ipcmsg.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace demo {
namespace {
void unsolicited(int, k_ipcmsg_message_t *) { std::fprintf(stderr, "[media-ipc] unsolicited request ignored\n"); }
}
int VisionClient::start() {
    k_ipcmsg_connect_t attrs{};
    attrs.u32RemoteId = 1; attrs.u32Port = ipc_port; attrs.u32Priority = 0;
    int ret = kd_ipcmsg_add_service(ipc_service, &attrs);
    if (ret) return ret < 0 ? ret : -EIO;
    registered_ = true;
    ret = kd_ipcmsg_try_connect(&handle_, ipc_service, unsolicited);
    if (ret) { stop(); return ret < 0 ? ret : -EIO; }
    running_ = true;
    try {
        receiver_ = std::thread([this] {
            // Initial connection only; no replay/recovery after the run loop ends.
            for (int i = 0; i < 50 && running_ && !kd_ipcmsg_is_connect(handle_); ++i) usleep(100000);
            if (running_ && kd_ipcmsg_is_connect(handle_)) kd_ipcmsg_run(handle_);
            running_ = false;
        });
    } catch (...) { stop(); return -EAGAIN; }
    return 0;
}
int VisionClient::exchange(const Request &request, Status &status) {
    if (!running_ || handle_ < 0 || !kd_ipcmsg_is_connect(handle_)) return -ENOTCONN;
    auto *message = kd_ipcmsg_create_message(vision_module, request.command, &request, sizeof(request));
    if (!message) return -ENOMEM;
    k_ipcmsg_message_t *response = nullptr;
    int ret = kd_ipcmsg_send_sync(handle_, message, &response, 700);
    kd_ipcmsg_destroy_message(message);
    if (ret) ret = ret == K_IPCMSG_ETIMEOUT ? -ETIMEDOUT : (ret < 0 ? ret : -EIO);
    else if (!response || !response->pBody || response->u32BodyLen != sizeof(Status) ||
        response->u32Module != vision_module || response->u32CMD != request.command) ret = -EPROTO;
    else {
        Status answer;
        std::memcpy(&answer, response->pBody, sizeof(answer));
        if (!valid_status(answer, request.id) || answer.result != response->s32RetVal) ret = -EPROTO;
        else status = answer;
    }
    if (response) kd_ipcmsg_destroy_message(response);
    return ret;
}
int VisionClient::stop() {
    running_ = false;
    int error = 0;
    if (handle_ >= 0) {
        int ret = kd_ipcmsg_disconnect(handle_);
        if (ret) error = ret < 0 ? ret : -EIO;
    }
    if (receiver_.joinable()) receiver_.join();
    handle_ = -1;
    if (registered_) {
        int ret = kd_ipcmsg_del_service(ipc_service);
        if (ret && !error) error = ret < 0 ? ret : -EIO;
        registered_ = false;
    }
    return error;
}
}
