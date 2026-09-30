#include "lawrec_audio.h"
#include <cstdio>
extern "C" {
#include "mapi_ai_api.h"
#include "mapi_aenc_api.h"
}

namespace {
std::mutex operations, frames_lock;
LawrecFrameQueue *subscribers[3]{};
unsigned owners;
k_handle ai;
bool ai_ready, ai_started, enc_ready, enc_started, registered, bound, poisoned;

k_s32 encoded(k_u32, k_audio_stream *stream, void *) {
    std::lock_guard<std::mutex> guard(frames_lock);
    if (!subscribers[1] && !subscribers[2]) return 0;
    int error = 0;
    try {
        if (!stream || !stream->stream || !stream->len || stream->len > 65536) {
            error = -EINVAL;
        } else {
            auto frame = std::make_shared<LawrecEncodedFrame>();
            const auto *data = reinterpret_cast<const uint8_t *>(stream->stream);
            frame->bytes.assign(data, data + stream->len);
            frame->pts_us = stream->time_stamp;
            for (int i = 1; i <= 2; ++i) {
                if (!subscribers[i]) continue;
                int ret = subscribers[i]->push(frame);
                if (ret) {
                    fprintf(stderr, "[audio] owner=%d delivery failed=%d\n", i, ret);
                    subscribers[i] = nullptr;
                }
            }
        }
    } catch (...) { error = -ENOMEM; }
    if (error) {
        for (int i = 1; i <= 2; ++i) {
            if (subscribers[i]) subscribers[i]->fail(error);
            subscribers[i] = nullptr;
        }
        fprintf(stderr, "[audio] stream failed=%d\n", error);
    }
    return error;
}

// Never hold frames_lock across SDK stop/unregister: they may join callbacks.
int cleanup() {
    int error = 0;
    auto check = [&](const char *operation, int ret) {
        if (ret) {
            error = ret;
            fprintf(stderr, "[audio] %s failed=%d\n", operation, ret);
        }
    };
    if (bound) check("unbind", kd_mapi_aenc_unbind_ai(ai, 0));
    if (ai_started) check("ai stop", kd_mapi_ai_stop(ai));
    if (enc_started) check("aenc stop", kd_mapi_aenc_stop(0));
    if (registered) check("unregister", kd_mapi_aenc_unregistercallback(0));
    if (enc_ready) check("aenc deinit", kd_mapi_aenc_deinit(0));
    if (ai_ready) check("ai deinit", kd_mapi_ai_deinit(ai));
    bound = ai_started = enc_started = registered = enc_ready = ai_ready = false;
    if (error) poisoned = true;
    return error;
}
}

int lawrec_audio_subscribe(int owner, LawrecFrameQueue *queue) {
    if (owner < 1 || owner > 2 || !queue) return -EINVAL;
    std::lock_guard<std::mutex> operation(operations);
    if (poisoned) return -EIO;
    const unsigned bit = 1u << owner;
    if (owners & bit) return -EALREADY;
    {
        std::lock_guard<std::mutex> frames(frames_lock);
        queue->reset();
        subscribers[owner] = queue;
    }
    int ret = 0;
    if (!owners) {
        k_aio_dev_attr attr{};
        attr.audio_type = KD_AUDIO_INPUT_TYPE_I2S;
        auto &i2s = attr.kd_audio_attr.i2s_attr;
        i2s.sample_rate = 8000;
        i2s.bit_width = KD_AUDIO_BIT_WIDTH_16;
        i2s.chn_cnt = 2;
        i2s.i2s_mode = K_STANDARD_MODE;
        i2s.snd_mode = KD_AUDIO_SOUND_MODE_MONO;
        i2s.mono_channel = KD_I2S_IN_MONO_RIGHT_CHANNEL;
        i2s.frame_num = 25;
        i2s.point_num_per_frame = 320;
        i2s.i2s_type = K_AIO_I2STYPE_INNERCODEC;
        ret = kd_mapi_ai_init(0, 0, &attr, &ai);
        ai_ready = !ret;
        k_aenc_chn_attr enc{};
        enc.buf_size = 25;
        enc.point_num_per_frame = 320;
        enc.type = K_PT_G711A;
        if (!ret) { ret = kd_mapi_aenc_init(0, &enc); enc_ready = !ret; }
        k_aenc_callback_s cb{};
        cb.pfn_data_cb = encoded;
        if (!ret) { ret = kd_mapi_aenc_registercallback(0, &cb); registered = !ret; }
        if (!ret) { ret = kd_mapi_aenc_start(0); enc_started = !ret; }
        if (!ret) { ret = kd_mapi_ai_start(ai); ai_started = !ret; }
        if (!ret) { ret = kd_mapi_aenc_bind_ai(ai, 0); bound = !ret; }
    }
    if (ret) {
        { std::lock_guard<std::mutex> frames(frames_lock); subscribers[owner] = nullptr; }
        queue->fail(ret < 0 ? ret : -EIO);
        if (!owners) cleanup();
        fprintf(stderr, "[audio] subscribe owner=%d failed=%d\n", owner, ret);
        return ret;
    }
    owners |= bit;
    fprintf(stderr, "[audio] subscribe owner=%d mask=%u G711A/8000/mono\n", owner, owners);
    return 0;
}

int lawrec_audio_unsubscribe(int owner) {
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
    fprintf(stderr, "[audio] unsubscribe owner=%d mask=%u result=%d\n", owner, owners, ret);
    return ret;
}
