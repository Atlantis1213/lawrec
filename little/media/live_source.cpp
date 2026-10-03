#include "live_source.h"
#include "config.h"
#include <cerrno>
#include <cstring>
#include <limits>
#include <sys/time.h>

namespace demo {
LiveSource::LiveSource(UsageEnvironment &env, FrameQueue &queue, bool video,
                       MediaClock &clock, LiveDelivery &delivery)
    : FramedSource(env), queue_(queue), video_(video), clock_(clock), delivery_(delivery) {}
LiveSource::~LiveSource() { envir().taskScheduler().unscheduleDelayedTask(nextTask()); }
void LiveSource::doGetNextFrame() {
    // Avoid recursively delivering a large AU into live555's packet assembler.
    nextTask() = envir().taskScheduler().scheduleDelayedTask(0, retry, this);
}
void LiveSource::doStopGettingFrames() {
    envir().taskScheduler().unscheduleDelayedTask(nextTask());
    frame_.reset(); nals_.clear(); cursor_ = 0;
}
void LiveSource::retry(void *self) {
    auto &source = *static_cast<LiveSource *>(self);
    source.nextTask() = nullptr;
    try { source.deliver(); } catch (...) { source.fail(-ENOMEM); }
}
void LiveSource::fail(int error) {
    int empty = 0; delivery_.error.compare_exchange_strong(empty, error);
    queue_.fail(error); handleClosure();
}
void LiveSource::deliver() {
    if (!isCurrentlyAwaitingData()) return;
    if (!frame_) {
        int ret = queue_.pop(frame_, 0);
        if (ret < 0) { if (ret != -ECANCELED) fail(ret); else handleClosure(); return; }
        if (!ret) {
            nextTask() = envir().taskScheduler().scheduleDelayedTask(2000, retry, this);
            return;
        }
        if (video_) {
            ret = split_h264(frame_->bytes, nals_);
            if (ret) { fail(ret); return; }
            cursor_ = 0;
        }
    }
    size_t offset = 0, size = frame_->bytes.size();
    access_unit_end_ = !video_;
    if (video_) {
        const auto &nal = nals_[cursor_]; offset = nal.offset; size = nal.size;
        access_unit_end_ = ++cursor_ == nals_.size();
    }
    if (!size || size > fMaxSize) { fail(-EMSGSIZE); return; }
    timeval now{}; uint64_t wall = 0;
    if (gettimeofday(&now, nullptr) < 0 || now.tv_sec < 0 ||
        !clock_.map(frame_->pts_us, uint64_t(now.tv_sec) * 1000000 + now.tv_usec, wall) ||
        wall / 1000000 > uint64_t(std::numeric_limits<time_t>::max())) { fail(-ERANGE); return; }
    fPresentationTime.tv_sec = wall / 1000000; fPresentationTime.tv_usec = wall % 1000000;
    fFrameSize = size; fNumTruncatedBytes = 0;
    fDurationInMicroseconds = video_ ? (access_unit_end_ ? 1000000 / video_fps : 0) : size * (1000000 / audio_rate);
    std::memcpy(fTo, frame_->bytes.data() + offset, size);
    if (video_) ++delivery_.video_nals; else ++delivery_.audio_packets;
    if (access_unit_end_) frame_.reset();
    FramedSource::afterGetting(this);
}
}
