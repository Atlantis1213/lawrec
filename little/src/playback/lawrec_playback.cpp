#include "lawrec_playback.h"
#include "lawrec_demux.h"
#include "../common/lawrec_media.h"
#include "../common/lawrec_storage.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
extern "C" {
#include "mp4_format.h"
#include "mapi_sys_api.h"
#include "mapi_vdec_api.h"
#include "mapi_adec_api.h"
#include "mapi_ao_api.h"
}

namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned kStreamBytes = 4 * 1024 * 1024;
constexpr unsigned kFrameBytes = 1280 * 720 * 2;
constexpr int kOwner = 3;
std::mutex mutex;
std::thread worker;
std::atomic<bool> stop{false}, paused{false}, active{false};
lawrec_playback_status status{};
std::atomic<bool> poisoned{false};

void state(int value, int error = 0)
{
    std::lock_guard<std::mutex> guard(mutex);
    status.state = value;
    status.error = error;
    fprintf(stderr, "[playback] state=%d error=%d cleanup_error=%d retained=%d frames=%llu\n",
            value, error, status.cleanup_error, poisoned.load(), (unsigned long long)status.frames);
}

void run(int file)
{
    LawrecDemux demux(stop);
    int error = 0, cleanup_error = 0, input_pool = -1, output_pool = -1, mem = -1;
    bool leased = false, initialized = false, started = false, bound = false;
    bool display_requested = false, cleanup_ok = true, completed = false;
    bool audio_present = false, ao_initialized = false, ao_started = false;
    bool adec_initialized = false, adec_started = false, audio_bound = false;
    k_handle ao = 0;
    k_mp4_codec_id_e audio_codec = K_MP4_CODEC_ID_G711A;
    uint64_t physical = 0;
    void *mapping = MAP_FAILED;
    size_t map_size = 0;
    auto check = [&](int ret, const char *operation) {
        if (ret) fprintf(stderr, "[playback] %s error=%d\n", operation, ret);
        return ret > 0 ? -EIO : ret;
    };
    auto cleanup = [&](int ret, const char *operation) {
        const int result = check(ret, operation);
        if (result) {
            if (!cleanup_error) cleanup_error = result;
            cleanup_ok = false;
        }
        return result;
    };
    do {
        k_mp4_file_info_s info{};
        k_mp4_track_info_s tracks[2]{};
        if ((error = check(demux.start(file, info, tracks), "demux start"))) break;
        // The SDK frame API does not identify track IDs. Reject ambiguous files.
        if (!info.track_num || info.track_num > 2) { error = -ENOTSUP; break; }
        bool video_present = false;
        for (unsigned i = 0; i < info.track_num; ++i) {
            const k_mp4_track_info_s &track = tracks[i];
            if (track.track_type == K_MP4_STREAM_VIDEO && !video_present &&
                track.video_info.codec_id == K_MP4_CODEC_ID_H264 &&
                track.video_info.width == 1280 && track.video_info.height == 720) video_present = true;
            else if (track.track_type == K_MP4_STREAM_AUDIO && !audio_present &&
                     (track.audio_info.codec_id == K_MP4_CODEC_ID_G711A ||
                      track.audio_info.codec_id == K_MP4_CODEC_ID_G711U) &&
                     track.audio_info.channels == 1 && track.audio_info.sample_rate == 8000) {
                audio_present = true;
                audio_codec = track.audio_info.codec_id;
            } else { error = -ENOTSUP; break; }
        }
        if (error) break;
        if (!video_present) { error = -ENOTSUP; break; }
        { std::lock_guard<std::mutex> guard(mutex);
          status.duration_ms = info.duration; status.audio_present = audio_present; }
        if (stop) break;
        if ((error = check(lawrec_media_acquire(kOwner), "media acquire"))) break;
        leased = true;
        display_requested = true; // Also restore after an ambiguous IPC timeout.
        if ((error = check(lawrec_playback_display_request(1), "display enter"))) break;
        k_vb_pool_config pool{};
        pool.mode = VB_REMAP_MODE_NOCACHE;
        pool.blk_cnt = 2; pool.blk_size = kStreamBytes;
        input_pool = kd_mapi_vb_create_pool(&pool);
        if (input_pool < 0) { error = input_pool; break; }
        pool.blk_cnt = 6; pool.blk_size = kFrameBytes;
        output_pool = kd_mapi_vb_create_pool(&pool);
        if (output_pool < 0) { error = output_pool; break; }
        k_vdec_chn_attr attr{};
        attr.type = K_PT_H264;
        attr.pic_width = 1280; attr.pic_height = 720;
        attr.frame_buf_cnt = 6; attr.frame_buf_size = kFrameBytes;
        attr.stream_buf_size = kStreamBytes;
        attr.frame_buf_pool_id = output_pool;
        if ((error = check(kd_mapi_vdec_init(0, &attr), "vdec init"))) break;
        initialized = true;
        if ((error = check(kd_mapi_vdec_start(0), "vdec start"))) break;
        started = true;
        if ((error = check(kd_mapi_vdec_bind_vo(0, 0, 0), "vdec bind"))) break;
        bound = true;
        if (audio_present) {
            k_aio_dev_attr audio{};
            audio.audio_type = KD_AUDIO_OUTPUT_TYPE_I2S;
            auto &i2s = audio.kd_audio_attr.i2s_attr;
            i2s.sample_rate = 8000; i2s.bit_width = KD_AUDIO_BIT_WIDTH_16;
            i2s.chn_cnt = 2; i2s.snd_mode = KD_AUDIO_SOUND_MODE_MONO;
            i2s.i2s_mode = K_STANDARD_MODE; i2s.i2s_type = K_AIO_I2STYPE_INNERCODEC;
            i2s.frame_num = 25; i2s.point_num_per_frame = 320;
            if ((error = check(kd_mapi_ao_init(0, 0, &audio, &ao), "ao init"))) break;
            ao_initialized = true;
            if ((error = check(kd_mapi_ao_start(ao), "ao start"))) break;
            ao_started = true;
            k_adec_chn_attr decoder{};
            decoder.type = audio_codec == K_MP4_CODEC_ID_G711A ? K_PT_G711A : K_PT_G711U;
            decoder.buf_size = 25; decoder.point_num_per_frame = 320;
            decoder.mode = K_ADEC_MODE_PACK;
            if ((error = check(kd_mapi_adec_init(0, &decoder), "adec init"))) break;
            adec_initialized = true;
            if ((error = check(kd_mapi_adec_start(0), "adec start"))) break;
            adec_started = true;
            if ((error = check(kd_mapi_adec_bind_ao(ao, 0), "adec bind"))) break;
            audio_bound = true;
        }
        mem = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (mem < 0) { error = -errno; break; }
        uint64_t first_pts = 0, last_pts[2]{};
        bool seen[2]{};
        uint32_t audio_sequence = 0;
        auto audio_until = Clock::now();
        bool first = true, announced = false;
        uint32_t decoded_count = 0;
        auto last_decoded = Clock::now();
        auto origin = Clock::now();
        while (!stop) {
            k_mp4_frame_data_s frame{};
            if ((error = check(demux.next(frame), "demux frame"))) break;
            if (frame.eof) {
                if (first) { error = -EBADMSG; break; }
                if ((error = check(kd_mapi_sys_get_vb_block_from_pool_id(input_pool, &physical, kStreamBytes, nullptr), "EOS block"))) break;
                k_vdec_stream eos{};
                eos.phy_addr = physical; eos.end_of_stream = K_TRUE;
                if ((error = check(kd_mapi_vdec_send_stream(0, &eos, 1000), "send EOS"))) break;
                auto deadline = Clock::now() + std::chrono::seconds(3);
                while (!stop && Clock::now() < deadline) {
                    k_vdec_chn_status final{};
                    if ((error = check(kd_mapi_vdec_query_status(0, &final), "drain status"))) break;
                    if (final.end_of_stream) {
                        completed = final.dec_stream_frames > 0;
                        if (!completed) error = -EBADMSG;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (!stop && !completed && !error) error = -ETIMEDOUT;
                // ADEC has no EOS query API; allow the final submitted sample
                // duration to elapse before stopping AO (not a hardware drain ACK).
                while (!stop && !error && Clock::now() < audio_until)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                break;
            }
            bool is_audio = audio_present && frame.codec_id == audio_codec;
            if (!frame.data || !frame.data_length || frame.data_length > kStreamBytes ||
                (!is_audio && frame.codec_id != K_MP4_CODEC_ID_H264) ||
                (is_audio && frame.data_length > 320)) { error = -EBADMSG; break; }
            if (first) { first_pts = frame.time_stamp; origin = Clock::now(); first = false; }
            unsigned track_index = is_audio ? 1 : 0;
            if (frame.time_stamp < first_pts ||
                // Also bound a second track's first PTS before constructing a
                // chrono deadline; malformed metadata must not overflow it.
                (!seen[track_index] && frame.time_stamp-first_pts > 10000) ||
                (seen[track_index] && (frame.time_stamp < last_pts[track_index] ||
                 frame.time_stamp-last_pts[track_index] > 10000))) { error = -EBADMSG; break; }
            seen[track_index] = true;
            last_pts[track_index] = frame.time_stamp;
            auto due = origin + std::chrono::milliseconds(frame.time_stamp - first_pts);
            while (!stop && (paused || Clock::now() < due)) {
                auto before = Clock::now();
                bool was_paused = paused;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                if (was_paused) { auto delay = Clock::now() - before; origin += delay; due += delay; last_decoded += delay; }
            }
            if (stop) break;
            if ((error = check(kd_mapi_sys_get_vb_block_from_pool_id(input_pool, &physical, kStreamBytes, nullptr), "input block"))) break;
            long page = sysconf(_SC_PAGESIZE);
            if (page <= 0) { error = -EINVAL; break; }
            size_t offset = physical % page;
            map_size = (kStreamBytes + offset + page - 1) / page * page;
            mapping = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, mem, physical - offset);
            if (mapping == MAP_FAILED) { error = -errno; break; }
            memcpy(static_cast<char *>(mapping) + offset, frame.data, frame.data_length);
            k_vdec_stream stream{};
            stream.phy_addr = physical; stream.len = frame.data_length;
            stream.pts = (frame.time_stamp - first_pts) * 1000;
            // SDK currently ignores this timeout. If it stalls, stop_wait reports
            // timeout and resources remain owned; never free a block in flight.
            if (is_audio) {
                k_audio_stream audio{};
                audio.stream = reinterpret_cast<k_u8 *>(static_cast<char *>(mapping) + offset);
                audio.phys_addr = physical; audio.len = frame.data_length;
                audio.time_stamp = stream.pts; audio.seq = audio_sequence++;
                error = check(kd_mapi_adec_send_stream(0, &audio), "send audio");
                audio_until = Clock::now() + std::chrono::microseconds(frame.data_length * 125);
            } else error = check(kd_mapi_vdec_send_stream(0, &stream, 1000), "send stream");
            if (error) break; // Keep the input block until decoder teardown.
            if (munmap(mapping, map_size)) { error = check(-errno, "unmap input"); break; }
            mapping = MAP_FAILED;
            int released = cleanup(kd_mapi_sys_release_vb_block(physical, kStreamBytes), "release input");
            physical = 0;
            if (released && !error) error = released;
            if (error) break;
            k_vdec_chn_status decoded{};
            if ((error = check(kd_mapi_vdec_query_status(0, &decoded), "decode status"))) break;
            if (decoded.dec_err.set_pic_buf_size_err || decoded.dec_err.format_err || decoded.dec_err.stream_unsupport) {
                error = -EBADMSG; break;
            }
            if (decoded.dec_stream_frames != decoded_count) {
                decoded_count = decoded.dec_stream_frames;
                last_decoded = Clock::now();
                if (!announced) { state(LAWREC_PLAY_PLAYING); announced = true; }
            } else if (Clock::now() - last_decoded > std::chrono::seconds(8)) {
                error = -ETIMEDOUT; break;
            }
            { std::lock_guard<std::mutex> guard(mutex);
              status.position_ms = frame.time_stamp - first_pts; if (!is_audio) ++status.frames; }
        }
    } while (false);
    // One worker owns both feed and teardown; UI never frees decoder resources.
    if (audio_bound) cleanup(kd_mapi_adec_unbind_ao(ao, 0), "audio unbind");
    if (adec_started) cleanup(kd_mapi_adec_stop(0), "adec stop");
    if (adec_initialized) cleanup(kd_mapi_adec_deinit(0), "adec deinit");
    if (ao_started) cleanup(kd_mapi_ao_stop(ao), "ao stop");
    if (ao_initialized) cleanup(kd_mapi_ao_deinit(ao), "ao deinit");
    if (bound) cleanup(kd_mapi_vdec_unbind_vo(0, 0, 0), "unbind");
    if (started) cleanup(kd_mapi_vdec_stop(0), "stop");
    if (initialized) cleanup(kd_mapi_vdec_deinit(0), "deinit");
    if (mapping != MAP_FAILED && munmap(mapping, map_size)) cleanup(-errno, "unmap remaining input");
    if (cleanup_ok && physical) cleanup(kd_mapi_sys_release_vb_block(physical, kStreamBytes), "release remaining input");
    if (mem >= 0 && close(mem)) cleanup(-errno, "close memory fd");
    if (display_requested) cleanup(lawrec_playback_display_request(0), "display exit");
    // Retain pools if decoder teardown failed: it may still reference them.
    if (cleanup_ok && output_pool >= 0) cleanup(kd_mapi_vb_destory_pool(output_pool), "destroy output pool");
    if (cleanup_ok && input_pool >= 0) cleanup(kd_mapi_vb_destory_pool(input_pool), "destroy input pool");
    if (leased && cleanup_ok) lawrec_media_release(kOwner);
    int demux_error = check(demux.close(), "demux close");
    if (!error && demux_error) error = demux_error;
    if (close(file)) {
        const int result = check(-errno, "close file");
        if (!error) error = result;
    }
    if (!cleanup_ok) { if (!error) error = cleanup_error; poisoned = true; }
    {
        std::lock_guard<std::mutex> guard(mutex);
        status.cleanup_error = cleanup_error;
    }
    state(error ? LAWREC_PLAY_FAILED : completed ? LAWREC_PLAY_FINISHED : LAWREC_PLAY_IDLE, error);
    active = false;
}
}

