#pragma once
#include "frame.h"
#include "media_time.h"
#include <cstdio>
#include <mov-writer.h>

namespace demo {
struct MuxedStats {
    uint64_t video_frames = 0, audio_packets = 0, video_bytes = 0, audio_bytes = 0;
    uint64_t last_video_pts = 0, audio_end_pts = 0;
};
// Single recorder thread owns all file/muxer calls. open takes ownership of fd.
class Mp4Sink {
public:
    ~Mp4Sink() { close(); }
    int open(int fd, const Frame &first);
    int video(const Frame &frame);
    int audio(const Frame &frame, uint64_t end_pts);
    int close(uint64_t video_end_pts = 0);
    MuxedStats stats() const { return stats_; }
private:
    int remember(int error);
    static int read(void *, void *, uint64_t);
    static int write(void *, const void *, uint64_t);
    static int seek(void *, int64_t);
    static int64_t tell(void *);
    FILE *file_ = nullptr;
    mov_writer_t *writer_ = nullptr;
    int error_ = 0, video_track_ = -1, audio_track_ = -1;
    uint64_t start_pts_ = 0;
    std::vector<uint8_t> sps_, pps_, sample_;
    MuxedStats stats_;
};
}
