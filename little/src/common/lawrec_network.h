#pragma once
#include "lawrec_settings.h"
#ifdef __cplusplus
extern "C" {
#endif
#define LAWREC_WIFI_AP_MAX 24
typedef struct {
    char ssid[33];
    char bssid[18];
    char flags[128];
    int signal_dbm;
} lawrec_wifi_ap;
typedef struct {
    unsigned generation;
    unsigned scan_generation;
    int busy, error, count, scan_result;
    int rollback_error, connection_changed;
    int ipv4_changed;
    int dhcp_pid, dhcp_bound, dhcp_error;
    unsigned lease_seconds;
    unsigned lease_remaining_seconds;
    char state[32], ssid[33], ipv4[64];
    lawrec_wifi_ap aps[LAWREC_WIFI_AP_MAX];
} lawrec_network_snapshot;
/* Nonblocking requests. Scan never selects a network or changes credentials. */
int lawrec_network_refresh_async(int scan);
/* Explicit user confirmation required: DHCP can change the address/SSH route. */
int lawrec_network_renew_async(void);
/* Live connection only; Save WiFi is a separate explicit persistent operation. */
int lawrec_network_connect_async(const char *ssid, const char *password);
/* Explicit second confirmation required. Live-only, never saves the boot draft. */
int lawrec_network_ipv4_apply_async(const lawrec_ipv4_settings *settings);
void lawrec_network_get(lawrec_network_snapshot *result);
/* S45wifi-only entry, before UI/DRM init. Applies saved IPv4 after association. */
int lawrec_network_boot_configure(void);
int lawrec_network_dhcp_stop(void);
int lawrec_network_dhcp_hook(const char *event, const char *job);
/* Cancel pending socket waits and join before process exit. */
void lawrec_network_shutdown(void);
#ifdef __cplusplus
}
#endif
