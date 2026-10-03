#include "rtsp_stream.h"
#include "config.h"
#include <H264VideoRTPSink.hh>
#include <H264VideoStreamDiscreteFramer.hh>
#include <OnDemandServerMediaSubsession.hh>
#include <SimpleRTPSink.hh>
#include <memory>

namespace demo {
namespace {
class AccessUnitFramer : public H264VideoStreamDiscreteFramer {
public:
    AccessUnitFramer(UsageEnvironment &env, LiveSource *source)
        : H264VideoStreamDiscreteFramer(env, source, False, False), source_(*source) {}
private:
    // The default framer marks every VCL NAL; SDK frames may contain many slices.
    Boolean nalUnitEndsAccessUnit(u_int8_t) override { return source_.access_unit_end(); }
    LiveSource &source_;
};
class Track : public OnDemandServerMediaSubsession {
public:
    Track(UsageEnvironment &env, Feed &feed, bool video, MediaClock &clock, LiveDelivery &delivery,
          const std::vector<uint8_t> &sps, const std::vector<uint8_t> &pps, std::function<void()> idr)
        : OnDemandServerMediaSubsession(env, True), feed_(feed), video_(video), clock_(clock),
          delivery_(delivery), sps_(sps), pps_(pps), idr_(std::move(idr)) {}
private:
    FramedSource *createNewStreamSource(unsigned, unsigned &bitrate) override {
        bitrate = video_ ? video_bitrate_kbps : audio_rate * 8 / 1000;
        if (video_) {
            feed_.video.await_idr(); feed_.audio.discard(); idr_();
            auto *source = new LiveSource(envir(), feed_.video, true, clock_, delivery_);
            try { return new AccessUnitFramer(envir(), source); }
            catch (...) { Medium::close(source); throw; }
        }
        return new LiveSource(envir(), feed_.audio, false, clock_, delivery_);
    }
    RTPSink *createNewRTPSink(Groupsock *socket, unsigned char payload, FramedSource *) override {
        if (video_) return H264VideoRTPSink::createNew(envir(), socket, payload,
            sps_.data(), sps_.size(), pps_.data(), pps_.size());
        return SimpleRTPSink::createNew(envir(), socket, 8, audio_rate, "audio", "PCMA", 1, False);
    }
    Feed &feed_;
    bool video_;
    MediaClock &clock_;
    LiveDelivery &delivery_;
    std::vector<uint8_t> sps_, pps_;
    std::function<void()> idr_;
};
struct Close { void operator()(Medium *medium) const { Medium::close(medium); } };
}
ServerMediaSession *create_stream(UsageEnvironment &env, Feed &feed, MediaClock &clock,
    LiveDelivery &delivery, const std::vector<uint8_t> &sps, const std::vector<uint8_t> &pps,
    std::function<void()> request_idr) {
    if (sps.empty() || pps.empty()) return nullptr;
    std::unique_ptr<ServerMediaSession, Close> session(
        ServerMediaSession::createNew(env, rtsp_name, rtsp_name, "K230 H264 / G711A"));
    if (!session) return nullptr;
    for (bool video : {true, false}) {
        std::unique_ptr<Track, Close> track(new Track(env, feed, video, clock, delivery, sps, pps, request_idr));
        if (!session->addSubsession(track.get())) return nullptr;
        track.release();
    }
    return session.release();
}
}
