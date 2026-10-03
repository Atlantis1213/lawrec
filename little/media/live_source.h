#pragma once
#include "frame_queue.h"
#include "media_time.h"
#include <FramedSource.hh>
#include <atomic>

namespace demo {
struct LiveDelivery {
    std::atomic<int> error{0};
    std::atomic<uint64_t> video_nals{0}, audio_packets{0};
};
// All live555 calls and frame cursors belong to the RTSP reactor thread.
class LiveSource : public FramedSource {
public:
    LiveSource(UsageEnvironment &env, FrameQueue &queue, bool video,
               MediaClock &clock, LiveDelivery &delivery);
    bool access_unit_end() const { return access_unit_end_; }
protected:
    ~LiveSource() override;
private:
    void doGetNextFrame() override;
    void doStopGettingFrames() override;
    static void retry(void *self);
    void deliver();
    void fail(int error);
    FrameQueue &queue_;
    const bool video_;
    MediaClock &clock_;
    LiveDelivery &delivery_;
    FramePtr frame_;
    std::vector<Nal> nals_;
    size_t cursor_ = 0;
    bool access_unit_end_ = false;
};
}
