// Board-only camera owner. Stop both demo cores before running, even in background.
#include "camera.h"
#include <cstdio>
#include <unistd.h>

int main() {
    demo::Camera camera;
    int ret = camera.start();
    if (!ret) ret = camera.set_preview(true);
    std::printf("[preview-probe] ready=%d; no stdin reader; automatic stop after 12s\n", ret);
    for (unsigned second = 0; !ret && second < 12; ++second) {
        sleep(1);
        if (second == 0 || second == 6 || second == 10) camera.log_buffers();
    }
    int cleanup = camera.stop();
    std::printf("[preview-probe] stop=%d\n", cleanup);
    return ret || cleanup ? 1 : 0;
}
