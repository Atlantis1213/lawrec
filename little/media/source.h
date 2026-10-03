#pragma once
#include "frame_queue.h"
#include <atomic>
#include <mutex>
#include "mapi_venc_api.h"
#include "mapi_aenc_api.h"

namespace demo {
enum class Consumer : unsigned { Rtsp = 0, Record = 1 };
struct SourceStats {
    uint64_t generation = 0;
    uint64_t video_frames = 0, video_bytes = 0, audio_packets = 0, audio_bytes = 0;
    uint64_t video_pts = 0, audio_pts = 0;
    int error = 0, cleanup_error = 0;
};
// Exactly one process-lifetime owner. Shutdown must precede destruction; do not
// destruct this object if SDK stop failed and a callback may still reference it.
class MediaSource {
public:
    int attach(Consumer consumer, Feed &feed);
    int detach(Consumer consumer, bool drain = false);
    int tick(); // Performs requested IDR outside callbacks.
    int shutdown();
    SourceStats stats() const;
    bool h264_config(std::vector<uint8_t> &sps, std::vector<uint8_t> &pps) const;
    void request_idr() { idr_needed_ = 1; }
private:
    int start();
    int cleanup();
    int fail_locked(int error);
    static k_s32 video_callback(k_u32 channel, kd_venc_data_s *data, k_u8 *context);
    static k_s32 audio_callback(k_u32 channel, k_audio_stream *data, void *context);
    int video(kd_venc_data_s *data);
    int audio(k_audio_stream *data);
    std::mutex operations_;
    mutable std::mutex frames_;
    Feed *feeds_[2]{};
    unsigned owners_ = 0;
    H264Headers headers_;
    SourceStats stats_;
    std::atomic<unsigned> idr_needed_{0};
    bool video_pts_valid_ = false, audio_pts_valid_ = false;
    bool client_ = false, venc_ = false, venc_cb_ = false, venc_started_ = false, video_bound_ = false;
    bool ai_ = false, ai_started_ = false, aenc_ = false, aenc_cb_ = false, aenc_started_ = false, audio_bound_ = false;
    k_handle ai_handle_ = 0;
    int cleanup_error_ = 0;
};
}
