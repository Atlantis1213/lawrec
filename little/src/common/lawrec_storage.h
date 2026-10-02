#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The configured directory must already exist on writable persistent storage. */
const char *lawrec_storage_dir(void);
/* Existing non-symlink directory, persistent FS, reserve space and write probe.
 * Does not mkdir/format or change the active directory. */
int lawrec_storage_validate_dir(const char *path, uint64_t *available);
int lawrec_storage_check(uint64_t *available);
int lawrec_storage_reserve(char *path, size_t capacity);
int lawrec_storage_publish(const char *partial, char *final_path, size_t capacity);
/* Legacy bounded text adapter; returns -ENOSPC if incomplete. UI uses cursor pages. */
int lawrec_storage_list(char *text, size_t capacity);
int lawrec_storage_delete(const char *name);
typedef struct {
    char name[128];
    uint64_t bytes, device, inode;
    int64_t modified_seconds, modified_nanoseconds;
} lawrec_recording_entry;
/* Constant-memory cursor page, descending filenames. newer selects the closest
 * page before anchor; otherwise selects the next older page. No numeric offset. */
int lawrec_storage_page(const char *anchor, int newer, lawrec_recording_entry *entries,
                        size_t capacity, size_t *count, int *more);
/* Revalidate the confirmed identity before deletion; not atomic against external renames. */
int lawrec_storage_delete_matching(const lawrec_recording_entry *expected);
/* Returns a pinned read-only descriptor or a negative errno. Caller closes. */
int lawrec_storage_open_recording(const char *name);
#ifdef __cplusplus
}
#endif
