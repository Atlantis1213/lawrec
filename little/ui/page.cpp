#include "page.h"
#include "fonts.h"
#include <cstdio>

namespace demo {
namespace {
constexpr uint32_t ink = 0x123c38, cream = 0xf5efdd, gold = 0xefc66e, muted = 0xa8beb5;
const char *names[] = {"实时预览", "人脸检测", "网络推流", "视频录像"};
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
lv_obj_t *utility_button(lv_obj_t *root, int x, int y, int w, int h, const char *text) {
    auto *button = lv_btn_create(root);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, x, y); lv_obj_set_size(button, w, h);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x204b44), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x65837b), 0);
    auto *caption = label(button, 0, 0, w, &demo_font_cn_24, cream);
    lv_label_set_text(caption, text);
    lv_obj_set_style_text_align(caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(caption);
    return button;
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
    auto *header = box(root, 0, 0, 480, 96, ink);
    auto *title = label(header, 20, 12, 316, &demo_font_cn_32, cream);
    lv_label_set_text(title, "边缘视觉");
    feedback_ = label(header, 20, 60, 316, &demo_font_cn_20, gold);
    lv_label_set_long_mode(feedback_, LV_LABEL_LONG_DOT);
    details_button_ = utility_button(header, 356, 16, 104, 64, "详情");
    lv_obj_add_event_cb(details_button_, toggle_details, LV_EVENT_CLICKED, this);
    auto *chip = box(root, 16, 112, 224, 38, ink);
    lv_obj_set_style_radius(chip, 4, 0);
    viewport_ = label(chip, 10, 8, 204, &demo_font_cn_20, cream);
    auto *footer = box(root, 0, 468, 480, 332, ink);
    faces_ = label(footer, 20, 16, 212, &demo_font_cn_24, cream);
    video_ = label(footer, 248, 16, 212, &demo_font_cn_24, cream);
    for (unsigned i = 0; i < 4; ++i) {
        buttons_[i] = lv_btn_create(footer);
        lv_obj_remove_style_all(buttons_[i]);
        lv_obj_set_pos(buttons_[i], 16 + int(i % 2) * 232, 100 + int(i / 2) * 112);
        lv_obj_set_size(buttons_[i], 216, 104);
        lv_obj_set_style_radius(buttons_[i], 10, 0);
        lv_obj_set_style_bg_opa(buttons_[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(buttons_[i], 1, 0);
        lv_obj_set_style_border_color(buttons_[i], lv_color_hex(0x65837b), 0);
        auto *name = label(buttons_[i], 16, 12, 184, &demo_font_cn_32, cream);
        lv_label_set_text(name, names[i]);
        states_[i] = label(buttons_[i], 16, 64, 184, &demo_font_cn_24, muted);
        lv_obj_add_event_cb(buttons_[i], clicked, LV_EVENT_CLICKED, this);
    }
    details_ = box(root, 0, 0, 480, 800, 0x000000);
    lv_obj_set_style_bg_opa(details_, LV_OPA_60, 0);
    auto *card = box(details_, 16, 160, 448, 476, ink);
    lv_obj_set_style_radius(card, 16, 0);
    auto *heading = label(card, 20, 20, 408, &demo_font_cn_32, cream);
    lv_label_set_text(heading, "运行详情");
    metrics_ = label(card, 20, 76, 408, &demo_font_cn_24, cream);
    lv_obj_set_style_text_line_space(metrics_, 8, 0);
    close_button_ = utility_button(card, 116, 380, 216, 76, "返回画面");
    lv_obj_add_event_cb(close_button_, toggle_details, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(details_, LV_OBJ_FLAG_HIDDEN);
    update({});
}
void Page::toggle_details(lv_event_t *event) {
    auto *page = static_cast<Page *>(lv_event_get_user_data(event));
    if (lv_event_get_target(event) == page->details_button_) {
        lv_obj_clear_flag(page->details_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(page->details_);
    } else lv_obj_add_flag(page->details_, LV_OBJ_FLAG_HIDDEN);
}
void Page::clicked(lv_event_t *event) {
    auto *page = static_cast<Page *>(lv_event_get_user_data(event));
    if (page->details_visible()) return;
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
        lv_label_set_text(states_[i], waiting ? "处理中..." : active ? "已开启 / 关闭" : "已关闭 / 开启");
        lv_obj_set_style_bg_color(buttons_[i], lv_color_hex(active ? 0x386359 : 0x204b44), 0);
        lv_obj_set_style_border_color(buttons_[i], lv_color_hex(active ? gold : 0x65837b), 0);
        if (disabled(view, i)) lv_obj_add_state(buttons_[i], LV_STATE_DISABLED);
        else lv_obj_clear_state(buttons_[i], LV_STATE_DISABLED);
        lv_obj_set_style_opa(buttons_[i], disabled(view, i) ? LV_OPA_60 : LV_OPA_COVER, 0);
    }
    char text[256];
    if (view.transport_error) std::snprintf(text, sizeof(text), "后台未连接 (%d)", view.transport_error);
    else if (view.operation_error) std::snprintf(text, sizeof(text), "%s失败 (%d)",
        view.operation_command >= 1 && view.operation_command <= 4 ? names[view.operation_command - 1] : "操作", view.operation_error);
    else if (status.last_error) std::snprintf(text, sizeof(text), "运行异常 (%d)", status.last_error);
    else std::snprintf(text, sizeof(text), "%s", view.pending || status.busy ? "正在处理，请稍候" : "就绪 / 录像15秒自动停止");
    lv_label_set_text(feedback_, text);
    lv_label_set_text(viewport_, status.flags & Preview ? "预览：请求开启" : "预览：已关闭");
    std::snprintf(text, sizeof(text), "人脸 %u 个\n耗时 %.1f ms", status.detections, status.total_us / 1000.0);
    lv_label_set_text(faces_, text);
    std::snprintf(text, sizeof(text), "视频 %.1f 帧/秒\n%.2f Mbps", status.video_fps_milli / 1000.0, status.bitrate_kbps / 1000.0);
    lv_label_set_text(video_, text);
    char cpu[24] = "--", rss[24] = "--";
    if (status.cpu_percent_milli != metric_unavailable) std::snprintf(cpu, sizeof(cpu), "%.1f%%", status.cpu_percent_milli / 1000.0);
    if (status.rss_kib != metric_unavailable) std::snprintf(rss, sizeof(rss), "%.1fMB", status.rss_kib / 1024.0);
    int error = view.transport_error ? view.transport_error : view.operation_error ? view.operation_error : status.last_error;
    char detail[640];
    std::snprintf(detail, sizeof(detail),
        "AI2D %.1f / KPU %.1f ms\n后处理 %.1f / 总计 %.1f ms\n"
        "AI %.1f / 视频 %.1f 帧/秒\n码率 %u kbps / 队列 %u/%u\n"
        "后台CPU %s / 内存 %s\nVB预算 %.1fMB（不含KPU）\n错误码 %d\n录像15秒自动停止",
        status.ai2d_us / 1000.0, status.kpu_us / 1000.0, status.post_us / 1000.0, status.total_us / 1000.0,
        status.ai_fps_milli / 1000.0, status.video_fps_milli / 1000.0, status.bitrate_kbps,
        status.video_queue, status.audio_queue, cpu, rss, status.vb_kib / 1024.0, error);
    lv_label_set_text(metrics_, detail);
}
}
