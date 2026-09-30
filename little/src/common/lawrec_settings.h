#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Port changes are persistent but only applied to the next UI process. */
int lawrec_settings_port(void);
int lawrec_settings_save_port(unsigned port);
/* Explicit replacement of saved WPA2 networks; never reconfigures a live link. */
int lawrec_settings_save_wifi(const char *ssid, const char *password);
int lawrec_network_addresses(char *buffer, size_t size);
#ifdef __cplusplus
}
#endif
