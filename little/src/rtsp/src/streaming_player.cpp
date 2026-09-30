#include <functional>
#include <chrono>
#include <thread>
#include <stdexcept>
#include "streaming_player.h"
#include "lawrec_settings.h"
#include "lawrec_encoder.h"
#include "lawrec_audio.h"
#include "lawrec_media_clock.h"
#include "../../common/lawrec_media.h"
#include "../../common/lawrec_config.h"

#define AUDIO_PERSEC_DIV_NUM 25
#define VI_ALIGN_UP(addr, size) (((addr)+((size)-1U))&(~((size)-1U)))

extern "C" void kd_mapi_media_init_workaround(k_bool media_init_flag);

static constexpr int kLawrecMapiMediaAlreadyInitialized =
    static_cast<int>(0xB0008012u);

struct AudioSessionInfo {
    G711LiveFrameSource *g711LiveSource;
    StreamReplicator *g711Replicator;
};

static SessionInfo session_info[MAX_SESSION_NUM];
static AudioSessionInfo audio_session;
static std::atomic<unsigned long> g_video_cb_count[MAX_SESSION_NUM];
static std::atomic<unsigned long> g_audio_cb_count{0};
static std::atomic<bool> g_rtsp_stopping{false};
static LawrecMediaClock g_presentation_clock;

static uint64_t presentation_time(uint64_t pts) {
    struct timeval now;
    if (gettimeofday(&now, nullptr)) throw std::runtime_error("RTSP wall clock unavailable");
    uint64_t mapped;
    if (!g_presentation_clock.map(pts, uint64_t(now.tv_sec) * 1000000 + now.tv_usec, mapped))
        throw std::runtime_error("RTSP timestamp overflow");
    return mapped;
}
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
/*
 * The current LCKFB lawrec build disables the AI pipeline on big core, so
 * chn1 is the stable 1280x720 RTSP feed. chn2 was configured but did not
 * actually produce frames in the running online pipeline.
 */
static constexpr k_vicap_chn kLawrecRtspVicapChn = VICAP_CHN_ID_1;
#else
static constexpr k_vicap_chn kLawrecRtspVicapChn = VICAP_CHN_ID_2;
#endif

static void reset_session_slot(int session_idx)
{
    if (session_idx < 0 || session_idx >= MAX_SESSION_NUM)
        return;

    session_info[session_idx].sessionVideoType = kVideoTypeButt;
    session_info[session_idx].sessionVideoLiveSource = nullptr;
    session_info[session_idx].sessionVideoReplicator = nullptr;
    session_info[session_idx].sessionName.clear();
}

