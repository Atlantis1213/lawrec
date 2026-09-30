#include "lawrec_encoder.h"
#include <cassert>
#include <cstdio>
extern "C" {
#include "mapi_venc_api.h"
}
static kd_venc_callback_s callback{};
static int init_count, stop_count, deinit_count, bind_count, idr_count;
static int fail_stage;
extern "C" int lawrec_settings_bitrate() { return 4000; }
extern "C" k_s32 kd_mapi_venc_init(k_u32 n, k_venc_chn_attr *attr) {
    assert(n == 0 && attr->venc_attr.pic_width == 1280 && attr->rc_attr.cbr.bit_rate == 4000);
    ++init_count; return fail_stage == 1 ? -EIO : 0;
}
extern "C" k_s32 kd_mapi_venc_enable_idr(k_s32, k_bool) { return fail_stage == 2 ? -EIO : 0; }
extern "C" k_s32 kd_mapi_venc_registercallback(k_u32, kd_venc_callback_s *cb) {
    callback = *cb; return fail_stage == 3 ? -EIO : 0;
}
extern "C" k_s32 kd_mapi_venc_start(k_s32, k_s32) { return fail_stage == 4 ? -EIO : 0; }
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
    return fail_stage == 7 ? -EIO : 0;
}
extern "C" k_s32 kd_mapi_venc_unregistercallback(k_u32, kd_venc_callback_s *) { callback = {}; return 0; }
extern "C" k_s32 kd_mapi_venc_deinit(k_u32) { ++deinit_count; return 0; }
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
    assert(lawrec_encoder_subscribe(2, &record) == 0);
    emit(headers, 10); emit(idr, 11); assert(record.pop(b, 0) == 1);
    assert(lawrec_encoder_subscribe(1, &rtsp) == 0);
    emit(delta, 12); assert(rtsp.pop(a, 0) == 0 && record.pop(b, 0) == 1);
    emit(idr, 13); assert(rtsp.pop(a, 0) == 1 && record.pop(b, 0) == 1);
    int stopped = stop_count;
    assert(lawrec_encoder_unsubscribe(1) == 0 && stop_count == stopped);
    assert(lawrec_encoder_unsubscribe(2) == 0 && stop_count == stopped+1);
    for (int stage = 1; stage <= 6; ++stage) {
        fail_stage = stage;
        assert(lawrec_encoder_subscribe(1, &rtsp) == -EIO);
        assert(lawrec_encoder_unsubscribe(1) == 0);
    }
    fail_stage = 0;
    assert(lawrec_encoder_subscribe(1, &rtsp) == 0);
    stopped = stop_count; fail_stage = 6;
    assert(lawrec_encoder_subscribe(2, &record) == -EIO && stop_count == stopped);
    fail_stage = 0;
    emit(headers, 20); emit(idr, 21); assert(rtsp.pop(a, 0) == 1);
    fail_stage = 7;
    assert(lawrec_encoder_unsubscribe(1) == -EIO);
    fail_stage = 0;
    assert(lawrec_encoder_subscribe(1, &rtsp) == -EIO);
    puts("encoder: shared VENC, both join orders, isolated overflow, teardown, failure quarantine passed");
}
