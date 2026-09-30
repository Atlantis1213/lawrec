#include "ui_common.h"
#include "key_proc.h"
#include "lawrec_settings.h"
#include <errno.h>
#include <string.h>

static lv_obj_t *screen, *summary, *port;
static lv_group_t *group;
LV_FONT_DECLARE(lawrec_font_cn_20);

static void refresh(void)
{
    char addresses[256], text[768];
    int ret = lawrec_network_addresses(addresses, sizeof(addresses));
    snprintf(text, sizeof(text),
             "Network / Settings\n\n%s\nRTSP port: %d\nURL: rtsp://<IPv4>:%d/lawrec\n\n"
             "Video: H.264 1280 x 720\nAudio: unavailable\nRTSP + Record: exclusive\n\n"
             "Port applies after UI restart.\nWiFi: use main menu WiFi page.",
             ret ? strerror(-ret) : addresses,
             lawrec_settings_port(), lawrec_settings_port());
    lv_label_set_text(summary, text);
}

static void action(lv_event_t *e)
{
    intptr_t cmd = (intptr_t)lv_event_get_user_data(e);
    if (!cmd) { jump_to_scr_main(); return; }
    if (cmd == 1) { refresh(); return; }
    if (cmd == 2) { lv_spinbox_decrement(port); return; }
    if (cmd == 3) { lv_spinbox_increment(port); return; }
    int ret = lawrec_settings_save_port((unsigned)lv_spinbox_get_value(port));
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
        lv_obj_align(summary, LV_ALIGN_TOP_MID, 0, 20);
        lv_obj_set_style_text_font(summary, &lawrec_font_cn_20, 0);
        lv_obj_set_style_text_color(summary, lv_color_hex(0xffffff), 0);
        port = lv_spinbox_create(screen);
        lv_spinbox_set_range(port, 1024, 65535);
        lv_spinbox_set_digit_format(port, 5, 0);
        lv_spinbox_set_value(port, lawrec_settings_port());
        lv_spinbox_set_step(port, 1);
        lv_obj_set_size(port, 200, 56);
        lv_obj_align(port, LV_ALIGN_TOP_MID, 0, 450);
        lv_obj_set_style_text_font(port, &lawrec_font_cn_20, 0);
        const char *titles[] = {"返回", "Refresh", "Port -", "Port +", "Save"};
        lv_obj_t *buttons[5];
        for (unsigned i = 0; i < 5; ++i) {
            buttons[i] = lv_btn_create(screen);
            lv_obj_set_size(buttons[i], 190, 62);
            lv_obj_set_pos(buttons[i], 20 + (i % 2) * 215, 530 + (i / 2) * 78);
            lv_obj_add_event_cb(buttons[i], action, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *label = lv_label_create(buttons[i]);
            lv_obj_set_style_text_font(label, &lawrec_font_cn_20, 0);
            lv_label_set_text(label, titles[i]);
            lv_obj_center(label);
        }
        group = lawrec_key_create_group(buttons, 5);
    }
    refresh();
    lv_scr_load(screen);
    lawrec_key_set_group(group);
}