extern "C" int lawrec_playback_start(const char *name)
{
    std::lock_guard<std::mutex> guard(mutex);
    if (active || poisoned) return -EBUSY;
    if (worker.joinable()) worker.join();
    int file = lawrec_storage_open_recording(name);
    if (file < 0) return file;
    status = {}; status.state = LAWREC_PLAY_STARTING;
    snprintf(status.filename, sizeof(status.filename), "%s", name);
    stop = false; paused = false; active = true;
    try { worker = std::thread(run, file); }
    catch (...) { close(file); active = false; status.state = LAWREC_PLAY_FAILED; status.error = -EAGAIN; return -EAGAIN; }
    return 0;
}
extern "C" void lawrec_playback_pause(int value)
{
    std::lock_guard<std::mutex> guard(mutex);
    if (status.state != LAWREC_PLAY_PLAYING && status.state != LAWREC_PLAY_PAUSED) return;
    paused = value != 0;
    status.state = value ? LAWREC_PLAY_PAUSED : LAWREC_PLAY_PLAYING;
}
extern "C" void lawrec_playback_stop(void)
{
    stop = true;
    std::lock_guard<std::mutex> guard(mutex);
    if (active) status.state = LAWREC_PLAY_STOPPING;
}
extern "C" int lawrec_playback_stop_wait(unsigned timeout_ms)
{
    lawrec_playback_stop();
    auto until = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (active && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (active) { fprintf(stderr, "[playback] stop timeout; decoder resources retained\n"); return -ETIMEDOUT; }
    std::lock_guard<std::mutex> guard(mutex);
    if (poisoned) {
        fprintf(stderr, "[playback] stop complete; resources retained error=%d cleanup_error=%d\n",
                status.error, status.cleanup_error);
        return status.cleanup_error ? status.cleanup_error : -EIO;
    }
    return 0;
}
extern "C" void lawrec_playback_get_status(lawrec_playback_status *out)
{
    if (!out) return;
    std::lock_guard<std::mutex> guard(mutex); *out = status; out->active = active;
    out->resource_retained = poisoned.load();
}
extern "C" int lawrec_playback_active(void) { return active || poisoned; }
