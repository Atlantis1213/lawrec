#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
int lawrec_time_parse_utc(const char *text, int64_t *seconds);
int lawrec_time_current_utc(char *text, size_t size);
// Low-level API: UI must use lawrec_control_set_time_utc after confirmation.
// Changes CLOCK_REALTIME only; does not claim RTC persistence or NTP sync.
int lawrec_time_set_utc(const char *text);
#ifdef __cplusplus
}
#endif
