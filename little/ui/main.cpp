#include "lv_port.h"
#include "config.h"
#include "socket.h"
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
lv_obj_t *labels[4], *buttons[4], *summary;
const char *names[] = {"Preview", "Face AI", "RTSP", "Record"};
void stop(int) { running = 0; }
void clicked(lv_event_t *event) {
    unsigned index = static_cast<unsigned>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    std::lock_guard<std::mutex> guard(lock);
    if (busy || transport_error) return;
    pending_command = index + 1;
    pending_value = !(snapshot.flags & (1U << index));
    busy = true;
}
void refresh(lv_timer_t *) {
    std::lock_guard<std::mutex> guard(lock);
    char line[256];
    for (unsigned i = 0; i < 4; ++i) {
        std::snprintf(line, sizeof(line), "%s  %s", names[i], snapshot.flags & (1U << i) ? "ON" : "OFF");
        lv_label_set_text(labels[i], line);
        if (busy || transport_error) lv_obj_add_state(buttons[i], LV_STATE_DISABLED);
        else lv_obj_clear_state(buttons[i], LV_STATE_DISABLED);
    }
    if (transport_error)
        std::snprintf(line, sizeof(line), "Backend error: %d\nHardware not ready", transport_error);
    else if (operation_error)
        std::snprintf(line, sizeof(line), "%s request failed: %d\nTry another operation; codecs may be unavailable",
            names[operation_command - 1], operation_error);
    else if (snapshot.last_error)
        std::snprintf(line, sizeof(line), "Vision/media error: %d\nCheck vision and media logs", snapshot.last_error);
    else std::snprintf(line, sizeof(line), "Faces %u | AI %.1f ms | %.1f fps\nVideo %u kbps%s",
        snapshot.detections, snapshot.total_us / 1000.0, snapshot.ai_fps_milli / 1000.0,
        snapshot.bitrate_kbps, busy ? " | Working..." : "");
    lv_label_set_text(summary, line);
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
    auto *root = lv_scr_act();
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    auto *title = lv_label_create(root);
    lv_label_set_text(title, "EDGE VISION / K230");
    lv_obj_set_pos(title, 24, 28);
    lv_obj_set_style_text_color(title, lv_color_hex(0xf6e5b5), 0);
    summary = lv_label_create(root);
    lv_obj_set_pos(summary, 24, 80); lv_obj_set_width(summary, 432);
    lv_obj_set_style_text_color(summary, lv_color_hex(0xffffff), 0);
    // Leave the video/OSD area transparent; only header and controls cover it.
    for (unsigned i = 0; i < 4; ++i) {
        buttons[i] = lv_btn_create(root);
        lv_obj_set_pos(buttons[i], 24 + (i % 2) * 224, 566 + (i / 2) * 104);
        lv_obj_set_size(buttons[i], 208, 84);
        lv_obj_set_style_bg_color(buttons[i], lv_color_hex(0x193b3b), 0);
        lv_obj_set_style_border_color(buttons[i], lv_color_hex(0xf6c96e), 0);
        lv_obj_set_style_border_width(buttons[i], 1, 0);
        labels[i] = lv_label_create(buttons[i]); lv_obj_center(labels[i]);
        lv_obj_add_event_cb(buttons[i], clicked, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }
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
