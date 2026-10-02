#include "../big/src/common/lawrec_ipc_server.h"
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>

static std::mutex lock;
static std::condition_variable changed;
static std::map<int, bool> handles;
static int next_handle, runs, allow_return, disconnects, register_error, sends;
static int send_error;
static bool peer = true, allocation_failure;
static int run_handle;
static bool running() { return true; }
static void callback(k_s32, k_ipcmsg_message_t *) {}

extern "C" {
k_s32 kd_ipcmsg_add_service(const k_char *, const k_ipcmsg_connect_t *attr)
{ assert(attr->u32RemoteId == 0 && attr->u32Port == 101); return register_error; }
k_s32 kd_ipcmsg_try_connect(k_s32 *id, const k_char *, k_ipcmsg_handle_fn_ptr)
{
    std::lock_guard<std::mutex> guard(lock);
    *id = ++next_handle; handles[*id] = true; changed.notify_all(); return 0;
}
k_bool kd_ipcmsg_is_connect(k_s32 id)
{
    std::lock_guard<std::mutex> guard(lock);
    return peer && handles.count(id) && handles[id] ? K_TRUE : K_FALSE;
}
void kd_ipcmsg_run(k_s32 id)
{
    std::unique_lock<std::mutex> guard(lock);
    const int run = ++runs; run_handle = id; changed.notify_all();
    changed.wait(guard, [=] { return !handles.count(id) || allow_return >= run; });
}
k_s32 kd_ipcmsg_disconnect(k_s32 id)
{
    std::lock_guard<std::mutex> guard(lock);
    assert(handles.erase(id) == 1); ++disconnects; changed.notify_all(); return 0;
}
k_ipcmsg_message_t *kd_ipcmsg_create_message(k_u32 module, k_u32 command, const void *, k_u32)
{
    if (allocation_failure) return nullptr;
    auto *message = static_cast<k_ipcmsg_message_t *>(calloc(1, sizeof(k_ipcmsg_message_t)));
    assert(message); message->u32Module = module; message->u32CMD = command; return message;
}
void kd_ipcmsg_destroy_message(k_ipcmsg_message_t *message) { free(message); }
k_s32 kd_ipcmsg_send_only(k_s32 id, k_ipcmsg_message_t *message)
{
    std::lock_guard<std::mutex> guard(lock);
    assert(handles.count(id) && message->u32Module == 3); ++sends; return send_error;
}
k_s32 kd_ipcmsg_send_async(k_s32 id, k_ipcmsg_message_t *, k_ipcmsg_resphandle_fn_ptr)
{ std::lock_guard<std::mutex> guard(lock); assert(handles.count(id)); return 0; }
}

template<class Predicate> static void wait_for(Predicate predicate)
{
    std::unique_lock<std::mutex> guard(lock);
    assert(changed.wait_for(guard, std::chrono::seconds(3), predicate));
}

int main()
{
    LawrecIpcServer server("door_lock", callback, running);
    register_error = -EIO; assert(server.Init() == -EIO);
    register_error = 0; assert(server.Init() == 0);
    assert(server.Send(3, 8, nullptr, 0) == -ENOTCONN);
    auto worker = std::async(std::launch::async, [&] { server.Run(); });
    wait_for([] { return runs == 1; });
    assert(server.Send(3, 8, nullptr, 0) == 0 && sends == 1);
    allocation_failure = true;
    assert(server.Send(3, 8, nullptr, 0) == -ENOMEM && sends == 1);
    allocation_failure = false; send_error = -EIO;
    assert(server.Send(3, 8, nullptr, 0) == -EIO); send_error = 0;
    k_ipcmsg_message_t reply{};
    const int old = run_handle;
    assert(server.Reply(old + 99, &reply) == -ENOTCONN);
    assert(server.Reply(old, &reply) == 0);
    { std::lock_guard<std::mutex> guard(lock); allow_return = 1; changed.notify_all(); }
    wait_for([] { return runs == 2; });
    assert(disconnects == 1 && run_handle != old);
    assert(server.Reply(old, &reply) == -ENOTCONN);
    assert(server.Send(3, 8, nullptr, 0) == 0);
    server.Stop();
    assert(worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready); worker.get();
    assert(disconnects == 2 && server.Send(3, 8, nullptr, 0) == -ENOTCONN);
    server.Stop(); assert(disconnects == 2);

    // Valid SDK handle, absent peer: stop must not wait on blocking connect.
    peer = false;
    LawrecIpcServer absent("door_lock", callback, running); assert(absent.Init() == 0);
    auto waiting = std::async(std::launch::async, [&] { absent.Run(); });
    wait_for([] { return next_handle == 3; });
    absent.Stop();
    assert(waiting.wait_for(std::chrono::seconds(2)) == std::future_status::ready); waiting.get();
    assert(handles.empty() && disconnects == 3);
    puts("Big IPC: registration/send errors, reconnect, stale handle replies, stop and absent peer passed (mock SDK).");
}
