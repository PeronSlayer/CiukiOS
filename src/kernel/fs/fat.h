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
    FAT_RO_ACTIVE_ONLY=256, FAT_RO_DMF=512, FAT_RO_WRITE_PROTECTED=1024 };
struct fat_times { uint16_t create_date, create_time, access_date, write_date, write_time; uint8_t create_tenths; };
enum fat_publication_stage { FAT_P_DATA, FAT_P_CHAIN, FAT_P_LINK, FAT_P_DIRECTORY,
    FAT_P_FSINFO, FAT_P_DELETE, FAT_P_RENAME, FAT_P_RECLAIM, FAT_P_FLAGS, FAT_P_STAGES };
enum fat_identity_change { FAT_ID_WRITE, FAT_ID_TRUNCATE, FAT_ID_RENAME, FAT_ID_UNLINK };
/* f3-51c: an open file's last resolved (cluster index -> cluster) pair, the
 * model of MS-DOS's SFT sf_cluspos/sf_lstclus (FNDCLUS continues from it when
 * the target is at or after it, else from sf_firclus). It is a hint, valid
 * only while the entry still names the same first cluster and chain
 * generation; volatile, never encoded on disk. A zero cursor is empty. */
/* run0 (fx-16): clusters 0..run0-1 of the chain are first..first+run0-1,
 * learned by a read walk under this chain generation (0 = not known). */
struct fat_cursor { uint64_t generation; uint32_t first, index, cluster, run0; };
/* f1-93b: a directory walk position, the (cluster index -> cluster) pair of
 * a directory chain, carried across directory steps (a readdir cursor, a
 * resumed lookup). Valid only while the volume's mutation_sequence and
 * media generation are those recorded: any publication (every FAT, directory
 * or data write bumps the sequence) retires it, so a walk never follows a
 * stale link. Without it each slot resolved its cluster from the chain
 * start, O(slots x clusters) per directory scan. Volatile, never on disk. */
struct fat_dir_hint { uint64_t sequence; uint32_t generation, first, index, cluster; bool valid; };
struct fat_entry {
    char name[FS_NAME_BYTES], alias[40];
    uint32_t parent, index, first, size, lfn_index;
    uint64_t identity; /* volatile VFS file identity; never encoded on disk */
    uint8_t lfn_count, attr;
    struct fat_times times;
    /* f3-51c: volume-unique stamp of the cluster chain this copy names,
     * assigned when the entry is decoded or created and on every FAT
     * mutation of its chain (link, relocation, truncate, release). Cursors
     * taken under another stamp are never followed. Not encoded on disk. */
    uint64_t chain_generation;
    /* Caller-serialized write cursor; not encoded on disk. Reset on COW. */
    struct fat_cursor write_cursor;
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
    /* f3-51c: last chain generation issued. f1-89: atomic (lock cmpxchg8b
     * on i686, which needs the natural 8-byte alignment). */
    _Alignas(8) uint64_t chain_stamp;
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
    uint64_t mutation_sequence;
    /* f1-89 Amendment 1 item 5: set only around a guarded lookup on the
     * source volume (exclusive under the guard). Reads come from resident
     * cache slots only; the first missing sector is reported, never read. */
    bool cache_only;
    uint64_t cache_miss;
    /* f1-93b: work of the current cache-only section (FAT_CACHED_BUDGET),
     * counted once the step has read its first directory sector. */
    uint32_t cache_work;
    bool cache_progress;
    struct fat_transaction *transaction, *transactions;
    /* f1-93: logical providers only (no block cache). Set by the VFS that
     * attached this source volume: every device call of the volume goes
     * through io_window, which may give the caller's volume guard up around
     * the call and readmit it afterwards. write: the call changes on-disk
     * bytes (announced to readers before the guard is given up). Views and
     * unattached volumes leave it null and call the device directly. */
    int (*io_window)(void *ctx, struct fat_volume *, bool write, int (*call)(void *), void *arg);
    void *io_window_ctx;
};
/* The caller pins borrowed identities before releasing its namespace turn.
 * enter takes a fresh short turn; leave releases it. All cold planning,
 * admission and durability waits happen between these callbacks. */
