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

#include "ui_common.h"
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include "lv_port.h"
#include "key_proc.h"
#include "../../control/include/lawrec_control.h"

static int set_priority(void)
{
    int max_prio = sched_get_priority_max(SCHED_RR);
    int min_prio = sched_get_priority_min(SCHED_RR);
    struct sched_param sp = {(max_prio + min_prio) / 4};

    return pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
}

static int set_mallopt(void)
{
    /* 在小核 Buildroot 运行时里尽量保持分配器行为稳定可预测。 */
    mallopt(M_TRIM_THRESHOLD, 128 * 1024);
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    mallopt(M_MMAP_MAX, 1024);

    return 0;
}

static void setup_log_streams(void)
{
    FILE *fp;

    /*
     * UI 日志故意重定向到小核统一日志中。
     * 现场调试时，UI/service/control 共用一条时间线会更容易排查问题。
     */
    fp = freopen("/tmp/lawrec-service.log", "a", stdout);
    if (fp != NULL)
        setvbuf(stdout, NULL, _IOLBF, 0);

    fp = freopen("/tmp/lawrec-service.log", "a", stderr);
    if (fp != NULL)
        setvbuf(stderr, NULL, _IOLBF, 0);
}

int main(void)
{
    set_mallopt();
    setup_log_streams();
    fprintf(stderr, "lawrec ui build: 2026-04-15 ui-layout-v9 touch-off dhcp\n");

    /*
     * UI 侧也要初始化 control，
     * 因为预览页和 RTSP 按钮会在异步消息前后直接读取共享状态。
     */
    lawrec_control_set_log_path("/tmp/lawrec-service.log");
    lawrec_control_init();
    lv_init();
    lv_port_disp_init();
    fprintf(stderr, "lawrec indev: touch disabled\n");

    setup_scr_scr_main();
    lawrec_key_init();
    msg_proc_init();
    set_priority();
    jump_to_scr_main();
    /*
     * 主循环故意保持简单：
     * 1. 驱动 LVGL 的绘制和定时器
     * 2. 轮询 GPIO 按键并转成 UI 动作
     * 3. 处理异步 IPC/service 结果并刷新界面状态
     */
    while (1) {
        lv_timer_handler();
        lawrec_key_poll();
        if (ui_msg_proc() < 0)
            usleep(1 * 1000);
    }

    return 0;
}
