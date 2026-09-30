#include "ui_common.h"
#include "key_proc.h"
#include "lawrec_settings.h"
#include <errno.h>
#include <string.h>

static lv_obj_t *screen, *summary, *port, *bitrate;
static lv_group_t *group;
static unsigned segment_seconds;
static unsigned audio_enabled;
LV_FONT_DECLARE(lawrec_font_cn_20);

static void refresh(void)
{
    char addresses[256], text[768];
    int ret = lawrec_network_addresses(addresses, sizeof(addresses));
    snprintf(text, sizeof(text),
             "Network / Settings\n%s\nRTSP port: %d\nURL: rtsp://<IPv4>:%d/lawrec\n"
             "H.264 720P / active %d kbps\nSegment draft: %u s (0=off)\nRTSP/Record audio draft: %s\nG.711A / 8 kHz mono\n"
             "Save applies after UI restart.",
             ret ? strerror(-ret) : addresses,
             lawrec_settings_port(), lawrec_settings_port(), lawrec_settings_bitrate(), segment_seconds,
             audio_enabled ? "ON" : "OFF");
    lv_label_set_text(summary, text);
}

static void action(lv_event_t *e)
{
    intptr_t cmd = (intptr_t)lv_event_get_user_data(e);
    if (!cmd) { jump_to_scr_main(); return; }
    if (cmd == 9) { extern void jump_to_scr_time(void); jump_to_scr_time(); return; }
    if (cmd == 8) { audio_enabled = !audio_enabled; refresh(); return; }
    if (cmd == 1) {
        const unsigned choices[] = {0, 60, 180, 300, 600};
        unsigned next = 0;
        for (unsigned i = 0; i < 4; ++i) if (segment_seconds == choices[i]) next = choices[i+1];
        segment_seconds = next;
        refresh(); return;
    }
    if (cmd == 2) { lv_spinbox_decrement(port); return; }
    if (cmd == 3) { lv_spinbox_increment(port); return; }
    if (cmd == 4) { lv_spinbox_decrement(bitrate); return; }
    if (cmd == 5) { lv_spinbox_increment(bitrate); return; }
    if (cmd == 7) {
        lv_spinbox_set_value(port, 8554);
        lv_spinbox_set_value(bitrate, 4000);
        segment_seconds = 0;
        audio_enabled = 0;
        lv_label_set_text(summary, "Defaults selected, not saved.\nPress Save to persist.");
        return;
    }
    lawrec_media_settings settings = {1, (unsigned)lv_spinbox_get_value(port),
                                     (unsigned)lv_spinbox_get_value(bitrate), segment_seconds, audio_enabled};
    int ret = lawrec_settings_media_save(&settings);
    char text[192];
    snprintf(text, sizeof(text), ret ? "Save failed: %s" : "Saved. Restart UI to apply.\nCurrent streams are unchanged.",
             ret ? strerror(-ret) : "");
    lv_label_set_text(summary, text);
}

void jump_to_scr_settings(void)
{
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0c1b28), 0);
        summary = lv_label_create(screen);
        lv_obj_set_width(summary, 420);
        lv_obj_set_height(summary, 350);
        lv_label_set_long_mode(summary, LV_LABEL_LONG_DOT);
        lv_obj_align(summary, LV_ALIGN_TOP_MID, 0, 20);
        lv_obj_set_style_text_font(summary, &lawrec_font_cn_20, 0);
        lv_obj_set_style_text_color(summary, lv_color_hex(0xffffff), 0);
        port = lv_spinbox_create(screen);
        bitrate = lv_spinbox_create(screen);
        lv_spinbox_set_range(port, 1024, 65535);
        lv_spinbox_set_digit_format(port, 5, 0);
        lv_spinbox_set_value(port, lawrec_settings_port());
        lv_spinbox_set_step(port, 1);
        lv_obj_set_size(port, 190, 56);
        lv_obj_set_pos(port, 20, 450);
        lv_obj_set_style_text_font(port, &lawrec_font_cn_20, 0);
        lv_spinbox_set_range(bitrate, 1000, 8000);
        lv_spinbox_set_digit_format(bitrate, 4, 0);
        lv_spinbox_set_step(bitrate, 500);
        lv_obj_set_size(bitrate, 190, 56);
        lv_obj_set_pos(bitrate, 235, 450);
        lv_obj_set_style_text_font(bitrate, &lawrec_font_cn_20, 0);
        const char *fields[] = {"RTSP port", "Video kbps"};
        for (unsigned i = 0; i < 2; ++i) {
            lv_obj_t *label = lv_label_create(screen);
            lv_label_set_text(label, fields[i]);
            lv_obj_set_style_text_font(label, &lawrec_font_cn_20, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
            lv_obj_set_pos(label, 20 + i*215, 420);
        }
        const char *titles[] = {"返回", "Segment", "Port -", "Port +", "Kbps -", "Kbps +", "Save", "Defaults", "Audio", "Time"};
        lv_obj_t *buttons[10];
        for (unsigned i = 0; i < 10; ++i) {
            buttons[i] = lv_btn_create(screen);
            lv_obj_set_size(buttons[i], 190, 54);
            lv_obj_set_pos(buttons[i], 20 + (i % 2) * 215, 522 + (i / 2) * 64);
            if (i == 8) lv_obj_set_pos(buttons[i], 20, 360);
            if (i == 9) lv_obj_set_pos(buttons[i], 235, 360);
            lv_obj_add_event_cb(buttons[i], action, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *label = lv_label_create(buttons[i]);
            lv_obj_set_style_text_font(label, &lawrec_font_cn_20, 0);
            lv_label_set_text(label, titles[i]);
            lv_obj_center(label);
        }
        group = lawrec_key_create_group(buttons, 10);
    }
    lawrec_media_settings pending;
    int ret = lawrec_settings_media_pending(&pending);
    lv_spinbox_set_value(port, pending.rtsp_port);
    lv_spinbox_set_value(bitrate, pending.video_bitrate_kbps);
    segment_seconds = pending.record_segment_seconds;
    audio_enabled = pending.audio_enabled;
    refresh();
    if (ret) lv_label_set_text(summary, "Saved settings invalid/unreadable.\nDefaults shown. Check log/file\nbefore overwriting with Save.");
    lv_scr_load(screen);
    lawrec_key_set_group(group);
}
