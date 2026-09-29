/* Copyright (c) 2026
 *
 * Touch draw demo for LCKFB little Linux input verification.
 */

#include "lv_port.h"
#include "lvgl.h"
#include <stdbool.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>

#define APP_W 480
#define APP_H 800
#define CANVAS_W 456
#define CANVAS_H 600

typedef struct {
    lv_obj_t *coord_label;
    lv_obj_t *mode_label;
    lv_obj_t *press_dot;
    lv_obj_t *canvas;
    lv_color_t *canvas_buf;
    lv_draw_line_dsc_t line_dsc;
    lv_draw_rect_dsc_t clear_dsc;
    lv_point_t last_point;
    bool last_valid;
} xiaodemo_ctx_t;

static xiaodemo_ctx_t g_ctx;

static int set_priority(void)
{
    int max_prio = sched_get_priority_max(SCHED_RR);
    int min_prio = sched_get_priority_min(SCHED_RR);
    struct sched_param sp = {(max_prio + min_prio) / 4};

    return pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
}

static int set_mallopt(void)
{
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    mallopt(M_MMAP_MAX, 1024);
    return 0;
}

static void set_mode_text(const char *text)
{
    if (g_ctx.mode_label)
        lv_label_set_text_fmt(g_ctx.mode_label, "map: %s", text);
}

static void apply_map_mode(int mode, const char *label)
{
    input_map_config(mode, APP_W, APP_H);
    set_mode_text(label);
}

static void clear_canvas(void)
{
    lv_canvas_fill_bg(g_ctx.canvas, lv_color_hex(0x08131f), LV_OPA_COVER);
    lv_obj_invalidate(g_ctx.canvas);
    g_ctx.last_valid = false;
}

static void btn_normal_event(lv_event_t *e)
{
    (void)e;
    apply_map_mode(MAP_MODE_CLAMP_ONLY, "normal");
}

static void btn_flipx_event(lv_event_t *e)
{
    (void)e;
    apply_map_mode(MAP_MODE_LCKFB_REFLECT_X, "flip-x");
}

static void btn_legacy_event(lv_event_t *e)
{
    (void)e;
    apply_map_mode(MAP_MODE_DOORLOCK_HALF, "doorlock-half");
}

static void btn_clear_event(lv_event_t *e)
{
    (void)e;
    clear_canvas();
}

static lv_obj_t *create_btn(lv_obj_t *parent, const char *text, lv_coord_t x,
                            lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 102, 42);
    lv_obj_set_pos(btn, x, 0);
    lv_obj_set_style_radius(btn, 18, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x143248), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x65d7ff), LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

static void draw_touch(const lv_point_t *p)
{
    lv_point_t local = {
        .x = p->x - 12,
        .y = p->y - 146,
    };

    if (local.x < 0 || local.y < 0 || local.x >= CANVAS_W || local.y >= CANVAS_H)
        return;

    if (g_ctx.last_valid) {
        lv_canvas_draw_line(g_ctx.canvas,
                            (const lv_point_t[]){g_ctx.last_point, local}, 2,
                            &g_ctx.line_dsc);
    }

    lv_canvas_draw_rect(g_ctx.canvas, local.x - 2, local.y - 2, 5, 5,
                        &g_ctx.clear_dsc);
    g_ctx.last_point = local;
    g_ctx.last_valid = true;
    lv_obj_invalidate(g_ctx.canvas);
}

