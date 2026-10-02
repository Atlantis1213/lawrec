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

#include "msg_proc.h"
#include "../../common/lawrec_preview_wire.h"
#include "../../common/lawrec_playback_wire.h"
#include "../../playback/lawrec_playback.h"
#include "ui_common.h"
#include <iostream>
#include <mutex>
#include <atomic>
#include <chrono>
#include <queue>
#include <thread>
#include <sched.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstddef>
#include "k_ipcmsg.h"
#include "../../control/include/lawrec_control.h"
#include "../../record/include/lawrec_record_entry.h"
#include "../../rtsp/include/lawrec_rtsp_entry.h"

using namespace std;
static_assert(offsetof(ui_msg_t, data) % alignof(lawrec_record_status_t) == 0,
              "UI payload must remain aligned for status snapshots");

#define UI_MSG_QUEUE_MAX_COUNT 100
#define IPCMSG_DEV_WAIT_RETRY_US (200 * 1000)
#define IPCMSG_DEV_WAIT_MAX_RETRY 25
#define IPCMSG_CONNECT_RETRY_US (1000 * 1000)
#define LAWREC_IPC_SERVICE_NAME "door_lock"
#define _STR(s) #s
#define STR(s) _STR(s)

typedef struct {
    std::mutex mtx;
    std::queue<ui_msg_t *> msg_q;
} msg_mgt_t;

static msg_mgt_t msg_mgt;
static std::atomic<int> ipcmsg_handle{-1};
static std::atomic<int> ipc_status{0};
static std::atomic<uint32_t> ipc_generation{0};
/* Pin an SDK handle through sends/disconnect; an atomic integer alone cannot do this. */
static std::mutex ipc_transport_lock;

extern "C" int lawrec_playback_display_request(int enabled)
{
    std::lock_guard<std::mutex> lock(ipc_transport_lock);
    int id = ipcmsg_handle.load();
    if (id < 0 || !kd_ipcmsg_is_connect(id)) return -ENOTCONN;
    lawrec_playback_wire_t body = {LAWREC_PLAYBACK_VERSION, (uint32_t)enabled, 1280, 720};
    auto *request = kd_ipcmsg_create_message(0, MSG_CMD_PLAYBACK_DISPLAY, &body, sizeof(body));
    if (!request) return -ENOMEM;
    k_ipcmsg_message_t *response = nullptr;
    int ret = kd_ipcmsg_send_sync(id, request, &response, 3000);
    if (!ret) ret = response ? response->s32RetVal : -EIO;
    if (response) kd_ipcmsg_destroy_message(response);
    kd_ipcmsg_destroy_message(request);
    fprintf(stderr, "[playback] display request enabled=%d result=%d\n", enabled, ret);
    return ret;
}
static uint32_t pending_preview = 0;
static uint32_t preview_sequence = 0;
static uint32_t preview_connection_generation = 0;
static std::chrono::steady_clock::time_point preview_deadline;
static bool display_query_pending = false;
static uint32_t display_query_sequence = 0, display_query_generation = 0;
static uint32_t display_sync_generation = UINT32_MAX;
static std::chrono::steady_clock::time_point display_query_deadline, display_query_retry;

static ui_msg_t *ui_msg_alloc(uint32_t size);
static int ui_msg_put(ui_msg_t *pmsg);
static int msg_send_data(uint32_t cmd, void *payload, uint32_t payload_len,
                         uint32_t *connection_generation = nullptr);

static void apply_preview_rtsp_state(void)
{
    int preview_active = lawrec_control_is_preview_active();
    int rtsp_state = lawrec_control_get_rtsp_state();

    /*
     * 当前产品里 preview 和 RTSP 是耦合关系：
     * 只有大核侧预览真正起来后，RTSP 按钮才允许进入可操作状态。
     */
    if (!preview_active || scr_preview_is_back_pending()) {
        scr_preview_set_rtsp_button_unavailable();
        return;
    }

    switch (rtsp_state) {
    case LAWREC_RTSP_STATE_STARTING:
        scr_preview_set_rtsp_button_starting();
        break;
    case LAWREC_RTSP_STATE_LIVE:
        scr_preview_set_rtsp_button_live();
        break;
    case LAWREC_RTSP_STATE_STOPPING:
        scr_preview_set_rtsp_button_stopping();
        break;
    case LAWREC_RTSP_STATE_FAILED:
    case LAWREC_RTSP_STATE_IDLE:
    default:
        scr_preview_set_rtsp_button_idle();
        break;
    }
}

static void apply_preview_record_state(void)
{
    int preview_active = lawrec_control_is_preview_active();
    int record_state = lawrec_control_get_record_state();

    if (!preview_active || scr_preview_is_back_pending()) {
        scr_preview_set_record_button_unavailable();
        return;
    }

    switch (record_state) {
    case LAWREC_RECORD_STATE_STARTING:
        scr_preview_set_record_button_starting();
        break;
    case LAWREC_RECORD_STATE_RECORDING:
        scr_preview_set_record_button_live();
        break;
    case LAWREC_RECORD_STATE_STOPPING:
        scr_preview_set_record_button_stopping();
        break;
    case LAWREC_RECORD_STATE_FAILED:
    case LAWREC_RECORD_STATE_IDLE:
    default:
        scr_preview_set_record_button_idle();
        break;
    }
}

static int common_msg_proc_helper(ui_cmd_e cmd, int8_t *pdata, uint32_t sequence = 0)
{
    ui_msg_t *pmsg = ui_msg_alloc(sizeof(ui_msg_t));
    if (pmsg == NULL) {
        printf("%s no mem\n", __func__);
        return -1;
    }
    pmsg->cmd = cmd;
    pmsg->result = *pdata;
    memcpy(pmsg->reserve, &sequence, sizeof(sequence));

    return ui_msg_put(pmsg);
}