StreamingPlayer::StreamingPlayer(const k_vicap_sensor_type &sensor_type,
                                 int video_width, int video_height,
                                 int session_num)
    : video_width_(video_width),
      video_height_(video_height),
      session_num_(session_num),
      sensor_type_(sensor_type) {
    if (session_num_ < 1 || session_num_ > 1) {
        std::cout << "[lawrec-rtsp] invalid session_num=" << session_num_
                  << std::endl;
        init_ret_ = -1;
        return;
    }

    for (int i = 0; i < MAX_SESSION_NUM; ++i)
        reset_session_slot(i);
    audio_session.g711LiveSource = nullptr;
    audio_session.g711Replicator = nullptr;
    audio_enabled_ = lawrec_settings_audio_enabled() != 0;
    for (int i = 0; i < MAX_SESSION_NUM; ++i)
        g_video_cb_count[i].store(0);
    g_audio_cb_count.store(0);

    std::cout << "[lawrec-rtsp] ctor begin" << std::endl;
    std::cout << "[lawrec-rtsp] create scheduler" << std::endl;
    scheduler_ = BasicTaskScheduler::createNew();
    if (scheduler_ == nullptr) {
        std::cout << "[lawrec-rtsp] create scheduler failed" << std::endl;
        init_ret_ = -1;
        return;
    }

    std::cout << "[lawrec-rtsp] create env" << std::endl;
    env_ = BasicUsageEnvironment::createNew(*scheduler_);
    if (env_ == nullptr) {
        std::cout << "[lawrec-rtsp] create env failed" << std::endl;
        delete scheduler_;
        scheduler_ = nullptr;
        init_ret_ = -1;
        return;
    }

    std::cout << "[lawrec-rtsp] create rtsp server begin" << std::endl;
    UserAuthenticationDatabase* authDB = nullptr;
    unsigned reclamationSeconds = 10;
    rtspServer_ = RTSPServer::createNew(*env_, lawrec_settings_port(), authDB, reclamationSeconds);
    if (!rtspServer_) {
        *env_ << "create rtsp server failed." << env_->getResultMsg() << "\n";
        env_->reclaim();
        env_ = nullptr;
        delete scheduler_;
        scheduler_ = nullptr;
        init_ret_ = -1;
        return;
    }
    std::cout << "[lawrec-rtsp] create rtsp server done" << std::endl;

    std::cout << "[lawrec-rtsp] StreamingPlayerInit begin" << std::endl;
    int ret = StreamingPlayerInit();
    if (ret != K_SUCCESS) {
        std::cout << "[lawrec-rtsp] StreamingPlayerInit failed ret=" << ret
                  << std::endl;
        Medium::close(rtspServer_);
        rtspServer_ = nullptr;
        env_->reclaim();
        env_ = nullptr;
        delete scheduler_;
        scheduler_ = nullptr;
        init_ret_ = ret;
        return;
    }

    audio_created_.store(false);
    init_ok_ = true;
    init_ret_ = K_SUCCESS;
    std::cout << "[lawrec-rtsp] ctor ready" << std::endl;
}

static k_s32 sessionVideoCallback(k_u32 chn_num, kd_venc_data_s* p_vstream_data, k_u8 *p_private_data) try {
    if (!p_vstream_data || !p_vstream_data->status.cur_packs ||
        p_vstream_data->status.cur_packs > KD_VENC_MAX_FRAME_PACKCOUNT) return -1;
    if (g_rtsp_stopping.load() || chn_num >= MAX_SESSION_NUM ||
        session_info[chn_num].sessionVideoLiveSource == nullptr) {
        return 0;
    }
    for (unsigned i = 0; i < p_vstream_data->status.cur_packs; ++i)
        if (!p_vstream_data->astPack[i].vir_addr || !p_vstream_data->astPack[i].len) return -1;
    unsigned long cb_count = ++g_video_cb_count[chn_num];
    if (cb_count <= 3 || (cb_count % 120) == 0) {
        printf("[lawrec-rtsp] video cb chn=%u count=%lu packs=%u pts=%llu len0=%u\n",
               chn_num, cb_count, p_vstream_data->status.cur_packs,
               (unsigned long long)p_vstream_data->astPack[0].pts,
               p_vstream_data->astPack[0].len);
    }
    int cut = p_vstream_data->status.cur_packs;
    for (int i = 0; i < cut; i++) {
        k_char *pdata = p_vstream_data->astPack[i].vir_addr;
        uint64_t timestamp = presentation_time(p_vstream_data->astPack[i].pts);
        if (session_info[chn_num].sessionVideoType == kVideoTypeH264) {
            H264LiveFrameSource *h264LiveSource = (H264LiveFrameSource*)session_info[chn_num].sessionVideoLiveSource;
            h264LiveSource->pushData((const uint8_t*)pdata, p_vstream_data->astPack[i].len, timestamp);
        } else if (session_info[chn_num].sessionVideoType == kVideoTypeH265) {
            H265LiveFrameSource *h265LiveSource = (H265LiveFrameSource*)session_info[chn_num].sessionVideoLiveSource;
            h265LiveSource->pushData((const uint8_t*)pdata, p_vstream_data->astPack[i].len, timestamp);
        } else if (session_info[chn_num].sessionVideoType == kVideoTypeMjpeg) {
            MjpegLiveVideoSource *mjpegLiveSource = (MjpegLiveVideoSource*)session_info[chn_num].sessionVideoLiveSource;
            mjpegLiveSource->pushData((const uint8_t*)pdata, p_vstream_data->astPack[i].len, timestamp);
        }
    }
    return 0;
}

