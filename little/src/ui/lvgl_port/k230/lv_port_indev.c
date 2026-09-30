/* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file lv_port_indev.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_port.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

/*********************
 *      DEFINES
 *********************/
#define TOUCHPAD_DEV_GLOB "/dev/input/event*"
#define TOUCH_TRACE_LOG "/tmp/touch_trace.log"
#define TOUCH_MAX_MT_SLOTS 16

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    int evdev_fd;
    int evdev_root_x;
    int evdev_root_y;
    int sample_x;
    int sample_y;
    int evdev_button;
    int evdev_key_val;
    int btn_touching;
    int tracking_id;
    int active_tracking_id;
    int current_slot;
    int mt_slot_count;
    int mt_active_count;
    unsigned char mt_slot_active[TOUCH_MAX_MT_SLOTS];
    int sample_valid;
    int abs_x_min;
    int abs_x_max;
    int abs_y_min;
    int abs_y_max;
    int abs_x_code;
    int abs_y_code;
    int has_abs_xy;
    int has_mt_xy;
    int use_abs_xy;
    char devname[PATH_MAX];
} evdev_t;

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void touchpad_init(void);
static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data);
static void map0(int *evdev_root_x, int *evdev_root_y);
static void map1(int *evdev_root_x, int *evdev_root_y);
static void map2(int *evdev_root_x, int *evdev_root_y);
static int touchpad_open_device(void);
static bool touchpad_is_touch_device(int fd, const char *devname);
static void touchpad_query_abs_range(evdev_t *evdev);
static int touchpad_scale_axis(int value, int min, int max, int span);
static void touch_trace_open(void);
static void touch_trace_log_report(int raw_x, int raw_y, int scaled_x,
                                   int scaled_y, int mapped_x, int mapped_y,
                                   int state, int tracking_id);

/**********************
 *  STATIC VARIABLES
 **********************/
