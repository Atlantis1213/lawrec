#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int lawrec_media_acquire(int owner);
void lawrec_media_release(int owner);
/* Includes quarantined owners retained after cleanup failure; no SDK calls. */
unsigned lawrec_media_owner_mask(void);
#ifdef __cplusplus
}
#endif
