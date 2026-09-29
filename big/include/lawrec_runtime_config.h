#pragma once

/*
 * Centralized runtime-facing constants for the current lawrec transition
 * phase. Runtime compatibility stays stable even while the internal project
 * structure is being cleaned up.
 */

#define LAWREC_IPC_SERVICE_NAME "door_lock"
#define LAWREC_BUILD_TAG "lawrec-lckfb-2026-04-12-big-ab-osd-disabled"
#define LAWREC_RTSP_DEFAULT_PORT 8554
#define LAWREC_RTSP_DEFAULT_STREAM_NAME "lawrec"

/*
 * Current bring-up defaults:
 * - preview owner remains on big core
 * - AI pipeline is optional and disabled by default for the lckfb path
 */
#define LAWREC_STAGE0_PREVIEW_ONLY 0
#define LAWREC_ENABLE_AI_PIPELINE 0
