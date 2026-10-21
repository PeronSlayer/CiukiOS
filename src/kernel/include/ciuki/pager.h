/* f0-49 dynamic backing file, page-out/page-in and idle prewrite (M4).
 * Numbers are CiukiOS policy (docs/design/boot-memory.md, "Backing store"),
 * not Windows 98 constants. Kernel-internal interface; no user ABI.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_PAGER_H
#define CIUKI_PAGER_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PAGER_PAGE 4096u
#define PAGER_EXTENT_BYTES (16u << 20)                 /* initial size and growth step */
#define PAGER_EXTENT_SLOTS (PAGER_EXTENT_BYTES / PAGER_PAGE)
#define PAGER_MAX_EXTENTS 64u                          /* 1 GiB: bounded slot metadata */
#define PAGER_MIN_EXTENTS 1u                           /* configured minimum: 16 MiB */
#define PAGER_RESERVE_FLOOR (16ull << 20)              /* disk-free reserve floor */
#define PAGER_RESERVE_PERCENT 5u                       /* max(16 MiB, 5% of capacity) */
#define PAGER_SHRINK_IDLE_MS 30000u                    /* idle time before trailing shrink */
#define PAGER_RUNS 32u                                 /* contiguous LBA runs per extent */
#define PAGER_BATCH 8u                                 /* pages per page-out request */
#define PAGER_IN_REQUESTS 4u                           /* reserved demand page-in records */
#define PAGER_OUT_REQUESTS 1u                          /* reserved page-out record */
#define PAGER_PREWRITE_QUEUE 8u                        /* speculative queue entries */
#define PAGER_GROW_STEP (256u << 10)                   /* bytes per volume reservation */
#define PAGER_SHRINK_STEP (1u << 20)                   /* trailing bytes released per reservation */
#define PAGER_MAP_STEP 256u                            /* clusters mapped per reservation */
#define PAGER_COMMIT_WAIT_MS 10000u                    /* growth wait for a commit */
#define PAGER_PREWRITE_GAP_MS 50u                      /* between idle prewrite slices */
#define PAGER_DEMAND_QUIET_MS 100u                     /* no prewrite after demand I/O */

/* Errors are negative and numerically equal to the POSIX/FS codes. */
#define PAGER_EIO 5
#define PAGER_EAGAIN 11
#define PAGER_ENOMEM 12
#define PAGER_EBUSY 16
#define PAGER_ENOSPC 28
#define PAGER_EROFS 30
#define PAGER_EDEADLK 35
#define PAGER_ECANCELED 125

/* Lock/resource order, outermost first. A task may acquire a level only
 * while every level it holds is lower. Trylock-style probes create no
 * wait-for edge and are exempt. Violations are recorded and refused. */
enum pager_order {
    PAGER_ORDER_ASPACE = 1, /* address-space range/mutation pins */
    PAGER_ORDER_MANAGER,    /* extent growth/shrink; VFS namespace->volume->cache->device below */
    PAGER_ORDER_OBJECT,     /* memory-object busy: fault, page-in, alias transaction */
    PAGER_ORDER_ALLOC,      /* reclaimable frame allocation, may wait for reclaim */
    PAGER_ORDER_PAGEOUT,    /* pager worker page-out or prewrite operation */
    PAGER_ORDER_SLOTS,      /* slot map: cooperative-atomic, no sleep or allocation */
    PAGER_ORDER_IO,         /* reserved request submit/wait on the controller queue */
    PAGER_ORDER_LEVELS
};

enum pager_class { PAGER_DEMAND, PAGER_MANDATORY, PAGER_PREWRITE };
/* count slots starting at extent slot first are contiguous from lba. */
struct pager_run { uint32_t first, count; uint64_t lba; };
struct pager_map { uint32_t runs; struct pager_run run[PAGER_RUNS]; };
struct pager_io {
    void *buffer;
    uint64_t lba;
    uint32_t bytes;
    enum pager_class cls;
    bool write;
    void *token; /* backend request; caller storage, valid submit..wait */
};
/* Storage adapter. open/grow/shrink/reattach run only on the sleepable
 * manager (PAGER_ORDER_MANAGER); submit/wait/cancel use reserved records
 * only and never allocate, evict cache, take the VFS namespace or FAT locks.
 * submit returns -PAGER_EAGAIN when the reservation is momentarily
 * exhausted. grow ensures extent exists (allocated and flushed) and maps
 * it; for an existing extent it only maps. */
