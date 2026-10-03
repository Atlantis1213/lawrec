#include "recorder.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace demo {
namespace {
using Steady = std::chrono::steady_clock;
constexpr uint64_t frame_us = 1000000 / video_fps;
int new_file(const char *directory, char *path, size_t size) {
    if (mkdir(directory, 0755) && errno != EEXIST) return -errno;
    time_t now = time(nullptr); tm utc{};
    if (!gmtime_r(&now, &utc)) return -ERANGE;
    char stamp[32]; if (!strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc)) return -ERANGE;
    int count = std::snprintf(path, size, "%s/clip-%s-XXXXXX.mp4.part", directory, stamp);
    if (count < 0 || size_t(count) >= size) return -ENAMETOOLONG;
    int fd = mkstemps(path, 9); return fd < 0 ? -errno : fd;
}
bool packet_before(const Frame &frame, uint64_t end) {
    return frame.pts_us <= end && frame.bytes.size() <= (end - frame.pts_us) / (1000000 / audio_rate);
}
}
int Recorder::request(bool enabled) {
    std::lock_guard<std::mutex> guard(operations_);
    auto current = state_.load();
    if (!enabled) {
        if (current == StreamState::Starting || current == StreamState::Running) {
            state_ = StreamState::Stopping; stop_ = 1;
            std::fprintf(stderr, "[record] request=stop state=stopping\n");
        }
        return 0;
    }
    if (current == StreamState::Starting || current == StreamState::Running) return 0;
    if (current == StreamState::Stopping) return -EBUSY;
    if (cleanup_error_) return cleanup_error_;
    if (worker_.joinable()) worker_.join();
    stop_ = 0; error_ = 0;
    { std::lock_guard<std::mutex> data(written_lock_); written_ = {}; }
    state_ = StreamState::Starting;
    try { worker_ = std::thread(&Recorder::run, this); }
    catch (...) { error_ = -EAGAIN; state_ = StreamState::Failed; return -EAGAIN; }
    std::fprintf(stderr, "[record] request=start state=starting limit=15s\n"); return 0;
}
void Recorder::publish(const Mp4Sink &sink) {
    std::lock_guard<std::mutex> guard(written_lock_); written_ = sink.stats();
}
void Recorder::run() {
    char path[512]{};
    int fd = -1, result = 0, cleanup = 0;
    bool attached = false, opened = false, have_base = false;
    uint64_t start = 0, limit = 0, video_end = 0;
    FramePtr audio;
    Mp4Sink sink;
    try {
        fd = new_file(directory_, path, sizeof(path));
        if (fd < 0) result = fd;
        if (!result && !stop_) { result = source_.attach(Consumer::Record, feed_); attached = !result; }
        auto video_at = Steady::now(), audio_at = video_at, began = video_at;
        while (!result && attached && !stop_) {
            auto video_queue = feed_.video.stats(), audio_queue = feed_.audio.stats();
            result = source_.stats().error;
            if (!result) result = video_queue.error ? video_queue.error : audio_queue.error;
            if (result) break;
            FramePtr video;
            int ret = feed_.video.pop(video, 0);
            if (ret < 0) { result = ret; break; }
            if (ret) {
                video_at = Steady::now();
                if (!have_base) {
                    if (video->pts_us > std::numeric_limits<uint64_t>::max() - record_duration_us) { result = -ERANGE; break; }
                    start = video->pts_us; limit = start + record_duration_us; have_base = true;
                    int owned_fd = fd; fd = -1;
                    result = sink.open(owned_fd, *video); opened = !result;
                }
                if (!result && video->pts_us >= limit) break;
                if (!result) result = sink.video(*video);
                if (result) break;
                video_end = video->pts_us + std::min(frame_us, limit - video->pts_us);
            }
            if (!audio) {
                ret = feed_.audio.pop(audio, 0);
                if (ret < 0) { result = ret; break; }
                if (ret) audio_at = Steady::now();
            }
            if (audio && have_base && packet_before(*audio, video_end)) {
                result = sink.audio(*audio, video_end); audio.reset();
            }
            auto stats = sink.stats(); publish(sink);
            if (stats.video_frames && stats.audio_packets) {
                StreamState expected = StreamState::Starting;
                if (state_.compare_exchange_strong(expected, StreamState::Running))
                    std::fprintf(stderr, "[record] result=0 state=recording path=%s\n", path);
            }
            auto now = Steady::now();
            if (now - video_at > std::chrono::seconds(3) || now - audio_at > std::chrono::seconds(3) ||
                (now - began > std::chrono::seconds(3) && (!stats.video_frames || !stats.audio_packets))) result = -ETIMEDOUT;
            if (!ret) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    } catch (...) { result = -ENOMEM; }
    state_ = StreamState::Stopping;
    if (attached) cleanup = source_.detach(Consumer::Record, !result);
    if (!cleanup) cleanup = source_.stats().cleanup_error;
    // SDK no longer delivers to this feed. Drain its bounded accepted tail before close.
    try {
        if (!result && opened) {
            FramePtr video;
            int ret;
            while ((ret = feed_.video.pop(video, 0)) == 1) {
                if (video->pts_us >= limit) continue;
                result = sink.video(*video); if (result) break;
                video_end = video->pts_us + std::min(frame_us, limit - video->pts_us);
            }
            if (ret < 0 && ret != -ECANCELED && !result) result = ret;
            while (!result) {
                if (!audio) {
                    ret = feed_.audio.pop(audio, 0);
                    if (ret <= 0) { if (ret < 0 && ret != -ECANCELED) result = ret; break; }
                }
                result = sink.audio(*audio, video_end); audio.reset();
            }
        }
    } catch (...) { result = -ENOMEM; }
    auto stats = sink.stats();
    if (opened && (!stats.video_frames || !stats.audio_packets) && !result) result = -ENODATA;
    int close_result = sink.close(result ? 0 : video_end);
    if (!result) result = close_result;
    if (!result) result = cleanup;
    publish(sink);
    if (fd >= 0 && ::close(fd) && !result) result = -errno;
    if (!opened) {
        if (*path && unlink(path) && errno != ENOENT && !result) result = -errno;
    } else if (!result) {
        char final[sizeof(path)]; size_t length = std::strlen(path) - 5;
        std::memcpy(final, path, length); final[length] = '\0';
        if (rename(path, final)) result = -errno;
        else std::fprintf(stderr, "[record] completed path=%s video=%llu audio_samples=%llu\n", final,
            (unsigned long long)stats.video_frames, (unsigned long long)stats.audio_bytes);
    }
    cleanup_error_ = cleanup; error_ = result;
    state_ = result ? StreamState::Failed : StreamState::Off;
    std::fprintf(stderr, "[record] result=%d cleanup=%d state=%s partial=%s\n", result, cleanup,
        result ? "failed" : "off", result && opened ? path : "none");
}
int Recorder::shutdown() {
    std::lock_guard<std::mutex> guard(operations_); stop_ = 1;
    if (worker_.joinable()) worker_.join();
    return cleanup_error_;
}
RecordStatus Recorder::status() const {
    std::lock_guard<std::mutex> guard(written_lock_);
    return {state_.load(), error_.load(), cleanup_error_.load(), feed_.video.stats(), feed_.audio.stats(), written_};
}
}
