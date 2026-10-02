#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lawrec_record_entry.h"
#include "lawrec_mp4_io.h"
#include "../../common/lawrec_storage.h"
#include "../../common/lawrec_media.h"
#include "../../common/lawrec_settings.h"
#include "../../common/lawrec_frame_queue.h"
#include "../../common/lawrec_encoder.h"
#include "../../common/lawrec_audio.h"
#include "../../common/lawrec_media_diagnostics.h"

namespace {

#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
static constexpr int kDefaultSensorType = 52;
#else
static constexpr int kDefaultSensorType = 7;
#endif
static constexpr uint64_t kAudioSampleUs = 125; // G711A mono, 8 kHz.
static constexpr unsigned kAudioTailWaitMs = 250;

struct RecordRuntime {
    std::mutex lock;
    std::thread worker;
    lawrec_record_config_t config{};
    lawrec_record_state_e state{LAWREC_RECORD_STATE_IDLE};
    bool stop_requested{false};
    bool worker_active{false};
    std::chrono::steady_clock::time_point first_frame_time;
    std::chrono::steady_clock::time_point worker_started;
    uint64_t elapsed_ms{0};
    uint64_t bytes_written{0};
    int last_error{0};
    std::string last_path;
    std::vector<uint8_t> sps, pps;
    uint64_t startup_pts{0};
    uint64_t last_pts{0};
    uint64_t last_pts_us{0}, frame_period_us{0};
    uint64_t callback_count{0};
    bool got_first_frame{false};
    bool have_sps{false};
    bool have_pps{false};
    unsigned segment_seconds{0}, segment_index{0};
    uint64_t segment_pts{0}, segment_frames{0};
    uint64_t segment_pts_us{0};
    bool segment_idr_pending{false};
    std::chrono::steady_clock::time_point segment_idr_requested_at;
    KD_HANDLE mp4_muxer{nullptr};
    KD_HANDLE video_track{nullptr};
    KD_HANDLE audio_track{nullptr};
    bool audio_enabled{false};
    uint64_t audio_frames{0}, audio_last_pts_us{0};
    uint64_t audio_end_pts_us{0};
    uint64_t audio_source_pts_us{0};
    bool have_audio_source{false};
    bool sys_initialized{false};
    k_mp4_codec_id_e codec_id{K_MP4_CODEC_ID_H264};
};

RecordRuntime g_record;
LawrecFrameQueue g_record_queue(64, 8*1024*1024);
LawrecFrameQueue g_record_audio_queue(64, 128*1024);

struct RecordAudioCursor {
    LawrecFramePtr frame;
    size_t offset{0};
    explicit operator bool() const { return bool(frame); }
    void reset() { frame.reset(); offset = 0; }
    uint64_t pts_us() const { return frame->pts_us + offset*kAudioSampleUs; }
};

void record_set_state_locked(lawrec_record_state_e state, int last_error)
{
    g_record.state = state;
    g_record.last_error = last_error;
}

void record_log_state(const char *tag, int last_error, const char *path = nullptr)
{
    std::cout << "[lawrec-record] " << tag << " last_error=" << last_error;
    if (path != nullptr && path[0] != '\0')
        std::cout << " path=" << path;
    std::cout << std::endl;
}

// Worker-owned muxer inspection, with record.lock held (no I/O or SDK wait).
void record_log_muxer_locked(const char *phase)
{
    if (!g_record.mp4_muxer) return;
    lawrec_mp4_muxer_stats_t s{};
    const int ret = lawrec_mp4_muxer_stats(g_record.mp4_muxer, &s);
    fprintf(stderr, "[lawrec-record] muxer phase=%s segment=%u stats_error=%d tracks=%u samples=%llu capacity=%llu index_bytes=%llu media_bytes=%llu video_frames=%llu audio_packets=%llu\n",
        phase, g_record.segment_index, ret, s.track_count,
        (unsigned long long)s.sample_count, (unsigned long long)s.sample_capacity,
        (unsigned long long)s.sample_index_bytes, (unsigned long long)s.media_bytes,
        (unsigned long long)g_record.segment_frames, (unsigned long long)g_record.audio_frames);
}

void record_log_diagnostics(const char *phase)
{
    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        fprintf(stderr, "[lawrec-record] runtime phase=%s state=%d error=%d stop=%d segments=%u video_frames=%llu written_bytes=%llu elapsed_ms=%llu last_video_pts_us=%llu last_audio_end_us=%llu\n",
            phase, (int)g_record.state, g_record.last_error, g_record.stop_requested,
            g_record.segment_index, (unsigned long long)g_record.callback_count,
            (unsigned long long)g_record.bytes_written, (unsigned long long)g_record.elapsed_ms,
            (unsigned long long)g_record.last_pts_us, (unsigned long long)g_record.audio_end_pts_us);
        record_log_muxer_locked(phase);
    }
    // /proc collection and queue snapshots never run under the state mutex.
    lawrec_log_queue_stats("record", phase, "video", g_record_queue);
    lawrec_log_queue_stats("record", phase, "audio", g_record_audio_queue);
    lawrec_log_process_stats("record", phase);
}

