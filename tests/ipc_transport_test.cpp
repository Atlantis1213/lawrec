// SDK transport contract fixture, not a hardware IPC simulator.
#include "k_comm_ipcmsg.h"
#include "ipc_driver.h"
#include "ipcm_userdev.h"
#include <cassert>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

extern "C" k_s32 __wrap_IPCMSG_TransConnect(IPCMSG_TRANS_CONNECT_ATTR_S *, k_bool);
namespace {
int open_error, attr_error, connect_result, state = HANDLE_CONNECTING;
int opened, closed, blocking;
struct Transport {
    char name[16];
    k_s32 id;
    ipcm_handle_attr config;
} transport{};
}
extern "C" {
int open(const char *path, int flags, ...) {
    assert(std::strcmp(path, "/dev/ipcm_user") == 0 && flags == O_RDWR);
    if (open_error) { errno = open_error; return -1; }
    ++opened;
    return 7;
}
int close(int fd) noexcept { assert(fd == 7); ++closed; return 0; }
int ioctl(int fd, unsigned long request, ...) noexcept {
    assert(fd == 7);
    va_list args;
    va_start(args, request);
    auto *attr = va_arg(args, ipcm_handle_attr *);
    va_end(args);
    if (request == K_IPCM_IOC_CHECK) return state;
    if (request == K_IPCM_IOC_ATTR_INIT) {
        *attr = {};
        return attr_error;
    }
    assert(request == K_IPCM_IOC_TRY_CONNECT && attr->target == 0 && attr->port == 102);
    return connect_result;
}
k_s32 __real_IPCMSG_TransConnect(IPCMSG_TRANS_CONNECT_ATTR_S *, k_bool block) {
    assert(block); ++blocking; return -123;
}
}
int main() {
    static_assert(sizeof(demo::IpcDriverAttrs) == sizeof(ipcm_handle_attr));
    static_assert(demo::ipc_attr_init == K_IPCM_IOC_ATTR_INIT);
    static_assert(demo::ipc_try_connect == K_IPCM_IOC_TRY_CONNECT);
    static_assert(demo::ipc_check == K_IPCM_IOC_CHECK);
    static_assert(demo::ipc_connecting == HANDLE_CONNECTING && demo::ipc_connected == HANDLE_CONNECTED);
    transport.config.target = 0; transport.config.port = 102; transport.id = -1;
    auto *attrs = reinterpret_cast<IPCMSG_TRANS_CONNECT_ATTR_S *>(&transport);
    assert(__wrap_IPCMSG_TransConnect(nullptr, K_FALSE) == K_IPCMSG_EINVAL);
    assert(__wrap_IPCMSG_TransConnect(attrs, K_TRUE) == -123 && blocking == 1);
    connect_result = 1;
    assert(__wrap_IPCMSG_TransConnect(attrs, K_FALSE) == 0 && transport.id == 7 && closed == 0);
    close(7);
    state = HANDLE_CONNECTED;
    assert(__wrap_IPCMSG_TransConnect(attrs, K_FALSE) == 0);
    close(7);
    connect_result = 0;
    assert(__wrap_IPCMSG_TransConnect(attrs, K_FALSE) == 0);
    close(7);
    transport.id = -1; connect_result = 1; state = HANDLE_DISCONNECTED;
    assert(__wrap_IPCMSG_TransConnect(attrs, K_FALSE) != 0 && transport.id == -1);
    connect_result = -1; errno = EFAULT;
    assert(__wrap_IPCMSG_TransConnect(attrs, K_FALSE) != 0);
    attr_error = -1;
    assert(__wrap_IPCMSG_TransConnect(attrs, K_FALSE) != 0);
    open_error = ENOENT;
    assert(__wrap_IPCMSG_TransConnect(attrs, K_FALSE) != 0 && closed == opened);
    std::puts("IPC transport MOCK ONLY: pending peer retained, true failures closed, blocking path unchanged");
}