struct fat_transaction_guard {
    void *ctx;
    int (*enter)(void *);
    void (*leave)(void *);
    int (*wait)(void *); /* optional, outside all cache/publication pins */
    void (*retire)(void *); /* mandatory, uncancellable admission; no cache pins */
    /* Optional short namespace turn for the in-flight cluster set only: it
     * excludes legacy mutators and other set walkers, never validates the
     * publication sequence and never waits for readers. 0: enter/leave. */
    int (*claim)(void *);
    void (*unclaim)(void *);
    /* Optional (f1-89 Amendment 1 item 4): a data transaction gives the
     * writer turn up while it waits for device I/O. resume returns with
     * the namespace turn held (no reservation); the transaction rebases
     * its view, then calls leave. */
    void (*suspend)(void *);
    void (*resume)(void *);
    /* Resume only if the turn is free now (1, namespace held as resume);
     * otherwise 0 with nothing held. A transaction holding cache pins
     * never waits for the turn: the turn holder may need those pins. */
    int (*try_resume)(void *);
};
#define FAT_TRANSACTION_CLUSTERS 256u
enum fat_transaction_op { FAT_TX_CREATE, FAT_TX_WRITE, FAT_TX_LOG_WRITE,
    FAT_TX_COMMIT, FAT_TX_REMOVE, FAT_TX_RENAME, FAT_TX_LOOKUP, FAT_TX_PUBLISH, FAT_TX_EXTEND,
    FAT_TX_RENAME_REPLACE, FAT_TX_TRUNCATE, FAT_TX_RESERVE, FAT_TX_NEXT, FAT_TX_SET_METADATA };
struct fat_transaction_args {
    enum fat_transaction_op op;
    struct fat_entry *entry;
    struct fat_entry *replacement;
    struct fat_cursor *cursor;
    const char *name;
    const void *data;
    size_t size, *done;
    uint64_t position;
    uint64_t sequence; /* caller's guarded snapshot, checked before planning */
    uint32_t directory, capacity;
    uint32_t *directory_cursor;
    uint8_t attr;
    bool flag;
    /* LOOKUP/NEXT only: run under the short guard (cache-warm retry after
     * a reader restart), never invalidated by a concurrent publisher. */
    bool guarded;
    /* entry is a caller's copy from a path walk, not a live node entry:
     * after any wait without the turn the walk must be redone (EAGAIN). */
    bool borrowed;
    /* LOOKUP/NEXT: the entry (parent cluster, slot) naming `directory`. It
     * joins the read set, so renaming, emptying or removing an ancestor
     * during an unguarded walk restarts the walk instead of trusting a
     * cluster that may have been released. */
    bool depend;
    uint32_t depend_parent, depend_index;
    const struct fat_times *times; /* SET_METADATA; 0 keeps the times */
    /* Namespace bookkeeping for the owning entry, under the short guard.
     * Must not wait, allocate or issue I/O. */
    void (*visible)(void *, const struct fat_entry *);
    void *visible_ctx;
    void (*unlinked)(void *, bool replacement);
};
int fat_transaction_run(struct fat_volume *, const struct fat_transaction_args *,
                         const struct fat_transaction_guard *);
/* f1-93/f1-93b: one directory step from resident cache sectors only, under
 * the caller's guard. name 0: the next entry at or after *cursor (labels:
 * fat_next_dos); otherwise the lookup of name from the entry boundary
 * *cursor (0: the whole directory). Returns as fat_next/fat_lookup, or
 * -FS_EAGAIN with *miss set and *cursor at the first entry boundary not yet
 * settled (resume there while the volume is unchanged, else from the start):
 * *miss is a missing sector (warm it with nothing held) or FAT_CACHE_YIELD,
 * the section's work bound (release the guard, offer a preemption point,
 * readmit). A section never calls fs_service: the holder is never preempted
 * while it holds the volume (T23 b453bc92, f1-93b). hint (optional) is the
 * walk position kept across calls; it is used only while still valid. */
#define FAT_CACHE_YIELD (UINT64_MAX-1)
/* Work units of one cache-only section, about one microsecond each on the
 * T23 (one resident sector copy or FAT decode: 2 units), the same order as
 * the 128 us cadence between fs_service points of the uncached walks. */
#define FAT_CACHED_BUDGET 128u
int fat_dir_cached(struct fat_volume *, uint32_t dir, const char *name, uint32_t *cursor,
                   struct fat_entry *, bool labels, struct fat_dir_hint *, uint64_t *miss);
