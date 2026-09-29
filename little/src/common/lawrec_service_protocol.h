#ifndef __LAWREC_SERVICE_PROTOCOL_H__
#define __LAWREC_SERVICE_PROTOCOL_H__

#include <stdint.h>
#include "lawrec_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LAWREC_RTSP_STREAM_NAME_MAX 32
#define LAWREC_SERVICE_MSG_MAGIC 0x4c535243U
#define LAWREC_SERVICE_MSG_VERSION 1U

typedef enum {
    LAWREC_SERVICE_MEDIA_STATE_IDLE = 0,
    LAWREC_SERVICE_MEDIA_STATE_STARTING = 1,
    LAWREC_SERVICE_MEDIA_STATE_LIVE = 2,
    LAWREC_SERVICE_MEDIA_STATE_STOPPING = 3,
    LAWREC_SERVICE_MEDIA_STATE_FAILED = 4,
} lawrec_service_media_state_e;

/* UI、service、control 三方共享的 RTSP 运行时快照。 */
typedef struct {
    uint8_t enabled;
    uint8_t reserved[3];
    uint32_t state;
    int32_t last_error;
    uint32_t port;
    char stream_name[LAWREC_RTSP_STREAM_NAME_MAX];
} lawrec_rtsp_status_t;

typedef struct {
    uint8_t enabled;
    uint8_t reserved[3];
    uint32_t state;
    int32_t last_error;
    char file_path[128];
} lawrec_record_status_t;

/* 当前 service 对外统一暴露 RTSP/录像生命周期命令。 */
typedef enum {
    LAWREC_SERVICE_CMD_RTSP_START = 1,
    LAWREC_SERVICE_CMD_RTSP_STOP = 2,
    LAWREC_SERVICE_CMD_RTSP_QUERY = 3,
    LAWREC_SERVICE_CMD_RECORD_START = 4,
    LAWREC_SERVICE_CMD_RECORD_STOP = 5,
    LAWREC_SERVICE_CMD_RECORD_QUERY = 6,
    LAWREC_SERVICE_CMD_PREVIEW_REQUEST = 7,
    LAWREC_SERVICE_CMD_PREVIEW_RESULT = 8,
    LAWREC_SERVICE_CMD_STATUS_QUERY = 9,
} lawrec_service_cmd_e;

/* 固定长度请求结构，调用方无需额外做动态内存分配。 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t cmd;
    uint32_t value;
} lawrec_service_request_t;

/* 响应同时返回命令执行结果和最新 RTSP 状态。 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    int32_t result;
    int32_t error_no;
    lawrec_rtsp_status_t rtsp;
    lawrec_record_status_t record;
    char message[64];
} lawrec_service_response_t;

#ifdef __cplusplus
}
#endif

#endif
