#include "lawrec_media.h"
#include <mutex>
#include <cerrno>
#include <cstdio>
extern "C" {
#include "mapi_sys_api.h"
void kd_mapi_media_init_workaround(k_bool);
}
static std::mutex lock;
static int current_owner;
int lawrec_media_acquire(int owner)
{
    std::lock_guard<std::mutex> guard(lock);
    if (current_owner) return -EBUSY;
    int ret = kd_mapi_sys_init();
    if (ret) return ret;
    // Preview ACK establishes big-core VB readiness. Only set the SDK client
    // flag; the application never acquires ownership of the remote VB pools.
    kd_mapi_media_init_workaround(K_TRUE);
    current_owner = owner;
    fprintf(stderr, "[media] acquired owner=%d\n", owner);
    return 0;
}
void lawrec_media_release(int owner)
{
    std::lock_guard<std::mutex> guard(lock);
    if (current_owner != owner) return;
    // Retain the SDK client connection for process lifetime.
    current_owner = 0;
    fprintf(stderr, "[media] released owner=%d\n", owner);
}
