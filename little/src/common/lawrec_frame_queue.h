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

/* A subscriber never retains an SDK-owned pointer. Immutable shared frames can
 * later feed RTSP and recording without a second copy. Recording overflow is
 * fatal: dropping arbitrary inter-frames would silently corrupt the file. */
class LawrecFrameQueue {
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<LawrecFramePtr> frames_;
    const size_t max_frames_, max_bytes_;
    size_t bytes_ = 0;
    bool closed_ = true;
    int error_ = 0;
public:
    LawrecFrameQueue(size_t frames, size_t bytes) : max_frames_(frames), max_bytes_(bytes) {}
    void reset() {
        std::lock_guard<std::mutex> guard(mutex_);
        frames_.clear(); bytes_ = 0; error_ = 0; closed_ = false;
    }
    int fail(int error) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) return error_ ? error_ : -ECANCELED;
        error_ = error < 0 ? error : -EIO;
        closed_ = true; frames_.clear(); bytes_ = 0;
        changed_.notify_all();
        return error_;
    }
    int push(const LawrecFramePtr &frame) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) return error_ ? error_ : -ECANCELED;
        int error = 0;
        if (!frame || frame->bytes.empty()) error = -EINVAL;
        else if (frames_.size() >= max_frames_ || frame->bytes.size() > max_bytes_-bytes_) error = -ENOBUFS;
        if (!error) {
            try { frames_.push_back(frame); }
            catch (...) { error = -ENOMEM; }
        }
        if (error) {
            error_ = error; closed_ = true; frames_.clear(); bytes_ = 0;
            changed_.notify_all(); return error;
        }
        bytes_ += frame->bytes.size();
        changed_.notify_one(); return 0;
    }
    /* 1=frame, 0=timeout, negative=closed/error. Explicit close discards tail
     * frames: the user stop boundary is the frame currently being written. */
    int pop(LawrecFramePtr &frame, unsigned timeout_ms) {
        std::unique_lock<std::mutex> guard(mutex_);
        frame.reset();
        changed_.wait_for(guard, std::chrono::milliseconds(timeout_ms), [&] { return closed_ || !frames_.empty(); });
        if (error_) return error_;
        if (frames_.empty()) return closed_ ? -ECANCELED : 0;
        frame = frames_.front(); frames_.pop_front(); bytes_ -= frame->bytes.size();
        return 1;
    }
    void close() {
        std::lock_guard<std::mutex> guard(mutex_);
        closed_ = true; frames_.clear(); bytes_ = 0; changed_.notify_all();
    }
};
