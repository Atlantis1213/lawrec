#pragma once
#include "lawrec_frame_queue.h"
/* Owner 1: RTSP, owner 2: recording. The caller holds its media lease.
 * Queues must outlive unsubscribe. One VENC0/VI binding serves all owners. */
int lawrec_encoder_subscribe(int owner, LawrecFrameQueue *queue);
int lawrec_encoder_unsubscribe(int owner);
int lawrec_encoder_request_idr();
