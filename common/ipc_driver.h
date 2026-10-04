#pragma once
#include <sys/ioctl.h>

namespace demo {
// IPCM user-driver wire ABI. RT-Smart's musl sysroot lacks linux/ioctl.h;
// the native fixture checks these definitions against the SDK driver header.
struct IpcDriverAttrs {
    int target, port, priority;
    int remote_ids[8];
};
constexpr auto ipc_attr_init = _IOW('M', 8, IpcDriverAttrs);
constexpr auto ipc_try_connect = _IOW('M', 2, IpcDriverAttrs);
constexpr auto ipc_check = _IOW('M', 3, unsigned long);
constexpr int ipc_connecting = 1, ipc_connected = 2;
}
