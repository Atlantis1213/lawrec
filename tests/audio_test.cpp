#include <cassert>
#include <cstring>
#include "../little/src/common/lawrec_audio.cpp"

static k_aenc_callback_s callback;
static int inits, stops, fail_bind, fail_stop;
extern "C" {
k_s32 kd_mapi_ai_init(k_u32, k_u32, const k_aio_dev_attr *attr, k_handle *handle) {
    assert(attr->kd_audio_attr.i2s_attr.sample_rate == 8000);
    *handle = 0; // Zero is a valid SDK handle, not a lifecycle flag.
    ++inits;
    return 0;
}
k_s32 kd_mapi_ai_deinit(k_handle) { return 0; }
k_s32 kd_mapi_ai_start(k_handle) { return 0; }
k_s32 kd_mapi_ai_stop(k_handle) { return 0; }
k_s32 kd_mapi_aenc_init(k_handle, const k_aenc_chn_attr *attr) {
    assert(attr->type == K_PT_G711A); return 0;
}
k_s32 kd_mapi_aenc_deinit(k_handle) { return 0; }
k_s32 kd_mapi_aenc_start(k_handle) { return 0; }
k_s32 kd_mapi_aenc_stop(k_handle) {
    ++stops;
    // A stop may wait for one last callback; no callback mutex may be held.
    assert(callback.pfn_data_cb(0, nullptr, nullptr) == 0);
    return fail_stop;
}
k_s32 kd_mapi_aenc_registercallback(k_handle, k_aenc_callback_s *cb) { callback = *cb; return 0; }
k_s32 kd_mapi_aenc_unregistercallback(k_handle) { return 0; }
k_s32 kd_mapi_aenc_bind_ai(k_handle, k_handle) { return fail_bind; }
k_s32 kd_mapi_aenc_unbind_ai(k_handle, k_handle) { return 0; }
}

int main() {
    LawrecFrameQueue rtsp(4, 4096), record(1, 1024);
    assert(lawrec_audio_subscribe(1, &rtsp) == 0);
    assert(lawrec_audio_subscribe(2, &record) == 0);
    assert(inits == 1);
    uint8_t bytes[320]{};
    k_audio_stream stream{};
    stream.stream = bytes;
    stream.len = sizeof(bytes);
    stream.time_stamp = 1234000;
    assert(callback.pfn_data_cb(0, &stream, nullptr) == 0);
    bytes[0] = 99;
    LawrecFramePtr a, b;
    assert(rtsp.pop(a, 0) == 1 && record.pop(b, 0) == 1);
    assert(a == b && a->bytes[0] == 0 && a->pts_us == 1234000);
    callback.pfn_data_cb(0, &stream, nullptr);
    callback.pfn_data_cb(0, &stream, nullptr);
    assert(record.pop(b, 0) == -ENOBUFS);
    assert(rtsp.pop(a, 0) == 1);
    assert(lawrec_audio_unsubscribe(2) == 0 && stops == 0);
    assert(lawrec_audio_unsubscribe(1) == 0 && stops == 1);
    fail_bind = -EIO;
    assert(lawrec_audio_subscribe(2, &record) == -EIO);
    assert(lawrec_audio_unsubscribe(2) == 0);
    fail_bind = 0;
    assert(lawrec_audio_subscribe(2, &record) == 0);
    fail_stop = -EIO;
    assert(lawrec_audio_unsubscribe(2) == -EIO);
    assert(lawrec_audio_subscribe(1, &rtsp) == -EIO);
    puts("audio: shared capture, owned frames, isolated overflow, cleanup and quarantine passed");
}
