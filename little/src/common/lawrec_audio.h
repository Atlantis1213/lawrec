#pragma once
#include "lawrec_frame_queue.h"

// Requires a media lease. Owners match video: 1=RTSP, 2=recording.
// Output is G.711A, mono, 8 kHz; timestamps retain the SDK microsecond clock.
int lawrec_audio_subscribe(int owner, LawrecFrameQueue *queue);
// Recording may drain already delivered packets after detaching. RTSP discards
// its tail by default. This never stops another owner's shared capture.
int lawrec_audio_unsubscribe(int owner, bool keep_tail = false);
