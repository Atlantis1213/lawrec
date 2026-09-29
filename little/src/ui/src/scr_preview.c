/* Copyright (c) 2026
 */

#include <stdbool.h>
#include "ui_common.h"
#include "key_proc.h"
#include "../../control/include/lawrec_control.h"
#include "../../record/include/lawrec_record_entry.h"
#include "../../rtsp/include/lawrec_rtsp_entry.h"

static lv_group_t *g_scr_preview_group;
static bool g_back_pending = false;
typedef enum {
    RTSP_BTN_UNAVAILABLE = 0,
    RTSP_BTN_IDLE,
    RTSP_BTN_STARTING,
    RTSP_BTN_LIVE,
    RTSP_BTN_STOPPING,
} scr_preview_rtsp_btn_state_e;

typedef enum {
    RECORD_BTN_UNAVAILABLE = 0,
    RECORD_BTN_IDLE,
    RECORD_BTN_STARTING,
    RECORD_BTN_LIVE,
    RECORD_BTN_STOPPING,
} scr_preview_record_btn_state_e;

static scr_preview_rtsp_btn_state_e g_rtsp_btn_state = RTSP_BTN_UNAVAILABLE;
static scr_preview_record_btn_state_e g_record_btn_state = RECORD_BTN_UNAVAILABLE;
LV_FONT_DECLARE(lawrec_font_cn_20);

static bool scr_preview_rtsp_state_active(void)
{
    int state = lawrec_control_get_rtsp_state();

    return state == LAWREC_RTSP_STATE_STARTING ||
           state == LAWREC_RTSP_STATE_LIVE ||
           state == LAWREC_RTSP_STATE_STOPPING;
}

static bool scr_preview_record_state_active(void)
{
    int state = lawrec_control_get_record_state();

    return state == LAWREC_RECORD_STATE_STARTING ||
           state == LAWREC_RECORD_STATE_RECORDING ||
           state == LAWREC_RECORD_STATE_STOPPING;
}

static void scr_preview_send_enter(void)
{
    /*
     * 进入预览页时，只负责通知大核拉起预览链路。
     * RTSP 是否开启，由预览就绪后的 RTSP 按钮单独控制。
     */
    scr_preview_set_status("预览启动中", lv_color_hex(0xffd166));
    msg_send_cmd(MSG_CMD_PREVIEW_ENTER);
}

static void scr_preview_send_exit(void)
{
    /* 对应的大核退出预览请求。 */
    msg_send_cmd(MSG_CMD_PREVIEW_EXIT);
}

