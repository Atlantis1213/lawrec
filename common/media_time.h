#pragma once
#include "frame.h"
#include "config.h"
#include <algorithm>
#include <limits>
#include <mutex>

namespace demo {
// One epoch for both RTSP tracks. Queue residence never changes PTS spacing.
class MediaClock {
public:
    bool map(uint64_t pts, uint64_t wall_now, uint64_t &wall) {
        std::lock_guard<std::mutex> guard(lock_);
        if (!ready_) { source_ = pts; wall_ = wall_now; ready_ = true; }
        if (pts < source_) {
            uint64_t delta = source_ - pts;
            if (delta > wall_) return false;
            wall = wall_ - delta;
        } else {
            uint64_t delta = pts - source_;
            if (delta > std::numeric_limits<uint64_t>::max() - wall_) return false;
            wall = wall_ + delta;
        }
        return true;
    }
    void reset() { std::lock_guard<std::mutex> guard(lock_); ready_ = false; }
private:
    std::mutex lock_;
    bool ready_ = false;
    uint64_t source_ = 0, wall_ = 0;
};
struct AudioSlice { size_t offset = 0, samples = 0; uint64_t pts_us = 0; };
// G711A mono: one byte per 125-us sample. Assumes SDK PTS names first sample;
// the actual board clock alignment/semantics remain an acceptance requirement.
inline AudioSlice clip_audio(const Frame &frame, uint64_t start, uint64_t end) {
    constexpr uint64_t sample_us = 1000000 / audio_rate;
    static_assert(1000000 % audio_rate == 0, "Exact sample duration required");
    if (end <= start || end <= frame.pts_us || frame.bytes.size() >
        (std::numeric_limits<uint64_t>::max() - frame.pts_us) / sample_us) return {};
    auto ceiling = [](uint64_t delta) { return delta / sample_us + (delta % sample_us != 0); };
    uint64_t first = start > frame.pts_us ? ceiling(start - frame.pts_us) : 0;
    uint64_t last = std::min<uint64_t>(frame.bytes.size(), ceiling(end - frame.pts_us));
    if (first >= last) return {};
    return {size_t(first), size_t(last - first), frame.pts_us + first * sample_us};
}
}