static int rtsp_status_msg_proc_helper(const lawrec_rtsp_status_t *status)
{
    ui_msg_t *pmsg = ui_msg_alloc(sizeof(ui_msg_t) + sizeof(*status));
    if (pmsg == NULL) {
        printf("%s no mem\n", __func__);
        return -1;
    }

    pmsg->cmd = UI_CMD_RTSP_STATUS;
    pmsg->result = 0;
    memcpy(pmsg->data, status, sizeof(*status));
    return ui_msg_put(pmsg);
}

static int record_status_msg_proc_helper(const lawrec_record_status_t *status)
{
    ui_msg_t *pmsg = ui_msg_alloc(sizeof(ui_msg_t) + sizeof(*status));
    if (pmsg == NULL) {
        printf("%s no mem\n", __func__);
        return -1;
    }

    pmsg->cmd = UI_CMD_RECORD_STATUS;
    pmsg->result = 0;
    memcpy(pmsg->data, status, sizeof(*status));
    return ui_msg_put(pmsg);
}

static int lawrec_rtsp_enqueue_result(ui_cmd_e cmd, int8_t result)
{
    ui_msg_t *pmsg = ui_msg_alloc(sizeof(ui_msg_t));
    if (pmsg == NULL) {
        printf("%s no mem\n", __func__);
        return -1;
    }

    pmsg->cmd = cmd;
    pmsg->result = result;
    return ui_msg_put(pmsg);
}

static int lawrec_record_enqueue_result(ui_cmd_e cmd, int8_t result)
{
    ui_msg_t *pmsg = ui_msg_alloc(sizeof(ui_msg_t));
    if (pmsg == NULL) {
        printf("%s no mem\n", __func__);
        return -1;
    }

    pmsg->cmd = cmd;
    pmsg->result = result;
    return ui_msg_put(pmsg);
}

static int lawrec_rtsp_handle_local_cmd(uint32_t cmd)
{
    lawrec_service_response_t resp;
    lawrec_service_cmd_e service_cmd;

    switch (cmd) {
    case MSG_CMD_RTSP_START:
        service_cmd = LAWREC_SERVICE_CMD_RTSP_START;
        break;
    case MSG_CMD_RTSP_STOP:
        service_cmd = LAWREC_SERVICE_CMD_RTSP_STOP;
        break;
    case MSG_CMD_RTSP_QUERY:
        service_cmd = LAWREC_SERVICE_CMD_RTSP_QUERY;
        break;
    default:
        return 1;
    }

    /*
     * RTSP 生命周期现在完全由小核负责，
     * 所以这些命令在本地处理，不再经过大小核 IPC 往大核转发。
     */
    printf("LAWREC-UI: local rtsp cmd=%u\n", cmd);
    lawrec_control_handle_rtsp_cmd(service_cmd, &resp);

    if (resp.magic != LAWREC_SERVICE_MSG_MAGIC ||
        resp.version != LAWREC_SERVICE_MSG_VERSION) {
        if (cmd == MSG_CMD_RTSP_START)
            lawrec_rtsp_enqueue_result(UI_CMD_RTSP_START_RESULT, -1);
        else if (cmd == MSG_CMD_RTSP_STOP)
            lawrec_rtsp_enqueue_result(UI_CMD_RTSP_STOP_RESULT, -1);
        return -1;
    }

    if (cmd == MSG_CMD_RTSP_START)
        lawrec_rtsp_enqueue_result(UI_CMD_RTSP_START_RESULT, resp.result == 0 ? 0 : -1);
    else if (cmd == MSG_CMD_RTSP_STOP)
        lawrec_rtsp_enqueue_result(UI_CMD_RTSP_STOP_RESULT, resp.result == 0 ? 0 : -1);

    rtsp_status_msg_proc_helper(&resp.rtsp);
    return resp.result == 0 ? 0 : -1;
}

static int lawrec_record_handle_local_cmd(uint32_t cmd)
{
    lawrec_service_response_t resp;
    lawrec_service_cmd_e service_cmd;

    switch (cmd) {
    case MSG_CMD_RECORD_START:
        service_cmd = LAWREC_SERVICE_CMD_RECORD_START;
        break;
    case MSG_CMD_RECORD_STOP:
        service_cmd = LAWREC_SERVICE_CMD_RECORD_STOP;
        break;
    case MSG_CMD_RECORD_QUERY:
        service_cmd = LAWREC_SERVICE_CMD_RECORD_QUERY;
        break;
    default:
        return 1;
    }

    printf("LAWREC-UI: local record cmd=%u\n", cmd);
    lawrec_control_handle_record_cmd(service_cmd, &resp);

    if (resp.magic != LAWREC_SERVICE_MSG_MAGIC ||
        resp.version != LAWREC_SERVICE_MSG_VERSION) {
        if (cmd == MSG_CMD_RECORD_START)
            lawrec_record_enqueue_result(UI_CMD_RECORD_START_RESULT, -1);
        else if (cmd == MSG_CMD_RECORD_STOP)
            lawrec_record_enqueue_result(UI_CMD_RECORD_STOP_RESULT, -1);
        return -1;
    }

    if (cmd == MSG_CMD_RECORD_START)
        lawrec_record_enqueue_result(UI_CMD_RECORD_START_RESULT, resp.result == 0 ? 0 : -1);
    else if (cmd == MSG_CMD_RECORD_STOP)
        lawrec_record_enqueue_result(UI_CMD_RECORD_STOP_RESULT, resp.result == 0 ? 0 : -1);

    record_status_msg_proc_helper(&resp.record);
    return resp.result == 0 ? 0 : -1;
}

