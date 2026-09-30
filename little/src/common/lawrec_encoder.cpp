#include "lawrec_encoder.h"
#include "lawrec_annexb.h"
#include "lawrec_settings.h"
#include <cstdio>
#include <cstring>
extern "C" {
#include "mapi_venc_api.h"
}

namespace {
constexpr int channel = 0;
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
constexpr int vi_channel = 1;
#else
constexpr int vi_channel = 2;
#endif
// Never hold frames_lock across MAPI: stop/unregister may wait for a callback.
std::mutex operations, frames_lock;
LawrecFrameQueue *subscribers[3]{};
bool waiting_idr[3]{};
unsigned owners = 0;
bool initialized = false, registered = false, started = false, bound = false, poisoned = false;
int stream_error = 0;
std::vector<uint8_t> sps, pps;

int fail_stream(int error) {
    stream_error = error;
    for (int i = 1; i <= 2; ++i) if (subscribers[i]) subscribers[i]->fail(error);
    fprintf(stderr, "[encoder] stream failed=%d\n", error);
    return error;
}

k_s32 encoded(k_u32, kd_venc_data_s *data, k_u8 *) {
    std::lock_guard<std::mutex> guard(frames_lock);
    if (!subscribers[1] && !subscribers[2]) return 0;
    if (stream_error) return stream_error;
    try {
        if (!data || !data->status.cur_packs || data->status.cur_packs > KD_VENC_MAX_FRAME_PACKCOUNT)
            return fail_stream(-EINVAL);
        size_t length = 0;
        for (unsigned i = 0; i < data->status.cur_packs; ++i) {
            const auto &p = data->astPack[i];
            if (!p.vir_addr || !p.len || p.len > 4*1024*1024-length) return fail_stream(-EOVERFLOW);
            length += p.len;
        }
        auto frame = std::make_shared<LawrecEncodedFrame>();
        frame->pts_us = data->astPack[0].pts;
        frame->bytes.reserve(length);
        for (unsigned i = 0; i < data->status.cur_packs; ++i) {
            auto *p = reinterpret_cast<uint8_t *>(data->astPack[i].vir_addr);
            frame->bytes.insert(frame->bytes.end(), p, p+data->astPack[i].len);
        }
        size_t remaining = frame->bytes.size();
        uint8_t *cursor = frame->bytes.data();
        bool key = false, vcl = false;
        while (remaining) {
            size_t before = remaining, size;
            uint8_t *nal = lawrec_annexb_next(cursor, remaining, size);
            if (!nal) return fail_stream(-EBADMSG);
            unsigned type = nal[0] & 31;
            if (type == 7 || type == 8) {
                if (before-remaining > 32*1024) return fail_stream(-EOVERFLOW);
                (type == 7 ? sps : pps).assign(cursor, cursor + before-remaining);
            }
            if (type >= 1 && type <= 5) vcl = true;
            if (type == 5) key = true;
            cursor += before-remaining;
        }
        if (!vcl) return 0;
        // Every IDR is self-contained for subscribers joining mid-stream.
        if (key && !sps.empty() && !pps.empty()) {
            if (length+sps.size()+pps.size() > 4*1024*1024) return fail_stream(-EOVERFLOW);
            frame->bytes.insert(frame->bytes.begin(), pps.begin(), pps.end());
            frame->bytes.insert(frame->bytes.begin(), sps.begin(), sps.end());
        }
        for (int i = 1; i <= 2; ++i) {
            if (!subscribers[i]) continue;
            if (waiting_idr[i]) {
                if (!key || sps.empty() || pps.empty()) continue;
                waiting_idr[i] = false;
            }
            // A slow consumer fails only its own queue, not the other stream.
            int ret = subscribers[i]->push(frame);
            if (ret && ret != -ECANCELED) {
                // Detach delivery, but retain owner until explicit unsubscribe.
                fprintf(stderr, "[encoder] owner=%d delivery failed=%d\n", i, ret);
                subscribers[i] = nullptr;
            }
        }
        return 0;
    } catch (...) { return fail_stream(-ENOMEM); }
}

int cleanup() {
    int error = 0;
    auto check = [&](const char *name, int ret) {
        if (ret) { error = ret; fprintf(stderr, "[encoder] %s failed=%d\n", name, ret); }
    };
    if (bound) check("unbind", kd_mapi_venc_unbind_vi(0, vi_channel, channel));
    if (started) check("stop", kd_mapi_venc_stop(channel));
    kd_venc_callback_s cb{};
    if (registered) check("unregister", kd_mapi_venc_unregistercallback(channel, &cb));
    if (initialized) check("deinit", kd_mapi_venc_deinit(channel));
    bound = started = registered = initialized = false;
    if (error) poisoned = true;
    return error;
}
}

