// A fake codec adapter only; server, client, SDP and RTP use the real SDK live555.
#include "rtsp.h"
#include "config.h"
#include <liveMedia.hh>
#include <BasicUsageEnvironment.hh>
#include <GroupsockHelper.hh>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
std::atomic<unsigned> producing{0}, attaches{0}, detaches{0};
std::thread producer;
bool silent = false;
std::vector<uint8_t> sps{0x67, 0x42, 0, 0x1e, 0xaa}, pps{0x68, 0xce};
std::vector<uint8_t> au{0,0,0,1,0x67,0x42,0,0x1e,0xaa,0,0,1,0x68,0xce,
                       0,0,1,0x65,0x88,0x11,0,0,1,0x65,0x88,0x22};
}
namespace demo {
int MediaSource::attach(Consumer consumer, Feed &feed) {
    assert(consumer == Consumer::Rtsp && !producing);
    feed.video.reset(); feed.audio.reset();
    { std::lock_guard<std::mutex> guard(frames_); stats_ = {}; feeds_[0] = &feed; }
    ++attaches; producing = 1;
    producer = std::thread([this, &feed] {
        uint64_t pts = 1000000;
        while (producing) {
            if (!silent) {
                auto video = std::make_shared<Frame>(); video->bytes = au; video->key = true; video->pts_us = pts;
                auto audio = std::make_shared<Frame>(); audio->bytes.assign(audio_samples, 0xd5); audio->pts_us = pts;
                feed.video.push(video); feed.audio.push(audio);
                std::lock_guard<std::mutex> guard(frames_);
                ++stats_.video_frames; ++stats_.audio_packets;
            }
            pts += 40000; std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
    });
    return 0;
}
int MediaSource::detach(Consumer consumer, bool) {
    assert(consumer == Consumer::Rtsp);
    producing = 0; if (producer.joinable()) producer.join();
    ++detaches;
    feeds_[0]->video.close(); feeds_[0]->audio.close(); return 0;
}
SourceStats MediaSource::stats() const { std::lock_guard<std::mutex> guard(frames_); return stats_; }
bool MediaSource::h264_config(std::vector<uint8_t> &a, std::vector<uint8_t> &b) const {
    a = sps; b = pps; return true;
}
}
namespace {
struct Client;
class Sink : public MediaSink {
public:
    Sink(UsageEnvironment &env, MediaSubsession &track, Client &client)
        : MediaSink(env), track_(track), client_(client) {}
private:
    Boolean continuePlaying() override;
    static void received(void *, unsigned, unsigned, timeval, unsigned);
    uint8_t bytes_[demo::max_access_unit]{};
    MediaSubsession &track_;
    Client &client_;
};
struct Client : RTSPClient {
    explicit Client(UsageEnvironment &env, const char *url = nullptr)
        : RTSPClient(env, url ? url : "rtsp://127.0.0.1:8554/lawrec", 0, "demo-check", 0, -1), board(url != nullptr) {}
    MediaSession *session = nullptr;
    std::unique_ptr<MediaSubsessionIterator> iterator;
    MediaSubsession *track = nullptr;
    unsigned audio = 0, slices = 0;
    bool board;
    bool playing = false;
    char done = 0;
    void check() { if (playing && audio >= (board ? 25U : 2U) && slices >= (board ? 30U : 4U)) done = 1; }
    ~Client() override {
        iterator.reset();
        if (session) {
            MediaSubsessionIterator tracks(*session);
            while (auto *sub = tracks.next()) { Medium::close(sub->sink); sub->sink = nullptr; }
            Medium::close(session);
        }
    }
    static void play(RTSPClient *base, int code, char *text) {
        assert(!code); delete[] text;
        auto &c = *static_cast<Client *>(base); c.playing = true; c.check();
    }
    static void setup(RTSPClient *base, int code, char *text) {
        assert(!code); delete[] text;
        auto &c = *static_cast<Client *>(base);
        c.track->sink = new Sink(c.envir(), *c.track, c);
        assert(c.track->sink->startPlaying(*c.track->readSource(), nullptr, nullptr));
        c.next();
    }
    void next() {
        track = iterator->next();
        if (!track) { assert(sendPlayCommand(*session, play)); return; }
        bool video = !std::strcmp(track->mediumName(), "video");
        assert(!std::strcmp(track->codecName(), video ? "H264" : "PCMA"));
        assert(track->rtpTimestampFrequency() == (video ? 90000 : demo::audio_rate));
        if (!video) assert(track->numChannels() == 1);
        assert(track->initiate());
        assert(sendSetupCommand(*track, setup, False, True));
    }
    static void describe(RTSPClient *base, int code, char *text) {
        assert(!code && text);
        assert(std::strstr(text, "H264/90000") && std::strstr(text, "m=audio 0 RTP/AVP 8"));
        assert(std::strstr(text, "sprop-parameter-sets="));
        auto &c = *static_cast<Client *>(base);
        c.session = MediaSession::createNew(c.envir(), text); delete[] text;
        assert(c.session && c.session->hasSubsessions());
        c.iterator.reset(new MediaSubsessionIterator(*c.session)); c.next();
    }
};
Boolean Sink::continuePlaying() {
    if (!fSource) return False;
    fSource->getNextFrame(bytes_, sizeof(bytes_), received, this, onSourceClosure, this); return True;
}
void Sink::received(void *self, unsigned size, unsigned truncated, timeval, unsigned) {
    auto &sink = *static_cast<Sink *>(self); assert(!truncated);
    if (sink.client_.board) {
        assert(size > 0);
        if (!std::strcmp(sink.track_.mediumName(), "video")) {
            auto type = sink.bytes_[0] & 31;
            if (type == 1 || type == 5) ++sink.client_.slices;
        } else {
            assert(size == demo::audio_samples);
            ++sink.client_.audio;
        }
        sink.client_.check(); sink.continuePlaying(); return;
    }
    if (!std::strcmp(sink.track_.mediumName(), "video")) {
        if ((sink.bytes_[0] & 31) == 5) {
            assert(size == 3 && sink.bytes_[1] == 0x88);
            bool final = sink.bytes_[2] == 0x22;
            assert(sink.bytes_[2] == 0x11 || final);
            assert(bool(sink.track_.rtpSource()->curPacketMarkerBit()) == final);
            ++sink.client_.slices;
        }
    } else {
        assert(size == demo::audio_samples);
        for (unsigned i = 0; i < size; ++i) assert(sink.bytes_[i] == 0xd5);
        ++sink.client_.audio;
    }
    sink.client_.check(); sink.continuePlaying();
}
void timeout(void *self) { *static_cast<char *>(self) = 1; }
demo::RtspStatus wait(demo::RtspWorker &server, demo::StreamState state) {
    for (int i = 0; i < 420; ++i) {
        auto status = server.status();
        if (status.state == state) return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(false && "RTSP state deadline"); return {};
}
int occupy_port() {
    int socket = ::socket(AF_INET, SOCK_STREAM, 0); assert(socket >= 0);
    int yes = 1; assert(!setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(demo::rtsp_port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    assert(!bind(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)));
    assert(!listen(socket, 2)); return socket;
}

struct Read {
    uint8_t bytes[64]{};
    timeval at{};
    unsigned size = 0;
    char done = 0;
    static void frame(void *self, unsigned size, unsigned trunc, timeval at, unsigned) {
        auto &r = *static_cast<Read *>(self); assert(!trunc); r.size = size; r.at = at; r.done = 1;
    }
    static void closed(void *self) { static_cast<Read *>(self)->done = 1; }
};
void source_edges(UsageEnvironment &env) {
    demo::MediaClock clock; demo::LiveDelivery delivery;
    demo::Feed feed(true); feed.video.reset(); feed.audio.reset();
    auto *video = new demo::LiveSource(env, feed.video, true, clock, delivery);
    auto *audio = new demo::LiveSource(env, feed.audio, false, clock, delivery);
    auto v = std::make_shared<demo::Frame>(); v->bytes = au; v->pts_us = 1000000; v->key = true;
    auto a = std::make_shared<demo::Frame>(); a->bytes.assign(16, 0xd5); a->pts_us = 1010000;
    assert(!feed.video.push(v) && !feed.audio.push(a));
    Read vr, ar;
    video->getNextFrame(vr.bytes, sizeof(vr.bytes), Read::frame, &vr, Read::closed, &vr);
    env.taskScheduler().doEventLoop(&vr.done);
    audio->getNextFrame(ar.bytes, sizeof(ar.bytes), Read::frame, &ar, Read::closed, &ar);
    env.taskScheduler().doEventLoop(&ar.done);
    assert(ar.size == 16 && vr.size == sps.size());
    auto us = [](timeval t) { return uint64_t(t.tv_sec) * 1000000 + t.tv_usec; };
    assert(us(ar.at) - us(vr.at) == 10000); // Shared source epoch, not dequeue time.
    Read small;
    video->getNextFrame(small.bytes, 1, Read::frame, &small, Read::closed, &small);
    env.taskScheduler().doEventLoop(&small.done);
    assert(!small.size && delivery.error == -EMSGSIZE); // Never truncate a NAL.
    Medium::close(video); Medium::close(audio);
}
}
int main(int argc, char **argv) {
    if (argc == 2) {
        assert(!std::strncmp(argv[1], "rtsp://", 7));
        auto *scheduler = BasicTaskScheduler::createNew();
        auto *env = BasicUsageEnvironment::createNew(*scheduler);
        auto *client = new Client(*env, argv[1]);
        assert(client->sendDescribeCommand(Client::describe));
        TaskToken deadline = scheduler->scheduleDelayedTask(5000000, timeout, &client->done);
        scheduler->doEventLoop(&client->done);
        scheduler->unscheduleDelayedTask(deadline);
        assert(client->playing && client->audio >= 25 && client->slices >= 30);
        std::printf("board RTSP TCP: H264 NALs=%u PCMA packets=%u; actual RTP received, no image/audio subjective check\n",
                    client->slices, client->audio);
        client->sendTeardownCommand(*client->session, nullptr);
        Medium::close(client); assert(env->reclaim()); delete scheduler;
        return 0;
    }
    assert(argc == 1);
    // network-none Docker has only loopback; this hint is test-only, not a NIC change.
    ReceivingInterfaceAddr = SendingInterfaceAddr = htonl(INADDR_LOOPBACK);
    auto *scheduler = BasicTaskScheduler::createNew();
    auto *env = BasicUsageEnvironment::createNew(*scheduler);
    source_edges(*env);
    demo::MediaSource source; demo::RtspWorker server(source);
    assert(!server.request(true) && !server.request(true));
    auto running = wait(server, demo::StreamState::Running);
    assert(!running.error && attaches == 1);
    auto *client = new Client(*env);
    assert(client->sendDescribeCommand(Client::describe));
    TaskToken deadline = scheduler->scheduleDelayedTask(3000000, timeout, &client->done);
    scheduler->doEventLoop(&client->done);
    scheduler->unscheduleDelayedTask(deadline);
    assert(client->playing && client->audio >= 2 && client->slices >= 4);
    auto delivery = server.status(); assert(delivery.video_nals >= 4 && delivery.audio_packets >= 2);
    Medium::close(client);
    assert(!server.request(false)); wait(server, demo::StreamState::Off);
    assert(!server.shutdown() && detaches == 1);
    // Port conflict fails before touching the codecs, even if IPv6 is available.
    int occupied = occupy_port();
    assert(!server.request(true)); auto busy = wait(server, demo::StreamState::Failed);
    assert(busy.error && attaches == 1 && !server.shutdown()); close(occupied);
    // No first IDR/audio: deadline fails visibly, detaches and releases the port.
    silent = true; assert(!server.request(true));
    auto stalled = wait(server, demo::StreamState::Failed);
    assert(stalled.error == -ETIMEDOUT && !server.shutdown());
    assert(attaches == 2 && detaches == 2);
    occupied = occupy_port(); close(occupied);
    assert(env->reclaim()); delete scheduler;
    std::puts("real live555: SDP/TCP RTP H264 multislice markers, PCMA bytes, shared PTS, bounds, stop/port and first-frame deadline passed; no decoder/hardware test");
}
