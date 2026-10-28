/* Destination FAT backend for OWNER. ACCOUNT evidence comes from the sealed
 * account-service adapter; no public registration syscall.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_OWNER_VFS_H
#define CIUKI_OWNER_VFS_H
#include "owner.h"
#include "vfs.h"
struct owner_account_evidence {
    void *ctx;
    int (*intent)(void *,const struct owner_request *);
    int (*current)(void *,uint64_t *,uint8_t[32]);
    int (*checkpoint)(void *,const struct owner_request *,uint64_t,uint32_t);
    int (*import_map)(void *,uint64_t,uint32_t,uint32_t,const char *,const char *);
};
struct owner_vfs {
    struct vfs_table *table;
    struct owner_account_evidence account;
    unsigned drive;
    uint64_t volume, sealed_g;
    uint8_t sealed_digest[32];
    struct owner_request request;
    char prior_roots[OWNER_PROFILES][64];
    bool initialized;
};
int owner_vfs_backend(struct owner_vfs *,struct vfs_table *,uint64_t volume,
    const struct owner_account_evidence *,struct owner_backend *);
int owner_vfs_tree_digest(struct vfs_table *,const char *absolute,uint8_t[32]);
/* All namespace mutators (including SYSTEM/ACCOUNT) must consult this lease
 * in k03. A held lease covers the destination manifest/tree, root names and
 * their ancestors. Only OWNER's internal publish callback bypasses it. */
bool owner_vfs_reserved(const struct owner_vfs *,const char *canonical_path);
#endif
