#include "config.h"
#include "socket.h"
#include <cstdio>
#include <cstring>

int main(int argc, char **argv) {
    demo::Request request; request.id = 1;
    if (argc == 2 && !std::strcmp(argv[1], "status")) {}
    else if (argc == 3 && (!std::strcmp(argv[2], "on") || !std::strcmp(argv[2], "off"))) {
        const char *names[] = {"preview", "ai", "rtsp", "record"};
        for (unsigned i = 0; i < 4; ++i) if (!std::strcmp(argv[1], names[i])) request.command = i + 1;
        if (!request.command) return 2;
        request.value = !std::strcmp(argv[2], "on");
    } else {
        std::fprintf(stderr, "Usage: democtl status | {preview|ai|rtsp|record} {on|off}\n"); return 2;
    }
    demo::Status status;
    int result = demo::exchange(demo::socket_path, request, status);
    if (result) { std::fprintf(stderr, "[democtl] transport=%d\n", result); return 1; }
    std::printf("request=%u value=%u result=%d flags=%u busy=%u error=%d faces=%u "
        "AI_us=%u/%u/%u/%u AI_fps_milli=%u video_fps_milli=%u kbps=%u queues=%u/%u "
        "media_cpu_milli=%u rss_kib=%u vb_budget_kib=%u\n",
        request.command, request.value, status.result, status.flags, status.busy, status.last_error,
        status.detections, status.ai2d_us, status.kpu_us, status.post_us, status.total_us,
        status.ai_fps_milli, status.video_fps_milli, status.bitrate_kbps, status.video_queue,
        status.audio_queue, status.cpu_percent_milli, status.rss_kib, status.vb_kib);
    return status.result ? 1 : 0;
}
