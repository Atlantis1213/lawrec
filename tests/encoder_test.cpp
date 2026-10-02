#include "lawrec_encoder.h"
#include "lawrec_settings.h"
#include <cassert>
#include <cstdio>
extern "C" {
#include "mapi_venc_api.h"
}
static kd_venc_callback_s callback{};
static int init_count, stop_count, deinit_count, bind_count, idr_count;
static int fail_stage;
static int stop_error, deinit_error;
static bool fail_callback_on_start;
static lawrec_media_settings settings{LAWREC_MEDIA_SETTINGS_VERSION, 8554, 4000, 0, 0, 30};
static unsigned settings_reads, applied_fps;
extern "C" void lawrec_settings_media_current(lawrec_media_settings *result) { *result = settings; ++settings_reads; }
extern "C" k_s32 kd_mapi_venc_init(k_u32 n, k_venc_chn_attr *attr) {
    assert(n == 0 && attr->venc_attr.pic_width == 1280 && attr->rc_attr.cbr.bit_rate == 4000);
    assert(attr->venc_attr.pic_height == 720 && attr->rc_attr.cbr.src_frame_rate == 30);
    assert(attr->rc_attr.cbr.dst_frame_rate == settings.video_frame_rate);
    applied_fps = attr->rc_attr.cbr.dst_frame_rate;
    ++init_count; return fail_stage == 1 ? -EIO : fail_stage == 8 ? 17 : 0;
}
extern "C" k_s32 kd_mapi_venc_enable_idr(k_s32, k_bool) { return fail_stage == 2 ? -EIO : 0; }
extern "C" k_s32 kd_mapi_venc_registercallback(k_u32, kd_venc_callback_s *cb) {
    callback = *cb; return fail_stage == 3 ? -EIO : 0;
}
extern "C" k_s32 kd_mapi_venc_start(k_s32, k_s32) {
    if (fail_callback_on_start) assert(callback.pfn_data_cb(0, nullptr, nullptr) == -EINVAL);
    return fail_stage == 4 ? -EIO : 0;
}
extern "C" k_s32 kd_mapi_venc_bind_vi(k_s32 dev, k_s32 vi, k_s32 venc) {
    assert(dev == 0 && vi == 1 && venc == 0); ++bind_count;
    return fail_stage == 5 ? -EIO : 0;
}
extern "C" k_s32 kd_mapi_venc_request_idr(k_s32) { ++idr_count; return fail_stage == 6 ? -EIO : 0; }
extern "C" k_s32 kd_mapi_venc_unbind_vi(k_s32, k_s32, k_s32) { return 0; }
extern "C" k_s32 kd_mapi_venc_stop(k_s32) {
    ++stop_count;
    // Teardown may wait for a last callback. No frame-lock may surround stop.
    if (callback.pfn_data_cb) assert(callback.pfn_data_cb(0, nullptr, nullptr) == 0);
    return stop_error ? stop_error : fail_stage == 7 ? -EIO : 0;
}
extern "C" k_s32 kd_mapi_venc_unregistercallback(k_u32, kd_venc_callback_s *) { callback = {}; return 0; }
extern "C" k_s32 kd_mapi_venc_deinit(k_u32) { ++deinit_count; return deinit_error; }
static void emit(std::vector<uint8_t> bytes, uint64_t pts) {
    kd_venc_data_s data{}; data.status.cur_packs = 1;
    data.astPack[0].vir_addr = reinterpret_cast<char *>(bytes.data());
    data.astPack[0].len = bytes.size(); data.astPack[0].pts = pts;
    assert(callback.pfn_data_cb(0, &data, nullptr) == 0);
    std::fill(bytes.begin(), bytes.end(), 0); // SDK buffer lifetime ends here.
}
const std::vector<uint8_t> headers = {0,0,0,1,0x67,0x42,0,0,0,1,0x68,0xce};
const std::vector<uint8_t> idr = {0,0,0,1,0x65,0x11};
const std::vector<uint8_t> delta = {0,0,0,1,0x41,0x22};
int main() {
    LawrecFrameQueue rtsp(8, 1024), record(1, 1024);
    LawrecFramePtr a, b;
    assert(lawrec_encoder_subscribe(1, &rtsp) == 0);
    assert(lawrec_encoder_subscribe(2, &record) == 0);
    assert(init_count == 1 && bind_count == 1 && idr_count == 2);
    assert(settings_reads == 1 && applied_fps == 30);
    assert(lawrec_encoder_subscribe(1, &rtsp) == -EALREADY);
    emit(delta, 0); assert(rtsp.pop(a, 0) == 0);
    emit(headers, 1); emit(idr, 2);
    assert(rtsp.pop(a, 0) == 1 && record.pop(b, 0) == 1 && a.get() == b.get());
    auto expected = headers; expected.insert(expected.end(), idr.begin(), idr.end());
    assert(a->bytes == expected && a->pts_us == 2);
    emit(delta, 3); assert(rtsp.pop(a, 0) == 1);
    emit(delta, 4); assert(rtsp.pop(a, 0) == 1 && record.pop(b, 0) == -ENOBUFS);
    assert(lawrec_encoder_unsubscribe(2) == 0 && stop_count == 0);
    emit(delta, 5); assert(rtsp.pop(a, 0) == 1);
    assert(lawrec_encoder_unsubscribe(1) == 0 && stop_count == 1 && deinit_count == 1);
    assert(lawrec_encoder_request_idr() == -EIO);
    // Record first, RTSP later. A late join must wait for a new IDR.
    settings.video_frame_rate = 15;
    assert(lawrec_encoder_subscribe(2, &record) == 0);
    assert(applied_fps == 15);
    emit(headers, 10); emit(idr, 11); assert(record.pop(b, 0) == 1);
    unsigned read_count = settings_reads;
    settings.video_frame_rate = 30; // Simulate a changed draft, not an active encoder change.
    assert(lawrec_encoder_subscribe(1, &rtsp) == 0);
    assert(settings_reads == read_count && applied_fps == 15);
    emit(delta, 12); assert(rtsp.pop(a, 0) == 0 && record.pop(b, 0) == 1);
    emit(idr, 13); assert(rtsp.pop(a, 0) == 1 && record.pop(b, 0) == 1);
    int stopped = stop_count;
    assert(lawrec_encoder_unsubscribe(1) == 0 && stop_count == stopped);
    assert(lawrec_encoder_unsubscribe(2) == 0 && stop_count == stopped+1);
    int before = init_count;
    for (unsigned fps : {0u, 25u, 60u}) {
        settings.video_frame_rate = fps;
        assert(lawrec_encoder_subscribe(1, &rtsp) == -EINVAL && init_count == before);
        assert(lawrec_encoder_unsubscribe(1) == 0);
    }
    settings.video_frame_rate = 30;
    for (int stage = 1; stage <= 6; ++stage) {
        fail_stage = stage;
        assert(lawrec_encoder_subscribe(1, &rtsp) == -EIO);
        assert(lawrec_encoder_unsubscribe(1) == 0);
    }
    fail_stage = 8;
    assert(lawrec_encoder_subscribe(1, &rtsp) == -EIO && rtsp.pop(a, 0) == -EIO);
    assert(lawrec_encoder_unsubscribe(1) == 0);
    fail_stage = 0;
    assert(lawrec_encoder_subscribe(1, &rtsp) == 0);
    assert(callback.pfn_data_cb(0, nullptr, nullptr) == -EINVAL && rtsp.pop(a, 0) == -EINVAL);
    assert(lawrec_encoder_subscribe(2, &record) == -EINVAL);
    assert(lawrec_encoder_request_idr() == -EINVAL);
    assert(lawrec_encoder_unsubscribe(1) == 0);
    fail_callback_on_start = true;
    assert(lawrec_encoder_subscribe(1, &rtsp) == -EINVAL);
    assert(lawrec_encoder_unsubscribe(1) == 0);
    fail_callback_on_start = false;
    assert(lawrec_encoder_subscribe(1, &rtsp) == 0);
    stopped = stop_count; fail_stage = 6;
    assert(lawrec_encoder_subscribe(2, &record) == -EIO && stop_count == stopped);
    fail_stage = 0;
    emit(headers, 20); emit(idr, 21); assert(rtsp.pop(a, 0) == 1);
    stop_error = -EPIPE; deinit_error = 17;
    assert(lawrec_encoder_unsubscribe(1) == -EPIPE);
    assert(lawrec_encoder_unsubscribe(1) == -EPIPE);
    assert(lawrec_encoder_subscribe(1, &rtsp) == -EPIPE);
    assert(lawrec_encoder_request_idr() == -EPIPE);
    puts("encoder: FPS, both join orders, isolated overflow, startup callback and first-error quarantine passed");
}