static evdev_t touchpad_evdev = {.evdev_fd = -1};
static void (*map)(int *evdev_root_x, int *evdev_root_y);
static int map_width = 800;
static int map_height = 1280;
static FILE *touch_trace_fp;
static bool touch_sync_lost;
static bool touch_wait_release;

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_port_indev_init(void)
{
    static lv_indev_drv_t driver;
    static lv_indev_t *device;
    const char *enabled = getenv("LAWREC_TOUCH_ENABLE");
    if (enabled && strcmp(enabled, "0") == 0) {
        fprintf(stderr, "lawrec indev: touch disabled by environment\n");
        return;
    }
    if (device)
        return;
    touchpad_init();
    if (touchpad_evdev.evdev_fd < 0)
        return;
    lv_indev_drv_init(&driver);
    driver.type = LV_INDEV_TYPE_POINTER;
    driver.read_cb = touchpad_read;
    device = lv_indev_drv_register(&driver);
    if (!device) {
        fprintf(stderr, "lawrec indev: touch registration failed\n");
        close(touchpad_evdev.evdev_fd);
        touchpad_evdev.evdev_fd = -1;
        return;
    }
    fprintf(stderr, "lawrec indev: touch enabled map=%dx%d\n", map_width, map_height);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void touchpad_init(void)
{
    int evdev_fd = touchpad_evdev.evdev_fd;

    if (evdev_fd != -1)
        close(evdev_fd);
    touchpad_evdev.evdev_fd = -1;

    evdev_fd = touchpad_open_device();
    if (evdev_fd == -1) {
        fprintf(stderr, "lawrec indev: no touch event device found\n");
        return;
    }

    {
        int flags = fcntl(evdev_fd, F_GETFL, 0);
        if (flags < 0)
            flags = 0;
        if (fcntl(evdev_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            fprintf(stderr, "lawrec indev: nonblocking setup failed errno=%d\n", errno);
            close(evdev_fd);
            return;
        }
    }

    touchpad_evdev.evdev_fd = evdev_fd;
    touchpad_evdev.evdev_root_x = 0;
    touchpad_evdev.evdev_root_y = 0;
    touchpad_evdev.sample_x = 0;
    touchpad_evdev.sample_y = 0;
    touchpad_evdev.evdev_key_val = 0;
    touchpad_evdev.evdev_button = LV_INDEV_STATE_REL;
    touchpad_evdev.btn_touching = 0;
    touchpad_evdev.tracking_id = -1;
    touchpad_evdev.active_tracking_id = -1;
    touchpad_evdev.current_slot = 0;
    touchpad_evdev.mt_slot_count = 0;
    touchpad_evdev.mt_active_count = 0;
    memset(touchpad_evdev.mt_slot_active, 0,
           sizeof(touchpad_evdev.mt_slot_active));
    touchpad_evdev.sample_valid = 0;
    touch_trace_open();
    touchpad_query_abs_range(&touchpad_evdev);
    fprintf(stderr, "lawrec indev: use %s\n", touchpad_evdev.devname);
}

static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    struct input_event in;
    int evdev_fd = touchpad_evdev.evdev_fd;
    int evdev_root_x = touchpad_evdev.evdev_root_x;
    int evdev_root_y = touchpad_evdev.evdev_root_y;
    int evdev_button = touchpad_evdev.evdev_button;
    int btn_touching = touchpad_evdev.btn_touching;
    int sample_x = touchpad_evdev.sample_x;
    int sample_y = touchpad_evdev.sample_y;
    int active_tracking_id = touchpad_evdev.active_tracking_id;
    int current_slot = touchpad_evdev.current_slot;
    int mt_active_count = touchpad_evdev.mt_active_count;
    int sample_valid = touchpad_evdev.sample_valid;
    int report_raw_x = sample_x;
    int report_raw_y = sample_y;
    int report_pending = 0;

    (void)indev_drv;
    data->continue_reading = false;

    if (evdev_fd < 0) {
        data->state = LV_INDEV_STATE_REL;
        data->point.x = 0;
        data->point.y = 0;
        return;
    }

    while (1) {
        int rbytes = read(evdev_fd, &in, sizeof(in));
        if (rbytes < 0 && errno == EINTR)
            continue;
        if (rbytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "lawrec indev: read failed errno=%d device=%s\n",
                    errno, touchpad_evdev.devname);
            close(evdev_fd);
            touchpad_evdev.evdev_fd = -1;
            evdev_button = LV_INDEV_STATE_REL;
            break;
        }
        if (rbytes < (int)sizeof(in))
            break;
        if (in.type == EV_SYN && in.code == SYN_DROPPED) {
            /* Cancel the gesture rather than turn a lost press into a click.
             * Linux requires ignoring events through the next SYN_REPORT. */
            touch_sync_lost = true;
            touch_wait_release = true;
            lv_indev_reset(lv_indev_get_act(), NULL);
            evdev_button = LV_INDEV_STATE_REL;
            btn_touching = 0;
            report_pending = 0;
            fprintf(stderr, "lawrec indev: SYN_DROPPED; cancel gesture and wait for release\n");
            continue;
        }
        if (touch_sync_lost || touch_wait_release) {
            if (in.type == EV_SYN && in.code == SYN_REPORT) {
                unsigned long keys[(KEY_MAX + 8 * sizeof(unsigned long)) /
                                   (8 * sizeof(unsigned long))] = {0};
                struct input_absinfo x, y;
                touch_sync_lost = false;
                /* This board exports ABS_X/Y + BTN_TOUCH via the kernel's
                 * pointer emulation. Re-snapshot, but do not resume a held
                 * finger: require a clean release before a new gesture. */
                if (touchpad_evdev.use_abs_xy &&
                    ioctl(evdev_fd, EVIOCGKEY(sizeof(keys)), keys) >= 0 &&
                    ioctl(evdev_fd, EVIOCGABS(ABS_X), &x) >= 0 &&
                    ioctl(evdev_fd, EVIOCGABS(ABS_Y), &y) >= 0 &&
                    !(keys[BTN_TOUCH / (8 * sizeof(unsigned long))] &
                      (1UL << (BTN_TOUCH % (8 * sizeof(unsigned long)))))) {
                    sample_x = x.value;
                    sample_y = y.value;
                    active_tracking_id = -1;
                    mt_active_count = 0;
                    sample_valid = 0;
                    touch_wait_release = false;
                    fprintf(stderr, "lawrec indev: resynchronized after release\n");
                }
                data->continue_reading = true;
                break;
            }
            continue;
        }
        if (in.type == EV_ABS) {
            if (touchpad_evdev.use_abs_xy && in.code == ABS_X) {
                sample_x = in.value;
                report_pending = 1;
            } else if (touchpad_evdev.use_abs_xy && in.code == ABS_Y) {
                sample_y = in.value;
                report_pending = 1;
            } else if (!touchpad_evdev.use_abs_xy &&
                       in.code == ABS_MT_POSITION_X) {
                sample_x = in.value;
                report_pending = 1;
            } else if (!touchpad_evdev.use_abs_xy &&
                       in.code == ABS_MT_POSITION_Y) {
                sample_y = in.value;
                report_pending = 1;
            } else if (in.code == ABS_MT_SLOT) {
                if (in.value >= 0 && in.value < touchpad_evdev.mt_slot_count)
                    current_slot = in.value;
            } else if (!touchpad_evdev.use_abs_xy &&
                       in.code == ABS_MT_TRACKING_ID) {
                touchpad_evdev.tracking_id = in.value;
                if (in.value < 0) {
                    if (current_slot >= 0 &&
                        current_slot < touchpad_evdev.mt_slot_count &&
                        touchpad_evdev.mt_slot_active[current_slot]) {
                        touchpad_evdev.mt_slot_active[current_slot] = 0;
                        if (mt_active_count > 0)
                            mt_active_count--;
                    }
                    active_tracking_id = -1;
                    sample_valid = 0;
                    if (!btn_touching && mt_active_count <= 0)
                        evdev_button = LV_INDEV_STATE_REL;
                } else {
                    if (current_slot >= 0 &&
                        current_slot < touchpad_evdev.mt_slot_count &&
                        !touchpad_evdev.mt_slot_active[current_slot]) {
                        touchpad_evdev.mt_slot_active[current_slot] = 1;
                        mt_active_count++;
                    }
                    active_tracking_id = in.value;
                    sample_valid = 0;
                    evdev_button = LV_INDEV_STATE_PR;
                }
                report_pending = 1;
            }
        } else if (in.type == EV_KEY) {
            if (in.code == BTN_TOUCH || in.code == BTN_LEFT) {
                if (in.value == 0) {
                    btn_touching = 0;
                    evdev_button = LV_INDEV_STATE_REL;
                    active_tracking_id = -1;
                    sample_valid = 0;
                } else if (in.value == 1) {
                    btn_touching = 1;
                    evdev_button = LV_INDEV_STATE_PR;
                }
                report_pending = 1;
            }
        } else if (in.type == EV_SYN && in.code == SYN_REPORT) {
            if (!touchpad_evdev.use_abs_xy &&
                touchpad_evdev.tracking_id < 0 && !btn_touching &&
                mt_active_count <= 0)
                evdev_button = LV_INDEV_STATE_REL;
            if (report_pending) {
                int scaled_x;
                int scaled_y;
                int mapped_x;
                int mapped_y;

                report_raw_x = sample_x;
                report_raw_y = sample_y;

                if (touchpad_evdev.use_abs_xy) {
                    evdev_button = btn_touching ? LV_INDEV_STATE_PR
                                                : LV_INDEV_STATE_REL;
                }

                if (evdev_button == LV_INDEV_STATE_PR &&
                    (touchpad_evdev.use_abs_xy ? btn_touching
                                               : (btn_touching ||
                                                  active_tracking_id >= 0))) {
                    evdev_root_x = sample_x;
                    evdev_root_y = sample_y;
                    sample_valid = 1;
                }

                if (evdev_button == LV_INDEV_STATE_REL) {
                    sample_valid = 0;
                    if (!btn_touching && mt_active_count <= 0)
                        active_tracking_id = -1;
                }

                scaled_x = touchpad_scale_axis(evdev_root_x,
                                               touchpad_evdev.abs_x_min,
                                               touchpad_evdev.abs_x_max,
                                               map_width);
                scaled_y = touchpad_scale_axis(evdev_root_y,
                                               touchpad_evdev.abs_y_min,
                                               touchpad_evdev.abs_y_max,
                                               map_height);
                mapped_x = scaled_x;
                mapped_y = scaled_y;
                if (map)
                    map(&mapped_x, &mapped_y);
                touch_trace_log_report(report_raw_x, report_raw_y, scaled_x,
                                       scaled_y, mapped_x, mapped_y,
                                       evdev_button,
                                       touchpad_evdev.tracking_id);

                report_pending = 0;
            }
            /* Deliver each complete frame to LVGL: draining through release
             * here would erase a short press before LVGL can observe it. */
            data->continue_reading = true;
            break;
        }
    }

    touchpad_evdev.evdev_root_x = evdev_root_x;
    touchpad_evdev.evdev_root_y = evdev_root_y;
    touchpad_evdev.sample_x = sample_x;
    touchpad_evdev.sample_y = sample_y;
    touchpad_evdev.evdev_button = evdev_button;
    touchpad_evdev.btn_touching = btn_touching;
    touchpad_evdev.active_tracking_id = active_tracking_id;
    touchpad_evdev.current_slot = current_slot;
    touchpad_evdev.mt_active_count = mt_active_count;
    touchpad_evdev.sample_valid = sample_valid;

    evdev_root_x = touchpad_scale_axis(evdev_root_x, touchpad_evdev.abs_x_min,
                                       touchpad_evdev.abs_x_max, map_width);
    evdev_root_y = touchpad_scale_axis(evdev_root_y, touchpad_evdev.abs_y_min,
                                       touchpad_evdev.abs_y_max, map_height);

    if (map)
        map(&evdev_root_x, &evdev_root_y);

    data->state = evdev_button;
    data->point.x = evdev_root_x;
    data->point.y = evdev_root_y;
}

