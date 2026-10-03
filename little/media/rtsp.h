#pragma once
#include "source.h"
#include "live_source.h"
#include <thread>

namespace demo {
enum class StreamState : unsigned { Off, Starting, Running, Stopping, Failed };
struct RtspStatus {
    StreamState state = StreamState::Off;
    int error = 0, cleanup_error = 0;
    QueueStats video, audio;
    uint64_t video_nals = 0, audio_packets = 0;
};
class RtspWorker {
public:
    explicit RtspWorker(MediaSource &source) : source_(source) {}
    ~RtspWorker() { shutdown(); }
    int request(bool enabled);
    int shutdown();
    RtspStatus status() const;
private:
    void run();
    MediaSource &source_;
    Feed feed_{true};
    MediaClock clock_;
    LiveDelivery delivery_;
    std::mutex operations_;
    std::thread worker_;
    std::atomic<StreamState> state_{StreamState::Off};
    std::atomic<unsigned> stop_{0};
    std::atomic<int> error_{0}, cleanup_error_{0};
};
}
