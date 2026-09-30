#pragma once
#include <cstdint>
#include <limits>
#include <mutex>

// One anchor per RTSP session, shared by audio and video. Arrival time only
// establishes the epoch; later queue latency must never alter media spacing.
class LawrecMediaClock {
    std::mutex mutex_;
    bool initialized_ = false;
    uint64_t source_ = 0, wall_ = 0;
public:
    void reset() {
        std::lock_guard<std::mutex> guard(mutex_);
        initialized_ = false;
    }
    bool map(uint64_t pts, uint64_t wall_now, uint64_t &result) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!initialized_) {
            source_ = pts; wall_ = wall_now; initialized_ = true;
        }
        // A delayed audio frame can legitimately precede the video anchor.
        if (pts < source_) {
            uint64_t delta = source_ - pts;
            if (delta > wall_) return false;
            result = wall_ - delta;
        } else {
            uint64_t delta = pts - source_;
            if (delta > std::numeric_limits<uint64_t>::max() - wall_) return false;
            result = wall_ + delta;
        }
        return true;
    }
};