void reset_runtime_fields_locked()
{
    g_record.sps.clear(); g_record.pps.clear();
    g_record.startup_pts = 0;
    g_record.last_pts = 0;
    g_record.last_pts_us = 0;
    // Freeze the same process setting used by the shared encoder, not a draft.
    const unsigned fps = lawrec_settings_frame_rate();
    g_record.frame_period_us = (1000000u + fps - 1) / fps;
    g_record.callback_count = 0;
    g_record.got_first_frame = false;
    g_record.elapsed_ms = g_record.bytes_written = 0;
    g_record.have_sps = g_record.have_pps = false;
    g_record.segment_seconds = lawrec_settings_segment_seconds();
    g_record.segment_index = 0;
    g_record.segment_pts = g_record.segment_frames = 0;
    g_record.segment_pts_us = 0;
    g_record.segment_idr_pending = false;
    g_record.mp4_muxer = nullptr;
    g_record.video_track = nullptr;
    g_record.audio_track = nullptr;
    g_record.audio_enabled = lawrec_settings_audio_enabled() != 0;
    g_record.audio_frames = 0;
    g_record.audio_last_pts_us = 0;
    g_record.audio_end_pts_us = 0;
    g_record.audio_source_pts_us = 0;
    g_record.have_audio_source = false;
    g_record.sys_initialized = false;
    g_record.worker_started = std::chrono::steady_clock::now();
}

/* Called with record.lock held, or before subscribing to the shared encoder.
 * The encoder stays running across segment rotation; only the muxer changes. */
int open_segment_locked()
{
    char path[128];
    int ret = lawrec_storage_reserve(path, sizeof(path));
    if (ret) return ret;
    g_record.last_path = path;
    k_mp4_config_s config{};
    config.config_type = K_MP4_CONFIG_MUXER;
    snprintf(config.muxer_config.file_name, sizeof(config.muxer_config.file_name), "%s", path);
    ret = kd_mp4_create(&g_record.mp4_muxer, &config);
    if (ret) return ret;
    k_mp4_track_info_s track{};
    track.track_type = K_MP4_STREAM_VIDEO;
    track.time_scale = 1000;
    track.video_info.width = g_record.config.video_width;
    track.video_info.height = g_record.config.video_height;
    track.video_info.track_id = 1;
    track.video_info.codec_id = g_record.codec_id;
    ret = kd_mp4_create_track(g_record.mp4_muxer, &g_record.video_track, &track);
    if (ret) return ret;
    g_record.segment_frames = 0;
    g_record.audio_frames = 0;
    g_record.segment_idr_pending = false;
    ++g_record.segment_index;
    record_log_state("segment opened", 0, path);
    return 0;
}

int close_segment_locked(bool publish)
{
    int ret = 0;
    if (g_record.mp4_muxer) {
        record_log_muxer_locked("segment-close");
        int tracks = kd_mp4_destroy_tracks(g_record.mp4_muxer);
        int close = kd_mp4_destroy(g_record.mp4_muxer);
        if (tracks || close) {
            ret = tracks ? (tracks < 0 ? tracks : -EIO) : (close < 0 ? close : -EIO);
            record_log_state("MP4 finalize failed", ret, g_record.last_path.c_str());
        }
        g_record.mp4_muxer = g_record.video_track = g_record.audio_track = nullptr;
    }
    if (!ret && g_record.audio_enabled && g_record.segment_frames && !g_record.audio_frames) ret = -ENODATA;
    if (!ret && publish && g_record.segment_frames) {
        char path[128];
        ret = lawrec_storage_publish(g_record.last_path.c_str(), path, sizeof(path));
        if (!ret) {
            g_record.last_path = path;
            record_log_state("segment published", 0, path);
        }
    }
    return ret;
}

int end_segment_locked(uint64_t boundary_us)
{
    if (!g_record.segment_frames) return 0;
    if (boundary_us <= g_record.last_pts_us || boundary_us < g_record.segment_pts_us)
        return -EINVAL;
    int ret = lawrec_mp4_muxer_end_track(g_record.mp4_muxer, g_record.video_track,
                                       boundary_us - g_record.segment_pts_us);
    if (!ret && g_record.audio_track) {
        if (g_record.audio_end_pts_us <= g_record.segment_pts_us) return -EINVAL;
        ret = lawrec_mp4_muxer_end_track(g_record.mp4_muxer, g_record.audio_track,
                                       g_record.audio_end_pts_us - g_record.segment_pts_us);
    }
    if (ret) record_log_state("track end metadata failed", ret, g_record.last_path.c_str());
    return ret;
}

