#include "settings_ui.h"
#include "lawrec_settings.h"
#include "lawrec_network.h"
#include <errno.h>

static settings_page page;
static lv_obj_t *ssid, *password, *message, *connection_message, *result_card;
static lv_obj_t *lease_message, *scan_message, *hotspots, *password_button;
static unsigned generation, scan_generation;
static uint32_t last_status_refresh;
static lawrec_network_snapshot scan;
static char confirmed_ssid[33], confirmed_password[64];
static char saved_ssid[33];
static intptr_t confirmed_command;
static int show_password;
LV_FONT_DECLARE(lawrec_font_cn_20);

static void clear_confirmed_password(void)
{
    memset(confirmed_password,0,sizeof(confirmed_password));
}

static void leave_network(void *data)
{
    (void)data;
    lv_textarea_set_text(ssid,saved_ssid);
    lv_textarea_set_text(password,"");
    clear_confirmed_password();
    show_password=0;
    lv_textarea_set_password_mode(password,true);
    lv_label_set_text(lv_obj_get_child(password_button,0),"显示密码");
    jump_to_scr_settings();
}

static void operation_message(const char *text)
{
    lv_obj_clear_flag(result_card,LV_OBJ_FLAG_HIDDEN);
    settings_message(message,text);
}

static void status(const lawrec_network_snapshot *state)
{
    char text[512], lease[192];
    if (state->dhcp_error == -ETIMEDOUT)
        snprintf(lease,sizeof(lease),"DHCP 租约已过期，请重新获取 IP。");
    else if (state->dhcp_error == -EINPROGRESS)
        snprintf(lease,sizeof(lease),"DHCP 正在配置，尚无有效租约。");
    else if (state->dhcp_error == -EADDRINUSE)
        snprintf(lease,sizeof(lease),"地址冲突，租约未生效；请检查网络。");
    else if (state->dhcp_error)
        snprintf(lease,sizeof(lease),"DHCP 状态异常：%s\n请检查日志或重新获取 IP。",strerror(-state->dhcp_error));
    else if (state->dhcp_bound)
        snprintf(lease,sizeof(lease),"DHCP 有效，剩余 %u 秒 / 租约 %u 秒",state->lease_remaining_seconds,state->lease_seconds);
    else if (state->dhcp_pid)
        snprintf(lease,sizeof(lease),"DHCP 客户端运行中，尚无有效租约。");
    else snprintf(lease,sizeof(lease),"DHCP 未运行；静态地址请查看 IPv4 设置。");
    if (state->busy) snprintf(text,sizeof(text),"网络操作进行中，请等待结果。\n当前地址：%s",state->ipv4[0] ? state->ipv4 : "未获取");
    else if (state->error) snprintf(text,sizeof(text),"操作失败：%s\n%s",strerror(-state->error),
              state->rollback_error ? "恢复失败，请用串口检查网络与日志。" : "请刷新状态，确认连接后再重试。");
    else snprintf(text,sizeof(text),"WiFi：%s\nIP：%s%s",state->ssid[0] ? state->ssid : "未连接",
              state->ipv4[0] ? state->ipv4 : "未获取",
              state->connection_changed ? "\n已切换连接；下次启动配置需单独保存。" : "");
    // Passive connection refresh must not erase a save/validation result.
    lv_label_set_text(connection_message,text);
    lv_label_set_text(lease_message,lease);
    lv_obj_set_style_text_color(lease_message,lv_color_hex(
        state->dhcp_error && state->dhcp_error!=-EINPROGRESS ? 0xffc078 :
        state->dhcp_bound ? 0x59d2c4 : 0x9cb3c2),0);
}

static void show_hotspots(void)
{
    char options[1536] = "选择热点";
    size_t used = strlen(options);
    for (int i=0;i<scan.count && i<LAWREC_WIFI_AP_MAX;++i) {
        char name[33]; snprintf(name,sizeof(name),"%s",scan.aps[i].ssid);
        for (char *p=name;*p;++p) if (*p=='\n' || *p=='\r') *p=' ';
        int n = snprintf(options+used,sizeof(options)-used,"\n%s (%d dBm)",name[0] ? name : "隐藏热点",scan.aps[i].signal_dbm);
        if (n<0 || (size_t)n>=sizeof(options)-used) break;
        used += n;
    }
    lv_dropdown_set_options(hotspots,scan.count ? options : "未发现可显示的热点");
    lv_dropdown_set_selected(hotspots,0);
    if (scan.count) lv_obj_clear_state(hotspots,LV_STATE_DISABLED);
    else lv_obj_add_state(hotspots,LV_STATE_DISABLED);
}

