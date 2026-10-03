#include "config.h"
#include "protocol.h"
#include "camera.h"
#include <cstdio>
#include <cstring>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include "mapi_sys_api.h"

namespace {
volatile sig_atomic_t running = 1;
void stop(int) { running = 0; }
}

int main(int argc, char **argv) {
    if (argc == 2 && std::strcmp(argv[1], "--build-info") == 0) {
        std::printf("vision SDK skeleton: preview=%u rgb=%u encode=%u protocol=%u\n",
                    demo::preview_channel, demo::rgb_channel, demo::encode_channel, demo::version);
        return 0;
    }
    if (argc != 1) {
        std::fprintf(stderr, "Usage: vision.elf [--build-info]\n");
        return 2;
    }
    signal(SIGINT, stop); signal(SIGTERM, stop);
    int ret = kd_mapi_sys_init();
    std::printf("[vision] MAPI server init=%d\n", ret);
    if (ret) return 1;
    demo::Camera camera;
    ret = camera.start();
    if (!ret) ret = camera.set_preview(true);
    std::printf("[vision] camera init=%d; AI/IPC integration pending; q exits\n", ret);
    int tty = open("/dev/tty", O_RDONLY | O_NONBLOCK);
    if (!ret && tty < 0) ret = -1;
    while (!ret && running) {
        char input;
        if (read(tty, &input, 1) == 1 && (input == 'q' || input == 'Q')) break;
        usleep(20000);
    }
    if (tty >= 0) close(tty);
    int cleanup = camera.stop();
    int mapi = kd_mapi_sys_deinit();
    std::printf("[vision] shutdown camera=%d mapi=%d\n", cleanup, mapi);
    return ret || cleanup || mapi ? 1 : 0;
}
