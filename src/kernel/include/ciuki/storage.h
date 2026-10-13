/* F1 mount orchestration, also compiled by the host integration suite.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_STORAGE_H
#define CIUKI_STORAGE_H
#include "blkpart.h"
#include "../../fs/vfs.h"

#define STORAGE_DISKS 4u
#define STORAGE_NTFS 8u
struct storage_volume {
    struct blkpart part;
    struct blkdev io;                 /* counted forwarding view, never a snapshot copy */
    struct fat_volume fat;
    unsigned disk, partition, drive;
    int error;
    bool present, read_gate, removable;
    uint32_t read_sequence, write_sequence;
    uint64_t reads, writes, flushes, refused, writes_before_gate;
    /* Selected crash probe only; notification after synchronous completion.
     * No callback in ordinary boot, no controller fault/recovery backdoor. */
    void (*trace)(struct storage_volume *, char action, uint64_t lba, int result);
};
/* f4-04: an NTFS partition (MBR type 07h) mounted read-only through fs_ro.
 * io is a counted forwarding view of the partition: reads go to the disk;
 * write and flush are refused here and counted, so writes/flushes stay zero
 * unless a driver bypasses the cache. The view is never durable. */
struct storage_ntfs {
    struct blkpart part;
    struct blkdev io;
    struct fs_ro_volume ro;
    unsigned disk, partition, drive;  /* drive: letter index of the last mount */
    int error;
    bool present;
    uint64_t reads, writes, flushes;
};
struct storage {
    struct block_cache cache;
    struct vfs vfs;
    struct storage_volume volumes[26];
    struct fs_ro_volume optical[STORAGE_DISKS];
    struct storage_ntfs ntfs[STORAGE_NTFS];
    int disk_errors[STORAGE_DISKS];
    bool disks[STORAGE_DISKS], ready, stopped;
    unsigned next_drive;
    uint32_t sequence, writer_ticks;
    int writer_error;
};
int storage_setup(struct storage *, size_t cache_bytes);
/* disk is the IDE slot, independent of discovery order. Only MBR primary
 * entry 1 on slot 0 may become C:, all other FAT partitions start at D:. */
int storage_add_disk(struct storage *, unsigned disk, struct blkdev *);
/* USB records/views are retained for the boot. Letters are E: onward and
 * never reused. Dirty removable media stay read-only; no recovery writes. */
int storage_add_removable(struct storage *, struct blkdev *);
int storage_remove_removable(struct storage *, struct blkdev *, bool surprise);
int storage_remove_removable_locked(struct storage *, struct blkdev *, bool surprise);
int storage_add_ro(struct storage *, unsigned slot, struct blkdev *, uint64_t session);
void storage_refresh_ro(struct storage *);
/* NTFS slot lifecycle. Mount returns the drive letter index (>= 3) or a
 * negative FS_E*; the refusal reason is ntfs[slot].ro.diagnostic. Unmount
 * refuses (EBUSY) while files are open and issues no media write. */
int storage_ntfs_mount(struct storage *, unsigned slot);
int storage_ntfs_unmount(struct storage *, unsigned slot);
struct storage_volume *storage_volume(struct storage *, unsigned drive);
int storage_enable_write(struct storage *, unsigned drive);
int storage_writeback(struct storage *, uint32_t now_ms);
int storage_shutdown(struct storage *); /* no clean mark following any error */
void storage_destroy(struct storage *); /* quiescent host tests / failed init only */
struct storage *storage_get(void);
void storage_init(void);
int storage_sync(void);

/* Production probe helpers: no hardware or records in these decisions. */
bool storage_probe_readonly(const char *selector, unsigned length);
int storage_file_digest(struct vfs_table *, const char *, uint8_t digest[32], uint32_t *size);
int storage_write_workload(struct storage *, struct vfs_table *, bool *reboot);
int storage_cache_fault(struct block_cache *, struct blkdev *, void (*inject)(void *, bool), void *, bool flush);
#endif
