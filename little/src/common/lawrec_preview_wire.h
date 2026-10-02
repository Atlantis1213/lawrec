#pragma once
#include <stdint.h>
#define LAWREC_PREVIEW_WIRE_VERSION 1U
/* Shared by both cores; request and response carry the same sequence. */
typedef struct {
    uint32_t version;
    uint32_t sequence;
    int32_t result;
} lawrec_preview_wire_t;

/* Read-only display ownership snapshot. It does not certify camera frame delivery. */
#define LAWREC_DISPLAY_STATUS_VERSION 1U
typedef struct {
    uint32_t version;
    uint32_t sequence;
    int32_t result;
    uint32_t backend_ready;
    uint32_t preview_enabled;
    uint32_t preview_bound;
    uint32_t playback_enabled;
} lawrec_display_status_t;
