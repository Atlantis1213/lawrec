#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Port changes are persistent but only applied to the next UI process. */
int lawrec_settings_port(void);
#define LAWREC_MEDIA_SETTINGS_VERSION 2
typedef struct {
    unsigned version;
    unsigned rtsp_port;
    unsigned video_bitrate_kbps;
    unsigned record_segment_seconds;
    unsigned audio_enabled;
    unsigned video_frame_rate;
} lawrec_media_settings;
void lawrec_settings_media_current(lawrec_media_settings *result);
/* Returns a valid default on malformed input as well as an error code. */
int lawrec_settings_media_pending(lawrec_media_settings *result);
int lawrec_settings_media_save(const lawrec_media_settings *settings);
int lawrec_settings_bitrate(void);
/* H.264 1280x720 target FPS; capture remains 30, hardware validation pending. */
int lawrec_settings_frame_rate(void);
int lawrec_settings_segment_seconds(void);
int lawrec_settings_audio_enabled(void);
int lawrec_settings_save_port(unsigned port);
/* Explicit replacement of saved WPA2 networks; never reconfigures a live link. */
int lawrec_settings_save_wifi(const char *ssid, const char *password);
/* SDK MP4 pathname is 128 bytes; leave space for the generated filename. */
#define LAWREC_RECORD_DIR_MAX 80
int lawrec_settings_record_dir_validate(const char *path);
int lawrec_settings_record_dir_pending(char *path, size_t capacity);
/* Immutable process snapshot; errors are returned, not silently ignored. */
int lawrec_settings_record_dir_current(char *path, size_t capacity);
/* Syntax-only persistence. UI checks existing/writable storage separately. */
int lawrec_settings_record_dir_save(const char *path);
typedef struct {
    unsigned version;
    unsigned dhcp;
    unsigned prefix;
    char address[16], gateway[16], dns1[16], dns2[16];
} lawrec_ipv4_settings;
/* wlan0 only. Save never applies to a live connection; S45wifi applies on boot. */
int lawrec_settings_ipv4_validate(const lawrec_ipv4_settings *settings);
int lawrec_settings_ipv4_pending(lawrec_ipv4_settings *result);
int lawrec_settings_ipv4_save(const lawrec_ipv4_settings *settings);
/* S45 start freezes the draft before launching its delayed worker/UI. */
int lawrec_settings_ipv4_stage_boot(void);
int lawrec_settings_ipv4_boot(lawrec_ipv4_settings *result);
/* Runtime snapshot is distinct from the next-boot draft. Missing means legacy DHCP. */
int lawrec_settings_ipv4_active(lawrec_ipv4_settings *result);
/* Incomplete boot application must never be reported as a successful DHCP fallback. */
int lawrec_settings_ipv4_begin_apply(void);
int lawrec_settings_ipv4_note_active(const lawrec_ipv4_settings *settings);
int lawrec_network_addresses(char *buffer, size_t size);
#ifdef __cplusplus
}
#endif
