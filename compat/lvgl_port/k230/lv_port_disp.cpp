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
 * @file lv_port_disp.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "buf_mgt.hpp"
#include "disp.h"
#include "lv_port.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**********************
 *  STATIC PROTOTYPES
 **********************/
static int disp_init(void);
static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                       lv_color_t *color_p);
static void *thread_drm_vsync(void *arg);
static uint64_t drm_get_prop_value(struct drm_object *obj, const char *name,
                                   uint64_t fallback);
static const char *drm_plane_type_name(uint64_t type);
static void drm_dump_plane_info(struct drm_dev *dev);
static int plane_config_idx(struct drm_dev *dev, struct drm_buffer *pbuf_ui,
                            uint32_t plane_idx, bool test_only);
static int select_ui_plane(struct drm_dev *dev, struct drm_buffer *pbuf_ui);

/**********************
 *  STATIC VARIABLES
 **********************/
static struct drm_dev drm_dev;
static uint32_t screen_width, screen_height;
static uint32_t display_width, display_height;
static uint32_t ui_plane_idx;

#define DRM_UI_BUF_USE_PLANE 2

#define DRM_UI_BUF_COUNT 2

#define DRM_BUF_COUNT                                                          \
    (DRM_UI_BUF_COUNT)

#define DRM_UI_BUF_SRART_IDX 0
#define DRM_UI_BUF_END_IDX (DRM_UI_BUF_SRART_IDX + DRM_UI_BUF_COUNT)

static struct drm_buffer drm_bufs[DRM_BUF_COUNT];
static void *lvgl_buf;
static buf_mgt_t ui_buf_mgt;
static bool ui_plane_configured;

#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
#define LAWREC_UI_DISP_HOR_RES 480
#define LAWREC_UI_DISP_VER_RES 800
#define LAWREC_UI_FOURCC DRM_FORMAT_ARGB4444
#define LAWREC_UI_BPP 16
#else
#define LAWREC_UI_FOURCC DRM_FORMAT_ARGB8888
#define LAWREC_UI_BPP 32
#endif

/**********************
 *      MACROS
 **********************/
