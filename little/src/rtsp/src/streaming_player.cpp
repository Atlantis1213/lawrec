#include <functional>
#include <chrono>
#include <thread>
#include "streaming_player.h"

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
static k_handle ai_handle = 0;
static AudioSessionInfo audio_session;
static std::atomic<unsigned long> g_video_cb_count[MAX_SESSION_NUM];
static std::atomic<unsigned long> g_audio_cb_count{0};
static std::atomic<bool> g_rtsp_stopping{false};
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
    if (session_num_ < 1 || session_num_ > MAX_SESSION_NUM) {
        std::cout << "[lawrec-rtsp] invalid session_num=" << session_num_
                  << std::endl;
        init_ret_ = -1;
        return;
    }

    for (int i = 0; i < MAX_SESSION_NUM; ++i)
        reset_session_slot(i);
    audio_session.g711LiveSource = nullptr;
    audio_session.g711Replicator = nullptr;
    ai_handle = 0;
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
    rtspServer_ = RTSPServer::createNew(*env_, 8554, authDB, reclamationSeconds);
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

static k_s32 sessionVideoCallback(k_u32 chn_num, kd_venc_data_s* p_vstream_data, k_u8 *p_private_data) {
    if (g_rtsp_stopping.load() || chn_num >= MAX_SESSION_NUM ||
        session_info[chn_num].sessionVideoLiveSource == nullptr) {
        return 0;
    }
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
        if (session_info[chn_num].sessionVideoType == kVideoTypeH264) {
            H264LiveFrameSource *h264LiveSource = (H264LiveFrameSource*)session_info[chn_num].sessionVideoLiveSource;
            h264LiveSource->pushData((const uint8_t*)pdata, p_vstream_data->astPack[i].len, p_vstream_data->astPack[i].pts);
        } else if (session_info[chn_num].sessionVideoType == kVideoTypeH265) {
            H265LiveFrameSource *h265LiveSource = (H265LiveFrameSource*)session_info[chn_num].sessionVideoLiveSource;
            h265LiveSource->pushData((const uint8_t*)pdata, p_vstream_data->astPack[i].len, p_vstream_data->astPack[i].pts);
        } else if (session_info[chn_num].sessionVideoType == kVideoTypeMjpeg) {
            MjpegLiveVideoSource *mjpegLiveSource = (MjpegLiveVideoSource*)session_info[chn_num].sessionVideoLiveSource;
            mjpegLiveSource->pushData((const uint8_t*)pdata, p_vstream_data->astPack[i].len, p_vstream_data->astPack[i].pts);
        }
    }
    return 0;
}

static k_s32 sessionAudioCallback(k_u32 chn_num, k_audio_stream* stream_data, void* p_private_data) {
    if (g_rtsp_stopping.load() || audio_session.g711LiveSource == nullptr)
        return 0;
    unsigned long cb_count = ++g_audio_cb_count;
    if (cb_count <= 3 || (cb_count % 200) == 0) {
        printf("[lawrec-rtsp] audio cb count=%lu len=%u ts=%llu\n",
               cb_count, stream_data->len,
               (unsigned long long)stream_data->time_stamp);
    }
    audio_session.g711LiveSource->pushData((const uint8_t*)stream_data->stream, stream_data->len, stream_data->time_stamp);

    return 0;
}

