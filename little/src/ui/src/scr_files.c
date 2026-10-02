#include "settings_ui.h"
#include "../../common/lawrec_storage.h"
#include "../../control/include/lawrec_control.h"
#include "../../playback/lawrec_playback.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define FILES_PER_PAGE 4
LV_FONT_DECLARE(lawrec_font_cn_16);
static settings_page page;
static lv_obj_t *message, *details, *items[FILES_PER_PAGE], *newer_button, *older_button, *play_button, *delete_button;
static lawrec_recording_entry entries[FILES_PER_PAGE], confirmed_delete;
static size_t count, selected;
static int has_newer, has_older;

static void enabled(lv_obj_t *button, int value)
{
    if (value) lv_obj_clear_state(button,LV_STATE_DISABLED);
    else lv_obj_add_state(button,LV_STATE_DISABLED);
}

static void show_selection(void)
{
    char text[384];
    if (count) snprintf(text,sizeof(text),"%s\n大小 %.2f MiB\n仅支持 H.264 720P，音轨为可选 G.711。",
                        entries[selected].name,entries[selected].bytes/1048576.0);
    else snprintf(text,sizeof(text),"没有已完成的录像。\n正在录制的 .part 文件不会显示。\n可先进入拍摄并完成一段录像。");
    lv_label_set_text(details,text);
    for (size_t i=0;i<FILES_PER_PAGE;++i) {
        if (i==selected && i<count) lv_obj_add_state(items[i],LV_STATE_CHECKED);
        else lv_obj_clear_state(items[i],LV_STATE_CHECKED);
    }
    enabled(play_button,count && !lawrec_playback_active());
    enabled(delete_button,count && !lawrec_playback_active());
}