static void poll_network(lv_timer_t *timer)
{
    (void)timer;
    if (lv_scr_act()!=page.screen || page.dialog) return;
    lawrec_network_snapshot next;
    lawrec_network_get(&next);
    if (next.busy) return;
    if (next.generation!=generation) {
        generation=next.generation;
        if (!next.error && next.scan_result && next.scan_generation!=scan_generation) {
            scan_generation=next.scan_generation;
            scan=next; show_hotspots();
            lv_label_set_text(scan_message,next.count ? "扫描完成，选择热点可填入名称。\n输入密码后，再选择连接或保存。" : "没有发现热点，请重试或手动输入名称。");
        }
        status(&next);
    }
    if (lv_tick_elaps(last_status_refresh)>=5000) {
        last_status_refresh=lv_tick_get();
        // STATUS is passive and asynchronous; never auto-scan/join/renew.
        lawrec_network_refresh_async(0);
    }
}

static void choose_ap(lv_event_t *event)
{
    (void)event;
    unsigned selected=lv_dropdown_get_selected(hotspots);
    if (!selected) return;
    --selected;
    if (selected>=(unsigned)scan.count || selected>=LAWREC_WIFI_AP_MAX) return;
    lv_textarea_set_text(ssid,scan.aps[selected].ssid);
    lv_textarea_set_text(password,"");
    lv_label_set_text(scan_message,"已填入热点名称，请输入 WPA2 密码。\n尚未切换当前连接。");
}

static void commit(void *data)
{
    (void)data;
    lawrec_network_snapshot current; lawrec_network_get(&current);
    if (current.busy) { operation_message("网络操作尚未完成，请稍后再试。"); return; }
    int ret;
    if (confirmed_command==7) ret=lawrec_network_connect_async(confirmed_ssid,confirmed_password);
    else if (confirmed_command==6) ret=lawrec_network_renew_async();
    else ret=lawrec_settings_save_wifi(confirmed_ssid,confirmed_password);
    /* Do not keep a second plaintext password after the request has copied it. */
    memset(confirmed_password,0,sizeof(confirmed_password));
    if (ret) { char text[192]; snprintf(text,sizeof(text),"操作失败：%s",strerror(-ret)); operation_message(text); return; }
    if (confirmed_command==2) {
        snprintf(saved_ssid,sizeof(saved_ssid),"%s",confirmed_ssid);
        operation_message("已保存，下次启动时使用。\n当前 WiFi 与 IP 不变。");
        lv_textarea_set_text(password,"");
    } else {
        lv_obj_add_flag(result_card,LV_OBJ_FLAG_HIDDEN);
        settings_message(connection_message,"正在连接或获取 IP，请等待结果。\n失败会尝试恢复原连接。");
    }
}

static void action(lv_event_t *event)
{
    intptr_t cmd=(intptr_t)lv_event_get_user_data(event);
    if (!cmd) {
        if (strcmp(saved_ssid,lv_textarea_get_text(ssid)) || lv_textarea_get_text(password)[0])
            settings_confirm(&page,"放弃 WiFi 输入？","热点名称或密码尚未保存。\n仅放弃输入，不会断开或回滚当前网络。",
                             "放弃修改",leave_network,NULL);
        else leave_network(NULL);
        return;
    }
    if (cmd==8) { jump_to_scr_ipv4(); return; }
    if (cmd==9) {
        show_password=!show_password;
        lv_textarea_set_password_mode(password,!show_password);
        lv_label_set_text(lv_obj_get_child(password_button,0),show_password ? "隐藏密码" : "显示密码");
        return;
    }
    if (cmd==1 || cmd==3) {
        last_status_refresh=lv_tick_get();
        int ret=lawrec_network_refresh_async(cmd==3);
        if (cmd==3) settings_message(scan_message,ret ? strerror(-ret) : "正在扫描热点，请稍候。");
        else settings_message(connection_message,ret ? strerror(-ret) : "正在刷新连接状态，请稍候。");
        return;
    }
    lawrec_network_snapshot current; lawrec_network_get(&current);
    if (current.busy) { operation_message("网络操作尚未完成，请稍后再试。"); return; }
    if ((cmd==2 || cmd==7) && strlen(lv_textarea_get_text(ssid))>32) {
        operation_message("热点名称最多 32 字节，中文通常占 3 字节。\n请缩短名称，不能截断后连接。"); return;
    }
    confirmed_command=cmd;
    snprintf(confirmed_ssid,sizeof(confirmed_ssid),"%s",lv_textarea_get_text(ssid));
    snprintf(confirmed_password,sizeof(confirmed_password),"%s",lv_textarea_get_text(password));
    if (cmd==2 || cmd==7) {
        size_t length=strlen(confirmed_password);
        if (!confirmed_ssid[0] || length<8 || length>63 || strchr(confirmed_password,'"') || strchr(confirmed_password,'\\') ||
            strchr(confirmed_ssid,'"') || strchr(confirmed_ssid,'\\')) {
            memset(confirmed_password,0,sizeof(confirmed_password));
            operation_message("名称不能为空；WPA2 密码需 8 到 63 个 ASCII 字符。\n名称与密码不能包含引号或反斜杠。"); return;
        }
    }
    if (cmd==7) settings_confirm(&page,"立即连接此 WiFi？","SSH 与推流可能断开。\n失败会尝试恢复原连接。\n此操作不会保存下次启动的配置。","连接",commit,NULL);
    else if (cmd==6) settings_confirm(&page,"重新获取 IP？","SSH 与推流可能断开。\n完成后请检查新的 IP 地址。","获取 IP",commit,NULL);
    else settings_confirm(&page,"保存 WiFi 配置？","将替换所有已保存的 WiFi。\n下次启动时使用，当前连接不变。\nIP 策略在 IPv4 页面单独设置。","保存",commit,NULL);
}