catch (...) {
    g_rtsp_stopping.store(true);
    fprintf(stderr, "[rtsp] video callback allocation failure\n");
    return -1;
}

static k_s32 sessionAudioCallback(k_u32 chn_num, k_audio_stream* stream_data, void* p_private_data) try {
    if (g_rtsp_stopping.load() || audio_session.g711LiveSource == nullptr)
        return 0;
    if (!stream_data || !stream_data->stream || !stream_data->len || stream_data->len > 65536) {
        fprintf(stderr, "[lawrec-rtsp] invalid audio callback payload\n");
        return -1;
    }
    unsigned long cb_count = ++g_audio_cb_count;
    if (cb_count <= 3 || (cb_count % 200) == 0) {
        printf("[lawrec-rtsp] audio cb count=%lu len=%u ts=%llu\n",
               cb_count, stream_data->len,
               (unsigned long long)stream_data->time_stamp);
    }
    audio_session.g711LiveSource->pushData((const uint8_t*)stream_data->stream, stream_data->len,
                                         presentation_time(stream_data->time_stamp));

    return 0;
}
catch (...) {
    g_rtsp_stopping.store(true);
    fprintf(stderr, "[lawrec-rtsp] audio callback allocation failure\n");
    return -1;
}

int StreamingPlayer::StreamingPlayerInit() {
    int ret = lawrec_media_acquire(1);
    media_reused_ = true;
    owns_vicap_ = false;
    return ret;
}

int StreamingPlayer::StreamingPlayerDeinit() {
    if (!cleanup_error_) lawrec_media_release(1);
    else std::cerr << "[rtsp] cleanup failed; restart required error=" << cleanup_error_ << std::endl;
    init_ok_ = false;
    return 0;
}

int StreamingPlayer::Start() {
    started_ = true;
    std::cout << "[lawrec-rtsp] Start() enter" << std::endl;
    g_rtsp_stopping.store(false);
    watchVariable_ = 0;
    g_presentation_clock.reset();
    video_error_.store(0);
    int ret = lawrec_encoder_subscribe(1, &video_queue_);
    if (ret) return ret;
    // Owned frames decouple live555 from the SDK callback and MP4 writer.
    video_worker_ = std::thread([this]() {
        try {
            while (!g_rtsp_stopping.load()) {
                LawrecFramePtr frame;
                int result = video_queue_.pop(frame, 20);
                if (result < 0) {
                    if (!g_rtsp_stopping.load()) video_error_.store(result);
                    break;
                }
                if (!result) continue;
                kd_venc_data_s data{};
                data.status.cur_packs = 1;
                data.astPack[0].vir_addr = reinterpret_cast<k_char *>(
                    const_cast<uint8_t *>(frame->bytes.data()));
                data.astPack[0].len = frame->bytes.size();
                data.astPack[0].pts = frame->pts_us;
                if (sessionVideoCallback(0, &data, nullptr)) {
                    video_error_.store(-EIO);
                    break;
                }
            }
        } catch (...) { video_error_.store(-ENOMEM); }
        if (video_error_.load())
            fprintf(stderr, "[lawrec-rtsp] video consumer failed=%d\n", video_error_.load());
    });

    if (audio_created_) {
        audio_error_.store(0);
        int ret = lawrec_audio_subscribe(1, &audio_queue_);
        if (ret) return ret;
        audio_worker_ = std::thread([this]() {
            auto last = std::chrono::steady_clock::now();
            try {
                while (!g_rtsp_stopping.load()) {
                    LawrecFramePtr frame;
                    int result = audio_queue_.pop(frame, 40);
                    if (result < 0) {
                        if (!g_rtsp_stopping.load()) audio_error_.store(result);
                        break;
                    }
                    if (result) {
                        k_audio_stream data{};
                        data.stream = const_cast<uint8_t *>(frame->bytes.data());
                        data.len = frame->bytes.size();
                        data.time_stamp = frame->pts_us;
                        if (sessionAudioCallback(0, &data, nullptr)) {
                            audio_error_.store(-EIO); break;
                        }
                        last = std::chrono::steady_clock::now();
                    } else if (std::chrono::steady_clock::now() - last > std::chrono::seconds(8)) {
                        audio_error_.store(-ETIMEDOUT); break;
                    }
                }
            } catch (...) { audio_error_.store(-ENOMEM); }
            if (audio_error_.load())
                fprintf(stderr, "[lawrec-rtsp] audio consumer failed=%d\n", audio_error_.load());
        });
    }

    if (owns_vicap_) {
        int ret = kd_mapi_vicap_start(VICAP_DEV_ID_0);
        std::cout << "[lawrec-rtsp] kd_mapi_vicap_start ret=" << ret << std::endl;
    } else {
        std::cout << "[lawrec-rtsp] skip vicap start, reuse running big-core preview"
                  << std::endl;
    }

    if (rtspServer_->setUpTunnelingOverHTTP(80) || rtspServer_->setUpTunnelingOverHTTP(8000) || rtspServer_->setUpTunnelingOverHTTP(8080)) {
        *env_ << "\n(We use port " << rtspServer_->httpServerPortNum() << " for optional RTSP-over-HTTP tunneling.)\n";
    } else {
        *env_ << "\n(RTSP-over-HTTP tunneling is not available.)\n";
    }

    wake_event_ = scheduler_->createEventTrigger([](void *opaque) {
        static_cast<StreamingPlayer *>(opaque)->watchVariable_ = 1;
    });
    if (!wake_event_) return -ENOMEM;
    server_loop_ = std::thread([this]() {
        env_->taskScheduler().doEventLoop(&watchVariable_);
    });
    server_loop_started_ = true;
    started_ = true;
    std::cout << "[lawrec-rtsp] Start() leave" << std::endl;
    return 0;
}