int record_audio_next(RecordAudioCursor &pending,
                      std::chrono::steady_clock::time_point &progress, unsigned timeout_ms)
{
    int ret = g_record_audio_queue.pop(pending.frame, timeout_ms);
    if (ret != 1) return ret;
    pending.offset = 0;
    progress = std::chrono::steady_clock::now();
    const auto &frame = *pending.frame;
    if (frame.bytes.empty() || frame.bytes.size() > 320 ||
        frame.pts_us > UINT64_MAX - frame.bytes.size()*kAudioSampleUs ||
        (g_record.have_audio_source && frame.pts_us <= g_record.audio_source_pts_us))
        return -EINVAL;
    g_record.audio_source_pts_us = frame.pts_us;
    g_record.have_audio_source = true;
    return 1;
}

// Keep packet ownership and a byte cursor across file rotation. Normal video
// progress writes only complete packets; clip only at startup/IDR/stop edges.
int record_audio_before(uint64_t boundary_us, RecordAudioCursor &pending,
                        std::chrono::steady_clock::time_point &progress,
                        bool finishing = false, bool clip = false)
{
    if (!g_record.audio_enabled) return 0;
    for (;;) {
        int delivery_error = g_record_audio_queue.error();
        if (delivery_error) return delivery_error;
        if (!g_record.segment_frames) return 0;
        if (!pending) {
            int ret = record_audio_next(pending, progress, 0);
            if (ret <= 0) return finishing && ret == -ECANCELED ? 0 : ret;
        }
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (g_record.last_error) return g_record.last_error;
        if (g_record.stop_requested && !finishing) return 0;
        const auto &packet = *pending.frame;
        if (pending.pts_us() < g_record.segment_pts_us) {
            uint64_t delta = g_record.segment_pts_us - packet.pts_us;
            uint64_t skip = delta/kAudioSampleUs + (delta%kAudioSampleUs != 0);
            pending.offset = skip > packet.bytes.size() ? packet.bytes.size() : skip;
        }
        if (pending.offset == packet.bytes.size()) { pending.reset(); continue; }
        if (pending.pts_us() >= boundary_us) return 0;
        size_t end = packet.bytes.size();
        if (packet.pts_us + end*kAudioSampleUs > boundary_us) {
            if (!clip) return 0;
            uint64_t delta = boundary_us - packet.pts_us;
            end = delta/kAudioSampleUs + (delta%kAudioSampleUs != 0);
        }
        if (end > pending.offset) {
            if (g_record.audio_frames && pending.pts_us() <= g_record.audio_last_pts_us) return -EINVAL;
            if (!g_record.audio_track) {
                // Creating audio before the first video write triggers the SDK's
                // hardcoded G711U track path. Add our G711A track afterwards.
                k_mp4_track_info_s track{};
                track.track_type = K_MP4_STREAM_AUDIO;
                track.time_scale = 8000;
                track.audio_info.channels = 1;
                track.audio_info.sample_rate = 8000;
                track.audio_info.bit_per_sample = 16;
                track.audio_info.track_id = 2;
                track.audio_info.codec_id = K_MP4_CODEC_ID_G711A;
                int ret = kd_mp4_create_track(g_record.mp4_muxer, &g_record.audio_track, &track);
                if (ret) return ret;
            }
            k_mp4_frame_data_s frame{};
            frame.codec_id = K_MP4_CODEC_ID_G711A;
            frame.data = const_cast<uint8_t *>(packet.bytes.data() + pending.offset);
            frame.data_length = end - pending.offset;
            frame.time_stamp = pending.pts_us() - g_record.segment_pts_us;
            int ret = kd_mp4_write_frame(g_record.mp4_muxer, g_record.audio_track, &frame);
            if (ret) return ret < 0 ? ret : -EIO;
            g_record.bytes_written += frame.data_length;
            g_record.audio_last_pts_us = pending.pts_us();
            g_record.audio_end_pts_us = packet.pts_us + end*kAudioSampleUs;
            ++g_record.audio_frames;
        }
        pending.offset = end;
        if (end < packet.bytes.size()) return 0;
        pending.reset();
    }
}

/* Wait only for audio covering the last accepted video frame, never for a
 * growing queue to become empty. SDK stop/unregister and queue waits must not
 * hold record.lock. A partially written packet keeps its remainder for the
 * next segment; sample start times choose sides with <125-us quantization. */