static std::mutex local_cmd_lock;
static std::queue<uint32_t> local_commands;
static void enqueue_local_command(uint32_t cmd)
{
    std::lock_guard<std::mutex> lock(local_cmd_lock);
    if (local_commands.size() >= 32) {
        int8_t failed = -1;
        ui_cmd_e result = cmd == MSG_CMD_RECORD_START ? UI_CMD_RECORD_START_RESULT :
                          cmd == MSG_CMD_RECORD_STOP ? UI_CMD_RECORD_STOP_RESULT :
                          cmd == MSG_CMD_RTSP_START ? UI_CMD_RTSP_START_RESULT : UI_CMD_RTSP_STOP_RESULT;
        common_msg_proc_helper(result, &failed);
        return;
    }
    local_commands.push(cmd);
}
static void lawrec_rtsp_handle_local_cmd_async(uint32_t cmd) { enqueue_local_command(cmd); }
static void lawrec_record_handle_local_cmd_async(uint32_t cmd) { enqueue_local_command(cmd); }

static void* thread_local_rtsp_status(void* arg)
{
    int last_state = -1;
    int last_preview = -1;
    int last_record = -1;
    unsigned poll_count = 0;

    (void)arg;

    while (1) {
        uint32_t command = UINT32_MAX;
        {
            std::lock_guard<std::mutex> lock(local_cmd_lock);
            if (!local_commands.empty()) { command = local_commands.front(); local_commands.pop(); }
        }
        if (command != UINT32_MAX) {
            if (lawrec_rtsp_handle_local_cmd(command) == 1)
                lawrec_record_handle_local_cmd(command);
        }
        /*
         * 轮询本地 preview/RTSP 状态，只在状态变化时向 UI 发消息，
         * 这样既能保持控件同步，也不会无意义地频繁刷新界面。
         */
        int current_state = lawrec_control_get_rtsp_state();
        int current_preview = lawrec_control_is_preview_active();
        int current_record = lawrec_control_get_record_state();

        if (current_state != last_state || current_preview != last_preview ||
            current_record != last_record || (++poll_count % 5 == 0 && current_record == LAWREC_RECORD_STATE_RECORDING)) {
            lawrec_service_response_t resp;

            if (lawrec_control_handle_rtsp_cmd(LAWREC_SERVICE_CMD_RTSP_QUERY,
                                               &resp) == 0 &&
                resp.magic == LAWREC_SERVICE_MSG_MAGIC &&
                resp.version == LAWREC_SERVICE_MSG_VERSION) {
                rtsp_status_msg_proc_helper(&resp.rtsp);
            }
            if (lawrec_control_handle_record_cmd(LAWREC_SERVICE_CMD_RECORD_QUERY,
                                                 &resp) == 0 &&
                resp.magic == LAWREC_SERVICE_MSG_MAGIC &&
                resp.version == LAWREC_SERVICE_MSG_VERSION) {
                record_status_msg_proc_helper(&resp.record);
            }
            last_state = current_state;
            last_preview = current_preview;
            last_record = current_record;
        }

        usleep(200 * 1000);
    }

    return NULL;
}

static void msg_recv(int handle, k_ipcmsg_message_t* msg)
{
    if (handle != ipcmsg_handle.load() || ipc_status.load() != 1) return;
    if (!msg || !msg->pBody || msg->u32BodyLen < 1) {
        printf("IPCMSG: reject empty response\n");
        return;
    }
    /* 大核回包在这里转换成 UI 线程可消费的队列消息。 */
    switch (msg->u32CMD) {
    case MSG_CMD_DISPLAY_STATUS: {
        if (msg->u32BodyLen != sizeof(lawrec_display_status_t)) break;
        lawrec_display_status_t status;
        memcpy(&status, msg->pBody, sizeof(status));
        if (status.version != LAWREC_DISPLAY_STATUS_VERSION || status.backend_ready > 1 ||
            status.preview_enabled > 1 || status.preview_bound > 1 || status.playback_enabled > 1) {
            printf("IPCMSG: reject display status fields\n");
            break;
        }
        ui_msg_t *event = ui_msg_alloc(sizeof(ui_msg_t) + sizeof(status));
        if (event) {
            event->cmd = UI_CMD_DISPLAY_STATUS;
            memcpy(event->data, &status, sizeof(status));
            ui_msg_put(event);
        }
        break;
    }
    case MSG_CMD_SIGNUP_RESULT:
        common_msg_proc_helper(UI_CMD_SIGNUP_RESULT, (int8_t *)(msg->pBody));
    break;
    case MSG_CMD_IMPORT_RESULT:
        common_msg_proc_helper(UI_CMD_IMPORT_RESULT, (int8_t *)(msg->pBody));
    break;
    case MSG_CMD_DELETE_RESULT:
        common_msg_proc_helper(UI_CMD_DELETE_RESULT, (int8_t *)(msg->pBody));
    break;
    case MSG_CMD_PREVIEW_ENTER_RESULT:
    case MSG_CMD_PREVIEW_EXIT_RESULT:
    case MSG_CMD_DISPLAY_RECOVER_RESULT: {
        if (msg->u32BodyLen != sizeof(lawrec_preview_wire_t)) {
            printf("IPCMSG: reject preview body cmd=%u bytes=%u\n", msg->u32CMD, msg->u32BodyLen);
            break;
        }
        lawrec_preview_wire_t wire;
        memcpy(&wire, msg->pBody, sizeof(wire));
        if (wire.version != LAWREC_PREVIEW_WIRE_VERSION) {
            printf("IPCMSG: reject preview version=%u\n", wire.version);
            break;
        }
        printf("IPCMSG: preview response cmd=%u seq=%u remote_result=%d\n",
               msg->u32CMD, wire.sequence, wire.result);
        if (msg->u32CMD == MSG_CMD_DISPLAY_RECOVER_RESULT) {
            ui_msg_t *event = ui_msg_alloc(sizeof(ui_msg_t));
            if (event) {
                event->cmd = UI_CMD_DISPLAY_RECOVER_RESULT; event->result = wire.result;
                memcpy(event->reserve, &wire.sequence, sizeof(wire.sequence));
                ui_msg_put(event);
            }
            break;
        }
        int8_t result = wire.result ? -1 : 0;
        common_msg_proc_helper(msg->u32CMD == MSG_CMD_PREVIEW_ENTER_RESULT ?
                              UI_CMD_PREVIEW_ENTER_RESULT : UI_CMD_PREVIEW_EXIT_RESULT,
                              &result, wire.sequence);
        break;
    }
    case MSG_CMD_PING_RESULT:
        common_msg_proc_helper(UI_CMD_PING_RESULT, (int8_t *)(msg->pBody));
    break;
    case MSG_CMD_RTSP_START_RESULT:
        common_msg_proc_helper(UI_CMD_RTSP_START_RESULT, (int8_t *)(msg->pBody));
    break;
    case MSG_CMD_RTSP_STOP_RESULT:
        common_msg_proc_helper(UI_CMD_RTSP_STOP_RESULT, (int8_t *)(msg->pBody));
    break;
    case MSG_CMD_RTSP_STATUS:
        if (msg->u32BodyLen >= sizeof(lawrec_rtsp_status_t))
            rtsp_status_msg_proc_helper((const lawrec_rtsp_status_t *)(msg->pBody));
    break;
    case MSG_CMD_RECORD_START_RESULT:
        common_msg_proc_helper(UI_CMD_RECORD_START_RESULT, (int8_t *)(msg->pBody));
    break;
    case MSG_CMD_RECORD_STOP_RESULT:
        common_msg_proc_helper(UI_CMD_RECORD_STOP_RESULT, (int8_t *)(msg->pBody));
    break;
    case MSG_CMD_RECORD_STATUS:
        if (msg->u32BodyLen >= sizeof(lawrec_record_status_t))
            record_status_msg_proc_helper((const lawrec_record_status_t *)(msg->pBody));
    break;
    case MSG_CMD_FEATURE_SAVE: {
        if (msg->u32BodyLen < 8) break;
        uint32_t phyaddr, length;
        memcpy(&phyaddr, msg->pBody, 4);
        memcpy(&length, (char *)msg->pBody + 4, 4);
        feature_db_save(phyaddr, length);
        break;
    }
    default:
    break;
    }
}

