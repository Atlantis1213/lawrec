#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>

#include "lawrec_rtsp_entry.h"
#include "streaming_player.h"

using namespace std::chrono_literals;

namespace {

struct RtspRuntime {
    std::mutex lock;
    std::thread worker;
    lawrec_rtsp_config_t config{};
    lawrec_rtsp_state_e state{LAWREC_RTSP_STATE_IDLE};
    bool stop_requested{false};
    bool worker_active{false};
    int last_error{0};
};

RtspRuntime g_rtsp;
static const char *kDefaultLawrecStreamName = "lawrec";

const char *rtsp_state_name(lawrec_rtsp_state_e state)
{
    switch (state) {
    case LAWREC_RTSP_STATE_IDLE:
        return "idle";
    case LAWREC_RTSP_STATE_STARTING:
        return "starting";
    case LAWREC_RTSP_STATE_LIVE:
        return "live";
    case LAWREC_RTSP_STATE_STOPPING:
        return "stopping";
    case LAWREC_RTSP_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

void rtsp_set_state_locked(lawrec_rtsp_state_e state, int last_error)
{
    g_rtsp.state = state;
    g_rtsp.last_error = last_error;
}

void rtsp_log_state(lawrec_rtsp_state_e state, int last_error)
{
    std::cout << "[lawrec-rtsp] state=" << rtsp_state_name(state)
              << " last_error=" << last_error << std::endl;
}

void rtsp_worker(lawrec_rtsp_config_t config)
{
    int ret = 0;
    StreamingPlayer *player = nullptr;
    int created_sessions = 0;
    VideoType video_type = kVideoTypeH265;
    k_vicap_sensor_type sensor_type =
        IMX335_MIPI_2LANE_RAW12_1920X1080_30FPS_LINEAR;
    int session_num = config.session_num;
    int video_width = config.video_width;
    int video_height = config.video_height;
    k_i2s_in_mono_channel audio_input_type =
        static_cast<k_i2s_in_mono_channel>(config.audio_input_type);

    signal(SIGPIPE, SIG_IGN);

    if (config.video_type != nullptr) {
        if (strcmp(config.video_type, "h264") == 0)
            video_type = kVideoTypeH264;
        else if (strcmp(config.video_type, "h265") == 0)
            video_type = kVideoTypeH265;
        else if (strcmp(config.video_type, "mjpeg") == 0)
            video_type = kVideoTypeMjpeg;
    }
    sensor_type = static_cast<k_vicap_sensor_type>(config.sensor_type);

    std::cout << "[lawrec-rtsp] worker start sensor=" << sensor_type
              << " sessions=" << session_num
              << " type=" << (video_type == kVideoTypeH264 ? "h264" :
                              video_type == kVideoTypeH265 ? "h265" : "mjpeg")
              << " size=" << video_width << "x" << video_height
              << " audio=" << audio_input_type << std::endl;

    try {
        player = new StreamingPlayer(sensor_type, video_width, video_height,
                                     session_num);
        if (player == nullptr) {
            ret = -1;
            goto fail;
        }
        if (!player->Ready()) {
            ret = player->InitResult();
            std::cout << "[lawrec-rtsp] player init failed ret=" << ret
                      << std::endl;
            goto fail;
        }

        for (int i = 0; i < session_num; i++) {
            std::string session_name;
            SessionAttr session_attr{};
            session_attr.session_idx = i;
            session_attr.video_type = video_type;
            session_attr.video_width = video_width;
            session_attr.video_height = video_height;
            session_name = (session_num == 1)
                               ? std::string(kDefaultLawrecStreamName)
                               : std::string(kDefaultLawrecStreamName) +
                                     std::to_string(i);
            session_attr.session_name = session_name;
            session_attr.auido_mono_channel_type = audio_input_type;

            std::cout << "[lawrec-rtsp] create session begin idx=" << i
                      << " name=" << session_attr.session_name << std::endl;
            ret = player->CreateSession(session_attr);
            if (ret < 0) {
                std::cout << "[lawrec-rtsp] CreateSession failed idx=" << i
                          << " ret=" << ret << std::endl;
                goto fail;
            }
            created_sessions += 1;
            std::cout << "[lawrec-rtsp] create session done idx=" << i
                      << " name=" << session_attr.session_name << std::endl;
        }

        ret = player->Start();
        if (ret != 0) goto fail;
        auto last_frame = std::chrono::steady_clock::now();
        unsigned long frames = 0;
        {
            std::lock_guard<std::mutex> guard(g_rtsp.lock);
            if (g_rtsp.stop_requested)
                rtsp_set_state_locked(LAWREC_RTSP_STATE_STOPPING, 0);
            else
                rtsp_set_state_locked(LAWREC_RTSP_STATE_STARTING, 0);
        }
        rtsp_log_state(lawrec_rtsp_get_state() == LAWREC_RTSP_STATE_STOPPING
                           ? LAWREC_RTSP_STATE_STOPPING
                           : LAWREC_RTSP_STATE_STARTING,
                       0);
        std::cout << "[lawrec-rtsp] event loop started" << std::endl;

        while (true) {
            {
                std::lock_guard<std::mutex> guard(g_rtsp.lock);
                if (g_rtsp.stop_requested)
                    break;
            }
            unsigned long count = player->FrameCount();
            if ((ret = player->DeliveryError())) {
                std::cerr << "[lawrec-rtsp] delivery failed ret=" << ret << std::endl;
                goto fail;
            }
            auto now = std::chrono::steady_clock::now();
            if (count != frames) {
                frames = count;
                last_frame = now;
                std::lock_guard<std::mutex> guard(g_rtsp.lock);
                if (g_rtsp.state == LAWREC_RTSP_STATE_STARTING) {
                    rtsp_set_state_locked(LAWREC_RTSP_STATE_LIVE, 0);
                    rtsp_log_state(LAWREC_RTSP_STATE_LIVE, 0);
                }
            }
            if (now - last_frame > std::chrono::seconds(8)) {
                ret = -ETIMEDOUT;
                std::cerr << "[lawrec-rtsp] no frame for 8s" << std::endl;
                goto fail;
            }
            std::this_thread::sleep_for(100ms);
        }

        std::cout << "[lawrec-rtsp] stop requested" << std::endl;
        player->Stop();

        for (int i = created_sessions - 1; i >= 0; --i)
            player->DestroySession(i);
        created_sessions = 0;

        player->DeInit();
        ret = player->CleanupError();
        delete player;
        player = nullptr;

        {
            std::lock_guard<std::mutex> guard(g_rtsp.lock);
            g_rtsp.stop_requested = false;
            rtsp_set_state_locked(ret ? LAWREC_RTSP_STATE_FAILED : LAWREC_RTSP_STATE_IDLE, ret);
        }
        rtsp_log_state(ret ? LAWREC_RTSP_STATE_FAILED : LAWREC_RTSP_STATE_IDLE, ret);
        std::cout << "[lawrec-rtsp] exit complete" << std::endl;
        return;
    } catch (const std::exception &ex) {
        std::cout << "[lawrec-rtsp] worker exception: " << ex.what()
                  << std::endl;
        ret = -1;
    } catch (...) {
        std::cout << "[lawrec-rtsp] worker unknown exception" << std::endl;
        ret = -1;
    }

fail:
    if (player != nullptr) {
        player->Stop();
        for (int i = created_sessions - 1; i >= 0; --i)
            player->DestroySession(i);
        player->DeInit();
        delete player;
    }

    {
        std::lock_guard<std::mutex> guard(g_rtsp.lock);
        g_rtsp.stop_requested = false;
        rtsp_set_state_locked(LAWREC_RTSP_STATE_FAILED, ret);
    }
    rtsp_log_state(LAWREC_RTSP_STATE_FAILED, ret);
}

}  // namespace

extern "C" int lawrec_rtsp_entry(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    return 0;
}

extern "C" int lawrec_rtsp_start_async(const lawrec_rtsp_config_t *config)
{
    lawrec_rtsp_config_t local_config;
    std::thread old_worker;

    if (config == nullptr || config->session_num != 1 || !config->video_type ||
        strcmp(config->video_type, "h264") != 0 || config->video_width != 1280 ||
        config->video_height != 720)
        return -EINVAL;

    {
        std::lock_guard<std::mutex> guard(g_rtsp.lock);
        if (g_rtsp.state == LAWREC_RTSP_STATE_STARTING ||
            g_rtsp.state == LAWREC_RTSP_STATE_LIVE) {
            return 0;
        }
        if (g_rtsp.state == LAWREC_RTSP_STATE_STOPPING) {
            return -EBUSY;
        }
        if (g_rtsp.worker_active) return -EBUSY;

        if (g_rtsp.worker.joinable())
            old_worker = std::move(g_rtsp.worker);

        local_config = *config;
        g_rtsp.config = local_config;
        g_rtsp.stop_requested = false;
        rtsp_set_state_locked(LAWREC_RTSP_STATE_STARTING, 0);
        g_rtsp.worker_active = true;
    }

    if (old_worker.joinable())
        old_worker.join();

    rtsp_log_state(LAWREC_RTSP_STATE_STARTING, 0);

    try {
        std::lock_guard<std::mutex> guard(g_rtsp.lock);
        g_rtsp.worker_active = true;
        g_rtsp.worker = std::thread([local_config]() {
            rtsp_worker(local_config);
            std::lock_guard<std::mutex> lock(g_rtsp.lock);
            g_rtsp.worker_active = false;
        });
    } catch (const std::exception &ex) {
        std::lock_guard<std::mutex> guard(g_rtsp.lock);
        g_rtsp.worker_active = false;
        rtsp_set_state_locked(LAWREC_RTSP_STATE_FAILED, -EAGAIN);
        std::cout << "[lawrec-rtsp] thread create failed: " << ex.what()
                  << std::endl;
        return -EAGAIN;
    } catch (...) {
        std::lock_guard<std::mutex> guard(g_rtsp.lock);
        g_rtsp.worker_active = false;
        rtsp_set_state_locked(LAWREC_RTSP_STATE_FAILED, -EAGAIN);
        std::cout << "[lawrec-rtsp] thread create failed: unknown"
                  << std::endl;
        return -EAGAIN;
    }
    return 0;
}

extern "C" int lawrec_rtsp_stop_async(void)
{
    lawrec_rtsp_state_e state;

    {
        std::lock_guard<std::mutex> guard(g_rtsp.lock);
        state = g_rtsp.state;
        if (state == LAWREC_RTSP_STATE_IDLE)
            return 0;
        if (state == LAWREC_RTSP_STATE_FAILED) {
            g_rtsp.stop_requested = false;
            rtsp_set_state_locked(LAWREC_RTSP_STATE_IDLE, 0);
            return 0;
        }
        if (state == LAWREC_RTSP_STATE_STOPPING)
            return 0;

        g_rtsp.stop_requested = true;
        rtsp_set_state_locked(LAWREC_RTSP_STATE_STOPPING, 0);
    }

    if (state == LAWREC_RTSP_STATE_STARTING || state == LAWREC_RTSP_STATE_LIVE)
        rtsp_log_state(LAWREC_RTSP_STATE_STOPPING, 0);
    return 0;
}

extern "C" int lawrec_rtsp_stop_wait(int timeout_ms)
{
    int ret;
    std::thread done_worker;

    ret = lawrec_rtsp_stop_async();
    if (ret != 0)
        return ret;

    if (timeout_ms < 0)
        timeout_ms = 0;

    for (int elapsed = 0; elapsed <= timeout_ms; elapsed += 50) {
        {
            std::lock_guard<std::mutex> guard(g_rtsp.lock);
            if (!g_rtsp.worker_active) {
                if (g_rtsp.worker.joinable())
                    done_worker = std::move(g_rtsp.worker);
                break;
            }
        }

        if (elapsed == timeout_ms)
            break;
        std::this_thread::sleep_for(50ms);
    }

    if (done_worker.joinable())
        done_worker.join();

    {
        std::lock_guard<std::mutex> guard(g_rtsp.lock);
        if (g_rtsp.worker_active) {
            std::cout << "[lawrec-rtsp] stop wait timeout state="
                      << rtsp_state_name(g_rtsp.state) << std::endl;
            return -ETIMEDOUT;
        }
    }

    return 0;
}

extern "C" int lawrec_rtsp_is_running(void)
{
    std::lock_guard<std::mutex> guard(g_rtsp.lock);
    return g_rtsp.state == LAWREC_RTSP_STATE_LIVE ? 1 : 0;
}

extern "C" int lawrec_rtsp_get_state(void)
{
    std::lock_guard<std::mutex> guard(g_rtsp.lock);
    return static_cast<int>(g_rtsp.state);
}

extern "C" int lawrec_rtsp_get_last_error(void)
{
    std::lock_guard<std::mutex> guard(g_rtsp.lock);
    return g_rtsp.last_error;
}
