#include "source.h"
#include "config.h"
#include <cerrno>
#include <cstdio>
#include "mapi_sys_api.h"
#include "mapi_ai_api.h"
extern "C" void kd_mapi_media_init_workaround(k_bool);

namespace demo {
namespace {
int check(const char *operation, int result) {
    std::fprintf(stderr, "[media-source] %s result=%d hex=0x%08x\n", operation, result, unsigned(result));
    return result > 0 ? -EIO : result;
}
}
int MediaSource::start() {
    int ret = 0;
    if (!client_) {
        ret = check("MAPI client init", kd_mapi_sys_init());
        if (ret) return ret;
        client_ = true;
        // Only sets a client flag. vision already owns VB; never media_init/deinit.
        kd_mapi_media_init_workaround(K_TRUE);
    }
    k_venc_chn_attr video{};
    video.venc_attr.type = K_PT_H264; video.venc_attr.profile = VENC_PROFILE_H264_HIGH;
    video.venc_attr.pic_width = video_width; video.venc_attr.pic_height = video_height;
    video.venc_attr.stream_buf_cnt = stream_buffers; video.venc_attr.stream_buf_size = stream_block_bytes;
    video.rc_attr.rc_mode = K_VENC_RC_MODE_CBR;
    video.rc_attr.cbr.src_frame_rate = video_fps; video.rc_attr.cbr.dst_frame_rate = video_fps;
    video.rc_attr.cbr.bit_rate = video_bitrate_kbps; video.rc_attr.cbr.gop = video_fps;
    // SDK init creates a local FIFO even when its remote init reports failure.
    venc_ = true;
    ret = check("VENC0 init", kd_mapi_venc_init(0, &video));
    if (!ret) ret = check("VENC0 enable IDR", kd_mapi_venc_enable_idr(0, K_TRUE));
    kd_venc_callback_s vcb{video_callback, reinterpret_cast<k_u8 *>(this)};
    if (!ret) { ret = check("VENC0 callback", kd_mapi_venc_registercallback(0, &vcb)); venc_cb_ = !ret; }
    if (!ret) { ret = check("VENC0 start", kd_mapi_venc_start(0, -1)); venc_started_ = !ret; }
    if (!ret) { ret = check("VI2 -> VENC0 bind", kd_mapi_venc_bind_vi(0, encode_channel, 0)); video_bound_ = !ret; }
    k_aio_dev_attr attr{};
    attr.audio_type = KD_AUDIO_INPUT_TYPE_I2S;
    auto &i2s = attr.kd_audio_attr.i2s_attr;
    i2s.sample_rate = audio_rate; i2s.bit_width = KD_AUDIO_BIT_WIDTH_16;
    i2s.chn_cnt = 2; i2s.snd_mode = KD_AUDIO_SOUND_MODE_MONO;
    i2s.mono_channel = KD_I2S_IN_MONO_RIGHT_CHANNEL; i2s.i2s_mode = K_STANDARD_MODE;
    i2s.frame_num = 25; i2s.point_num_per_frame = audio_samples; i2s.i2s_type = K_AIO_I2STYPE_INNERCODEC;
    if (!ret) { ret = check("AI mono right init", kd_mapi_ai_init(0, 0, &attr, &ai_handle_)); ai_ = !ret; }
    k_aenc_chn_attr audio{};
    audio.type = K_PT_G711A; audio.buf_size = 25; audio.point_num_per_frame = audio_samples;
    if (!ret) { aenc_ = true; ret = check("AENC0 init", kd_mapi_aenc_init(0, &audio)); }
    k_aenc_callback_s acb{audio_callback, this};
    if (!ret) { ret = check("AENC0 callback", kd_mapi_aenc_registercallback(0, &acb)); aenc_cb_ = !ret; }
    // SDK starts its local reader before checking the remote START result.
    if (!ret) { aenc_started_ = true; ret = check("AENC0 start", kd_mapi_aenc_start(0)); }
    if (!ret) { ret = check("AI start", kd_mapi_ai_start(ai_handle_)); ai_started_ = !ret; }
    if (!ret) { ret = check("AI -> AENC0 bind", kd_mapi_aenc_bind_ai(ai_handle_, 0)); audio_bound_ = !ret; }
    if (!ret) ret = check("VENC0 initial IDR", kd_mapi_venc_request_idr(0));
    return ret;
}
int MediaSource::cleanup() {
    int error = 0;
    auto release = [&](bool &owned, const char *operation, int result) {
        int ret = check(operation, result);
        if (!ret) owned = false;
        else if (!error) error = ret;
    };
    // No frames_ lock here: SDK stop/unregister can wait for data callbacks.
    if (audio_bound_) release(audio_bound_, "audio unbind", kd_mapi_aenc_unbind_ai(ai_handle_, 0));
    if (ai_started_) release(ai_started_, "AI stop", kd_mapi_ai_stop(ai_handle_));
    if (aenc_started_) release(aenc_started_, "AENC stop", kd_mapi_aenc_stop(0));
    if (!aenc_started_ && aenc_cb_) release(aenc_cb_, "audio unregister", kd_mapi_aenc_unregistercallback(0));
    if (!audio_bound_ && !aenc_started_ && !aenc_cb_ && aenc_) release(aenc_, "AENC deinit", kd_mapi_aenc_deinit(0));
    if (!audio_bound_ && !ai_started_ && ai_) release(ai_, "AI deinit", kd_mapi_ai_deinit(ai_handle_));
    if (video_bound_) release(video_bound_, "video unbind", kd_mapi_venc_unbind_vi(0, encode_channel, 0));
    if (venc_started_) release(venc_started_, "VENC stop", kd_mapi_venc_stop(0));
    kd_venc_callback_s cb{};
    if (!venc_started_ && venc_cb_) release(venc_cb_, "video unregister", kd_mapi_venc_unregistercallback(0, &cb));
    if (!video_bound_ && !venc_started_ && !venc_cb_ && venc_) release(venc_, "VENC deinit", kd_mapi_venc_deinit(0));
    if (error && !cleanup_error_) cleanup_error_ = error;
    { std::lock_guard<std::mutex> guard(frames_); stats_.cleanup_error = cleanup_error_; }
    return cleanup_error_;
}
int MediaSource::attach(Consumer consumer, Feed &feed) {
    unsigned index = unsigned(consumer);
    if (index > 1) return -EINVAL;
    std::lock_guard<std::mutex> operation(operations_);
    if (cleanup_error_) return cleanup_error_;
    if (owners_ & (1U << index)) return -EALREADY;
    {
        std::lock_guard<std::mutex> guard(frames_);
        if (feeds_[1 - index] == &feed) return -EINVAL;
        if (owners_ && stats_.error) return stats_.error;
        if (!owners_) { headers_.reset(); stats_ = {}; video_pts_valid_ = audio_pts_valid_ = false; }
        feed.video.reset(); feed.audio.reset(); feeds_[index] = &feed;
    }
    int ret = owners_ ? check("joining consumer IDR", kd_mapi_venc_request_idr(0)) : start();
    {
        std::lock_guard<std::mutex> guard(frames_);
        if (!ret) ret = stats_.error; // SDK success cannot erase a startup callback failure.
        if (!ret) owners_ |= 1U << index;
        else { feeds_[index] = nullptr; feed.video.fail(ret); feed.audio.fail(ret); }
    }
    if (ret && !owners_) cleanup();
    std::fprintf(stderr, "[media-source] attach consumer=%u owners=%u result=%d\n", index, owners_, ret);
    return ret;
}
int MediaSource::detach(Consumer consumer, bool drain) {
    unsigned index = unsigned(consumer);
    if (index > 1) return -EINVAL;
    std::lock_guard<std::mutex> operation(operations_);
    if (!(owners_ & (1U << index))) return cleanup_error_;
    {
        std::lock_guard<std::mutex> guard(frames_);
        auto *feed = feeds_[index]; feeds_[index] = nullptr;
        if (drain) { feed->video.finish(); feed->audio.finish(); }
        else { feed->video.close(); feed->audio.close(); }
        owners_ &= ~(1U << index);
    }
    int ret = owners_ ? 0 : cleanup();
    std::fprintf(stderr, "[media-source] detach consumer=%u owners=%u result=%d\n", index, owners_, ret);
    return ret;
}
int MediaSource::tick() {
    if (!idr_needed_.exchange(false)) return 0;
    std::lock_guard<std::mutex> operation(operations_);
    if (!owners_ || cleanup_error_) return cleanup_error_;
    int ret = check("congestion IDR", kd_mapi_venc_request_idr(0));
    if (ret) { std::lock_guard<std::mutex> guard(frames_); fail_locked(ret); }
    return ret;
}
int MediaSource::shutdown() {
    int first = detach(Consumer::Rtsp), second = detach(Consumer::Record);
    std::lock_guard<std::mutex> operation(operations_);
    if (first || second || cleanup_error_) return first ? first : second ? second : cleanup_error_;
    if (!client_) return 0;
    int ret = check("MAPI client deinit", kd_mapi_sys_deinit());
    if (!ret) client_ = false;
    return ret;
}
}
