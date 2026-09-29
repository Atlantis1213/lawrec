#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mutex>

#include "../../common/lawrec_config.h"
#include "../../common/lawrec_storage.h"
#include "../../record/include/lawrec_record_entry.h"
#include "../../rtsp/include/lawrec_rtsp_entry.h"
#include "../include/lawrec_control.h"

static const char *kDefaultStreamName = LAWREC_RTSP_DEFAULT_STREAM_NAME;
static const char *kDefaultLogPath = LAWREC_LOG_PATH;

static std::mutex g_control_lock;
static const char *g_log_path = kDefaultLogPath;
static int g_initialized = 0;
/* UI 已经发起预览进入请求，但大核可能还没有真正返回成功。 */
static int g_preview_requested = 0;
/* 大核已经明确返回：预览链路已真正启动并可用。 */
static int g_preview_active = 0;
/* 面向用户的目标状态：在条件满足时 RTSP 应该处于开启态。 */
static int g_rtsp_enabled = 0;
/* 录像状态与 RTSP 平级，由 control 统一收敛。 */
static int g_record_enabled = 0;

static int rtsp_state_active(int state)
{
    return state == LAWREC_RTSP_STATE_STARTING ||
           state == LAWREC_RTSP_STATE_LIVE ||
           state == LAWREC_RTSP_STATE_STOPPING;
}

static lawrec_rtsp_config_t default_rtsp_config(void)
{
    lawrec_rtsp_config_t config;

    /* 将当前默认码流参数集中放在一处，便于 UI 和 service 复用。 */
    config.sensor_type = LAWREC_DEFAULT_SENSOR_TYPE;
    config.session_num = LAWREC_RTSP_DEFAULT_SESSION_NUM;
    config.video_type = LAWREC_RTSP_DEFAULT_VIDEO_TYPE;
    config.video_width = LAWREC_RTSP_DEFAULT_WIDTH;
    config.video_height = LAWREC_RTSP_DEFAULT_HEIGHT;
    config.audio_input_type = LAWREC_RTSP_DEFAULT_AUDIO_INPUT;
    return config;
}

static lawrec_record_config_t default_record_config(void)
{
    lawrec_record_config_t config;

    config.sensor_type = LAWREC_DEFAULT_SENSOR_TYPE;
    config.video_type = LAWREC_RTSP_DEFAULT_VIDEO_TYPE;
    config.video_width = LAWREC_RTSP_DEFAULT_WIDTH;
    config.video_height = LAWREC_RTSP_DEFAULT_HEIGHT;
    config.output_dir = LAWREC_RECORD_DEFAULT_OUTPUT_DIR;
    config.file_prefix = LAWREC_RECORD_DEFAULT_PREFIX;
    return config;
}

static void log_line(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;
    time_t now;
    struct tm tm_now;
    char buf[32];

    fp = fopen(g_log_path, "a");
    if (fp == NULL)
        return;

    now = time(NULL);
    localtime_r(&now, &tm_now);
    strftime(buf, sizeof(buf), "%F %T", &tm_now);
    fprintf(fp, "[lawrec-control] %s ", buf);

    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
    fclose(fp);
}

static void fill_rtsp_status(lawrec_rtsp_status_t *status)
{
    int rtsp_state = lawrec_rtsp_get_state();

    memset(status, 0, sizeof(*status));
    /* 当前 UI 只关心一个粗粒度启用标记和基础推流端点信息。 */
    status->enabled =
        rtsp_state_active(rtsp_state) ? 1 : 0;
    status->state = (uint32_t)rtsp_state;
    status->last_error = lawrec_rtsp_get_last_error();
    status->port = LAWREC_RTSP_DEFAULT_PORT;
    snprintf(status->stream_name, sizeof(status->stream_name), "%s",
             kDefaultStreamName);
}

static void fill_record_status(lawrec_record_status_t *status)
{
    int record_state = lawrec_record_get_state();

    memset(status, 0, sizeof(*status));
    status->enabled = (record_state == LAWREC_RECORD_STATE_STARTING ||
                       record_state == LAWREC_RECORD_STATE_RECORDING ||
                       record_state == LAWREC_RECORD_STATE_STOPPING)
                          ? 1
                          : 0;
    status->state = (uint32_t)record_state;
    status->last_error = lawrec_record_get_last_error();
    lawrec_record_copy_last_path(status->file_path, sizeof(status->file_path));
    lawrec_record_get_progress(&status->elapsed_ms, &status->bytes_written);
    lawrec_storage_check(&status->free_bytes);
}