static void load_page(const char *anchor, int newer, int paging)
{
    int more=0;
    count=selected=0;
    const int ret=lawrec_storage_page(anchor,newer,entries,FILES_PER_PAGE,&count,&more);
    has_newer=!ret && count && (paging ? newer ? more : 1 : 0);
    has_older=!ret && count && (paging && newer ? 1 : more);
    uint64_t available=0;
    const int space=lawrec_storage_check(&available);
    lawrec_playback_status playback; lawrec_playback_get_status(&playback);
    char text[384];
    if (ret) snprintf(text,sizeof(text),"读取录像目录失败：%s（%d）\n请检查存储挂载与权限。",strerror(-ret),ret);
    else if (playback.resource_retained) snprintf(text,sizeof(text),"回放清理失败，操作已禁用。\n请用串口检查并恢复。\n显示恢复不能回收媒体。\n目录：%s",lawrec_storage_dir());
    else if (space) snprintf(text,sizeof(text),"目录：%s\n录像空间检查：%s\n已有文件仍可浏览，删除以操作结果为准。",lawrec_storage_dir(),strerror(-space));
    else snprintf(text,sizeof(text),"目录：%s\n剩余空间 %.0f MiB",lawrec_storage_dir(),available/1048576.0);
    lv_label_set_text(message,text);
    lv_obj_set_style_text_color(message,lv_color_hex(ret || space || playback.resource_retained ? 0xffb6a0 : 0x9cb3c2),0);
    for (size_t i=0;i<FILES_PER_PAGE;++i) {
        if (i>=count) { lv_obj_add_flag(items[i],LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(items[i],LV_OBJ_FLAG_HIDDEN);
        snprintf(text,sizeof(text),"%s\n%.2f MiB",entries[i].name,entries[i].bytes/1048576.0);
        lv_label_set_text(lv_obj_get_child(items[i],0),text);
    }
    enabled(newer_button,has_newer); enabled(older_button,has_older);
    show_selection();
    lv_obj_scroll_to_y(page.body,0,LV_ANIM_OFF);
}

static void select_file(lv_event_t *event)
{
    const size_t index=(size_t)(intptr_t)lv_event_get_user_data(event);
    if (index>=count) return;
    selected=index; show_selection();
    lv_obj_scroll_to_view_recursive(details,LV_ANIM_OFF);
}

static void delete_confirmed(void *unused)
{
    (void)unused;
    if (lawrec_playback_active()) { settings_message(message,"回放仍占用资源，不能删除。"); return; }
    const int ret=lawrec_storage_delete_matching(&confirmed_delete);
    load_page(NULL,0,0);
    char text[192];
    if (ret==-ESTALE) snprintf(text,sizeof(text),"文件已被替换或修改，未删除。\n请重新选择并确认。");
    else if (ret) snprintf(text,sizeof(text),"删除失败：%s（%d）\n请刷新目录并检查日志。",strerror(-ret),ret);
    else snprintf(text,sizeof(text),"已删除确认的文件。");
    settings_message(message,text);
}

static void action(lv_event_t *event)
{
    const intptr_t command=(intptr_t)lv_event_get_user_data(event);
    if (!command) { jump_to_scr_main(); return; }
    if (command==1) { load_page(NULL,0,0); return; }
    if ((command==2 || command==3) && count) {
        char anchor[128];
        snprintf(anchor,sizeof(anchor),"%s",entries[command==2 ? 0 : count-1].name);
        load_page(anchor,command==2,1); return;
    }
    if (!count || lawrec_playback_active()) { settings_message(message,"没有可操作文件，或回放仍占用资源。"); return; }
    if (command==4) {
        confirmed_delete=entries[selected];
        char text[320];
        snprintf(text,sizeof(text),"%s\n大小 %.2f MiB\n\n删除后无法撤销，请先导出需要保留的录像。",confirmed_delete.name,confirmed_delete.bytes/1048576.0);
        settings_confirm(&page,"删除这段录像？",text,"删除录像",delete_confirmed,NULL);
    } else if (command==5) {
        const int ret=lawrec_control_playback_start(entries[selected].name);
        if (!ret) { extern void jump_to_scr_playback(void); jump_to_scr_playback(); }
        else {
            char text[256];
            snprintf(text,sizeof(text),"无法播放：%s（%d）\n请先关闭预览、推流和录像；状态待确认时可检查设备维护。",strerror(ret<0 ? -ret : ret),ret);
            settings_message(message,text);
        }
    }
}

void jump_to_scr_files(void)
{
    if (!page.screen) {
        settings_page_create(&page,"录像文件","已完成文件 / 分页浏览与本地回放",action);
        lv_obj_t *card=settings_card(page.body,NULL,NULL);
        lv_obj_t *row=settings_row(card);
        message=settings_label(row,"",1);
        lv_obj_set_width(message,0); lv_obj_set_flex_grow(message,1);
        lv_obj_t *refresh=settings_button(&page,row,"刷新文件",0,action,1);
        lv_obj_set_width(refresh,112);
        card=settings_card(page.body,"选择录像","每页 4 个，按文件名倒序；下滑查看详情");
        for (size_t i=0;i<FILES_PER_PAGE;++i) {
            items[i]=settings_button(&page,card,"",0,select_file,i);
            lv_obj_set_height(items[i],64);
            lv_obj_t *label=lv_obj_get_child(items[i],0);
            lv_obj_set_width(label,LV_PCT(100));
            lv_label_set_long_mode(label,LV_LABEL_LONG_DOT);
            lv_obj_set_height(label,48);
            lv_obj_set_style_text_font(label,&lawrec_font_cn_16,0);
            lv_obj_set_style_bg_color(items[i],lv_color_hex(0x2b615f),LV_STATE_CHECKED);
        }
        row=settings_row(card);
        newer_button=settings_button(&page,row,"上一页",0,action,2);
        older_button=settings_button(&page,row,"下一页",0,action,3);
        lv_obj_set_width(newer_button,0); lv_obj_set_flex_grow(newer_button,1);
        lv_obj_set_width(older_button,0); lv_obj_set_flex_grow(older_button,1);
        card=settings_card(page.body,"已选文件",NULL);
        details=settings_label(card,"",1);
        card=settings_card(page.body,"导出录像",NULL);
        settings_label(card,"通过电脑端 SCP 从上方目录下载文件。\n当前没有 HTTP 下载服务，不会自动切换网络。",1);
        delete_button=settings_button(&page,page.footer,"删除录像",0,action,4);
        lv_obj_set_width(delete_button,144);
        play_button=settings_button(&page,page.footer,"播放录像",1,action,5);
        lv_obj_set_width(play_button,0); lv_obj_set_flex_grow(play_button,1);
    }
    load_page(NULL,0,0);
    settings_page_show(&page);
}