void StreamingPlayer::Stop() {
    std::cout << "[lawrec-rtsp] Stop() enter" << std::endl;
    if (!started_) {
        std::cout << "[lawrec-rtsp] Stop() skip, not started" << std::endl;
        return;
    }

    g_rtsp_stopping.store(true);
    audio_queue_.close();
    if (audio_worker_.joinable()) audio_worker_.join();
    if (audio_enabled_) {
        int ret = lawrec_audio_unsubscribe(1);
        if (ret) cleanup_error_ = ret;
    }
    video_queue_.close();
    if (video_worker_.joinable()) video_worker_.join();
    // Also query failed subscriptions: cleanup may have quarantined VENC.
    int encoder_ret = lawrec_encoder_unsubscribe(1);
    if (encoder_ret) cleanup_error_ = encoder_ret;

    if (wake_event_) scheduler_->triggerEvent(wake_event_, this);
    if (server_loop_started_ && server_loop_.joinable()) {
        server_loop_.join();
    }
    server_loop_started_ = false;
    if (wake_event_) scheduler_->deleteEventTrigger(wake_event_);
    wake_event_ = 0;

    if (owns_vicap_) {
        kd_mapi_vicap_stop(VICAP_DEV_ID_0);
    } else {
        std::cout << "[lawrec-rtsp] skip vicap stop, big-core owns device" << std::endl;
    }


    started_ = false;
    std::cout << "[lawrec-rtsp] Stop() leave" << std::endl;
}

