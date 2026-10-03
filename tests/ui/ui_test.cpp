#include "page.h"
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#define LODEPNG_NO_COMPILE_CPP
extern "C" {
#include "src/extra/libs/png/lodepng.h"
}

namespace {
constexpr int width = 480, height = 800;
uint32_t tick = 0;
std::array<lv_color_t, width * height> draw, screen;
void flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *pixels) {
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) screen[y * width + x] = *pixels++;
    lv_disp_flush_ready(driver);
}
struct Click { unsigned count = 0; uint32_t command = 0, value = 0; };
void command(void *context, uint32_t operation, uint32_t value) {
    auto *click = static_cast<Click *>(context);
    ++click->count; click->command = operation; click->value = value;
}
void in_bounds(lv_obj_t *object) {
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < width && area.y2 < height);
    if (lv_obj_check_type(object, &lv_label_class)) {
        auto *text = lv_label_get_text(object);
        assert(text && *text);
        // Text must not wrap outside its parent (especially two-line metrics).
        lv_area_t parent;
        lv_obj_get_coords(lv_obj_get_parent(object), &parent);
        if (area.x2 > parent.x2 || area.y2 > parent.y2)
            std::fprintf(stderr, "layout overflow text='%s' area=%d,%d,%d,%d parent=%d,%d,%d,%d\n",
                text, area.x1, area.y1, area.x2, area.y2, parent.x1, parent.y1, parent.x2, parent.y2);
        assert(area.x2 <= parent.x2 && area.y2 <= parent.y2);
    }
    for (unsigned i = 0; i < lv_obj_get_child_cnt(object); ++i) in_bounds(lv_obj_get_child(object, i));
}
void render(const std::string &path) {
    tick += 250;
    lv_obj_update_layout(lv_scr_act());
    in_bounds(lv_scr_act());
    lv_refr_now(nullptr);
    assert(screen[300 * width + 240].ch.alpha == 0); // Camera/OSD remain visible.
    assert(screen[40 * width + 20].ch.alpha == 255);
    // Actual LVGL pixels over a synthetic grid, not camera evidence.
    std::vector<uint8_t> rgb;
    rgb.reserve(width * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto color = screen[y * width + x].ch;
            unsigned alpha = color.alpha;
            unsigned background = x % 40 == 0 || y % 40 == 0 ? 124 : 143;
            rgb.push_back(uint8_t((color.red * alpha + (background - 22) * (255 - alpha)) / 255));
            rgb.push_back(uint8_t((color.green * alpha + background * (255 - alpha)) / 255));
            rgb.push_back(uint8_t((color.blue * alpha + (background - 12) * (255 - alpha)) / 255));
        }
    }
    unsigned char *png = nullptr;
    size_t bytes = 0;
    assert(lodepng_encode24(&png, &bytes, rgb.data(), width, height) == 0);
    FILE *output = std::fopen(path.c_str(), "wb"); assert(output);
    assert(std::fwrite(png, 1, bytes, output) == bytes);
    assert(std::fclose(output) == 0);
    lv_mem_free(png);
}
}
extern "C" uint32_t custom_tick_get(void) { return tick; }
int main(int argc, char **argv) {
    assert(argc == 2);
    lv_init();
    lv_disp_draw_buf_t buffer;
    lv_disp_draw_buf_init(&buffer, draw.data(), nullptr, draw.size());
    lv_disp_drv_t driver;
    lv_disp_drv_init(&driver);
    driver.hor_res = width; driver.ver_res = height;
    driver.draw_buf = &buffer; driver.flush_cb = flush; driver.screen_transp = 1;
    assert(lv_disp_drv_register(&driver));
    demo::Page page; Click click;
    page.create(lv_scr_act(), command, &click);
    demo::UiView view;
    page.update(view);
    render(std::string(argv[1]) + "/idle.png");
    lv_event_send(page.button(0), LV_EVENT_CLICKED, nullptr);
    assert(click.count == 1 && click.command == 1 && click.value == 1);
    view.status.flags = 15;
    view.status.detections = 3; view.status.total_us = 36000; view.status.ai_fps_milli = 12500;
    view.status.ai2d_us = 2300; view.status.kpu_us = 25000; view.status.post_us = 8700;
    view.status.video_fps_milli = 29970; view.status.bitrate_kbps = 3880;
    view.status.rss_kib = 21380; view.status.vb_kib = 50666; view.status.cpu_percent_milli = 24800;
    view.status.video_queue = 2; view.status.audio_queue = 1;
    page.update(view);
    render(std::string(argv[1]) + "/running.png");
    lv_event_send(page.button(2), LV_EVENT_CLICKED, nullptr);
    assert(click.count == 2 && click.command == 3 && click.value == 0);
    view.pending = true; page.update(view);
    lv_event_send(page.button(3), LV_EVENT_CLICKED, nullptr); assert(click.count == 2);
    view.pending = false; view.status.busy = demo::Record; page.update(view);
    lv_event_send(page.button(3), LV_EVENT_CLICKED, nullptr); assert(click.count == 2);
    view.status.busy = 0; view.transport_error = -ETIMEDOUT; page.update(view);
    assert(lv_obj_has_state(page.button(0), LV_STATE_DISABLED));
    assert(!lv_obj_has_state(page.button(2), LV_STATE_DISABLED));
    lv_event_send(page.button(2), LV_EVENT_CLICKED, nullptr); assert(click.count == 3);
    render(std::string(argv[1]) + "/error.png");
    std::puts("LVGL: actual page rendered; viewport alpha/layout/commands/busy/error STOP passed (no hardware)");
}
