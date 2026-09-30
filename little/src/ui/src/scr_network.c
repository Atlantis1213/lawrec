#include "ui_common.h"
#include "key_proc.h"
#include "lawrec_settings.h"
#include "lawrec_network.h"
#include <string.h>

static lv_obj_t *screen, *ssid, *password, *keyboard, *message;
static lv_group_t *group;
static int confirming;
static lawrec_network_snapshot snapshot;
static unsigned generation;
static int selected;
LV_FONT_DECLARE(lawrec_font_cn_20);

static void show_ap(void)
{
    if (!snapshot.count) { lv_label_set_text(message, "No displayable hotspots found."); return; }
    const lawrec_wifi_ap *ap = &snapshot.aps[selected];
    char text[384];
    snprintf(text, sizeof(text), "%d / %d  %d dBm\n%s\n%.90s\nUse SSID copies the name only.",
             selected+1, snapshot.count, ap->signal_dbm, ap->ssid, ap->flags);
    lv_label_set_text(message, text);
}

static void poll_network(lv_timer_t *timer)
{
    (void)timer;
    if (lv_scr_act() != screen) return;
    lawrec_network_snapshot next;
    lawrec_network_get(&next);
    if (next.busy || next.generation == generation) return;
    generation = next.generation;
    snapshot = next;
    selected = 0;
    if (snapshot.error) {
        char text[256];
        snprintf(text, sizeof(text), "WiFi request failed (%d):\n%s\nRecovery error: %d\n%s",
                 snapshot.error, strerror(-snapshot.error), snapshot.rollback_error,
                 snapshot.rollback_error ? "Check serial/network before retry." : "Check status before retry.");
        lv_label_set_text(message, text);
    } else if (snapshot.scan_result) show_ap();
    else {
        char text[256];
        snprintf(text, sizeof(text), "State: %s\nSSID: %s\nIPv4: %s\n%s",
                 snapshot.state[0] ? snapshot.state : "unknown",
                 snapshot.ssid, snapshot.ipv4[0] ? snapshot.ipv4 : "not assigned",
                 snapshot.connection_changed ? "Connected. Save WiFi to persist." : "");
        lv_label_set_text(message, text);
    }
}

static void edit(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) { confirming = 0; return; }
    lv_keyboard_set_textarea(keyboard, lv_event_get_target(e));
    lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void keyboard_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY || lv_event_get_code(e) == LV_EVENT_CANCEL)
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void action(lv_event_t *e)
{
    intptr_t cmd = (intptr_t)lv_event_get_user_data(e);
    poll_network(NULL);
    if (cmd == 0) {
        lv_textarea_set_text(password, ""); confirming = 0;
        jump_to_scr_main(); return;
    }
    if (cmd == 1 || cmd == 3) {
        int ret = lawrec_network_refresh_async(cmd == 3);
        lv_label_set_text(message, ret ? strerror(-ret) : "WiFi request in progress...");
        confirming = 0; return;
    }
    if (cmd == 4) {
        if (snapshot.count) selected = (selected+1) % snapshot.count;
        confirming = 0; show_ap(); return;
    }
    if (cmd == 5) {
        if (snapshot.count) {
            lv_textarea_set_text(ssid, snapshot.aps[selected].ssid);
            lv_textarea_set_text(password, "");
            lv_label_set_text(message, "SSID copied. Enter password.\nOnly WPA2-PSK can be saved.");
        }
        confirming = 0; return;
    }
    lawrec_network_snapshot current;
    lawrec_network_get(&current);
    if (current.busy) {
        lv_label_set_text(message, "Wait for the WiFi request to finish.");
        confirming = 0; return;
    }
    if (cmd == 7) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        if (confirming != 3) {
            confirming = 3;
            lv_label_set_text(message, "Switch WiFi now? SSH may drop.\nPress Connect again to confirm.\nFailure attempts old connection.\nSave separately after success.");
            return;
        }
        confirming = 0;
        int ret = lawrec_network_connect_async(lv_textarea_get_text(ssid), lv_textarea_get_text(password));
        lv_label_set_text(message, ret ? strerror(-ret) : "Connecting / DHCP...\nFailure will attempt rollback.\nPlease wait for the result.");
        return;
    }
    if (cmd == 6) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        if (confirming != 2) {
            confirming = 2;
            lv_label_set_text(message, "Renew current WiFi DHCP lease?\nSSH / streaming may disconnect.\nPress Renew IP again to confirm.");
            return;
        }
        confirming = 0;
        int ret = lawrec_network_renew_async();
        lv_label_set_text(message, ret ? strerror(-ret) : "Requesting DHCP lease...");
        return;
    }
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    if (confirming != 1) {
        lv_label_set_text(message, "Replace ALL saved WiFi networks?\nPress Save again to confirm.\nApplies on next board boot.\nCurrent SSH stays connected.");
        confirming = 1; return;
    }
    confirming = 0;
    int ret = lawrec_settings_save_wifi(lv_textarea_get_text(ssid), lv_textarea_get_text(password));
    lv_label_set_text(message, ret ? strerror(-ret) : "Saved for next board boot.\nDHCP is used.\nNo live network changes were made.");
    if (!ret) lv_textarea_set_text(password, "");
}