#define DRM_DEV_NAME_DEFAULT "/dev/dri/card0"

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void lv_port_disp_init(void)
{
    int retry = 0;

    /*-------------------------
     * Initialize your display
     * -----------------------*/
    while (disp_init()) {
        retry++;
        if (retry >= 50) {
            fprintf(stderr, "[lawrec-ui] display init failed after %d retries\n", retry);
            return;
        }
        usleep(200 * 1000);
    }
    /*-----------------------------
     * Create a buffer for drawing
     *----------------------------*/
    static lv_disp_draw_buf_t draw_buf_dsc;
    lv_color_t *draw_buf = (lv_color_t *)malloc(display_width * 100 *
                            sizeof(lv_color_t));
    if (draw_buf == NULL)
        return;
    /*Initialize the display buffer*/
    lv_disp_draw_buf_init(&draw_buf_dsc, draw_buf, NULL, display_width * 100);
    /*-----------------------------------
     * Register the display in LVGL
     *----------------------------------*/

    static lv_disp_drv_t disp_drv; /*Descriptor of a display driver*/
    lv_disp_drv_init(&disp_drv);   /*Basic initialization*/
    /*Set up the functions to access to your display*/

    /*Set the resolution of the display*/
    disp_drv.hor_res = display_width;
    disp_drv.ver_res = display_height;

    /*Used to copy the buffer's content to the display*/
    disp_drv.flush_cb = disp_flush;

    /*Set a display buffer*/
    disp_drv.draw_buf = &draw_buf_dsc;
    disp_drv.screen_transp = 1;

    /*Finally register the driver*/
    lv_disp_drv_register(&disp_drv);
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static void drm_wait_vsync(struct drm_dev *dev)
{
    static drmEventContext drm_event_ctx;
    int ret;
    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(dev->fd, &fds);

    do {
        ret = select(dev->fd + 1, &fds, NULL, NULL, NULL);
    } while (ret == -1 && errno == EINTR);

    if (ret < 0) {
        fprintf(stderr, "select failed: %s\n", strerror(errno));
        return;
    }

    if (FD_ISSET(dev->fd, &fds)) {
        drmHandleEvent(dev->fd, &drm_event_ctx);
        dev->pflip_pending = false;
    }
}

static uint64_t drm_get_prop_value(struct drm_object *obj, const char *name,
                                   uint64_t fallback)
{
    if (!obj || !obj->props || !obj->props_info)
        return fallback;

    for (uint32_t i = 0; i < obj->props->count_props; i++) {
        if (!obj->props_info[i])
            continue;
        if (!strcmp(obj->props_info[i]->name, name))
            return obj->props->prop_values[i];
    }

    return fallback;
}

static const char *drm_plane_type_name(uint64_t type)
{
    switch (type) {
    case DRM_PLANE_TYPE_OVERLAY:
        return "overlay";
    case DRM_PLANE_TYPE_PRIMARY:
        return "primary";
    case DRM_PLANE_TYPE_CURSOR:
        return "cursor";
    default:
        return "unknown";
    }
}

static void drm_dump_plane_info(struct drm_dev *dev)
{
    for (uint32_t i = 0; i < dev->plane_count; i++) {
        struct drm_object *plane = &dev->planes[i];
        uint64_t type = drm_get_prop_value(plane, "type", UINT64_MAX);
        uint64_t zpos = drm_get_prop_value(plane, "zpos", UINT64_MAX);
        fprintf(stderr,
                "[lawrec-ui] plane[%u] id=%u type=%s zpos=%s%s%llu\n",
                i, plane->id,
                type == UINT64_MAX ? "n/a" : drm_plane_type_name(type),
                zpos == UINT64_MAX ? "n/a" : "",
                zpos == UINT64_MAX ? "" : " ",
                zpos == UINT64_MAX ? 0ULL : (unsigned long long)zpos);
    }
}

static int plane_config_idx(struct drm_dev *dev, struct drm_buffer *pbuf_ui,
                            uint32_t plane_idx, bool test_only)
{
    drmModeAtomicReq *req;
    struct drm_object *obj;
    struct drm_buffer *buf;
    uint32_t flags;
    int ret;

    if ((ret = drmModeCreatePropertyBlob(dev->fd, &dev->mode, sizeof(dev->mode),
                                         &dev->mode_blob_id)) != 0) {
        fprintf(stderr, "couldn't create a blob property\n");
        return ret;
    }

    req = drmModeAtomicAlloc();
    /* set id of the CRTC id that the connector is using */
    obj = &dev->conn;
    if ((ret = drm_set_object_property(req, obj, "CRTC_ID", dev->crtc_id)) < 0)
        goto err;

    /* set the mode id of the CRTC; this property receives the id of a blob
     * property that holds the struct that actually contains the mode info */
    obj = &dev->crtc;
    if ((ret = drm_set_object_property(req, obj, "MODE_ID",
                                       dev->mode_blob_id)) < 0)
        goto err;

    /* set the CRTC object as active */
    if ((ret = drm_set_object_property(req, obj, "ACTIVE", 1)) < 0)
        goto err;

    /* set properties of the plane related to the CRTC and the framebuffer */
    obj = &dev->planes[plane_idx];
    buf = pbuf_ui;
    if ((ret = drm_set_object_property(req, obj, "FB_ID", buf->fb)) < 0)
        goto err;
    if ((ret = drm_set_object_property(req, obj, "CRTC_ID", dev->crtc_id)) < 0)
        goto err;
    if ((ret = drm_set_object_property(req, obj, "SRC_X", 0)) < 0)
        goto err;
    if ((ret = drm_set_object_property(req, obj, "SRC_Y", 0)) < 0)
        goto err;
    if ((ret = drm_set_object_property(req, obj, "SRC_W", buf->width << 16)) <
        0)
        goto err;
    if ((ret = drm_set_object_property(req, obj, "SRC_H", buf->height << 16)) <
        0)
        goto err;
    if ((ret = drm_set_object_property(req, obj, "CRTC_X", buf->offset_x)) < 0)
        goto err;
    if ((ret = drm_set_object_property(req, obj, "CRTC_Y", buf->offset_y)) < 0)
        goto err;
    if ((ret = drm_set_object_property(req, obj, "CRTC_W", buf->width)) < 0)
        goto err;
    if ((ret = drm_set_object_property(req, obj, "CRTC_H", buf->height)) < 0)
        goto err;

    flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (test_only)
        flags |= DRM_MODE_ATOMIC_TEST_ONLY;
    else
        flags |= DRM_MODE_PAGE_FLIP_EVENT;

    if ((ret = drmModeAtomicCommit(dev->fd, req, flags, NULL)) < 0) {
        fprintf(stderr,
                test_only
                    ? "[lawrec-ui] test-only atomic commit failed, errno=%d plane_idx=%u plane_id=%u\n"
                    : "[lawrec-ui] atomic commit failed, errno=%d plane_idx=%u plane_id=%u\n",
                errno, plane_idx, obj->id);
        goto err;
    }

    if (!test_only)
        dev->pflip_pending = true;
err:
    drmModeAtomicFree(req);

    return ret;
}

static int plane_config(struct drm_dev *dev, struct drm_buffer *pbuf_ui)
{
    return plane_config_idx(dev, pbuf_ui, ui_plane_idx, false);
}

static int plane_update(struct drm_dev *dev, struct drm_buffer *pbuf_ui)
{
    drmModeAtomicReq *req;
    struct drm_object *obj;
    uint32_t fb;
    uint32_t flags;
    int ret;

    req = drmModeAtomicAlloc();
    /* set properties of the plane related to the CRTC and the framebuffer */
    obj = &dev->planes[ui_plane_idx];
    fb = pbuf_ui ? pbuf_ui->fb : 0;
    if ((ret = drm_set_object_property(req, obj, "FB_ID", fb)) < 0)
        goto err;
    if ((ret = drm_set_object_property(req, obj, "CRTC_ID",
                                       fb ? dev->crtc_id : 0)) < 0)
        goto err;

    flags = DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_PAGE_FLIP_EVENT;
    if ((ret = drmModeAtomicCommit(dev->fd, req, flags, NULL)) < 0) {
        fprintf(stderr, "[lawrec-ui] plane update failed, errno=%d plane_idx=%u\n",
                errno, ui_plane_idx);
        goto err;
    }

    dev->pflip_pending = true;
err:
    drmModeAtomicFree(req);

    return ret;
}

static int select_ui_plane(struct drm_dev *dev, struct drm_buffer *pbuf_ui)
{
    uint32_t order[32];
    uint32_t count = 0;
    uint32_t limit = dev->plane_count > 32 ? 32 : dev->plane_count;

    if (limit == 0)
        return -1;

    for (uint32_t i = 0; i < limit; i++)
        order[i] = i;
    count = limit;

    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = i + 1; j < count; j++) {
            uint64_t type_i =
                drm_get_prop_value(&dev->planes[order[i]], "type", UINT64_MAX);
            uint64_t type_j =
                drm_get_prop_value(&dev->planes[order[j]], "type", UINT64_MAX);
            uint64_t zi = drm_get_prop_value(&dev->planes[order[i]], "zpos", 0);
            uint64_t zj = drm_get_prop_value(&dev->planes[order[j]], "zpos", 0);
            bool overlay_i = (type_i == DRM_PLANE_TYPE_OVERLAY);
            bool overlay_j = (type_j == DRM_PLANE_TYPE_OVERLAY);
            if ((overlay_j && !overlay_i) ||
                (overlay_j == overlay_i && zj > zi)) {
                uint32_t tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    if (DRM_UI_BUF_USE_PLANE < count) {
        uint32_t preferred = DRM_UI_BUF_USE_PLANE;
        uint64_t preferred_type =
            drm_get_prop_value(&dev->planes[preferred], "type", UINT64_MAX);
        if (preferred_type != DRM_PLANE_TYPE_CURSOR) {
            for (uint32_t i = 0; i < count; i++) {
                if (order[i] == preferred) {
                    uint32_t tmp = order[0];
                    order[0] = order[i];
                    order[i] = tmp;
                    break;
                }
            }
        }
    }

#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    for (int i = (int)count - 1; i >= 0; i--) {
        uint32_t candidate = order[i];
        uint64_t type =
            drm_get_prop_value(&dev->planes[candidate], "type", UINT64_MAX);

        if (type != DRM_PLANE_TYPE_OVERLAY)
            continue;

        uint32_t tmp = order[0];
        order[0] = order[i];
        order[i] = tmp;
        fprintf(stderr,
                "[lawrec-ui] prefer lckfb overlay plane_idx=%u plane_id=%u\n",
                order[0], dev->planes[order[0]].id);
        break;
    }
#endif

    for (uint32_t i = 0; i < count; i++) {
        uint32_t candidate = order[i];
        uint64_t type =
            drm_get_prop_value(&dev->planes[candidate], "type", UINT64_MAX);

        if (type == DRM_PLANE_TYPE_CURSOR) {
            fprintf(stderr,
                    "[lawrec-ui] skip cursor plane_idx=%u plane_id=%u\n",
                    candidate, dev->planes[candidate].id);
            continue;
        }

        if (plane_config_idx(dev, pbuf_ui, candidate, true) == 0 &&
            plane_config_idx(dev, pbuf_ui, candidate, false) == 0) {
            ui_plane_idx = candidate;
            ui_plane_configured = true;
            fprintf(stderr,
                    "[lawrec-ui] selected plane_idx=%u plane_id=%u\n",
                    ui_plane_idx, dev->planes[ui_plane_idx].id);
            return 0;
        }
    }

    /* Fallback: if every overlay/primary candidate failed, retry any plane. */
    for (uint32_t i = 0; i < count; i++) {
        uint32_t candidate = order[i];
        if (plane_config_idx(dev, pbuf_ui, candidate, true) == 0 &&
            plane_config_idx(dev, pbuf_ui, candidate, false) == 0) {
            ui_plane_idx = candidate;
            ui_plane_configured = true;
            fprintf(stderr,
                    "[lawrec-ui] fallback selected plane_idx=%u plane_id=%u\n",
                    ui_plane_idx, dev->planes[ui_plane_idx].id);
            return 0;
        }
    }

    fprintf(stderr, "[lawrec-ui] no drm plane accepted UI framebuffer\n");
    return -1;
}

static int alloc_drm_buff(void)
{
    int fd = drm_dev.fd;

    for (int i = 0; i < DRM_BUF_COUNT; i++) {
        if (drm_create_fb(fd, &drm_bufs[i])) {
            fprintf(stderr, "couldn't create buffer %u\n", i);
            for (int ii = 0; ii < i; ii++)
                drm_destroy_fb(fd, &drm_bufs[ii]);
            return -1;
        }
    }

    return 0;
}

static void free_drm_buff(void)
{
    int fd = drm_dev.fd;

    for (int i = 0; i < DRM_BUF_COUNT; i++)
        drm_destroy_fb(fd, &drm_bufs[i]);
}

static uint16_t argb8888_to_argb4444(uint32_t data)
{
    return (uint16_t)(((data >> 16) & 0xf000) | ((data >> 12) & 0x0f00) |
                      ((data >> 8) & 0x00f0) | ((data >> 4) & 0x000f));
}

static int disp_init(void)
{
    if (drm_dev_setup(&drm_dev, DRM_DEV_NAME_DEFAULT)) {
        fprintf(stderr, "[lawrec-ui] drm_dev_setup failed for %s\n",
                DRM_DEV_NAME_DEFAULT);
        return -1;
    }

    drm_get_resolution(&drm_dev, &screen_width, &screen_height);
    if (drm_dev.plane_count == 0) {
        fprintf(stderr, "[lawrec-ui] no usable drm plane found\n");
        drm_dev_cleanup(&drm_dev);
        return -1;
    }

    drm_dump_plane_info(&drm_dev);

    ui_plane_idx = DRM_UI_BUF_USE_PLANE;
    if (ui_plane_idx >= drm_dev.plane_count)
        ui_plane_idx = drm_dev.plane_count - 1;
    ui_plane_configured = false;

#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    display_width = LAWREC_UI_DISP_HOR_RES;
    display_height = LAWREC_UI_DISP_VER_RES;
    /* Portrait panel reports the opposite horizontal direction; keep Y intact. */
    input_map_config(MAP_MODE_LCKFB_REFLECT_X, display_width, display_height);
    fprintf(stderr, "[lawrec-ui] touch map=reflect-x x=479-raw_x y=raw_y\n");
#else
    display_width = screen_width;
    display_height = screen_height / 2;
    input_map_config(MAP_MODE_DOORLOCK_HALF, display_width, display_height);
#endif

    fprintf(stderr,
            "[lawrec-ui] drm screen=%ux%u display=%ux%u plane_count=%u use_plane=%u\n",
            screen_width, screen_height, display_width, display_height,
            drm_dev.plane_count, ui_plane_idx);

    lvgl_buf = malloc(display_width * display_height * (LAWREC_UI_BPP / 8));
    if (lvgl_buf == NULL) {
        drm_dev_cleanup(&drm_dev);
        return -1;
    }
    memset(lvgl_buf, 0, display_width * display_height * (LAWREC_UI_BPP / 8));

    for (int i = DRM_UI_BUF_SRART_IDX; i < DRM_UI_BUF_END_IDX; i++) {
        drm_bufs[i].width = ALIGNED_UP_POWER_OF_TWO(display_width, 3);
        drm_bufs[i].height = ALIGNED_DOWN_POWER_OF_TWO(display_height, 0);
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
        drm_bufs[i].offset_x = 0;
        drm_bufs[i].offset_y = 0;
#else
        drm_bufs[i].offset_x = (screen_width - display_width) / 2;
        drm_bufs[i].offset_y = (screen_height - display_height);
#endif
        drm_bufs[i].fourcc = LAWREC_UI_FOURCC;
        drm_bufs[i].bpp = LAWREC_UI_BPP;
        buf_mgt_reader_put(&ui_buf_mgt, (void *)(uintptr_t)i);
    }

    if (alloc_drm_buff()) {
        drm_dev_cleanup(&drm_dev);
        return -1;
    }

    for (int i = DRM_UI_BUF_SRART_IDX; i < DRM_UI_BUF_END_IDX; i++)
        memset(drm_bufs[i].map, 0, drm_bufs[i].size);

    if (select_ui_plane(&drm_dev, &drm_bufs[DRM_UI_BUF_SRART_IDX])) {
        free_drm_buff();
        drm_dev_cleanup(&drm_dev);
        return -1;
    }

    pthread_t tid_drm_vsync;
    pthread_attr_t tattr_drm_vsync;
    pthread_attr_init(&tattr_drm_vsync);
    int max_prio = sched_get_priority_max(SCHED_RR);
    struct sched_param sp = {max_prio};
    pthread_attr_setschedpolicy(&tattr_drm_vsync, SCHED_RR);
    pthread_attr_setschedparam(&tattr_drm_vsync, &sp);
    pthread_attr_setdetachstate(&tattr_drm_vsync, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid_drm_vsync, &tattr_drm_vsync, thread_drm_vsync,
                   &drm_dev);
    pthread_attr_destroy(&tattr_drm_vsync);

    return 0;
}

static void disp_deinit(void)
{
    free_drm_buff();
    if (lvgl_buf != NULL) {
        free(lvgl_buf);
        lvgl_buf = NULL;
    }
    drm_dev_cleanup(&drm_dev);
}

static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area,
                       lv_color_t *color_p)
{
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    uint32_t *src = (uint32_t *)color_p;
    uint16_t *dst;
#else
    lv_color_t *src = color_p;
    lv_color_t *dst;
#endif
    for (int y = area->y1; y <= area->y2; y++) {
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
        dst = (uint16_t *)lvgl_buf + display_width * y + area->x1;
        for (int x = area->x1; x <= area->x2; x++)
            *dst++ = argb8888_to_argb4444(*src++);
#else
        dst = (lv_color_t *)lvgl_buf + display_width * y + area->x1;
        for (int x = area->x1; x <= area->x2; x++)
            *dst++ = *src++;
#endif
    }
    lv_disp_flush_ready(disp_drv);
    if (disp_drv->draw_buf->last_area != 1 ||
        disp_drv->draw_buf->last_part != 1)
        return;

#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
    /* Log opacity transitions, not every refresh: zero exposes the camera layer. */
    static int last_center_alpha = -1;
    uint16_t center = ((uint16_t *)lvgl_buf)[(display_height / 2) * display_width + display_width / 2];
    int center_alpha = center >> 12;
    if (center_alpha != last_center_alpha) {
        fprintf(stderr, "[lawrec-ui] overlay center alpha=%d/15 pixel=0x%04x plane_id=%u\n",
                center_alpha, center, drm_dev.planes[ui_plane_idx].id);
        last_center_alpha = center_alpha;
    }
#endif

    while (1) {
        uint64_t index;
        if (buf_mgt_writer_get(&ui_buf_mgt, (void **)&index, 1) < 0) {
            usleep(5000);
            continue;
        }
#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
        memcpy(drm_bufs[index].map, lvgl_buf,
               display_width * display_height * (LAWREC_UI_BPP / 8));
#else
        memcpy(drm_bufs[index].map, lvgl_buf,
               display_width * display_height * sizeof(lv_color_t));
#endif
        buf_mgt_writer_put(&ui_buf_mgt, (void *)(uintptr_t)index);
        if (!ui_plane_configured) {
            plane_config(&drm_dev, &drm_bufs[index]);
            ui_plane_configured = true;
        }
        break;
    }
}