int record_audio_tail(uint64_t boundary_us, RecordAudioCursor &pending,
                      std::chrono::steady_clock::time_point &progress,
                      const char *phase = "stop")
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kAudioTailWaitMs);
    for (;;) {
        int ret = record_audio_before(boundary_us, pending, progress, true, true);
        if (ret) return ret;
        {
            std::lock_guard<std::mutex> guard(g_record.lock);
            if (g_record.audio_end_pts_us >= boundary_us ||
                (pending && pending.pts_us() >= boundary_us)) {
                fprintf(stderr, "[lawrec-record] audio boundary phase=%s segment=%u boundary_us=%llu end_us=%llu packets=%llu\n",
                    phase, g_record.segment_index,
                    static_cast<unsigned long long>(boundary_us),
                    static_cast<unsigned long long>(g_record.audio_end_pts_us),
                    static_cast<unsigned long long>(g_record.audio_frames));
                return g_record.audio_end_pts_us >= boundary_us ? 0 : -ENODATA;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            fprintf(stderr, "[lawrec-record] audio deadline phase=%s boundary_us=%llu end_us=%llu error=%d\n",
                phase, static_cast<unsigned long long>(boundary_us),
                static_cast<unsigned long long>(g_record.audio_end_pts_us), -ETIMEDOUT);
            return -ETIMEDOUT;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        ret = record_audio_next(pending, progress, remaining > 20 ? 20 : remaining > 0 ? remaining : 1);
        if (ret < 0) return ret;
    }
}

int record_video_frame_impl(const LawrecFramePtr &encoded, RecordAudioCursor &pending,
                            std::chrono::steady_clock::time_point &audio_progress)
{
    if (!encoded || encoded->bytes.empty() || encoded->bytes.size() > 2*1024*1024)
        return -EINVAL;

    std::unique_lock<std::mutex> guard(g_record.lock);
    if (g_record.mp4_muxer == nullptr || g_record.video_track == nullptr || g_record.stop_requested)
        return 0;

    const auto &frame = encoded->bytes;
    bool key = false, vcl = false;
    bool frame_sps = false, frame_pps = false;
    for (size_t i = 0; i + 3 < frame.size();) {
        size_t prefix = 0;
        if (!frame[i] && !frame[i+1] && frame[i+2] == 1) prefix = 3;
        else if (!frame[i] && !frame[i+1] && !frame[i+2] && frame[i+3] == 1) prefix = 4;
        if (!prefix || i + prefix >= frame.size()) { ++i; continue; }
        size_t end = i + prefix + 1;
        while (end + 3 < frame.size() &&
               !(frame[end] == 0 && frame[end+1] == 0 &&
                 (frame[end+2] == 1 || (frame[end+2] == 0 && frame[end+3] == 1)))) ++end;
        if (end + 3 >= frame.size()) end = frame.size();
        int type = frame[i+prefix] & 31;
        if (type >= 1 && type <= 5) vcl = true;
        if ((type == 7 || type == 8) && end-i > 32*1024) {
            g_record.last_error = -EOVERFLOW; g_record.stop_requested = true;
            return -EOVERFLOW;
        }
        if (type == 7) {
            g_record.have_sps = frame_sps = true;
            g_record.sps.assign(frame.begin()+i, frame.begin()+end);
        }
        if (type == 8) {
            g_record.have_pps = frame_pps = true;
            g_record.pps.assign(frame.begin()+i, frame.begin()+end);
        }
        if (type == 5) key = true;
        i = end;
    }
    if (!vcl) return 0;
    uint64_t pts = encoded->pts_us / 1000;
    if (pts < g_record.startup_pts ||
        (g_record.got_first_frame && encoded->pts_us <= g_record.last_pts_us)) {
        g_record.last_error = -EINVAL;
        g_record.stop_requested = true;
        return -EINVAL;
    }
    if (!g_record.segment_frames && (!key || !g_record.have_sps || !g_record.have_pps)) return 0;
    const bool rotate = g_record.got_first_frame && g_record.segment_frames && key &&
        g_record.segment_seconds && encoded->pts_us-g_record.segment_pts_us >=
        uint64_t(g_record.segment_seconds)*1000000;
    // This validated video frame is now in flight. Complete it even if stop
    // arrives while audio catches up, rather than mux audio beyond an unwritten
    // video boundary. The worker admits no further frame once stop is requested.
    guard.unlock();
    int audio_ret = rotate ? (g_record.audio_enabled ? record_audio_tail(encoded->pts_us, pending, audio_progress, "segment") : 0) :
                            record_audio_before(encoded->pts_us, pending, audio_progress, true);
    guard.lock();
    if (audio_ret) {
        if (!g_record.last_error) g_record.last_error = audio_ret;
        g_record.stop_requested = true;
        g_record.state = LAWREC_RECORD_STATE_STOPPING;
        record_log_state(rotate ? "segment audio boundary failed" : "audio write/queue failed", audio_ret);
        return audio_ret;
    }
    if (rotate) {
        int ret = end_segment_locked(encoded->pts_us);
        if (!ret) ret = close_segment_locked(true);
        if (!ret) ret = open_segment_locked();
        if (ret) {
            g_record.last_error = ret; g_record.stop_requested = true;
            g_record.state = LAWREC_RECORD_STATE_STOPPING;
            record_log_state("segment rotation failed", ret);
            return ret;
        }
    }
    std::vector<uint8_t> headed;
    if (!g_record.segment_frames) {
        const size_t header_size = (frame_sps ? 0 : g_record.sps.size()) +
                                   (frame_pps ? 0 : g_record.pps.size());
        if (frame.size()+header_size > 2*1024*1024) {
            g_record.last_error = -EOVERFLOW; g_record.stop_requested = true;
            return -EOVERFLOW;
        }
        if (header_size) {
            headed.reserve(header_size + frame.size());
            if (!frame_sps) headed.insert(headed.end(), g_record.sps.begin(), g_record.sps.end());
            if (!frame_pps) headed.insert(headed.end(), g_record.pps.begin(), g_record.pps.end());
            headed.insert(headed.end(), frame.begin(), frame.end());
        }
        g_record.segment_pts = pts;
        g_record.segment_pts_us = encoded->pts_us;
        if (!g_record.got_first_frame) g_record.startup_pts = pts;
    }
    const auto &payload = headed.empty() ? frame : headed;
    k_mp4_frame_data_s frame_data{};
    frame_data.codec_id = g_record.codec_id;
    frame_data.data = const_cast<uint8_t *>(payload.data());
    frame_data.data_length = payload.size();
    // SDK mp4_format converts microseconds to milliseconds internally.
    // Keep exact capture PTS here; the millisecond fields above are UI/rotation state.
    if (encoded->pts_us < g_record.segment_pts_us) return -EINVAL;
    frame_data.time_stamp = encoded->pts_us - g_record.segment_pts_us;
    int write_ret = kd_mp4_write_frame(g_record.mp4_muxer, g_record.video_track, &frame_data);
    if (write_ret) {
        g_record.last_error = write_ret < 0 ? write_ret : -EIO;
        g_record.stop_requested = true;
        g_record.state = LAWREC_RECORD_STATE_STOPPING;
        record_log_state("video write failed", g_record.last_error, g_record.last_path.c_str());
        return g_record.last_error;
    }
    if (!g_record.got_first_frame) {
        g_record.got_first_frame = true;
        g_record.first_frame_time = std::chrono::steady_clock::now();
        if (!g_record.stop_requested) g_record.state = LAWREC_RECORD_STATE_RECORDING;
        record_log_state(g_record.stop_requested ? "first IDR written state=stopping" : "first IDR written state=recording", 0);
        fprintf(stderr, "[lawrec-record] first_frame_latency_ms=%lld capture_pts_us=%llu\n",
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                g_record.first_frame_time - g_record.worker_started).count(),
            (unsigned long long)encoded->pts_us);
    }
    g_record.bytes_written += payload.size();
    ++g_record.segment_frames;
    g_record.last_pts = pts;
    g_record.last_pts_us = encoded->pts_us;
    ++g_record.callback_count;
    g_record.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_record.first_frame_time).count();
    return 0;
}

