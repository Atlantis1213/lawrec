#pragma once

typedef enum {
    LAWREC_RUNMODE_IDLE = 0,
    LAWREC_RUNMODE_PREVIEW_ONLY,
    LAWREC_RUNMODE_PREVIEW_AND_AI,
    LAWREC_RUNMODE_PREVIEW_AND_RTSP,
    LAWREC_RUNMODE_PREVIEW_AND_RECORD
} lawrec_runmode_t;

typedef struct {
    int preview_backend_ready;
    int preview_enabled;
    int ai_enabled;
    int rtsp_enabled;
    int record_enabled;
} lawrec_runtime_state_t;
