#include "settings_ui.h"
#include "../../control/include/lawrec_control.h"
#include <errno.h>

static settings_page page;
static lv_obj_t *message, *recover;
static int busy, last_result, has_result;

void scr_maintenance_recovery_result(int result)
{
    busy=0; last_result=result; has_result=1;
    if (!message) return;
    lv_obj_clear_state(recover,LV_STATE_DISABLED);
    char text[256];
    if (!result) snprintf(text,sizeof(text),"显示已恢复，可重新进入拍摄或回放。\n这不代表遗留编码器已被回收。");
    else if (result==-EBUSY) snprintf(text,sizeof(text),"媒体仍占用资源，请先停止预览、推流与录像。\n清理失败的资源需用串口重启程序。");
    else snprintf(text,sizeof(text),"显示恢复失败：%s（%d）\n未解除资源保护，请检查双核日志后重试。",strerror(result<0 ? -result : result),result);
    lv_label_set_text(message,text);
    lv_obj_set_style_text_color(message,lv_color_hex(result ? 0xffc078 : 0x59d2c4),0);
}

static void confirmed_recovery(void *unused)
{
    (void)unused;
    if (busy) return;
    busy=1;
    lv_obj_add_state(recover,LV_STATE_DISABLED);
    lv_label_set_text(message,"正在恢复显示，请等待大核确认。\n不会自动开始推流、录像或回放。");
    const int ret=msg_send_cmd(MSG_CMD_DISPLAY_RECOVER);
    if (ret && busy) scr_maintenance_recovery_result(ret);
}

static void action(lv_event_t *event)
{
    if (!(intptr_t)lv_event_get_user_data(event)) { jump_to_scr_settings(); return; }
    if (busy) return;
    settings_confirm(&page,"恢复显示？",
        "将关闭遗留预览和回放图层。\n不会修改显示时序或触控参数。\n不会回收遗留编码器、解码器。\n媒体运行或清理失败时会拒绝操作。",
        "恢复显示",confirmed_recovery,NULL);
}

void jump_to_scr_maintenance(void)
{
    if (!page.screen) {
        settings_page_create(&page,"设备维护","异常恢复 / 操作前再次确认",action);
        lv_obj_t *card=settings_card(page.body,"显示状态",NULL);
        message=settings_label(card,"",1);
        card=settings_card(page.body,"显示恢复","用于 UI 重启后的遗留图层或图层切换失败");
        settings_label(card,"只恢复视频图层与预览绑定。\n不重建摄像头、显示连接器或媒体池。\n不会切换网络，也不会重启设备。",1);
        card=settings_card(page.body,"仍无法播放？",NULL);
        settings_label(card,"若编码或解码资源清理失败，请通过串口重启大小核业务程序。\n恢复后仍需检查真实预览与文件播放。",1);
        recover=settings_button(&page,page.footer,"恢复显示",1,action,1);
    }
    if (!busy && has_result) scr_maintenance_recovery_result(last_result);
    else if (!busy) lv_label_set_text(message,lawrec_control_preview_needs_close() ?
        "显示占用尚未关闭或状态待确认。\n可在停止媒体后尝试恢复。" : "当前没有未关闭的显示占用。");
    lv_obj_scroll_to_y(page.body,0,LV_ANIM_OFF);
    settings_page_show(&page);
}
