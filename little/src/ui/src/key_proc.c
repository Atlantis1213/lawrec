/* Copyright (c) 2026
 */

#include "ui_common.h"
#include "key_proc.h"
#include <fcntl.h>
#include <glob.h>
#include <linux/gpio.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#define GPIO_CHIP_GLOB "/dev/gpiochip*"
#define LAWREC_KEY_SHORT_PRESS_MS 600
#define LAWREC_GPIO53_LABEL "9140c000.gpio53"
#define LAWREC_GPIO53_LINE_NAME "gpio-key-53"
#define LAWREC_GPIO53_FALLBACK_NAME "gpio53"

typedef enum {
    LAWREC_KEY_ACTION_NONE = 0,
    LAWREC_KEY_ACTION_FOCUS_NEXT,
    LAWREC_KEY_ACTION_ACTIVATE,
} lawrec_key_action_e;

typedef struct {
    int fd;
    int chip_fd;
    pthread_t thread;
    pthread_mutex_t lock;
    struct timeval press_time;
    lv_group_t *main_group;
    lv_group_t *group;
    volatile int running;
    lawrec_key_action_e pending_action;
    unsigned int line_offset;
    int active_high;
    int last_pressed;
    char devname[128];
} lawrec_key_ctx_t;

static lawrec_key_ctx_t g_key_ctx = {
    .fd = -1,
    .chip_fd = -1,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

void lawrec_key_set_group(lv_group_t *group)
{
    if (group == NULL)
        return;

    pthread_mutex_lock(&g_key_ctx.lock);
    g_key_ctx.group = group;
    pthread_mutex_unlock(&g_key_ctx.lock);

    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD)
            lv_indev_set_group(indev, group);
        indev = lv_indev_get_next(indev);
    }
}

lv_group_t *lawrec_key_create_group(lv_obj_t **items, size_t count)
{
    lv_group_t *group;
    size_t i;

    group = lv_group_create();
    if (group == NULL)
        return NULL;

    for (i = 0; i < count; i++) {
        if (items[i] != NULL)
            lv_group_add_obj(group, items[i]);
    }

    if (count > 0 && items[0] != NULL)
        lv_group_focus_obj(items[0]);

    return group;
}

lv_group_t *lawrec_key_get_main_group(void)
{
    return g_key_ctx.main_group;
}

