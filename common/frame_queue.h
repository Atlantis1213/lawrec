#pragma once
#include "frame.h"
#include <condition_variable>
#include <deque>
#include <mutex>

namespace demo {
enum class QueuePolicy { Record, LiveVideo, LiveAudio };
struct QueueStats {
    size_t depth = 0, bytes = 0, peak_bytes = 0;
    uint64_t accepted = 0, popped = 0, dropped = 0;
    int error = 0;
};
// One queue per track per consumer, never shared between RTSP and recording.
class FrameQueue {
public:
    FrameQueue(QueuePolicy policy, bool video, size_t frames, size_t bytes)
        : policy_(policy), video_(video), max_frames_(frames), max_bytes_(bytes) {}
    void reset();
    // 0=accepted/skipped, 1=live GOP reset/request IDR, negative=closed/error.
    int push(const FramePtr &frame);
    // 1=frame, 0=deadline, negative=closed/error. Finish drains accepted frames.
    int pop(FramePtr &frame, unsigned timeout_ms);
    void finish();
    void close();
    void discard();
    int fail(int error);
    QueueStats stats() const;
private:
    void discard_locked();
    mutable std::mutex lock_;
    std::condition_variable changed_;
    std::deque<FramePtr> frames_;
    const QueuePolicy policy_;
    const bool video_;
    const size_t max_frames_, max_bytes_;
    bool closed_ = true, waiting_idr_ = true;
    QueueStats stats_;
};
struct Feed {
    explicit Feed(bool live)
        : video(live ? QueuePolicy::LiveVideo : QueuePolicy::Record, true, 90, 8 * 1024 * 1024),
          audio(live ? QueuePolicy::LiveAudio : QueuePolicy::Record, false, 100, 128 * 1024) {}
    FrameQueue video, audio;
};
}
