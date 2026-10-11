/* Shared sector cache. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_CACHE_H
#define CIUKI_CACHE_H
#include "blkdev.h"
#define CACHE_DEFAULT_BYTES (8u*1024u*1024u)
#define CACHE_DEVICES 32u
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
    uint32_t since, generation;
    uint16_t sector_size, capacity;
    bool dirty;
};
struct cache_page {
    uint8_t *data;          /* one fs_page_alloc() page, pure data */
    uint8_t layout[2];      /* per half: 0 = 4 x 512, 1 = 1 x 2048 */
    struct cache_block b[CACHE_PAGE_SLOTS];
};
struct cache_meta { struct cache_meta *next; unsigned count; struct cache_page pages[]; };
struct cache_lru { struct cache_block *oldest, *newest; };
struct cache_device { struct blkdev *dev; int error; };
struct block_cache {
    fs_lock lock, workspace_lock;
    struct cache_page inline_page;  /* first page header: no descriptor page needed */
    struct cache_meta *meta;
    struct cache_lru lru[2];        /* [0] 512-byte slots, [1] 2048-byte slots */
    struct cache_block *hash[256];
    struct cache_device devices[CACHE_DEVICES];
    void *workspace[256];
    uint32_t workspace_pages, blocks;
    uint32_t data_pages, meta_pages, slots[2];
    uint64_t hits, misses, writes;
    /* f1-09: refresh frozen block-view scalars even on cache hits. The
     * callback must be short, nonblocking and must not enter the cache. */
    void (*refresh)(void *, struct blkdev *);
    void *refresh_ctx;
    uint32_t next_writeback;
    bool writeback_pending;
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
    uint32_t pages, data_pages, meta_pages, workspace_pages, slots_512, slots_2048;
    uint64_t hits, misses, writes;
};
void cache_stats(struct block_cache *, struct cache_stats *);
void cache_destroy(struct block_cache *);
int cache_read(struct block_cache *, struct blkdev *, uint64_t lba, void *sector);
int cache_write(struct block_cache *, struct blkdev *, uint64_t lba, const void *sector);
int cache_barrier(struct block_cache *, struct blkdev *);
int cache_writeback_tick(struct block_cache *, uint32_t now_ms);
bool cache_writeback_due(const struct block_cache *, uint32_t now_ms);
int cache_error(struct block_cache *, struct blkdev *);
/* Invalidate only after durable unmount or externally established media loss.
 * forget_error is for a new media generation, never automatic error recovery. */
void cache_invalidate(struct block_cache *, struct blkdev *, bool forget_error);
/* Exclusive mount/checker scratch, from the same preallocated pool. */
int cache_workspace_begin(struct block_cache *, uint32_t bytes);
uint8_t *cache_workspace_byte(struct block_cache *, uint32_t offset);
void cache_workspace_end(struct block_cache *);
#endif