static void scr_preview_apply_rtsp_btn(const char *title, lv_color_t accent,
                                       lv_opa_t bg_opa, bool disabled)
{
    if (lv_ui.scr_preview_btn_rtsp.obj == NULL || lv_ui.scr_preview_rtsp_label == NULL)
        return;

    lv_label_set_text(lv_ui.scr_preview_rtsp_label, title);
    lv_obj_set_style_border_color(lv_ui.scr_preview_btn_rtsp.obj, accent, LV_PART_MAIN);
    lv_obj_set_style_outline_color(lv_ui.scr_preview_btn_rtsp.obj, accent, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(lv_ui.scr_preview_btn_rtsp.obj,
                              disabled ? lv_color_hex(0x1f2a33) : lv_color_hex(0x0c1b28),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(lv_ui.scr_preview_btn_rtsp.obj,
                                   disabled ? lv_color_hex(0x273642) : lv_color_hex(0x17364b),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_ui.scr_preview_btn_rtsp.obj, bg_opa, LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_ui.scr_preview_rtsp_label,
                                disabled ? lv_color_hex(0x90a4b2) : lv_color_hex(0xf4fbff),
                                LV_PART_MAIN);

    if (disabled)
        lv_obj_add_state(lv_ui.scr_preview_btn_rtsp.obj, LV_STATE_DISABLED);
    else
        lv_obj_clear_state(lv_ui.scr_preview_btn_rtsp.obj, LV_STATE_DISABLED);
}

static void scr_preview_apply_record_btn(const char *title, lv_color_t accent,
                                         lv_opa_t bg_opa, bool disabled)
{
    if (lv_ui.scr_preview_btn_record.obj == NULL || lv_ui.scr_preview_record_label == NULL)
        return;

    lv_label_set_text(lv_ui.scr_preview_record_label, title);
    lv_obj_set_style_border_color(lv_ui.scr_preview_btn_record.obj, accent, LV_PART_MAIN);
    lv_obj_set_style_outline_color(lv_ui.scr_preview_btn_record.obj, accent, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(lv_ui.scr_preview_btn_record.obj,
                              disabled ? lv_color_hex(0x241922) : lv_color_hex(0x241116),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(lv_ui.scr_preview_btn_record.obj,
                                   disabled ? lv_color_hex(0x31232d) : lv_color_hex(0x4a1822),
                                   LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_ui.scr_preview_btn_record.obj, bg_opa, LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_ui.scr_preview_record_label,
                                disabled ? lv_color_hex(0xc0a2ab) : lv_color_hex(0xfff4f6),
                                LV_PART_MAIN);

    if (disabled)
        lv_obj_add_state(lv_ui.scr_preview_btn_record.obj, LV_STATE_DISABLED);
    else
        lv_obj_clear_state(lv_ui.scr_preview_btn_record.obj, LV_STATE_DISABLED);
}

void scr_preview_set_rtsp_button_unavailable(void)
{
    g_rtsp_btn_state = RTSP_BTN_UNAVAILABLE;
    scr_preview_apply_rtsp_btn("RTSP开", lv_color_hex(0x5f7688), LV_OPA_60, true);
}

void scr_preview_set_rtsp_button_idle(void)
{
    g_rtsp_btn_state = RTSP_BTN_IDLE;
    scr_preview_apply_rtsp_btn("RTSP开", lv_color_hex(0x6fe7ff), LV_OPA_90, false);
}

void scr_preview_set_rtsp_button_starting(void)
{
    g_rtsp_btn_state = RTSP_BTN_STARTING;
    scr_preview_apply_rtsp_btn("启动中", lv_color_hex(0xffd166), LV_OPA_90, true);
}

void scr_preview_set_rtsp_button_live(void)
{
    g_rtsp_btn_state = RTSP_BTN_LIVE;
    scr_preview_apply_rtsp_btn("RTSP关", lv_color_hex(0x4ade80), LV_OPA_90, false);
}

void scr_preview_set_rtsp_button_stopping(void)
{
    g_rtsp_btn_state = RTSP_BTN_STOPPING;
    scr_preview_apply_rtsp_btn("停止中", lv_color_hex(0xffb86b), LV_OPA_90, true);
}

void scr_preview_set_record_button_unavailable(void)
{
    g_record_btn_state = RECORD_BTN_UNAVAILABLE;
    scr_preview_apply_record_btn("录像开", lv_color_hex(0x88636b), LV_OPA_60, true);
}

void scr_preview_set_record_button_idle(void)
{
    g_record_btn_state = RECORD_BTN_IDLE;
    scr_preview_apply_record_btn("录像开", lv_color_hex(0xff8f9f), LV_OPA_90, false);
}

void scr_preview_set_record_button_starting(void)
{
    g_record_btn_state = RECORD_BTN_STARTING;
    scr_preview_apply_record_btn("启动中", lv_color_hex(0xffd166), LV_OPA_90, true);
}

void scr_preview_set_record_button_live(void)
{
    g_record_btn_state = RECORD_BTN_LIVE;
    scr_preview_apply_record_btn("录像关", lv_color_hex(0xff6b6b), LV_OPA_90, false);
}

void scr_preview_set_record_button_stopping(void)
{
    g_record_btn_state = RECORD_BTN_STOPPING;
    scr_preview_apply_record_btn("停止中", lv_color_hex(0xffb86b), LV_OPA_90, true);
}

void scr_preview_set_status(const char *status, lv_color_t color)
{
    if (lv_ui.scr_preview_status == NULL)
        return;

    lv_label_set_text(lv_ui.scr_preview_status, status);
    lv_obj_set_style_text_color(lv_ui.scr_preview_status, color, LV_PART_MAIN);
}

static void scr_preview_back_event(lv_event_t *e)
{
    (void)e;
    /*
     * 如果 RTSP 仍在运行，就先停 RTSP，再等停止结果回来后退出页面。
     * 这样可以保证预览链路和推流链路按顺序回收。
     */
    scr_preview_set_status("预览关闭中", lv_color_hex(0xffd166));
    g_back_pending = true;
    if (scr_preview_rtsp_state_active()) {
        scr_preview_set_rtsp_button_stopping();
        msg_send_cmd(MSG_CMD_RTSP_STOP);
    }
    if (scr_preview_record_state_active()) {
        scr_preview_set_record_button_stopping();
        msg_send_cmd(MSG_CMD_RECORD_STOP);
    }
    if (!scr_preview_rtsp_state_active() && !scr_preview_record_state_active()) {
        scr_preview_send_exit();
    }
}

static void scr_preview_record_event(lv_event_t *e)
{
    (void)e;
    if (!lawrec_control_is_preview_active()) {
        scr_preview_set_record_button_unavailable();
        scr_preview_set_status("预览未就绪，无法开始录像", lv_color_hex(0xff6b6b));
        return;
    }

    if (g_record_btn_state == RECORD_BTN_STARTING || g_record_btn_state == RECORD_BTN_STOPPING)
        return;

    if (g_record_btn_state == RECORD_BTN_LIVE ||
        lawrec_control_get_record_state() == LAWREC_RECORD_STATE_RECORDING) {
        scr_preview_set_record_button_stopping();
        scr_preview_set_status("录像停止中", lv_color_hex(0xffd166));
        msg_send_cmd(MSG_CMD_RECORD_STOP);
        return;
    }

    scr_preview_set_record_button_starting();
    scr_preview_set_status("录像启动中", lv_color_hex(0xffd166));
    msg_send_cmd(MSG_CMD_RECORD_START);
}

static void scr_preview_rtsp_event(lv_event_t *e)
{
    (void)e;

    /*
     * 只有在 preview 已被确认启动后，RTSP 才允许切换。
     * 按钮外观和可点击状态都由 control 模块的真实状态驱动，而不是本地臆测。
     */
    if (!lawrec_control_is_preview_active()) {
        scr_preview_set_rtsp_button_unavailable();
        scr_preview_set_status("预览未就绪，无法开启RTSP", lv_color_hex(0xff6b6b));
        return;
    }

    if (g_rtsp_btn_state == RTSP_BTN_STARTING || g_rtsp_btn_state == RTSP_BTN_STOPPING)
        return;

    if (g_rtsp_btn_state == RTSP_BTN_LIVE ||
        lawrec_control_get_rtsp_state() == LAWREC_RTSP_STATE_LIVE) {
        scr_preview_set_rtsp_button_stopping();
        scr_preview_set_status("RTSP停止中", lv_color_hex(0xffd166));
        msg_send_cmd(MSG_CMD_RTSP_STOP);
        return;
    }

    scr_preview_set_rtsp_button_starting();
    scr_preview_set_status("RTSP启动中", lv_color_hex(0xffd166));
    msg_send_cmd(MSG_CMD_RTSP_START);
}

static lv_obj_t *scr_preview_create_action_btn(lv_obj_t *parent,
                                               const char *title,
                                               lv_color_t accent,
                                               lv_coord_t x,
                                               lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 100, 94);
    lv_obj_set_pos(btn, x, 14);
    lv_obj_set_style_radius(btn, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0c1b28), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(btn, lv_color_hex(0x17364b), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, accent, LV_PART_MAIN);
    lv_obj_set_style_outline_width(btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(btn, 4, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(btn, accent, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, title);
    lv_obj_center(label);
    lv_obj_set_style_text_font(label, &lawrec_font_cn_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(0xf4fbff), LV_PART_MAIN);

    return btn;
}

static void setup_scr_scr_preview(void)
{
    lv_obj_t *obj;
    lv_obj_t *bar;
    lv_obj_t *items[3];

    if (lv_ui.scr_preview != NULL) {
        lawrec_key_set_group(g_scr_preview_group);
        return;
    }

    obj = lv_obj_create(NULL);
    lv_ui.scr_preview = obj;
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

    obj = lv_label_create(lv_ui.scr_preview);
    lv_ui.scr_preview_status = obj;
    lv_label_set_text(obj, "预览待机");
    lv_obj_align(obj, LV_ALIGN_TOP_RIGHT, -20, 18);
    lv_obj_set_style_text_font(obj, &lawrec_font_cn_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(obj, lv_color_hex(0xe6f7ff), LV_PART_MAIN);

    bar = lv_obj_create(lv_ui.scr_preview);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 420, 124);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_radius(bar, 28, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x06131f), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x4e7a92), LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 12, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    items[0] = scr_preview_create_action_btn(bar, "返回",
                                             lv_color_hex(0x6fe7ff),
                                             12, scr_preview_back_event);
    lv_ui.scr_preview_btn_back.obj = items[0];
    items[1] = scr_preview_create_action_btn(bar, "RTSP开",
                                             lv_color_hex(0x6fe7ff),
                                             116, scr_preview_rtsp_event);
    lv_ui.scr_preview_btn_rtsp.obj = items[1];
    lv_ui.scr_preview_rtsp_label = lv_obj_get_child(items[1], 0);
    items[2] = scr_preview_create_action_btn(bar, "录像",
                                             lv_color_hex(0xff8f9f),
                                             220, scr_preview_record_event);
    lv_ui.scr_preview_btn_record.obj = items[2];
    lv_ui.scr_preview_record_label = lv_obj_get_child(items[2], 0);

    g_scr_preview_group = lawrec_key_create_group(items, ARRAY_SIZE(items));
    lawrec_key_set_group(g_scr_preview_group);
    scr_preview_set_rtsp_button_unavailable();
    scr_preview_set_record_button_unavailable();
}

void jump_to_scr_preview(void)
{
    setup_scr_scr_preview();
    g_back_pending = false;
    lv_ui.scr_status = SCR_PREVIEW;
    lv_scr_load_anim(lv_ui.scr_preview, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
    lawrec_key_set_group(g_scr_preview_group);
    scr_preview_set_rtsp_button_unavailable();
    scr_preview_set_record_button_unavailable();
    /* 先进入预览，再等待异步结果回来后解锁 RTSP 控件。 */
    scr_preview_send_enter();
}

void scr_preview_request_back(void)
{
    g_back_pending = true;
}

int scr_preview_is_back_pending(void)
{
    return g_back_pending ? 1 : 0;
}

void scr_preview_clear_back_pending(void)
{
    g_back_pending = false;
}