static void* thread_ipcmsg(void*)
{
    int handle;
    int fd;
    int retry;
    int ret;

    while (1) {
        /* 先等 /dev/ipcm_user 节点出现，再尝试发起连接。 */
        for (retry = 0; retry < IPCMSG_DEV_WAIT_MAX_RETRY; retry++) {
            fd = open("/dev/ipcm_user", O_RDWR);
            if (fd >= 0) {
                close(fd);
                break;
            }
            usleep(IPCMSG_DEV_WAIT_RETRY_US);
        }

        if (retry >= IPCMSG_DEV_WAIT_MAX_RETRY) {
            printf("IPCMSG: /dev/ipcm_user not ready, retry later\n");
            ipc_status.store(0);
            sleep(1);
            continue;
        }

        printf("IPCMSG: connecting %s\n", LAWREC_IPC_SERVICE_NAME);
        if (kd_ipcmsg_connect(&handle, LAWREC_IPC_SERVICE_NAME, msg_recv)) {
            printf("IPCMSG: connect failed, retry later\n");
            ipc_status.store(-1);
            usleep(IPCMSG_CONNECT_RETRY_US);
            continue;
        }

        printf("IPCMSG: connected\n");
        {
            std::lock_guard<std::mutex> lock(ipc_transport_lock);
            ipcmsg_handle = handle;
            ++ipc_generation;
            ipc_status.store(1);
        }

        {
            char tmp = 0;
            msg_send_data(MSG_CMD_PING, &tmp, 1);
        }
        {
            char tmp = 0;
            msg_send_data(MSG_CMD_RTSP_QUERY, &tmp, 1);
        }
        {
            char tmp = 0;
            msg_send_data(MSG_CMD_RECORD_QUERY, &tmp, 1);
        }

        kd_ipcmsg_run(handle);
        {
            std::lock_guard<std::mutex> lock(ipc_transport_lock);
            ipcmsg_handle = -1;
            ipc_status.store(-1);
            ++ipc_generation;
            ret = kd_ipcmsg_disconnect(handle);
        }
        if (ret != 0)
            printf("IPCMSG: disconnect failed: %d\n", ret);
        printf("IPCMSG: disconnected, retry later\n");
        usleep(IPCMSG_CONNECT_RETRY_US);
    }

    return NULL;
}