extern "C" int lawrec_control_init(void)
{
    std::lock_guard<std::mutex> guard(g_control_lock);

    if (g_initialized)
        return 0;

    g_initialized = 1;
    log_line("control init");
    return 0;
}

extern "C" void lawrec_control_deinit(void)
{
    std::lock_guard<std::mutex> guard(g_control_lock);

    if (!g_initialized)
        return;

    if (rtsp_state_active(lawrec_rtsp_get_state())) {
        lawrec_rtsp_stop_async();
        log_line("control deinit stop rtsp");
    }
    if (lawrec_record_get_state() == LAWREC_RECORD_STATE_STARTING ||
        lawrec_record_get_state() == LAWREC_RECORD_STATE_RECORDING ||
        lawrec_record_get_state() == LAWREC_RECORD_STATE_STOPPING) {
        lawrec_record_stop_async();
        log_line("control deinit stop record");
    }
    g_preview_requested = 0;
    g_preview_active = 0;
    g_rtsp_enabled = 0;
    g_record_enabled = 0;
    g_initialized = 0;
}

extern "C" void lawrec_control_set_log_path(const char *path)
{
    std::lock_guard<std::mutex> guard(g_control_lock);

    if (path != NULL && path[0] != '\0')
        g_log_path = path;
    else
        g_log_path = kDefaultLogPath;
}

extern "C" void lawrec_control_note_preview_request(int enabled)
{
    std::lock_guard<std::mutex> guard(g_control_lock);

    /*
     * 这里记录的是 UI 侧的“乐观请求边沿”。
     * 一旦 UI 请求退出预览，就必须同步拉停 RTSP，因为推流依赖底层预览/采集链路存活。
     */
    g_preview_requested = enabled ? 1 : 0;
    if (!enabled) {
        g_preview_active = 0;
        if (rtsp_state_active(lawrec_rtsp_get_state())) {
            lawrec_rtsp_stop_async();
            g_rtsp_enabled = 0;
            log_line("preview request disabled, stop rtsp");
        }
        if (lawrec_record_get_state() == LAWREC_RECORD_STATE_STARTING ||
            lawrec_record_get_state() == LAWREC_RECORD_STATE_RECORDING ||
            lawrec_record_get_state() == LAWREC_RECORD_STATE_STOPPING) {
            lawrec_record_stop_async();
            g_record_enabled = 0;
            log_line("preview request disabled, stop record");
        }
    }
    log_line("preview request enabled=%d", g_preview_requested);
}

extern "C" void lawrec_control_note_preview_result(int enabled)
{
    std::lock_guard<std::mutex> guard(g_control_lock);

    /*
     * 这里记录的是来自大核的“最终确认边沿”。
     * 只有大核明确返回预览已启动后，RTSP 才允许真正开启；
     * 只要预览失败或退出，RTSP 也必须回到空闲态。
     */
    g_preview_active = enabled ? 1 : 0;
    if (!g_preview_active) {
        g_preview_requested = 0;
        g_rtsp_enabled = 0;
        g_record_enabled = 0;
        if (rtsp_state_active(lawrec_rtsp_get_state())) {
            lawrec_rtsp_stop_async();
            log_line("preview inactive, stop rtsp");
        }
        if (lawrec_record_get_state() == LAWREC_RECORD_STATE_STARTING ||
            lawrec_record_get_state() == LAWREC_RECORD_STATE_RECORDING ||
            lawrec_record_get_state() == LAWREC_RECORD_STATE_STOPPING) {
            lawrec_record_stop_async();
            log_line("preview inactive, stop record");
        }
    }
    log_line("preview result active=%d requested=%d", g_preview_active,
             g_preview_requested);
}

extern "C" int lawrec_control_is_preview_active(void)
{
    std::lock_guard<std::mutex> guard(g_control_lock);
    return g_preview_active;
}

extern "C" int lawrec_control_is_rtsp_enabled(void)
{
    std::lock_guard<std::mutex> guard(g_control_lock);
    return g_rtsp_enabled;
}

