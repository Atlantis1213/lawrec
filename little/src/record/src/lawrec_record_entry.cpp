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
    int last_error{0};
    std::string last_path;
    std::vector<uint8_t> startup_idr;
    uint64_t startup_pts{0};
    int callback_count{0};
    bool got_first_frame{false};
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
    g_record.callback_count = 0;
    g_record.got_first_frame = false;
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

int record_video_callback(k_u32 chn_num, kd_venc_data_s *data, k_u8 *private_data)
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
    if (cut <= 0)
        return 0;

    for (int i = 0; i < cut; ++i) {
        memset(&frame_data, 0, sizeof(frame_data));
        frame_data.codec_id = g_record.codec_id;
        frame_data.data = reinterpret_cast<uint8_t *>(data->astPack[i].vir_addr);
        frame_data.data_length = data->astPack[i].len;
        frame_data.time_stamp = data->astPack[i].pts / 1000;

        if (!g_record.got_first_frame) {
            g_record.startup_idr.insert(g_record.startup_idr.end(),
                                        frame_data.data,
                                        frame_data.data + frame_data.data_length);
            if (g_record.startup_pts == 0)
                g_record.startup_pts = frame_data.time_stamp;
        }
    }

    g_record.callback_count += 1;
    if (!g_record.got_first_frame && g_record.callback_count >= 2 &&
        !g_record.startup_idr.empty()) {
        memset(&frame_data, 0, sizeof(frame_data));
        frame_data.codec_id = g_record.codec_id;
        frame_data.data = g_record.startup_idr.data();
        frame_data.data_length = static_cast<uint32_t>(g_record.startup_idr.size());
        frame_data.time_stamp = g_record.startup_pts;
        if (kd_mp4_write_frame(g_record.mp4_muxer, g_record.video_track, &frame_data) < 0) {
            g_record.last_error = -EIO;
            g_record.stop_requested = true;
            g_record.state = LAWREC_RECORD_STATE_FAILED;
            record_log_state("mp4 write startup frame failed", g_record.last_error,
                             g_record.last_path.c_str());
            return -EIO;
        }
        g_record.got_first_frame = true;
        g_record.startup_idr.clear();
        return 0;
    }

    if (g_record.got_first_frame) {
        for (int i = 0; i < cut; ++i) {
            memset(&frame_data, 0, sizeof(frame_data));
            frame_data.codec_id = g_record.codec_id;
            frame_data.data = reinterpret_cast<uint8_t *>(data->astPack[i].vir_addr);
            frame_data.data_length = data->astPack[i].len;
            frame_data.time_stamp = data->astPack[i].pts / 1000;
            if (kd_mp4_write_frame(g_record.mp4_muxer, g_record.video_track, &frame_data) < 0) {
                g_record.last_error = -EIO;
                g_record.stop_requested = true;
                g_record.state = LAWREC_RECORD_STATE_FAILED;
                record_log_state("mp4 write frame failed", g_record.last_error,
                                 g_record.last_path.c_str());
                return -EIO;
            }
        }
    }
    return 0;
}

