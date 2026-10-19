/* Global replacement, CiukiOS policy (M3). No backing-store I/O here.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_RECLAIM_H
#define CIUKI_RECLAIM_H
#include <ciuki/mm.h>
#define RECLAIM_SLICE_PAGES 64u
#define RECLAIM_OWNER_LIMIT 8u
#define RECLAIM_QUEUE_SIZE 32u
struct block_cache;
struct reclaim_result { uint32_t scanned, reclaimed, dirty, inflight, pinned; bool done; };
/* One physical-page preflight, no I/O, transport or retained cache pointers. */
void cache_reclaim_step(struct block_cache *, bool speculative, struct reclaim_result *);
void reclaim_cache_register(struct block_cache *) __attribute__((weak));
void reclaim_cache_unregister(struct block_cache *) __attribute__((weak));
struct reclaim_page;
/* f0-49 provider: valid/retain/release are bounded, nonblocking and called
 * under IRQ exclusion. valid excludes mutable aliases before admission;
 * retain/release hold the FRAME and
 * object generation across exit. finish performs bounded alias revocation;
 * false means more slices are needed. A failed write must preserve backing
 * and dirty state. Never free a frame before the last stale alias is gone. */
struct reclaim_backing {
    bool (*valid)(struct reclaim_page *);
    void (*retain)(struct reclaim_page *);
    bool (*release)(struct reclaim_page *); /* true only if the PFN was freed */
    bool (*finish)(struct reclaim_page *, bool success);
};
struct reclaim_page {
    struct reclaim_page *prev, *next;
    const struct reclaim_backing *backing;
    uint32_t phys, generation, owner_id;
    enum mm_owner owner;
    uint8_t chances;
    bool linked, queued;
};
struct reclaim_io { uint32_t ticket, phys, generation; bool dirty; };
void reclaim_track(struct reclaim_page *) __attribute__((weak));
void reclaim_untrack(struct reclaim_page *) __attribute__((weak));
void reclaim_fault(bool stalled) __attribute__((weak));
void reclaim_wake(void) __attribute__((weak));
void reclaim_init(void) __attribute__((weak));
void reclaim_slice(void);
void reclaim_poll(uint64_t now_ms) __attribute__((weak));
bool reclaim_writeback_take(struct reclaim_io *);
/* Generation/ticket checked; stale or duplicate completion returns false. */
bool reclaim_writeback_complete(const struct reclaim_io *, bool success);
#endif
