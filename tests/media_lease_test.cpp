#include "lawrec_media.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
extern "C" {
#include "mapi_sys_api.h"
static int init_calls, init_error;
k_s32 kd_mapi_sys_init(void) { ++init_calls; return init_error; }
void kd_mapi_media_init_workaround(k_bool) {}
}
int main() {
    assert(lawrec_media_owner_mask() == 0);
    assert(lawrec_media_acquire(0) == -EINVAL);
    init_error = -EIO;
    assert(lawrec_media_acquire(1) == -EIO);
    init_error = 0;
    assert(lawrec_media_acquire(1) == 0);
    assert(lawrec_media_acquire(2) == 0);
    assert(lawrec_media_owner_mask() == ((1u << 1) | (1u << 2)));
    assert(lawrec_media_acquire(1) == -EBUSY);
    assert(lawrec_media_acquire(3) == -EBUSY);
    lawrec_media_release(1);
    assert(lawrec_media_owner_mask() == (1u << 2));
    assert(lawrec_media_acquire(3) == -EBUSY);
    lawrec_media_release(2);
    assert(lawrec_media_acquire(3) == 0);
    assert(lawrec_media_acquire(1) == -EBUSY);
    assert(lawrec_media_acquire(2) == -EBUSY);
    lawrec_media_release(3);
    assert(lawrec_media_acquire(2) == 0);
    assert(lawrec_media_acquire(1) == 0);
    lawrec_media_release(2);
    lawrec_media_release(1);
    assert(lawrec_media_owner_mask() == 0);
    assert(init_calls == 2);
    puts("media lease tests passed");
}