static void map0(int *evdev_root_x, int *evdev_root_y)
{
    int x, y;
    x = map_width - *evdev_root_x;
    y = map_height - *evdev_root_y;
    y = y - map_height / 2;
    
    // y limit
    y = y < 0 ? 0 : y;
    x = x < 0 ? 0 : x;

    *evdev_root_y = y;
    *evdev_root_x = x;
}

static void map1(int *evdev_root_x, int *evdev_root_y)
{
    int x, y;

    x = *evdev_root_x;
    y = *evdev_root_y;
    // x,y limit
    x = x < 0 ? 0 : x >= map_width ? map_width - 1 : x;
    y = y < 0 ? 0 : y >= map_height ? map_height - 1 : y;

    *evdev_root_x = x;
    *evdev_root_y = y;
}

static void map2(int *evdev_root_x, int *evdev_root_y)
{
    int x, y;

    x = map_width - 1 - *evdev_root_x;
    y = *evdev_root_y;
    x = x < 0 ? 0 : x >= map_width ? map_width - 1 : x;
    y = y < 0 ? 0 : y >= map_height ? map_height - 1 : y;

    *evdev_root_x = x;
    *evdev_root_y = y;
}

void input_map_config(int mode, int width, int height)
{
    map_width = width;
    map_height = height;

    if (mode == MAP_MODE_DOORLOCK_HALF)
        map = map0;
    else if (mode == MAP_MODE_CLAMP_ONLY)
        map = map1;
    else if (mode == MAP_MODE_LCKFB_REFLECT_X)
        map = map2;
    else
        map = NULL;
}

