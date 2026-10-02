#include "settings_ui.h"
#include "lawrec_settings.h"
#include <errno.h>

static settings_page menu, media_page;
static lv_obj_t *menu_state, *message, *port, *bitrate, *fps_select, *audio_button, *segment_select, *segment_hint;
static unsigned segment_seconds, audio_enabled, frame_rate;
static const unsigned segment_choices[] = {0,60,180,300,600};
static int loading;
static lawrec_media_settings initial, confirmed;
LV_FONT_DECLARE(lawrec_font_cn_20);

static void menu_action(lv_event_t *e)
{
    switch ((intptr_t)lv_event_get_user_data(e)) {
    case 0: jump_to_scr_main(); break;
    case 1: jump_to_scr_media(); break;
    case 2: jump_to_scr_network(); break;
    case 3: jump_to_scr_storage(); break;
    case 4: jump_to_scr_time(); break;
    case 5: jump_to_scr_maintenance(); break;
    }
}

void jump_to_scr_settings(void)
{
    if (!menu.screen) {
        settings_page_create(&menu, "系统设置", "按功能分类，修改前可查看生效方式", menu_action);
        menu_state = settings_label(menu.body, "", 1);
        const char *titles[] = {"视频与录像", "网络连接", "录像存储", "系统时间"};
        const char *hints[] = {"画质、帧率、推流端口与分段", "WiFi 配网、IP 地址与 DNS", "保存目录与可用空间检查", "查看与校准 UTC 时间"};
        for (unsigned i = 0; i < 4; ++i) {
            lv_obj_t *button = settings_button(&menu, menu.body, titles[i], 0, menu_action, i+1);
            lv_obj_set_height(button, 104);
            lv_obj_t *title = lv_obj_get_child(button, 0);
            lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_t *hint = settings_label(button, hints[i], 1);
            lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        }
        lv_obj_t *maintenance = settings_button(&menu,menu.footer,"设备维护",0,menu_action,5);
        lv_obj_set_width(maintenance,168);
        lv_obj_t *hint = settings_label(menu.footer,"保存不等于立即应用\n各页面会说明生效方式",1);
        lv_obj_set_width(hint,0); lv_obj_set_flex_grow(hint,1);
    }
    char text[128];
    snprintf(text, sizeof(text), "当前视频 720P / %d FPS / %d kbps", lawrec_settings_frame_rate(), lawrec_settings_bitrate());
    lv_label_set_text(menu_state, text);
    settings_page_show(&menu);
}

static void button_title(lv_obj_t *button, const char *text)
{
    lv_label_set_text(lv_obj_get_child(button, 0), text);
}

static void refresh_values(void)
{
    lv_dropdown_set_selected(fps_select, frame_rate == 15 ? 0 : 1);
    button_title(audio_button, audio_enabled ? "音频：已开启" : "音频：已关闭");
    unsigned selected = 0;
    for (unsigned i = 0; i < sizeof(segment_choices)/sizeof(segment_choices[0]); ++i)
        if (segment_seconds == segment_choices[i]) selected = i;
    lv_dropdown_set_selected(segment_select, selected);
    lv_label_set_text(segment_hint, segment_seconds ?
        "每段独立保存，便于长时间录像和回放。" :
        "长时间录像建议开启分段，减少索引内存占用。");
}

static int read_draft(lawrec_media_settings *value)
{
    const char *text = lv_textarea_get_text(port);
    char *end;
    unsigned long number = strtoul(text, &end, 10);
    if (!text[0] || *end || number < 1024 || number > 65535) return -EINVAL;
    *value = (lawrec_media_settings){LAWREC_MEDIA_SETTINGS_VERSION, (unsigned)number,
             (unsigned)lv_spinbox_get_value(bitrate), segment_seconds, audio_enabled, frame_rate};
    return 0;
}

static void dirty(lv_event_t *event)
{
    (void)event;
    if (!loading) lv_label_set_text(message, "修改尚未保存。保存后重启 UI 生效。\n当前推流和录像不会被修改。");
}

static void leave_media(void *data) { (void)data; jump_to_scr_settings(); }

static void save_media(void *data)
{
    (void)data;
    int ret = lawrec_settings_media_save(&confirmed);
    char text[192];
    if (ret) snprintf(text,sizeof(text),"保存失败：%s\n请检查配置文件权限。",strerror(-ret));
    else { initial = confirmed; snprintf(text,sizeof(text),"已保存，重启 UI 后生效。\n当前视频参数保持不变。"); }
    settings_message(message,text);
}

