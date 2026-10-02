#include "settings_ui.h"
#include "../../playback/lawrec_playback.h"
#include <stdio.h>

static lv_obj_t *screen, *filename, *state_label, *time_label, *notice, *progress;
static lv_obj_t *pause_button, *pause_label, *back_button;
static lv_group_t *group;
static lv_timer_t *timer;
static int leaving;
extern void jump_to_scr_files(void);

static void enabled(lv_obj_t *button, int value)
{
    if (value) lv_obj_clear_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
}

static void update(lv_timer_t *unused)
{
    (void)unused;
    if (lv_scr_act() != screen) { lv_timer_pause(timer); return; }
    lawrec_playback_status status;
    lawrec_playback_get_status(&status);
    /* Inactive means the worker exited, not that a poisoned lease was released. */
    if (leaving && !status.active) {
        lv_timer_pause(timer);
        jump_to_scr_files();
        return;
    }
    const char *names[] = {"未播放", "正在打开", "播放中", "已暂停", "正在停止", "播放结束", "播放失败"};
    char text[256];
    lv_label_set_text(filename, status.filename[0] ? status.filename : "未选择录像");
    snprintf(text, sizeof(text), "%s / %s",
             status.state >= 0 && status.state <= LAWREC_PLAY_FAILED ? names[status.state] : "状态未知",
             status.state == LAWREC_PLAY_STARTING ? "正在读取音轨" : status.audio_present ? "G.711 音轨" : "无音轨");
    lv_label_set_text(state_label, text);
    uint64_t position = status.position_ms / 1000, duration = status.duration_ms / 1000;
    snprintf(text, sizeof(text), "%02llu:%02llu / %02llu:%02llu",
             (unsigned long long)(position / 60), (unsigned long long)(position % 60),
             (unsigned long long)(duration / 60), (unsigned long long)(duration % 60));
    lv_label_set_text(time_label, text);
    const double fraction = status.duration_ms ? (double)status.position_ms / status.duration_ms : 0;
    lv_bar_set_value(progress, fraction >= 1 ? 1000 : (int)(fraction * 1000), LV_ANIM_OFF);
    if (status.resource_retained) {
        const int cleanup = status.cleanup_error ? status.cleanup_error : status.error;
        if (status.error && status.error != cleanup)
            snprintf(text, sizeof(text), "媒体资源清理失败（%d）。\n播放错误码 %d，暂不可再次播放。\n请用串口恢复；显示恢复不能回收媒体。", cleanup, status.error);
        else snprintf(text, sizeof(text), "媒体资源清理失败（%d）。\n返回后暂不可再次播放，请用串口恢复；显示恢复不能回收媒体。", cleanup);
    }
    else if (leaving || status.state == LAWREC_PLAY_STOPPING)
        snprintf(text, sizeof(text), "正在停止并等待资源释放。\n若长时间未返回，请检查回放日志。");
    else if (status.state == LAWREC_PLAY_FAILED)
        snprintf(text, sizeof(text), "播放失败，错误码 %d。\n请返回检查文件格式与日志；仅支持 H.264 720P 和可选 G.711 音轨。", status.error);
    else if (status.state == LAWREC_PLAY_FINISHED)
        snprintf(text, sizeof(text), "本段录像已播放结束。\n点击返回文件，可选择其他录像。");
    else
        snprintf(text, sizeof(text), "暂停后可继续播放；返回将停止回放。\n进度仅供查看，本版本不支持拖动跳转。");
    lv_label_set_text(notice, text);
    lv_obj_set_style_text_color(notice, lv_color_hex(status.error || status.resource_retained ? 0xffb6a0 : 0x9cb3c2), 0);
    lv_label_set_text(pause_label, status.state == LAWREC_PLAY_PAUSED ? "继续播放" : "暂停播放");
    enabled(pause_button, !leaving && status.active &&
            (status.state == LAWREC_PLAY_PLAYING || status.state == LAWREC_PLAY_PAUSED));
    enabled(back_button, !leaving);
}

static void action(lv_event_t *e)
{
    if (leaving) return;
    if ((intptr_t)lv_event_get_user_data(e)) {
        leaving = 1;
        lawrec_playback_stop();
    } else {
        lawrec_playback_status status;
        lawrec_playback_get_status(&status);
        if (status.active && (status.state == LAWREC_PLAY_PLAYING || status.state == LAWREC_PLAY_PAUSED))
            lawrec_playback_pause(status.state != LAWREC_PLAY_PAUSED);
    }
    update(NULL);
}

static lv_obj_t *panel(int y, int height)
{
    lv_obj_t *object = lv_obj_create(screen);
    lv_obj_remove_style_all(object);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(object, 0, y);
    lv_obj_set_size(object, 480, height);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x091722), 0);
    lv_obj_set_style_bg_grad_color(object, lv_color_hex(0x17303e), 0);
    lv_obj_set_style_bg_grad_dir(object, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    return object;
}

static lv_obj_t *label_at(lv_obj_t *parent, const char *text, int y, int height, int small)
{
    lv_obj_t *label = settings_label(parent, text, small);
    lv_obj_set_pos(label, 20, y);
    lv_obj_set_size(label, 440, height);
    return label;
}

void jump_to_scr_playback(void)
{
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_remove_style_all(screen);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0);
        /* Big-core video is 480x270 at y=200; neither panel may cover it. */
        lv_obj_t *header = panel(0, 188), *footer = panel(488, 312);
        label_at(header, "录像回放", 24, 30, 0);
        filename = label_at(header, "", 66, 48, 1);
        lv_label_set_long_mode(filename, LV_LABEL_LONG_DOT);
        state_label = label_at(header, "", 142, 30, 0);
        time_label = label_at(footer, "", 24, 30, 0);
        progress = lv_bar_create(footer);
        lv_obj_set_pos(progress, 20, 70); lv_obj_set_size(progress, 440, 8);
        lv_bar_set_range(progress, 0, 1000);
        lv_obj_set_style_bg_color(progress, lv_color_hex(0x29414f), 0);
        lv_obj_set_style_bg_color(progress, lv_color_hex(0x59d2c4), LV_PART_INDICATOR);
        notice = label_at(footer, "", 100, 116, 1);
        pause_button = settings_button(NULL, footer, "暂停播放", 1, action, 0);
        back_button = settings_button(NULL, footer, "返回文件", 0, action, 1);
        lv_obj_set_pos(pause_button, 20, 228); lv_obj_set_size(pause_button, 214, 64);
        lv_obj_set_pos(back_button, 246, 228); lv_obj_set_size(back_button, 214, 64);
        pause_label = lv_obj_get_child(pause_button, 0);
        lv_obj_t *buttons[] = {pause_button, back_button};
        group = lawrec_key_create_group(buttons, 2);
        timer = lv_timer_create(update, 250, NULL);
    }
    leaving = 0;
    lv_scr_load(screen);
    lawrec_key_set_group(group);
    lv_timer_resume(timer);
    update(NULL);
}