int record_video_frame(const LawrecFramePtr &frame, RecordAudioCursor &pending,
                       std::chrono::steady_clock::time_point &audio_progress)
{
    try { return record_video_frame_impl(frame, pending, audio_progress); }
    catch (...) {
        std::lock_guard<std::mutex> guard(g_record.lock);
        g_record.last_error = -ENOMEM;
        g_record.stop_requested = true;
        g_record.state = LAWREC_RECORD_STATE_STOPPING;
        return -ENOMEM;
    }
}

int init_record_media(const lawrec_record_config_t &config)
{
    (void)config;
    int ret = lawrec_media_acquire(2);
    if (!ret) g_record.sys_initialized = true;
    return ret;
}

void cleanup_record_runtime(RecordAudioCursor *pending_audio = nullptr,
                            std::chrono::steady_clock::time_point *audio_progress = nullptr)
{
    uint64_t video_boundary = 0, audio_boundary = 0;
    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        g_record.stop_requested = true;
        if (g_record.got_first_frame && !g_record.last_error) {
            if (g_record.last_pts_us > UINT64_MAX - g_record.frame_period_us)
                g_record.last_error = -EOVERFLOW;
            else video_boundary = g_record.last_pts_us + g_record.frame_period_us;
        }
        if (pending_audio && audio_progress && g_record.audio_enabled)
            audio_boundary = video_boundary;
    }
    g_record_queue.close();
    int cleanup_error = 0;
    auto check = [&](const char *operation, int ret) {
        if (ret) {
            if (!cleanup_error) cleanup_error = ret;
            fprintf(stderr, "[record] %s failed=%d; restart required\n", operation, ret);
        }
    };

    if (g_record.sys_initialized)
        check("encoder unsubscribe", lawrec_encoder_unsubscribe(2));
    if (audio_boundary && !cleanup_error) {
        int ret = record_audio_tail(audio_boundary, *pending_audio, *audio_progress);
        if (ret) {
            std::lock_guard<std::mutex> guard(g_record.lock);
            if (!g_record.last_error) g_record.last_error = ret;
            record_log_state("audio tail failed", ret);
        }
    }
    if (g_record.sys_initialized && g_record.audio_enabled) {
        check("audio unsubscribe", lawrec_audio_unsubscribe(2, audio_boundary && !cleanup_error));
        // Detachment seals delivery under the callback mutex, before SDK stop.
        // Preserve any already accepted packet until the final bounded drain.
        if (audio_boundary && !cleanup_error) {
            int ret = record_audio_before(audio_boundary, *pending_audio, *audio_progress, true, true);
            if (ret) {
                std::lock_guard<std::mutex> guard(g_record.lock);
                if (!g_record.last_error) g_record.last_error = ret;
            }
        }
    }
    g_record_audio_queue.close();
    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (cleanup_error && !g_record.last_error) g_record.last_error = cleanup_error;
        if (video_boundary && !g_record.last_error) {
            int ret = end_segment_locked(video_boundary);
            if (ret) g_record.last_error = ret;
        }
        int close_ret = close_segment_locked(false);
        if (close_ret && !g_record.last_error) g_record.last_error = close_ret;
    }
    if (g_record.sys_initialized && !cleanup_error)
        lawrec_media_release(2);
    g_record.sys_initialized = false;
}