static int touchpad_open_device(void)
{
    glob_t gl;
    int ret;
    size_t i;

    memset(touchpad_evdev.devname, 0, sizeof(touchpad_evdev.devname));
    ret = glob(TOUCHPAD_DEV_GLOB, 0, NULL, &gl);
    if (ret != 0)
        return -1;

    for (i = 0; i < gl.gl_pathc; i++) {
        int fd = open(gl.gl_pathv[i], O_RDONLY | O_NOCTTY | O_NDELAY);
        if (fd < 0)
            continue;

        if (touchpad_is_touch_device(fd, gl.gl_pathv[i])) {
            snprintf(touchpad_evdev.devname, sizeof(touchpad_evdev.devname),
                     "%s", gl.gl_pathv[i]);
            globfree(&gl);
            return fd;
        }

        close(fd);
    }

    globfree(&gl);
    return -1;
}

static bool touchpad_is_touch_device(int fd, const char *devname)
{
    unsigned long ev_bits[(EV_MAX + 8 * sizeof(unsigned long)) /
                          (8 * sizeof(unsigned long))];
    unsigned long abs_bits[(ABS_MAX + 8 * sizeof(unsigned long)) /
                           (8 * sizeof(unsigned long))];
    char name[128];
    bool has_abs_x;
    bool has_abs_y;

    memset(ev_bits, 0, sizeof(ev_bits));
    memset(abs_bits, 0, sizeof(abs_bits));
    memset(name, 0, sizeof(name));

    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
        snprintf(name, sizeof(name), "%s", devname);

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
        return false;

    if (!(ev_bits[EV_ABS / (8 * sizeof(unsigned long))] &
          (1UL << (EV_ABS % (8 * sizeof(unsigned long))))))
        return false;

    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0)
        return false;

    has_abs_x =
        abs_bits[ABS_X / (8 * sizeof(unsigned long))] &
        (1UL << (ABS_X % (8 * sizeof(unsigned long))));
    has_abs_y =
        abs_bits[ABS_Y / (8 * sizeof(unsigned long))] &
        (1UL << (ABS_Y % (8 * sizeof(unsigned long))));

    if (!(has_abs_x && has_abs_y)) {
        has_abs_x =
            abs_bits[ABS_MT_POSITION_X / (8 * sizeof(unsigned long))] &
            (1UL << (ABS_MT_POSITION_X % (8 * sizeof(unsigned long))));
        has_abs_y =
            abs_bits[ABS_MT_POSITION_Y / (8 * sizeof(unsigned long))] &
            (1UL << (ABS_MT_POSITION_Y % (8 * sizeof(unsigned long))));
    }

    if (!(has_abs_x && has_abs_y))
        return false;

    if (strstr(name, "gpio_keys") != NULL)
        return false;

    fprintf(stderr, "lawrec indev: candidate %s name=%s\n", devname, name);

    return true;
}

