/* Copyright (c) 2026
 */

#ifndef __KEY_PROC_H__
#define __KEY_PROC_H__

#ifdef __cplusplus
extern "C" {
#endif

int lawrec_key_init(void);
void lawrec_key_poll(void);
void lawrec_key_set_group(lv_group_t *group);
lv_group_t *lawrec_key_create_group(lv_obj_t **items, size_t count);
lv_group_t *lawrec_key_get_main_group(void);

#ifdef __cplusplus
}
#endif

#endif
