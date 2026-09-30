#include "ui_common.h"
#include "key_proc.h"
#include "lawrec_time.h"
#include "../../rtsp/include/lawrec_rtsp_entry.h"
#include "../../record/include/lawrec_record_entry.h"
#include "../../playback/lawrec_playback.h"
#include <string.h>

static lv_obj_t *screen, *clock_label, *input, *message, *keyboard;
static lv_group_t *group;
static int confirming;
LV_FONT_DECLARE(lawrec_font_cn_20);
extern void jump_to_scr_settings(void);

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (lv_scr_act() != screen) return;
    char utc[32], text[128];
    int ret = lawrec_time_current_utc(utc, sizeof(utc));
    snprintf(text, sizeof(text), "System time (UTC)\n%s", ret ? "Read failed" : utc);
    lv_label_set_text(clock_label, text);
}

static void edit(lv_event_t *e)
{
    confirming = 0;
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED)
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void action(lv_event_t *e)
{
    intptr_t cmd = (intptr_t)lv_event_get_user_data(e);
    if (!cmd) { confirming = 0; jump_to_scr_settings(); return; }
    int rtsp = lawrec_rtsp_get_state(), record = lawrec_record_get_state();
    if ((rtsp != LAWREC_RTSP_STATE_IDLE && rtsp != LAWREC_RTSP_STATE_FAILED) ||
        (record != LAWREC_RECORD_STATE_IDLE && record != LAWREC_RECORD_STATE_FAILED) ||
        lawrec_playback_active()) {
        confirming = 0;
        lv_label_set_text(message, "Stop RTSP, recording and playback\nbefore changing system time.");
        return;
    }
    int64_t seconds;
    const char *text = lv_textarea_get_text(input);
    int ret = lawrec_time_parse_utc(text, &seconds);
    if (ret) {
        confirming = 0;
        lv_label_set_text(message, "Invalid UTC date/time.\nUse YYYY-MM-DD HH:MM:SS\nYear range: 2020-2099.");
        return;
    }
    if (!confirming) {
        confirming = 1;
        lv_label_set_text(message, "Press Apply again to confirm.\nUTC only. Beijing time = UTC+8.\nRTC/NTP persistence not updated.");
        return;
    }
    confirming = 0;
    ret = lawrec_time_set_utc(text);
    char result[192];
    snprintf(result, sizeof(result), ret ? "Set failed: %s" : "System clock updated.\nRTC not updated; reboot may reset it.", ret ? strerror(-ret) : "");
    lv_label_set_text(message, result);
    refresh(NULL);
}

void jump_to_scr_time(void)
{
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0c1b28), 0);
        clock_label = lv_label_create(screen);
        lv_obj_set_pos(clock_label, 20, 20);
        lv_obj_set_style_text_font(clock_label, &lawrec_font_cn_20, 0);
        lv_obj_set_style_text_color(clock_label, lv_color_hex(0xffffff), 0);
        input = lv_textarea_create(screen);
        lv_obj_set_size(input, 420, 64); lv_obj_set_pos(input, 20, 105);
        lv_textarea_set_one_line(input, true); lv_textarea_set_max_length(input, 19);
        lv_textarea_set_accepted_chars(input, "0123456789- :");
        lv_obj_set_style_text_font(input, &lawrec_font_cn_20, 0);
        message = lv_label_create(screen);
        lv_obj_set_pos(message, 20, 190); lv_obj_set_width(message, 420);
        lv_obj_set_style_text_font(message, &lawrec_font_cn_20, 0);
        lv_obj_set_style_text_color(message, lv_color_hex(0xffffff), 0);
        lv_obj_t *buttons[2];
        for (unsigned i = 0; i < 2; ++i) {
            buttons[i] = lv_btn_create(screen);
            lv_obj_set_size(buttons[i], 190, 60); lv_obj_set_pos(buttons[i], 20+i*220, 320);
            lv_obj_add_event_cb(buttons[i], action, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *label = lv_label_create(buttons[i]);
            lv_obj_set_style_text_font(label, &lawrec_font_cn_20, 0);
            lv_label_set_text(label, i ? "Apply UTC" : "返回"); lv_obj_center(label);
        }
        keyboard = lv_keyboard_create(screen);
        lv_obj_set_size(keyboard, 460, 300); lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -10);
        lv_keyboard_set_textarea(keyboard, input);
        lv_obj_add_event_cb(input, edit, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(input, edit, LV_EVENT_FOCUSED, NULL);
        group = lawrec_key_create_group(buttons, 2);
        lv_timer_create(refresh, 1000, NULL);
    }
    char utc[32];
    if (lawrec_time_current_utc(utc, sizeof(utc))) snprintf(utc, sizeof(utc), "2026-01-01 00:00:00");
    lv_textarea_set_text(input, utc); confirming = 0;
    lv_label_set_text(message, "Enter UTC, not local time.\nYYYY-MM-DD HH:MM:SS\nApply requires a second press.");
    lv_scr_load(screen); lawrec_key_set_group(group); refresh(NULL);
}
