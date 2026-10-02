#include "settings_ui.h"

LV_FONT_DECLARE(lawrec_font_cn_16);
LV_FONT_DECLARE(lawrec_font_cn_20);
static settings_page *visible;

/* UTC entry needs digits, separators and a space, not three alphabetic modes. */
static const char *time_keys[] = {
    "1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
    "4", "5", "6", "-", "\n",
    "7", "8", "9", ":", "\n",
    LV_SYMBOL_KEYBOARD, "0", " ", LV_SYMBOL_OK, ""
};
#define KEY_ACTION (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG | 2)
static const lv_btnmatrix_ctrl_t time_controls[] = {
    1, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 2, KEY_ACTION, 1, 1, KEY_ACTION
};
#undef KEY_ACTION

static void plain(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *settings_label(lv_obj_t *parent, const char *text, int small)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, small ? &lawrec_font_cn_16 : &lawrec_font_cn_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(small ? 0x9cb3c2 : 0xf0f7fa), 0);
    lv_obj_set_style_text_line_space(label, small ? 5 : 6, 0);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t *settings_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    plain(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    return row;
}

lv_obj_t *settings_card(lv_obj_t *parent, const char *title, const char *hint)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x142936), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x29414f), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    if (title) settings_label(card, title, 0);
    if (hint) settings_label(card, hint, 1);
    return card;
}

lv_obj_t *settings_button(settings_page *p, lv_obj_t *parent, const char *text, int primary,
                          lv_event_cb_t cb, intptr_t command)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_size(button, LV_PCT(100), 56);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? 0x59d2c4 : 0x203d4d), 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(primary ? 0x59d2c4 : 0x395869), 0);
    lv_obj_set_style_outline_color(button, lv_color_hex(0x77e1d6), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(button, 2, LV_STATE_FOCUSED);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &lawrec_font_cn_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(primary ? 0x082623 : 0xe8f3f8), 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (cb) lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, (void *)command);
    if (p) lv_group_add_obj(p->group, button);
    return button;
}

void settings_keyboard_hide(settings_page *p)
{
    lv_obj_add_flag(p->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(p->keyboard, NULL);
    lv_obj_set_height(p->body, 580);
    lv_obj_clear_flag(p->footer, LV_OBJ_FLAG_HIDDEN);
}

static void keyboard_event(lv_event_t *event)
{
    settings_page *p = lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_READY || lv_event_get_code(event) == LV_EVENT_CANCEL)
        settings_keyboard_hide(p);
}