static void touchpad_query_abs_range(evdev_t *evdev)
{
    struct input_absinfo abs;
    struct input_absinfo abs_mt;
    struct input_absinfo abs_slot;

    evdev->has_abs_xy = 0;
    evdev->has_mt_xy = 0;
    evdev->use_abs_xy = 1;
    evdev->abs_x_code = ABS_X;
    evdev->abs_y_code = ABS_Y;
    evdev->abs_x_min = 0;
    evdev->abs_y_min = 0;
    evdev->abs_x_max = map_width > 0 ? map_width - 1 : 0;
    evdev->abs_y_max = map_height > 0 ? map_height - 1 : 0;

    if (ioctl(evdev->evdev_fd, EVIOCGABS(ABS_X), &abs) == 0) {
        evdev->has_abs_xy = 1;
        evdev->abs_x_min = abs.minimum;
        evdev->abs_x_max = abs.maximum;
    }

    if (ioctl(evdev->evdev_fd, EVIOCGABS(ABS_Y), &abs) == 0) {
        evdev->has_abs_xy = evdev->has_abs_xy && 1;
        evdev->abs_y_min = abs.minimum;
        evdev->abs_y_max = abs.maximum;
    } else {
        evdev->has_abs_xy = 0;
    }

    if (ioctl(evdev->evdev_fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_mt) == 0) {
        evdev->has_mt_xy = 1;
    }
    if (ioctl(evdev->evdev_fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_mt) != 0) {
        evdev->has_mt_xy = 0;
    }

    if (!evdev->has_abs_xy &&
        ioctl(evdev->evdev_fd, EVIOCGABS(ABS_MT_POSITION_X), &abs) == 0) {
        evdev->abs_x_code = ABS_MT_POSITION_X;
        evdev->abs_x_min = abs.minimum;
        evdev->abs_x_max = abs.maximum;
    }

    if (!evdev->has_abs_xy &&
        ioctl(evdev->evdev_fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs) == 0) {
        evdev->abs_y_code = ABS_MT_POSITION_Y;
        evdev->abs_y_min = abs.minimum;
        evdev->abs_y_max = abs.maximum;
    }

    evdev->use_abs_xy = evdev->has_abs_xy ? 1 : 0;
    evdev->mt_slot_count = 0;
    if (ioctl(evdev->evdev_fd, EVIOCGABS(ABS_MT_SLOT), &abs_slot) == 0 &&
        abs_slot.maximum >= 0) {
        evdev->mt_slot_count = abs_slot.maximum + 1;
        if (evdev->mt_slot_count > TOUCH_MAX_MT_SLOTS)
            evdev->mt_slot_count = TOUCH_MAX_MT_SLOTS;
    }

    fprintf(stderr,
            "lawrec indev: abs x=[%d,%d] y=[%d,%d] mode=%s mt_slots=%d\n",
            evdev->abs_x_min, evdev->abs_x_max,
            evdev->abs_y_min, evdev->abs_y_max,
            evdev->use_abs_xy ? "ABS_X/Y" : "ABS_MT_POSITION_X/Y",
            evdev->mt_slot_count);
}

