#include "frame_queue.h"
#include <algorithm>
#include <cerrno>
#include <chrono>

namespace demo {
void FrameQueue::discard_locked() {
    stats_.dropped += frames_.size(); frames_.clear(); stats_.bytes = 0;
}
void FrameQueue::reset() {
    std::lock_guard<std::mutex> guard(lock_);
    frames_.clear(); stats_ = {}; closed_ = false; waiting_idr_ = video_;
}
int FrameQueue::push(const FramePtr &frame) {
    std::lock_guard<std::mutex> guard(lock_);
    if (closed_) return stats_.error ? stats_.error : -ECANCELED;
    int error = 0;
    bool live_reset = false;
    if (!frame || frame->bytes.empty()) error = -EINVAL;
    else if (!max_frames_ || frame->bytes.size() > max_bytes_) error = -EMSGSIZE;
    if (!error && video_ && waiting_idr_ && !frame->key) { ++stats_.dropped; return 0; }
    auto full = [&] { return frames_.size() >= max_frames_ || frame->bytes.size() > max_bytes_ - stats_.bytes; };
    if (!error && full()) {
        if (policy_ == QueuePolicy::Record) error = -ENOBUFS;
        else if (policy_ == QueuePolicy::LiveVideo) {
            discard_locked(); waiting_idr_ = true; live_reset = true;
            if (!frame->key) { ++stats_.dropped; return 1; }
        } else {
            while (full() && !frames_.empty()) {
                stats_.bytes -= frames_.front()->bytes.size(); frames_.pop_front(); ++stats_.dropped;
            }
        }
    }
    if (!error) try { frames_.push_back(frame); } catch (...) { error = -ENOMEM; }
    if (error) {
        stats_.error = error; closed_ = true; discard_locked(); changed_.notify_all(); return error;
    }
    waiting_idr_ = false; ++stats_.accepted; stats_.bytes += frame->bytes.size();
    stats_.peak_bytes = std::max(stats_.peak_bytes, stats_.bytes);
    changed_.notify_one(); return live_reset ? 1 : 0;
}
int FrameQueue::pop(FramePtr &frame, unsigned timeout_ms) {
    std::unique_lock<std::mutex> guard(lock_); frame.reset();
    changed_.wait_for(guard, std::chrono::milliseconds(timeout_ms), [&] { return closed_ || !frames_.empty(); });
    if (stats_.error) return stats_.error;
    if (frames_.empty()) return closed_ ? -ECANCELED : 0;
    frame = frames_.front(); frames_.pop_front(); stats_.bytes -= frame->bytes.size(); ++stats_.popped;
    return 1;
}
void FrameQueue::finish() {
    std::lock_guard<std::mutex> guard(lock_); closed_ = true; changed_.notify_all();
}
void FrameQueue::close() {
    std::lock_guard<std::mutex> guard(lock_); closed_ = true; discard_locked(); changed_.notify_all();
}
void FrameQueue::discard() {
    std::lock_guard<std::mutex> guard(lock_); discard_locked();
}
void FrameQueue::await_idr() {
    std::lock_guard<std::mutex> guard(lock_);
    discard_locked(); waiting_idr_ = video_;
}
int FrameQueue::fail(int error) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!stats_.error) stats_.error = error < 0 ? error : -EIO;
    closed_ = true; discard_locked(); changed_.notify_all(); return stats_.error;
}
QueueStats FrameQueue::stats() const {
    std::lock_guard<std::mutex> guard(lock_);
    QueueStats copy = stats_; copy.depth = frames_.size(); return copy;
}
}