void jump_to_scr_network(void)
{
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0c1b28), 0);
        lv_obj_t *title = lv_label_create(screen);
        lv_obj_set_width(title, 420);
        lv_obj_set_pos(title, 20, 20);
        lv_obj_set_style_text_font(title, &lawrec_font_cn_20, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
        lv_label_set_text(title, "WiFi: WPA2-PSK / DHCP\nSSID: 1-32 bytes\nPassword: 8-63 ASCII characters\nNo quote or backslash characters");
        ssid = lv_textarea_create(screen);
        password = lv_textarea_create(screen);
        lv_obj_t *fields[] = {ssid, password};
        for (unsigned i = 0; i < 2; ++i) {
            lv_obj_set_size(fields[i], 420, 60);
            lv_obj_set_pos(fields[i], 20, 150 + i * 76);
            lv_textarea_set_one_line(fields[i], true);
            lv_textarea_set_max_length(fields[i], i ? 63 : 32);
            lv_textarea_set_placeholder_text(fields[i], i ? "Password" : "SSID");
            lv_obj_add_event_cb(fields[i], edit, LV_EVENT_FOCUSED, NULL);
            lv_obj_add_event_cb(fields[i], edit, LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(fields[i], edit, LV_EVENT_VALUE_CHANGED, NULL);
        }
        lv_textarea_set_password_mode(password, true);
        message = lv_label_create(screen);
        lv_obj_set_width(message, 420);
        lv_obj_set_height(message, 138);
        lv_label_set_long_mode(message, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(message, 20, 310);
        lv_obj_set_style_text_font(message, &lawrec_font_cn_20, 0);
        lv_obj_set_style_text_color(message, lv_color_hex(0xffffff), 0);
        const char *titles[] = {"返回", "Status", "Save WiFi", "Scan", "Next AP", "Use SSID", "Renew IP", "Connect"};
        lv_obj_t *buttons[8];
        for (unsigned i = 0; i < 8; ++i) {
            buttons[i] = lv_btn_create(screen);
            lv_obj_set_size(buttons[i], 135, 64);
            lv_obj_set_pos(buttons[i], 16 + (i % 3) * 147, 460 + (i / 3) * 78);
            lv_obj_add_event_cb(buttons[i], action, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *label = lv_label_create(buttons[i]);
            lv_obj_set_style_text_font(label, &lawrec_font_cn_20, 0);
            lv_label_set_text(label, titles[i]); lv_obj_center(label);
        }
        group = lawrec_key_create_group(buttons, 8);
        keyboard = lv_keyboard_create(screen);
        lv_obj_set_size(keyboard, 480, 260);
        lv_obj_add_event_cb(keyboard, keyboard_event, LV_EVENT_ALL, NULL);
        lv_timer_create(poll_network, 200, NULL);
    }
    confirming = 0;
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(message, "Connect switches WiFi now.\nSave WiFi applies on next boot.\nBoth require confirmation.");
    lv_scr_load(screen);
    lawrec_key_set_group(group);
}
