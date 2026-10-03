// Tests wire validation/main-thread acknowledgement, not a hardware IPC transport.
#include "control.h"
#include "config.h"
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
k_ipcmsg_handle_fn_ptr callback;
std::atomic<bool> connected{false};
int allocations = 0, sends = 0;
demo::Status sent;
void deliver(demo::Request &request, size_t length = sizeof(demo::Request),
             uint32_t module = demo::vision_module, bool null_body = false) {
    k_ipcmsg_message_t message{};
    message.u32Module = module; message.u32CMD = request.command;
    message.pBody = null_body ? nullptr : &request; message.u32BodyLen = length;
    callback(7, &message);
}
}
extern "C" {
k_s32 kd_ipcmsg_add_service(const k_char *name, const k_ipcmsg_connect_t *attrs) {
    assert(std::strcmp(name, demo::ipc_service) == 0 && attrs->u32Port == demo::ipc_port);
    return 0;
}
k_s32 kd_ipcmsg_del_service(const k_char *) { return 0; }
k_s32 kd_ipcmsg_try_connect(k_s32 *handle, const k_char *, k_ipcmsg_handle_fn_ptr fn) {
    *handle = 7; callback = fn; connected = true; return 0;
}
k_bool kd_ipcmsg_is_connect(k_s32) { return connected ? K_TRUE : K_FALSE; }
void kd_ipcmsg_run(k_s32) {
    while (connected) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
k_s32 kd_ipcmsg_disconnect(k_s32) { connected = false; return 0; }
k_ipcmsg_message_t *kd_ipcmsg_create_resp_message(k_ipcmsg_message_t *request,
        k_s32 result, void *body, k_u32 size) {
    auto *response = new k_ipcmsg_message_t(*request);
    response->pBody = new unsigned char[size]; response->u32BodyLen = size;
    response->bIsResp = K_TRUE; response->s32RetVal = result;
    std::memcpy(response->pBody, body, size); ++allocations;
    return response;
}
void kd_ipcmsg_destroy_message(k_ipcmsg_message_t *message) {
    delete[] static_cast<unsigned char *>(message->pBody); delete message; --allocations;
}
k_s32 kd_ipcmsg_send_async(k_s32, k_ipcmsg_message_t *message, k_ipcmsg_resphandle_fn_ptr) {
    assert(message->u32BodyLen == sizeof(sent));
    std::memcpy(&sent, message->pBody, sizeof(sent));
    assert(sent.result == message->s32RetVal); ++sends; return 0;
}
}
int main() {
    demo::VisionControl control;
    assert(control.start() == 0);
    demo::Status status; status.last_error = -EIO;
    control.publish(status);
    demo::Request request; request.id = 11;
    deliver(request);
    assert(sent.id == 11 && sent.result == 0 && sent.last_error == -EIO);
    deliver(request, sizeof(request) - 1); assert(sent.result == -EPROTO);
    deliver(request, sizeof(request), demo::vision_module, true); assert(sent.result == -EPROTO);
    deliver(request, sizeof(request), 42); assert(sent.result == -EPROTO);
    request.command = uint32_t(demo::Command::SetRtsp);
    deliver(request); assert(sent.result == -EOPNOTSUPP);
    request.command = uint32_t(demo::Command::SetPreview); request.value = 1;
    int previous = sends;
    deliver(request); assert(sends == previous && allocations == 1);
    deliver(request); assert(sent.result == -EBUSY && allocations == 1);
    demo::Request taken;
    assert(control.take(taken) && taken.id == request.id && !control.take(taken));
    status.flags = demo::Preview;
    control.finish(0, status);
    assert(sent.flags == demo::Preview && !sent.busy && sent.last_error == -EIO && allocations == 0);
    deliver(request);
    assert(control.take(taken));
    control.finish(-EIO, status); assert(sent.result == -EIO && sent.last_error == -EIO);
    deliver(request); assert(allocations == 1);
    assert(control.stop() == 0 && allocations == 0);
    std::puts("control MOCK ONLY: validation, one pending command, delayed ACK and error retention passed");
}
