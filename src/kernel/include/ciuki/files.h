/* F2 native file view. Lock order: namespace -> volume -> cache -> device.
 * VFS_IO releases namespace during filesystem work. No ABI records here.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FILES_H
#define CIUKI_FILES_H
#include <ciuki/process.h>
#include "../../fs/vfs.h"

enum px_kind { PX_FILE, PX_DIRECTORY, PX_NULL, PX_CONSOLE, PX_MNT, PX_DEV, PX_AUDIO, PX_MIXER, PX_NET, PX_MEDIA, PX_DISK };
struct px_namespace;
struct px_node {
    struct px_namespace *space;
    struct px_node *next, *parent;
    struct fat_volume *volume;
    struct fs_ro_volume *ro;
    uint64_t ro_id, ro_size;
    struct fat_entry entry;
    struct vfs_node *legacy;
    struct ciuki_timespec ctime;
    uint64_t ino;
    uint32_t refs, generation, drive;
    enum px_kind kind;
    bool linked, mountpoint, delete_pending;
};
struct audio_stream;
struct file_description {
    struct proc_object object;
    struct file_description *next;
    struct px_node *node;
    struct audio_stream *audio;
    struct vfs_file_identity *identity; /* common FAT identity retained through final close */
    struct vfs_node lock_node;
    struct vfs_description lock_description;
    struct vfs_lock_owner lock_owner;
    uint32_t share_access; /* Win32 read/write/delete; native derives access from flags */
    bool win32, delete_on_close;
    bool mixer_control;
    uint64_t position;
    uint32_t references, flags, deny, cursor, stream;
    struct fat_cursor fat_cursor; /* f3-51c FAT cluster cursor; namespace + volume reservation */
};
struct px_namespace {
    struct vfs *vfs;
    struct px_namespace *next;
    struct px_node *nodes, *root, *mnt, *dev, *null, *console, *audio, *mixer, *net, *media;
    struct px_node *disks[4]; /* f9-02 /dev/ata0..3; node->drive is the ATA slot */
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
/* All *_locked helpers require vfs_lock/vfs_unlock. They reserve their
 * volume set before borrowing identities; filesystem work uses VFS_IO.
 * Paths are copied once by
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
int px_range_locked(struct px_node *, uint64_t start, uint64_t length);
int px_description_range_locked(struct file_description *, uint64_t, uint64_t, bool write);
int px_delete_finish_locked(struct px_node *);
int px_open_locked(struct px_namespace *, struct px_node *, const char *, uint32_t,
                   struct file_description *);
uint32_t file_description_count(void);
int file_open_flags(uint32_t flags, uint32_t mode);
int file_fd_slot(const struct process *, unsigned start);
int file_fd_reserve(struct process *);
int file_fd_publish(struct process *, int, struct proc_object *, uint32_t);
void file_fd_cancel(struct process *, int);
struct proc_object *file_fd_object(const struct process *, int32_t fd);
struct file_description *file_fd(const struct process *, int32_t fd);
struct file_description *file_description_new(uint32_t flags);
int file_description_close(struct file_description *);
/* Metadata reference only; does not enable mmap or create a dirty page copy. */
int file_mapping_identity(struct file_description *, uint64_t page_index, struct vfs_page_identity **);
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
/* f9-02 raw disk nodes (drivers/disk_dev.c). Presence needs no lock; the
 * write gate runs with the namespace lock held. */
bool disk_node_present(unsigned slot) __attribute__((weak));
bool disk_write_denied(unsigned drive) __attribute__((weak));
int disk_node_io(struct process *, unsigned slot, bool write, uint64_t offset, void *, uint32_t bytes) __attribute__((weak));
int disk_node_sync(struct process *, unsigned slot) __attribute__((weak));
int disk_devctl(struct process *, int32_t fd, uint32_t op, uint32_t address, uint32_t bytes) __attribute__((weak));
static inline bool px_write_denied(const struct px_node *n)
{
    return n && n->volume && disk_write_denied && disk_write_denied(n->drive);
}

/* F1 integration notifications, under the SAME namespace lock. The legacy
 * VFS uses weak imports so its standalone harness retains no proc dependency. */
int px_legacy_share(struct vfs *, struct fat_volume *, const struct fat_entry *, unsigned, unsigned, unsigned, uint64_t);
bool px_legacy_busy(struct vfs *, struct fat_volume *, const struct fat_entry *);
/* Confirmed bytes require ctime notification even when FAT metadata is equal. */
void px_legacy_changed(struct vfs *, struct fat_volume *, const struct fat_entry *, const struct fat_entry *, bool committed_write);
int px_legacy_closed(struct vfs *, struct vfs_node *);
bool px_volume_busy(struct vfs *, struct fat_volume *);
void px_volume_detached(struct vfs *, unsigned drive);
bool px_ro_busy(struct vfs *, struct fs_ro_volume *);
int px_ro_check_locked(struct px_node *);
int vfs_directory_pinned(struct vfs *, struct fat_volume *, uint32_t directory);
#endif