uint32_t custom_tick_get(void)
{
    static uint64_t start_ms = 0;
    if (start_ms == 0) {
        struct timeval tv_start;
        gettimeofday(&tv_start, NULL);
        start_ms = (tv_start.tv_sec * 1000000 + tv_start.tv_usec) / 1000;
    }

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    uint64_t now_ms;
    now_ms = (tv_now.tv_sec * 1000000 + tv_now.tv_usec) / 1000;

    uint32_t time_ms = now_ms - start_ms;
    return time_ms;
}

static void *thread_drm_vsync(void *arg)
{
    struct drm_dev *pdev = (struct drm_dev *)arg;
    uint64_t ui_idx;
    int ui_stat;

    while (1) {
        if (pdev->pflip_pending) {
            drm_wait_vsync(pdev);
        } else {
            usleep(30000);
            continue;
        }
        if (pdev->cleanup) {
            disp_deinit();
            break;
        }
        // get ui buf index
        ui_stat = buf_mgt_reader_get(&ui_buf_mgt, (void **)&ui_idx, 1);

        if (ui_idx < DRM_UI_BUF_SRART_IDX || ui_idx >= DRM_UI_BUF_END_IDX)
            ui_idx = DRM_UI_BUF_SRART_IDX;

        plane_update(pdev, &drm_bufs[ui_idx]);
    }

    return 0;
}

#ifdef __cplusplus
} /*extern "C"*/
#endif