static void focus_field(lv_event_t *event)
{
    settings_page *p = lv_event_get_user_data(event);
    lv_obj_t *field = lv_event_get_target(event);
    const char *accepted = lv_textarea_get_accepted_chars(field);
    lv_keyboard_set_mode(p->keyboard, accepted && (strcmp(accepted, "0123456789.") == 0 ||
                         strcmp(accepted, "0123456789") == 0) ?
                         LV_KEYBOARD_MODE_NUMBER : accepted && strcmp(accepted, "0123456789- :") == 0 ?
                         LV_KEYBOARD_MODE_USER_1 : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(p->keyboard, field);
    lv_obj_set_height(p->body, 400);
    lv_obj_add_flag(p->footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(p->keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(p->screen);
    lv_obj_scroll_to_view_recursive(field, LV_ANIM_OFF);
}

void settings_input_style(lv_obj_t *input)
{
    lv_obj_set_style_text_font(input, &lawrec_font_cn_20, 0);
    lv_obj_set_style_bg_color(input, lv_color_hex(0x0c1c27), 0);
    lv_obj_set_style_text_color(input, lv_color_hex(0xf0f7fa), 0);
    lv_obj_set_style_border_color(input, lv_color_hex(0x446270), 0);
    lv_obj_set_style_radius(input, 10, 0);
}

static void focus_dropdown(lv_event_t *event)
{
    lv_obj_scroll_to_view_recursive(lv_event_get_target(event), LV_ANIM_OFF);
}

lv_obj_t *settings_dropdown(settings_page *p, lv_obj_t *parent, const char *options,
                            lv_event_cb_t changed, intptr_t command)
{
    lv_obj_t *dropdown = lv_dropdown_create(parent);
    lv_obj_set_size(dropdown, LV_PCT(100), 56);
    lv_dropdown_set_options(dropdown, options);
    lv_dropdown_set_symbol(dropdown, "v");
    settings_input_style(dropdown);
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    settings_input_style(list);
    lv_obj_set_style_max_height(list, 280, 0);
    lv_obj_set_style_text_line_space(list, 24, 0);
    lv_obj_set_style_text_font(list, &lawrec_font_cn_20, LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(list, 24, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x59d2c4), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(list, lv_color_hex(0x082623), LV_PART_SELECTED | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x59d2c4), LV_PART_SELECTED | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(list, lv_color_hex(0x082623), LV_PART_SELECTED | LV_STATE_PRESSED);
    lv_obj_add_event_cb(dropdown, focus_dropdown, LV_EVENT_FOCUSED, NULL);
    if (changed) lv_obj_add_event_cb(dropdown, changed, LV_EVENT_VALUE_CHANGED, (void *)command);
    lv_group_add_obj(p->group, dropdown);
    return dropdown;
}

lv_obj_t *settings_field(settings_page *p, lv_obj_t *parent, const char *placeholder,
                         unsigned max, const char *accepted)
{
    lv_obj_t *field = lv_textarea_create(parent);
    lv_textarea_set_one_line(field, true);
    lv_obj_set_size(field, LV_PCT(100), 56);
    lv_textarea_set_max_length(field, max);
    if (accepted) lv_textarea_set_accepted_chars(field, accepted);
    lv_textarea_set_placeholder_text(field, placeholder);
    settings_input_style(field);
    lv_obj_add_event_cb(field, focus_field, LV_EVENT_CLICKED, p);
    /* GPIO focus navigation must not open the keyboard merely by visiting a field. */
    lv_group_add_obj(p->group, field);
    return field;
}

static void close_dialog(settings_page *p)
{
    lv_obj_t *dialog = p->dialog;
    p->dialog = NULL;
    if (dialog) lv_obj_del(dialog);
    if (p->dialog_group) lv_group_del(p->dialog_group);
    p->dialog_group = NULL;
    lawrec_key_set_group(p->group);
}

static void confirm_event(lv_event_t *event)
{
    settings_page *p = lv_event_get_user_data(event);
    int accept = lv_event_get_target(event) == p->confirm_button;
    void (*cb)(void *) = p->confirm_cb;
    void *data = p->confirm_data;
    close_dialog(p);
    if (accept && cb) cb(data);
    if (p->dialog_closed_cb) p->dialog_closed_cb();
}

void settings_confirm(settings_page *p, const char *title, const char *message,
                      const char *accept, void (*cb)(void *), void *data)
{
    settings_keyboard_hide(p);
    if (p->dialog) close_dialog(p);
    p->confirm_cb = cb; p->confirm_data = data;
    p->dialog = lv_obj_create(lv_layer_top());
    plain(p->dialog);
    lv_obj_set_size(p->dialog, 480, 800);
    lv_obj_set_style_bg_color(p->dialog, lv_color_hex(0x030b11), 0);
    lv_obj_set_style_bg_opa(p->dialog, LV_OPA_80, 0);
    lv_obj_add_flag(p->dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *card = settings_card(p->dialog, title, message);
    lv_obj_set_width(card, 432);
    lv_obj_set_style_pad_all(card, 22, 0);
    lv_obj_center(card);
    lv_obj_t *row = settings_row(card);
    lv_obj_t *cancel = settings_button(NULL, row, "取消", 0, NULL, 0);
    p->confirm_button = settings_button(NULL, row, accept, 1, NULL, 0);
    lv_obj_set_flex_grow(cancel, 1); lv_obj_set_width(cancel, 0);
    lv_obj_set_flex_grow(p->confirm_button, 1); lv_obj_set_width(p->confirm_button, 0);
    lv_obj_add_event_cb(cancel, confirm_event, LV_EVENT_CLICKED, p);
    lv_obj_add_event_cb(p->confirm_button, confirm_event, LV_EVENT_CLICKED, p);
    p->dialog_group = lv_group_create();
    lv_group_add_obj(p->dialog_group, cancel); lv_group_add_obj(p->dialog_group, p->confirm_button);
    lv_group_focus_obj(cancel);
    lawrec_key_set_group(p->dialog_group);
}

void settings_message(lv_obj_t *label, const char *text)
{
    lv_label_set_text(label, text);
    lv_obj_scroll_to_view_recursive(label, LV_ANIM_OFF);
}

void settings_page_create(settings_page *p, const char *title, const char *subtitle, lv_event_cb_t back)
{
    p->group = lv_group_create();
    p->screen = lv_obj_create(NULL);
    plain(p->screen);
    lv_obj_set_style_bg_color(p->screen, lv_color_hex(0x091722), 0);
    lv_obj_set_style_bg_grad_color(p->screen, lv_color_hex(0x17303e), 0);
    lv_obj_set_style_bg_grad_dir(p->screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(p->screen, LV_OPA_COVER, 0);
    lv_obj_t *button = settings_button(p, p->screen, "<", 0, back, 0);
    lv_obj_set_size(button, 56, 56); lv_obj_set_pos(button, 20, 22);
    lv_obj_t *label = settings_label(p->screen, title, 0);
    lv_obj_set_size(label, 360, LV_SIZE_CONTENT); lv_obj_set_pos(label, 96, 20);
    label = settings_label(p->screen, subtitle, 1);
    lv_obj_set_width(label, 360); lv_obj_set_pos(label, 96, 52);
    p->body = lv_obj_create(p->screen);
    plain(p->body);
    lv_obj_set_pos(p->body, 20, 104); lv_obj_set_size(p->body, 440, 580);
    lv_obj_set_flex_flow(p->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p->body, 12, 0);
    lv_obj_set_style_pad_bottom(p->body, 8, 0);
    lv_obj_add_flag(p->body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p->body, LV_DIR_VER);
    /* An always-visible overflow indicator makes lower cards discoverable. */
    lv_obj_set_scrollbar_mode(p->body, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(p->body, lv_color_hex(0x59d2c4), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(p->body, LV_OPA_80, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(p->body, 4, LV_PART_SCROLLBAR);
    p->footer = settings_row(p->screen);
    lv_obj_set_pos(p->footer, 20, 716); lv_obj_set_size(p->footer, 440, 64);
    p->keyboard = lv_keyboard_create(p->screen);
    lv_keyboard_set_map(p->keyboard, LV_KEYBOARD_MODE_USER_1, time_keys, time_controls);
    /* The keyboard constructor uses BOTTOM_MID; set_pos alone is still an offset. */
    lv_obj_set_align(p->keyboard, LV_ALIGN_TOP_LEFT);
    lv_obj_set_size(p->keyboard, 480, 280); lv_obj_set_pos(p->keyboard, 0, 520);
    lv_obj_set_style_bg_color(p->keyboard, lv_color_hex(0x0c1c27), 0);
    lv_obj_set_style_bg_color(p->keyboard, lv_color_hex(0x203d4d), LV_PART_ITEMS);
    lv_obj_set_style_text_color(p->keyboard, lv_color_hex(0xf0f7fa), LV_PART_ITEMS);
    /* SimSun lacks LV_SYMBOL_*; the bundled Latin font includes keyboard icons. */
    lv_obj_set_style_text_font(p->keyboard, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(p->keyboard, lv_color_hex(0x375565), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(p->keyboard, lv_color_hex(0xf0f7fa), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(p->keyboard, lv_color_hex(0x59d2c4), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(p->keyboard, lv_color_hex(0x082623), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(p->keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(p->keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(p->keyboard, 8, 0);
    lv_obj_set_style_pad_row(p->keyboard, 8, 0);
    lv_obj_set_style_pad_column(p->keyboard, 6, 0);
    lv_obj_add_event_cb(p->keyboard, keyboard_event, LV_EVENT_ALL, p);
    settings_keyboard_hide(p);
}

void settings_page_show(settings_page *p)
{
    if (visible && visible != p) {
        settings_keyboard_hide(visible);
        if (visible->dialog) {
            close_dialog(visible);
            if (visible->dialog_closed_cb) visible->dialog_closed_cb();
        }
    }
    visible = p;
    settings_keyboard_hide(p);
    lv_scr_load(p->screen);
    lawrec_key_set_group(p->group);
}