static void update_pointer_visual(const lv_point_t *p, bool pressed)
{
    lv_label_set_text_fmt(g_ctx.coord_label, "x=%03d y=%03d state=%s",
                          p->x, p->y, pressed ? "PR" : "REL");

    if (!pressed) {
        lv_obj_add_flag(g_ctx.press_dot, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(g_ctx.press_dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(g_ctx.press_dot, p->x - 8, p->y - 8);
}

static void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t p = {0, 0};

    if (indev != NULL)
        lv_indev_get_point(indev, &p);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        update_pointer_visual(&p, true);
        draw_touch(&p);
    } else if (code == LV_EVENT_RELEASED) {
        update_pointer_visual(&p, false);
        g_ctx.last_valid = false;
    }
}

static void build_ui(void)
{
    static lv_style_t panel_style;
    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x050c14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "xiaodemo touch draw");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 12);
    lv_obj_set_style_text_color(title, lv_color_hex(0xf4fbff), LV_PART_MAIN);

    g_ctx.mode_label = lv_label_create(scr);
    lv_obj_align(g_ctx.mode_label, LV_ALIGN_TOP_RIGHT, -14, 12);
    lv_obj_set_style_text_color(g_ctx.mode_label, lv_color_hex(0x8fe7ff),
                                LV_PART_MAIN);
    set_mode_text("flip-x");

    g_ctx.coord_label = lv_label_create(scr);
    lv_label_set_text(g_ctx.coord_label, "x=000 y=000 state=REL");
    lv_obj_align(g_ctx.coord_label, LV_ALIGN_TOP_LEFT, 14, 42);
    lv_obj_set_style_text_color(g_ctx.coord_label, lv_color_hex(0xffd166),
                                LV_PART_MAIN);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Buttons: normal / flip-x / legacy / clear");
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 14, 68);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x89a7bb), LV_PART_MAIN);

    lv_style_init(&panel_style);
    lv_style_set_radius(&panel_style, 20);
    lv_style_set_bg_color(&panel_style, lv_color_hex(0x08131f));
    lv_style_set_bg_opa(&panel_style, LV_OPA_COVER);
    lv_style_set_border_width(&panel_style, 2);
    lv_style_set_border_color(&panel_style, lv_color_hex(0x1d5870));

    g_ctx.canvas = lv_canvas_create(scr);
    lv_obj_add_style(g_ctx.canvas, &panel_style, LV_PART_MAIN);
    lv_obj_set_size(g_ctx.canvas, CANVAS_W, CANVAS_H);
    lv_obj_set_pos(g_ctx.canvas, 12, 146);
    lv_obj_clear_flag(g_ctx.canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_ctx.canvas, LV_OBJ_FLAG_CLICKABLE);
    g_ctx.canvas_buf = lv_mem_alloc(CANVAS_W * CANVAS_H * sizeof(lv_color_t));
    lv_canvas_set_buffer(g_ctx.canvas, g_ctx.canvas_buf, CANVAS_W, CANVAS_H,
                         LV_IMG_CF_TRUE_COLOR_ALPHA);

    lv_draw_line_dsc_init(&g_ctx.line_dsc);
    g_ctx.line_dsc.width = 4;
    g_ctx.line_dsc.color = lv_color_hex(0x6fe7ff);

    lv_draw_rect_dsc_init(&g_ctx.clear_dsc);
    g_ctx.clear_dsc.bg_color = lv_color_hex(0xff7b72);
    g_ctx.clear_dsc.bg_opa = LV_OPA_COVER;
    g_ctx.clear_dsc.radius = LV_RADIUS_CIRCLE;
    g_ctx.clear_dsc.border_width = 0;

    clear_canvas();

    lv_obj_t *btn_row = lv_obj_create(scr);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, APP_W - 24, 46);
    lv_obj_set_pos(btn_row, 12, 756);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    create_btn(btn_row, "normal", 0, btn_normal_event);
    create_btn(btn_row, "flip-x", 114, btn_flipx_event);
    create_btn(btn_row, "legacy", 228, btn_legacy_event);
    create_btn(btn_row, "clear", 342, btn_clear_event);

    g_ctx.press_dot = lv_obj_create(scr);
    lv_obj_remove_style_all(g_ctx.press_dot);
    lv_obj_set_size(g_ctx.press_dot, 16, 16);
    lv_obj_set_style_radius(g_ctx.press_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_ctx.press_dot, lv_color_hex(0x7dff93),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_ctx.press_dot, LV_OPA_90, LV_PART_MAIN);
    lv_obj_add_flag(g_ctx.press_dot, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(g_ctx.canvas, touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_ctx.canvas, touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(g_ctx.canvas, touch_event_cb, LV_EVENT_RELEASED, NULL);
}

int main(void)
{
    set_mallopt();
    lv_init();
    lv_port_disp_init();
    lv_port_indev_init();
    input_map_config(MAP_MODE_LCKFB_REFLECT_X, APP_W, APP_H);
    set_priority();

    build_ui();

    while (1) {
        lv_timer_handler();
        usleep(5 * 1000);
    }

    return 0;
}