static void media_action(lv_event_t *e)
{
    intptr_t cmd = (intptr_t)lv_event_get_user_data(e);
    if (!cmd) {
        lawrec_media_settings draft;
        if (read_draft(&draft) || memcmp(&draft,&initial,sizeof(draft)))
            settings_confirm(&media_page,"放弃未保存的修改？","已保存的配置不会改变。", "放弃修改",leave_media,NULL);
        else leave_media(NULL);
        return;
    }
    if (cmd == 1) frame_rate = lv_dropdown_get_selected(fps_select) == 0 ? 15 : 30;
    if (cmd == 2) audio_enabled = !audio_enabled;
    if (cmd == 3) {
        const unsigned selected = lv_dropdown_get_selected(segment_select);
        if (selected >= sizeof(segment_choices)/sizeof(segment_choices[0])) return;
        segment_seconds = segment_choices[selected];
    }
    if (cmd == 4) lv_spinbox_decrement(bitrate);
    if (cmd == 5) lv_spinbox_increment(bitrate);
    if (cmd == 6) {
        if (read_draft(&confirmed)) {
            settings_message(message,"端口无效，请输入 1024 到 65535。\n例如 RTSP 默认端口 8554。"); return;
        }
        char text[256];
        snprintf(text,sizeof(text),"720P / %u FPS / %u kbps\nRTSP 端口 %u，音频%s\n分段 %u 秒（0 为关闭）\n\n重启 UI 后应用，当前业务不变。",
                 confirmed.video_frame_rate,confirmed.video_bitrate_kbps,confirmed.rtsp_port,
                 confirmed.audio_enabled ? "开启" : "关闭",confirmed.record_segment_seconds);
        settings_confirm(&media_page,"保存视频设置？",text,"保存设置",save_media,NULL);
        return;
    }
    if (cmd == 7) {
        lv_textarea_set_text(port,"8554"); lv_spinbox_set_value(bitrate,4000);
        segment_seconds=0; audio_enabled=0; frame_rate=30;
    }
    refresh_values(); dirty(NULL);
}

void jump_to_scr_media(void)
{
    if (!media_page.screen) {
        settings_page_create(&media_page,"视频与录像","720P 固定档位 / 保存后重启 UI 生效",media_action);
        lv_obj_t *card = settings_card(media_page.body,NULL,NULL);
        message = settings_label(card,"",1);
        card = settings_card(media_page.body,"视频质量","帧率与码率影响画面流畅度及文件大小");
        fps_select = settings_dropdown(&media_page,card,
            "15 FPS\n30 FPS",media_action,1);
        settings_label(card,"码率（kbps）：1000 到 8000",1);
        lv_obj_t *row = settings_row(card);
        lv_obj_t *minus = settings_button(&media_page,row,"-",0,media_action,4);
        lv_obj_set_width(minus,56);
        bitrate = lv_spinbox_create(row);
        lv_obj_set_size(bitrate,LV_PCT(50),56); lv_obj_set_flex_grow(bitrate,1);
        lv_spinbox_set_range(bitrate,1000,8000); lv_spinbox_set_digit_format(bitrate,4,0);
        lv_spinbox_set_step(bitrate,500);
        settings_input_style(bitrate);
        /* Only +/- changes bitrate; tapping digits must not change the 500-kbps step. */
        lv_obj_clear_flag(bitrate,LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_style_bg_opa(bitrate,LV_OPA_TRANSP,LV_PART_CURSOR);
        lv_obj_add_event_cb(bitrate,dirty,LV_EVENT_VALUE_CHANGED,NULL);
        lv_obj_t *plus = settings_button(&media_page,row,"+",0,media_action,5);
        lv_obj_set_width(plus,56);
        card = settings_card(media_page.body,"RTSP 端口","点按数字直接输入，默认 8554");
        port = settings_field(&media_page,card,"8554",5,"0123456789");
        lv_obj_add_event_cb(port,dirty,LV_EVENT_VALUE_CHANGED,NULL);
        card = settings_card(media_page.body,"录像选项","音频为 G.711A / 8 kHz，默认关闭");
        audio_button = settings_button(&media_page,card,"音频：已关闭",0,media_action,2);
        settings_label(card,"录像分段",0);
        segment_select = settings_dropdown(&media_page,card,
            "不分段\n每 1 分钟\n每 3 分钟\n每 5 分钟\n每 10 分钟",media_action,3);
        segment_hint = settings_label(card,"",1);
        lv_obj_t *defaults = settings_button(&media_page,media_page.footer,"恢复默认",0,media_action,7);
        lv_obj_set_width(defaults,144);
        lv_obj_t *save = settings_button(&media_page,media_page.footer,"保存设置",1,media_action,6);
        lv_obj_set_width(save,0); lv_obj_set_flex_grow(save,1);
    }
    loading=1;
    int ret = lawrec_settings_media_pending(&initial);
    char text[256], number[16];
    snprintf(number,sizeof(number),"%u",initial.rtsp_port);
    lv_textarea_set_text(port,number); lv_spinbox_set_value(bitrate,initial.video_bitrate_kbps);
    segment_seconds=initial.record_segment_seconds; audio_enabled=initial.audio_enabled; frame_rate=initial.video_frame_rate;
    refresh_values(); loading=0;
    snprintf(text,sizeof(text),"当前运行：%d FPS / %d kbps / 端口 %d\n%s",lawrec_settings_frame_rate(),lawrec_settings_bitrate(),lawrec_settings_port(),
             ret ? "已保存配置异常，显示默认值；请检查后再保存。" : "下方为已保存配置，可修改后重新保存。");
    lv_label_set_text(message,text);
    lv_obj_scroll_to_y(media_page.body,0,LV_ANIM_OFF);
    settings_page_show(&media_page);
}