int StreamingPlayer::CreateSession(const SessionAttr &session_attr) {
    if (!init_ok_ || session_attr.session_idx != 0 || session_created_[0]) return -EINVAL;
    char const* descriptionString = "Lawrec H.264 video";
    std::string streamName = session_attr.session_name;
    std::cout << "[lawrec-rtsp] CreateSession enter idx="
              << session_attr.session_idx
              << " stream=" << streamName << std::endl;
    ServerMediaSession *sms = ServerMediaSession::createNew(*env_, streamName.c_str(), streamName.c_str(), descriptionString);
    if (!sms) return -1;
    std::unique_ptr<ServerMediaSession, void(*)(ServerMediaSession*)> guard(
        sms, [](ServerMediaSession *p) { Medium::close(p); });
    session_info[session_attr.session_idx].sessionName = streamName;

    if (audio_enabled_ && !audio_created_) {

        audio_session.g711LiveSource = G711LiveFrameSource::createNew(*env_, 8);
        if (!audio_session.g711LiveSource) {
            std::cout << "failed to create G711LiveFrameSource." << std::endl;
            return -1;
        }
        audio_session.g711Replicator = StreamReplicator::createNew(*env_, audio_session.g711LiveSource, false);
        if (!audio_session.g711Replicator) {
            Medium::close(audio_session.g711LiveSource);
            audio_session.g711LiveSource = nullptr;
            return -ENOMEM;
        }

        audio_created_.store(true);
    }

    int ret = CreateVideoEncode(session_attr);
    if (ret < 0) {
        std::cout << "create video encode failed." << std::endl;
        return -1;
    }
    std::cout << "[lawrec-rtsp] video encode created idx=" << session_attr.session_idx << std::endl;

    ret = createSubSession(sms, session_attr);
    if (ret < 0) {
        std::cout << "create sub session failed." << std::endl;
        return -1;
    }

    rtspServer_->addServerMediaSession(sms);
    guard.release();
    announceStream(sms, session_attr.session_name.c_str());
    std::cout << "[lawrec-rtsp] CreateSession leave idx="
              << session_attr.session_idx
              << " stream=" << streamName << std::endl;

    return 0;
}

int StreamingPlayer::CreateVideoEncode(const SessionAttr &session_attr) {
    if (session_attr.session_idx != 0 || session_attr.video_type != kVideoTypeH264 ||
        session_attr.video_width != 1280 || session_attr.video_height != 720)
        return -EINVAL;
    // Session resources are local; VENC is acquired only when Start subscribes.
    session_created_[0] = true;
    return 0;
}

int StreamingPlayer::DestroySession(int session_idx) {
    if (session_idx < 0 || session_idx >= MAX_SESSION_NUM) return -1;
    std::cout << "[lawrec-rtsp] DestroySession idx=" << session_idx << " begin" << std::endl;

    if (rtspServer_ != nullptr && !session_info[session_idx].sessionName.empty()) {
        std::cout << "[lawrec-rtsp] delete server media session name="
                  << session_info[session_idx].sessionName << std::endl;
        rtspServer_->deleteServerMediaSession(session_info[session_idx].sessionName.c_str());
        session_info[session_idx].sessionName.clear();
    }

    session_created_[session_idx] = false;

    if (audio_created_) {
        std::cout << "[lawrec-rtsp] audio teardown begin" << std::endl;
        if (audio_session.g711Replicator) {
            audio_session.g711LiveSource->stopReader();
            Medium::close(audio_session.g711Replicator);
            audio_session.g711Replicator = nullptr;
        }
        audio_session.g711LiveSource = nullptr;
        audio_created_.store(false);
        std::cout << "[lawrec-rtsp] audio teardown done" << std::endl;
    }

    if (session_info[session_idx].sessionVideoReplicator) {
        if (session_info[session_idx].sessionVideoType == kVideoTypeH264 ||
            session_info[session_idx].sessionVideoType == kVideoTypeH265)
            static_cast<LiveFrameSource *>(session_info[session_idx].sessionVideoLiveSource)->stopReader();
        std::cout << "[lawrec-rtsp] close video replicator idx=" << session_idx
                  << " begin" << std::endl;
        if (session_info[session_idx].sessionVideoType ==  kVideoTypeMjpeg)
            Medium::close((JpegStreamReplicator*)session_info[session_idx].sessionVideoReplicator);
        else
            Medium::close((StreamReplicator*)session_info[session_idx].sessionVideoReplicator);
        session_info[session_idx].sessionVideoReplicator = nullptr;
        std::cout << "[lawrec-rtsp] close video replicator idx=" << session_idx
                  << " done" << std::endl;
    }

    session_info[session_idx].sessionVideoLiveSource = nullptr;
    reset_session_slot(session_idx);
    std::cout << "[lawrec-rtsp] DestroySession idx=" << session_idx << " done" << std::endl;

    return 0;
}

