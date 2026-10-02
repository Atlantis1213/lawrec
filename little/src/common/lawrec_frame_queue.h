#pragma once
#include <cstdint>
#include <cstddef>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

struct LawrecEncodedFrame {
    std::vector<uint8_t> bytes;
    uint64_t pts_us = 0;
};
using LawrecFramePtr = std::shared_ptr<const LawrecEncodedFrame>;

struct LawrecFrameQueueStats {
    size_t depth = 0, bytes = 0, peak_depth = 0, peak_bytes = 0;
    uint64_t accepted = 0, popped = 0, discarded = 0, rejected = 0;
    uint64_t max_residence_us = 0, oldest_age_us = 0;
    bool closed = true;
    int error = 0;
};

/* A subscriber never retains an SDK-owned pointer. Immutable shared frames can
 * later feed RTSP and recording without a second copy. Recording overflow is
 * fatal: dropping arbitrary inter-frames would silently corrupt the file. */
class LawrecFrameQueue {
    using Clock = std::chrono::steady_clock;
    struct Entry { LawrecFramePtr frame; Clock::time_point arrived; };
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Entry> frames_;
    const size_t max_frames_, max_bytes_;
    size_t bytes_ = 0;
    bool closed_ = true;
    int error_ = 0;
    LawrecFrameQueueStats stats_;
    void discard_locked() {
        stats_.discarded += frames_.size();
        frames_.clear(); bytes_ = 0;
    }
public:
    LawrecFrameQueue(size_t frames, size_t bytes) : max_frames_(frames), max_bytes_(bytes) {}
    void reset() {
        std::lock_guard<std::mutex> guard(mutex_);
        frames_.clear(); bytes_ = 0; error_ = 0; closed_ = false;
        stats_ = {};
    }
    int fail(int error) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) return error_ ? error_ : -ECANCELED;
        error_ = error < 0 ? error : -EIO;
        closed_ = true; discard_locked();
        changed_.notify_all();
        return error_;
    }
    int push(const LawrecFramePtr &frame) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) { ++stats_.rejected; return error_ ? error_ : -ECANCELED; }
        int error = 0;
        if (!frame || frame->bytes.empty()) error = -EINVAL;
        else if (frames_.size() >= max_frames_ || frame->bytes.size() > max_bytes_-bytes_) error = -ENOBUFS;
        if (!error) {
            try { frames_.push_back({frame, Clock::now()}); }
            catch (...) { error = -ENOMEM; }
        }
        if (error) {
            ++stats_.rejected;
            error_ = error; closed_ = true; discard_locked();
            changed_.notify_all(); return error;
        }
        bytes_ += frame->bytes.size();
        ++stats_.accepted;
        if (frames_.size() > stats_.peak_depth) stats_.peak_depth = frames_.size();
        if (bytes_ > stats_.peak_bytes) stats_.peak_bytes = bytes_;
        changed_.notify_one(); return 0;
    }
    /* 1=frame, 0=timeout, negative=closed/error. finish retains the accepted
     * tail; close discards it. Neither operation clears a delivery failure. */
    int pop(LawrecFramePtr &frame, unsigned timeout_ms) {
        std::unique_lock<std::mutex> guard(mutex_);
        frame.reset();
        changed_.wait_for(guard, std::chrono::milliseconds(timeout_ms), [&] { return closed_ || !frames_.empty(); });
        if (error_) return error_;
        if (frames_.empty()) return closed_ ? -ECANCELED : 0;
        const auto &entry = frames_.front();
        const uint64_t age = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - entry.arrived).count();
        if (age > stats_.max_residence_us) stats_.max_residence_us = age;
        frame = entry.frame; frames_.pop_front(); bytes_ -= frame->bytes.size();
        ++stats_.popped;
        return 1;
    }
    void close() {
        std::lock_guard<std::mutex> guard(mutex_);
        closed_ = true; discard_locked(); changed_.notify_all();
    }
    void finish() {
        std::lock_guard<std::mutex> guard(mutex_);
        closed_ = true; changed_.notify_all();
    }
    int error() {
        std::lock_guard<std::mutex> guard(mutex_);
        return error_;
    }
    /* Snapshot under the queue mutex only; no SDK calls or capture-PTS clock.
     * accepted - popped - discarded == depth, even after failure/close.
     * History survives finish/close and resets with the next subscription. */
    LawrecFrameQueueStats stats() const {
        std::lock_guard<std::mutex> guard(mutex_);
        auto result = stats_;
        result.depth = frames_.size(); result.bytes = bytes_;
        result.closed = closed_; result.error = error_;
        if (!frames_.empty()) result.oldest_age_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - frames_.front().arrived).count();
        return result;
    }
};
