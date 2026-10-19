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
enum fat_publication_stage { FAT_P_DATA, FAT_P_CHAIN, FAT_P_LINK, FAT_P_DIRECTORY,
    FAT_P_FSINFO, FAT_P_DELETE, FAT_P_RENAME, FAT_P_RECLAIM, FAT_P_FLAGS, FAT_P_STAGES };
enum fat_identity_change { FAT_ID_WRITE, FAT_ID_TRUNCATE, FAT_ID_RENAME, FAT_ID_UNLINK };
struct fat_entry {
    char name[FS_NAME_BYTES], alias[40];
    uint32_t parent, index, first, size, lfn_index;
    uint64_t identity; /* volatile VFS file identity; never encoded on disk */
    uint8_t lfn_count, attr;
    struct fat_times times;
    /* Caller-serialized write cursor; not encoded on disk. Reset on COW. */
    uint32_t write_cluster, write_index;
};
struct fat_volume {
    struct block_cache *cache; struct blkdev *dev;
    uint64_t start, sectors;
    uint32_t reserved, fat_sectors, root_sector, data_sector, clusters, root, root_entries;
    uint32_t free_clusters, next_free, ro_reasons, lost_clusters;
    uint32_t volume_serial;
    char volume_label[12];
    uint16_t fsinfo, backup; uint8_t type, spc, fats, media, active_fat;
    bool mounted, readonly, writable_session, dirty_recovered;
    bool preparing; /* unpublished mount: complete direct I/O, no cache mutex */
    bool scan_complete, scan_fast, scan_deferred;
    uint32_t scan_extent, scan_passes, scan_needed_bytes, scan_available_bytes;
    uint32_t mount_ms, repair_ms, scan_ms;
    uint32_t repaired_lost, repaired_logs, repaired_copies, log_directory;
    struct fat_prepare_cache *prepare_cache; /* private, freed before publication */
    const char *diagnostic;
    /* First write-revocation cause, retained through later failed calls. */
    const char *readonly_reason;
    char operation_file[96], readonly_file[96];
    uint32_t readonly_cluster;
    /* Read observations, possibly repeated by lookups; never on-disk counts. */
    uint32_t lfn_orphans, lfn_bad_checksum, lfn_invalid;
    uint64_t publication_group, publication_frontier, publication_count, publication_durable;
    uint32_t publication_generation, identity_mount_generation;
    enum fat_publication_stage publication_stage;
    bool publication_pending, write_through;
    struct cache_completion completion;
    /* Called under the volume reservation, without namespace/cache locks.
     * Observer records group start ('G') and durable barrier ('B') or error ('E'). */
    void (*publication_observer)(void *, uint64_t, enum fat_publication_stage, char, int);
    void *publication_ctx;
    void (*identity_changed)(void *, struct fat_volume *, const struct fat_entry *,
                              const struct fat_entry *, enum fat_identity_change);
    void *identity_ctx;
    struct fat_entry alias_scratch; /* serialized alias collision walk; saves kernel stack */
};
/* Mount does not write unless requested AND validation and durability pass.
 * requested RO always issues zero writes/flushes. why is retained in volume. */
int fat_mount(struct fat_volume *, struct block_cache *, struct blkdev *, uint64_t start, uint64_t sectors, bool writable);
int fat_prepare(struct fat_volume *, struct block_cache *, struct blkdev *, uint64_t start, uint64_t sectors);
/* Removable read-only publication: bounded headers only, no read-ahead or
 * ownership scan. The explicit write upgrade validates ownership first. */
int fat_prepare_removable(struct fat_volume *, struct block_cache *, struct blkdev *, uint64_t start, uint64_t sectors);
/* f1-09/f1-74: upgrade a RO mount; deferred removable ownership is proved
 * first. Caller refreshes its view and serializes against VFS. Repairs
 * scanned log tails, lost allocation and
 * advisory flags durably; live ownership conflicts and issued I/O errors
 * retain their write gate. */
int fat_enable_write(struct fat_volume *);
int fat_unmount(struct fat_volume *);
int fat_commit(struct fat_volume *);
int fat_scan(struct fat_volume *); /* bounded read-only ownership scan */
int fat_next(struct fat_volume *, uint32_t directory, uint32_t *cursor, struct fat_entry *);
int fat_next_dos(struct fat_volume *, uint32_t directory, uint32_t *cursor, struct fat_entry *);
int fat_lookup(struct fat_volume *, uint32_t directory, const char *, struct fat_entry *);
int fat_create(struct fat_volume *, uint32_t directory, const char *, uint8_t attr, struct fat_entry *);
int fat_read(struct fat_volume *, const struct fat_entry *, uint64_t position, void *, size_t, size_t *done);
/* Same demand path; schedules shared-cache speculation after two adjacent reads. */
int fat_read_stream(struct fat_volume *, const struct fat_entry *, uint64_t pos,
                    void *, size_t, size_t *, struct cache_stream *);
int fat_write(struct fat_volume *, struct fat_entry *, uint64_t position, const void *, size_t, size_t *done);
int fat_truncate(struct fat_volume *, struct fat_entry *, uint64_t size);
/* f0-49 pagefile growth: zero-filled in-place extension to size bytes via the
 * append publication order; live clusters are never relocated. */
int fat_extend(struct fat_volume *, struct fat_entry *, uint32_t size);
/* Reserved boot-log chain only: data writes do not allocate or publish
 * metadata. Caller owns the chain and bounds writes by its reservation. */
int fat_write_reserved(struct fat_volume *, const struct fat_entry *, uint32_t capacity,
                       uint64_t position, const void *, size_t, size_t *done);
int fat_publish_reserved(struct fat_volume *, struct fat_entry *, bool finish);
int fat_remove(struct fat_volume *, struct fat_entry *, bool directory);
int fat_rename(struct fat_volume *, struct fat_entry *, uint32_t new_parent, const char *new_name);
int fat_set_metadata(struct fat_volume *, struct fat_entry *, uint8_t attr, const struct fat_times *);
/* Useful to host/probe diagnostics; bounded, does not expose a private cache. */
int fat_get_cluster(struct fat_volume *, uint32_t cluster, uint32_t *value);
void fat_revoke_write(struct fat_volume *, const char *reason, uint32_t cluster);
const char *fat_readonly_reason(const struct fat_volume *);
#endif
