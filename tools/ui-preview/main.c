/* Exact production pages and LVGL renderer; hardware/services are fixture-only. */
#include "ui_common.h"
#include "key_proc.h"
#include "lawrec_settings.h"
#include "lawrec_network.h"
#include "lawrec_storage.h"
#include "lawrec_time.h"
#include "../../little/src/playback/lawrec_playback.h"
#include "src/extra/libs/png/lodepng.h"
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>

extern void jump_to_scr_settings(void), jump_to_scr_network(void), jump_to_scr_ipv4(void);
extern void jump_to_scr_storage(void), jump_to_scr_time(void);
extern void jump_to_scr_media(void) __attribute__((weak));
extern void jump_to_scr_maintenance(void);
extern void jump_to_scr_files(void), jump_to_scr_playback(void);
lv_ui_t lv_ui;
static uint32_t ticks;
static unsigned char pixels[480 * 800 * 4];
static lv_color_t draw[480 * 100];
static lawrec_media_settings media = {2, 8554, 4000, 0, 0, 30};
static lawrec_network_snapshot network = {.generation = 1, .state = "COMPLETED", .ssid = "Lushan-Lab", .ipv4 = "192.168.123.74", .dhcp_bound = 1, .lease_seconds = 3600, .lease_remaining_seconds = 2800};
static lawrec_ipv4_settings pending_ipv4 = {.version=1,.dhcp=1,.prefix=24};
static int media_saves, live_requests, wifi_saves, ipv4_saves, directory_saves;
static int recovery_requests, recovery_request_error;
static lv_group_t *key_group;
static lawrec_playback_status playback;
static int file_error, empty_files, deleted_index = -1, deletes, delete_error, play_requests, stop_requests;
static char deleted_name[128];
uint32_t custom_tick_get(void) { return ticks; }
void jump_to_scr_main(void) { jump_to_scr_settings(); }
lv_group_t *lawrec_key_create_group(lv_obj_t **items, size_t count)
{
    lv_group_t *group = lv_group_create();
    for (size_t i = 0; i < count; ++i) lv_group_add_obj(group, items[i]);
    return group;
}
void lawrec_key_set_group(lv_group_t *group) { key_group = group; }
int lawrec_settings_port(void) { return 8554; }
int lawrec_settings_bitrate(void) { return 4000; }
int lawrec_settings_frame_rate(void) { return 30; }
int lawrec_settings_segment_seconds(void) { return 0; }
int lawrec_settings_audio_enabled(void) { return 0; }
void lawrec_settings_media_current(lawrec_media_settings *value) { *value = (lawrec_media_settings){2,8554,4000,0,0,30}; }
int lawrec_settings_media_pending(lawrec_media_settings *value) { *value = media; return 0; }
int lawrec_settings_media_save(const lawrec_media_settings *value) { media = *value; ++media_saves; return 0; }
int lawrec_network_addresses(char *value, size_t size) { snprintf(value,size,"wlan0: 192.168.123.74"); return 0; }
int lawrec_settings_record_dir_current(char *value, size_t size) { snprintf(value,size,"/sharefs/lawrec_records"); return 0; }
int lawrec_settings_record_dir_pending(char *value, size_t size) { return lawrec_settings_record_dir_current(value,size); }
int lawrec_settings_record_dir_validate(const char *value) { return value[0] == '/' ? 0 : -EINVAL; }
int lawrec_settings_record_dir_save(const char *value) { (void)value; ++directory_saves; return 0; }
int lawrec_storage_validate_dir(const char *value, uint64_t *free) { (void)value; *free = 4096ULL*1024*1024; return 0; }
const char *lawrec_storage_dir(void) { return "/sharefs/lawrec_records"; }
int lawrec_storage_check(uint64_t *free) { if (free) *free = 4096ULL*1024*1024; return 0; }
static lawrec_recording_entry fixture_entry(int index)
{
    lawrec_recording_entry entry = {.bytes=20*1024*1024, .device=1, .inode=index+1};
    snprintf(entry.name, sizeof(entry.name), "lawrec_20261002_0830%02d_abcdef0123456789.mp4", 9-index);
    return entry;
}
int lawrec_storage_page(const char *anchor, int newer, lawrec_recording_entry *entries,
                        size_t capacity, size_t *count, int *more)
{
    *count=0; *more=0;
    if (file_error) return file_error;
    lawrec_recording_entry candidates[9]; size_t found=0;
    for (int i=0; i<9 && !empty_files; ++i) {
        lawrec_recording_entry entry=fixture_entry(i);
        if (i==deleted_index) continue;
        if (anchor && *anchor && (newer ? strcmp(entry.name,anchor)<=0 : strcmp(entry.name,anchor)>=0)) continue;
        candidates[found++]=entry;
    }
    *more=found>capacity;
    size_t begin=newer && found>capacity ? found-capacity : 0;
    while (begin<found && *count<capacity) entries[(*count)++]=candidates[begin++];
    return 0;
}
int lawrec_storage_delete_matching(const lawrec_recording_entry *entry)
{
    if (delete_error) return delete_error;
    for (int i=0; i<9; ++i) {
        lawrec_recording_entry candidate=fixture_entry(i);
        if (!strcmp(entry->name,candidate.name)) {
            assert(entry->inode==candidate.inode && entry->bytes==candidate.bytes);
            deleted_index=i; ++deletes;
            snprintf(deleted_name,sizeof(deleted_name),"%s",entry->name);
            return 0;
        }
    }
    return -ENOENT;
}
void lawrec_playback_get_status(lawrec_playback_status *status) { *status=playback; }
int lawrec_playback_active(void) { return playback.active || playback.resource_retained; }
int lawrec_playback_start(const char *name)
{
    ++play_requests;
    playback=(lawrec_playback_status){.state=LAWREC_PLAY_STARTING,.active=1};
    snprintf(playback.filename,sizeof(playback.filename),"%s",name); return 0;
}
int lawrec_control_playback_start(const char *name) { return lawrec_playback_start(name); }
void lawrec_playback_pause(int pause) { playback.state=pause ? LAWREC_PLAY_PAUSED : LAWREC_PLAY_PLAYING; }
void lawrec_playback_stop(void) { ++stop_requests; playback.state=LAWREC_PLAY_STOPPING; }
int lawrec_settings_ipv4_active(lawrec_ipv4_settings *value) { *value = (lawrec_ipv4_settings){.version=1,.dhcp=1,.prefix=24}; return 0; }
int lawrec_settings_ipv4_pending(lawrec_ipv4_settings *value) { *value=pending_ipv4; return 0; }
int lawrec_settings_ipv4_validate(const lawrec_ipv4_settings *value) { return !value->dhcp && !value->address[0] ? -EINVAL : 0; }
int lawrec_settings_ipv4_save(const lawrec_ipv4_settings *value) { pending_ipv4=*value; ++ipv4_saves; return 0; }
int lawrec_settings_save_wifi(const char *ssid, const char *password) { (void)ssid; (void)password; ++wifi_saves; return 0; }
void lawrec_network_get(lawrec_network_snapshot *value) { *value = network; }
int lawrec_network_refresh_async(int scan)
{
    network.scan_result = scan;
    if (scan) {
        network.count = 2;
        snprintf(network.aps[0].ssid,sizeof(network.aps[0].ssid),"Lushan-Lab");
        snprintf(network.aps[1].ssid,sizeof(network.aps[1].ssid),"Lawrec-Test");
        network.aps[0].signal_dbm = -42; network.aps[1].signal_dbm = -65;
    }
    ++network.generation;
    if (scan) network.scan_generation = network.generation;
    return 0;
}
int lawrec_network_connect_async(const char *ssid, const char *password) { (void)ssid; (void)password; ++live_requests; return 0; }
int lawrec_network_renew_async(void) { ++live_requests; return 0; }
int lawrec_network_ipv4_apply_async(const lawrec_ipv4_settings *settings) { (void)settings; ++live_requests; return 0; }
int lawrec_time_current_utc(char *text, size_t size) { snprintf(text,size,"2026-10-02 08:30:00"); return 0; }
int lawrec_time_parse_utc(const char *text, int64_t *seconds) { *seconds=0; return strlen(text)==19 ? 0 : -EINVAL; }
int lawrec_control_set_time_utc(const char *text) { (void)text; ++live_requests; return 0; }
int lawrec_control_preview_needs_close(void) { return 1; }
int msg_send_cmd(uint32_t command)
{
    assert(command==MSG_CMD_DISPLAY_RECOVER);
    ++recovery_requests; return recovery_request_error;
}

