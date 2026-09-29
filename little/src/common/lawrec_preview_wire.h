#pragma once
#include <stdint.h>
#define LAWREC_PREVIEW_WIRE_VERSION 1U
/* Shared by both cores; request and response carry the same sequence. */
typedef struct {
    uint32_t version;
    uint32_t sequence;
    int32_t result;
} lawrec_preview_wire_t;
