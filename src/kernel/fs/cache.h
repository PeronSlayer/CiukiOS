/* Shared sector cache. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_CACHE_H
#define CIUKI_CACHE_H
#include "blkdev.h"
#define CACHE_DEFAULT_BYTES (8u*1024u*1024u)
#define CACHE_DEVICES 32u
#define CACHE_DIRTY_AGE_MS 1000u
#define CACHE_WRITEBACK_BLOCKS 64u
#define CACHE_CLEAN_WAIT_MS 20u
/* f4-04 page layouts. One 4096-byte data page is two 2048-byte halves; each
 * half serves four 512-byte slots or one 2048-byte slot, chosen by the sector
 * size of the devices attached (a whole page is eight 512-byte slots or two
 * 2048-byte slots; mixed pages exist only when a pinned failed dirty block
 * prevents converting a whole page). Slot descriptors live outside the data
 * page, in descriptor pages charged to the same byte budget. */
#define CACHE_PAGE_SLOTS 8u
struct cache_block {
    struct cache_block *hash_next, *prev, *next;
    struct blkdev *dev;
    uint8_t *data;
    uint64_t lba;
    union { uint32_t since; int io_error; };
    uint32_t generation;
    uint16_t sector_size, capacity;
    bool dirty, busy, invalid;
    uint8_t waiters;
};
struct cache_page {
    uint8_t *data;          /* one fs_page_alloc() page, pure data */
    uint8_t layout[2];      /* per half: 0 = 4 x 512, 1 = 1 x 2048 */
    struct cache_block b[CACHE_PAGE_SLOTS];
};
struct cache_meta { struct cache_meta *next; unsigned count; struct cache_page pages[]; };
struct cache_lru { struct cache_block *oldest, *newest; };
struct cache_device {
    struct blkdev *dev; int error; uint32_t epoch;
    uint32_t dirty_bytes, oldest;
    uint64_t frontier; /* numbered publication boundary, passed to f1-83 requests */
    uint64_t accepted, durable, buffered_completions, durable_completions;
    uint32_t generation;
    bool barrier_active, durability_lost;
    bool requested;
};
/* Stack-owned transport reservation; owner waits for joined readers before
 * returning. Covers uncached sectors when a run exceeds the cache capacity. */
struct cache_fill {
    struct cache_fill *next; struct blkdev *dev; uint64_t lba;
    uint32_t count, generation, epoch; void *buffer; unsigned waiters; int error; bool complete;
};
struct block_cache {
    fs_lock lock, workspace_lock;
    struct fs_event changed;
    struct cache_fill *fills;
    struct cache_page inline_page;  /* first page header: no descriptor page needed */
    struct cache_meta *meta;
    struct cache_lru lru[2];        /* [0] 512-byte slots, [1] 2048-byte slots */
    struct cache_block *hash[256];
    struct cache_device devices[CACHE_DEVICES];
    void *workspace[256];
    uint32_t workspace_pages, blocks;
    uint32_t data_pages, meta_pages, slots[2], device_epoch;
    uint64_t hits, misses, writes;
    uint64_t cache_sync_evictions, writer_waits, writer_wait_ms;
    uint32_t dirty_budget, dirty_bytes, tick_flushed, max_flush_ms, max_writer_wait_ms;
    uint32_t report_time, writeback_pages;
    uint8_t *writeback_buffer;
    bool batch_busy, writer_online; /* online installed at boot/quiescence */
    /* f1-09: refresh frozen block-view scalars even on cache hits. The
     * callback must be short, nonblocking and must not enter the cache. */
    void (*refresh)(void *, struct blkdev *);
    void *refresh_ctx;
    uint32_t next_writeback;
    bool writeback_pending;
    bool workspace_in_use; /* reservation only; no mutex held across scan I/O */
};
/* Allocate only at init. bytes includes metadata and scan workspace. One
 * instance is shared by ALL mounted devices. Destroy requires quiescence and
 * explicitly discards dirty data; normal callers barrier/unmount first. */
int cache_init(struct block_cache *, size_t bytes);
/* f4-04: make sure the page layout serves this device's sector size. A cache
 * starts with every page in the 512-byte layout; the first 2048-byte device
 * converts max(1, data_pages/8) pages (never more than the budget) to the
 * 2048-byte layout, writing back healthy dirty blocks and refusing to drop
 * a pinned failed dirty block. cache_read/write attach implicitly. */
int cache_attach(struct block_cache *, struct blkdev *);
struct cache_stats {
    uint32_t pages, data_pages, meta_pages, workspace_pages, writeback_pages, slots_512, slots_2048;
    uint64_t hits, misses, writes;
    uint32_t dirty_bytes, dirty_budget, throttle_bytes, tick_flushed, max_flush_ms, max_writer_wait_ms;
    uint64_t cache_sync_evictions, writer_waits, writer_wait_ms;
};
void cache_stats(struct block_cache *, struct cache_stats *);
void cache_destroy(struct block_cache *);
int cache_read(struct block_cache *, struct blkdev *, uint64_t lba, void *sector);
/* FAT run read, 512-byte sectors, at most BLKDEV_READ_SECTORS. Shared cache
 * hits (including dirty data) split physical requests; no private cache. */
int cache_read_run(struct block_cache *, struct blkdev *, uint64_t lba, uint32_t count, void *buffer);
/* Demand sector plus bounded metadata read-ahead. Hits allocate nothing;
 * temporary transport workspace is released before returning. */
int cache_read_ahead(struct block_cache *, struct blkdev *, uint64_t, uint32_t, void *);
int cache_write(struct block_cache *, struct blkdev *, uint64_t lba, const void *sector);
/* A barrier freezes this device's write admission while draining a captured
 * version. Reads and other devices continue. Later writers cannot extend it.
 * The returned record remains meaningful after removal; it is not a pin. */
struct cache_completion {
    uint64_t accepted, durable, group, buffered_completions, durable_completions;
    uint32_t generation, epoch;
    int error;
    bool incomplete;
};
int cache_barrier_record(struct block_cache *, struct blkdev *, struct cache_completion *);
int cache_completion(struct block_cache *, struct blkdev *, struct cache_completion *);
int cache_group_begin(struct block_cache *, struct blkdev *, uint64_t *group);
int cache_barrier(struct block_cache *, struct blkdev *);
/* No transport, no DMA-pin retirement. Cancel queues at the physical owner. */
void cache_media_lost(struct block_cache *, struct blkdev *);
int cache_writeback_tick(struct block_cache *, uint32_t now_ms);
bool cache_writeback_due(const struct block_cache *, uint32_t now_ms);
/* Writer event sleep (bounded safety poll), never a volume reservation.
 * Set writer_online only with a live service; standalone caches need none. */
void cache_writer_wait(struct block_cache *, uint32_t max_ms);
bool cache_writeback_report(struct block_cache *, uint32_t now_ms, struct cache_stats *);
int cache_error(struct block_cache *, struct blkdev *);
/* Invalidate only after durable unmount or externally established media loss.
 * forget_error is for a new media generation, never automatic error recovery. */
void cache_invalidate(struct block_cache *, struct blkdev *, bool forget_error);
/* Exclusive mount/checker scratch, from the same preallocated pool. */
int cache_workspace_begin(struct block_cache *, uint32_t bytes);
uint8_t *cache_workspace_byte(struct block_cache *, uint32_t offset);
void cache_workspace_end(struct block_cache *);
#endif
