/* SPDX-License-Identifier: MIT */
#ifndef CIUKI_BACKUP_H
#define CIUKI_BACKUP_H
#include "model.h"
#define BK_MAX_FILES 4096u
struct bk_result { unsigned files, folders, skipped; uint64_t bytes; };
enum bk_replace { BK_NEVER, BK_OLDER, BK_ALWAYS, BK_ASK };
struct bk_callbacks {
    void *ctx;
    int (*cancel)(void *);
    void (*progress)(void *, const char *, const struct bk_result *);
    int (*replace)(void *, const char *); /* 1 yes, 0 skip, negative cancel */
};
/* base=NULL is full. Differential compares the base catalogue's fixed file
 * set, not a fresh directory scan. All errors are negative CiukiOS errno. */
const struct fm_vfs *fm_native_backup_vfs(void);
int bk_create(const struct fm_vfs *, const char *archive, const char *const *sources,
              unsigned count, const char *base, const struct bk_callbacks *, struct bk_result *);
int bk_verify(const struct fm_vfs *, const char *, const struct bk_callbacks *, struct bk_result *);
/* root=NULL means original paths. Otherwise preserve the complete original
 * path below root. Verify the entire archive before creating any output. */
int bk_restore(const struct fm_vfs *, const char *, const char *root, enum bk_replace,
               const struct bk_callbacks *, struct bk_result *);
#endif