static int lawrec_key_read_initial_value(int chip_fd, unsigned int offset)
{
    struct gpiohandle_request req;
    struct gpiohandle_data data;
    int ret;

    memset(&req, 0, sizeof(req));
    req.lines = 1;
    req.lineoffsets[0] = offset;
    req.flags = GPIOHANDLE_REQUEST_INPUT;
    snprintf(req.consumer_label, sizeof(req.consumer_label), "lawrec-key-init");

    if (ioctl(chip_fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0)
        return -1;

    memset(&data, 0, sizeof(data));
    ret = ioctl(req.fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);
    close(req.fd);
    if (ret < 0)
        return -1;

    return data.values[0] ? 1 : 0;
}

static int lawrec_key_find_offset(int chip_fd, const struct gpiochip_info *cinfo,
                                  unsigned int *offset_out)
{
    struct gpioline_info linfo;
    unsigned int offset;

    if (cinfo->lines == 1) {
        *offset_out = 0;
        return 0;
    }

    for (offset = 0; offset < cinfo->lines; offset++) {
        memset(&linfo, 0, sizeof(linfo));
        linfo.line_offset = offset;
        if (ioctl(chip_fd, GPIO_GET_LINEINFO_IOCTL, &linfo) < 0)
            continue;

        if (strcmp(linfo.name, LAWREC_GPIO53_LINE_NAME) == 0 ||
            strcmp(linfo.name, LAWREC_GPIO53_FALLBACK_NAME) == 0) {
            *offset_out = offset;
            return 0;
        }
    }

    return -1;
}

static int lawrec_key_open_gpio53(int *initial_raw)
{
    glob_t gl;
    int ret;
    size_t i;

    ret = glob(GPIO_CHIP_GLOB, 0, NULL, &gl);
    if (ret != 0)
        return -1;

    for (i = 0; i < gl.gl_pathc; i++) {
        struct gpiochip_info cinfo;
        struct gpioevent_request req;
        unsigned int offset = 0;
        int chip_fd;
        int raw;

        chip_fd = open(gl.gl_pathv[i], O_RDONLY | O_CLOEXEC);
        if (chip_fd < 0)
            continue;

        memset(&cinfo, 0, sizeof(cinfo));
        if (ioctl(chip_fd, GPIO_GET_CHIPINFO_IOCTL, &cinfo) < 0) {
            close(chip_fd);
            continue;
        }

        if (strcmp(cinfo.label, LAWREC_GPIO53_LABEL) != 0) {
            close(chip_fd);
            continue;
        }

        if (lawrec_key_find_offset(chip_fd, &cinfo, &offset) < 0) {
            fprintf(stderr, "lawrec key: no matching line on %s, fallback offset=0\n",
                    cinfo.label);
            offset = 0;
        }

        raw = lawrec_key_read_initial_value(chip_fd, offset);

        memset(&req, 0, sizeof(req));
        req.lineoffset = offset;
        req.handleflags = GPIOHANDLE_REQUEST_INPUT;
        req.eventflags = GPIOEVENT_REQUEST_BOTH_EDGES;
        snprintf(req.consumer_label, sizeof(req.consumer_label), "lawrec-key");

        if (ioctl(chip_fd, GPIO_GET_LINEEVENT_IOCTL, &req) < 0) {
            close(chip_fd);
            continue;
        }

        g_key_ctx.chip_fd = chip_fd;
        g_key_ctx.line_offset = offset;
        snprintf(g_key_ctx.devname, sizeof(g_key_ctx.devname), "%s", cinfo.label);
        if (initial_raw != NULL)
            *initial_raw = raw;
        globfree(&gl);
        return req.fd;
    }

    globfree(&gl);
    return -1;
}

static void lawrec_key_queue_action(lawrec_key_action_e action)
{
    pthread_mutex_lock(&g_key_ctx.lock);
    g_key_ctx.pending_action = action;
    pthread_mutex_unlock(&g_key_ctx.lock);

    if (action == LAWREC_KEY_ACTION_ACTIVATE)
        fprintf(stderr, "lawrec key: queue activate\n");
    else if (action == LAWREC_KEY_ACTION_FOCUS_NEXT)
        fprintf(stderr, "lawrec key: queue next\n");
}

static void lawrec_key_handle_release(void)
{
    struct timeval now;
    long elapsed_ms;

    gettimeofday(&now, NULL);
    elapsed_ms = (now.tv_sec - g_key_ctx.press_time.tv_sec) * 1000L +
                 (now.tv_usec - g_key_ctx.press_time.tv_usec) / 1000L;

    fprintf(stderr, "lawrec key: release after %ld ms\n", elapsed_ms);

    if (elapsed_ms >= LAWREC_KEY_SHORT_PRESS_MS)
        lawrec_key_queue_action(LAWREC_KEY_ACTION_ACTIVATE);
    else
        lawrec_key_queue_action(LAWREC_KEY_ACTION_FOCUS_NEXT);
}

static int lawrec_key_event_is_press(__u32 id)
{
    if (g_key_ctx.active_high)
        return id == GPIOEVENT_EVENT_RISING_EDGE;
    return id == GPIOEVENT_EVENT_FALLING_EDGE;
}

static void *lawrec_key_thread(void *arg)
{
    (void)arg;

    while (g_key_ctx.running) {
        struct gpioevent_data event;
        struct pollfd pfd;
        int pret;
        ssize_t r;
        int pressed;

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = g_key_ctx.fd;
        pfd.events = POLLIN;

        pret = poll(&pfd, 1, 1000);
        if (pret <= 0)
            continue;

        if ((pfd.revents & POLLIN) == 0)
            continue;

        r = read(g_key_ctx.fd, &event, sizeof(event));
        if (r != (ssize_t)sizeof(event))
            continue;

        pressed = lawrec_key_event_is_press(event.id);
        if (pressed && !g_key_ctx.last_pressed) {
            fprintf(stderr, "lawrec key: gpio53 press edge=%u offset=%u\n",
                    event.id, g_key_ctx.line_offset);
            gettimeofday(&g_key_ctx.press_time, NULL);
        } else if (!pressed && g_key_ctx.last_pressed) {
            fprintf(stderr, "lawrec key: gpio53 release edge=%u offset=%u\n",
                    event.id, g_key_ctx.line_offset);
            lawrec_key_handle_release();
        }

        g_key_ctx.last_pressed = pressed;
    }

    return NULL;
}

int lawrec_key_init(void)
{
    lv_obj_t *items[] = {
        lv_ui.scr_main_btn_signup.obj,
        lv_ui.scr_main_btn_import.obj,
        lv_ui.scr_main_btn_delete.obj,
        lv_ui.scr_main_btn_ota.obj,
    };
    int i;
    int initial_raw = -1;

    g_key_ctx.main_group = lv_group_create();
    g_key_ctx.group = g_key_ctx.main_group;
    if (g_key_ctx.group == NULL)
        return -1;

    for (i = 0; i < ARRAY_SIZE(items); i++) {
        if (items[i] != NULL)
            lv_group_add_obj(g_key_ctx.group, items[i]);
    }

    if (items[0] != NULL)
        lv_group_focus_obj(items[0]);

    g_key_ctx.active_high = 1;
    g_key_ctx.fd = lawrec_key_open_gpio53(&initial_raw);

    if (g_key_ctx.fd < 0) {
        fprintf(stderr, "lawrec key: gpio53 backend unavailable\n");
        return -1;
    }

    fprintf(stderr, "lawrec key: use %s (gpio53 event offset=%u active_%s)\n",
            g_key_ctx.devname, g_key_ctx.line_offset,
            g_key_ctx.active_high ? "high" : "low");
    fprintf(stderr, "lawrec key: gpio53 init raw=%d\n", initial_raw);
    g_key_ctx.last_pressed = (initial_raw > 0) ? 1 : 0;
    if (g_key_ctx.last_pressed)
        gettimeofday(&g_key_ctx.press_time, NULL);
    g_key_ctx.running = 1;
    if (pthread_create(&g_key_ctx.thread, NULL, lawrec_key_thread, NULL) != 0) {
        g_key_ctx.running = 0;
        close(g_key_ctx.fd);
        g_key_ctx.fd = -1;
        if (g_key_ctx.chip_fd >= 0) {
            close(g_key_ctx.chip_fd);
            g_key_ctx.chip_fd = -1;
        }
        return -1;
    }

    pthread_detach(g_key_ctx.thread);
    lawrec_key_set_group(g_key_ctx.group);
    scr_main_set_status("短按切换 长按进入", lv_color_hex(0x6fdcff));
    return 0;
}

void lawrec_key_poll(void)
{
    lawrec_key_action_e action;
    lv_obj_t *focused;

    pthread_mutex_lock(&g_key_ctx.lock);
    action = g_key_ctx.pending_action;
    g_key_ctx.pending_action = LAWREC_KEY_ACTION_NONE;
    pthread_mutex_unlock(&g_key_ctx.lock);

    if (action == LAWREC_KEY_ACTION_NONE || g_key_ctx.group == NULL)
        return;

    if (action == LAWREC_KEY_ACTION_FOCUS_NEXT) {
        lv_group_focus_next(g_key_ctx.group);
        scr_main_set_status("进入", lv_color_hex(0xffd166));
        return;
    }

    focused = lv_group_get_focused(g_key_ctx.group);
    if (focused != NULL) {
        scr_main_set_status("进入", lv_color_hex(0x4ade80));
        lv_event_send(focused, LV_EVENT_CLICKED, NULL);
    }
}
