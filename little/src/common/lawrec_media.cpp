#include "lawrec_media.h"
#include <mutex>
#include <cerrno>
#include <cstdio>
extern "C" {
#include "mapi_sys_api.h"
void kd_mapi_media_init_workaround(k_bool);
}
static std::mutex lock;
static unsigned owners;
static bool client_initialized;
int lawrec_media_acquire(int owner)
{
    std::lock_guard<std::mutex> guard(lock);
    if (owner < 1 || owner > 3) return -EINVAL;
    unsigned bit = 1u << owner;
    // RTSP and recording share VENC; playback exclusively owns decoder/VO.
    if ((owners & bit) || (owners && owner == 3) || (owners & (1u << 3))) return -EBUSY;
    if (!client_initialized) {
        int ret = kd_mapi_sys_init();
        if (ret) return ret;
        client_initialized = true;
    }
    // Preview ACK establishes big-core VB readiness. Only set the SDK client
    // flag; the application never acquires ownership of the remote VB pools.
    kd_mapi_media_init_workaround(K_TRUE);
    owners |= bit;
    fprintf(stderr, "[media] acquired owner=%d mask=%u\n", owner, owners);
    return 0;
}
void lawrec_media_release(int owner)
{
    std::lock_guard<std::mutex> guard(lock);
    if (owner < 1 || owner > 3) return;
    // Retain the SDK client connection for process lifetime.
    owners &= ~(1u << owner);
    fprintf(stderr, "[media] released owner=%d mask=%u\n", owner, owners);
}
