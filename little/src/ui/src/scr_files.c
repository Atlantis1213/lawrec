#include "ui_common.h"
#include "key_proc.h"
#include "../../common/lawrec_storage.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static lv_obj_t *screen, *summary;
static lv_group_t *group;
static char names[100][128];
static unsigned count, selected;
static int confirming;
LV_FONT_DECLARE(lawrec_font_cn_20);

static void show_file(void)
{
    char text[768], path[512];
    uint64_t free_bytes = 0;
    int ret = lawrec_storage_check(&free_bytes);
    struct stat st = {0};
    snprintf(path, sizeof(path), "%s/%s", lawrec_storage_dir(), count ? names[selected] : "");
    if (count && stat(path, &st)) st.st_size = 0;
    snprintf(text, sizeof(text),
             "%u / %u\n%s\nSize: %.2f MiB\nFree: %.0f MiB\nStorage: %s\n%s\n%s",
             count ? selected + 1 : 0, count, count ? names[selected] : "No completed recordings",
             (double)st.st_size / 1048576.0, (double)free_bytes / 1048576.0,
             ret ? strerror(-ret) : "ready", lawrec_storage_dir(),
             confirming ? "Press Delete again to confirm" : "Export: use SCP from this directory");
    lv_label_set_text(summary, text);
}

static void refresh(void)
{
    char text[16384], *save, *name;
    count = selected = 0; confirming = 0;
    int ret = lawrec_storage_list(text, sizeof(text));
    if (!ret) {
        for (name = strtok_r(text, "\n", &save); name && count < 100; name = strtok_r(NULL, "\n", &save))
            snprintf(names[count++], sizeof(names[0]), "%s", name);
    }
    show_file();
}

static void action(lv_event_t *e)
{
    intptr_t cmd = (intptr_t)lv_event_get_user_data(e);
    if (cmd == 0) { confirming = 0; jump_to_scr_main(); return; }
    if (cmd == 1) { refresh(); return; }
    if (cmd == 2 && count) selected = (selected + count - 1) % count;
    if (cmd == 3 && count) selected = (selected + 1) % count;
    if (cmd == 4 && count) {
        if (confirming) {
            int ret = lawrec_storage_delete(names[selected]);
            refresh();
            if (ret) lv_label_set_text(summary, strerror(-ret));
            return;
        }
        confirming = 1; show_file(); return;
    }
    confirming = 0; show_file();
}

void jump_to_scr_files(void)
{
    if (!screen) {
        screen = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0c1b28), 0);
        summary = lv_label_create(screen);
        lv_obj_set_width(summary, 420);
        lv_obj_align(summary, LV_ALIGN_TOP_MID, 0, 28);
        lv_obj_set_style_text_font(summary, &lawrec_font_cn_20, 0);
        lv_obj_set_style_text_color(summary, lv_color_hex(0xffffff), 0);
        const char *titles[] = {"返回", "Refresh", "Previous", "Next", "Delete"};
        lv_obj_t *buttons[5];
        for (unsigned i = 0; i < 5; ++i) {
            buttons[i] = lv_btn_create(screen);
            lv_obj_set_size(buttons[i], 190, 66);
            lv_obj_set_pos(buttons[i], 20 + (i % 2) * 215, 470 + (i / 2) * 85);
            lv_obj_add_event_cb(buttons[i], action, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *label = lv_label_create(buttons[i]);
            lv_obj_set_style_text_font(label, &lawrec_font_cn_20, 0);
            lv_label_set_text(label, titles[i]); lv_obj_center(label);
        }
        group = lawrec_key_create_group(buttons, 5);
    }
    refresh();
    lv_scr_load(screen);
    lawrec_key_set_group(group);
}
