/* F2 native file view. Lock order: VFS namespace (also node/description I/O)
 * then F1 cache/device. No public ABI records are defined here.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FILES_H
#define CIUKI_FILES_H
#include <ciuki/process.h>
#include "../../fs/vfs.h"

enum px_kind { PX_FILE, PX_DIRECTORY, PX_NULL, PX_CONSOLE, PX_MNT, PX_DEV };
struct px_namespace;
struct px_node {
    struct px_namespace *space;
    struct px_node *next, *parent;
    struct fat_volume *volume;
    struct fat_entry entry;
    struct vfs_node *legacy;
    struct ciuki_timespec ctime;
    uint64_t ino;
    uint32_t refs, generation, drive;
    enum px_kind kind;
    bool linked, mountpoint;
};
struct file_description {
    struct proc_object object;
    struct file_description *next;
    struct px_node *node;
    uint64_t position;
    uint32_t references, flags, deny, cursor, stream;
};
struct px_namespace {
    struct vfs *vfs;
    struct px_namespace *next;
    struct px_node *nodes, *root, *mnt, *dev, *null, *console;
    struct file_description *descriptions;
    uint64_t next_ino;
};
struct px_path {
    struct px_node *node, *parent;
    char name[FS_NAME_BYTES];
    bool trailing, dot;
};
struct file_ledger { uint32_t nodes, descriptions, pins; };

int files_attach(struct vfs *vfs, struct px_namespace **out);
void files_detach(struct vfs *vfs); /* quiescent, from vfs_destroy */
void files_snapshot(struct px_namespace *space, struct file_ledger *out);
int files_bootstrap(struct vfs *vfs); /* install PID 1 cwd, stdio and loader */
int files_stdio(struct process *p);
void px_retain(void *node);
void px_release(void *node);
int px_error(int error);
/* All *_locked helpers require space->vfs->lock. Paths are copied once by
 * syscalls; missing_leaf admits only a missing FINAL ordinary component. */
int px_validate(const char *path);
int px_resolve_locked(struct px_namespace *, struct px_node *cwd, const char *,
                      bool missing_leaf, struct px_path *out);
int px_getcwd_locked(struct px_node *, char out[CIUKI_PATH_MAX]);
int px_chdir(struct process *, const char *);
int px_stat_locked(struct px_node *, struct ciuki_stat *);
int px_mkdir_locked(struct px_namespace *, struct px_node *, const char *, uint32_t);
int px_remove_locked(struct px_namespace *, struct px_node *, const char *, bool directory);
int px_rename_locked(struct px_namespace *, struct px_node *, const char *, const char *);
int px_getdents_locked(struct file_description *, struct ciuki_dirent *);
void px_changed_locked(struct px_node *);
int px_drop_locked(struct px_node *);
int px_share_locked(struct px_node *, uint32_t access, uint32_t deny);
int px_open_locked(struct px_namespace *, struct px_node *, const char *, uint32_t,
                   struct file_description *);
uint32_t file_description_count(void);
int file_open_flags(uint32_t flags, uint32_t mode);
int file_fd_slot(const struct process *, unsigned start);
struct proc_object *file_fd_object(const struct process *, int32_t fd);
struct file_description *file_fd(const struct process *, int32_t fd);
struct file_description *file_description_new(uint32_t flags);
int file_description_close(struct file_description *);
int file_close(struct process *, int32_t fd);
int file_dup(struct process *, int32_t fd, int32_t target, bool exact, uint32_t flags);
int file_fcntl(struct process *, int32_t fd, uint32_t cmd, uint32_t arg);
int file_open(struct process *, const char *, uint32_t flags, uint32_t mode);
int file_seek_locked(struct file_description *, int64_t off, uint32_t whence, int64_t *out);
int file_io_locked(struct file_description *, void *, size_t, bool write,
                    bool positioned, uint64_t offset, size_t *done);
int file_truncate_locked(struct file_description *, uint64_t length);
int file_sync_locked(struct file_description *);
int device_open(enum px_kind kind, uint32_t flags);
int device_write(const void *, uint32_t bytes);
bool file_syscall(struct trap_frame *tf); /* true only for rows 29..51 */

/* F1 integration notifications, under the SAME namespace lock. The legacy
 * VFS uses weak imports so its standalone harness retains no proc dependency. */
int px_legacy_share(struct vfs *, struct fat_volume *, const struct fat_entry *, unsigned, unsigned);
bool px_legacy_busy(struct vfs *, struct fat_volume *, const struct fat_entry *);
void px_legacy_changed(struct vfs *, struct fat_volume *, const struct fat_entry *, const struct fat_entry *);
int px_legacy_closed(struct vfs *, struct vfs_node *);
bool px_volume_busy(struct vfs *, struct fat_volume *);
void px_volume_detached(struct vfs *, unsigned drive);
int vfs_directory_pinned(struct vfs *, struct fat_volume *, uint32_t directory);
#endif
