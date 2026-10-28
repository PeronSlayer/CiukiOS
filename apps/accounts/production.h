/* SPDX-License-Identifier: MIT
 * Sealed production bindings of the OWNER and protected-storage adapter
 * tables used by the ACCOUNT service image (main.c) and by the host
 * integration harness. The kernel table is kernel_adapter.h.
 */
#ifndef ACCT_PRODUCTION_H
#define ACCT_PRODUCTION_H
#include "adapter.h"
/* Interim OWNER backend (sec-k02 not integrated, decision u01-D2; the image
 * binds the kernel OWNER adapter below since sec-own, host tests keep this): it is
 * compiled into the sealed image, so authenticate succeeds, and every
 * owner-store operation (generation, bindings, root occupancy, staging,
 * tree digests, snapshots, orphan recovery, checkpoints and every
 * RESERVE/PREPARE/COMMIT/ABORT/STATUS call) refuses with ENOSYS. The
 * service therefore never opens on a provisioned store and never commits
 * an owner transaction until the real kernel OWNER endpoint is bound. */
#define ACCT_INTERIM_VOLUME UINT64_C(1)
void acct_owner_interim(struct acct_owner *out);
/* Protected storage over the native file API below one SYSTEM-only
 * directory; see storage_posix.c. */
#define ACCT_IO_PATH 256
#define ACCT_IO_DIRTY 8
struct acct_posix_io {
    char base[128];
    char dirty[ACCT_IO_DIRTY][ACCT_IO_PATH]; /* changed directories below base */
    unsigned dirty_count;
    int overflow;
};
int acct_io_posix(struct acct_posix_io *state, const char *base, struct acct_io *out);

/* ---- sec-own: kernel OWNER adapter (owner_kernel.c) ----
 * Native file access of the system volume used by the OWNER adapter (profile
 * staging, tree digests, the SYSTEM-only snapshot file) and by the DOS saves
 * migration. Paths are absolute on the system volume ("/System/Security/..",
 * "/Windows/Profiles/.."). Every callback returns 0 or -errno; stat/read
 * report ENOENT, mkdir EEXIST, rename never replaces. tree_posix.c binds it
 * to the native file API; the host harness binds it to the kernel VFS. */
struct acct_tree_entry { char name[260]; int directory; uint64_t size; };
struct acct_tree_io {
    void *ctx;
    int (*inspect)(void *, const char *, int *directory, uint64_t *size); /* not "stat": an SDK macro */
    int (*mkdir)(void *, const char *);
    /* Every entry but "." and ".."; each() returning nonzero stops the walk
     * with that value. */
    int (*list)(void *, const char *, int (*each)(void *, const struct acct_tree_entry *), void *);
    int (*read)(void *, const char *, uint64_t offset, void *, size_t, size_t *);
    /* offset 0 creates or truncates; otherwise writes at offset of an existing file */
    int (*write)(void *, const char *, uint64_t offset, const void *, size_t);
    int (*rename)(void *, const char *, const char *);
    int (*remove)(void *, const char *);                      /* regular file */
    int (*rmdir)(void *, const char *);                       /* empty directory */
    int (*sync)(void *, const char *);                        /* file or directory durable */
};
int acct_tree_posix(struct acct_tree_io *out);
/* The OWNER role's endpoint over the role-bound OWNER link (service 3) of
 * the kernel supervisor: batch operations 1..7 (contract 3.5) and the
 * query operations 8..14 of src/kernel/fs/owner_system.h. */
struct acct_owner_link {
    const struct acct_tree_io *tree;
    int fd;
    uint32_t endpoint, peer_pid;
    uint64_t serial, volume; /* volume: set by acct_owner_kernel_identity */
};
int acct_owner_kernel(struct acct_owner_link *, const struct acct_tree_io *, struct acct_owner *out);
/* The mounted system volume's identity (partition LBA << 32 | FAT serial)
 * as the kernel OWNER knows it; ENODEV when the volume is not managed.
 * store_open: 0 no store yet, 1 open, 2 unavailable (OWNER answers EIO). */
int acct_owner_kernel_identity(struct acct_owner_link *, uint64_t *volume, uint32_t *store_open);
/* Old machine DOS title data (C:\SYSTEM\DOS\SAVES) moves into
 * <profile root>/Settings/DOS of the first administrator (sec-k05 store
 * layout). Restartable: an entry already present at the destination stays
 * in the legacy folder. moved/kept may be NULL. */
int acct_dos_saves_migrate(const struct acct_tree_io *, const char *profile_root, unsigned *moved, unsigned *kept);
/* end sec-own */
#endif
