/* Frozen f1-01 VFS interface, v1. All failures are negative FS_E*.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VFS_H
#define CIUKI_VFS_H
#include "fat.h"
#define VFS_DRIVE_REMOVABLE 1u
#include <ciuki/fs_ro.h>
#define VFS_NODES 64u
#define VFS_DESCRIPTIONS 128u
#define VFS_HANDLES 64u
#define VFS_TABLES 16u
#define VFS_READ 1u
#define VFS_WRITE 2u
#define VFS_CREATE 4u
#define VFS_EXCLUSIVE 8u
#define VFS_TRUNCATE 16u
/* Bitmask of access denied to other opens, checked in BOTH directions. */
enum vfs_share { VFS_DENY_NONE=0, VFS_DENY_READ=1, VFS_DENY_WRITE=2, VFS_DENY_ALL=3 };
enum vfs_whence { VFS_SEEK_SET, VFS_SEEK_CUR, VFS_SEEK_END };
struct vfs_node { struct fat_volume *volume; struct fat_entry entry; unsigned refs;
    struct fs_ro_volume *ro; uint64_t ro_id, ro_size; uint32_t drive, generation;
    uint32_t log_capacity; };
struct vfs_description { struct vfs_node *node; uint64_t position; unsigned refs, access, deny; };
struct vfs_table;
struct vfs {
    /* Order: namespace (1) -> volumes (2, ascending letter) -> cache (3)
     * -> device (4). Never wait for a reserved volume with namespace held.
     * Bookkeeping/identity phases hold namespace and the operation's volumes.
     * VFS_IO drops namespace during filesystem work. Before resuming it drops
     * the volume mutexes, then reacquires namespace -> volumes. Reservations
     * stay published throughout, excluding same-volume operations/removal
     * until bookkeeping and observers finish. Media snapshots need only
     * namespace and never join reservations. No IRQ/interrupts-off I/O. */
    fs_lock lock, volume_lock[26];
    struct fs_event volume_changed;
    uint32_t reserved, active; /* namespace protected; active belongs to its owner */
    uint32_t next[26], serving[26]; /* FIFO reservation tickets per letter */
    char holder; /* atomic diagnostic snapshot of namespace owner's drive */
    unsigned default_drive; /* operation's legacy path-resolution snapshot */
    struct fat_volume *volumes[26];
    uint32_t drive_flags[26];
    struct fs_ro_volume *ro_volumes[26];
    uint32_t generation[26];
    struct vfs_node nodes[VFS_NODES];
    struct vfs_description descriptions[VFS_DESCRIPTIONS];
    struct vfs_table *tables[VFS_TABLES];
};
struct vfs_table {
    struct vfs *vfs;
    struct vfs_description *handles[VFS_HANDLES];
    unsigned drive; /* 0=A, 2=C */
    uint32_t cwd_cluster[26]; /* zero denotes the volume root */
    char cwd[26][FS_PATH_BYTES];
};
struct vfs_find {
    unsigned drive; uint32_t generation, directory, cursor;
    uint8_t attr_mask; char pattern[FS_NAME_BYTES];
    bool ro; uint64_t ro_directory;
};
/* Caller owns tables, mounted volumes and cache. Cold FAT metadata reads
 * may use a temporary <=64 KiB run buffer, with demand-sector fallback on
 * allocation failure. Node/description capacity exhaustion is explicit.
 * Initialize once; destroy after tables/volumes are detached. */
void vfs_init(struct vfs *);
void vfs_destroy(struct vfs *);
/* Every namespace entry (native callers included) ends with vfs_unlock.
 * Reserve the complete drive set BEFORE borrowing identities across I/O.
 * _locked helpers may reserve a set while active is zero; extending an
 * operation must not wait (multi-path callers preflight the union). */
#define VFS_ALL_DRIVES ((1u << 26) - 1)
static inline uint32_t vfs_drive_mask(unsigned d) { return d < 26 ? 1u << d : 0; }
void vfs_lock(struct vfs *, uint32_t drives);
void vfs_reserve_locked(struct vfs *, uint32_t drives);
void vfs_unlock(struct vfs *);
struct vfs_io_state { uint32_t drives; unsigned default_drive; };
struct vfs_io_state vfs_io_begin_locked(struct vfs *);
void vfs_io_end_locked(struct vfs *, struct vfs_io_state);
/* Filesystem expressions must not call namespace/observer helpers. */
#define VFS_IO(v, expression) __extension__ ({ \
    struct vfs *io_v_ = (v); \
    if (!io_v_->active) fs_panic("filesystem without volume reservation"); \
    struct vfs_io_state io_mask_ = vfs_io_begin_locked(io_v_); \
    int io_result_ = (expression); vfs_io_end_locked(io_v_, io_mask_); io_result_; })
