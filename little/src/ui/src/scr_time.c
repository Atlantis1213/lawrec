#include "settings_ui.h"
#include "lawrec_time.h"
#include "lawrec_control.h"
#include <errno.h>

static settings_page page;
static lv_obj_t *clock_label, *input, *message;
static char confirmed[32];

static void refresh(lv_timer_t *timer)
{
    (void)timer;
    if (lv_scr_act()!=page.screen) return;
    char utc[32];
    int ret=lawrec_time_current_utc(utc,sizeof(utc));
    lv_label_set_text(clock_label,ret ? "读取系统时间失败" : utc);
}

static void apply(void *data)
{
    (void)data;
    int ret=lawrec_control_set_time_utc(confirmed);
    if (ret==-EBUSY) { settings_message(message,"请先停止推流、录像和回放，再校准时间。"); return; }
    char text[192];
    if (ret) snprintf(text,sizeof(text),"校准失败：%s",strerror(-ret));
    else snprintf(text,sizeof(text),"系统时间已更新，RTC 未更新。\n重启可能重置，尚未接入 NTP 自动校时。");
    settings_message(message,text); refresh(NULL);
}

static void action(lv_event_t *event)
{
    intptr_t cmd=(intptr_t)lv_event_get_user_data(event);
    if (!cmd) { jump_to_scr_settings(); return; }
    if (cmd==2) {
        char utc[32];
        if (!lawrec_time_current_utc(utc,sizeof(utc))) lv_textarea_set_text(input,utc);
        return;
    }
    int64_t seconds;
    const char *text=lv_textarea_get_text(input);
    if (lawrec_time_parse_utc(text,&seconds)) {
        settings_message(message,"时间格式无效。\n请输入 YYYY-MM-DD HH:MM:SS，年份 2020 到 2099。"); return;
    }
    snprintf(confirmed,sizeof(confirmed),"%s",text);
    char prompt[256];
    snprintf(prompt,sizeof(prompt),"%s UTC\n\n请输入 UTC，不是北京时间。\n北京时间 = UTC + 8 小时。\n仅校准系统时钟，不更新 RTC。",confirmed);
    settings_confirm(&page,"立即校准系统时间？",prompt,"校准时间",apply,NULL);
}

void jump_to_scr_time(void)
{
    if (!page.screen) {
        settings_page_create(&page,"系统时间","仅校准系统 UTC / 不保证重启保时",action);
        lv_obj_t *card=settings_card(page.body,"当前系统时间（UTC）",NULL);
        clock_label=settings_label(card,"",0);
        card=settings_card(page.body,"输入新时间","YYYY-MM-DD HH:MM:SS");
        input=settings_field(&page,card,"2026-10-02 00:00:00",19,"0123456789- :");
        settings_button(&page,card,"填入当前时间",0,action,2);
        card=settings_card(page.body,"时区提示","此处使用 UTC。北京时间需要减去 8 小时。\n例如北京时间 16:30，对应 UTC 08:30。");
        (void)card;
        card=settings_card(page.body,"操作提示",NULL);
        message=settings_label(card,"校准前请停止推流、录像与回放。\n校时不改变录像的单调时钟计时。",1);
        settings_button(&page,page.footer,"校准系统时间",1,action,1);
        lv_timer_create(refresh,1000,NULL);
    }
    char utc[32];
    if (lawrec_time_current_utc(utc,sizeof(utc))) snprintf(utc,sizeof(utc),"2026-01-01 00:00:00");
    lv_textarea_set_text(input,utc);
    lv_label_set_text(message,"校准前请停止推流、录像与回放。\n校时不改变录像的单调时钟计时。");
    lv_obj_scroll_to_y(page.body,0,LV_ANIM_OFF);
    settings_page_show(&page); refresh(NULL);
}
