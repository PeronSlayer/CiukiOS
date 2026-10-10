/* Unified FAT driver, serialized by the caller (VFS namespace lock).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FAT_H
#define CIUKI_FAT_H
#include "cache.h"
#include "path.h"
#define FAT_ATTR_RO 1u
#define FAT_ATTR_HIDDEN 2u
#define FAT_ATTR_SYSTEM 4u
#define FAT_ATTR_VOLUME 8u
#define FAT_ATTR_DIR 16u
#define FAT_ATTR_ARCHIVE 32u
enum fat_ro_reason { FAT_RO_REQUEST=1, FAT_RO_DURABILITY=2, FAT_RO_DIRTY=4,
    FAT_RO_IO_FLAG=8, FAT_RO_COPIES=16, FAT_RO_CORRUPT=32, FAT_RO_LOST=64, FAT_RO_WRITE_ERROR=128,
    FAT_RO_ACTIVE_ONLY=256 };
struct fat_times { uint16_t create_date, create_time, access_date, write_date, write_time; uint8_t create_tenths; };
struct fat_entry {
    char name[FS_NAME_BYTES], alias[40];
    uint32_t parent, index, first, size, lfn_index;
    uint8_t lfn_count, attr;
    struct fat_times times;
};
struct fat_volume {
    struct block_cache *cache; struct blkdev *dev;
    uint64_t start, sectors;
    uint32_t reserved, fat_sectors, root_sector, data_sector, clusters, root, root_entries;
    uint32_t free_clusters, next_free, ro_reasons, lost_clusters;
    uint16_t fsinfo, backup; uint8_t type, spc, fats, media, active_fat;
    bool mounted, readonly, writable_session, dirty_recovered;
    const char *diagnostic;
    /* Read observations, possibly repeated by lookups; never on-disk counts. */
    uint32_t lfn_orphans, lfn_bad_checksum, lfn_invalid;
    struct fat_entry alias_scratch; /* serialized alias collision walk; saves kernel stack */
};
/* Mount does not write unless requested AND validation and durability pass.
 * requested RO always issues zero writes/flushes. why is retained in volume. */
int fat_mount(struct fat_volume *, struct block_cache *, struct blkdev *, uint64_t start, uint64_t sectors, bool writable);
/* f1-09: upgrade an already scanned RO mount; caller refreshes its view and
 * serializes against VFS. Recovers a scanned dirty-only volume durably;
 * never clears corruption, copy-divergence or hardware-error reasons. */
int fat_enable_write(struct fat_volume *);
int fat_unmount(struct fat_volume *);
int fat_commit(struct fat_volume *);
int fat_scan(struct fat_volume *); /* bounded read-only ownership scan */
int fat_next(struct fat_volume *, uint32_t directory, uint32_t *cursor, struct fat_entry *);
int fat_lookup(struct fat_volume *, uint32_t directory, const char *, struct fat_entry *);
int fat_create(struct fat_volume *, uint32_t directory, const char *, uint8_t attr, struct fat_entry *);
int fat_read(struct fat_volume *, const struct fat_entry *, uint64_t position, void *, size_t, size_t *done);
int fat_write(struct fat_volume *, struct fat_entry *, uint64_t position, const void *, size_t, size_t *done);
int fat_truncate(struct fat_volume *, struct fat_entry *, uint64_t size);
int fat_remove(struct fat_volume *, struct fat_entry *, bool directory);
int fat_rename(struct fat_volume *, struct fat_entry *, uint32_t new_parent, const char *new_name);
int fat_set_metadata(struct fat_volume *, struct fat_entry *, uint8_t attr, const struct fat_times *);
/* Useful to host/probe diagnostics; bounded, does not expose a private cache. */
int fat_get_cluster(struct fat_volume *, uint32_t cluster, uint32_t *value);
#endif