extern "C" int lawrec_control_is_rtsp_running(void)
{
    return lawrec_rtsp_is_running();
}

extern "C" int lawrec_control_get_rtsp_state(void)
{
    return lawrec_rtsp_get_state();
}

extern "C" int lawrec_control_is_record_enabled(void)
{
    std::lock_guard<std::mutex> guard(g_control_lock);
    return g_record_enabled;
}

extern "C" int lawrec_control_is_record_running(void)
{
    return lawrec_record_is_running();
}

extern "C" int lawrec_control_get_record_state(void)
{
    return lawrec_record_get_state();
}

extern "C" int lawrec_control_handle_rtsp_cmd(lawrec_service_cmd_e cmd,
                                              lawrec_service_response_t *resp)
{
    lawrec_rtsp_config_t config;
    int ret = 0;

    if (resp == NULL)
        return -EINVAL;

    std::lock_guard<std::mutex> guard(g_control_lock);

    memset(resp, 0, sizeof(*resp));
    resp->magic = LAWREC_SERVICE_MSG_MAGIC;
    resp->version = LAWREC_SERVICE_MSG_VERSION;
    resp->result = -1;
    resp->error_no = 0;

    if (!g_initialized) {
        g_initialized = 1;
        log_line("control init");
    }

    switch (cmd) {
    case LAWREC_SERVICE_CMD_RTSP_START:
        if (lawrec_record_get_state() == LAWREC_RECORD_STATE_STARTING ||
            lawrec_record_get_state() == LAWREC_RECORD_STATE_RECORDING ||
            lawrec_record_get_state() == LAWREC_RECORD_STATE_STOPPING) {
            resp->result = -EBUSY; resp->error_no = EBUSY;
            snprintf(resp->message, sizeof(resp->message), "record owns encoder");
            break;
        }
        /*
         * 当前产品规则：只有预览已经被大核确认启动后，RTSP 才允许开启。
         * 这样可以避免在摄像头链路尚未打开时误起编码/推流流程。
         */
        if (!g_preview_active) {
            resp->result = -EAGAIN;
            resp->error_no = EAGAIN;
            snprintf(resp->message, sizeof(resp->message),
                     "%s", "preview not ready");
            log_line("rtsp start rejected preview_active=0 requested=%d",
                     g_preview_requested);
            break;
        }

        if (lawrec_rtsp_get_state() == LAWREC_RTSP_STATE_LIVE ||
            lawrec_rtsp_get_state() == LAWREC_RTSP_STATE_STARTING) {
            g_rtsp_enabled = 1;
            resp->result = 0;
            snprintf(resp->message, sizeof(resp->message),
                     "%s", "rtsp already running");
            break;
        }

        config = default_rtsp_config();
        ret = lawrec_rtsp_start_async(&config);
        if (ret != 0) {
            g_rtsp_enabled = 0;
            resp->result = ret;
            resp->error_no = -ret;
            snprintf(resp->message, sizeof(resp->message),
                     "rtsp start failed:%d", ret);
            log_line("rtsp start failed ret=%d", ret);
            break;
        }

        g_rtsp_enabled = 1;
        resp->result = 0;
        snprintf(resp->message, sizeof(resp->message), "%s", "rtsp started");
        log_line("rtsp start in-process sensor=%d stream=%s",
                 config.sensor_type, kDefaultStreamName);
        break;
    case LAWREC_SERVICE_CMD_RTSP_STOP:
        /* stop 故意设计成幂等接口，调用方可以放心重复调用。 */
        if (lawrec_rtsp_get_state() == LAWREC_RTSP_STATE_IDLE ||
            lawrec_rtsp_get_state() == LAWREC_RTSP_STATE_FAILED) {
            g_rtsp_enabled = 0;
            resp->result = 0;
            snprintf(resp->message, sizeof(resp->message),
                     "%s", "rtsp already stopped");
            break;
        }

        ret = lawrec_rtsp_stop_async();
        if (ret != 0) {
            resp->result = ret;
            resp->error_no = -ret;
            snprintf(resp->message, sizeof(resp->message),
                     "rtsp stop failed:%d", ret);
            log_line("rtsp stop failed ret=%d", ret);
            break;
        }

        g_rtsp_enabled = 0;
        resp->result = 0;
        snprintf(resp->message, sizeof(resp->message), "%s", "rtsp stopped");
        log_line("rtsp stop in-process");
        break;
    case LAWREC_SERVICE_CMD_RTSP_QUERY:
        /* query 只读，用于刷新 UI 上的状态显示。 */
        resp->result = 0;
        snprintf(resp->message, sizeof(resp->message), "%s", "query ok");
        break;
    default:
        resp->result = -EINVAL;
        resp->error_no = EINVAL;
        snprintf(resp->message, sizeof(resp->message), "%s", "unknown cmd");
        break;
    }

    fill_rtsp_status(&resp->rtsp);
    fill_record_status(&resp->record);
    return resp->result;
}

extern "C" int lawrec_control_handle_record_cmd(lawrec_service_cmd_e cmd,
                                                lawrec_service_response_t *resp)
{
    lawrec_record_config_t config;
    int ret = 0;

    if (resp == NULL)
        return -EINVAL;

    std::lock_guard<std::mutex> guard(g_control_lock);

    memset(resp, 0, sizeof(*resp));
    resp->magic = LAWREC_SERVICE_MSG_MAGIC;
    resp->version = LAWREC_SERVICE_MSG_VERSION;
    resp->result = -1;
    resp->error_no = 0;

    if (!g_initialized) {
        g_initialized = 1;
        log_line("control init");
    }

    switch (cmd) {
    case LAWREC_SERVICE_CMD_RECORD_START:
        if (rtsp_state_active(lawrec_rtsp_get_state())) {
            resp->result = -EBUSY; resp->error_no = EBUSY;
            snprintf(resp->message, sizeof(resp->message), "RTSP owns encoder");
            break;
        }
        if (!g_preview_active) {
            resp->result = -EAGAIN;
            resp->error_no = EAGAIN;
            snprintf(resp->message, sizeof(resp->message), "%s", "preview not ready");
            log_line("record start rejected preview_active=0 requested=%d",
                     g_preview_requested);
            break;
        }

        if (lawrec_record_get_state() == LAWREC_RECORD_STATE_RECORDING ||
            lawrec_record_get_state() == LAWREC_RECORD_STATE_STARTING) {
            g_record_enabled = 1;
            resp->result = 0;
            snprintf(resp->message, sizeof(resp->message), "%s", "record already running");
            break;
        }

        config = default_record_config();
        ret = lawrec_record_start_async(&config);
        if (ret != 0) {
            g_record_enabled = 0;
            resp->result = ret;
            resp->error_no = -ret;
            snprintf(resp->message, sizeof(resp->message), "record start failed:%d", ret);
            log_line("record start failed ret=%d", ret);
            break;
        }

        g_record_enabled = 1;
        resp->result = 0;
        snprintf(resp->message, sizeof(resp->message), "%s", "record started");
        log_line("record start accepted");
        break;
    case LAWREC_SERVICE_CMD_RECORD_STOP:
        if (lawrec_record_get_state() == LAWREC_RECORD_STATE_IDLE ||
            lawrec_record_get_state() == LAWREC_RECORD_STATE_FAILED) {
            g_record_enabled = 0;
            resp->result = 0;
            snprintf(resp->message, sizeof(resp->message), "%s", "record already stopped");
            break;
        }

        ret = lawrec_record_stop_async();
        if (ret != 0) {
            resp->result = ret;
            resp->error_no = -ret;
            snprintf(resp->message, sizeof(resp->message), "record stop failed:%d", ret);
            log_line("record stop failed ret=%d", ret);
            break;
        }

        g_record_enabled = 0;
        resp->result = 0;
        snprintf(resp->message, sizeof(resp->message), "%s", "record stopped");
        log_line("record stop in-process");
        break;
    case LAWREC_SERVICE_CMD_RECORD_QUERY:
        resp->result = 0;
        snprintf(resp->message, sizeof(resp->message), "%s", "record query ok");
        break;
    default:
        resp->result = -EINVAL;
        resp->error_no = EINVAL;
        snprintf(resp->message, sizeof(resp->message), "%s", "unknown record cmd");
        break;
    }

    fill_rtsp_status(&resp->rtsp);
    fill_record_status(&resp->record);
    return resp->result;
}