int init_record_media(const lawrec_record_config_t &config)
{
    k_mapi_media_attr_t media_attr;
    k_u64 pic_size;
    k_u64 stream_size;
    int ret = K_FAILED;

    for (int attempt = 1; attempt <= 20; ++attempt) {
        ret = kd_mapi_sys_init();
        if (ret == K_SUCCESS) {
            g_record.sys_initialized = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (ret != K_SUCCESS)
        return ret;

    memset(&media_attr, 0, sizeof(media_attr));
    pic_size = static_cast<k_u64>(config.video_width) *
               static_cast<k_u64>(config.video_height) * 2;
    stream_size = static_cast<k_u64>(config.video_width) *
                  static_cast<k_u64>(config.video_height) / 2;

    media_attr.media_config.vb_config.max_pool_cnt = 2;
    media_attr.media_config.vb_config.comm_pool[0].blk_cnt = 10;
    media_attr.media_config.vb_config.comm_pool[0].blk_size = ((pic_size + 0xfff) & ~0xfff);
    media_attr.media_config.vb_config.comm_pool[0].mode = VB_REMAP_MODE_NOCACHE;
    media_attr.media_config.vb_config.comm_pool[1].blk_cnt = 30;
    media_attr.media_config.vb_config.comm_pool[1].blk_size = ((stream_size + 0xfff) & ~0xfff);
    media_attr.media_config.vb_config.comm_pool[1].mode = VB_REMAP_MODE_NOCACHE;

    ret = kd_mapi_media_init(&media_attr);
    if (ret == kLawrecMapiMediaAlreadyInitialized) {
        kd_mapi_media_init_workaround(K_TRUE);
        g_record.media_reused = true;
        g_record.media_initialized = true;
        return K_SUCCESS;
    }
    g_record.media_reused = false;
    if (ret != K_SUCCESS) {
        if (g_record.sys_initialized) {
            kd_mapi_sys_deinit();
            g_record.sys_initialized = false;
        }
        return ret;
    }
    g_record.media_initialized = true;
    return K_SUCCESS;
}

void cleanup_record_runtime()
{
    kd_venc_callback_s venc_callback;

    if (g_record.venc_bound) {
        kd_mapi_venc_unbind_vi(VICAP_DEV_ID_0, kLawrecRecordVicapChn, kRecordVencChn);
        g_record.venc_bound = false;
    }
    if (g_record.venc_started) {
        kd_mapi_venc_stop(kRecordVencChn);
        g_record.venc_started = false;
    }
    if (g_record.callback_registered) {
        memset(&venc_callback, 0, sizeof(venc_callback));
        kd_mapi_venc_unregistercallback(kRecordVencChn, &venc_callback);
        g_record.callback_registered = false;
    }
    if (g_record.venc_initialized) {
        kd_mapi_venc_deinit(kRecordVencChn);
        g_record.venc_initialized = false;
    }
    {
        std::lock_guard<std::mutex> guard(g_record.lock);
        if (g_record.mp4_muxer != nullptr) {
            kd_mp4_destroy_tracks(g_record.mp4_muxer);
            kd_mp4_destroy(g_record.mp4_muxer);
            g_record.mp4_muxer = nullptr;
            g_record.video_track = nullptr;
        }
    }
    if (g_record.media_initialized && !g_record.media_reused)
        kd_mapi_media_deinit();
    g_record.media_initialized = false;
    if (g_record.sys_initialized)
        kd_mapi_sys_deinit();
    g_record.sys_initialized = false;
}

void record_worker(lawrec_record_config_t config)
{
    k_mp4_config_s mp4_config;
    k_mp4_track_info_s track_info;
    k_venc_chn_attr chn_attr;
    kd_venc_callback_s venc_callback;
    int ret;

    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

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

    ret = mkdirs(config.output_dir != nullptr ? config.output_dir : kDefaultOutputDir);
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
        record_set_state_locked(LAWREC_RECORD_STATE_RECORDING, 0);
    }
    record_log_state("state=recording", 0, g_record.last_path.c_str());

    while (true) {
        {
            std::lock_guard<std::mutex> guard(g_record.lock);
            if (g_record.stop_requested)
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    cleanup_record_runtime();
    {
        std::lock_guard<std::mutex> guard(g_record.lock);
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

    if (config == nullptr)
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
        g_record.worker = std::thread(record_worker, local_config);
    } catch (const std::exception &ex) {
        std::lock_guard<std::mutex> guard(g_record.lock);
        record_set_state_locked(LAWREC_RECORD_STATE_FAILED, -EAGAIN);
        record_log_state("thread create failed", -EAGAIN, ex.what());
        return -EAGAIN;
    } catch (...) {
        std::lock_guard<std::mutex> guard(g_record.lock);
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
            if (g_record.state == LAWREC_RECORD_STATE_IDLE ||
                g_record.state == LAWREC_RECORD_STATE_FAILED) {
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
        if (g_record.state != LAWREC_RECORD_STATE_IDLE &&
            g_record.state != LAWREC_RECORD_STATE_FAILED) {
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
