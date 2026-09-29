#include "rtsp_session.h"

#include <cstdio>

LawrecRtspSession::LawrecRtspSession() = default;
LawrecRtspSession::~LawrecRtspSession()
{
    DeInit();
}

SessionAttr LawrecRtspSession::BuildSessionAttr() const
{
    SessionAttr attr;
    attr.with_audio = false;
    attr.with_audio_backchannel = false;
    attr.with_video = config_.video_valid;

    switch (config_.video_type) {
    case KdMediaVideoType::kVideoTypeH264:
        attr.video_type = VideoType::kVideoTypeH264;
        break;
    case KdMediaVideoType::kVideoTypeH265:
        attr.video_type = VideoType::kVideoTypeH265;
        break;
    case KdMediaVideoType::kVideoTypeMjpeg:
        attr.video_type = VideoType::kVideoTypeMjpeg;
        break;
    default:
        attr.video_type = VideoType::kVideoTypeH264;
        break;
    }

    return attr;
}

KdMediaInputConfig LawrecRtspSession::BuildMediaConfig() const
{
    KdMediaInputConfig config;
    config.video_valid = config_.video_valid;
    config.sensor_type = config_.sensor_type;
    config.video_type = config_.video_type;
    config.venc_width = config_.venc_width;
    config.venc_height = config_.venc_height;
    config.bitrate_kbps = config_.bitrate_kbps;
    return config;
}

int LawrecRtspSession::Init(const lawrec_rtsp_config_t &config)
{
    if (initialized_.load())
        return 0;

    config_ = config;

    if (rtsp_server_.Init(config_.port, nullptr) < 0) {
        printf("[lawrec-rtsp] rtsp server init failed, port=%d\n", config_.port);
        return -1;
    }

    if (rtsp_server_.CreateSession(config_.stream_name, BuildSessionAttr()) < 0) {
        printf("[lawrec-rtsp] create session failed, stream=%s\n",
               config_.stream_name.c_str());
        rtsp_server_.DeInit();
        return -1;
    }

    if (media_.Init(BuildMediaConfig()) < 0) {
        printf("[lawrec-rtsp] media init failed\n");
        rtsp_server_.DestroySession(config_.stream_name);
        rtsp_server_.DeInit();
        return -1;
    }

    if (config_.video_valid && media_.CreateVcapVEnc(this) < 0) {
        printf("[lawrec-rtsp] CreateVcapVEnc failed\n");
        media_.Deinit();
        rtsp_server_.DestroySession(config_.stream_name);
        rtsp_server_.DeInit();
        return -1;
    }

    initialized_.store(true);
    printf("[lawrec-rtsp] init ok, url=%s\n", Url().c_str());
    return 0;
}

int LawrecRtspSession::Start()
{
    if (!initialized_.load())
        return -1;
    if (started_.load())
        return 0;

    rtsp_server_.Start();
    if (config_.video_valid && media_.StartVcapVEnc() < 0) {
        printf("[lawrec-rtsp] StartVcapVEnc failed\n");
        rtsp_server_.Stop();
        return -1;
    }

    started_.store(true);
    printf("[lawrec-rtsp] started, url=%s\n", Url().c_str());
    return 0;
}

int LawrecRtspSession::Stop()
{
    if (!started_.load())
        return 0;

    if (config_.video_valid)
        media_.StopVcapVEnc();
    rtsp_server_.Stop();
    started_.store(false);
    printf("[lawrec-rtsp] stopped\n");
    return 0;
}

int LawrecRtspSession::DeInit()
{
    if (!initialized_.load())
        return 0;

    Stop();
    if (config_.video_valid)
        media_.DestroyVcapVEnc();
    media_.Deinit();
    rtsp_server_.DestroySession(config_.stream_name);
    rtsp_server_.DeInit();
    initialized_.store(false);
    return 0;
}

bool LawrecRtspSession::IsStarted() const
{
    return started_.load();
}

const std::string &LawrecRtspSession::StreamName() const
{
    return config_.stream_name;
}

std::string LawrecRtspSession::Url() const
{
    return std::string("rtsp://<board-ip>:") + std::to_string(config_.port) +
           "/" + config_.stream_name;
}

void LawrecRtspSession::OnVEncData(k_u32 chn_id, void *data, size_t size,
                                   k_venc_pack_type type, uint64_t timestamp)
{
    (void)chn_id;
    (void)type;
    if (!started_.load())
        return;
    rtsp_server_.SendVideoData(config_.stream_name,
                               reinterpret_cast<const uint8_t *>(data),
                               size, timestamp);
}
