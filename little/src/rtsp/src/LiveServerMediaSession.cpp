#include <iostream>
#include <sstream>
#include "LiveServerMediaSession.h"
#include "LiveFrameSource.h"

LiveServerMediaSession *LiveServerMediaSession::createNew(UsageEnvironment &env, StreamReplicator *replicator) {
    return new LiveServerMediaSession(env, replicator);
}

// For live streaming, we always set reuseFirstSource False, and use StreamReplicator,
//    so that createNewStreamSource() will be called when a new client comes.
//
LiveServerMediaSession::LiveServerMediaSession(UsageEnvironment &env, StreamReplicator *replicator)
    : OnDemandServerMediaSubsession(env, False/*reuseFirstSource*/), fReplicator(replicator)
    { }

LiveServerMediaSession::~LiveServerMediaSession() {
}

char const *LiveServerMediaSession::getAuxSDPLine(RTPSink *rtpSink, FramedSource *inputSource) {
    const char *auxLine = NULL;
    std::string sdpLine =  ((LiveFrameSource *)(fReplicator->inputSource()))->getAuxLine();
    if (!sdpLine.empty()) {
        std::ostringstream os;
        os << "a=fmtp:" << (int)rtpSink->rtpPayloadType() << " ";
        os << sdpLine;
        os << "\r\n";
        fSdpLine = os.str();
        auxLine = fSdpLine.c_str();

        // std::cout << auxLine << std::endl;
    }
    return auxLine;
}

FramedSource *LiveServerMediaSession::createNewStreamSource(unsigned clientSessionId, unsigned &estBitrate) {
    EncodeType type = ((LiveFrameSource *)fReplicator->inputSource())->GetEncodeType();
    if (type == EncodeType::H264) {
        estBitrate = 2048;
        return H264VideoStreamDiscreteFramer::createNew(envir(), fReplicator->createStreamReplica());
    } else if (type == EncodeType::H265) {
        estBitrate = 2048;
        return H265VideoStreamDiscreteFramer::createNew(envir(), fReplicator->createStreamReplica());
    } else if (type == EncodeType::G711A) {
        estBitrate = 64;
        return fReplicator->createStreamReplica();
    } else {
        std::cout << "createNewStreamSource() -- type not supported yet" << (int) type << std::endl;
        return nullptr;
    }
}

RTPSink *LiveServerMediaSession::createNewRTPSink(Groupsock *rtpGroupsock, unsigned char rtpPayloadTypeIfDynamic, FramedSource* inputSource) {
    EncodeType type = ((LiveFrameSource *)fReplicator->inputSource())->GetEncodeType();
    if (type == EncodeType::H264) {
        OutPacketBuffer::maxSize = LiveFrameSource::kMaxFrameBytes;
        return H264VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
    } else if (type == EncodeType::H265) {
        OutPacketBuffer::maxSize = LiveFrameSource::kMaxFrameBytes;
        return H265VideoRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic);
    } else if (type == EncodeType::G711A) {
        // Sink construction runs on the single live555 scheduler. Do not give
        // tiny G711 packets a video-sized buffer; restore the global limit even
        // if allocation throws, since H264's fragmenter reads it later on PLAY.
        struct RestoreLimit {
            unsigned value;
            ~RestoreLimit() { OutPacketBuffer::maxSize = value; }
        } restore{OutPacketBuffer::maxSize};
        OutPacketBuffer::maxSize = 64 * 1024;
        return SimpleRTPSink::createNew(envir(), rtpGroupsock, rtpPayloadTypeIfDynamic, 8000, "audio", "PCMA", 1, False /*allowMultipleFramesPerPacket*/);
    } else {
        std::cout << "createNewRTPSink() -- type not supported yet" << (int) type << std::endl;
        return nullptr;
    }
}
