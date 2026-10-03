#include "control.h"
#include "config.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace demo {
VisionControl *VisionControl::instance_ = nullptr;
int VisionControl::start() {
    if (instance_) return -EALREADY;
    k_ipcmsg_connect_t attrs{};
    attrs.u32RemoteId = 0; attrs.u32Port = ipc_port; attrs.u32Priority = 0;
    int ret = kd_ipcmsg_add_service(ipc_service, &attrs);
    if (ret) return ret < 0 ? ret : -EIO;
    registered_ = true; instance_ = this;
    ret = kd_ipcmsg_try_connect(&handle_, ipc_service, callback);
    if (ret) { stop(); return ret < 0 ? ret : -EIO; }
    running_ = true;
    try {
        receiver_ = std::thread([this] {
            // Startup waits for the Linux peer. Never reconnect or replay commands.
            while (running_ && !kd_ipcmsg_is_connect(handle_)) usleep(100000);
            if (running_) kd_ipcmsg_run(handle_);
        });
    } catch (...) { stop(); return -EAGAIN; }
    return 0;
}
void VisionControl::callback(int handle, k_ipcmsg_message_t *message) {
    if (instance_) instance_->receive(handle, message);
}
void VisionControl::receive(int handle, k_ipcmsg_message_t *message) {
    if (!message || message->bIsResp) return;
    std::lock_guard<std::mutex> guard(lock_);
    Request request;
    Status answer = status_;
    int result = 0;
    if (!decode_request(message->pBody, message->u32BodyLen, request) ||
        message->u32Module != vision_module || message->u32CMD != request.command) result = -EPROTO;
    else if (request.command > uint32_t(Command::SetAi)) result = -EOPNOTSUPP;
    else if (request.command && reply_) result = -EBUSY;
    answer.id = request.id; answer.result = result;
    auto *response = kd_ipcmsg_create_resp_message(message, result, &answer, sizeof(answer));
    if (!response) { std::printf("[vision-ipc] response allocation failed\n"); return; }
    if (!result && request.command) {
        request_ = request; reply_ = response; pending_ = true;
        status_.busy = 1U << (request.command - 1);
    } else {
        int ret = kd_ipcmsg_send_async(handle, response, nullptr);
        if (ret) std::printf("[vision-ipc] snapshot/error reply failed=%d\n", ret);
        kd_ipcmsg_destroy_message(response);
    }
}
void VisionControl::publish(const Status &status) {
    std::lock_guard<std::mutex> guard(lock_);
    uint32_t busy = status_.busy;
    status_ = status; status_.result = 0;
    status_.busy = reply_ ? busy : 0;
}
bool VisionControl::take(Request &request) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!pending_) return false;
    request = request_; pending_ = false;
    return true;
}
void VisionControl::finish(int result, const Status &status) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!reply_) return;
    status_ = status; status_.busy = 0; status_.result = 0;
    if (result) status_.last_error = result;
    Status answer = status_; answer.id = request_.id; answer.result = result;
    if (reply_->pBody && reply_->u32BodyLen == sizeof(answer)) {
        std::memcpy(reply_->pBody, &answer, sizeof(answer)); reply_->s32RetVal = result;
        int ret = kd_ipcmsg_send_async(handle_, reply_, nullptr);
        std::printf("[vision-ipc] command=%u id=%u result=%d send=%d\n", request_.command, request_.id, result, ret);
    } else std::printf("[vision-ipc] invalid allocated response body\n");
    kd_ipcmsg_destroy_message(reply_); reply_ = nullptr;
}
int VisionControl::stop() {
    running_ = false;
    int error = 0;
    if (handle_ >= 0) {
        int ret = kd_ipcmsg_disconnect(handle_);
        if (ret) error = ret < 0 ? ret : -EIO;
    }
    if (receiver_.joinable()) receiver_.join();
    handle_ = -1;
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (reply_) { kd_ipcmsg_destroy_message(reply_); reply_ = nullptr; }
        pending_ = false;
    }
    if (registered_) {
        int ret = kd_ipcmsg_del_service(ipc_service);
        if (ret && !error) error = ret < 0 ? ret : -EIO;
        registered_ = false;
    }
    instance_ = nullptr;
    return error;
}
}
