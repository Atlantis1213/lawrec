#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The configured directory must already exist on writable persistent storage. */
const char *lawrec_storage_dir(void);
int lawrec_storage_check(uint64_t *available);
int lawrec_storage_reserve(char *path, size_t capacity);
int lawrec_storage_publish(const char *partial, char *final_path, size_t capacity);
int lawrec_storage_list(char *text, size_t capacity);
int lawrec_storage_delete(const char *name);
#ifdef __cplusplus
}
#endif
