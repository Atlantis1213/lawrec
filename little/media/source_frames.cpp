#include "source.h"
#include "config.h"
#include <cerrno>
#include <cstdio>

namespace demo {
int MediaSource::fail_locked(int error) {
    if (!stats_.error) stats_.error = error < 0 ? error : -EIO;
    for (auto *feed : feeds_) if (feed) { feed->video.fail(stats_.error); feed->audio.fail(stats_.error); }
    std::fprintf(stderr, "[media-source] callback error=%d\n", stats_.error);
    return stats_.error;
}
k_s32 MediaSource::video_callback(k_u32 channel, kd_venc_data_s *data, k_u8 *context) {
    if (!context || channel != 0) return -EINVAL;
    return reinterpret_cast<MediaSource *>(context)->video(data);
}
k_s32 MediaSource::audio_callback(k_u32 channel, k_audio_stream *data, void *context) {
    if (!context || channel != 0) return -EINVAL;
    return static_cast<MediaSource *>(context)->audio(data);
}
int MediaSource::video(kd_venc_data_s *data) {
    std::lock_guard<std::mutex> guard(frames_);
    if (!feeds_[0] && !feeds_[1]) return 0;
    if (stats_.error) return stats_.error;
    try {
        if (!data || !data->status.cur_packs || data->status.cur_packs > KD_VENC_MAX_FRAME_PACKCOUNT ||
            data->u32_pack_cnt != data->status.cur_packs) return fail_locked(-EPROTO);
        size_t bytes = 0;
        uint64_t pts = 0; bool have_pts = false;
        for (unsigned i = 0; i < data->status.cur_packs; ++i) {
            const auto &pack = data->astPack[i];
            if (!pack.vir_addr || !pack.len || pack.len > max_access_unit - bytes) return fail_locked(-EMSGSIZE);
            if (pack.type != K_VENC_HEADER) {
                if ((pack.type != K_VENC_I_FRAME && pack.type != K_VENC_P_FRAME) ||
                    (have_pts && pts != pack.pts)) return fail_locked(-EPROTO);
                pts = pack.pts; have_pts = true;
            }
            bytes += pack.len;
        }
        auto frame = std::make_shared<Frame>();
        frame->bytes.reserve(bytes);
        for (unsigned i = 0; i < data->status.cur_packs; ++i) {
            const auto &pack = data->astPack[i];
            const auto *data = reinterpret_cast<const uint8_t *>(pack.vir_addr);
            frame->bytes.insert(frame->bytes.end(), data, data + pack.len);
        }
        int ret = headers_.prepare(*frame);
        if (ret < 0) return fail_locked(ret);
        if (!ret) return 0;
        if (!have_pts || (video_pts_valid_ && pts <= stats_.video_pts)) return fail_locked(-ERANGE);
        frame->pts_us = pts; stats_.video_pts = pts; video_pts_valid_ = true;
        ++stats_.video_frames; stats_.video_bytes += bytes;
        for (auto *feed : feeds_) if (feed) {
            int pushed = feed->video.push(frame);
            if (pushed == 1) { feed->audio.discard(); idr_needed_ = true; }
            else if (pushed < 0 && pushed != -ECANCELED) feed->audio.fail(pushed);
        }
        return 0;
    } catch (...) { return fail_locked(-ENOMEM); }
}
int MediaSource::audio(k_audio_stream *data) {
    std::lock_guard<std::mutex> guard(frames_);
    if (!feeds_[0] && !feeds_[1]) return 0;
    if (stats_.error) return stats_.error;
    try {
        if (!data || !data->stream || data->len != audio_samples) return fail_locked(-EPROTO);
        if (audio_pts_valid_ && data->time_stamp <= stats_.audio_pts) return fail_locked(-ERANGE);
        auto frame = std::make_shared<Frame>();
        const auto *bytes = reinterpret_cast<const uint8_t *>(data->stream);
        frame->bytes.assign(bytes, bytes + data->len); frame->pts_us = data->time_stamp;
        stats_.audio_pts = frame->pts_us; audio_pts_valid_ = true;
        ++stats_.audio_packets; stats_.audio_bytes += frame->bytes.size();
        for (auto *feed : feeds_) if (feed) {
            int ret = feed->audio.push(frame);
            if (ret < 0 && ret != -ECANCELED) feed->video.fail(ret);
        }
        return 0;
    } catch (...) { return fail_locked(-ENOMEM); }
}
SourceStats MediaSource::stats() const {
    std::lock_guard<std::mutex> guard(frames_); return stats_;
}
bool MediaSource::h264_config(std::vector<uint8_t> &sps, std::vector<uint8_t> &pps) const {
    std::lock_guard<std::mutex> guard(frames_); return headers_.config(sps, pps);
}
}
