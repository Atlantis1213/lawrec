#pragma once
#include "source.h"
#include "worker_state.h"
#include "mp4_sink.h"
#include <thread>

namespace demo {
struct RecordStatus {
    StreamState state = StreamState::Off;
    int error = 0, cleanup_error = 0;
    QueueStats video, audio;
    MuxedStats written;
};
class Recorder {
public:
    explicit Recorder(MediaSource &source, const char *directory = record_directory)
        : source_(source), directory_(directory) {}
    ~Recorder() { shutdown(); }
    int request(bool enabled);
    int shutdown();
    RecordStatus status() const;
private:
    void run();
    void publish(const Mp4Sink &sink);
    MediaSource &source_;
    const char *directory_;
    Feed feed_{false};
    std::thread worker_;
    std::mutex operations_;
    mutable std::mutex written_lock_;
    MuxedStats written_;
    std::atomic<StreamState> state_{StreamState::Off};
    std::atomic<unsigned> stop_{0};
    std::atomic<int> error_{0}, cleanup_error_{0};
};
}
