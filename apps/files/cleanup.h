/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_FILES_CLEANUP_H
#define CIUKI_FILES_CLEANUP_H
#include "model.h"
enum fm_cleanup_kind { FM_CLEAN_TEMP, FM_CLEAN_RECYCLE, FM_CLEAN_INTERNET, FM_CLEAN_CATEGORIES };
#define FM_CLEAN_MASK(kind) (1u << (kind))
enum fm_cleanup_state { FM_CLEAN_READY, FM_CLEAN_RUNNING, FM_CLEAN_DONE, FM_CLEAN_CANCELLED };
struct fm_cleanup;
struct fm_cleanup_category { uint64_t bytes; unsigned items; int available; };
struct fm_cleanup_progress {
    enum fm_cleanup_state state;
    uint64_t bytes;
    unsigned items, skipped, errors;
    int first_error;
};
typedef int (*fm_cleanup_cancelled)(void *);
/* Native FAT volumes cannot contain symlinks; the host adapter uses lstat.
 * Test/adaptor VFS implementations must return FM_OTHER for symlinks. */
const struct fm_vfs *fm_native_cleanup_vfs(void);
/* Returns 0 or -errno; on failure *out is NULL and no files are changed.
 * Callback is sampled before each filesystem scan operation and entry. */
int fm_cleanup_scan_cancelable(const struct fm_vfs *, char drive, int64_t now,
                              fm_cleanup_cancelled, void *, struct fm_cleanup **out);
int fm_cleanup_scan(const struct fm_vfs *, char drive, int64_t now, struct fm_cleanup **out);
const struct fm_cleanup_category *fm_cleanup_category(const struct fm_cleanup *, unsigned);
int fm_cleanup_start(struct fm_cleanup *, unsigned mask);
int fm_cleanup_step(struct fm_cleanup *); /* 1 running, 0 terminal */
void fm_cleanup_cancel(struct fm_cleanup *);
void fm_cleanup_progress(const struct fm_cleanup *, struct fm_cleanup_progress *);
void fm_cleanup_destroy(struct fm_cleanup *);
uint64_t fm_cleanup_low_threshold(uint64_t capacity);
enum { FM_SPACE_OK, FM_SPACE_LOW, FM_SPACE_FULL };
int fm_cleanup_space_warning(uint32_t *session_seen, char drive, uint64_t capacity, uint64_t free_bytes);
#endif
