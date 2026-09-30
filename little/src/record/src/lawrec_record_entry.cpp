#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "lawrec_record_entry.h"
#include "../../common/lawrec_storage.h"
#include "../../common/lawrec_media.h"
#include "../../common/lawrec_settings.h"
#include "../../common/lawrec_frame_queue.h"
#include "../../common/lawrec_encoder.h"
#include "../../common/lawrec_audio.h"

extern "C" {
#include "k_vicap_comm.h"
#include "mapi_sys_api.h"
#include "mapi_venc_api.h"
#include "mp4_format.h"
void kd_mapi_media_init_workaround(k_bool media_init_flag);
}

namespace {

static constexpr int kLawrecMapiMediaAlreadyInitialized =
    static_cast<int>(0xB0008012u);
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
static constexpr k_vicap_chn kLawrecRecordVicapChn = VICAP_CHN_ID_1;
static constexpr int kDefaultSensorType = 52;
#else
static constexpr k_vicap_chn kLawrecRecordVicapChn = VICAP_CHN_ID_2;
static constexpr int kDefaultSensorType = 7;
#endif
static constexpr int kRecordVencChn = 1;
static constexpr char kDefaultOutputDir[] = "/tmp/lawrec_records";
static constexpr char kDefaultPrefix[] = "lawrec";

struct RecordRuntime {
    std::mutex lock;
    std::thread worker;
    lawrec_record_config_t config{};
    lawrec_record_state_e state{LAWREC_RECORD_STATE_IDLE};
    bool stop_requested{false};
    bool worker_active{false};
    std::chrono::steady_clock::time_point first_frame_time;
    uint64_t elapsed_ms{0};
    uint64_t bytes_written{0};
    int last_error{0};
    std::string last_path;
    std::vector<uint8_t> sps, pps;
    uint64_t startup_pts{0};
    uint64_t last_pts{0};
    int callback_count{0};
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
    bool media_reused{false};
    bool sys_initialized{false};
    bool media_initialized{false};
    k_mp4_codec_id_e codec_id{K_MP4_CODEC_ID_H264};
};

RecordRuntime g_record;
LawrecFrameQueue g_record_queue(64, 8*1024*1024);
LawrecFrameQueue g_record_audio_queue(64, 128*1024);

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

void reset_runtime_fields_locked()
{
    g_record.sps.clear(); g_record.pps.clear();
    g_record.startup_pts = 0;
    g_record.last_pts = 0;
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
    g_record.media_reused = false;
    g_record.sys_initialized = false;
    g_record.media_initialized = false;
}

int mkdirs(const char *path)
{
    char tmp[256];
    size_t len;

    if (path == nullptr || path[0] == '\0')
        return -EINVAL;
    len = strnlen(path, sizeof(tmp) - 1);
    if (len == 0 || len >= sizeof(tmp))
        return -ENAMETOOLONG;

    memcpy(tmp, path, len);
    tmp[len] = '\0';
    for (char *p = tmp + 1; *p != '\0'; ++p) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0775) != 0 && errno != EEXIST)
            return -errno;
        *p = '/';
    }
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST)
        return -errno;
    return 0;
}

std::string build_record_path(const lawrec_record_config_t &config)
{
    char ts[32];
    time_t now;
    struct tm tm_now;
    const char *dir = (config.output_dir != nullptr && config.output_dir[0] != '\0')
                          ? config.output_dir
                          : kDefaultOutputDir;
    const char *prefix = (config.file_prefix != nullptr && config.file_prefix[0] != '\0')
                             ? config.file_prefix
                             : kDefaultPrefix;

    now = time(nullptr);
    localtime_r(&now, &tm_now);
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_now);
    return std::string(dir) + "/" + prefix + "_" + ts + ".mp4";
}

