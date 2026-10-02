#ifndef __LAWREC_CONFIG_H__
#define __LAWREC_CONFIG_H__

/*
 * Small-core runtime constants shared by UI, service and control.
 * Keeping these values here prevents the same hardware/runtime defaults from
 * drifting across multiple modules during bring-up.
 */

#if defined(CONFIG_BOARD_K230_CANMV_LCKFB)
#define LAWREC_DEFAULT_SENSOR_TYPE 52
#else
#define LAWREC_DEFAULT_SENSOR_TYPE 7
#endif

#define LAWREC_IPC_SERVICE_NAME "door_lock"
#define LAWREC_SERVICE_SOCKET_PATH "/var/run/lawrec-service.sock"
#define LAWREC_SERVICE_PID_PATH "/var/run/lawrec-service.pid"
#define LAWREC_UI_PID_PATH "/var/run/lawrec-ui.pid"
#define LAWREC_LOG_PATH "/tmp/lawrec.log"
#define LAWREC_RTSP_LOG_PATH "/tmp/lawrec-rtsp.log"

#define LAWREC_RTSP_DEFAULT_PORT 8554
#define LAWREC_RTSP_DEFAULT_STREAM_NAME "lawrec"
#define LAWREC_RTSP_DEFAULT_SESSION_NUM 1
#define LAWREC_RTSP_DEFAULT_VIDEO_TYPE "h264"
#define LAWREC_RTSP_DEFAULT_WIDTH 1280
#define LAWREC_RTSP_DEFAULT_HEIGHT 720
#define LAWREC_RTSP_DEFAULT_AUDIO_INPUT 0

/* The big-core VB pool and little-core VENC request must use identical blocks. */
#define LAWREC_VENC_STREAM_BUFFER_COUNT 30U
#define LAWREC_VENC_STREAM_BUFFER_SIZE \
    ((LAWREC_RTSP_DEFAULT_WIDTH * LAWREC_RTSP_DEFAULT_HEIGHT * 3U / 4U + 4095U) & ~4095U)
#define LAWREC_CAPTURE_BUFFER_COUNT 5U

#define LAWREC_RECORD_DEFAULT_OUTPUT_DIR "/sharefs/lawrec_records"
#define LAWREC_RECORD_DEFAULT_PREFIX "lawrec"

#endif
