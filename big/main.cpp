#include "config.h"
#include "protocol.h"
#include "camera.h"
#include "control.h"
#include "detector.h"
#include "osd.h"
#include <cstdio>
#include <cstring>
#include <csignal>
#include <chrono>
#include <memory>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include "mapi_sys_api.h"

namespace {
volatile sig_atomic_t running = 1;
void stop(int) { running = 0; }
}

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--build-info") == 0) {
        std::printf("vision: preview=%u rgb=%u encode=%u protocol=%u MobileRetinaFace320\n",
                    demo::preview_channel, demo::rgb_channel, demo::encode_channel, demo::version);
        return 0;
    }
    if (argc != 2) {
        std::fprintf(stderr, "Usage: vision.elf RETINAFACE_KMODEL | --build-info\n");
        return 2;
    }
    signal(SIGINT, stop); signal(SIGTERM, stop);
    int ret = kd_mapi_sys_init();
    std::printf("[vision] MAPI server init=%d\n", ret);
    if (ret) return 1;
    demo::Camera camera;
    demo::Osd osd;
    demo::VisionControl control;
    std::unique_ptr<demo::Detector> detector(new (std::nothrow) demo::Detector);
    ret = camera.start();
    if (!ret) ret = detector ? detector->load(argv[1]) : -ENOMEM;
    if (!ret) ret = osd.start();
    demo::Status status;
    status.vb_kib = camera.vb_budget_kib() + osd.budget_kib();
    control.publish(status);
    if (!ret) ret = control.start();
    std::printf("[vision] ready result=%d; preview/AI initially OFF; stop Linux media before q\n", ret);
    int tty = open("/dev/tty", O_RDONLY | O_NONBLOCK);
    if (!ret && tty < 0) ret = -1;
    bool ai_enabled = false;
    std::vector<demo::Face> faces;
    auto period = std::chrono::steady_clock::now();
    unsigned processed = 0;
    while (!ret && running) {
        char input;
        if (read(tty, &input, 1) == 1 && (input == 'q' || input == 'Q')) break;
        demo::Request request;
        if (control.take(request)) {
            int result = 0;
            if (request.command == uint32_t(demo::Command::SetPreview)) {
                result = camera.set_preview(request.value != 0);
                if (!result && !request.value) result = osd.clear();
            } else if (request.command == uint32_t(demo::Command::SetAi)) {
                bool enabled = request.value != 0;
                if (enabled != ai_enabled) {
                    ai_enabled = enabled;
                    faces.clear(); processed = 0; period = std::chrono::steady_clock::now();
                    status.detections = status.ai_fps_milli = 0;
                    if (!enabled) result = osd.clear();
                }
            }
            status.flags = (camera.preview_enabled() ? uint32_t(demo::Preview) : 0U) |
                           (ai_enabled ? uint32_t(demo::Ai) : 0U);
            if (result) status.last_error = result;
            control.finish(result, status);
        }
        if (ai_enabled) {
            demo::AiTiming timing;
            int result = detector->process(faces, timing);
            if (!result && camera.preview_enabled()) result = osd.show(faces);
            if (result) {
                std::printf("[vision-ai] processing failed=%d hex=0x%08x; AI disabled, camera/encoder remain live\n", result, unsigned(result));
                status.last_error = result; ai_enabled = false;
                int clear = osd.clear();
                if (clear) std::printf("[vision-osd] failed-frame clear=%d\n", clear);
                status.flags &= ~demo::Ai; status.detections = status.ai_fps_milli = 0;
            } else {
                status.detections = uint32_t(faces.size());
                status.ai2d_us = timing.ai2d_us; status.kpu_us = timing.kpu_us;
                status.post_us = timing.post_us; status.total_us = timing.total_us;
                ++processed;
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - period).count();
                if (elapsed >= 1000) {
                    status.ai_fps_milli = uint32_t(uint64_t(processed) * 1000000 / elapsed);
                    std::printf("[vision-ai] count=%u ai2d=%u kpu=%u post=%u total=%u us fps=%.2f\n",
                        status.detections, status.ai2d_us, status.kpu_us, status.post_us, status.total_us, status.ai_fps_milli / 1000.f);
                    processed = 0; period = now;
                }
            }
            control.publish(status);
        } else usleep(10000);
    }
    if (tty >= 0) close(tty);
    int ipc = control.stop();
    detector.reset();
    int overlay = osd.stop();
    int cleanup = overlay ? -EBUSY : camera.stop();
    int mapi = (overlay || cleanup) ? -EBUSY : kd_mapi_sys_deinit();
    if (overlay || cleanup) std::printf("[vision] retained resources; skip MAPI deinit\n");
    std::printf("[vision] shutdown IPC=%d OSD=%d camera=%d MAPI=%d\n", ipc, overlay, cleanup, mapi);
    return ret || ipc || cleanup || mapi ? 1 : 0;
}
