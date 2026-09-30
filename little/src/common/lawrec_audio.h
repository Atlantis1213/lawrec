#pragma once
#include "lawrec_frame_queue.h"

// Requires a media lease. Owners match video: 1=RTSP, 2=recording.
// Output is G.711A, mono, 8 kHz; timestamps retain the SDK microsecond clock.
int lawrec_audio_subscribe(int owner, LawrecFrameQueue *queue);
int lawrec_audio_unsubscribe(int owner);