void record_worker(lawrec_record_config_t config)
{
    int ret;
    auto last_progress = std::chrono::steady_clock::now();
    auto last_storage_check = last_progress;
    auto last_diagnostics = last_progress;
    uint64_t last_count = 0;
    RecordAudioCursor pending_audio;
    auto audio_progress = last_progress;

    signal(SIGPIPE, SIG_IGN);

    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        g_record.config = config;
        g_record.codec_id = K_MP4_CODEC_ID_H264;
        // Storage owns naming and the frozen directory; never dereference caller strings here.
        g_record.last_path.clear();
        g_record.last_error = 0;
        reset_runtime_fields_locked();
    }
    g_record_queue.reset();
    g_record_audio_queue.reset();
    record_log_diagnostics("start");

    ret = init_record_media(config);
    if (ret)
        goto fail;

    ret = open_segment_locked();
    if (ret)
        goto fail;

    ret = lawrec_encoder_subscribe(2, &g_record_queue);
    if (ret)
        goto fail;
    if (g_record.audio_enabled) {
        ret = lawrec_audio_subscribe(2, &g_record_audio_queue);
        if (ret) goto fail;
    }

    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (g_record.stop_requested)
            g_record.state = LAWREC_RECORD_STATE_STOPPING;
    }
    record_log_state("waiting for first IDR", 0, g_record.last_path.c_str());
    last_progress = std::chrono::steady_clock::now();

    while (true) {
        LawrecFramePtr frame;
        int queued = g_record_queue.pop(frame, 20);
        if (queued == 1) {
            int write_ret = record_video_frame(frame, pending_audio, audio_progress);
            if (write_ret) {
                std::lock_guard<std::mutex> guard(g_record.lock);
                g_record.last_error = write_ret;
                g_record.stop_requested = true;
            }
        } else if (queued < 0 && queued != -ECANCELED) {
            std::lock_guard<std::mutex> guard(g_record.lock);
            g_record.last_error = queued;
            g_record.stop_requested = true;
            record_log_state("encoded queue failed", queued);
        }
        bool request_idr = false;
        {
            std::lock_guard<std::mutex> guard(g_record.lock);
            int audio_error = g_record.audio_enabled ? g_record_audio_queue.error() : 0;
            if (audio_error && !g_record.last_error) {
                g_record.last_error = audio_error;
                g_record.stop_requested = true;
                g_record.state = LAWREC_RECORD_STATE_STOPPING;
                record_log_state("audio delivery failed", audio_error);
            }
            if (g_record.stop_requested) break;
            auto now = std::chrono::steady_clock::now();
            if (g_record.callback_count != last_count && g_record.got_first_frame) {
                last_progress = now;
                last_count = g_record.callback_count;
            }
            int storage_ret = 0;
            if (now-last_storage_check >= std::chrono::milliseconds(100)) {
                storage_ret = lawrec_mp4_muxer_flush(g_record.mp4_muxer);
                if (storage_ret) record_log_state("MP4 flush failed", storage_ret, g_record.last_path.c_str());
                else storage_ret = lawrec_storage_check(nullptr);
                last_storage_check = now;
            }
            if (storage_ret || now - last_progress > std::chrono::seconds(8) ||
                (g_record.audio_enabled && now-audio_progress > std::chrono::seconds(8))) {
                g_record.last_error = storage_ret ? storage_ret : -ETIMEDOUT;
                g_record.stop_requested = true;
                g_record.state = LAWREC_RECORD_STATE_STOPPING;
            }
            if (!g_record.stop_requested && g_record.segment_seconds && g_record.segment_frames &&
                g_record.last_pts-g_record.segment_pts >= uint64_t(g_record.segment_seconds)*1000) {
                if (!g_record.segment_idr_pending) {
                    g_record.segment_idr_pending = true;
                    g_record.segment_idr_requested_at = now;
                    request_idr = true;
                } else if (now-g_record.segment_idr_requested_at > std::chrono::seconds(5)) {
                    g_record.last_error = -ETIMEDOUT;
                    g_record.stop_requested = true;
                    g_record.state = LAWREC_RECORD_STATE_STOPPING;
                    record_log_state("segment IDR timeout", -ETIMEDOUT);
                }
            }
        }
        // MAPI may wait on another thread. Never call it with record.lock held.
        if (request_idr) {
            int idr_ret = lawrec_encoder_request_idr();
            record_log_state("segment IDR request", idr_ret);
            if (idr_ret) {
                std::lock_guard<std::mutex> guard(g_record.lock);
                g_record.last_error = idr_ret;
                g_record.stop_requested = true;
                g_record.state = LAWREC_RECORD_STATE_STOPPING;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last_diagnostics >= std::chrono::seconds(10)) {
            record_log_diagnostics("running");
            last_diagnostics = now;
        }
    }

    cleanup_record_runtime(&pending_audio, &audio_progress);
    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        char final_path[128];
        if (!g_record.last_error && g_record.got_first_frame) {
            int ret = lawrec_storage_publish(g_record.last_path.c_str(), final_path, sizeof(final_path));
            if (ret) g_record.last_error = ret;
            else g_record.last_path = final_path;
        } else if (!g_record.last_error) g_record.last_error = -ENODATA;
        g_record.stop_requested = false;
        if (g_record.last_error != 0)
            record_set_state_locked(LAWREC_RECORD_STATE_FAILED, g_record.last_error);
        else
            record_set_state_locked(LAWREC_RECORD_STATE_IDLE, 0);
    }
    record_log_state(g_record.last_error == 0 ? "state=idle" : "state=failed",
                     g_record.last_error, g_record.last_path.c_str());
    record_log_diagnostics("stopped");
    return;

