#include "settings_ui.h"
#include "lawrec_settings.h"
#include "lawrec_storage.h"
#include "lawrec_config.h"

static settings_page page;
static lv_obj_t *message, *path, *active_label;
static char initial[LAWREC_RECORD_DIR_MAX+1], confirmed[LAWREC_RECORD_DIR_MAX+1];

static void leave(void *data) { (void)data; jump_to_scr_settings(); }

static void check_or_save(void *data)
{
    int save=(intptr_t)data;
    uint64_t available=0;
    const char *value=save ? confirmed : lv_textarea_get_text(path);
    int ret=lawrec_storage_validate_dir(value,&available);
    if (!ret && save) ret=lawrec_settings_record_dir_save(value);
    char text[256];
    if (ret) snprintf(text,sizeof(text),"%s失败：%s\n请检查挂载点、目录及写入权限。",save ? "保存" : "检查",strerror(-ret));
    else {
        if (save) snprintf(initial,sizeof(initial),"%s",value);
        snprintf(text,sizeof(text),"%s\n可用空间 %.0f MiB，预留 128 MiB。\n现有录像不会移动或删除。",
                 save ? "已保存，重启 UI 后生效。" : "目录可写，检查用临时文件已移除。",(double)available/1048576.0);
    }
    settings_message(message,text);
}

static void action(lv_event_t *event)
{
    intptr_t cmd=(intptr_t)lv_event_get_user_data(event);
    if (!cmd) {
        if (strcmp(initial,lv_textarea_get_text(path)))
            settings_confirm(&page,"放弃目录修改？","已保存的目录保持不变。","放弃修改",leave,NULL);
        else leave(NULL);
        return;
    }
    if (cmd==3) {
        lv_textarea_set_text(path,LAWREC_RECORD_DEFAULT_OUTPUT_DIR);
        settings_message(message,"默认目录已填入，尚未保存。\n目录必须已经存在，且位于持久存储。"); return;
    }
    settings_keyboard_hide(&page);
    const char *value=lv_textarea_get_text(path);
    if (lawrec_settings_record_dir_validate(value)) {
        settings_message(message,"请输入合法绝对路径，最多 80 字节。\n仅限字母、数字及 . _ - /，不能包含空目录或点目录。"); return;
    }
    if (cmd==1) { check_or_save(NULL); return; }
    snprintf(confirmed,sizeof(confirmed),"%s",value);
    char text[256];
    snprintf(text,sizeof(text),"%s\n\n检查可写性后保存，重启 UI 生效。\n不会创建目录、格式化或移动录像。",confirmed);
    settings_confirm(&page,"保存录像目录？",text,"保存目录",check_or_save,(void *)(intptr_t)1);
}

void jump_to_scr_storage(void)
{
    if (!page.screen) {
        settings_page_create(&page,"录像存储","修改目录不影响当前录像 / 不会格式化",action);
        lv_obj_t *card=settings_card(page.body,"当前保存位置",NULL);
        active_label=settings_label(card,"",1);
        card=settings_card(page.body,"保存目录","输入已存在的持久化目录，不接受符号链接");
        path=settings_field(&page,card,"/sharefs/lawrec_records",LAWREC_RECORD_DIR_MAX,
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-/");
        settings_button(&page,card,"填入默认目录",0,action,3);
        card=settings_card(page.body,"检查结果",NULL);
        message=settings_label(card,"保存前可以先检查空间与写入权限。\n保存后重启 UI 生效，环境变量覆盖优先。",1);
        card=settings_card(page.body,"数据保护", "不会移动、删除已有录像，也不会创建目录。\n空间不足时应安全停止录像，不覆盖旧文件。");
        (void)card;
        lv_obj_t *button=settings_button(&page,page.footer,"检查目录",0,action,1);
        lv_obj_set_width(button,0); lv_obj_set_flex_grow(button,1);
        button=settings_button(&page,page.footer,"保存目录",1,action,2);
        lv_obj_set_width(button,0); lv_obj_set_flex_grow(button,1);
    }
    char active[LAWREC_RECORD_DIR_MAX+1], text[256];
    int pending_error=lawrec_settings_record_dir_pending(initial,sizeof(initial));
    int active_error=lawrec_settings_record_dir_current(active,sizeof(active));
    lv_textarea_set_text(path,initial);
    snprintf(text,sizeof(text),"%s\n%s",active,active_error ? "当前配置异常，请检查日志。" : "本次运行使用此目录。");
    lv_label_set_text(active_label,text);
    lv_label_set_text(message,pending_error ? "已保存的目录配置异常，当前显示默认值。\n请检查日志与配置文件后再保存。" : "先检查空间和权限，再保存。\n保存后重启 UI 生效，环境变量覆盖优先。");
    lv_obj_scroll_to_y(page.body,0,LV_ANIM_OFF);
    settings_page_show(&page);
}
