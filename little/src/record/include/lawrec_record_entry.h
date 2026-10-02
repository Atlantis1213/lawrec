#ifndef _LAWREC_RECORD_ENTRY_H
#define _LAWREC_RECORD_ENTRY_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sensor_type;
    const char *video_type;
    int video_width;
    int video_height;
    /* Legacy layout retained; storage/settings own directory and file naming. */
    const char *output_dir;
    const char *file_prefix;
} lawrec_record_config_t;

typedef enum {
    LAWREC_RECORD_STATE_IDLE = 0,
    LAWREC_RECORD_STATE_STARTING,
    LAWREC_RECORD_STATE_RECORDING,
    LAWREC_RECORD_STATE_STOPPING,
    LAWREC_RECORD_STATE_FAILED,
} lawrec_record_state_e;

int lawrec_record_start_async(const lawrec_record_config_t *config);
int lawrec_record_stop_async(void);
int lawrec_record_stop_wait(int timeout_ms);
int lawrec_record_is_running(void);
int lawrec_record_get_state(void);
int lawrec_record_get_last_error(void);
const char *lawrec_record_get_last_path(void);
int lawrec_record_copy_last_path(char *buf, unsigned int buf_size);
void lawrec_record_get_progress(uint64_t *elapsed_ms, uint64_t *bytes);

#ifdef __cplusplus
}
#endif

#endif
