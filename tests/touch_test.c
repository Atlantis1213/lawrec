/* Exercise the actual evdev reader with a pipe; LVGL registration is stubbed. */
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#define LV_PORT_H
#define LV_INDEV_STATE_REL 0
#define LV_INDEV_STATE_PR 1
#define LV_INDEV_TYPE_POINTER 1
#define MAP_MODE_DOORLOCK_HALF 0
#define MAP_MODE_CLAMP_ONLY 1
#define MAP_MODE_LCKFB_REFLECT_X 2
typedef struct { int unused; } lv_indev_t;
typedef struct { int state; struct { int x, y; } point; bool continue_reading; } lv_indev_data_t;
typedef struct lv_indev_drv_t {
    int type;
    void (*read_cb)(struct lv_indev_drv_t *, lv_indev_data_t *);
} lv_indev_drv_t;
static lv_indev_t stub_device;
static int resets;
static void lv_indev_drv_init(lv_indev_drv_t *d) { memset(d, 0, sizeof(*d)); }
static lv_indev_t *lv_indev_drv_register(lv_indev_drv_t *d) { (void)d; return &stub_device; }
static lv_indev_t *lv_indev_get_act(void) { return &stub_device; }
static void lv_indev_reset(lv_indev_t *d, void *obj) { (void)d; (void)obj; ++resets; }
#include "../little/src/ui/lvgl_port/k230/lv_port_indev.c"

static void event(int fd, int type, int code, int value)
{
    struct input_event e = {0};
    e.type = type; e.code = code; e.value = value;
    assert(write(fd, &e, sizeof(e)) == sizeof(e));
}

int main(void)
{
    int fds[2];
    lv_indev_data_t data = {0};
    assert(pipe(fds) == 0);
    assert(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    touchpad_evdev.evdev_fd = fds[0];
    touchpad_evdev.use_abs_xy = 1;
    touchpad_evdev.abs_x_max = 479;
    touchpad_evdev.abs_y_max = 799;
    input_map_config(MAP_MODE_CLAMP_ONLY, 480, 800);
    event(fds[1], EV_ABS, ABS_X, 479);
    event(fds[1], EV_ABS, ABS_Y, 799);
    event(fds[1], EV_KEY, BTN_TOUCH, 1);
    event(fds[1], EV_SYN, SYN_REPORT, 0);
    event(fds[1], EV_KEY, BTN_TOUCH, 0);
    event(fds[1], EV_SYN, SYN_REPORT, 0);
    touchpad_read(NULL, &data);
    assert(data.state == LV_INDEV_STATE_PR && data.continue_reading);
    assert(data.point.x == 479 && data.point.y == 799);
    touchpad_read(NULL, &data);
    assert(data.state == LV_INDEV_STATE_REL);
    touchpad_read(NULL, &data);
    assert(!data.continue_reading);
    assert(touchpad_scale_axis(0, 0, 479, 480) == 0);
    assert(touchpad_scale_axis(240, 0, 479, 480) == 240);
    event(fds[1], EV_SYN, SYN_DROPPED, 0);
    event(fds[1], EV_KEY, BTN_TOUCH, 1);
    event(fds[1], EV_SYN, SYN_REPORT, 0);
    touchpad_read(NULL, &data);
    assert(resets == 1 && data.state == LV_INDEV_STATE_REL);
    assert(touch_wait_release); /* Pipe cannot supply an ioctl snapshot. */
    close(fds[0]); close(fds[1]);
    puts("Touch: short press preserved, identity mapping, dropped-frame cancellation passed");
    return 0;
}