/* Called with record.lock held, or before registering the encoder callback.
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
    if (ret < 0) return ret;
    k_mp4_track_info_s track{};
    track.track_type = K_MP4_STREAM_VIDEO;
    track.time_scale = 1000;
    track.video_info.width = g_record.config.video_width;
    track.video_info.height = g_record.config.video_height;
    track.video_info.track_id = 1;
    track.video_info.codec_id = g_record.codec_id;
    ret = kd_mp4_create_track(g_record.mp4_muxer, &g_record.video_track, &track);
    if (ret < 0) return ret;
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
        int tracks = kd_mp4_destroy_tracks(g_record.mp4_muxer);
        int close = kd_mp4_destroy(g_record.mp4_muxer);
        if (tracks || close) ret = -EIO;
        g_record.mp4_muxer = g_record.video_track = g_record.audio_track = nullptr;
    }
    if (g_record.audio_enabled && g_record.segment_frames && !g_record.audio_frames) ret = -ENODATA;
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

// Only the recording worker calls this writer. Drain up to the next video
// boundary so an IDR rotation never puts future audio into the previous file.
int record_audio_before(uint64_t boundary_us, LawrecFramePtr &pending,
                        std::chrono::steady_clock::time_point &progress)
{
    if (!g_record.audio_enabled) return 0;
    for (;;) {
        if (!pending) {
            int ret = g_record_audio_queue.pop(pending, 0);
            if (ret <= 0) return ret;
            progress = std::chrono::steady_clock::now();
        }
        if (pending->pts_us >= boundary_us) return 0;
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (g_record.stop_requested) return 0;
        if (g_record.segment_frames && pending->pts_us >= g_record.segment_pts_us) {
            if (g_record.audio_frames && pending->pts_us < g_record.audio_last_pts_us) return -EINVAL;
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
            frame.data = const_cast<uint8_t *>(pending->bytes.data());
            frame.data_length = pending->bytes.size();
            frame.time_stamp = pending->pts_us - g_record.segment_pts_us;
            if (kd_mp4_write_frame(g_record.mp4_muxer, g_record.audio_track, &frame)) return -EIO;
            g_record.bytes_written += frame.data_length;
            g_record.audio_last_pts_us = pending->pts_us;
            ++g_record.audio_frames;
        }
        pending.reset();
    }
}

int record_video_callback_impl(k_u32 chn_num, kd_venc_data_s *data, k_u8 *private_data)
{
    k_mp4_frame_data_s frame_data;
    int cut;

    (void)chn_num;
    (void)private_data;
    if (data == nullptr)
        return -EINVAL;

    std::lock_guard<std::mutex> guard(g_record.lock);
    if (g_record.mp4_muxer == nullptr || g_record.video_track == nullptr || g_record.stop_requested)
        return 0;

    cut = static_cast<int>(data->status.cur_packs);
    if (cut <= 0 || cut > KD_VENC_MAX_FRAME_PACKCOUNT) return -EINVAL;
    std::vector<uint8_t> frame;
    for (int i = 0; i < cut; ++i) {
        auto &pack = data->astPack[i];
        if (!pack.vir_addr || !pack.len || pack.len > 4 * 1024 * 1024 ||
            frame.size() + pack.len > 4 * 1024 * 1024) return -EINVAL;
        const auto *p = reinterpret_cast<const uint8_t *>(pack.vir_addr);
        frame.insert(frame.end(), p, p + pack.len);
    }
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
    uint64_t pts = data->astPack[0].pts / 1000;
    if (pts < g_record.startup_pts || (g_record.got_first_frame && pts < g_record.last_pts)) {
        g_record.last_error = -EINVAL;
        g_record.stop_requested = true;
        return -EINVAL;
    }
    if (g_record.got_first_frame && g_record.segment_frames && key &&
        g_record.segment_seconds && pts-g_record.segment_pts >= uint64_t(g_record.segment_seconds)*1000) {
        int ret = close_segment_locked(true);
        if (!ret) ret = open_segment_locked();
        if (ret) {
            g_record.last_error = ret; g_record.stop_requested = true;
            g_record.state = LAWREC_RECORD_STATE_STOPPING;
            record_log_state("segment rotation failed", ret);
            return ret;
        }
    }
    if (!g_record.segment_frames) {
        if (!key || !g_record.have_sps || !g_record.have_pps) return 0;
        std::vector<uint8_t> headers;
        if (!frame_sps) headers.insert(headers.end(), g_record.sps.begin(), g_record.sps.end());
        if (!frame_pps) headers.insert(headers.end(), g_record.pps.begin(), g_record.pps.end());
        if (frame.size()+headers.size() > 4*1024*1024) {
            g_record.last_error = -EOVERFLOW; g_record.stop_requested = true;
            return -EOVERFLOW;
        }
        frame.insert(frame.begin(), headers.begin(), headers.end());
        g_record.segment_pts = pts;
        g_record.segment_pts_us = data->astPack[0].pts;
        if (!g_record.got_first_frame) g_record.startup_pts = pts;
    }
    memset(&frame_data, 0, sizeof(frame_data));
    frame_data.codec_id = g_record.codec_id;
    frame_data.data = frame.data();
    frame_data.data_length = frame.size();
    // SDK mp4_format converts microseconds to milliseconds internally.
    // Keep exact capture PTS here; the millisecond fields above are UI/rotation state.
    if (data->astPack[0].pts < g_record.segment_pts_us) return -EINVAL;
    frame_data.time_stamp = data->astPack[0].pts - g_record.segment_pts_us;
    if (kd_mp4_write_frame(g_record.mp4_muxer, g_record.video_track, &frame_data) < 0) {
        g_record.last_error = -EIO;
        g_record.stop_requested = true;
        g_record.state = LAWREC_RECORD_STATE_STOPPING;
        record_log_state("write failed", -EIO);
        return -EIO;
    }
    if (!g_record.got_first_frame) {
        g_record.got_first_frame = true;
        g_record.first_frame_time = std::chrono::steady_clock::now();
        g_record.state = LAWREC_RECORD_STATE_RECORDING;
        record_log_state("first IDR written state=recording", 0);
    }
    g_record.bytes_written += frame.size();
    ++g_record.segment_frames;
    g_record.last_pts = pts;
    ++g_record.callback_count;
    g_record.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_record.first_frame_time).count();
    return 0;
}

int record_video_callback(k_u32 chn_num, kd_venc_data_s *data, k_u8 *private_data)
{
    try { return record_video_callback_impl(chn_num, data, private_data); }
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

void cleanup_record_runtime()
{
    { std::lock_guard<std::mutex> guard(g_record.lock); g_record.stop_requested = true; }
    g_record_queue.close();
    g_record_audio_queue.close();
    int cleanup_error = 0;
    auto check = [&](const char *operation, int ret) {
        if (ret) { cleanup_error = ret; fprintf(stderr, "[record] %s failed=%d; restart required\n", operation, ret); }
    };

    if (g_record.sys_initialized)
        check("encoder unsubscribe", lawrec_encoder_unsubscribe(2));
    if (g_record.sys_initialized && g_record.audio_enabled)
        check("audio unsubscribe", lawrec_audio_unsubscribe(2));
    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (cleanup_error) g_record.last_error = cleanup_error;
        if (close_segment_locked(false)) g_record.last_error = -EIO;
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
    int last_count = 0;
    LawrecFramePtr pending_audio;
    auto audio_progress = last_progress;

    signal(SIGPIPE, SIG_IGN);

    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        g_record.config = config;
        g_record.codec_id = (config.video_type != nullptr &&
                             strcmp(config.video_type, "h265") == 0)
                                ? K_MP4_CODEC_ID_H265
                                : K_MP4_CODEC_ID_H264;
        g_record.last_path = build_record_path(config);
        g_record.last_error = 0;
        reset_runtime_fields_locked();
    }

    ret = init_record_media(config);
    if (ret != K_SUCCESS)
        goto fail;

    ret = open_segment_locked();
    if (ret < 0)
        goto fail;

    ret = lawrec_encoder_subscribe(2, &g_record_queue);
    if (ret != K_SUCCESS)
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
            int audio_ret = record_audio_before(frame->pts_us, pending_audio, audio_progress);
            if (audio_ret) {
                std::lock_guard<std::mutex> guard(g_record.lock);
                g_record.last_error = audio_ret;
                g_record.stop_requested = true;
                record_log_state("audio write/queue failed", audio_ret);
            }
            kd_venc_data_s data{};
            data.status.cur_packs = 1;
            data.astPack[0].vir_addr = reinterpret_cast<k_char *>(const_cast<uint8_t *>(frame->bytes.data()));
            data.astPack[0].len = frame->bytes.size();
            data.astPack[0].pts = frame->pts_us;
            int write_ret = record_video_callback(kRecordVencChn, &data, nullptr);
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
            if (g_record.stop_requested) break;
            auto now = std::chrono::steady_clock::now();
            if (g_record.callback_count != last_count && g_record.got_first_frame) {
                last_progress = now;
                last_count = g_record.callback_count;
            }
            int storage_ret = 0;
            if (now-last_storage_check >= std::chrono::milliseconds(100)) {
                storage_ret = lawrec_storage_check(nullptr);
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
    }

    cleanup_record_runtime();
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
    return;

fail:
    cleanup_record_runtime();
    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        g_record.stop_requested = false;
        record_set_state_locked(LAWREC_RECORD_STATE_FAILED, ret);
    }
    record_log_state("state=failed", ret, g_record.last_path.c_str());
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
        if (local_config.video_type == nullptr)
            local_config.video_type = "h264";
        if (local_config.video_width <= 0)
            local_config.video_width = 1280;
        if (local_config.video_height <= 0)
            local_config.video_height = 720;
        if (local_config.output_dir == nullptr)
            local_config.output_dir = kDefaultOutputDir;
        if (local_config.file_prefix == nullptr)
            local_config.file_prefix = kDefaultPrefix;

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
