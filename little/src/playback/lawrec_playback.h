#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { LAWREC_PLAY_IDLE, LAWREC_PLAY_STARTING, LAWREC_PLAY_PLAYING,
       LAWREC_PLAY_PAUSED, LAWREC_PLAY_STOPPING, LAWREC_PLAY_FINISHED, LAWREC_PLAY_FAILED };
typedef struct {
    int state, error, active, audio_present, resource_retained, cleanup_error;
    uint64_t position_ms, duration_ms, frames;
    char filename[128];
} lawrec_playback_status;
/* Called by control after capture consumers have stopped. */
int lawrec_playback_start(const char *filename);
void lawrec_playback_pause(int pause);
void lawrec_playback_stop(void);
/* stop_wait reports worker timeout or retained-resource cleanup error, not a
 * historical file/decoder error after otherwise successful teardown. */
int lawrec_playback_stop_wait(unsigned timeout_ms);
void lawrec_playback_get_status(lawrec_playback_status *status);
int lawrec_playback_active(void);
/* Implemented by the existing IPC connection, called only from worker. */
int lawrec_playback_display_request(int enabled);
#ifdef __cplusplus
}
#endif
