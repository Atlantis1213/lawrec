#include "settings_ui.h"
#include "lawrec_settings.h"
#include "lawrec_network.h"
#include <errno.h>

static settings_page page;
static lv_obj_t *message, *policy_label, *mode_button, *static_card, *dhcp_hint, *fields[4], *prefix;
static unsigned dhcp, generation;
static int awaiting_apply;
static intptr_t confirmed_command;
static lawrec_ipv4_settings initial, confirmed;
LV_FONT_DECLARE(lawrec_font_cn_20);

static int draft_changed(void)
{
    if (dhcp!=initial.dhcp) return 1;
    /* Hidden static fields are irrelevant when the saved and draft modes are DHCP. */
    if (dhcp) return 0;
    if ((unsigned)lv_spinbox_get_value(prefix)!=initial.prefix) return 1;
    const char *values[]={initial.address,initial.gateway,initial.dns1,initial.dns2};
    for (unsigned i=0;i<4;++i)
        if (strcmp(lv_textarea_get_text(fields[i]),values[i])) return 1;
    return 0;
}

static void leave_ipv4(void *data)
{
    (void)data;
    jump_to_scr_network();
}

static void policy(void)
{
    lawrec_ipv4_settings active;
    int ret=lawrec_settings_ipv4_active(&active);
    char text[128];
    snprintf(text,sizeof(text),"当前运行：%s\n正在编辑：%s",ret ? "状态未知，请检查日志" : active.dhcp ? "自动获取 DHCP" : "静态地址",dhcp ? "自动获取 DHCP" : "静态地址");
    lv_label_set_text(policy_label,text);
    lv_label_set_text(lv_obj_get_child(mode_button,0),dhcp ? "自动获取 DHCP" : "静态地址");
    if (dhcp) {
        lv_obj_add_flag(static_card,LV_OBJ_FLAG_HIDDEN); lv_obj_clear_flag(dhcp_hint,LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(static_card,LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(dhcp_hint,LV_OBJ_FLAG_HIDDEN);
    }
}

static void poll_apply(lv_timer_t *timer)
{
    (void)timer;
    if (lv_scr_act()!=page.screen || !awaiting_apply || page.dialog) return;
    lawrec_network_snapshot current; lawrec_network_get(&current);
    if (current.busy || current.generation==generation) return;
    awaiting_apply=0;
    if (current.generation!=generation+1) { settings_message(message,"收到更新的网络结果，请检查当前状态。\n立即应用没有保存下次启动的配置。"); policy(); return; }
    char text[256];
    if (current.error) snprintf(text,sizeof(text),"应用失败：%s\n%s",current.error==-EADDRINUSE ? "IP 地址冲突" : strerror(-current.error),
             current.rollback_error ? "恢复失败，请使用串口检查。" : current.ipv4_changed ? "原策略已恢复，IP 可能改变。" : "尚未开始修改原策略。");
    else snprintf(text,sizeof(text),"已应用：%s\n仅确认本机设置，不保证远端可达。\n下次启动配置未保存，请单独保存。",current.ipv4[0] ? current.ipv4 : "请查看网络状态");
    settings_message(message,text); policy();
}

static void commit(void *data)
{
    (void)data;
    lawrec_network_snapshot current; lawrec_network_get(&current);
    if (current.busy) { settings_message(message,"网络操作尚未完成，请稍后再试。"); return; }
    int ret;
    if (confirmed_command==5) {
        ret=lawrec_network_ipv4_apply_async(&confirmed);
        awaiting_apply=!ret; generation=current.generation;
    } else {
        ret=lawrec_settings_ipv4_save(&confirmed);
        if (!ret) initial=confirmed;
    }
    settings_message(message,ret ? strerror(-ret) : confirmed_command==5 ?
                     "正在检查与应用 IP，冲突检查约需 4 到 7 秒。\n失败会尝试恢复原策略。" :
                     "已保存，下次开机时应用。\n当前 IP 与连接未修改。");
}

static void action(lv_event_t *event)
{
    intptr_t cmd=(intptr_t)lv_event_get_user_data(event);
    if (!cmd) {
        if (draft_changed())
            settings_confirm(&page,"放弃 IP 策略修改？","下次启动策略尚未保存。\n仅放弃输入，不会撤销已经应用的网络操作。",
                             "放弃修改",leave_ipv4,NULL);
        else leave_ipv4(NULL);
        return;
    }
    if (cmd==1) { dhcp=!dhcp; settings_keyboard_hide(&page); policy(); return; }
    if (cmd==2 || cmd==3) {
        if (cmd==2) lv_spinbox_decrement(prefix); else lv_spinbox_increment(prefix);
        return;
    }
    confirmed=(lawrec_ipv4_settings){.version=1,.dhcp=dhcp,.prefix=(unsigned)lv_spinbox_get_value(prefix)};
    if (!dhcp) {
        char *dest[]={confirmed.address,confirmed.gateway,confirmed.dns1,confirmed.dns2};
        for (unsigned i=0;i<4;++i) snprintf(dest[i],16,"%s",lv_textarea_get_text(fields[i]));
    }
    if (lawrec_settings_ipv4_validate(&confirmed)) {
        settings_message(message,"静态配置无效：IP 与 DNS1 必填，前缀 1 到 30。\n网关可空；若填写，需为同网段的不同主机。"); return;
    }
    lawrec_network_snapshot current; lawrec_network_get(&current);
    if (current.busy) { settings_message(message,"网络操作尚未完成，请稍后再试。"); return; }
    confirmed_command=cmd;
    char prompt[320];
    snprintf(prompt,sizeof(prompt),"%s%s%s\n\n%s",dhcp ? "自动获取 DHCP" : "静态地址：",dhcp ? "" : confirmed.address,
             dhcp ? "" : "（请确认地址未被占用）",cmd==5 ?
             "SSH 与推流可能断开。\n失败会尝试恢复原策略。\n不会保存下次启动配置，请准备串口。" :
             "下次开机 IP 可能改变。\n当前 SSH 与推流不变。\n请准备串口以便恢复。");
    settings_confirm(&page,cmd==5 ? "立即应用 IP 策略？" : "保存下次启动策略？",prompt,
                     cmd==5 ? "立即应用" : "保存策略",commit,NULL);
}

void jump_to_scr_ipv4(void)
{
    if (!page.screen) {
        settings_page_create(&page,"IPv4 地址设置","wlan0 / 下次启动与立即应用分开",action);
        lv_obj_t *card=settings_card(page.body,"当前与编辑状态",NULL);
        policy_label=settings_label(card,"",1);
        mode_button=settings_button(&page,card,"自动获取 DHCP",0,action,1);
        dhcp_hint=settings_card(page.body,"自动获取","由路由器分配 IP、网关与 DNS。\n默认推荐，无需填写静态地址。");
        static_card=settings_card(page.body,"静态地址","IP 与 DNS1 必填；网关与 DNS2 可留空");
        const char *names[]={"IP 地址","网关（可选）","DNS1","DNS2（可选）"};
        for (unsigned i=0;i<4;++i) {
            settings_label(static_card,names[i],1);
            fields[i]=settings_field(&page,static_card,"例如 192.168.123.74",15,"0123456789.");
        }
        settings_label(static_card,"子网前缀 /1 到 /30，通常为 /24",1);
        lv_obj_t *row=settings_row(static_card);
        lv_obj_t *button=settings_button(&page,row,"-",0,action,2); lv_obj_set_width(button,56);
        prefix=lv_spinbox_create(row); lv_obj_set_size(prefix,160,56); lv_obj_set_flex_grow(prefix,1);
        lv_spinbox_set_range(prefix,1,30); lv_spinbox_set_digit_format(prefix,2,0); lv_spinbox_set_step(prefix,1);
        settings_input_style(prefix);
        lv_obj_clear_flag(prefix,LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_style_bg_opa(prefix,LV_OPA_TRANSP,LV_PART_CURSOR);
        button=settings_button(&page,row,"+",0,action,3); lv_obj_set_width(button,56);
        card=settings_card(page.body,"生效方式",NULL);
        message=settings_label(card,"保存：下次开机使用，当前连接不变。\n立即应用：只修改当前连接，不自动保存。",1);
        button=settings_button(&page,page.footer,"保存策略",0,action,4);
        lv_obj_set_width(button,0); lv_obj_set_flex_grow(button,1);
        button=settings_button(&page,page.footer,"立即应用",1,action,5);
        lv_obj_set_width(button,0); lv_obj_set_flex_grow(button,1);
        lv_timer_create(poll_apply,200,NULL);
    }
    int ret=lawrec_settings_ipv4_pending(&initial);
    dhcp=initial.dhcp;
    const char *values[]={initial.address,initial.gateway,initial.dns1,initial.dns2};
    for (unsigned i=0;i<4;++i) lv_textarea_set_text(fields[i],values[i]);
    lv_spinbox_set_value(prefix,initial.prefix);
    policy();
    if (!awaiting_apply) lv_label_set_text(message,ret ?
            "已保存配置异常，显示 DHCP 默认值但未应用。\n请检查串口和日志，再决定是否覆盖。" :
            "保存：下次开机使用，当前连接不变。\n立即应用：只修改当前连接，不自动保存。");
    lv_obj_scroll_to_y(page.body,0,LV_ANIM_OFF);
    settings_page_show(&page); poll_apply(NULL);
}
