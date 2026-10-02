#ifndef LAWREC_MP4_IO_H
#define LAWREC_MP4_IO_H

#include "mp4_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Selected SDK muxer adaptation: detect deferred stdio errors before publish.
 * Only the recording worker may flush/write/close a handle. Not an fsync. */
int lawrec_mp4_muxer_flush(KD_HANDLE handle);
/* Set the exclusive end PTS of the last sample before destroying tracks.
 * Same microsecond, segment-relative clock as write_frame; metadata rounds up
 * to the SDK's millisecond timebase. Does not write another media sample. */
int lawrec_mp4_muxer_end_track(KD_HANDLE handle, KD_HANDLE track, uint64_t end_us);

typedef struct {
    uint64_t sample_count, sample_capacity, sample_index_bytes, media_bytes;
    uint32_t track_count;
} lawrec_mp4_muxer_stats_t;
/* Worker-only, O(track count). Index bytes are allocated sample metadata, not
 * container size, total muxer heap, process RSS, or hardware VB memory. */
int lawrec_mp4_muxer_stats(KD_HANDLE handle, lawrec_mp4_muxer_stats_t *stats);

#ifdef __cplusplus
}
#endif
#endif