fail:
    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (!g_record.last_error) g_record.last_error = ret < 0 ? ret : -EIO;
    }
    cleanup_record_runtime();
    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        g_record.stop_requested = false;
        record_set_state_locked(LAWREC_RECORD_STATE_FAILED, g_record.last_error);
    }
    record_log_state("state=failed", g_record.last_error, g_record.last_path.c_str());
    record_log_diagnostics("startup-failed");
}

}  // namespace

extern "C" int lawrec_record_start_async(const lawrec_record_config_t *config)
{
    lawrec_record_config_t local_config{};
    std::thread old_worker;

    if (config == nullptr || (config->video_type && strcmp(config->video_type, "h264")) ||
        (config->video_width > 0 && config->video_width != 1280) ||
        (config->video_height > 0 && config->video_height != 720))
        return -EINVAL;

    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (g_record.state == LAWREC_RECORD_STATE_STARTING ||
            g_record.state == LAWREC_RECORD_STATE_RECORDING)
            return 0;
        if (g_record.state == LAWREC_RECORD_STATE_STOPPING)
            return -EBUSY;
        if (g_record.worker_active)
            return -EBUSY;

        if (g_record.worker.joinable())
            old_worker = std::move(g_record.worker);

        local_config = *config;
        if (local_config.sensor_type <= 0)
            local_config.sensor_type = kDefaultSensorType;
        local_config.video_type = "h264";
        if (local_config.video_width <= 0)
            local_config.video_width = 1280;
        if (local_config.video_height <= 0)
            local_config.video_height = 720;
        local_config.output_dir = local_config.file_prefix = nullptr;

        g_record.stop_requested = false;
        record_set_state_locked(LAWREC_RECORD_STATE_STARTING, 0);
        // Reserve worker lifetime before dropping the lock: stop_wait must
        // not report completion in the gap before std::thread is assigned.
        g_record.worker_active = true;
    }

    if (old_worker.joinable())
        old_worker.join();

    record_log_state("state=starting", 0);
    try {
        std::lock_guard<std::mutex> guard(g_record.lock);
        g_record.worker_active = true;
        g_record.worker = std::thread([local_config]() {
            try { record_worker(local_config); }
            catch (...) {
                { std::lock_guard<std::mutex> lock(g_record.lock); g_record.stop_requested = true; }
                cleanup_record_runtime();
                std::lock_guard<std::mutex> lock(g_record.lock);
                record_set_state_locked(LAWREC_RECORD_STATE_FAILED, -ENOMEM);
            }
            std::lock_guard<std::mutex> lock(g_record.lock);
            g_record.worker_active = false;
        });
    } catch (const std::exception &ex) {
        std::lock_guard<std::mutex> guard(g_record.lock);
        g_record.worker_active = false;
        record_set_state_locked(LAWREC_RECORD_STATE_FAILED, -EAGAIN);
        record_log_state("thread create failed", -EAGAIN, ex.what());
        return -EAGAIN;
    } catch (...) {
        std::lock_guard<std::mutex> guard(g_record.lock);
        g_record.worker_active = false;
        record_set_state_locked(LAWREC_RECORD_STATE_FAILED, -EAGAIN);
        record_log_state("thread create failed unknown", -EAGAIN);
        return -EAGAIN;
    }
    return 0;
}

