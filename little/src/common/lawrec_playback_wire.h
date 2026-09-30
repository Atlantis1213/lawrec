#pragma once
#include <stdint.h>
#define LAWREC_PLAYBACK_VERSION 1u
typedef struct {
    uint32_t version;
    uint32_t enabled;
    uint32_t width;
    uint32_t height;
} lawrec_playback_wire_t;
