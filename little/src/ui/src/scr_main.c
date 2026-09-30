/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "ui_common.h"
#include "key_proc.h"

lv_ui_t lv_ui;

#define SCR_MAIN_CARD_W 194
#define SCR_MAIN_CARD_H 146
#define SCR_MAIN_INFO_W 194
#define SCR_MAIN_INFO_H 56
LV_FONT_DECLARE(lawrec_font_cn_20);
#define SCR_MAIN_FONT_CJK &lawrec_font_cn_20
#define SCR_MAIN_FONT_LATIN &lv_font_montserrat_22
#define SCR_MAIN_FONT_STATUS &lawrec_font_cn_20

static void scr_main_apply_root_style(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x06131f), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x16324a), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
}

static void scr_main_apply_panel_style(lv_obj_t *obj)
{
    lv_obj_set_style_radius(obj, 26, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x0f2232), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x6fdcff), LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 14, LV_PART_MAIN);
}

static void scr_main_apply_section_style(lv_obj_t *obj)
{
    lv_obj_set_style_radius(obj, 20, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x091826), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x35566d), LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 12, LV_PART_MAIN);
}

static void scr_main_apply_card_style(lv_obj_t *obj, lv_color_t accent)
{
    lv_obj_set_style_radius(obj, 22, LV_PART_MAIN);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x112838), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x17394d), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, accent, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(obj, accent, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 14, LV_PART_MAIN);
    lv_obj_set_style_outline_width(obj, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(obj, 4, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(obj, accent, LV_STATE_FOCUSED);
}

static lv_obj_t *scr_main_create_png(lv_obj_t *parent, const char *rel_path,
                                     lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *img = lv_img_create(parent);
    char path[256];

    snprintf(path, sizeof(path), "%simg/%s", DATA_FILE_PATH, rel_path);
    lv_img_set_src(img, path);
    lv_obj_set_pos(img, x, y);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

static lv_obj_t *scr_main_create_text(lv_obj_t *parent, const char *text,
                                      const lv_font_t *font,
                                      lv_color_t color, lv_coord_t w,
                                      lv_text_align_t align, lv_coord_t x,
                                      lv_coord_t y)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    if (w > 0)
        lv_obj_set_width(label, w);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

    return label;
}

static lv_obj_t *scr_main_create_info_chip(lv_obj_t *parent, const char *text,
                                           lv_align_t align, lv_coord_t x,
                                           lv_coord_t y)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_style_radius(chip, 16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, lv_color_hex(0x0b1a26), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, lv_color_hex(0x3a627d), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(chip, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(chip, 10, LV_PART_MAIN);
    lv_obj_set_size(chip, SCR_MAIN_INFO_W, SCR_MAIN_INFO_H);
    lv_obj_set_align(chip, align);
    lv_obj_set_pos(chip, x, y);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_font(label, SCR_MAIN_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xd9f5ff), LV_PART_MAIN);

    return chip;
}

static lv_obj_t *scr_main_create_section(lv_obj_t *parent, lv_coord_t x,
                                         lv_coord_t y, lv_coord_t w,
                                         lv_coord_t h, const char *title)
{
    lv_obj_t *section = lv_obj_create(parent);
    lv_obj_remove_style_all(section);
    scr_main_apply_section_style(section);
    lv_obj_set_size(section, w, h);
    lv_obj_set_pos(section, x, y);
    lv_obj_clear_flag(section, LV_OBJ_FLAG_SCROLLABLE);

    scr_main_create_text(section, title, SCR_MAIN_FONT_CJK,
                         lv_color_hex(0xe7f7ff), w - 24, LV_TEXT_ALIGN_LEFT,
                         2, 0);

    return section;
}

static void scr_main_center_horiz(lv_obj_t *obj, lv_coord_t y)
{
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
}

static void scr_main_align_two_col(lv_obj_t *obj, lv_coord_t y, bool left_col,
                                   lv_coord_t item_w, lv_coord_t gap)
{
    lv_coord_t x = (gap + item_w) / 2;
    lv_obj_align(obj, LV_ALIGN_TOP_MID, left_col ? -x : x, y);
}

void scr_main_all_btn_enable(bool enable)
{
    lv_obj_t *objs[] = {
        lv_ui.scr_main_btn_signup.obj,
        lv_ui.scr_main_btn_import.obj,
        lv_ui.scr_main_btn_delete.obj,
        lv_ui.scr_main_btn_ota.obj,
    };

    for (int i = 0; i < ARRAY_SIZE(objs); i++) {
        if (objs[i] == NULL)
            continue;

        if (enable)
            lv_obj_clear_state(objs[i], LV_STATE_DISABLED);
        else
            lv_obj_add_state(objs[i], LV_STATE_DISABLED);
    }
}

static void scr_main_btn_signup_event_handler(lv_event_t *e)
{
    (void)e;
    jump_to_scr_preview();
}

static void scr_main_btn_import_event_handler(lv_event_t *e)
{
    (void)e;
    extern void jump_to_scr_files(void);
    jump_to_scr_files();
}

static void scr_main_btn_delete_event_handler(lv_event_t *e)
{
    (void)e;
    extern void jump_to_scr_settings(void);
    jump_to_scr_settings();
}

static void scr_main_btn_ota_event_handler(lv_event_t *e)
{
    (void)e;
    extern void jump_to_scr_network(void);
    jump_to_scr_network();
}

static void scr_main_create_menu_card(user_img_obj_t *slot, lv_obj_t *parent,
                                      const char *title, const char *desc,
                                      lv_color_t accent, lv_coord_t x,
                                      lv_coord_t y, lv_event_cb_t cb)
{
    lv_obj_t *card = lv_btn_create(parent);
    slot->obj = card;
    lv_obj_set_size(card, SCR_MAIN_CARD_W, SCR_MAIN_CARD_H);
    lv_obj_set_pos(card, x, y);
    scr_main_apply_card_style(card, accent);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

    lv_obj_t *tag = lv_obj_create(card);
    lv_obj_remove_style_all(tag);
    lv_obj_set_size(tag, 62, 10);
    lv_obj_set_style_radius(tag, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(tag, accent, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 0, 0);

    scr_main_create_text(card, title, SCR_MAIN_FONT_CJK,
                         lv_color_hex(0xf4fbff), SCR_MAIN_CARD_W - 28,
                         LV_TEXT_ALIGN_LEFT, 0, 26);
    scr_main_create_text(card, desc, SCR_MAIN_FONT_CJK,
                         lv_color_hex(0xb6d9e8), SCR_MAIN_CARD_W - 28,
                         LV_TEXT_ALIGN_LEFT, 0, 78);
}

void scr_main_set_status(const char *status, lv_color_t color)
{
    if (lv_ui.scr_main_status == NULL)
        return;

    lv_label_set_text(lv_ui.scr_main_status, status);
    lv_obj_set_style_text_color(lv_ui.scr_main_status, color, LV_PART_MAIN);
}

void setup_scr_scr_main(void)
{
    lv_obj_t *obj;
    lv_obj_t *status_section;
    lv_obj_t *menu_section;

    obj = lv_obj_create(NULL);
    lv_ui.scr_main = obj;
    lv_obj_remove_style_all(obj);
    scr_main_apply_root_style(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    obj = lv_obj_create(lv_ui.scr_main);
    lv_ui.scr_main_panel = obj;
    scr_main_apply_panel_style(obj);
    lv_obj_set_size(obj, 448, 752);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    obj = lv_label_create(lv_ui.scr_main_panel);
    lv_ui.scr_main_title = obj;
    lv_label_set_text(obj, "执法记录仪");
    lv_obj_set_pos(obj, 18, 14);
    lv_obj_set_style_text_font(obj, SCR_MAIN_FONT_CJK, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xf4fbff), LV_PART_MAIN);

    obj = lv_label_create(lv_ui.scr_main_panel);
    lv_label_set_text(obj, "设备控制面板");
    lv_obj_set_pos(obj, 20, 54);
    lv_obj_set_style_text_font(obj, SCR_MAIN_FONT_CJK, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(0x8fc7dc), LV_PART_MAIN);

    obj = lv_label_create(lv_ui.scr_main_panel);
    lv_ui.scr_main_status = obj;
    lv_label_set_text(obj, "IPC pending");
    lv_obj_align(obj, LV_ALIGN_TOP_RIGHT, -18, 14);
    lv_obj_set_style_text_font(obj, SCR_MAIN_FONT_STATUS, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xffd166), LV_PART_MAIN);

    status_section = scr_main_create_section(lv_ui.scr_main_panel, 0, 92, 420,
                                             194, "设备状态");
    scr_main_center_horiz(status_section, 92);
    obj = scr_main_create_info_chip(status_section, "H.264 720P",
                                    LV_ALIGN_TOP_MID, 0, 0);
    scr_main_align_two_col(obj, 52, true, SCR_MAIN_INFO_W, 16);
    obj = scr_main_create_info_chip(status_section, "AI 关闭",
                                    LV_ALIGN_TOP_MID, 0, 0);
    scr_main_align_two_col(obj, 52, false, SCR_MAIN_INFO_W, 16);
    obj = scr_main_create_info_chip(status_section, "RTSP 待机",
                                    LV_ALIGN_TOP_MID, 0, 0);
    scr_main_align_two_col(obj, 118, true, SCR_MAIN_INFO_W, 16);
    obj = scr_main_create_info_chip(status_section, "WiFi / IP",
                                    LV_ALIGN_TOP_MID, 0, 0);
    scr_main_align_two_col(obj, 118, false, SCR_MAIN_INFO_W, 16);

    menu_section = scr_main_create_section(lv_ui.scr_main_panel, 0, 304, 420,
                                           392, "快捷入口");
    scr_main_center_horiz(menu_section, 304);
    scr_main_create_menu_card(&lv_ui.scr_main_btn_signup, menu_section,
                              "进入拍摄",
                              "预览录制链路",
                              lv_color_hex(0x6fe7ff), 0, 0,
                              scr_main_btn_signup_event_handler);
    scr_main_align_two_col(lv_ui.scr_main_btn_signup.obj, 40, true,
                           SCR_MAIN_CARD_W, 16);
    scr_main_create_menu_card(&lv_ui.scr_main_btn_import, menu_section,
                              "录像回放",
                              "本地文件浏览",
                              lv_color_hex(0xffd36f), 0, 0,
                              scr_main_btn_import_event_handler);
    scr_main_align_two_col(lv_ui.scr_main_btn_import.obj, 40, false,
                           SCR_MAIN_CARD_W, 16);
    scr_main_create_menu_card(&lv_ui.scr_main_btn_delete, menu_section,
                              "系统设置",
                              "参数网络防抖",
                              lv_color_hex(0x78f0a8), 0, 0,
                              scr_main_btn_delete_event_handler);
    scr_main_align_two_col(lv_ui.scr_main_btn_delete.obj, 194, true,
                           SCR_MAIN_CARD_W, 16);
    scr_main_create_menu_card(&lv_ui.scr_main_btn_ota, menu_section,
                              "WiFi互传",
                              "连接热点推流",
                              lv_color_hex(0xff8f9f), 0, 0,
                              scr_main_btn_ota_event_handler);
    scr_main_align_two_col(lv_ui.scr_main_btn_ota.obj, 194, false,
                           SCR_MAIN_CARD_W, 16);
}

void jump_to_scr_main(void)
{
    lv_ui.scr_status = SCR_MAIN;
    lawrec_key_set_group(lawrec_key_get_main_group());
    lv_scr_load(lv_ui.scr_main);
}

lv_timer_t *create_oneshot_timer(lv_timer_cb_t timer_xcb, uint32_t period,
                                 void *user_data)
{
    lv_timer_t *new_timer;

    new_timer = lv_timer_create(timer_xcb, period, user_data);
    if (new_timer == NULL)
        return NULL;
    lv_timer_set_repeat_count(new_timer, 1);

    return new_timer;
}

static void msgbox_close_event_handler(lv_event_t *e)
{
    lv_obj_t **obj = lv_event_get_user_data(e);

    if (*obj == NULL)
        return;

    lv_timer_del((lv_timer_t *)lv_obj_get_user_data(*obj));
    lv_msgbox_close(*obj);
    *obj = NULL;
}

static void msgbox_timeout_timer_handler(lv_timer_t *timer)
{
    lv_obj_t **obj = (lv_obj_t **)timer->user_data;

    if (*obj == NULL)
        return;

    lv_msgbox_close(*obj);
    *obj = NULL;
}

lv_obj_t *create_msgbox(const char *title, const char *txt)
{
    static lv_obj_t *msgbox = NULL;
    static bool isfirst = true;
    static lv_style_t text_style;
    static lv_style_t title_style;

    if (isfirst) {
        isfirst = false;
        lv_style_init(&text_style);
        lv_style_set_text_font(&text_style, SCR_MAIN_FONT_CJK);
        lv_style_set_text_align(&text_style, LV_TEXT_ALIGN_CENTER);

        lv_style_init(&title_style);
        lv_style_set_text_font(&title_style, SCR_MAIN_FONT_CJK);
        lv_style_set_text_align(&title_style, LV_TEXT_ALIGN_CENTER);
    }

    if (msgbox)
        return msgbox;

    msgbox = lv_msgbox_create(NULL, title, txt, NULL, false);
    lv_label_set_recolor(lv_msgbox_get_text(msgbox), true);
    lv_obj_add_style(lv_msgbox_get_title(msgbox), &title_style, LV_PART_MAIN);
    lv_obj_add_style(lv_msgbox_get_text(msgbox), &text_style, LV_PART_MAIN);
    lv_obj_add_event_cb(lv_obj_get_parent(msgbox), msgbox_close_event_handler,
                        LV_EVENT_CLICKED, (void *)&msgbox);
    lv_obj_center(msgbox);

    lv_timer_t *timer =
        lv_timer_create(msgbox_timeout_timer_handler,
                        1200, (void *)&msgbox);
    lv_timer_set_repeat_count(timer, 1);
    lv_obj_set_user_data(msgbox, timer);

    return msgbox;
}

void scr_main_display_result(int8_t result)
{
    scr_main_set_status(result == 0 ? "IPC command ok" : "IPC command fail",
                        result == 0 ? lv_color_hex(0x4ade80)
                                     : lv_color_hex(0xff6b6b));
    create_msgbox(result == 0 ? "命令成功" : "命令失败",
                  result == 0 ? "大核控制命令\n执行成功"
                               : "大核控制命令\n执行失败");
}
