/* Frozen f1-01/ATA interface, v1. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BLKDEV_H
#define CIUKI_BLKDEV_H
#include "fs_port.h"
#define BLKDEV_SECTOR_SIZE 512u
#define BLKDEV_READ_BYTES 65536u
#define BLKDEV_READ_SECTORS (BLKDEV_READ_BYTES / BLKDEV_SECTOR_SIZE)
enum blkdev_cache_state { BLKDEV_CACHE_UNKNOWN, BLKDEV_CACHE_DISABLED, BLKDEV_CACHE_ENABLED };
struct blkdev {
    /* One sector, bounded polling, no locks/allocation/scheduler. Optional
     * emergency crash recorder only; failure is never retried. */
    int (*emergency_write)(struct blkdev *, uint64_t, const void *);
    /* Synchronous, complete request or negative FS_EIO / FS_EINVAL /
     * FS_EQUARANTINED. No retry after an issued error. count is in sectors.
     * flush != NULL certifies a qualified durability implementation; returning
     * success means all earlier writes reached stable media. */
    int (*read)(struct blkdev *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blkdev *dev, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct blkdev *dev);
    int (*media_check)(struct blkdev *dev); /* optional, sleepable readiness */
    uint64_t capacity;                 /* number of sectors, not last LBA */
    uint32_t sector_size;              /* FAT/partition: 512; ATAPI: 2048 */
    uint32_t media_generation;         /* zero for fixed disks; never recycled */
    enum blkdev_cache_state write_cache_state;
    bool quarantined;
    void *ctx;                        /* owned by the device implementation */
};
static inline int blkdev_range(struct blkdev *d, uint64_t lba, uint32_t n) {
    if (!d || (d->sector_size != 512 && d->sector_size != 2048) || !n || lba >= d->capacity || n > d->capacity-lba) return -FS_EINVAL;
    return d->quarantined ? -FS_EQUARANTINED : 0;
}
static inline bool blkdev_durable(struct blkdev *d) {
    return d && d->write && !d->quarantined &&
        (d->write_cache_state == BLKDEV_CACHE_DISABLED ||
         (d->write_cache_state == BLKDEV_CACHE_ENABLED && d->flush));
}
#endif