static int touchpad_scale_axis(int value, int min, int max, int span)
{
    long long scaled;

    if (span <= 1 || max <= min)
        return value;

    if (value < min)
        value = min;
    if (value > max)
        value = max;

    scaled = (long long)(value - min) * (span - 1);
    scaled /= (max - min);
    return (int)scaled;
}

static void touch_trace_open(void)
{
    const char *enable;

    if (touch_trace_fp != NULL)
        fclose(touch_trace_fp);

    touch_trace_fp = NULL;

    enable = getenv("LAWREC_TOUCH_TRACE");
    if (enable == NULL || strcmp(enable, "1") != 0)
        return;

    touch_trace_fp = fopen(TOUCH_TRACE_LOG, "w");
    if (touch_trace_fp == NULL)
        return;

    setvbuf(touch_trace_fp, NULL, _IOLBF, 0);
    fprintf(touch_trace_fp,
            "# sec.usec raw_x raw_y scaled_x scaled_y mapped_x mapped_y state tracking_id\n");
}

static void touch_trace_log_report(int raw_x, int raw_y, int scaled_x,
                                   int scaled_y, int mapped_x, int mapped_y,
                                   int state, int tracking_id)
{
    struct timeval tv;

    if (touch_trace_fp == NULL)
        return;

    gettimeofday(&tv, NULL);
    fprintf(touch_trace_fp,
            "%ld.%06ld raw=%d,%d scaled=%d,%d mapped=%d,%d state=%s tracking=%d\n",
            (long)tv.tv_sec, (long)tv.tv_usec, raw_x, raw_y, scaled_x,
            scaled_y, mapped_x, mapped_y,
            state == LV_INDEV_STATE_PR ? "PR" : "REL", tracking_id);
}
