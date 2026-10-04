#include "k_comm_ipcmsg.h"
#include "ipc_driver.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C" k_s32 __real_IPCMSG_TransConnect(IPCMSG_TRANS_CONNECT_ATTR_S *, k_bool);

// The SDK library rejects TRY_CONNECT's valid "peer pending" result (1).
// Keep that fd only after the driver confirms CONNECTING/CONNECTED; no retries.
extern "C" k_s32 __wrap_IPCMSG_TransConnect(IPCMSG_TRANS_CONNECT_ATTR_S *attrs, k_bool block) {
    if (block) return __real_IPCMSG_TransConnect(attrs, block);
    if (!attrs) return K_IPCMSG_EINVAL;
    static_assert(sizeof(demo::IpcDriverAttrs) == 44, "SDK IPCM transport ABI changed");
    demo::IpcDriverAttrs config;
    std::memcpy(&config, attrs->pData, sizeof(config));
    int fd = open("/dev/ipcm_user", O_RDWR);
    if (fd < 0) {
        std::fprintf(stderr, "[demo-ipc] open failed errno=%d\n", errno);
        return K_IPCMSG_ENOOP;
    }
    auto fail = [&](const char *stage, int result) {
        std::fprintf(stderr, "[demo-ipc] %s remote=%d port=%d result=%d errno=%d\n",
                     stage, config.target, config.port, result, errno);
        close(fd);
        return K_IPCMSG_ENOOP;
    };
    demo::IpcDriverAttrs defaults{};
    int ret = ioctl(fd, demo::ipc_attr_init, &defaults);
    if (ret) return fail("attr init", ret);
    ret = ioctl(fd, demo::ipc_try_connect, &config);
    if (ret == 1) {
        int state = ioctl(fd, demo::ipc_check, nullptr);
        if (state != demo::ipc_connecting && state != demo::ipc_connected)
            return fail("pending state", state);
    } else if (ret) return fail("try connect", ret);
    std::memcpy(attrs->pData, &config, sizeof(config));
    attrs->s32Id = fd;
    std::fprintf(stderr, "[demo-ipc] remote=%d port=%d fd=%d %s\n",
                 config.target, config.port, fd, ret == 1 ? "waiting for peer" : "connected");
    return 0;
}