void jump_to_scr_network(void)
{
    if (!page.screen) {
        settings_page_create(&page,"网络连接","支持 WPA2-PSK / 保存与立即连接分开",action);
        page.dialog_closed_cb=clear_confirmed_password;
        lv_obj_t *card=settings_card(page.body,"当前连接",NULL);
        connection_message=settings_label(card,"",1);
        lease_message=settings_label(card,"",1);
        card=settings_card(page.body,"选择热点",NULL);
        scan_message=settings_label(card,"扫描不会改变当前网络",1);
        lv_obj_t *row=settings_row(card);
        lv_obj_t *button=settings_button(&page,row,"扫描热点",0,action,3);
        lv_obj_set_width(button,0); lv_obj_set_flex_grow(button,1);
        button=settings_button(&page,row,"刷新状态",0,action,1);
        lv_obj_set_width(button,0); lv_obj_set_flex_grow(button,1);
        hotspots=settings_dropdown(&page,card,"未发现可显示的热点",choose_ap,0);
        show_hotspots();
        card=settings_card(page.body,"连接信息","也可手动输入热点名称与密码");
        settings_label(card,"热点名称 SSID",1);
        ssid=settings_field(&page,card,"如 Lushan-Lab",32,NULL);
        settings_label(card,"WPA2 密码",1);
        password=settings_field(&page,card,"8 到 63 个 ASCII 字符",63,NULL);
        lv_textarea_set_password_mode(password,true);
        password_button=settings_button(&page,card,"显示密码",0,action,9);
        result_card=settings_card(page.body,"操作结果",NULL);
        message=settings_label(result_card,"",1);
        lv_obj_add_flag(result_card,LV_OBJ_FLAG_HIDDEN);
        card=settings_card(page.body,"连接维护","会改变网络的操作都需要再次确认");
        settings_button(&page,card,"IPv4 地址设置",0,action,8);
        settings_button(&page,card,"重新获取 IP",0,action,6);
        button=settings_button(&page,page.footer,"保存配置",0,action,2);
        lv_obj_set_width(button,0); lv_obj_set_flex_grow(button,1);
        button=settings_button(&page,page.footer,"立即连接",1,action,7);
        lv_obj_set_width(button,0); lv_obj_set_flex_grow(button,1);
        lv_timer_create(poll_network,200,NULL);
    }
    show_password=0; lv_textarea_set_password_mode(password,true);
    lv_label_set_text(lv_obj_get_child(password_button,0),"显示密码");
    lv_obj_add_flag(result_card,LV_OBJ_FLAG_HIDDEN);
    lawrec_network_snapshot current; lawrec_network_get(&current); generation=current.generation;
    if (!current.busy && !current.error && current.scan_result && current.scan_generation!=scan_generation) {
        scan_generation=current.scan_generation; scan=current; show_hotspots();
    }
    status(&current);
    lv_obj_scroll_to_y(page.body,0,LV_ANIM_OFF);
    settings_page_show(&page);
    last_status_refresh=lv_tick_get();
    // A passive first read is safe; entering this page must never scan or join.
    if (!current.busy && !lawrec_network_refresh_async(0))
        lv_label_set_text(connection_message,"正在读取连接状态，请稍候。");
}
