#include "page.h"
#include <cstdio>

namespace demo {
namespace {
constexpr uint32_t ink = 0x123c38, cream = 0xf5efdd, gold = 0xefc66e, muted = 0xa8beb5;
const char *names[] = {"Preview", "Face AI", "RTSP", "Record"};
lv_obj_t *box(lv_obj_t *root, int x, int y, int w, int h, uint32_t color) {
    auto *object = lv_obj_create(root);
    lv_obj_remove_style_all(object);
    lv_obj_set_pos(object, x, y); lv_obj_set_size(object, w, h);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    return object;
}
lv_obj_t *label(lv_obj_t *root, int x, int y, int width, const lv_font_t *font, uint32_t color) {
    auto *object = lv_label_create(root);
    lv_obj_set_pos(object, x, y); lv_obj_set_width(object, width);
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    return object;
}
bool disabled(const UiView &view, unsigned i) {
    bool stop_media = i >= 2 && (view.status.flags & (1U << i));
    return view.pending || (view.status.busy & (1U << i)) || (view.transport_error && !stop_media);
}
}
void Page::create(lv_obj_t *root, Action action, void *context) {
    action_ = action; context_ = context;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, 480, 800);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    auto *header = box(root, 0, 0, 480, 128, ink);
    auto *eyebrow = label(header, 20, 16, 440, &lv_font_montserrat_14, muted);
    lv_label_set_text(eyebrow, "K230  /  CAMERA + AI");
    auto *title = label(header, 18, 40, 444, &lv_font_montserrat_40, cream);
    lv_label_set_text(title, "Edge Vision");
    feedback_ = label(header, 20, 96, 440, &lv_font_montserrat_16, gold);
    lv_label_set_long_mode(feedback_, LV_LABEL_LONG_DOT);
    auto *chip = box(root, 16, 144, 240, 30, ink);
    lv_obj_set_style_radius(chip, 4, 0);
    viewport_ = label(chip, 10, 6, 220, &lv_font_montserrat_14, cream);
    auto *footer = box(root, 0, 460, 480, 340, ink);
    auto *caption = label(footer, 20, 14, 440, &lv_font_montserrat_14, muted);
    lv_label_set_text(caption, "PIPELINE / MEASURED STATUS");
    faces_ = label(footer, 20, 40, 216, &lv_font_montserrat_16, cream);
    video_ = label(footer, 244, 40, 216, &lv_font_montserrat_16, cream);
    timing_ = label(footer, 20, 88, 440, &lv_font_montserrat_14, muted);
    system_ = label(footer, 20, 110, 440, &lv_font_montserrat_14, muted);
    for (unsigned i = 0; i < 4; ++i) {
        buttons_[i] = lv_btn_create(footer);
        lv_obj_remove_style_all(buttons_[i]);
        lv_obj_set_pos(buttons_[i], 16 + int(i % 2) * 232, 150 + int(i / 2) * 90);
        lv_obj_set_size(buttons_[i], 216, 76);
        lv_obj_set_style_radius(buttons_[i], 10, 0);
        lv_obj_set_style_bg_opa(buttons_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(buttons_[i], 1, 0);
        lv_obj_set_style_border_color(buttons_[i], lv_color_hex(0x65837b), 0);
        auto *name = label(buttons_[i], 16, 12, 184, &lv_font_montserrat_20, cream);
        lv_label_set_text(name, names[i]);
        states_[i] = label(buttons_[i], 16, 43, 184, &lv_font_montserrat_14, muted);
        lv_obj_add_event_cb(buttons_[i], clicked, LV_EVENT_CLICKED, this);
    }
    update({});
}
void Page::clicked(lv_event_t *event) {
    auto *page = static_cast<Page *>(lv_event_get_user_data(event));
    for (unsigned i = 0; i < 4; ++i) {
        if (lv_event_get_target(event) != page->buttons_[i] || disabled(page->view_, i)) continue;
        if (page->action_) page->action_(page->context_, i + 1, !(page->view_.status.flags & (1U << i)));
        return;
    }
}
void Page::update(const UiView &view) {
    view_ = view;
    const auto &status = view.status;
    for (unsigned i = 0; i < 4; ++i) {
        bool active = status.flags & (1U << i);
        bool waiting = status.busy & (1U << i);
        lv_label_set_text(states_[i], waiting ? "WORKING..." : active ? "ON / TAP TO STOP" : "OFF / TAP TO START");
        lv_obj_set_style_bg_color(buttons_[i], lv_color_hex(active ? 0x386359 : 0x204b44), 0);
        lv_obj_set_style_border_color(buttons_[i], lv_color_hex(active ? gold : 0x65837b), 0);
        if (disabled(view, i)) lv_obj_add_state(buttons_[i], LV_STATE_DISABLED);
        else lv_obj_clear_state(buttons_[i], LV_STATE_DISABLED);
        lv_obj_set_style_opa(buttons_[i], disabled(view, i) ? LV_OPA_60 : LV_OPA_COVER, 0);
    }
    char text[256];
    if (view.transport_error) std::snprintf(text, sizeof(text), "Backend unavailable (%d)", view.transport_error);
    else if (view.operation_error) std::snprintf(text, sizeof(text), "%s failed (%d)",
        view.operation_command >= 1 && view.operation_command <= 4 ? names[view.operation_command - 1] : "Request", view.operation_error);
    else if (status.last_error) std::snprintf(text, sizeof(text), "Pipeline error (%d) / check logs", status.last_error);
    else std::snprintf(text, sizeof(text), "%s", view.pending || status.busy ? "Applying request..." : "Ready / record auto-stops at 15s");
    lv_label_set_text(feedback_, text);
    lv_label_set_text(viewport_, status.flags & Preview ? "PREVIEW / REQUESTED ON" : "PREVIEW / OFF");
    std::snprintf(text, sizeof(text), "Faces %u\nAI %.1f ms / %.1f fps", status.detections, status.total_us / 1000.0, status.ai_fps_milli / 1000.0);
    lv_label_set_text(faces_, text);
    std::snprintf(text, sizeof(text), "Video %.1f fps\n%u kbps / Q %u + %u", status.video_fps_milli / 1000.0, status.bitrate_kbps, status.video_queue, status.audio_queue);
    lv_label_set_text(video_, text);
    std::snprintf(text, sizeof(text), "AI2D %.1f / KPU %.1f / POST %.1f ms", status.ai2d_us / 1000.0, status.kpu_us / 1000.0, status.post_us / 1000.0);
    lv_label_set_text(timing_, text);
    char cpu[24] = "--", rss[24] = "--";
    if (status.cpu_percent_milli != metric_unavailable) std::snprintf(cpu, sizeof(cpu), "%.1f%%", status.cpu_percent_milli / 1000.0);
    if (status.rss_kib != metric_unavailable) std::snprintf(rss, sizeof(rss), "%.1fM", status.rss_kib / 1024.0);
    std::snprintf(text, sizeof(text), "Media CPU %s / RSS %s / VB budget %.1fM", cpu, rss, status.vb_kib / 1024.0);
    lv_label_set_text(system_, text);
}
}
