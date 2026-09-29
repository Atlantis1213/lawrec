#pragma once

#include <atomic>
#include <string>
#include "media.h"
#include "rtsp_server.h"

struct lawrec_rtsp_config_t {
    bool video_valid {true};
    KdMediaVideoType video_type {KdMediaVideoType::kVideoTypeH264};
    k_vicap_sensor_type sensor_type {GC2093_MIPI_CSI2_1920X1080_30FPS_10BIT_LINEAR};
    int venc_width {1280};
    int venc_height {720};
    int bitrate_kbps {2000};
    int port {8554};
    std::string stream_name {"lawrec"};
};

class LawrecRtspSession : public IOnVEncData {
  public:
    LawrecRtspSession();
    ~LawrecRtspSession();

    int Init(const lawrec_rtsp_config_t &config);
    int Start();
    int Stop();
    int DeInit();

    bool IsStarted() const;
    const std::string &StreamName() const;
    std::string Url() const;

    void OnVEncData(k_u32 chn_id, void *data, size_t size,
                    k_venc_pack_type type, uint64_t timestamp) override;

  private:
    SessionAttr BuildSessionAttr() const;
    KdMediaInputConfig BuildMediaConfig() const;

  private:
    lawrec_rtsp_config_t config_;
    KdRtspServer rtsp_server_;
    KdMedia media_;
    std::atomic<bool> started_ {false};
    std::atomic<bool> initialized_ {false};
};
