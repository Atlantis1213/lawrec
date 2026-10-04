#include "mapi_wait.h"
#include "k_ipcmsg.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
std::atomic<bool> connected{true};
std::atomic<int> pending{0};
void ignore(int, k_ipcmsg_message_t *) {}
}
extern "C" {
k_s32 __wrap_kd_ipcmsg_connect(k_s32 *, const k_char *, k_ipcmsg_handle_fn_ptr);
k_s32 __real_kd_ipcmsg_connect(k_s32 *, const k_char *, k_ipcmsg_handle_fn_ptr) { return 42; }
k_s32 kd_ipcmsg_try_connect(k_s32 *id, const k_char *, k_ipcmsg_handle_fn_ptr) {
    *id = 7; ++pending; return 0;
}
k_bool kd_ipcmsg_is_connect(k_s32 id) { assert(id == 7); return connected ? K_TRUE : K_FALSE; }
}
int main() {
    int id = -1;
    assert(__wrap_kd_ipcmsg_connect(&id, "other", ignore) == 42);
    assert(__wrap_kd_ipcmsg_connect(&id, "kd_mapi_msg", ignore) == 0 && id == 7);
    connected = false;
    int ret = 0;
    std::thread waiting([&] { ret = __wrap_kd_ipcmsg_connect(&id, "kd_mapi_msg", ignore); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (pending < 2 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(pending == 2);
    demo::stop_mapi_connects();
    waiting.join();
    assert(ret == K_IPCMSG_ENOOP);
    assert(__wrap_kd_ipcmsg_connect(&id, "kd_mapi_msg", ignore) == K_IPCMSG_ENOOP && pending == 2);
    std::puts("MAPI wait MOCK ONLY: connected path and stop before Linux peer/reconnect passed");
}
