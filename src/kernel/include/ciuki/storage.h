/* F1 mount orchestration, also compiled by the host integration suite.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_STORAGE_H
#define CIUKI_STORAGE_H
#include "blkpart.h"
#include "../../fs/vfs.h"

#define STORAGE_DISKS 4u
struct storage_volume {
    struct blkpart part;
    struct blkdev io;                 /* counted forwarding view, never a snapshot copy */
    struct fat_volume fat;
    unsigned disk, partition, drive;
    int error;
    bool present, read_gate;
    uint32_t read_sequence, write_sequence;
    uint64_t reads, writes, flushes, refused, writes_before_gate;
    /* Selected crash probe only; notification after synchronous completion.
     * No callback in ordinary boot, no controller fault/recovery backdoor. */
    void (*trace)(struct storage_volume *, char action, uint64_t lba, int result);
};
struct storage {
    struct block_cache cache;
    struct vfs vfs;
    struct storage_volume volumes[26];
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
