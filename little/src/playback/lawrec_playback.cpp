#include "lawrec_playback.h"
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
    fprintf(stderr, "[playback] state=%d error=%d frames=%llu\n", value, error,
            (unsigned long long)status.frames);
}

void run(int file)
{
    KD_HANDLE demux = nullptr;
    int error = 0, input_pool = -1, output_pool = -1, mem = -1;
    bool leased = false, initialized = false, started = false, bound = false;
    bool display_requested = false, cleanup_ok = true, completed = false;
    uint64_t physical = 0;
    void *mapping = MAP_FAILED;
    size_t map_size = 0;
    auto check = [&](int ret, const char *operation) {
        if (ret) fprintf(stderr, "[playback] %s error=%d\n", operation, ret);
        return ret;
    };
    do {
        k_mp4_config_s config{};
        config.config_type = K_MP4_CONFIG_DEMUXER;
        snprintf(config.demuxer_config.file_name, sizeof(config.demuxer_config.file_name), "/proc/self/fd/%d", file);
        error = check(kd_mp4_create(&demux, &config), "demux create");
        if (error) break;
        k_mp4_file_info_s info{};
        if ((error = check(kd_mp4_get_file_info(demux, &info), "file info"))) break;
        // The SDK frame API does not identify track IDs. Reject ambiguous files.
        if (info.track_num != 1) { error = -ENOTSUP; break; }
        k_mp4_track_info_s track{};
        if ((error = check(kd_mp4_get_track_by_index(demux, 0, &track), "track info"))) break;
        if (track.track_type != K_MP4_STREAM_VIDEO || track.video_info.codec_id != K_MP4_CODEC_ID_H264 ||
            track.video_info.width != 1280 || track.video_info.height != 720) {
            error = -ENOTSUP; break;
        }
        { std::lock_guard<std::mutex> guard(mutex); status.duration_ms = info.duration; }
        if (stop) break;
        if ((error = check(lawrec_media_acquire(kOwner), "media acquire"))) break;
        leased = true;
        display_requested = true; // Also restore after an ambiguous IPC timeout.
        if ((error = lawrec_playback_display_request(1))) break;
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
        mem = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
        if (mem < 0) { error = -errno; break; }
        uint64_t first_pts = 0, last_pts = 0;
        bool first = true, announced = false;
        uint32_t decoded_count = 0;
        auto last_decoded = Clock::now();
        auto origin = Clock::now();
        while (!stop) {
            k_mp4_frame_data_s frame{};
            if ((error = check(kd_mp4_get_frame(demux, &frame), "demux frame"))) break;
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
                break;
            }
            if (!frame.data || !frame.data_length || frame.data_length > kStreamBytes ||
                frame.codec_id != K_MP4_CODEC_ID_H264) { error = -EBADMSG; break; }
            if (first) { first_pts = frame.time_stamp; last_pts = first_pts; origin = Clock::now(); first = false; }
            if (frame.time_stamp < last_pts || frame.time_stamp - last_pts > 10000) { error = -EBADMSG; break; }
            last_pts = frame.time_stamp;
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
            error = check(kd_mapi_vdec_send_stream(0, &stream, 1000), "send stream");
            if (error) break; // Keep the input block until decoder teardown.
            munmap(mapping, map_size); mapping = MAP_FAILED;
            int released = check(kd_mapi_sys_release_vb_block(physical, kStreamBytes), "release input");
            physical = 0;
            if (released) { cleanup_ok = false; if (!error) error = released; }
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
              status.position_ms = frame.time_stamp - first_pts; ++status.frames; }
        }
    } while (false);
    // One worker owns both feed and teardown; UI never frees decoder resources.
    if (bound && check(kd_mapi_vdec_unbind_vo(0, 0, 0), "unbind")) cleanup_ok = false;
    if (started && check(kd_mapi_vdec_stop(0), "stop")) cleanup_ok = false;
    if (initialized && check(kd_mapi_vdec_deinit(0), "deinit")) cleanup_ok = false;
    if (mapping != MAP_FAILED) munmap(mapping, map_size);
    if (cleanup_ok && physical && check(kd_mapi_sys_release_vb_block(physical, kStreamBytes), "release remaining input")) cleanup_ok = false;
    if (mem >= 0) close(mem);
    if (display_requested && lawrec_playback_display_request(0)) cleanup_ok = false;
    // Retain pools if decoder teardown failed: it may still reference them.
    if (cleanup_ok && output_pool >= 0 && check(kd_mapi_vb_destory_pool(output_pool), "destroy output pool")) cleanup_ok = false;
    if (cleanup_ok && input_pool >= 0 && check(kd_mapi_vb_destory_pool(input_pool), "destroy input pool")) cleanup_ok = false;
    if (leased && cleanup_ok) lawrec_media_release(kOwner);
    if (demux && check(kd_mp4_destroy(demux), "demux destroy") && !error) error = -EIO;
    close(file);
    if (!cleanup_ok) { error = error ? error : -EIO; poisoned = true; }
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
    return 0;
}
extern "C" void lawrec_playback_get_status(lawrec_playback_status *out)
{
    if (!out) return;
    std::lock_guard<std::mutex> guard(mutex); *out = status; out->active = active;
}
extern "C" int lawrec_playback_active(void) { return active || poisoned; }
