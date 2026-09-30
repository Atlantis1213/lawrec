#include "ui_common.h"
#include "key_proc.h"
#include "../../playback/lawrec_playback.h"

static lv_obj_t *screen, *text, *pause_label;
static lv_group_t *group;
static lv_timer_t *timer;
static int leaving;
LV_FONT_DECLARE(lawrec_font_cn_20);
extern void jump_to_scr_files(void);

static void update(lv_timer_t *unused)
{
    (void)unused;
    lawrec_playback_status status;
    lawrec_playback_get_status(&status);
    if (leaving && !status.active) {
        lv_timer_pause(timer);
        jump_to_scr_files();
        return;
    }
    const char *names[] = {"Idle", "Starting", "Playing", "Paused", "Stopping", "Finished", "Failed"};
    char buf[256];
    snprintf(buf, sizeof(buf), "Playback: %s\n%llu / %llu sec\nFrames: %llu   error: %d\n%s",
             status.state >= 0 && status.state <= LAWREC_PLAY_FAILED ? names[status.state] : "Unknown",
             (unsigned long long)status.position_ms / 1000,
             (unsigned long long)status.duration_ms / 1000,
             (unsigned long long)status.frames, status.error,
             leaving ? "Waiting for release; if stalled check log" : "H.264 1280x720 / video only");
    lv_label_set_text(text, buf);
    lv_label_set_text(pause_label, status.state == LAWREC_PLAY_PAUSED ? "Resume" : "Pause");
}

static void action(lv_event_t *e)
{
    if ((intptr_t)lv_event_get_user_data(e)) {
        leaving = 1;
        lawrec_playback_stop();
    } else {
        lawrec_playback_status status;
        lawrec_playback_get_status(&status);
        lawrec_playback_pause(status.state != LAWREC_PLAY_PAUSED);
    }
    update(NULL);
}

void jump_to_scr_playback(void)
{
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0);
        text = lv_label_create(screen);
        lv_obj_set_pos(text, 12, 12);
        lv_obj_set_width(text, 456);
        lv_obj_set_style_text_font(text, &lawrec_font_cn_20, 0);
        lv_obj_set_style_text_color(text, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_color(text, lv_color_hex(0x0c1b28), 0);
        lv_obj_set_style_bg_opa(text, LV_OPA_COVER, 0);
        lv_obj_t *buttons[2];
        for (unsigned i = 0; i < 2; ++i) {
            buttons[i] = lv_btn_create(screen);
            lv_obj_set_size(buttons[i], 200, 70);
            lv_obj_set_pos(buttons[i], 20 + i * 240, 660);
            lv_obj_add_event_cb(buttons[i], action, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *label = lv_label_create(buttons[i]);
            lv_obj_set_style_text_font(label, &lawrec_font_cn_20, 0);
            lv_label_set_text(label, i ? "Stop / Back" : "Pause");
            lv_obj_center(label);
            if (!i) pause_label = label;
        }
        group = lawrec_key_create_group(buttons, 2);
        timer = lv_timer_create(update, 250, NULL);
    }
    leaving = 0;
    lv_scr_load(screen);
    lawrec_key_set_group(group);
    lv_timer_resume(timer);
    update(NULL);
}