int StreamingPlayer::StreamingPlayerInit() {
    std::cout << "[lawrec-rtsp] StreamingPlayerInit enter" << std::endl;
    media_reused_ = false;
    owns_vicap_ = true;
    server_loop_started_ = false;

    int ret = K_FAILED;
    for (int attempt = 1; attempt <= 20; ++attempt) {
        std::cout << "[lawrec-rtsp] kd_mapi_sys_init attempt=" << attempt
                  << std::endl;
        ret = kd_mapi_sys_init();
        if (ret == K_SUCCESS) {
            break;
        }
        std::cout << "[lawrec-rtsp] kd_mapi_sys_init pending ret=" << ret
                  << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (ret != K_SUCCESS) {
        std::cout << "[lawrec-rtsp] kd_mapi_sys_init error ret=" << ret << std::endl;
        return ret;
    }
    std::cout << "[lawrec-rtsp] kd_mapi_sys_init ok" << std::endl;

    memset(&dev_attr_info_, 0, sizeof(dev_attr_info_));
    if (sensor_type_ <= OV_OV9286_MIPI_1280X720_60FPS_10BIT_LINEAR_IR_SPECKLE || sensor_type_ >= SC_SC035HGS_MIPI_1LANE_RAW10_640X480_120FPS_LINEAR)
        dev_attr_info_.dw_en = K_FALSE;
    else
        dev_attr_info_.dw_en = K_TRUE;

    memset(&media_attr_, 0, sizeof(media_attr_));
    k_u64 pic_size = video_width_ * video_height_ * 2;
    k_u64 stream_size = video_width_ * video_height_ / 2;
    media_attr_.media_config.vb_config.max_pool_cnt = session_num_ * 2 + 2 + 1;
    for (int i = 0; i < session_num_ * 2; i++) {
        if (i % 2 == 0) {
            media_attr_.media_config.vb_config.comm_pool[i].blk_cnt = 10;
            media_attr_.media_config.vb_config.comm_pool[i].blk_size = ((pic_size + 0xfff) & ~0xfff);
            media_attr_.media_config.vb_config.comm_pool[i].mode = VB_REMAP_MODE_NOCACHE;
        } else {
            media_attr_.media_config.vb_config.comm_pool[i].blk_cnt = 30;
            media_attr_.media_config.vb_config.comm_pool[i].blk_size = ((stream_size + 0xfff) & ~0xfff);
            media_attr_.media_config.vb_config.comm_pool[i].mode = VB_REMAP_MODE_NOCACHE;
        }
    }

    // auido
    media_attr_.media_config.vb_config.comm_pool[session_num_ * 2].blk_cnt = 150;
    media_attr_.media_config.vb_config.comm_pool[session_num_ * 2].blk_size = audio_sample_rate_ * 2 * 4 / AUDIO_PERSEC_DIV_NUM;
    media_attr_.media_config.vb_config.comm_pool[session_num_ * 2].mode = VB_REMAP_MODE_CACHED;
    media_attr_.media_config.vb_config.comm_pool[session_num_ * 2 + 1].blk_cnt = 2;
    media_attr_.media_config.vb_config.comm_pool[session_num_ * 2 + 1].blk_size = audio_sample_rate_ * 2 * 4 / AUDIO_PERSEC_DIV_NUM * 2;
    media_attr_.media_config.vb_config.comm_pool[session_num_ * 2 + 1].mode = VB_REMAP_MODE_CACHED;

    if (dev_attr_info_.dw_en) {
        k_vicap_sensor_info sensor_info;
        memset(&sensor_info, 0, sizeof(sensor_info));
        sensor_info.sensor_type = sensor_type_;
        ret = kd_mapi_vicap_get_sensor_info(&sensor_info);
        if (ret != K_SUCCESS) {
            printf("kd_mapi_vicap_get_sensor_info failed, %x.\n", ret);
            return -1;
        }
        printf("sensor type: %u, name: %p, width: %u, height: %u\n",
            sensor_type_, sensor_info.sensor_name, sensor_info.width, sensor_info.height);
        media_attr_.media_config.vb_config.comm_pool[session_num_ * 2 + 2].blk_cnt = 6;
        media_attr_.media_config.vb_config.comm_pool[session_num_ * 2 + 2].blk_size = VI_ALIGN_UP(sensor_info.width * sensor_info.height * 3 / 2, 0x1000);
        media_attr_.media_config.vb_config.comm_pool[session_num_ * 2 + 2].mode = VB_REMAP_MODE_NOCACHE;
    }

    memset(&media_attr_.media_config.vb_supp.supplement_config, 0, sizeof(media_attr_.media_config.vb_supp.supplement_config));
    media_attr_.media_config.vb_supp.supplement_config |= VB_SUPPLEMENT_JPEG_MASK;
    ret = kd_mapi_media_init(&media_attr_);
    if (ret == kLawrecMapiMediaAlreadyInitialized) {
        kd_mapi_media_init_workaround(K_TRUE);
        ret = K_SUCCESS;
        media_reused_ = true;
        owns_vicap_ = false;
        std::cout << "[lawrec-rtsp] kd_mapi_media_init reuse existing vb/media" << std::endl;
    }
    if (ret != K_SUCCESS) {
        std::cout << "kd_mapi_media_init error ret=" << ret << std::endl;
        goto sys_deinit;
    }
    std::cout << "[lawrec-rtsp] kd_mapi_media_init ok" << std::endl;

    if (!media_reused_) {
        dev_attr_info_.pipe_ctrl.data = 0xFFFFFFFF;
        dev_attr_info_.sensor_type = sensor_type_;
        dev_attr_info_.vicap_dev = VICAP_DEV_ID_0;
        ret = kd_mapi_vicap_set_dev_attr(dev_attr_info_);
        if (ret != K_SUCCESS) {
            printf("kd_mapi_vicap_set_dev_attr failed.\n", ret);
            goto media_deinit;
        }
        std::cout << "[lawrec-rtsp] kd_mapi_vicap_set_dev_attr ok" << std::endl;
    } else {
        std::cout << "[lawrec-rtsp] skip vicap dev attr, reuse big-core pipeline"
                  << std::endl;
    }

    return ret;

media_deinit:
    if (!media_reused_) {
        kd_mapi_media_deinit();
    }

sys_deinit:
    kd_mapi_sys_deinit();

    return ret;
}

int StreamingPlayer::StreamingPlayerDeinit() {
    if (!media_reused_) {
        kd_mapi_media_deinit();
    }
    kd_mapi_sys_deinit();

    return 0;
}

void StreamingPlayer::Start() {
    std::cout << "[lawrec-rtsp] Start() enter" << std::endl;
    g_rtsp_stopping.store(false);
    watchVariable_ = 0;
    for (int i = 0; i < session_num_; i++) {
        int ret = kd_mapi_venc_start(i, -1);
        std::cout << "[lawrec-rtsp] kd_mapi_venc_start chn=" << i << " ret=" << ret << std::endl;
        ret = kd_mapi_venc_bind_vi(0, kLawrecRtspVicapChn, i);
        std::cout << "[lawrec-rtsp] kd_mapi_venc_bind_vi vicap_chn="
                  << static_cast<int>(kLawrecRtspVicapChn)
                  << " venc_chn=" << i << " ret=" << ret << std::endl;
    }

    if (audio_created_) {
        int ret = kd_mapi_aenc_start(0);
        std::cout << "[lawrec-rtsp] kd_mapi_aenc_start ret=" << ret << std::endl;
        ret = kd_mapi_aenc_bind_ai(ai_handle, 0);
        std::cout << "[lawrec-rtsp] kd_mapi_aenc_bind_ai ret=" << ret << std::endl;
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

    server_loop_ = std::thread([this]() {
        env_->taskScheduler().doEventLoop(&watchVariable_);
    });
    server_loop_started_ = true;
    started_ = true;
    std::cout << "[lawrec-rtsp] Start() leave" << std::endl;
}

void StreamingPlayer::Stop() {
    std::cout << "[lawrec-rtsp] Stop() enter" << std::endl;
    if (!started_) {
        std::cout << "[lawrec-rtsp] Stop() skip, not started" << std::endl;
        return;
    }

    g_rtsp_stopping.store(true);

    watchVariable_ = 1;
    if (server_loop_started_ && server_loop_.joinable()) {
        server_loop_.join();
    }
    server_loop_started_ = false;

    for (int i = 0; i < session_num_; i++)
        kd_mapi_venc_unbind_vi(0, kLawrecRtspVicapChn, i);

    if (owns_vicap_) {
        kd_mapi_vicap_stop(VICAP_DEV_ID_0);
    } else {
        std::cout << "[lawrec-rtsp] skip vicap stop, big-core owns device" << std::endl;
    }

    for (int i = 0; i < session_num_; i++)
        kd_mapi_venc_stop(i);

    if (audio_created_) {
        kd_mapi_aenc_unbind_ai(ai_handle, 0);
        kd_mapi_ai_stop(ai_handle);
        kd_mapi_aenc_stop(0);
    }
    started_ = false;
    std::cout << "[lawrec-rtsp] Stop() leave" << std::endl;
}

int StreamingPlayer::CreateSession(const SessionAttr &session_attr) {
    char const* descriptionString = "Session streamed by \"testOnDemandRTSPServer\"";
    std::string streamName = session_attr.session_name;
    std::cout << "[lawrec-rtsp] CreateSession enter idx="
              << session_attr.session_idx
              << " stream=" << streamName << std::endl;
    ServerMediaSession *sms = ServerMediaSession::createNew(*env_, streamName.c_str(), streamName.c_str(), descriptionString);
    session_info[session_attr.session_idx].sessionName = streamName;

    if (audio_enabled_ && !audio_created_) {
        int ret = CreateAudioEncode(session_attr);
        if (ret < 0) {
            std::cout << "create audio encode failed." << std::endl;
            return -1;
        }
        std::cout << "[lawrec-rtsp] audio encode created" << std::endl;

        audio_session.g711LiveSource = G711LiveFrameSource::createNew(*env_, 8);
        if (!audio_session.g711LiveSource) {
            std::cout << "failed to create G711LiveFrameSource." << std::endl;
            return -1;
        }
        audio_session.g711Replicator = StreamReplicator::createNew(*env_, audio_session.g711LiveSource, false);

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
    announceStream(sms, session_attr.session_name.c_str());
    std::cout << "[lawrec-rtsp] CreateSession leave idx="
              << session_attr.session_idx
              << " stream=" << streamName << std::endl;

    return 0;
}

int StreamingPlayer::CreateVideoEncode(const SessionAttr &session_attr) {
    k_venc_chn_attr chn_attr;
    memset(&chn_attr, 0, sizeof(chn_attr));
    k_u64 stream_size = session_attr.video_width * session_attr.video_height / 2;
    chn_attr.venc_attr.pic_width = session_attr.video_width;
    chn_attr.venc_attr.pic_height = session_attr.video_height;
    chn_attr.venc_attr.stream_buf_size = ((stream_size + 0xfff) & ~0xfff);
    chn_attr.venc_attr.stream_buf_cnt = 30;
    chn_attr.rc_attr.rc_mode = K_VENC_RC_MODE_CBR;
    chn_attr.rc_attr.cbr.src_frame_rate = 30;
    chn_attr.rc_attr.cbr.dst_frame_rate = 30;
    if (sensor_type_ == OV_OV9286_MIPI_1280X720_30FPS_10BIT_LINEAR_IR ||
        sensor_type_ == OV_OV9286_MIPI_1280X720_30FPS_10BIT_LINEAR_SPECKLE) {
            chn_attr.rc_attr.cbr.src_frame_rate = 15;
            chn_attr.rc_attr.cbr.dst_frame_rate = 15;
    } else if (sensor_type_ == OV_OV9286_MIPI_1280X720_60FPS_10BIT_LINEAR_IR_SPECKLE) {
            chn_attr.rc_attr.cbr.src_frame_rate = 60;
            chn_attr.rc_attr.cbr.dst_frame_rate = 60;
    }
    chn_attr.rc_attr.cbr.bit_rate = 4000;
    if (session_attr.video_type ==  kVideoTypeH264) {
        chn_attr.venc_attr.type = K_PT_H264;
        chn_attr.venc_attr.profile = VENC_PROFILE_H264_HIGH;
    } else if (session_attr.video_type ==  kVideoTypeH265) {
        chn_attr.venc_attr.type = K_PT_H265;
        chn_attr.venc_attr.profile = VENC_PROFILE_H265_MAIN;
    } else if (session_attr.video_type ==  kVideoTypeMjpeg) {
        chn_attr.venc_attr.type = K_PT_JPEG;
        chn_attr.rc_attr.rc_mode = K_VENC_RC_MODE_MJPEG_FIXQP;
        chn_attr.rc_attr.mjpeg_fixqp.src_frame_rate = 30;
        chn_attr.rc_attr.mjpeg_fixqp.dst_frame_rate = 30;
        chn_attr.rc_attr.mjpeg_fixqp.q_factor = 45;
    }

    int ret = kd_mapi_venc_init(session_attr.session_idx, &chn_attr);
    if (ret != K_SUCCESS) {
        std::cout << "kd_mapi_venc_init error." << std::endl;
        return -1;
    }
    std::cout << "[lawrec-rtsp] kd_mapi_venc_init idx=" << session_attr.session_idx << " ok" << std::endl;

    if (session_attr.video_type != kVideoTypeMjpeg) {
        ret = kd_mapi_venc_enable_idr(session_attr.session_idx, K_TRUE);
        if (ret != K_SUCCESS) {
            std::cout << "kd_mapi_venc_enable_idr error." << std::endl;
            return -1;
        }
        std::cout << "[lawrec-rtsp] kd_mapi_venc_enable_idr idx=" << session_attr.session_idx << " ok" << std::endl;
    }

    kd_venc_callback_s venc_callback;
    memset(&venc_callback, 0, sizeof(venc_callback));
    venc_callback.p_private_data = nullptr;
    venc_callback.pfn_data_cb = sessionVideoCallback;
    ret = kd_mapi_venc_registercallback(session_attr.session_idx, &venc_callback);
    std::cout << "[lawrec-rtsp] kd_mapi_venc_registercallback idx=" << session_attr.session_idx << " ret=" << ret << std::endl;

    if (!media_reused_) {
        k_vicap_sensor_info sensor_info;
        memset(&sensor_info, 0, sizeof(sensor_info));
        sensor_info.sensor_type = sensor_type_;
        ret = kd_mapi_vicap_get_sensor_info(&sensor_info);
        if (ret != K_SUCCESS) {
            printf("kd_mapi_vicap_get_sensor_info failed, %x.\n", ret);
            return -1;
        }

        k_vicap_chn_set_info vi_chn_attr_info;
        memset(&vi_chn_attr_info, 0, sizeof(vi_chn_attr_info));

        vi_chn_attr_info.crop_en = K_FALSE;
        vi_chn_attr_info.scale_en = K_FALSE;
        vi_chn_attr_info.chn_en = K_TRUE;
        vi_chn_attr_info.crop_h_start = 0;
        vi_chn_attr_info.crop_v_start = 0;
        vi_chn_attr_info.out_width = VI_ALIGN_UP(session_attr.video_width,16);
        vi_chn_attr_info.out_height = session_attr.video_height;
        vi_chn_attr_info.pixel_format = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
        vi_chn_attr_info.vicap_dev = VICAP_DEV_ID_0;
        vi_chn_attr_info.vicap_chn = kLawrecRtspVicapChn;
        vi_chn_attr_info.alignment = 12;
        vi_chn_attr_info.buffer_num = 6;
        vi_chn_attr_info.buf_size = VI_ALIGN_UP(
            VI_ALIGN_UP(session_attr.video_width,16) * session_attr.video_height * 3 / 2,
            0x1000);
        ret = kd_mapi_vicap_set_chn_attr(vi_chn_attr_info);
        if (ret != K_SUCCESS) {
            printf("vicap chn %d set attr failed, %x.\n", (int)kLawrecRtspVicapChn, ret);
            return -1;
        }
        std::cout << "[lawrec-rtsp] kd_mapi_vicap_set_chn_attr vicap_chn="
                  << static_cast<int>(kLawrecRtspVicapChn) << " ok" << std::endl;
    } else {
        std::cout << "[lawrec-rtsp] skip vicap chn attr, expect big-core chn="
                  << static_cast<int>(kLawrecRtspVicapChn)
                  << " preconfigured" << std::endl;
    }

    return 0;
}

int StreamingPlayer::CreateAudioEncode(const SessionAttr &session_attr) {
    k_aio_dev_attr aio_dev_attr;
    memset(&aio_dev_attr, 0, sizeof(aio_dev_attr));
    aio_dev_attr.audio_type = KD_AUDIO_INPUT_TYPE_I2S;
    aio_dev_attr.kd_audio_attr.i2s_attr.sample_rate = audio_sample_rate_;
    aio_dev_attr.kd_audio_attr.i2s_attr.bit_width = KD_AUDIO_BIT_WIDTH_16;
    aio_dev_attr.kd_audio_attr.i2s_attr.chn_cnt = 2;
    aio_dev_attr.kd_audio_attr.i2s_attr.i2s_mode = K_STANDARD_MODE;
    aio_dev_attr.kd_audio_attr.i2s_attr.snd_mode = KD_AUDIO_SOUND_MODE_MONO;
    aio_dev_attr.kd_audio_attr.i2s_attr.mono_channel =  session_attr.auido_mono_channel_type;
    aio_dev_attr.kd_audio_attr.i2s_attr.frame_num = AUDIO_PERSEC_DIV_NUM;
    aio_dev_attr.kd_audio_attr.i2s_attr.point_num_per_frame = audio_sample_rate_ / aio_dev_attr.kd_audio_attr.i2s_attr.frame_num;
    aio_dev_attr.kd_audio_attr.i2s_attr.i2s_type = K_AIO_I2STYPE_INNERCODEC;
    if (K_SUCCESS != kd_mapi_ai_init(0, 0, &aio_dev_attr, &ai_handle)) {
        std::cout << "kd_mapi_ai_init failed." << std::endl;
        return -1;
    }

    if (K_SUCCESS != kd_mapi_ai_start(ai_handle)) {
        std::cout << "kd_mapi_ai_start failed." << std::endl;
        kd_mapi_ai_deinit(ai_handle);
        return -1;
    }

    k_aenc_chn_attr aenc_chn_attr;
    memset(&aenc_chn_attr, 0, sizeof(aenc_chn_attr));
    aenc_chn_attr.buf_size = AUDIO_PERSEC_DIV_NUM;
    aenc_chn_attr.point_num_per_frame = audio_sample_rate_ / aenc_chn_attr.buf_size;
    aenc_chn_attr.type = K_PT_G711A;

    if (K_SUCCESS != kd_mapi_aenc_init(0, &aenc_chn_attr)) {
        std::cout << "kd_mapi_aenc_init failed." << std::endl;
        kd_mapi_ai_stop(ai_handle);
        kd_mapi_ai_deinit(ai_handle);
        return -1;
    }

    k_aenc_callback_s aenc_cb;
    memset(&aenc_cb, 0, sizeof(aenc_cb));
    aenc_cb.p_private_data = nullptr;
    aenc_cb.pfn_data_cb = sessionAudioCallback;
    kd_mapi_aenc_registercallback(0, &aenc_cb);

    return 0;
}

int StreamingPlayer::DestroySession(int session_idx) {
    std::cout << "[lawrec-rtsp] DestroySession idx=" << session_idx << " begin" << std::endl;

    if (rtspServer_ != nullptr && !session_info[session_idx].sessionName.empty()) {
        std::cout << "[lawrec-rtsp] delete server media session name="
                  << session_info[session_idx].sessionName << std::endl;
        rtspServer_->deleteServerMediaSession(session_info[session_idx].sessionName.c_str());
        session_info[session_idx].sessionName.clear();
    }

    std::cout << "[lawrec-rtsp] kd_mapi_venc_deinit idx=" << session_idx
              << " begin" << std::endl;
    kd_mapi_venc_deinit(session_idx);
    std::cout << "[lawrec-rtsp] kd_mapi_venc_deinit idx=" << session_idx
              << " done" << std::endl;

    if (audio_created_) {
        std::cout << "[lawrec-rtsp] audio teardown begin" << std::endl;
        if (audio_session.g711Replicator) {
            Medium::close(audio_session.g711Replicator);
            audio_session.g711Replicator = nullptr;
        }
        audio_session.g711LiveSource = nullptr;
        if (ai_handle) {
            kd_mapi_ai_deinit(ai_handle);
            ai_handle = 0;
        }
        kd_mapi_aenc_deinit(0);
        audio_created_.store(false);
        std::cout << "[lawrec-rtsp] audio teardown done" << std::endl;
    }

    if (session_info[session_idx].sessionVideoReplicator) {
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