struct pager_backend {
    int (*open)(void *ctx, uint32_t *existing_extents);
    int (*grow)(void *ctx, uint32_t extent, struct pager_map *out);
    int (*shrink)(void *ctx, uint32_t extents);
    int (*space)(void *ctx, uint64_t *free_bytes, uint64_t *capacity_bytes);
    int (*submit)(void *ctx, struct pager_io *);
    int (*wait)(void *ctx, struct pager_io *);
    bool (*cancel)(void *ctx, struct pager_io *);
    bool (*idle)(void *ctx);     /* filesystem write queues idle */
    bool (*writable)(void *ctx); /* volume still writable and present */
    void (*close)(void *ctx);
    /* Shutdown/resume of the volume dependency (quiesce keeps reads). */
    int (*detach)(void *ctx);
    int (*reattach)(void *ctx);
};
enum pager_state { PAGER_OFF, PAGER_ACTIVE, PAGER_QUIESCED, PAGER_FAILED };
struct pager_stats {
    uint32_t state, extents, capacity, free, reserved, used, bad, inflight, generation;
    uint32_t order_violations, max_out_us, max_in_us, max_slice_us, max_atomic_us;
    uint64_t pageouts, pageins, prewrites, clean_discards, redirtied, deferred, cancelled;
    uint64_t write_errors, read_errors, grows, grow_failures, shrinks, shrink_failures;
    uint64_t out_us, in_us, commit_waits, commit_growths;
    int sticky;
    bool grow_blocked;
};

/* Activation from the storage adapter; the manager then opens the store.
 * Existing extents (a file kept across boots) are published empty: swap
 * contents are never trusted across a reboot. */
int pager_attach(const struct pager_backend *, void *ctx) __attribute__((weak));
void pager_init(void) __attribute__((weak));
/* Release boot metadata work after the desktop's first complete frame (or
 * console fallback). Before this, admission is RAM-only and never waits. */
void pager_bootstrap_complete(void) __attribute__((weak));
void pager_poll(uint64_t now_ms) __attribute__((weak));
void pager_snapshot(struct pager_stats *);

/* Slots (task context, never sleeps/allocates). Handles are 1-based. */
bool pager_slot_readable(uint32_t slot) __attribute__((weak));
bool pager_slot_commit(uint32_t slot) __attribute__((weak)); /* reserved -> used */
void pager_slot_free(uint32_t slot) __attribute__((weak));   /* failed slots stay failed */
bool pager_can_reserve(void) __attribute__((weak));
bool pager_online(void) __attribute__((weak));
/* A paged-in frame keeps its slot as a clean copy only while free slots
 * stay above max(capacity/8, 2 batches): resident pages never hoard swap. */
bool pager_keep_copy(void) __attribute__((weak));
/* Demand page-in into an unpublished frame. Caller holds PAGER_ORDER_OBJECT.
 * Never fabricates data: any failure is -PAGER_EIO for containment. */
int pager_page_in(uint32_t slot, void *buffer) __attribute__((weak));
/* Commit admission helpers. pager_commit_wait sleeps (task context, no
 * object/pager levels held) until growth covers pages or fails. */
bool pager_commit_wait(uint32_t pages) __attribute__((weak));
void pager_wake(void) __attribute__((weak));     /* nonblocking, any context */
void pager_demand(void) __attribute__((weak));   /* cancel queued speculative writes */
/* Volume dependency (storage_shutdown/removal of the pagefile volume). */
int pager_shutdown(void) __attribute__((weak));
int pager_resume(void) __attribute__((weak));
void pager_media_lost(void) __attribute__((weak));
bool pager_volume_live(void) __attribute__((weak));

/* Order assertion. enter returns false (recorded) on a violation and the
 * caller must refuse the operation; exit is paired only with a true enter. */
bool pager_order_enter(enum pager_order, uintptr_t site) __attribute__((weak));
void pager_order_exit(enum pager_order) __attribute__((weak));
bool pager_order_check(enum pager_order, uintptr_t site) __attribute__((weak));

/* Host replays drive the workers explicitly; production uses kernel tasks. */
bool pager_pageout_step(void);
bool pager_manager_step(uint64_t now_ms);
bool pager_prewrite_step(uint64_t now_ms);
#endif
