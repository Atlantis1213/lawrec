#include "rtsp.h"
#include "rtsp_stream.h"
#include "config.h"
#include <BasicUsageEnvironment.hh>
#include <RTSPServer.hh>
#include <GroupsockHelper.hh>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <memory>

namespace demo {
namespace {
using Steady = std::chrono::steady_clock;
class IPv4Server : public RTSPServer {
public:
    static IPv4Server *create(UsageEnvironment &env) {
        Port port(rtsp_port);
        int socket = setUpOurSocket(env, port, AF_INET);
        if (socket < 0) return nullptr;
        try { return new IPv4Server(env, socket, port); }
        catch (...) { ::closeSocket(socket); throw; }
    }
private:
    IPv4Server(UsageEnvironment &env, int socket, Port port)
        : RTSPServer(env, socket, -1, port, nullptr, 30) {}
};
struct Reactor {
    UsageEnvironment &env;
    MediaSource &source;
    Feed &feed;
    LiveDelivery &delivery;
    std::atomic<unsigned> &stop;
    char done = 0;
    int error = 0;
    TaskToken task = nullptr;
    uint64_t video_count = 0, audio_count = 0;
    Steady::time_point video_at = Steady::now(), audio_at = video_at;
    static void poll(void *self) {
        auto &r = *static_cast<Reactor *>(self); r.task = nullptr;
        if (r.stop) { r.done = 1; return; }
        auto stats = r.source.stats(); auto now = Steady::now();
        if (stats.video_frames != r.video_count) { r.video_count = stats.video_frames; r.video_at = now; }
        if (stats.audio_packets != r.audio_count) { r.audio_count = stats.audio_packets; r.audio_at = now; }
        r.error = stats.error ? stats.error : r.delivery.error.load();
        if (!r.error) r.error = r.feed.video.stats().error;
        if (!r.error) r.error = r.feed.audio.stats().error;
        if (!r.error && (now - r.video_at > std::chrono::seconds(3) ||
                         now - r.audio_at > std::chrono::seconds(3))) r.error = -ETIMEDOUT;
        if (r.error) r.done = 1;
        else r.task = r.env.taskScheduler().scheduleDelayedTask(20000, poll, &r);
    }
    ~Reactor() { env.taskScheduler().unscheduleDelayedTask(task); }
};
struct Close { void operator()(Medium *value) const { Medium::close(value); } };
struct Reclaim { void operator()(UsageEnvironment *value) const {
    if (!value->reclaim()) std::fprintf(stderr, "[rtsp] environment still owns live555 objects\n");
} };
}
int RtspWorker::request(bool enabled) {
    std::lock_guard<std::mutex> guard(operations_);
    auto current = state_.load();
    if (!enabled) {
        if (current == StreamState::Starting || current == StreamState::Running) {
            state_ = StreamState::Stopping; stop_ = 1;
            std::fprintf(stderr, "[rtsp] request=stop state=stopping\n");
        }
        return 0;
    }
    if (current == StreamState::Starting || current == StreamState::Running) return 0;
    if (current == StreamState::Stopping) return -EBUSY;
    if (cleanup_error_) return cleanup_error_;
    if (worker_.joinable()) worker_.join();
    stop_ = 0; error_ = 0; delivery_.error = 0;
    delivery_.video_nals = 0; delivery_.audio_packets = 0;
    clock_.reset();
    state_ = StreamState::Starting;
    try { worker_ = std::thread(&RtspWorker::run, this); }
    catch (...) { error_ = -EAGAIN; state_ = StreamState::Failed; return -EAGAIN; }
    std::fprintf(stderr, "[rtsp] request=start state=starting\n");
    return 0;
}
void RtspWorker::run() {
    int result = 0, cleanup = 0; bool attached = false;
    try {
        std::unique_ptr<TaskScheduler> scheduler(BasicTaskScheduler::createNew());
        if (!scheduler) result = -ENOMEM;
        std::unique_ptr<UsageEnvironment, Reclaim> env(
            scheduler ? BasicUsageEnvironment::createNew(*scheduler) : nullptr);
        if (!env) result = -ENOMEM;
        errno = 0;
        std::unique_ptr<IPv4Server, Close> server(env ? IPv4Server::create(*env) : nullptr);
        if (!server && !result) {
            result = errno ? -errno : -EADDRINUSE;
            std::fprintf(stderr, "[rtsp] IPv4 bind failed error=%d detail=%s\n", result, env->getResultMsg());
        }
        if (!result && !stop_) { result = source_.attach(Consumer::Rtsp, feed_); attached = !result; }
        auto deadline = Steady::now() + std::chrono::seconds(3);
        std::vector<uint8_t> sps, pps;
        while (!result && attached && !stop_) {
            auto video = feed_.video.stats(), audio = feed_.audio.stats();
            result = source_.stats().error;
            if (!result) result = video.error ? video.error : audio.error;
            if (result || (video.accepted && audio.accepted && source_.h264_config(sps, pps))) break;
            if (Steady::now() >= deadline) { result = -ETIMEDOUT; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!result && attached && !stop_) {
            OutPacketBuffer::maxSize = max_access_unit;
            auto *session = create_stream(*env, feed_, clock_, delivery_, sps, pps,
                                         [this] { source_.request_idr(); });
            if (!session) result = -ENOMEM;
            else {
                server->addServerMediaSession(session);
                StreamState expected = StreamState::Starting;
                if (state_.compare_exchange_strong(expected, StreamState::Running)) {
                    std::fprintf(stderr, "[rtsp] result=0 state=running url=rtsp://<board-ip>:%u/%s\n", rtsp_port, rtsp_name);
                    Reactor reactor{*env, source_, feed_, delivery_, stop_};
                    Reactor::poll(&reactor);
                    scheduler->doEventLoop(&reactor.done);
                    result = reactor.error;
                }
            }
        }
        // Destruction order: clients/sources, environment, scheduler, then SDK detach.
    } catch (...) { result = -ENOMEM; }
    if (attached) cleanup = source_.detach(Consumer::Rtsp);
    if (!cleanup) cleanup = source_.stats().cleanup_error;
    cleanup_error_ = cleanup; error_ = result ? result : cleanup;
    state_ = error_ ? StreamState::Failed : StreamState::Off;
    std::fprintf(stderr, "[rtsp] result=%d cleanup=%d state=%s\n", error_.load(), cleanup,
        error_ ? "failed" : "off");
}
int RtspWorker::shutdown() {
    std::lock_guard<std::mutex> guard(operations_);
    stop_ = 1;
    if (worker_.joinable()) worker_.join();
    return cleanup_error_;
}
RtspStatus RtspWorker::status() const {
    return {state_.load(), error_.load(), cleanup_error_.load(), feed_.video.stats(), feed_.audio.stats(),
        delivery_.video_nals.load(), delivery_.audio_packets.load()};
}
}
