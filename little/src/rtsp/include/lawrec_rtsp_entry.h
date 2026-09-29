#ifndef _LAWREC_RTSP_ENTRY_H
#define _LAWREC_RTSP_ENTRY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sensor_type;
    int session_num;
    const char *video_type;
    int video_width;
    int video_height;
    int audio_input_type;
} lawrec_rtsp_config_t;

typedef enum {
    LAWREC_RTSP_STATE_IDLE = 0,
    LAWREC_RTSP_STATE_STARTING,
    LAWREC_RTSP_STATE_LIVE,
    LAWREC_RTSP_STATE_STOPPING,
    LAWREC_RTSP_STATE_FAILED,
} lawrec_rtsp_state_e;

int lawrec_rtsp_entry(int argc, char *argv[]);
int lawrec_rtsp_start_async(const lawrec_rtsp_config_t *config);
int lawrec_rtsp_stop_async(void);
int lawrec_rtsp_stop_wait(int timeout_ms);
int lawrec_rtsp_is_running(void);
int lawrec_rtsp_get_state(void);
int lawrec_rtsp_get_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