int lawrec_encoder_subscribe(int owner, LawrecFrameQueue *queue) {
    if (owner < 1 || owner > 2 || !queue) return -EINVAL;
    std::lock_guard<std::mutex> operation(operations);
    if (poisoned) return -EIO;
    unsigned bit = 1u << owner;
    if (owners & bit) return -EALREADY;
    {
        std::lock_guard<std::mutex> frames(frames_lock);
        if (owners && stream_error) return stream_error;
        if (!owners) { sps.clear(); pps.clear(); stream_error = 0; }
        queue->reset(); subscribers[owner] = queue; waiting_idr[owner] = true;
    }
    int ret = 0;
    if (!owners) {
        k_venc_chn_attr attr{};
        attr.venc_attr.type = K_PT_H264;
        attr.venc_attr.profile = VENC_PROFILE_H264_HIGH;
        attr.venc_attr.pic_width = 1280; attr.venc_attr.pic_height = 720;
        attr.venc_attr.stream_buf_cnt = 30;
        attr.venc_attr.stream_buf_size = (1280*720*3/4 + 0xfff) & ~0xfff;
        attr.rc_attr.rc_mode = K_VENC_RC_MODE_CBR;
        attr.rc_attr.cbr.src_frame_rate = attr.rc_attr.cbr.dst_frame_rate = 30;
        attr.rc_attr.cbr.bit_rate = lawrec_settings_bitrate();
        ret = kd_mapi_venc_init(channel, &attr);
        if (!ret) { initialized = true; ret = kd_mapi_venc_enable_idr(channel, K_TRUE); }
        kd_venc_callback_s cb{}; cb.pfn_data_cb = encoded;
        if (!ret) { ret = kd_mapi_venc_registercallback(channel, &cb); registered = !ret; }
        if (!ret) { ret = kd_mapi_venc_start(channel, -1); started = !ret; }
        if (!ret) { ret = kd_mapi_venc_bind_vi(0, vi_channel, channel); bound = !ret; }
    }
    if (!ret) ret = kd_mapi_venc_request_idr(channel);
    if (ret) {
        { std::lock_guard<std::mutex> frames(frames_lock); subscribers[owner] = nullptr; }
        queue->fail(ret < 0 ? ret : -EIO);
        if (!owners) cleanup();
        return ret;
    }
    owners |= bit;
    fprintf(stderr, "[encoder] subscribe owner=%d owners=%u venc=%d\n", owner, owners, channel);
    return 0;
}

int lawrec_encoder_unsubscribe(int owner) {
    if (owner < 1 || owner > 2) return -EINVAL;
    std::lock_guard<std::mutex> operation(operations);
    if (!(owners & (1u << owner))) return poisoned ? -EIO : 0;
    {
        std::lock_guard<std::mutex> frames(frames_lock);
        if (subscribers[owner]) subscribers[owner]->close();
        subscribers[owner] = nullptr;
    }
    owners &= ~(1u << owner);
    int ret = owners ? 0 : cleanup();
    fprintf(stderr, "[encoder] unsubscribe owner=%d owners=%u result=%d\n", owner, owners, ret);
    return ret;
}

int lawrec_encoder_request_idr() {
    std::lock_guard<std::mutex> operation(operations);
    if (!owners || poisoned) return -EIO;
    return kd_mapi_venc_request_idr(channel);
}
