#pragma once
#include "ui_common.h"
#include "key_proc.h"

typedef struct {
    lv_obj_t *screen, *body, *footer, *keyboard, *dialog, *confirm_button;
    lv_group_t *group, *dialog_group;
    void (*confirm_cb)(void *);
    void (*dialog_closed_cb)(void);
    void *confirm_data;
} settings_page;

void settings_page_create(settings_page *p, const char *title, const char *subtitle, lv_event_cb_t back);
void settings_page_show(settings_page *p);
lv_obj_t *settings_card(lv_obj_t *parent, const char *title, const char *hint);
lv_obj_t *settings_label(lv_obj_t *parent, const char *text, int small);
lv_obj_t *settings_row(lv_obj_t *parent);
lv_obj_t *settings_button(settings_page *p, lv_obj_t *parent, const char *text, int primary,
                          lv_event_cb_t cb, intptr_t command);
lv_obj_t *settings_field(settings_page *p, lv_obj_t *parent, const char *placeholder,
                         unsigned max, const char *accepted);
lv_obj_t *settings_dropdown(settings_page *p, lv_obj_t *parent, const char *options,
                            lv_event_cb_t changed, intptr_t command);
void settings_input_style(lv_obj_t *input);
void settings_keyboard_hide(settings_page *p);
void settings_confirm(settings_page *p, const char *title, const char *message,
                      const char *accept, void (*cb)(void *), void *data);
void settings_message(lv_obj_t *label, const char *text);
void jump_to_scr_media(void);
void jump_to_scr_settings(void);
void jump_to_scr_network(void);
void jump_to_scr_ipv4(void);
void jump_to_scr_storage(void);
void jump_to_scr_time(void);
void jump_to_scr_maintenance(void);
