// Narrow SDK callback/lifecycle fixture; no camera/audio hardware emulation.
#include "source.h"
#include "config.h"
#include "mapi_ai_api.h"
#include "mapi_sys_api.h"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <thread>

namespace {
kd_venc_callback_s vcb;
k_aenc_callback_s acb;
int client_inits, client_deinits, video_inits, audio_inits, video_stops, audio_stops, video_deinits, idrs;
int video_init_error, audio_start_error, video_stop_error;
bool startup_callback_error;
thread_local bool in_callback;
std::vector<uint8_t> header{0,0,0,1,0x67,0x42,0,0x1e,0xaa,0,0,1,0x68,0xce};
std::vector<uint8_t> idr{0,0,1,0x65,0x88}, inter{0,0,1,0x41,0xaa};
int emit(std::vector<uint8_t> &bytes, uint64_t pts, k_venc_pack_type type) {
    kd_venc_data_s data{};
    data.u32_pack_cnt = data.status.cur_packs = 1;
    data.astPack[0].vir_addr = reinterpret_cast<char *>(bytes.data());
    data.astPack[0].len = bytes.size(); data.astPack[0].pts = pts; data.astPack[0].type = type;
    in_callback = true;
    int ret = vcb.pfn_data_cb(0, &data, vcb.p_private_data);
    in_callback = false; return ret;
}
}
extern "C" {
k_s32 kd_mapi_sys_init() { ++client_inits; return 0; }
k_s32 kd_mapi_sys_deinit() { ++client_deinits; return 0; }
void kd_mapi_media_init_workaround(k_bool flag) { assert(flag == K_TRUE); }
k_s32 kd_mapi_venc_init(k_u32 channel, k_venc_chn_attr *attr) {
    assert(channel == 0 && attr->venc_attr.type == K_PT_H264);
    assert(attr->venc_attr.pic_width == 1280 && attr->venc_attr.pic_height == 720);
    assert(attr->venc_attr.stream_buf_size == demo::stream_block_bytes && attr->venc_attr.stream_buf_cnt == demo::stream_buffers);
    assert(attr->rc_attr.cbr.src_frame_rate == 30 && attr->rc_attr.cbr.dst_frame_rate == 30);
    assert(attr->rc_attr.cbr.bit_rate == 4000 && attr->rc_attr.cbr.gop == 30);
    ++video_inits; return video_init_error;
}
k_s32 kd_mapi_venc_deinit(k_u32) { ++video_deinits; return 0; }
k_s32 kd_mapi_venc_enable_idr(k_s32, k_bool flag) { assert(flag == K_TRUE); return 0; }
k_s32 kd_mapi_venc_registercallback(k_u32, kd_venc_callback_s *cb) { vcb = *cb; return 0; }
k_s32 kd_mapi_venc_unregistercallback(k_u32, kd_venc_callback_s *) { return 0; }
k_s32 kd_mapi_venc_start(k_s32, k_s32 count) { assert(count == -1); return 0; }
k_s32 kd_mapi_venc_stop(k_s32) {
    // Proves source detach releases its callback mutex before joining an SDK reader.
    std::thread callback([] { assert(vcb.pfn_data_cb(0, nullptr, vcb.p_private_data) == 0); });
    callback.join(); ++video_stops; return video_stop_error;
}
k_s32 kd_mapi_venc_bind_vi(k_s32 device, k_s32 input, k_s32 channel) {
    assert(device == 0 && input == 2 && channel == 0); return 0;
}
k_s32 kd_mapi_venc_unbind_vi(k_s32 device, k_s32 input, k_s32 channel) {
    assert(device == 0 && input == 2 && channel == 0); return 0;
}
k_s32 kd_mapi_venc_request_idr(k_s32) { assert(!in_callback); ++idrs; return 0; }
k_s32 kd_mapi_ai_init(k_u32, k_u32, const k_aio_dev_attr *attr, k_handle *handle) {
    const auto &i2s = attr->kd_audio_attr.i2s_attr;
    assert(i2s.sample_rate == 8000 && i2s.bit_width == KD_AUDIO_BIT_WIDTH_16);
    assert(i2s.chn_cnt == 2 && i2s.snd_mode == KD_AUDIO_SOUND_MODE_MONO);
    assert(i2s.mono_channel == KD_I2S_IN_MONO_RIGHT_CHANNEL);
    assert(i2s.point_num_per_frame == 320 && i2s.frame_num == 25 && i2s.i2s_type == K_AIO_I2STYPE_INNERCODEC);
    *handle = 7; return 0;
}
k_s32 kd_mapi_ai_deinit(k_handle handle) { assert(handle == 7); return 0; }
k_s32 kd_mapi_ai_start(k_handle) { return 0; }
k_s32 kd_mapi_ai_stop(k_handle) { return 0; }
k_s32 kd_mapi_aenc_init(k_handle, const k_aenc_chn_attr *attr) {
    assert(attr->type == K_PT_G711A && attr->point_num_per_frame == 320 && attr->buf_size == 25);
    ++audio_inits; return 0;
}
k_s32 kd_mapi_aenc_deinit(k_handle) { return 0; }
k_s32 kd_mapi_aenc_registercallback(k_handle, k_aenc_callback_s *cb) { acb = *cb; return 0; }
k_s32 kd_mapi_aenc_unregistercallback(k_handle) { return 0; }
k_s32 kd_mapi_aenc_start(k_handle) {
    if (startup_callback_error) assert(acb.pfn_data_cb(0, nullptr, acb.p_private_data) == -EPROTO);
    return audio_start_error;
}
k_s32 kd_mapi_aenc_stop(k_handle) { ++audio_stops; return 0; }
k_s32 kd_mapi_aenc_bind_ai(k_handle handle, k_handle) { assert(handle == 7); return 0; }
k_s32 kd_mapi_aenc_unbind_ai(k_handle handle, k_handle) { assert(handle == 7); return 0; }
}
int main() {
    demo::MediaSource source;
    demo::Feed live(true), record(false);
    assert(source.attach(demo::Consumer::Rtsp, live) == 0);
    assert(source.attach(demo::Consumer::Record, live) == -EINVAL);
    assert(source.attach(demo::Consumer::Record, record) == 0);
    assert(source.attach(demo::Consumer::Record, record) == -EALREADY);
    assert(client_inits == 1 && video_inits == 1 && audio_inits == 1 && idrs == 2);
    assert(emit(header, 0, K_VENC_HEADER) == 0 && !source.stats().video_frames);
    assert(emit(inter, 990000, K_VENC_P_FRAME) == 0 && !live.video.stats().depth && !record.video.stats().depth);
    assert(emit(idr, 1000000, K_VENC_I_FRAME) == 0);
    demo::FramePtr first, second;
    assert(live.video.pop(first, 0) == 1 && record.video.pop(second, 0) == 1 && first == second);
    assert(first->pts_us == 1000000 && first->key);
    idr[4] = 0x33; assert(first->bytes.back() == 0x88); idr[4] = 0x88;
    std::vector<uint8_t> audio(320, 0xd5);
    k_audio_stream packet{}; packet.stream = audio.data(); packet.len = 320; packet.time_stamp = 1000000;
    assert(acb.pfn_data_cb(0, &packet, acb.p_private_data) == 0);
    assert(live.audio.pop(first, 0) == 1 && record.audio.pop(second, 0) == 1 && first == second);
    audio[0] = 0; assert(first->bytes[0] == 0xd5 && first->pts_us == 1000000);
    uint64_t pts = 1000000;
    for (unsigned i = 0; i < 91; ++i) {
        pts += 33333; assert(emit(inter, pts, K_VENC_P_FRAME) == 0);
        assert(record.video.pop(first, 0) == 1);
    }
    assert(live.video.stats().depth == 0 && !record.video.stats().error);
    int requests = idrs;
    assert(source.tick() == 0 && idrs == requests + 1); // No SDK calls inside callback.
    pts += 33333; assert(emit(idr, pts, K_VENC_I_FRAME) == 0);
    assert(live.video.pop(first, 0) == 1 && first->key && record.video.pop(second, 0) == 1);
    assert(source.detach(demo::Consumer::Rtsp) == 0 && !video_stops && !audio_stops);
    pts += 33333; assert(emit(inter, pts, K_VENC_P_FRAME) == 0);
    assert(source.detach(demo::Consumer::Record, true) == 0 && video_stops == 1 && audio_stops == 1);
    assert(record.video.pop(first, 0) == 1 && record.video.pop(first, 0) == -ECANCELED);
    assert(source.shutdown() == 0 && client_deinits == 1);

    demo::MediaSource malformed;
    assert(malformed.attach(demo::Consumer::Rtsp, live) == 0);
    kd_venc_data_s bad{}; bad.status.cur_packs = KD_VENC_MAX_FRAME_PACKCOUNT + 1;
    assert(vcb.pfn_data_cb(0, &bad, vcb.p_private_data) == -EPROTO);
    assert(malformed.attach(demo::Consumer::Record, record) == -EPROTO);
    assert(live.audio.stats().error == -EPROTO && malformed.shutdown() == 0);

    demo::MediaSource startup;
    startup_callback_error = true;
    assert(startup.attach(demo::Consumer::Record, record) == -EPROTO);
    assert(record.video.stats().error == -EPROTO && startup.shutdown() == 0);
    startup_callback_error = false;

    demo::MediaSource partial;
    audio_start_error = -7;
    int stops = audio_stops;
    assert(partial.attach(demo::Consumer::Record, record) == -7 && audio_stops == stops + 1);
    assert(partial.shutdown() == 0); audio_start_error = 0;
    demo::MediaSource init_failure;
    video_init_error = 6;
    int deinits = video_deinits;
    assert(init_failure.attach(demo::Consumer::Rtsp, live) == -EIO && video_deinits == deinits + 1);
    assert(init_failure.shutdown() == 0); video_init_error = 0;

    demo::MediaSource retained;
    assert(retained.attach(demo::Consumer::Rtsp, live) == 0);
    video_stop_error = -55;
    deinits = video_deinits;
    assert(retained.detach(demo::Consumer::Rtsp) == -55 && video_deinits == deinits);
    assert(retained.stats().cleanup_error == -55 && retained.attach(demo::Consumer::Record, record) == -55);
    assert(retained.shutdown() == -55);
    // This fixture has no persistent SDK threads. Real service exits without
    // destroying the callback owner when cleanup remains unconfirmed.
    std::puts("source MOCK ONLY: one VENC/AENC, CHN2 YUV feed, copies/PTS, independent consumers, partial-init cleanup and retained stop failures passed");
}
