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
/* What a page-out needs after the provider's snapshot (f0-49). */
enum reclaim_outcome { RECLAIM_WRITE, RECLAIM_CLEAN, RECLAIM_DEFER, RECLAIM_GONE };
/* ticket/phys/generation identify a queue entry. prewrite keeps the frame.
 * slot is the backing slot written (pager-owned until finish adopts it),
 * epoch the dirty version captured by prepare, written the I/O result. */
struct reclaim_io {
    uint32_t ticket, phys, generation;
    bool dirty, prewrite, written;
    uint32_t slot, epoch;
    enum reclaim_outcome outcome;
};
/* Provider: valid/retain/release/prewrite are bounded, nonblocking and called
 * under IRQ exclusion. valid excludes mutable aliases before admission;
 * retain/release hold the FRAME and object generation across exit.
 * prepare (task context, no yield) snapshots a page for writing into buffer
 * and sets io->outcome/io->slot. finish performs bounded alias revocation
 * and adopts or returns io->slot; false means retry later. A failed write
 * must preserve backing and dirty state. Never free a frame before the last
 * stale alias is gone. prepare/prewrite are optional. */
struct reclaim_backing {
    bool (*valid)(struct reclaim_page *);
    void (*retain)(struct reclaim_page *);
    bool (*release)(struct reclaim_page *); /* true only if the PFN was freed */
    bool (*finish)(struct reclaim_page *, const struct reclaim_io *, bool success);
    void (*prepare)(struct reclaim_page *, struct reclaim_io *, void *buffer);
    bool (*prewrite)(struct reclaim_page *); /* cold and lacks a valid copy */
};
struct reclaim_page {
    struct reclaim_page *prev, *next;
    const struct reclaim_backing *backing;
    uint32_t phys, generation, owner_id;
    enum mm_owner owner;
    uint8_t chances;
    bool linked, queued;
};
void reclaim_track(struct reclaim_page *) __attribute__((weak));
void reclaim_untrack(struct reclaim_page *) __attribute__((weak));
void reclaim_fault(bool stalled) __attribute__((weak));
void reclaim_wake(void) __attribute__((weak));
void reclaim_hurry(void) __attribute__((weak)); /* fault waiters: back-to-back slices */
void reclaim_init(void) __attribute__((weak));
void reclaim_slice(void);
void reclaim_poll(uint64_t now_ms) __attribute__((weak));
bool reclaim_writeback_take(struct reclaim_io *); /* mandatory entries first */
bool reclaim_writeback_take_class(struct reclaim_io *, bool prewrite);
/* Provider snapshot; rebases the redirty check on the captured version. */
bool reclaim_writeback_prepare(struct reclaim_io *, void *buffer);
/* Generation/ticket checked; stale or duplicate completion returns false. */
bool reclaim_writeback_complete(const struct reclaim_io *, bool success);
bool reclaim_writeback_live(const struct reclaim_io *);
bool reclaim_writeback_waiting(void);
/* f0-49 idle prewrite: one bounded slice (64 nodes / 500 us) selecting cold
 * pages without a valid backing copy; frames stay mapped. Returns queued. */
unsigned reclaim_prewrite_slice(unsigned limit);
#endif
