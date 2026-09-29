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
    std::vector<uint8_t> startup_idr;
    uint64_t startup_pts{0};
    uint64_t last_pts{0};
    int callback_count{0};
    bool got_first_frame{false};
    bool have_sps{false};
    bool have_pps{false};
    KD_HANDLE mp4_muxer{nullptr};
    KD_HANDLE video_track{nullptr};
    bool media_reused{false};
    bool sys_initialized{false};
    bool media_initialized{false};
    bool venc_initialized{false};
    bool callback_registered{false};
    bool venc_started{false};
    bool venc_bound{false};
    k_mp4_codec_id_e codec_id{K_MP4_CODEC_ID_H264};
};

RecordRuntime g_record;

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
    g_record.startup_idr.clear();
    g_record.startup_pts = 0;
    g_record.last_pts = 0;
    g_record.callback_count = 0;
    g_record.got_first_frame = false;
    g_record.elapsed_ms = g_record.bytes_written = 0;
    g_record.have_sps = g_record.have_pps = false;
    g_record.mp4_muxer = nullptr;
    g_record.video_track = nullptr;
    g_record.media_reused = false;
    g_record.sys_initialized = false;
    g_record.media_initialized = false;
    g_record.venc_initialized = false;
    g_record.callback_registered = false;
    g_record.venc_started = false;
    g_record.venc_bound = false;
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
    bool key = false;
    std::vector<uint8_t> headers;
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
        if (type == 7) g_record.have_sps = true;
        if (type == 8) g_record.have_pps = true;
        if (type == 5) key = true;
        if (type == 7 || type == 8) headers.insert(headers.end(), frame.begin()+i, frame.begin()+end);
        i = end;
    }
    if (!g_record.got_first_frame) {
        if (g_record.startup_idr.size() + headers.size() > 64 * 1024) {
            g_record.last_error = -EOVERFLOW; g_record.stop_requested = true;
            return -EOVERFLOW;
        }
        g_record.startup_idr.insert(g_record.startup_idr.end(), headers.begin(), headers.end());
        if (!key || !g_record.have_sps || !g_record.have_pps) return 0;
        frame.insert(frame.begin(), g_record.startup_idr.begin(), g_record.startup_idr.end());
        g_record.startup_idr.clear();
        g_record.startup_pts = data->astPack[0].pts / 1000;
    }
    memset(&frame_data, 0, sizeof(frame_data));
    frame_data.codec_id = g_record.codec_id;
    frame_data.data = frame.data();
    frame_data.data_length = frame.size();
    uint64_t pts = data->astPack[0].pts / 1000;
    if (pts < g_record.startup_pts || (g_record.got_first_frame && pts < g_record.last_pts)) {
        g_record.last_error = -EINVAL;
        g_record.stop_requested = true;
        return -EINVAL;
    }
    frame_data.time_stamp = pts - g_record.startup_pts;
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
    kd_venc_callback_s venc_callback;
    int cleanup_error = 0;
    auto check = [&](const char *operation, int ret) {
        if (ret) { cleanup_error = ret; fprintf(stderr, "[record] %s failed=%d; restart required\n", operation, ret); }
    };

    if (g_record.venc_bound) {
        check("unbind", kd_mapi_venc_unbind_vi(VICAP_DEV_ID_0, kLawrecRecordVicapChn, kRecordVencChn));
        g_record.venc_bound = false;
    }
    if (g_record.venc_started) {
        check("stop", kd_mapi_venc_stop(kRecordVencChn));
        g_record.venc_started = false;
    }
    if (g_record.callback_registered) {
        memset(&venc_callback, 0, sizeof(venc_callback));
        check("unregister", kd_mapi_venc_unregistercallback(kRecordVencChn, &venc_callback));
        g_record.callback_registered = false;
    }
    if (g_record.venc_initialized) {
        check("deinit", kd_mapi_venc_deinit(kRecordVencChn));
        g_record.venc_initialized = false;
    }
    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (cleanup_error) g_record.last_error = cleanup_error;
        if (g_record.mp4_muxer != nullptr) {
            int tracks_ret = kd_mp4_destroy_tracks(g_record.mp4_muxer);
            int close_ret = kd_mp4_destroy(g_record.mp4_muxer);
            if (tracks_ret || close_ret) g_record.last_error = -EIO;
            g_record.mp4_muxer = nullptr;
            g_record.video_track = nullptr;
        }
    }
    if (g_record.sys_initialized && !cleanup_error)
        lawrec_media_release(2);
    g_record.sys_initialized = false;
}