extern "C" int lawrec_record_stop_async(void)
{
    std::lock_guard<std::mutex> guard(g_record.lock);

    if (g_record.state == LAWREC_RECORD_STATE_IDLE)
        return 0;
    if (g_record.state == LAWREC_RECORD_STATE_FAILED) {
        record_set_state_locked(LAWREC_RECORD_STATE_IDLE, 0);
        g_record.stop_requested = false;
        return 0;
    }
    if (g_record.state == LAWREC_RECORD_STATE_STOPPING)
        return 0;

    g_record.stop_requested = true;
    record_set_state_locked(LAWREC_RECORD_STATE_STOPPING, 0);
    record_log_state("state=stopping", 0, g_record.last_path.c_str());
    return 0;
}

extern "C" int lawrec_record_stop_wait(int timeout_ms)
{
    int ret;
    std::thread done_worker;

    ret = lawrec_record_stop_async();
    if (ret != 0)
        return ret;

    if (timeout_ms < 0)
        timeout_ms = 0;

    for (int elapsed = 0; elapsed <= timeout_ms; elapsed += 50) {
        {
            std::lock_guard<std::mutex> guard(g_record.lock);
            if (!g_record.worker_active) {
                if (g_record.worker.joinable())
                    done_worker = std::move(g_record.worker);
                break;
            }
        }

        if (elapsed == timeout_ms)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (done_worker.joinable())
        done_worker.join();

    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (g_record.worker_active) {
            record_log_state("stop wait timeout", -ETIMEDOUT,
                             g_record.last_path.c_str());
            return -ETIMEDOUT;
        }
    }

    return 0;
}

extern "C" int lawrec_record_is_running(void)
{
    std::lock_guard<std::mutex> guard(g_record.lock);
    return g_record.state == LAWREC_RECORD_STATE_RECORDING ? 1 : 0;
}

extern "C" int lawrec_record_get_state(void)
{
    std::lock_guard<std::mutex> guard(g_record.lock);
    return static_cast<int>(g_record.state);
}

extern "C" int lawrec_record_get_last_error(void)
{
    std::lock_guard<std::mutex> guard(g_record.lock);
    return g_record.last_error;
}

extern "C" const char *lawrec_record_get_last_path(void)
{
    std::lock_guard<std::mutex> guard(g_record.lock);
    return g_record.last_path.c_str();
}

extern "C" int lawrec_record_copy_last_path(char *buf, unsigned int buf_size)
{
    std::lock_guard<std::mutex> guard(g_record.lock);

    if (buf == nullptr || buf_size == 0)
        return -EINVAL;
    std::snprintf(buf, buf_size, "%s", g_record.last_path.c_str());
    return 0;
}

extern "C" void lawrec_record_get_progress(uint64_t *elapsed_ms, uint64_t *bytes)
{
    std::lock_guard<std::mutex> guard(g_record.lock);
    if (elapsed_ms) *elapsed_ms = g_record.elapsed_ms;
    if (bytes) *bytes = g_record.bytes_written;
}
