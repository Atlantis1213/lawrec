#ifndef _LAWREC_CONTROL_H_
#define _LAWREC_CONTROL_H_

#include "../../common/lawrec_service_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * lawrec_control 是小核侧 preview/RTSP 运行状态的唯一事实来源。
 * UI、service、RTSP worker 都通过这里取状态，避免各自维护重复状态机。
 */
int lawrec_control_init(void);
void lawrec_control_deinit(void);
void lawrec_control_set_log_path(const char *path);

/* preview 就绪状态由大小核 IPC 的请求/结果回调共同驱动。 */
void lawrec_control_note_preview_request(int enabled);
void lawrec_control_note_preview_result(int enabled);
/* No close acknowledgement: stop dependants, but reserve the display from playback. */
void lawrec_control_note_preview_uncertain(void);
int lawrec_control_preview_needs_close(void);
/* Adopt only occupancy; observing an old preview never automatically resumes capture. */
void lawrec_control_note_display_status(int ready, int preview_occupied, int playback_occupied);
/* Explicitly confirmed recovery; no live or quarantined local consumer may own media. */
int lawrec_control_prepare_display_recovery(void);
void lawrec_control_finish_display_recovery(int result);
int lawrec_control_is_preview_active(void);

/* RTSP 状态来自小核本地 RTSP worker 的实时状态。 */
int lawrec_control_is_rtsp_enabled(void);
int lawrec_control_is_rtsp_running(void);
int lawrec_control_get_rtsp_state(void);
int lawrec_control_is_record_enabled(void);
int lawrec_control_is_record_running(void);
int lawrec_control_get_record_state(void);
int lawrec_control_playback_start(const char *filename);
/* Confirm in the UI first. Serialized with media starts; changes only UTC wall time. */
int lawrec_control_set_time_utc(const char *text);

/* UI 本地路径和 socket service 路径共用这一套 RTSP 控制入口。 */
int lawrec_control_handle_rtsp_cmd(lawrec_service_cmd_e cmd,
                                   lawrec_service_response_t *resp);
int lawrec_control_handle_record_cmd(lawrec_service_cmd_e cmd,
                                     lawrec_service_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif
