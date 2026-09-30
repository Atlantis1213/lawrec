#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Port changes are persistent but only applied to the next UI process. */
int lawrec_settings_port(void);
typedef struct {
    unsigned version;
    unsigned rtsp_port;
    unsigned video_bitrate_kbps;
    unsigned record_segment_seconds;
    unsigned audio_enabled;
} lawrec_media_settings;
void lawrec_settings_media_current(lawrec_media_settings *result);
/* Returns a valid default on malformed input as well as an error code. */
int lawrec_settings_media_pending(lawrec_media_settings *result);
int lawrec_settings_media_save(const lawrec_media_settings *settings);
int lawrec_settings_bitrate(void);
int lawrec_settings_segment_seconds(void);
int lawrec_settings_audio_enabled(void);
int lawrec_settings_save_port(unsigned port);
/* Explicit replacement of saved WPA2 networks; never reconfigures a live link. */
int lawrec_settings_save_wifi(const char *ssid, const char *password);
int lawrec_network_addresses(char *buffer, size_t size);
#ifdef __cplusplus
}
#endif