static int msg_send_data(uint32_t cmd, void *payload, uint32_t payload_len,
                         uint32_t *connection_generation)
{
    k_ipcmsg_message_t* pReq;

    if (cmd == MSG_CMD_PREVIEW_ENTER)
        lawrec_control_note_preview_request(1);
    else if (cmd == MSG_CMD_PREVIEW_EXIT)
        lawrec_control_note_preview_request(0);

    printf("LAWREC-UI: send cmd=%u ipc=%d\n", cmd, ipcmsg_handle.load());

    if (cmd == MSG_CMD_RTSP_START || cmd == MSG_CMD_RTSP_STOP ||
        cmd == MSG_CMD_RTSP_QUERY) {
        /* RTSP 命令在这里直接短路到本地 control 处理。 */
        lawrec_rtsp_handle_local_cmd_async(cmd);
        return 0;
    }
    if (cmd == MSG_CMD_RECORD_START || cmd == MSG_CMD_RECORD_STOP ||
        cmd == MSG_CMD_RECORD_QUERY) {
        lawrec_record_handle_local_cmd_async(cmd);
        return 0;
    }

    std::lock_guard<std::mutex> lock(ipc_transport_lock);
    const int handle = ipcmsg_handle.load();
    if (connection_generation) *connection_generation = ipc_generation.load();
    if (handle < 0 || ipc_status.load() != 1 || !kd_ipcmsg_is_connect(handle)) {
        printf("LAWREC-UI: ipc not connected for cmd=%u\n", cmd);
        return -ENOTCONN;
    }

    pReq = kd_ipcmsg_create_message(0, cmd, payload,
        payload_len);
    if (pReq == NULL) {
        printf("LAWREC-UI: create message failed cmd=%u\n", cmd);
        return -ENOMEM;
    }
    int ret = kd_ipcmsg_send_only(handle, pReq);
    kd_ipcmsg_destroy_message(pReq);
    if (ret) printf("IPCMSG: send failed cmd=%u ret=%d\n", cmd, ret);
    return ret;
}


static ui_msg_t *ui_msg_alloc(uint32_t size)
{
    ui_msg_t *msg;

    msg = (ui_msg_t *)calloc(1, size);

    return msg;
}

static int ui_msg_free(ui_msg_t *pmsg)
{
    if (pmsg) {
        free(pmsg);
        pmsg = NULL;
    }

    return 0;
}

static int ui_msg_get(ui_msg_t **ppmsg)
{
    int ret = 0;

    msg_mgt.mtx.lock();
    if (msg_mgt.msg_q.empty()) {
        ret = -1;
    } else {
        *ppmsg = msg_mgt.msg_q.front();
        msg_mgt.msg_q.pop();
    }
    msg_mgt.mtx.unlock();

    return ret;
}

static int ui_msg_put(ui_msg_t *pmsg)
{
    int ret = 0;

    msg_mgt.mtx.lock();
    if (msg_mgt.msg_q.size() >= UI_MSG_QUEUE_MAX_COUNT) {
        printf("%s ui msg more than max\n", __func__);
        ui_msg_free(pmsg);
        ret = -1;
    } else {
        msg_mgt.msg_q.push(pmsg);
    }
    msg_mgt.mtx.unlock();

    return ret;
}

static void scr_preview_continue_pending_exit_if_needed(void)
{
    if (!scr_preview_is_back_pending() || pending_preview || ipc_status.load() != 1)
        return;

    if (lawrec_control_get_rtsp_state() == LAWREC_RTSP_STATE_STARTING ||
        lawrec_control_get_rtsp_state() == LAWREC_RTSP_STATE_LIVE ||
        lawrec_control_get_rtsp_state() == LAWREC_RTSP_STATE_STOPPING)
        return;
    if (lawrec_control_get_record_state() == LAWREC_RECORD_STATE_STARTING ||
        lawrec_control_get_record_state() == LAWREC_RECORD_STATE_RECORDING ||
        lawrec_control_get_record_state() == LAWREC_RECORD_STATE_STOPPING)
        return;

    scr_preview_set_status("预览关闭中", lv_color_hex(0xffd166));
    msg_send_cmd(MSG_CMD_PREVIEW_EXIT);
}

/* UI-thread only. Transport failure/timeout cannot certify the big core's state. */
static void finish_preview_request(uint32_t command, int result)
{
    pending_preview = 0;
    printf("IPCMSG: preview result cmd=%u seq=%u generation=%u result=%d\n",
           command, preview_sequence, preview_connection_generation, result);
    if (command == MSG_CMD_DISPLAY_RECOVER) {
        lawrec_control_finish_display_recovery(result);
        apply_preview_rtsp_state(); apply_preview_record_state();
        scr_maintenance_recovery_result(result);
        return;
    }
    if (result == 0)
        lawrec_control_note_preview_result(command == MSG_CMD_PREVIEW_ENTER);
    else
        lawrec_control_note_preview_uncertain();
    apply_preview_rtsp_state();
    apply_preview_record_state();
    if (command == MSG_CMD_PREVIEW_ENTER) {
        scr_preview_set_status(result == 0 ? "预览已就绪" : "预览启动失败",
                               lv_color_hex(result == 0 ? 0x4ade80 : 0xff6b6b));
    } else {
        scr_main_set_status(result == 0 ? "预览已关闭" : "预览关闭失败",
                            lv_color_hex(result == 0 ? 0x6fdcff : 0xff6b6b));
        if (scr_preview_is_back_pending()) {
            scr_preview_clear_back_pending();
            if (result == 0) jump_to_scr_main();
            else scr_preview_set_status("预览关闭失败", lv_color_hex(0xff6b6b));
        }
    }
}

static void poll_display_sync(void)
{
    const auto now = std::chrono::steady_clock::now();
    if (display_query_pending && now >= display_query_deadline) {
        display_query_pending = false;
        display_query_retry = now + std::chrono::seconds(5);
        printf("IPCMSG: display query timeout; state remains unverified\n");
        scr_main_set_status("IPC sync failed", lv_color_hex(0xff6b6b));
    }
    if (ipc_status.load() != 1 || pending_preview || display_query_pending ||
        display_sync_generation == ipc_generation.load() || now < display_query_retry ||
        lawrec_playback_active()) return;
    const int rtsp = lawrec_control_get_rtsp_state(), record = lawrec_control_get_record_state();
    if (rtsp == LAWREC_RTSP_STATE_STARTING || rtsp == LAWREC_RTSP_STATE_LIVE || rtsp == LAWREC_RTSP_STATE_STOPPING ||
        record == LAWREC_RECORD_STATE_STARTING || record == LAWREC_RECORD_STATE_RECORDING || record == LAWREC_RECORD_STATE_STOPPING) return;
    lawrec_preview_wire_t request{LAWREC_PREVIEW_WIRE_VERSION, ++display_query_sequence, 0};
    int ret = msg_send_data(MSG_CMD_DISPLAY_QUERY, &request, sizeof(request), &display_query_generation);
    if (ret) display_query_retry = now + std::chrono::seconds(5);
    else {
        display_query_pending = true;
        display_query_deadline = now + std::chrono::seconds(5);
        scr_main_set_status("IPC syncing", lv_color_hex(0xffd166));
    }
}

#ifdef __cplusplus
extern "C" {
#endif

int msg_proc_init(void)
{
    int ret;
    k_ipcmsg_connect_t stConnectAttr;
    pthread_t tid_thread_ipcmsg;
    pthread_t tid_thread_rtsp_status;
    pthread_attr_t tattr_thread_ipcmsg;
    pthread_attr_t tattr_thread_rtsp_status;
    int max_prio;
    struct sched_param sp;

    stConnectAttr.u32RemoteId = 1;
    stConnectAttr.u32Port = 101;
    stConnectAttr.u32Priority = 0;
    ret = kd_ipcmsg_add_service(LAWREC_IPC_SERVICE_NAME, &stConnectAttr);
    if (ret != 0) {
        printf("IPCMSG: add service failed=%d\n", ret);
        return -1;
    }

    pthread_attr_init(&tattr_thread_ipcmsg);
    max_prio = sched_get_priority_max(SCHED_RR);
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = max_prio;
    pthread_attr_setschedpolicy(&tattr_thread_ipcmsg, SCHED_RR);
    pthread_attr_setschedparam(&tattr_thread_ipcmsg, &sp);
    pthread_attr_setinheritsched(&tattr_thread_ipcmsg, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setdetachstate(&tattr_thread_ipcmsg,
                                PTHREAD_CREATE_DETACHED);

    /*
     * We prefer a real-time IPC thread, but on some launch contexts this can
     * fail. Fallback to a normal detached thread instead of silently losing IPC.
     */
    ret = pthread_create(&tid_thread_ipcmsg, &tattr_thread_ipcmsg,
                         thread_ipcmsg, NULL);
    if (ret != 0) {
        printf("IPCMSG: rt thread create failed: %d, fallback to default thread\n", ret);
        pthread_attr_destroy(&tattr_thread_ipcmsg);

        pthread_attr_init(&tattr_thread_ipcmsg);
        pthread_attr_setdetachstate(&tattr_thread_ipcmsg, PTHREAD_CREATE_DETACHED);
        ret = pthread_create(&tid_thread_ipcmsg, &tattr_thread_ipcmsg,
                             thread_ipcmsg, NULL);
        if (ret != 0) {
            printf("IPCMSG: thread create failed: %d\n", ret);
            pthread_attr_destroy(&tattr_thread_ipcmsg);
            return -1;
        }
    }
    pthread_attr_destroy(&tattr_thread_ipcmsg);

    /* Separate watcher thread mirrors local RTSP state back into the UI queue. */
    pthread_attr_init(&tattr_thread_rtsp_status);
    pthread_attr_setdetachstate(&tattr_thread_rtsp_status,
                                PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&tid_thread_rtsp_status, &tattr_thread_rtsp_status,
                         thread_local_rtsp_status, NULL);
    pthread_attr_destroy(&tattr_thread_rtsp_status);
    if (ret != 0) {
        printf("RTSP: status thread create failed: %d\n", ret);
        return -1;
    }

    return 0;
}

int msg_send_cmd(uint32_t cmd)
{
    if ((cmd == MSG_CMD_PREVIEW_ENTER || cmd == MSG_CMD_PREVIEW_EXIT) && lawrec_playback_active()) return -EBUSY;
    char tmp = 0;
    bool preview = cmd == MSG_CMD_PREVIEW_ENTER || cmd == MSG_CMD_PREVIEW_EXIT || cmd == MSG_CMD_DISPLAY_RECOVER;
    if (preview && pending_preview) return -EBUSY;
    if (cmd == MSG_CMD_DISPLAY_RECOVER) {
        const int prepare = lawrec_control_prepare_display_recovery();
        if (prepare) return prepare;
    }
    if (preview) {
        // A user command supersedes the observational startup query.
        display_query_pending = false;
        display_sync_generation = ipc_generation.load();
    }
    lawrec_preview_wire_t wire{LAWREC_PREVIEW_WIRE_VERSION, preview ? ++preview_sequence : 0, 0};
    int ret = preview ? msg_send_data(cmd, &wire, sizeof(wire), &preview_connection_generation) : msg_send_data(cmd, &tmp, 1);
    if (preview) {
        if (!ret) {
            pending_preview = cmd;
            preview_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        } else {
            finish_preview_request(cmd, ret);
        }
    }
    return ret;
}

int msg_send_cmd_with_data(uint32_t cmd, void *payload, uint32_t payload_len)
{
    return msg_send_data(cmd, payload, payload_len);
}

int ui_msg_proc(void)
{
    int ret;
    ui_msg_t *pmsg;

    static int last_ipc_status = 0;
    static uint32_t last_ipc_generation = 0;
    int current_ipc_status = ipc_status.load();
    const uint32_t current_generation = ipc_generation.load();
    // A disconnect/reconnect can occur between UI polls and leave status == 1.
    const bool lost_connection = last_ipc_generation != current_generation && last_ipc_generation != 0;
    if (lost_connection || (current_ipc_status != 1 && current_ipc_status != last_ipc_status)) {
        display_query_pending = false;
        display_sync_generation = UINT32_MAX;
        display_query_retry = std::chrono::steady_clock::time_point();
        const bool recovery_cancelled = pending_preview == MSG_CMD_DISPLAY_RECOVER;
        pending_preview = 0;
        lawrec_playback_stop();
        const bool occupied = lawrec_control_preview_needs_close();
        lawrec_control_note_preview_uncertain();
        if (recovery_cancelled) scr_maintenance_recovery_result(-ENOTCONN);
        if (occupied) {
            scr_preview_set_status("IPC offline", lv_color_hex(0xff6b6b));
        }
        scr_preview_clear_back_pending();
        apply_preview_rtsp_state();
        apply_preview_record_state();
    }
    last_ipc_generation = current_generation;
    if (current_ipc_status != last_ipc_status) {
        last_ipc_status = current_ipc_status;
        scr_main_set_status(current_ipc_status == 1 ? "IPC connected" : "IPC offline",
                            lv_color_hex(current_ipc_status == 1 ? 0x4ade80 : 0xff6b6b));
    }
    if (pending_preview && preview_connection_generation != current_generation) {
        finish_preview_request(pending_preview, -ENOTCONN);
    } else if (pending_preview && std::chrono::steady_clock::now() >= preview_deadline) {
        printf("IPCMSG: preview timeout cmd=%u\n", pending_preview);
        finish_preview_request(pending_preview, -ETIMEDOUT);
    }
    poll_display_sync();
    ret = ui_msg_get(&pmsg);
    if (ret != 0) {
        scr_preview_continue_pending_exit_if_needed();
        return ret;
    }

    /*
     * All asynchronous results, whether they came from big-core IPC or local
     * RTSP control, are serialized through this single UI-thread consumer.
     */
    if (pmsg->cmd == UI_CMD_PREVIEW_ENTER_RESULT || pmsg->cmd == UI_CMD_PREVIEW_EXIT_RESULT ||
        pmsg->cmd == UI_CMD_DISPLAY_RECOVER_RESULT) {
        uint32_t seq;
        memcpy(&seq, pmsg->reserve, sizeof(seq));
        bool expected = (pending_preview == MSG_CMD_PREVIEW_ENTER && pmsg->cmd == UI_CMD_PREVIEW_ENTER_RESULT) ||
                        (pending_preview == MSG_CMD_PREVIEW_EXIT && pmsg->cmd == UI_CMD_PREVIEW_EXIT_RESULT) ||
                        (pending_preview == MSG_CMD_DISPLAY_RECOVER && pmsg->cmd == UI_CMD_DISPLAY_RECOVER_RESULT);
        if (!expected || seq != preview_sequence) {
            printf("IPCMSG: ignore stale preview seq=%u\n", seq);
            ui_msg_free(pmsg); return 0;
        }
    }
    switch (pmsg->cmd) {
    case UI_CMD_DISPLAY_RECOVER_RESULT:
        finish_preview_request(MSG_CMD_DISPLAY_RECOVER, pmsg->result);
        break;
    case UI_CMD_DISPLAY_STATUS: {
        const auto *status = reinterpret_cast<const lawrec_display_status_t *>(pmsg->data);
        if (!display_query_pending || status->sequence != display_query_sequence ||
            display_query_generation != ipc_generation.load() || pending_preview) break;
        display_query_pending = false;
        if (status->result || !status->backend_ready) {
            display_query_retry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            printf("IPCMSG: display sync deferred ready=%u result=%d\n", status->backend_ready, status->result);
            break;
        }
        display_sync_generation = display_query_generation;
        lawrec_control_note_display_status(1, status->preview_enabled || status->preview_bound,
                                          status->playback_enabled);
        printf("IPCMSG: display synchronized preview=%u bound=%u playback=%u; no capture resumed\n",
               status->preview_enabled, status->preview_bound, status->playback_enabled);
        scr_main_set_status(status->playback_enabled ? "回放未关闭" :
                            status->preview_enabled || status->preview_bound ? "预览未关闭" : "IPC ready",
                            lv_color_hex(status->playback_enabled || status->preview_enabled || status->preview_bound ? 0xffd166 : 0x4ade80));
        apply_preview_rtsp_state(); apply_preview_record_state();
        break;
    }
    case UI_CMD_SIGNUP_RESULT:
    case UI_CMD_IMPORT_RESULT:
    case UI_CMD_DELETE_RESULT:
        scr_main_display_result(pmsg->result);
    break;
    case UI_CMD_PREVIEW_ENTER_RESULT:
        finish_preview_request(MSG_CMD_PREVIEW_ENTER, pmsg->result);
    break;
    case UI_CMD_PREVIEW_EXIT_RESULT:
        finish_preview_request(MSG_CMD_PREVIEW_EXIT, pmsg->result);
    break;
    case UI_CMD_PING_RESULT:
        scr_main_set_status(pmsg->result == 0 ? "IPC ready" : "IPC error",
                            pmsg->result == 0 ? lv_color_hex(0x4ade80) : lv_color_hex(0xff6b6b));
    break;
    case UI_CMD_RTSP_START_RESULT:
        apply_preview_rtsp_state();
        scr_preview_set_status(pmsg->result == 0 ? "RTSP启动中" : "RTSP启动失败",
                               pmsg->result == 0 ? lv_color_hex(0x6fdcff)
                                                 : lv_color_hex(0xff6b6b));
        scr_main_set_status(pmsg->result == 0 ? "RTSP启动中" : "RTSP启动失败",
                            pmsg->result == 0 ? lv_color_hex(0x6fdcff)
                                              : lv_color_hex(0xff6b6b));
    break;
    case UI_CMD_RTSP_STOP_RESULT:
        apply_preview_rtsp_state();
        scr_preview_set_status(pmsg->result == 0 ? "RTSP停止中" : "RTSP停止失败",
                               pmsg->result == 0 ? lv_color_hex(0xffd166)
                                                 : lv_color_hex(0xff6b6b));
        scr_main_set_status(pmsg->result == 0 ? "RTSP停止中" : "RTSP停止失败",
                            pmsg->result == 0 ? lv_color_hex(0xffd166)
                                              : lv_color_hex(0xff6b6b));
        if (pmsg->result != 0 && scr_preview_is_back_pending())
            scr_preview_clear_back_pending();
    break;
    case UI_CMD_RECORD_START_RESULT:
        apply_preview_record_state();
        scr_preview_set_status(pmsg->result == 0 ? "录像启动中" : "录像启动失败",
                               pmsg->result == 0 ? lv_color_hex(0x6fdcff)
                                                 : lv_color_hex(0xff6b6b));
        scr_main_set_status(pmsg->result == 0 ? "录像启动中" : "录像启动失败",
                            pmsg->result == 0 ? lv_color_hex(0x6fdcff)
                                              : lv_color_hex(0xff6b6b));
    break;
    case UI_CMD_RECORD_STOP_RESULT:
        apply_preview_record_state();
        scr_preview_set_status(pmsg->result == 0 ? "录像停止中" : "录像停止失败",
                               pmsg->result == 0 ? lv_color_hex(0xffd166)
                                                 : lv_color_hex(0xff6b6b));
        scr_main_set_status(pmsg->result == 0 ? "录像停止中" : "录像停止失败",
                            pmsg->result == 0 ? lv_color_hex(0xffd166)
                                              : lv_color_hex(0xff6b6b));
        if (pmsg->result != 0 && scr_preview_is_back_pending())
            scr_preview_clear_back_pending();
    break;
    case UI_CMD_RTSP_STATUS:
        if (pmsg->data != NULL) {
            const lawrec_rtsp_status_t *status =
                reinterpret_cast<const lawrec_rtsp_status_t *>(pmsg->data);
            char status_text[96];
            int rtsp_state = lawrec_control_get_rtsp_state();
            const char *rtsp_state_text = "idle";

            if (rtsp_state == LAWREC_RTSP_STATE_STARTING)
                rtsp_state_text = "starting";
            else if (rtsp_state == LAWREC_RTSP_STATE_LIVE)
                rtsp_state_text = "live";
            else if (rtsp_state == LAWREC_RTSP_STATE_STOPPING)
                rtsp_state_text = "stopping";
            else if (rtsp_state == LAWREC_RTSP_STATE_FAILED)
                rtsp_state_text = "failed";

            snprintf(status_text, sizeof(status_text), "RTSP %s :%u/%s",
                     rtsp_state_text,
                     status->port,
                     status->stream_name[0] ? status->stream_name : "lawrec");
            apply_preview_rtsp_state();
            scr_main_set_status(status_text,
                                rtsp_state == LAWREC_RTSP_STATE_LIVE
                                    ? lv_color_hex(0x4ade80)
                                : (rtsp_state == LAWREC_RTSP_STATE_FAILED
                                       ? lv_color_hex(0xff6b6b)
                                       : lv_color_hex(0xffd166)));
            scr_preview_set_status(status_text,
                                   rtsp_state == LAWREC_RTSP_STATE_LIVE
                                       ? lv_color_hex(0x4ade80)
                                   : (rtsp_state == LAWREC_RTSP_STATE_FAILED
                                          ? lv_color_hex(0xff6b6b)
                                          : lv_color_hex(0xffd166)));
            if (!status->enabled)
                scr_preview_continue_pending_exit_if_needed();
        }
    break;
    case UI_CMD_RECORD_STATUS:
        if (pmsg->data != NULL) {
            const lawrec_record_status_t *status =
                reinterpret_cast<const lawrec_record_status_t *>(pmsg->data);
            char status_text[160];
            int record_state = lawrec_control_get_record_state();
            const char *record_state_text = "idle";

            if (record_state == LAWREC_RECORD_STATE_STARTING)
                record_state_text = "starting";
            else if (record_state == LAWREC_RECORD_STATE_RECORDING)
                record_state_text = "recording";
            else if (record_state == LAWREC_RECORD_STATE_STOPPING)
                record_state_text = "stopping";
            else if (record_state == LAWREC_RECORD_STATE_FAILED)
                record_state_text = "failed";

            snprintf(status_text, sizeof(status_text), "REC %s %llus %.1fMiB free %.0fMiB err=%d",
                     record_state_text, (unsigned long long)(status->elapsed_ms / 1000),
                     status->bytes_written / 1048576.0, status->free_bytes / 1048576.0,
                     status->last_error);
            apply_preview_record_state();
            scr_main_set_status(status_text,
                                record_state == LAWREC_RECORD_STATE_RECORDING
                                    ? lv_color_hex(0xff6b6b)
                                : (record_state == LAWREC_RECORD_STATE_FAILED
                                       ? lv_color_hex(0xff6b6b)
                                       : lv_color_hex(0xffd166)));
            scr_preview_set_status(status_text,
                                   record_state == LAWREC_RECORD_STATE_RECORDING
                                       ? lv_color_hex(0xff6b6b)
                                   : (record_state == LAWREC_RECORD_STATE_FAILED
                                          ? lv_color_hex(0xff6b6b)
                                          : lv_color_hex(0xffd166)));
            if (!status->enabled)
                scr_preview_continue_pending_exit_if_needed();
        }
    break;
    }
    ui_msg_free(pmsg);
    scr_preview_continue_pending_exit_if_needed();

    return ret;
}

#ifdef __cplusplus
}
#endif
