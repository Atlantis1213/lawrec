#include "lv_port.h"
#include "config.h"
#include "socket.h"
#include "page.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
volatile sig_atomic_t running = 1;
std::mutex lock;
demo::Status snapshot;
int transport_error = -ENOTCONN;
int operation_error = 0;
uint32_t operation_command = 0;
uint32_t pending_command = 0, pending_value = 0;
bool busy = false;
demo::Page page;
void stop(int) { running = 0; }
void clicked(void *, uint32_t command, uint32_t value) {
    unsigned index = command - 1;
    std::lock_guard<std::mutex> guard(lock);
    bool can_stop_media = index >= 2 && (snapshot.flags & (1U << index));
    if (busy || (snapshot.busy & (1U << index)) || (transport_error && !can_stop_media)) return;
    pending_command = command;
    pending_value = value;
    busy = true;
}
void refresh(lv_timer_t *) {
    std::lock_guard<std::mutex> guard(lock);
    page.update({snapshot, transport_error, operation_error, operation_command, busy});
}
void communications() {
    uint32_t sequence = 0;
    while (running) {
        demo::Request request;
        {
            std::lock_guard<std::mutex> guard(lock);
            request.id = ++sequence;
            request.command = pending_command;
            request.value = pending_value;
            pending_command = pending_value = 0;
        }
        demo::Status next;
        int result = demo::exchange(demo::socket_path, request, next);
        {
            std::lock_guard<std::mutex> guard(lock);
            if (!request.command) transport_error = result ? result : next.result;
            if (!result) snapshot = next;
            if (request.command) {
                operation_command = request.command;
                operation_error = result ? result : next.result;
                busy = false;
            }
        }
        for (int i = 0; i < 5 && running; ++i) usleep(100000);
    }
}
}
int main() {
    signal(SIGINT, stop); signal(SIGTERM, stop);
    lv_init(); lv_port_disp_init(); lv_port_indev_init();
    if (!lv_disp_get_default()) return 1;
    page.create(lv_scr_act(), clicked, nullptr);
    refresh(nullptr); lv_timer_create(refresh, 200, nullptr);
    std::thread worker;
    try { worker = std::thread(communications); }
    catch (...) { std::fprintf(stderr, "[ui] socket worker creation failed\n"); return 1; }
    while (running) {
        // The frozen LVGL configuration already supplies custom_tick_get().
        lv_timer_handler(); usleep(5000);
    }
    worker.join();
    // The preserved display adapter owns its own process-lifetime DRM thread.
    _Exit(0);
}
