#include "mapi_wait.h"
#include "k_ipcmsg.h"
#include <atomic>
#include <cstring>
#include <unistd.h>

namespace {
std::atomic<bool> stopping{false};
}
namespace demo {
void stop_mapi_connects() { stopping = true; }
}
extern "C" k_s32 __real_kd_ipcmsg_connect(k_s32 *, const k_char *, k_ipcmsg_handle_fn_ptr);

// SDK MAPI blocks inside connect(), including after the Linux peer exits.
// Publish the pending fd early so deinit can disconnect it and join the thread.
extern "C" k_s32 __wrap_kd_ipcmsg_connect(k_s32 *id, const k_char *name, k_ipcmsg_handle_fn_ptr fn) {
    if (!name || std::strcmp(name, "kd_mapi_msg")) return __real_kd_ipcmsg_connect(id, name, fn);
    if (stopping) return K_IPCMSG_ENOOP;
    int ret = kd_ipcmsg_try_connect(id, name, fn);
    if (ret) return ret;
    while (!stopping) {
        if (kd_ipcmsg_is_connect(*id)) return 0;
        usleep(10000);
    }
    return K_IPCMSG_ENOOP;
}
