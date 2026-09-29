#ifndef _STREAMING_PLAYER_H
#define _STREAMING_PLAYER_H

#include <unistd.h>
#include <signal.h>
#include <iostream>
#include <fstream>
#include <memory>
#include <atomic>
#include <thread>
#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"
#include "LiveServerMediaSession.h"
#include "h264LiveFrameSource.h"
#include "h265LiveFrameSource.h"
#include "g711LiveFrameSource.h"
#include "mjpegLiveFrameSource.h"
#include "mjpegMediaSubSession.h"
#include "mjpegStreamReplicator.h"
#include "mapi_sys_api.h"
#include "mapi_vvi_api.h"
#include "mapi_venc_api.h"
#include "mapi_ai_api.h"
#include "mapi_aenc_api.h"
#include "mapi_vicap_api.h"
#include "k_vicap_comm.h"

enum VideoType {
    kVideoTypeH264,
    kVideoTypeH265,
    kVideoTypeMjpeg,
    kVideoTypeButt
};

enum AudioType {
    kAudioTypeG711a,
    kAudioTypeButt
};

struct SessionAttr {
    uint32_t session_idx;
    std::string session_name;
    VideoType video_type;
    uint32_t video_width;
    uint32_t video_height;
    k_vicap_sensor_type sensor_type;
    k_i2s_in_mono_channel  auido_mono_channel_type;
};

#define MAX_SESSION_NUM 3

struct SessionInfo {
    VideoType sessionVideoType;
    void *sessionVideoLiveSource;
    void *sessionVideoReplicator;
    std::string sessionName;
};

class StreamingPlayer {
public:
    StreamingPlayer(const k_vicap_sensor_type &sensor_type, int video_width,
                    int video_height, int session_num = 1);

    void DeInit() {
        Stop();
        for (int i = 0; i < session_num_; ++i)
            if (venc_initialized_[i]) DestroySession(i);
        if (init_ok_)
            StreamingPlayerDeinit();
        if (rtspServer_ != nullptr) {
            Medium::close(rtspServer_);
            rtspServer_ = nullptr;
        }
        if (env_ != nullptr) {
            env_->reclaim();
            env_ = nullptr;
        }
        if (scheduler_ != nullptr) {
            delete scheduler_;
            scheduler_ = nullptr;
        }
    }

    int CreateSession(const SessionAttr &session_attr);
    int DestroySession(int session_idx);

    int Start();
    unsigned long FrameCount() const;
    bool Overflowed() const;
    int CleanupError() const { return cleanup_error_; }

    void Stop();
    bool Ready() const { return init_ok_; }
    int InitResult() const { return init_ret_; }

private:
    int CreateAudioEncode(const SessionAttr &session_attr);
    int CreateVideoEncode(const SessionAttr &session_attr);
    int StreamingPlayerInit();
    int StreamingPlayerDeinit();
    int createSubSession(ServerMediaSession *sms, const SessionAttr &session_attr);
    void announceStream(ServerMediaSession* sms, char const* streamName);

    int video_width_;
    int video_height_;
    int session_num_{1};
    k_u32 audio_sample_rate_{8000};
    bool audio_enabled_{false};
    bool init_ok_{false};
    int init_ret_{0};
    bool media_reused_{false};
    bool owns_vicap_{true};
    bool server_loop_started_{false};
    bool started_{false};
    int cleanup_error_{0};
    bool venc_started_[MAX_SESSION_NUM]{};
    bool venc_bound_[MAX_SESSION_NUM]{};
    bool venc_initialized_[MAX_SESSION_NUM]{};
    bool callback_registered_[MAX_SESSION_NUM]{};
    k_mapi_media_attr_t media_attr_;
    k_vicap_dev_set_info dev_attr_info_;
    k_vicap_sensor_type sensor_type_;
    std::atomic<bool> audio_created_{false};

    volatile char watchVariable_{0};
    EventTriggerId wake_event_{0};
    std::thread server_loop_;

    TaskScheduler *scheduler_{nullptr};
    UsageEnvironment* env_{nullptr};
    RTSPServer *rtspServer_{nullptr};
};

#endif // _STREAMING_PLAYER_H