#define VFS_IO_VOID(v, expression) do { \
    struct vfs *io_v_ = (v); \
    if (!io_v_->active) fs_panic("filesystem without volume reservation"); \
    struct vfs_io_state io_mask_ = vfs_io_begin_locked(io_v_); \
    (expression); vfs_io_end_locked(io_v_, io_mask_); \
} while (0)
int vfs_attach(struct vfs *, unsigned drive, struct fat_volume *);
int vfs_attach_locked(struct vfs *, unsigned drive, struct fat_volume *);
int vfs_attach_ro(struct vfs *, unsigned drive, struct fs_ro_volume *);
int vfs_attach_ro_locked(struct vfs *, unsigned drive, struct fs_ro_volume *);
bool vfs_drive_present(struct vfs *, unsigned drive);
/* Namespace lock held. EIO invalidates media cache/names/nodes/cwd and
 * removes the letter, even with live descriptions. No media writes. */
int vfs_ro_check_locked(struct vfs *, unsigned drive);
void vfs_ro_expire_locked(struct vfs *, unsigned drive);
int vfs_detach(struct vfs *, unsigned drive); /* durable unmount, refuses opens */
int vfs_table_init(struct vfs *, struct vfs_table *, unsigned default_drive);
void vfs_table_destroy(struct vfs_table *);
int vfs_open(struct vfs_table *, const char *path, unsigned flags, enum vfs_share, uint8_t create_attr);
int vfs_close(struct vfs_table *, int handle);
/* dst=-1 selects first free handle. Duplicates/inherited handles share position.
 * Explicit dst replacement releases the previous reference atomically. */
int vfs_dup(struct vfs_table *src, int handle, struct vfs_table *dst, int dst_handle);
int vfs_read(struct vfs_table *, int, void *, size_t, size_t *done);
int vfs_write(struct vfs_table *, int, const void *, size_t, size_t *done);
int vfs_seek(struct vfs_table *, int, int64_t offset, enum vfs_whence, uint64_t *position);
int vfs_truncate(struct vfs_table *, int, uint64_t size);
int vfs_commit(struct vfs_table *, int);
/* Internal, exclusive boot-log sink. Reserve once; write aligned data in
 * place; publish logical EOF periodically; finish releases unused clusters. */
int vfs_log_reserve(struct vfs_table *, int, uint32_t capacity);
int vfs_log_write(struct vfs_table *, int, const void *, size_t, uint32_t logical_size, size_t *done);
int vfs_log_publish(struct vfs_table *, int, bool finish);
int vfs_log_rotate(struct vfs_table *, const char *current, const char *previous,
                   const char *oldest, uint32_t capacity);
int vfs_stat(struct vfs_table *, const char *, struct fat_entry *);
int vfs_attrib(struct vfs_table *, const char *, uint8_t attr);
int vfs_times(struct vfs_table *, const char *, const struct fat_times *);
int vfs_find_first(struct vfs_table *, const char *pattern, uint8_t mask, struct vfs_find *, struct fat_entry *);
int vfs_find_next(struct vfs_table *, struct vfs_find *, struct fat_entry *);
int vfs_rename(struct vfs_table *, const char *from, const char *to);
int vfs_delete(struct vfs_table *, const char *);
int vfs_mkdir(struct vfs_table *, const char *, uint8_t attr);
int vfs_rmdir(struct vfs_table *, const char *);
int vfs_chdir(struct vfs_table *, const char *);
int vfs_set_drive(struct vfs_table *, unsigned drive);
int vfs_getcwd(struct vfs_table *, unsigned drive, char out[FS_PATH_BYTES]);
int vfs_free_space(struct vfs_table *, unsigned drive, uint64_t *bytes, uint32_t *cluster_bytes);
/* Byte locks/DOS compatibility mode are F3 bridge work, not silently emulated. */
#endif
