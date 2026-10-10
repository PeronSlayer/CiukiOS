/* Shared sector cache. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_CACHE_H
#define CIUKI_CACHE_H
#include "blkdev.h"
#define CACHE_DEFAULT_BYTES (8u*1024u*1024u)
#define CACHE_DEVICES 32u
struct cache_block;
struct cache_page;
struct cache_device { struct blkdev *dev; int error; };
struct block_cache {
    fs_lock lock, workspace_lock;
    struct cache_page *pages;
    struct cache_block *hash[256], *oldest, *newest;
    struct cache_device devices[CACHE_DEVICES];
    void *workspace[256];
    uint32_t workspace_pages, blocks;
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