/* f1-93: read sector s (with readsec's FAT/cluster read-ahead extent) into
 * the block cache. Geometry only: callable with no guard held. A logical
 * provider has no block cache: nothing is read. f1-93b: a cluster run
 * continues into the next cluster while the resident FAT names the adjacent
 * cluster as its link (a contiguous directory), up to one 64 KiB command. */
int fat_warm(const struct fat_volume *, uint64_t s);
/* f1-93: cluster count of a chain (st_blocks), one FAT sector read per
 * sector visited instead of one guarded call per cluster. Resumable:
 * cluster is the next cluster to visit (start: cluster=first, count=0).
 * cached: resident sectors only, the first missing one is reported in *miss
 * with -FS_EAGAIN. At most FAT_CHAIN_SECTORS sectors are visited per call
 * (FAT_CHAIN_CACHED_SECTORS when cached, which never calls fs_service: the
 * guard is not held across a preemption point, f1-93b): a yield returns
 * -FS_EAGAIN with *miss=UINT64_MAX and the walk resumes.
 * An invalid link or a cycle is -FS_EIO (no write revocation). */
#define FAT_CHAIN_SECTORS 128u
#define FAT_CHAIN_CACHED_SECTORS 32u
struct fat_chain_walk { uint32_t first, cluster, count; };
int fat_chain_clusters(struct fat_volume *, struct fat_chain_walk *, bool cached, uint64_t *miss);
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
/* Without a cursor every call resolves its first cluster from the chain start. */
int fat_read(struct fat_volume *, const struct fat_entry *, uint64_t position, void *, size_t, size_t *done);
/* f3-51c: the same read continuing from the open file's cursor when the
 * position is at or after it; on success the cursor names the cluster holding
 * the last byte read, on any error it is emptied. cursor may be null. */
int fat_read_cursor(struct fat_volume *, const struct fat_entry *, struct fat_cursor *,
                    uint64_t position, void *, size_t, size_t *done);
/* Same demand path; schedules shared-cache speculation after two adjacent reads. */
int fat_read_stream(struct fat_volume *, const struct fat_entry *, uint64_t pos,
                    void *, size_t, size_t *, struct cache_stream *, struct fat_cursor *);
int fat_write(struct fat_volume *, struct fat_entry *, uint64_t position, const void *, size_t, size_t *done);
/* f3-51c: fat_write resolving from the open file's cursor (or the entry's
 * write cursor); on success the cursor names the last cluster written. */
int fat_write_cursor(struct fat_volume *, struct fat_entry *, struct fat_cursor *,
                     uint64_t position, const void *, size_t, size_t *done);
int fat_truncate(struct fat_volume *, struct fat_entry *, uint64_t size);
/* f0-49 pagefile growth: zero-filled in-place extension to size bytes via the
 * append publication order; live clusters are never relocated. */
int fat_extend(struct fat_volume *, struct fat_entry *, uint32_t size);
/* Exclusive pagefile only: in-place metadata reservation, no data writes.
 * The caller must never expose bytes not written since opening the store. */
int fat_reserve(struct fat_volume *, struct fat_entry *, uint32_t size);
/* Reserved boot-log chain only: data writes do not allocate or publish
 * metadata. Caller owns the chain and bounds writes by its reservation. */
int fat_write_reserved_cursor(struct fat_volume *,const struct fat_entry *,struct fat_cursor *,uint32_t,
    uint64_t,const void *,size_t,size_t *);
int fat_write_reserved(struct fat_volume *, const struct fat_entry *, uint32_t capacity,
                       uint64_t position, const void *, size_t, size_t *done);
int fat_publish_reserved(struct fat_volume *, struct fat_entry *, bool finish);
int fat_remove(struct fat_volume *, struct fat_entry *, bool directory);
int fat_rename(struct fat_volume *, struct fat_entry *, uint32_t new_parent, const char *new_name);
int fat_rename_replace(struct fat_volume *,struct fat_entry *,struct fat_entry *,uint32_t,const char *,bool);
int fat_set_metadata(struct fat_volume *, struct fat_entry *, uint8_t attr, const struct fat_times *);
/* Useful to host/probe diagnostics; bounded, does not expose a private cache. */
int fat_get_cluster(struct fat_volume *, uint32_t cluster, uint32_t *value);
void fat_revoke_write(struct fat_volume *, const char *reason, uint32_t cluster);
const char *fat_readonly_reason(const struct fat_volume *);
#endif