static void flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color)
{
    for (int y=area->y1; y<=area->y2; ++y) for (int x=area->x1; x<=area->x2; ++x) {
        assert(x>=0 && x<480 && y>=0 && y<800);
        unsigned char *p = &pixels[(y*480+x)*4];
        p[0]=color->ch.red; p[1]=color->ch.green; p[2]=color->ch.blue; p[3]=255;
        ++color;
    }
    lv_disp_flush_ready(drv);
}
static void render(const char *dir, const char *name)
{
    lv_obj_update_layout(lv_scr_act());
    for (int i=0; i<4; ++i) { ticks+=40; lv_timer_handler(); }
    lv_refr_now(NULL);
    char path[512]; snprintf(path,sizeof(path),"%s/%s.png",dir,name);
    unsigned char *png = NULL;
    size_t png_size = 0;
    assert(lodepng_encode32(&png,&png_size,pixels,480,800)==0);
    FILE *file = fopen(path,"wb");
    assert(file && fwrite(png,1,png_size,file)==png_size);
    assert(fclose(file)==0);
    lv_mem_free(png);
    printf("Rendered real LVGL: %s (fixture service state)\n",path);
}

static lv_obj_t *find_type(lv_obj_t *root, const lv_obj_class_t *type)
{
    if (lv_obj_check_type(root,type)) return root;
    for (unsigned i=0;i<lv_obj_get_child_cnt(root);++i) {
        lv_obj_t *found=find_type(lv_obj_get_child(root,i),type);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_dropdown(lv_obj_t *root, unsigned options)
{
    if (lv_obj_check_type(root,&lv_dropdown_class) && lv_dropdown_get_option_cnt(root)==options) return root;
    for (unsigned i=0;i<lv_obj_get_child_cnt(root);++i) {
        lv_obj_t *found=find_dropdown(lv_obj_get_child(root,i),options);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_button(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root,&lv_btn_class)) {
        lv_obj_t *label=lv_obj_get_child(root,0);
        if (label && lv_obj_check_type(label,&lv_label_class) && !strcmp(lv_label_get_text(label),text)) return root;
    }
    for (unsigned i=0;i<lv_obj_get_child_cnt(root);++i) {
        lv_obj_t *found=find_button(lv_obj_get_child(root,i),text);
        if (found) return found;
    }
    return NULL;
}

static void click(lv_obj_t *root, const char *text)
{
    lv_obj_t *button=find_button(root,text);
    assert(button && !lv_obj_has_state(button,LV_STATE_DISABLED));
    lv_event_send(button,LV_EVENT_CLICKED,NULL);
}

static lv_obj_t *find_field(lv_obj_t *root, const char *placeholder)
{
    if (lv_obj_check_type(root,&lv_textarea_class) &&
        !strcmp(lv_textarea_get_placeholder_text(root),placeholder)) return root;
    for (unsigned i=0;i<lv_obj_get_child_cnt(root);++i) {
        lv_obj_t *found=find_field(lv_obj_get_child(root,i),placeholder);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_copy(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root,&lv_label_class) && strstr(lv_label_get_text(root),text)) return root;
    for (unsigned i=0;i<lv_obj_get_child_cnt(root);++i) {
        lv_obj_t *found=find_copy(lv_obj_get_child(root,i),text);
        if (found) return found;
    }
    return NULL;
}

static void check_keyboard(const char *dir, const char *name, const char *placeholder)
{
    lv_obj_t *field=find_field(lv_scr_act(),placeholder);
    assert(field);
    lv_event_send(field,LV_EVENT_CLICKED,NULL);
    render(dir,name);
    lv_obj_t *keyboard=find_type(lv_scr_act(),&lv_keyboard_class);
    assert(keyboard && !lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN));
    lv_area_t area, key_area;
    lv_obj_get_coords(field,&area); lv_obj_get_coords(keyboard,&key_area);
    assert(key_area.x1==0 && key_area.y1==520 && key_area.x2==479 && key_area.y2==799);
    assert(area.y1>=104 && area.y2<key_area.y1);
    unsigned button=0;
    while (strcmp(lv_btnmatrix_get_btn_text(keyboard,button),LV_SYMBOL_OK)) {
        ++button; assert(button<64);
    }
    /* Exercise the real key handler, not just our READY listener. */
    lv_btnmatrix_set_selected_btn(keyboard,button);
    lv_event_send(keyboard,LV_EVENT_VALUE_CHANGED,NULL);
    assert(lv_obj_has_flag(keyboard,LV_OBJ_FLAG_HIDDEN));
}

static void check_workflows(const char *dir)
{
    jump_to_scr_media();
    lv_obj_t *fps=find_dropdown(lv_scr_act(),2);
    assert(fps && lv_dropdown_get_selected(fps)==1);
    lv_dropdown_open(fps); render(dir,"media-fps-options"); lv_dropdown_close(fps);
    lv_obj_t *segment=find_dropdown(lv_scr_act(),5);
    assert(segment && lv_dropdown_get_option_cnt(segment)==5);
    lv_obj_update_layout(lv_scr_act());
    lv_obj_t *hint=lv_obj_get_child(lv_obj_get_parent(segment),-1);
    lv_obj_scroll_to_view_recursive(hint,LV_ANIM_OFF);
    render(dir,"media-record-options");
    lv_area_t segment_area;
    lv_obj_get_coords(segment,&segment_area);
    assert(segment_area.y1 >= 104 && segment_area.y2 < 684);
    lv_obj_get_coords(hint,&segment_area);
    assert(segment_area.y1 >= 104 && segment_area.y2 < 684);
    lv_dropdown_open(segment); render(dir,"media-segment-options"); lv_dropdown_close(segment);
    lv_dropdown_set_selected(segment,1); lv_event_send(segment,LV_EVENT_VALUE_CHANGED,NULL);
    assert(!media_saves && !live_requests);
    lv_dropdown_set_selected(fps,0); lv_event_send(fps,LV_EVENT_VALUE_CHANGED,NULL);
    lv_group_t *original_group=key_group;
    click(lv_scr_act(),"保存设置");
    assert(key_group!=original_group && lv_obj_get_child_cnt(lv_layer_top())==1);
    render(dir,"media-confirm");
    click(lv_layer_top(),"取消");
    assert(!media_saves && key_group==original_group && !lv_obj_get_child_cnt(lv_layer_top()));
    click(lv_scr_act(),"保存设置");
    /* Even a programmatic edit under the modal must not alter the confirmed snapshot. */
    lv_textarea_set_text(find_field(lv_scr_act(),"8554"),"9554");
    lv_dropdown_set_selected(fps,1); lv_event_send(fps,LV_EVENT_VALUE_CHANGED,NULL);
    lv_dropdown_set_selected(segment,4); lv_event_send(segment,LV_EVENT_VALUE_CHANGED,NULL);
    click(lv_layer_top(),"保存设置");
    assert(media_saves==1 && media.video_frame_rate==15 && media.rtsp_port==8554 && media.record_segment_seconds==60);
    lawrec_media_settings current; lawrec_settings_media_current(&current);
    assert(current.video_frame_rate==30 && current.rtsp_port==8554 && !current.record_segment_seconds && !live_requests);

    jump_to_scr_network(); click(lv_scr_act(),"扫描热点");
    ticks+=300; lv_timer_handler();
    lv_obj_t *dropdown=find_type(lv_scr_act(),&lv_dropdown_class);
    assert(dropdown && lv_dropdown_get_option_cnt(dropdown)==3);
    lv_dropdown_open(dropdown); render(dir,"network-hotspots"); lv_dropdown_close(dropdown);
    lv_dropdown_set_selected(dropdown,1); lv_event_send(dropdown,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_t *ssid_field=find_field(lv_scr_act(),"如 Lushan-Lab");
    assert(ssid_field && !strcmp(lv_textarea_get_text(ssid_field),"Lushan-Lab"));
    lv_obj_t *name_label=lv_obj_get_child(lv_obj_get_parent(ssid_field),lv_obj_get_index(ssid_field)-1);
    assert(lv_obj_check_type(name_label,&lv_label_class) && !strcmp(lv_label_get_text(name_label),"热点名称 SSID"));
    check_keyboard(dir,"network-keyboard","8 到 63 个 ASCII 字符");
    lv_textarea_set_text(find_field(lv_scr_act(),"8 到 63 个 ASCII 字符"),"fixture-pass");
    click(lv_scr_act(),"立即连接"); render(dir,"network-confirm");
    click(lv_layer_top(),"取消"); assert(!live_requests && !wifi_saves);
    click(lv_scr_act(),"保存配置"); click(lv_layer_top(),"保存");
    assert(wifi_saves==1 && !live_requests);
    assert(!lv_textarea_get_text(find_field(lv_scr_act(),"8 到 63 个 ASCII 字符"))[0]);
    lv_obj_t *save_result=find_copy(lv_scr_act(),"已保存，下次启动时使用。");
    assert(save_result && !lv_obj_has_flag(lv_obj_get_parent(save_result),LV_OBJ_FLAG_HIDDEN));
    render(dir,"network-saved");
    // Passive lease countdown/error updates are not another scan result.
    const unsigned old_scan=network.scan_generation;
    network.dhcp_bound=0; network.dhcp_error=-ETIMEDOUT; network.lease_seconds=60;
    network.lease_remaining_seconds=0; ++network.generation;
    ticks+=300; lv_timer_handler();
    assert(find_copy(lv_scr_act(),"已保存，下次启动时使用。")==save_result);
    assert(network.scan_generation==old_scan && find_copy(lv_scr_act(),"租约已过期"));
    lv_obj_t *expired=find_copy(lv_scr_act(),"租约已过期");
    lv_obj_scroll_to_view_recursive(lv_obj_get_parent(expired),LV_ANIM_OFF);
    render(dir,"network-lease-expired");
    assert(lv_dropdown_get_option_cnt(find_type(lv_scr_act(),&lv_dropdown_class))==3);
    assert(wifi_saves==1 && !live_requests);
    network.dhcp_bound=1; network.dhcp_error=0; network.lease_remaining_seconds=59;
    ++network.generation; ticks+=300; lv_timer_handler();
    assert(!find_copy(lv_scr_act(),"租约已过期") && find_copy(lv_scr_act(),"剩余 59 秒"));
    render(dir,"network-lease-recovered");

    // Leaving never silently saves a draft or rolls back the live connection.
    lv_obj_t *wifi_screen=lv_scr_act();
    lv_obj_t *password_field=find_field(wifi_screen,"8 到 63 个 ASCII 字符");
    lv_textarea_set_text(ssid_field,"Unsaved-Lab");
    lv_textarea_set_text(password_field,"unsaved-pass");
    click(wifi_screen,"显示密码");
    click(wifi_screen,"<"); render(dir,"network-discard-confirm");
    click(lv_layer_top(),"取消");
    assert(lv_scr_act()==wifi_screen && !strcmp(lv_textarea_get_text(ssid_field),"Unsaved-Lab"));
    assert(!strcmp(lv_textarea_get_text(password_field),"unsaved-pass"));
    assert(wifi_saves==1 && !live_requests);
    // IPv4 is a subpage; visiting it must preserve the WiFi draft.
    click(wifi_screen,"IPv4 地址设置"); click(lv_scr_act(),"<");
    assert(lv_scr_act()==wifi_screen && !strcmp(lv_textarea_get_text(password_field),"unsaved-pass"));
    click(wifi_screen,"<"); click(lv_layer_top(),"放弃修改");
    assert(lv_scr_act()!=wifi_screen && !strcmp(lv_textarea_get_text(ssid_field),"Lushan-Lab"));
    assert(!lv_textarea_get_text(password_field)[0] && lv_textarea_get_password_mode(password_field));
    assert(wifi_saves==1 && !live_requests);
    jump_to_scr_network(); click(lv_scr_act(),"<");
    assert(!lv_obj_get_child_cnt(lv_layer_top()));

    jump_to_scr_ipv4(); click(lv_scr_act(),"立即应用");
    render(dir,"ipv4-confirm"); click(lv_layer_top(),"取消");
    assert(!live_requests && !ipv4_saves);
    click(lv_scr_act(),"保存策略"); click(lv_layer_top(),"保存策略");
    assert(ipv4_saves==1 && !live_requests);
    click(lv_scr_act(),"自动获取 DHCP"); render(dir,"ipv4-static");
    check_keyboard(dir,"ipv4-keyboard","例如 192.168.123.74");
    lv_obj_t *ipv4_screen=lv_scr_act();
    lv_obj_t *address_field=find_field(ipv4_screen,"例如 192.168.123.74");
    lv_textarea_set_text(address_field,"192.168.123.88");
    click(ipv4_screen,"<"); render(dir,"ipv4-discard-confirm");
    click(lv_layer_top(),"取消");
    assert(lv_scr_act()==ipv4_screen && !strcmp(lv_textarea_get_text(address_field),"192.168.123.88"));
    assert(ipv4_saves==1 && !live_requests);
    click(ipv4_screen,"<"); click(lv_layer_top(),"放弃修改");
    assert(lv_scr_act()!=ipv4_screen && pending_ipv4.dhcp && ipv4_saves==1 && !live_requests);
    jump_to_scr_ipv4(); click(lv_scr_act(),"<");
    assert(!lv_obj_get_child_cnt(lv_layer_top()));
    // Saving a static draft updates the leave baseline, not the live policy.
    jump_to_scr_ipv4(); click(lv_scr_act(),"自动获取 DHCP");
    lv_textarea_set_text(find_field(lv_scr_act(),"例如 192.168.123.74"),"192.168.123.88");
    click(lv_scr_act(),"保存策略"); click(lv_layer_top(),"保存策略");
    assert(ipv4_saves==2 && !pending_ipv4.dhcp && !live_requests);
    click(lv_scr_act(),"<"); assert(!lv_obj_get_child_cnt(lv_layer_top()));
    jump_to_scr_ipv4();
    lv_textarea_set_text(find_field(lv_scr_act(),"例如 192.168.123.74"),"192.168.123.89");
    click(lv_scr_act(),"<");
    assert(find_copy(lv_layer_top(),"放弃 IP 策略修改？") && ipv4_saves==2 && !live_requests);
    click(lv_layer_top(),"放弃修改");

    jump_to_scr_storage(); click(lv_scr_act(),"检查目录"); render(dir,"storage-checked");
    assert(!directory_saves);
    click(lv_scr_act(),"保存目录"); click(lv_layer_top(),"取消"); assert(!directory_saves);
    click(lv_scr_act(),"保存目录"); click(lv_layer_top(),"保存目录"); assert(directory_saves==1);

    jump_to_scr_time(); check_keyboard(dir,"time-keyboard","2026-10-02 00:00:00");
    click(lv_scr_act(),"校准系统时间"); render(dir,"time-confirm");
    click(lv_layer_top(),"取消"); assert(!live_requests);
    click(lv_scr_act(),"校准系统时间"); click(lv_layer_top(),"校准时间"); assert(live_requests==1);
    jump_to_scr_settings(); click(lv_scr_act(),"设备维护"); render(dir,"maintenance");
    lv_group_t *maintenance_group=key_group;
    click(lv_scr_act(),"恢复显示"); render(dir,"maintenance-confirm");
    click(lv_layer_top(),"取消"); assert(!recovery_requests && key_group==maintenance_group);
    click(lv_scr_act(),"恢复显示"); click(lv_layer_top(),"恢复显示");
    assert(recovery_requests==1 && lv_obj_has_state(find_button(lv_scr_act(),"恢复显示"),LV_STATE_DISABLED));
    render(dir,"maintenance-pending");
    scr_maintenance_recovery_result(-EIO); render(dir,"maintenance-failed");
    assert(!lv_obj_has_state(find_button(lv_scr_act(),"恢复显示"),LV_STATE_DISABLED));
    click(lv_scr_act(),"恢复显示"); click(lv_layer_top(),"恢复显示");
    scr_maintenance_recovery_result(0); render(dir,"maintenance-recovered");
    assert(recovery_requests==2 && live_requests==1);
    puts("PASS: keyboard visibility, modal focus/cancel, frozen save snapshot, network draft discard/preservation, save/live separation and explicit display recovery states.");
}

static void select_fixture(int index)
{
    lawrec_recording_entry entry=fixture_entry(index);
    char text[192]; snprintf(text,sizeof(text),"%s\n20.00 MiB",entry.name);
    click(lv_scr_act(),text);
}

static void check_files_playback(const char *dir)
{
    jump_to_scr_files(); render(dir,"files");
    lv_area_t initial_bounds;
    lv_obj_get_coords(find_button(lv_scr_act(),"下一页"),&initial_bounds);
    assert(initial_bounds.y1>=104 && initial_bounds.y2<684);
    for (int i=0; i<4; ++i) {
        lawrec_recording_entry entry=fixture_entry(i);
        char text[192]; snprintf(text,sizeof(text),"%s\n20.00 MiB",entry.name);
        lv_obj_t *item=find_button(lv_scr_act(),text); assert(item);
        lv_obj_get_coords(item,&initial_bounds);
        assert(initial_bounds.y1>=104 && initial_bounds.y2<684 && lv_obj_get_height(item)>=56);
    }
    assert(lv_obj_has_state(find_button(lv_scr_act(),"上一页"),LV_STATE_DISABLED));
    click(lv_scr_act(),"下一页"); render(dir,"files-older");
    click(lv_scr_act(),"上一页"); select_fixture(1); render(dir,"files-selected");
    click(lv_scr_act(),"删除录像"); render(dir,"files-delete-confirm");
    click(lv_layer_top(),"取消"); assert(!deletes);
    click(lv_scr_act(),"删除录像");
    // Programmatic selection under the modal cannot replace the confirmed target.
    select_fixture(2); click(lv_layer_top(),"删除录像");
    lawrec_recording_entry expected=fixture_entry(1);
    assert(deletes==1 && !strcmp(deleted_name,expected.name));
    delete_error=-ESTALE;
    click(lv_scr_act(),"删除录像"); click(lv_layer_top(),"删除录像");
    assert(deletes==1 && find_copy(lv_scr_act(),"未删除"));
    render(dir,"files-changed"); delete_error=0;
    empty_files=1; jump_to_scr_files(); render(dir,"files-empty");
    assert(lv_obj_has_state(find_button(lv_scr_act(),"播放录像"),LV_STATE_DISABLED));
    empty_files=0; file_error=-ENOENT; jump_to_scr_files(); render(dir,"files-error");
    assert(lv_obj_has_state(find_button(lv_scr_act(),"删除录像"),LV_STATE_DISABLED));
    file_error=0; jump_to_scr_files(); click(lv_scr_act(),"播放录像");
    assert(play_requests==1 && playback.active); render(dir,"playback-starting");
    assert(lv_obj_has_state(find_button(lv_scr_act(),"暂停播放"),LV_STATE_DISABLED));
    lv_area_t bounds;
    assert(lv_obj_get_style_bg_opa(lv_scr_act(),0)==LV_OPA_TRANSP);
    for (unsigned i=0; i<lv_obj_get_child_cnt(lv_scr_act()); ++i) {
        lv_obj_get_coords(lv_obj_get_child(lv_scr_act(),i),&bounds);
        assert(bounds.y2<200 || bounds.y1>469);
    }
    playback.state=LAWREC_PLAY_PLAYING; playback.audio_present=1;
    playback.duration_ms=180000; playback.position_ms=47000;
    ticks+=300; lv_timer_handler(); render(dir,"playback-playing");
    assert(find_copy(lv_scr_act(),"G.711") && find_copy(lv_scr_act(),"00:47 / 03:00"));
    click(lv_scr_act(),"暂停播放"); render(dir,"playback-paused");
    assert(playback.state==LAWREC_PLAY_PAUSED);
    click(lv_scr_act(),"继续播放"); assert(playback.state==LAWREC_PLAY_PLAYING);
    click(lv_scr_act(),"返回文件"); render(dir,"playback-stopping");
    assert(stop_requests==1 && find_copy(lv_scr_act(),"等待资源释放"));
    assert(lv_obj_has_state(find_button(lv_scr_act(),"返回文件"),LV_STATE_DISABLED));
    playback.active=0; playback.state=LAWREC_PLAY_IDLE;
    ticks+=300; lv_timer_handler(); assert(find_button(lv_scr_act(),"播放录像"));
    playback.state=LAWREC_PLAY_FINISHED; playback.position_ms=180000;
    jump_to_scr_playback(); render(dir,"playback-finished");
    assert(lv_obj_has_state(find_button(lv_scr_act(),"暂停播放"),LV_STATE_DISABLED));
    playback.state=LAWREC_PLAY_FAILED; playback.error=-EIO;
    ticks+=300; lv_timer_handler(); render(dir,"playback-failed");
    playback.resource_retained=1; playback.error=-EBADMSG; playback.cleanup_error=-EPIPE;
    ticks+=300; lv_timer_handler(); render(dir,"playback-retained");
    assert(find_copy(lv_scr_act(),"媒体资源清理失败（-32）") && find_copy(lv_scr_act(),"播放错误码 -74"));
    assert(find_copy(lv_scr_act(),"媒体资源清理失败"));
    click(lv_scr_act(),"返回文件"); render(dir,"files-retained");
    assert(lv_obj_has_state(find_button(lv_scr_act(),"播放录像"),LV_STATE_DISABLED));
    assert(lv_obj_has_state(find_button(lv_scr_act(),"删除录像"),LV_STATE_DISABLED));
    puts("PASS: file paging/empty/errors, frozen delete/cancel/stale, video aperture and playback/pause/stop/retained states.");
}

int main(int argc, char **argv)
{
    assert(argc==2);
    lv_init();
    static lv_disp_draw_buf_t buf;
    lv_disp_draw_buf_init(&buf,draw,NULL,480*100);
    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv); drv.hor_res=480; drv.ver_res=800; drv.flush_cb=flush; drv.draw_buf=&buf;
    assert(lv_disp_drv_register(&drv));
    lv_disp_set_bg_color(lv_disp_get_default(),lv_color_hex(0x050c12));
    jump_to_scr_settings(); render(argv[1],"settings");
    if (jump_to_scr_media) { jump_to_scr_media(); render(argv[1],"media"); }
    jump_to_scr_network(); render(argv[1],"network-reading");
    ticks+=240; lv_timer_handler(); render(argv[1],"network");
    jump_to_scr_ipv4(); render(argv[1],"ipv4");
    jump_to_scr_storage(); render(argv[1],"storage");
    jump_to_scr_time(); render(argv[1],"time");
    assert(!media_saves && !live_requests);
    if (jump_to_scr_media) check_workflows(argv[1]);
    check_files_playback(argv[1]);
    puts("Preview is offline: no network, clock, storage or hardware mutations.");
}