void record_worker(lawrec_record_config_t config)
{
    k_mp4_config_s mp4_config;
    k_mp4_track_info_s track_info;
    k_venc_chn_attr chn_attr;
    kd_venc_callback_s venc_callback;
    int ret;
    auto last_progress = std::chrono::steady_clock::now();
    int last_count = 0;

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

    char reserved_path[128];
    ret = lawrec_storage_reserve(reserved_path, sizeof(reserved_path));
    if (!ret) {
        std::lock_guard<std::mutex> guard(g_record.lock);
        g_record.last_path = reserved_path;
    }
    if (ret != 0)
        goto fail;

    ret = init_record_media(config);
    if (ret != K_SUCCESS)
        goto fail;

    memset(&mp4_config, 0, sizeof(mp4_config));
    mp4_config.config_type = K_MP4_CONFIG_MUXER;
    snprintf(mp4_config.muxer_config.file_name,
             sizeof(mp4_config.muxer_config.file_name),
             "%s", g_record.last_path.c_str());
    ret = kd_mp4_create(&g_record.mp4_muxer, &mp4_config);
    if (ret < 0)
        goto fail;

    memset(&track_info, 0, sizeof(track_info));
    track_info.track_type = K_MP4_STREAM_VIDEO;
    track_info.time_scale = 1000;
    track_info.video_info.width = config.video_width;
    track_info.video_info.height = config.video_height;
    track_info.video_info.track_id = 1;
    track_info.video_info.codec_id = g_record.codec_id;
    ret = kd_mp4_create_track(g_record.mp4_muxer, &g_record.video_track, &track_info);
    if (ret < 0)
        goto fail;

    memset(&chn_attr, 0, sizeof(chn_attr));
    chn_attr.rc_attr.rc_mode = K_VENC_RC_MODE_CBR;
    chn_attr.rc_attr.cbr.src_frame_rate = 30;
    chn_attr.rc_attr.cbr.dst_frame_rate = 30;
    chn_attr.rc_attr.cbr.bit_rate = 4000;
    chn_attr.venc_attr.pic_width = config.video_width;
    chn_attr.venc_attr.pic_height = config.video_height;
    chn_attr.venc_attr.stream_buf_cnt = 30;
    chn_attr.venc_attr.stream_buf_size =
        ((config.video_width * config.video_height * 3 / 4 + 0xfff) & ~0xfff);
    if (g_record.codec_id == K_MP4_CODEC_ID_H265) {
        chn_attr.venc_attr.type = K_PT_H265;
        chn_attr.venc_attr.profile = VENC_PROFILE_H265_MAIN;
    } else {
        chn_attr.venc_attr.type = K_PT_H264;
        chn_attr.venc_attr.profile = VENC_PROFILE_H264_HIGH;
    }
    ret = kd_mapi_venc_init(kRecordVencChn, &chn_attr);
    if (ret != K_SUCCESS)
        goto fail;
    g_record.venc_initialized = true;

    ret = kd_mapi_venc_enable_idr(kRecordVencChn, K_TRUE);
    if (ret != K_SUCCESS)
        goto fail;

    memset(&venc_callback, 0, sizeof(venc_callback));
    venc_callback.pfn_data_cb = record_video_callback;
    ret = kd_mapi_venc_registercallback(kRecordVencChn, &venc_callback);
    if (ret != K_SUCCESS)
        goto fail;
    g_record.callback_registered = true;

    ret = kd_mapi_venc_start(kRecordVencChn, -1);
    if (ret != K_SUCCESS)
        goto fail;
    g_record.venc_started = true;

    ret = kd_mapi_venc_bind_vi(VICAP_DEV_ID_0, kLawrecRecordVicapChn, kRecordVencChn);
    if (ret != K_SUCCESS)
        goto fail;
    g_record.venc_bound = true;

    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (g_record.stop_requested)
            g_record.state = LAWREC_RECORD_STATE_STOPPING;
    }
    record_log_state("waiting for first IDR", 0, g_record.last_path.c_str());
    last_progress = std::chrono::steady_clock::now();

    while (true) {
        {
            std::lock_guard<std::mutex> guard(g_record.lock);
            if (g_record.stop_requested) break;
            auto now = std::chrono::steady_clock::now();
            if (g_record.callback_count != last_count && g_record.got_first_frame) {
                last_progress = now;
                last_count = g_record.callback_count;
            }
            int storage_ret = lawrec_storage_check(nullptr);
            if (storage_ret || now - last_progress > std::chrono::seconds(8)) {
                g_record.last_error = storage_ret ? storage_ret : -ETIMEDOUT;
                g_record.stop_requested = true;
                g_record.state = LAWREC_RECORD_STATE_STOPPING;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

    if (config == nullptr || (config->video_type && strcmp(config->video_type, "h264")))
        return -EINVAL;

    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (g_record.state == LAWREC_RECORD_STATE_STARTING ||
            g_record.state == LAWREC_RECORD_STATE_RECORDING)
            return 0;
        if (g_record.state == LAWREC_RECORD_STATE_STOPPING)
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