int StreamingPlayer::createSubSession(ServerMediaSession *sms, const SessionAttr &session_attr) {
    if (session_attr.video_type == kVideoTypeH264) {
        session_info[session_attr.session_idx].sessionVideoType = kVideoTypeH264;
        session_info[session_attr.session_idx].sessionVideoLiveSource = H264LiveFrameSource::createNew(*env_, 8);
        if (!session_info[session_attr.session_idx].sessionVideoLiveSource) {
            std::cout << "failed to create H264LiveFrameSource." << std::endl;
            return -1;
        }
        session_info[session_attr.session_idx].sessionVideoReplicator = StreamReplicator::createNew(*env_, (H264LiveFrameSource*)session_info[session_attr.session_idx].sessionVideoLiveSource, false);
        LiveServerMediaSession *h264liveSubSession = LiveServerMediaSession::createNew(*env_, (StreamReplicator*)session_info[session_attr.session_idx].sessionVideoReplicator);
        sms->addSubsession(h264liveSubSession);
    } else if (session_attr.video_type == kVideoTypeH265) {
        session_info[session_attr.session_idx].sessionVideoType = kVideoTypeH265;
        session_info[session_attr.session_idx].sessionVideoLiveSource = H265LiveFrameSource::createNew(*env_, 8);
        if (!session_info[session_attr.session_idx].sessionVideoLiveSource) {
            std::cout << "failed to create H265LiveFrameSource." << std::endl;
            return -1;
        }
        session_info[session_attr.session_idx].sessionVideoReplicator = StreamReplicator::createNew(*env_, (H265LiveFrameSource*)session_info[session_attr.session_idx].sessionVideoLiveSource, false);
        LiveServerMediaSession *h265LiveSubSession = LiveServerMediaSession::createNew(*env_, (StreamReplicator*)session_info[session_attr.session_idx].sessionVideoReplicator);
        sms->addSubsession(h265LiveSubSession);
    } else if (session_attr.video_type == kVideoTypeMjpeg) {
        session_info[session_attr.session_idx].sessionVideoType = kVideoTypeMjpeg;
        session_info[session_attr.session_idx].sessionVideoLiveSource = MjpegLiveVideoSource::createNew(*env_, 8);
        if (!session_info[session_attr.session_idx].sessionVideoLiveSource) {
            std::cout << "failed to create MjpegLiveVideoSource." << std::endl;
            return -1;
        }
        session_info[session_attr.session_idx].sessionVideoReplicator = JpegStreamReplicator::createNew(*env_, (MjpegLiveVideoSource*)session_info[session_attr.session_idx].sessionVideoLiveSource, false);
        MjpegMediaSubsession *jpegLiveSubSession = MjpegMediaSubsession::createNew(*env_, (JpegStreamReplicator*)session_info[session_attr.session_idx].sessionVideoReplicator);
        sms->addSubsession(jpegLiveSubSession);
    }

    if (audio_session.g711Replicator) {
        LiveServerMediaSession *g711liveSubSession = LiveServerMediaSession::createNew(*env_, audio_session.g711Replicator);
        sms->addSubsession(g711liveSubSession);
    }

    return 0;
}

void StreamingPlayer::announceStream(ServerMediaSession* sms, char const* streamName) {
    char* url = rtspServer_->rtspURL(sms);
    UsageEnvironment& env = rtspServer_->envir();
    env << "\n\"" << streamName << "\" stream " << "\n";
    env << "Play this stream using the URL \"" << url << "\"\n";
    delete[] url;

}

unsigned long StreamingPlayer::FrameCount() const { return g_video_cb_count[0].load(); }
bool StreamingPlayer::Overflowed() const {
    auto *source = static_cast<LiveFrameSource *>(session_info[0].sessionVideoLiveSource);
    return video_error_.load() != 0 || audio_error_.load() != 0 ||
           (source && source->overflowed()) ||
           (audio_session.g711LiveSource && audio_session.g711LiveSource->overflowed());
}
