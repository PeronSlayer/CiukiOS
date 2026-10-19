/* f0-49 M4 pager on the production PMM/ledger/VMM/uaddr/reclaim code. The
 * storage adapter is a host model here (fs/pagefile.c on real FAT/blkreq is
 * qualified by host_fs_tests). Kernel tasks are modelled explicitly: a
 * sleeping task lets the reclaim worker, pager worker and extent manager
 * run as separate order contexts. Times are host CPU (ASan), not P-III.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main ledger_fixture_main
#include "ledger_test.c"
#undef main
#undef CLOCKS_PER_SEC
#include <time.h>
#include <ciuki/reclaim.h>
#include <ciuki/pager.h>
uint64_t g_tsc_per_ms = 1000000; /* clock below counts nanoseconds */
uint64_t cpu_path_clock(void)
{
    clock_t t = clock(); CHECK(t != (clock_t)-1);
    return (uint64_t)t * (1000000000u / CLOCKS_PER_SEC);
}
uint64_t ktime_cycles(void) { return cpu_path_clock(); }
void kwait_wake_irq(struct kwait *q, bool io) { (void)q; (void)io; }
void kwait_wake_all(struct kwait *q) { (void)q; }
struct block_cache { int unused; };
void cache_reclaim_step(struct block_cache *c, bool speculative, struct reclaim_result *r)
{ (void)c; (void)speculative; *r = (struct reclaim_result){ .scanned = 1, .done = true }; }
#include "../../src/kernel/core/reclaim.c"
#define PAGER_HOST_MODEL 1
#include "../../src/kernel/core/pager.c"

/* ---- storage model: persistent sparse disk, fault injection, hooks ---- */
#define DISK_PAGES (PAGER_MAX_EXTENTS * PAGER_EXTENT_SLOTS + 4096)
static struct {
    uint8_t *page[DISK_PAGES];
    uint32_t written_boot[DISK_PAGES];
    uint32_t boot, extents, opens;
    uint64_t free_bytes, capacity;
    unsigned write_calls, read_calls, writes, reads, max_batch, cancels;
    unsigned fail_write_from, fail_write_count, fail_read_from, fail_read_count, eagain;
    int grow_error, shrink_error, open_error;
    bool fragment, readonly, busy, detached;
    void (*during)(struct pager_io *);
    struct pager_io *pending[4];
    bool cancelled[4];
    unsigned depth;
} disk;
static uint64_t disk_lba(uint32_t extent, uint32_t slot)
{ return 2048u + ((uint64_t)extent * PAGER_EXTENT_SLOTS + slot) * 8 + (disk.fragment ? (uint64_t)(slot / 1024) * 128 : 0); }
static int d_open(void *ctx, uint32_t *existing)
{ (void)ctx; disk.opens++; if (disk.open_error) return disk.open_error; *existing = disk.extents; disk.detached = false; return 0; }
static int d_grow(void *ctx, uint32_t extent, struct pager_map *map)
{
    (void)ctx;
    if (disk.grow_error) return disk.grow_error;
    if (extent >= disk.extents) {
        CHECK(extent == disk.extents && disk.free_bytes >= PAGER_EXTENT_BYTES);
        disk.free_bytes -= PAGER_EXTENT_BYTES; disk.extents++;
    }
    memset(map, 0, sizeof(*map));
    if (!disk.fragment) { map->runs = 1; map->run[0] = (struct pager_run){ 0, PAGER_EXTENT_SLOTS, disk_lba(extent, 0) }; }
    else for (uint32_t r = 0; r < 4; r++)
        map->run[map->runs++] = (struct pager_run){ r * 1024, r == 3 ? 1024 - 96 : 1024, disk_lba(extent, r * 1024) };
    return 0;
}
static int d_shrink(void *ctx, uint32_t extents)
{
    (void)ctx;
    if (disk.shrink_error) return disk.shrink_error;
    CHECK(extents < disk.extents);
    disk.free_bytes += (uint64_t)(disk.extents - extents) * PAGER_EXTENT_BYTES; disk.extents = extents;
    return 0;
}
static int d_space(void *ctx, uint64_t *free, uint64_t *capacity)
{ (void)ctx; *free = disk.free_bytes; *capacity = disk.capacity; return 0; }
static int d_submit(void *ctx, struct pager_io *io)
{
    (void)ctx;
    if (disk.eagain) { disk.eagain--; return -PAGER_EAGAIN; }
    CHECK(io->buffer && io->bytes && !(io->bytes % PAGER_PAGE) && !(io->lba % 8));
    if (io->write) CHECK(!disk.readonly && !disk.detached);
    io->token = io; return 0;
}
static int d_wait(void *ctx, struct pager_io *io)
{
    (void)ctx; CHECK(io->token == io && disk.depth < 4);
    unsigned level = disk.depth++;
    disk.pending[level] = io; disk.cancelled[level] = false;
    void (*hook)(struct pager_io *) = disk.during; disk.during = 0;
    if (hook) hook(io);
    bool cancelled = disk.cancelled[level];
    disk.pending[level] = 0; disk.depth--; io->token = 0;
    if (cancelled) { disk.cancels++; return -PAGER_ECANCELED; }
    unsigned pages = io->bytes / PAGER_PAGE;
    if (io->write) {
        unsigned call = ++disk.write_calls;
        if (pages > disk.max_batch) disk.max_batch = pages;
        if (call >= disk.fail_write_from && call < disk.fail_write_from + disk.fail_write_count) return -PAGER_EIO;
        for (unsigned k = 0; k < pages; k++) {
            uint64_t index = io->lba / 8 + k; CHECK(index < DISK_PAGES);
            if (!disk.page[index]) { disk.page[index] = malloc(PAGER_PAGE); CHECK(disk.page[index]); }
            memcpy(disk.page[index], (uint8_t *)io->buffer + k * PAGER_PAGE, PAGER_PAGE);
            disk.written_boot[index] = disk.boot; disk.writes++;
        }
        return 0;
    }
    unsigned call = ++disk.read_calls;
    if (call >= disk.fail_read_from && call < disk.fail_read_from + disk.fail_read_count) return -PAGER_EIO;
    uint64_t index = io->lba / 8;
    /* Never a slot not written during this boot: no untrusted bytes. */
    CHECK(pages == 1 && index < DISK_PAGES && disk.page[index] && disk.written_boot[index] == disk.boot);
    memcpy(io->buffer, disk.page[index], PAGER_PAGE); disk.reads++;
    return 0;
}
static bool d_cancel(void *ctx, struct pager_io *io)
{
    (void)ctx;
    for (unsigned i = 0; i < disk.depth; i++) if (disk.pending[i] == io) { disk.cancelled[i] = true; return true; }
    return false;
}
static bool d_idle(void *ctx) { (void)ctx; return !disk.busy; }
static bool d_writable(void *ctx) { (void)ctx; return !disk.readonly && !disk.detached; }
static int d_detach(void *ctx) { (void)ctx; disk.detached = true; return 0; }
static int d_reattach(void *ctx) { (void)ctx; disk.detached = false; return 0; }
static const struct pager_backend fake = { .open = d_open, .grow = d_grow, .shrink = d_shrink, .space = d_space,
    .submit = d_submit, .wait = d_wait, .cancel = d_cancel, .idle = d_idle, .writable = d_writable,
    .detach = d_detach, .reattach = d_reattach };

/* ---- task model ---- */
static struct task reclaim_task, pager_task, manager_task, task_b;
static unsigned drive_depth;
static bool drive_prewrite;
static uint64_t drive_slices;
static void manager(void) { struct task *s = g_current; g_current = &manager_task; pager_manager_step(g_ticks); g_current = s; }
static bool pageout(void) { struct task *s = g_current; g_current = &pager_task; bool r = pager_pageout_step(); g_current = s; return r; }
static void drive(void)
{
    g_ticks += 10;
    if (drive_depth) return; /* nested waits inside a modelled worker */
    drive_depth++;
    manager();
    for (unsigned n = 0; n < 1024; n++) {
        bool pressure = ledger_pressure(), work = false;
        if (pressure) { struct task *s = g_current; g_current = &reclaim_task; reclaim_slice(); g_current = s; drive_slices++; }
        while (pageout()) work = true;
        if (!pressure || (work && !ledger_pressure())) break;
    }
    if (drive_prewrite) {
        struct task *s = g_current; g_current = &pager_task;
        pager_prewrite_step(g_ticks); while (pager_pageout_step()) {}
        g_current = s;
    }
    drive_depth--;
}
static uint32_t worst_out_us, worst_in_us, worst_slice_us;
static void keep_maxima(void)
{
    if (pager.stats.max_out_us > worst_out_us) worst_out_us = pager.stats.max_out_us;
    if (pager.stats.max_in_us > worst_in_us) worst_in_us = pager.stats.max_in_us;
    if (pager.stats.max_slice_us > worst_slice_us) worst_slice_us = pager.stats.max_slice_us;
}
static void pager_reset(void)
{
    keep_maxima();
    memset(&pager, 0, sizeof(pager));
    CHECK(!resident_head && !resident_cursor);
    prewrite_cursor = 0; prewrite_queued = 0; memset(modified, 0, sizeof(modified));
    active = false; work_active = 0; phase = age_pfn = 0; memset(&pending, 0, sizeof(pending));
    hurry_until = 0;
}
static uint32_t *held; static uint32_t held_count;
static void hold_until(uint32_t normal_free)
{
    while (pmm_zone_free(ZONE_DMA)) { held[held_count] = pmm_alloc_zone(ZONE_DMA); CHECK(held[held_count++]); }
    while (pmm_zone_free(ZONE_NORMAL) > normal_free) { held[held_count] = pmm_alloc_zone(ZONE_NORMAL); CHECK(held[held_count++]); }
}
static void release_held(uint32_t pages) { while (pages-- && held_count) pmm_free(held[--held_count]); }
static void start(unsigned mib, uint64_t capacity_mib, uint64_t free_mib, bool keep_disk)
{
    release_held(UINT32_MAX);
    fresh(mib); pager_reset();
    if (!keep_disk) {
        for (unsigned i = 0; i < DISK_PAGES; i++) free(disk.page[i]);
        memset(&disk, 0, sizeof(disk)); disk.boot = 1;
    } else disk.boot++; /* reboot: old bytes stay on the medium */
    disk.capacity = capacity_mib << 20; disk.free_bytes = free_mib << 20;
    disk.fail_write_from = disk.fail_read_from = 0; disk.fail_write_count = disk.fail_read_count = 0;
    memory_sleep = drive; drive_prewrite = false; g_ticks += 1000;
    CHECK(!pager_attach(&fake, &disk));
    manager();
}
static uint32_t pattern(uint32_t i, uint32_t salt) { return 0x5a000000u ^ (i * 2654435761u) ^ salt; }
static void store_page(struct uaddr *u, uint32_t va, uint32_t value)
{ memory_store(u, va, value); memory_store(u, va + PAGE_SIZE - 4, ~value); }
static bool page_ok(struct uaddr *u, uint32_t va, uint32_t value)
{ return memory_word(u, va) == value && memory_word(u, va + PAGE_SIZE - 4) == ~value; }
static struct ua_slot *slot_at(struct uaddr *u, uint32_t va) { return area_slot(ua_find(u, va), va); }
static void end_process(struct uaddr *u) { cr3 = 0; ua_destroy(u); }
static void finish_process(struct uaddr *u)
{
    end_process(u);
    CHECK(!pager.counts[ST_USED] && !pager.counts[ST_RESERVED]);
}
static uint64_t host_us(void) { return cpu_path_clock() / 1000; }

/* ---- A: demand beyond RAM on the 128/256/512 MiB profiles ---- */
static void profile(unsigned mib)
{
    start(mib, 4096, 2048, false);
    struct mm_ledger v = snapshot();
    CHECK(pager.state == PAGER_ACTIVE && pager.extents == 1 && v.swap_capacity == PAGER_EXTENT_SLOTS && !v.swap_used);
    CHECK(v.pin_tokens == 1 && resident(MM_PAGER) == PAGER_BATCH + 1); /* pinned bounce + extent map */
    uint32_t keep = v.high + 2048, ws = keep + 8192;
    hold_until(keep);
    uint64_t baseline_commit = snapshot().commit_ram[MM_USER_PRIVATE];
    uint64_t t0 = host_us();
    struct uaddr u; CHECK(!ua_init(&u, &u));
    /* Admission beyond RAM waits for prevalidated extents, then succeeds. */
    CHECK(!ua_map_at(&u, CIUKI_MMAP_BASE, ws * PAGE_SIZE, PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE, UA_ANON, 0));
    v = snapshot();
    uint32_t grown = pager.extents;
    CHECK(grown >= 2 && pager.stats.commit_waits && v.commit_pagefile && v.commit_pagefile <= v.swap_capacity);
    CHECK(ledger_commit_headroom() >= 0);
    for (uint32_t i = 0; i < ws; i++) store_page(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, mib));
    uint64_t t1 = host_us();
    struct pager_stats after_write; pager_snapshot(&after_write);
    for (uint32_t i = 0; i < ws; i++) CHECK(page_ok(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, mib)));
    uint64_t t2 = host_us();
    struct pager_stats s; pager_snapshot(&s);
    CHECK(s.pageouts >= ws - keep && s.pageins && !s.write_errors && !s.read_errors && !s.order_violations);
    CHECK(ledger_reconcile());
    v = snapshot();
    printf("TRACE M4 profile=%u NORMAL_low=%u high=%u available=%u working_set=%u extents=%u->%u commit_waits=%llu commit_pagefile=%u swap_capacity=%u swap_used=%u\n",
           mib, v.low, v.high, keep, ws, 1u, grown, (unsigned long long)s.commit_waits, v.commit_pagefile, v.swap_capacity, v.swap_used);
    printf("TRACE M4 profile=%u pageouts=%llu pageins=%llu clean_discards=%llu batches=%u max_batch=%u reclaim_slices=%llu out_us_per_page=%.1f in_us_per_page=%.1f max_out_step_us=%u max_in_us=%u touch_ms=%llu verify_ms=%llu\n",
           mib, (unsigned long long)s.pageouts, (unsigned long long)s.pageins, (unsigned long long)s.clean_discards,
           disk.write_calls, disk.max_batch, (unsigned long long)drive_slices,
           s.pageouts ? (double)s.out_us / (double)s.pageouts : 0.0, s.pageins ? (double)s.in_us / (double)s.pageins : 0.0,
           s.max_out_us, s.max_in_us, (unsigned long long)((t1 - t0) / 1000), (unsigned long long)((t2 - t1) / 1000));
    (void)after_write;
    finish_process(&u);
    CHECK(snapshot().commit_ram[MM_USER_PRIVATE] == baseline_commit && !snapshot().swap_used);
    /* 30 s idle: trailing extents go back to the configured minimum. */
    release_held(UINT32_MAX);
    g_ticks += PAGER_SHRINK_IDLE_MS + 1;
    for (unsigned n = 0; n < 2 * PAGER_MAX_EXTENTS && pager.extents > PAGER_MIN_EXTENTS; n++) { g_ticks += 1000; manager(); }
    CHECK(pager.extents == PAGER_MIN_EXTENTS && disk.extents == PAGER_MIN_EXTENTS && pager.stats.shrinks == grown - 1);
    CHECK(snapshot().swap_capacity == PAGER_EXTENT_SLOTS && ledger_reconcile());
    printf("TRACE M4 profile=%u idle_shrink extents=%u->%u after_ms=%u live_slots_moved=0\n", mib, grown, pager.extents, PAGER_SHRINK_IDLE_MS);
}

/* ---- B: exhaustion, read-only, no pagefile ---- */
static void exhaustion(void)
{
    /* 64 MiB volume: reserve max(16 MiB, 5%) = 16 MiB; room for one extent. */
    start(128, 64, 40, false);
    CHECK(pager.extents == 1);
    struct mm_ledger v = snapshot();
    hold_until(v.high + 1024);
    struct uaddr u; CHECK(!ua_init(&u, &u));
    uint32_t ram = (uint32_t)(ledger_commit_headroom() - PAGER_EXTENT_SLOTS);
    int big = ua_map_at(&u, CIUKI_MMAP_BASE, (ram + PAGER_EXTENT_SLOTS + 64) * PAGE_SIZE,
        PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE, UA_ANON, 0);
    CHECK(big == -ENOMEM && pager.grow_blocked && pager.extents == 1 && pager.stats.grow_failures);
    CHECK(snapshot().commit_ram[MM_USER_PRIVATE] == 0); /* nothing partially admitted */
    /* Within RAM + capacity still works and pages out; the system stays usable. */
    uint32_t ws = ram + PAGER_EXTENT_SLOTS / 2;
    CHECK(!ua_map_at(&u, CIUKI_MMAP_BASE, ws * PAGE_SIZE, PROT_READ | PROT_WRITE, PROT_READ | PROT_WRITE, UA_ANON, 0));
    for (uint32_t i = 0; i < ws; i++) store_page(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 7));
    for (uint32_t i = 0; i < ws; i += 7) CHECK(page_ok(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 7)));
    CHECK(pager.stats.pageouts);
    printf("TRACE M4 exhaustion volume_mib=64 reserve_mib=16 extents=1 grow=refused(%d) overcommit=ENOMEM within_capacity=%u pages ok pageouts=%llu\n",
           -PAGER_ENOSPC, ws, (unsigned long long)pager.stats.pageouts);
    /* Read-only volume: page-out is deferred, resident data intact. */
    disk.readonly = true;
    struct pager_stats before; pager_snapshot(&before);
    for (uint32_t i = 0; i < 64; i++) store_page(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 8));
    for (unsigned n = 0; n < 50 && ledger_pressure(); n++) drive();
    for (uint32_t i = 0; i < 64; i++) CHECK(page_ok(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 8)));
    CHECK(pager.stats.write_errors == before.write_errors);
    disk.readonly = false;
    finish_process(&u);
    /* No pagefile (open refused): RAM-only commitment, usable system. */
    release_held(UINT32_MAX);
    fresh(128); pager_reset(); disk.open_error = -PAGER_EROFS;
    CHECK(!pager_attach(&fake, &disk)); manager();
    CHECK(pager.state == PAGER_FAILED && !snapshot().swap_capacity && !pager_online());
    struct uaddr w; CHECK(!ua_init(&w, &w));
    CHECK(!ua_map_at(&w, CIUKI_MMAP_BASE, 256 * PAGE_SIZE, 3, 3, UA_ANON, 0));
    for (uint32_t i = 0; i < 256; i++) store_page(&w, CIUKI_MMAP_BASE + i * PAGE_SIZE, i);
    CHECK(page_ok(&w, CIUKI_MMAP_BASE + 255 * PAGE_SIZE, 255));
    finish_process(&w); disk.open_error = 0;
    puts("TRACE M4 readonly=page-out deferred, data intact; no_pagefile=commit RAM-only, process usable");
}

/* Swap a controlled set out under pressure, then release the pressure. */
static void swap_out(struct uaddr *u, uint32_t pages)
{
    for (uint32_t i = 0; i < pages; i++) store_page(u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3));
    struct mm_ledger v = snapshot();
    hold_until(v.low - 64);
    for (unsigned n = 0; n < 200; n++) {
        drive();
        uint32_t swapped = 0;
        for (uint32_t i = 0; i < pages; i++) swapped += !!(slot_at(u, CIUKI_MMAP_BASE + i * PAGE_SIZE)->flags & SLOT_SWAPPED);
        if (swapped == pages) break;
    }
    release_held(4096);
}

/* ---- C: every I/O failure ---- */
static void io_failures(void)
{
    start(128, 4096, 2048, false);
    struct uaddr u; CHECK(!ua_init(&u, &u));
    CHECK(!ua_map_at(&u, CIUKI_MMAP_BASE, 64 * PAGE_SIZE, 3, 3, UA_ANON, 0));
    /* Write failure: one batch fails, its slots retire, pages stay resident. */
    disk.fail_write_from = disk.write_calls + 1; disk.fail_write_count = 1;
    uint32_t capacity = pager.capacity;
    swap_out(&u, 64);
    CHECK(pager.stats.write_errors && pager.counts[ST_BAD] == pager.stats.write_errors);
    CHECK(pager.capacity == capacity - pager.counts[ST_BAD] && snapshot().swap_capacity == pager.capacity);
    for (uint32_t i = 0; i < 64; i++) CHECK(page_ok(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3)));
    printf("TRACE M4 write_failure batch_pages=%llu slots_failed=%u capacity=%u->%u resident_dirty=intact\n",
           (unsigned long long)pager.stats.write_errors, pager.counts[ST_BAD], capacity, pager.capacity);
    /* Read failure: contained -EIO for this process, never zeros; retry works. */
    swap_out(&u, 64);
    uint32_t va = 0;
    for (uint32_t i = 0; i < 64 && !va; i++) if (slot_at(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE)->flags & SLOT_SWAPPED) va = CIUKI_MMAP_BASE + i * PAGE_SIZE;
    CHECK(va);
    uint32_t value = 0;
    disk.fail_read_from = disk.read_calls + 1; disk.fail_read_count = 2;
    CHECK(ua_read(&u, &value, va, 4) == -EIO && value == 0);
    CHECK(ua_fault(&u, va, 4) == -EIO && (slot_at(&u, va)->flags & SLOT_SWAPPED));
    struct uaddr other; CHECK(!ua_init(&other, &other));
    CHECK(!ua_map_at(&other, CIUKI_MMAP_BASE, 4 * PAGE_SIZE, 3, 3, UA_ANON, 0));
    store_page(&other, CIUKI_MMAP_BASE, 77); CHECK(page_ok(&other, CIUKI_MMAP_BASE, 77));
    uint32_t index = (va - CIUKI_MMAP_BASE) / PAGE_SIZE;
    CHECK(page_ok(&u, va, pattern(index, 3))); /* transient failure: verified retry */
    printf("TRACE M4 read_failure result=%d fault=%d other_process=unaffected retry=verified read_errors=%llu\n",
           -EIO, -EIO, (unsigned long long)pager.stats.read_errors);
    /* Reservation momentarily exhausted: bounded retry. */
    swap_out(&u, 64);
    disk.eagain = 3;
    for (uint32_t i = 0; i < 64; i++) CHECK(page_ok(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3)));
    CHECK(!disk.eagain);
    /* Sticky write error: consecutive failed batches stop page-out and the
     * ledger stops admitting commitment against the failed capacity. */
    disk.fail_write_from = disk.write_calls + 1; disk.fail_write_count = 1000;
    swap_out(&u, 64);
    CHECK(pager.sticky && !pager_can_reserve() && snapshot().swap_capacity == pager.counts[ST_USED] + pager.counts[ST_RESERVED]);
    for (uint32_t i = 0; i < 64; i++) CHECK(page_ok(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3)));
    int64_t headroom = ledger_commit_headroom();
    CHECK(!ledger_commit(MM_USER_PRIVATE, (uint32_t)(headroom > 0 ? headroom : 0) + 1));
    disk.fail_write_from = 0; disk.fail_write_count = 0;
    printf("TRACE M4 sticky_write_error after=%u batches capacity_published=%u commit_beyond=refused\n", WRITE_ERROR_LIMIT, snapshot().swap_capacity);
    end_process(&other); finish_process(&u);
    /* Growth and shrink failures; media loss. */
    start(128, 4096, 2048, false);
    disk.grow_error = -PAGER_EIO;
    struct uaddr w; CHECK(!ua_init(&w, &w));
    hold_until(snapshot().high + 1024);
    uint32_t over = (uint32_t)ledger_commit_headroom() + 128;
    CHECK(ua_map_at(&w, CIUKI_MMAP_BASE, over * PAGE_SIZE, 3, 3, UA_ANON, 0) == -ENOMEM && pager.grow_blocked);
    disk.grow_error = 0; g_ticks += GROW_RETRY_MS;
    CHECK(!ua_map_at(&w, CIUKI_MMAP_BASE, over * PAGE_SIZE, 3, 3, UA_ANON, 0) && pager.extents >= 2);
    CHECK(!ua_munmap(&w, CIUKI_MMAP_BASE, over * PAGE_SIZE));
    release_held(UINT32_MAX);
    uint32_t extents = pager.extents; disk.shrink_error = -PAGER_EIO;
    g_ticks += PAGER_SHRINK_IDLE_MS + 1; manager();
    CHECK(pager.extents == extents && pager.stats.shrink_failures == 1 && pager.capacity == extents * PAGER_EXTENT_SLOTS);
    disk.shrink_error = 0;
    CHECK(!ua_map_at(&w, CIUKI_MMAP_BASE, 64 * PAGE_SIZE, 3, 3, UA_ANON, 0));
    swap_out(&w, 64);
    uint32_t swapped = 0;
    for (uint32_t i = 0; i < 64; i++) swapped += !!(slot_at(&w, CIUKI_MMAP_BASE + i * PAGE_SIZE)->flags & SLOT_SWAPPED);
    CHECK(swapped);
    pager_media_lost();
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t x = 0; int e = ua_read(&w, &x, CIUKI_MMAP_BASE + i * PAGE_SIZE, 4);
        bool out = slot_at(&w, CIUKI_MMAP_BASE + i * PAGE_SIZE)->flags & SLOT_SWAPPED;
        CHECK(out ? e == -EIO && !x : !e && x == pattern(i, 3));
    }
    CHECK(snapshot().swap_capacity == pager.counts[ST_USED]);
    printf("TRACE M4 grow_failure=blocked,retried shrink_failure=extent republished media_lost=EIO for %u swapped pages, resident pages intact\n", swapped);
    finish_process(&w);
}

/* ---- D: idle prewrite versus foreground demand ---- */
static uint32_t demand_va;
static struct uaddr *demand_space;
static void demand_hook(struct pager_io *io)
{
    CHECK(io->cls == PAGER_PREWRITE && io->write);
    struct task *s = g_current; g_current = &task_b;
    CHECK(page_ok(demand_space, demand_va, pattern((demand_va - CIUKI_MMAP_BASE) / PAGE_SIZE, 3)));
    g_current = s;
}
static void prewrite_demand(void)
{
    start(128, 4096, 2048, false);
    struct uaddr u; CHECK(!ua_init(&u, &u));
    CHECK(!ua_map_at(&u, CIUKI_MMAP_BASE, 128 * PAGE_SIZE, 3, 3, UA_ANON, 0));
    swap_out(&u, 32); /* pages 0..31 swapped; one becomes the demand target */
    for (uint32_t i = 32; i < 128; i++) store_page(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3));
    struct mm_ledger v = snapshot();
    release_held(UINT32_MAX); hold_until(v.high + v.high / 4); /* near high, no pressure */
    CHECK(!ledger_pressure());
    /* Interactive pages 120..127 are accessed every slice. */
    struct task *s = g_current; g_current = &pager_task;
    disk.busy = true; g_ticks += 1000;
    CHECK(!pager_prewrite_step(g_ticks)); /* VFAT writes pending: no prewrite */
    disk.busy = false; pager.last_demand = g_ticks;
    CHECK(!pager_prewrite_step(g_ticks + 10)); /* demand within 100 ms */
    uint32_t free_before = pmm_free_count(), writes = disk.writes;
    for (unsigned n = 0; n < 400 && pager.stats.prewrites < 64; n++) {
        for (uint32_t i = 120; i < 128; i++) *pte(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, false) |= PTE_A;
        g_ticks += PAGER_PREWRITE_GAP_MS;
        pager_prewrite_step(g_ticks); while (pager_pageout_step()) {}
    }
    g_current = s;
    CHECK(pager.stats.prewrites >= 64 && disk.writes > writes && pmm_free_count() == free_before);
    unsigned interactive = 0, cached = 0;
    for (uint32_t i = 32; i < 128; i++) {
        struct ua_slot *slot = slot_at(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE);
        CHECK(slot->frame && !(slot->flags & SLOT_SWAPPED)); /* frames kept */
        if (i >= 120) interactive += !slot->frame->swap;
        cached += slot->frame->swap && !frame_descriptor(slot->frame)->dirty_epoch;
        CHECK(*pte(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, false) & PTE_P); /* still mapped */
    }
    CHECK(interactive == 8 && cached >= 64);
    printf("TRACE M4 prewrite fs_busy=deferred demand_quiet=deferred prewritten=%llu frames_reclaimed=0 interactive_prewritten=0 clean_copies=%u\n",
           (unsigned long long)pager.stats.prewrites, cached);
    /* Redirty one prewritten page, then pressure: clean copies are discarded
     * without I/O, the redirtied page is rewritten in its own slot. */
    uint32_t redirty = 0;
    for (uint32_t i = 32; i < 120 && !redirty; i++) if (slot_at(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE)->frame->swap) redirty = i;
    CHECK(redirty && slot_at(&u, CIUKI_MMAP_BASE + redirty * PAGE_SIZE)->frame->swap);
    uint32_t own = slot_at(&u, CIUKI_MMAP_BASE + redirty * PAGE_SIZE)->frame->swap;
    store_page(&u, CIUKI_MMAP_BASE + redirty * PAGE_SIZE, pattern(redirty, 99));
    uint64_t clean_before = pager.stats.clean_discards;
    writes = disk.writes;
    swap_out(&u, 0);
    v = snapshot(); hold_until(v.low - 64);
    for (unsigned n = 0; n < 300; n++) {
        drive();
        if (slot_at(&u, CIUKI_MMAP_BASE + redirty * PAGE_SIZE)->flags & SLOT_SWAPPED) break;
    }
    release_held(4096);
    CHECK(pager.stats.clean_discards > clean_before);
    CHECK((slot_at(&u, CIUKI_MMAP_BASE + redirty * PAGE_SIZE)->flags & SLOT_SWAPPED) &&
          slot_at(&u, CIUKI_MMAP_BASE + redirty * PAGE_SIZE)->swap == own);
    printf("TRACE M4 pressure_after_prewrite clean_discards=%llu writes_during_reclaim=%u redirtied_rewritten_in_place=1\n",
           (unsigned long long)(pager.stats.clean_discards - clean_before), disk.writes - writes);
    CHECK(page_ok(&u, CIUKI_MMAP_BASE + redirty * PAGE_SIZE, pattern(redirty, 99)));
    for (uint32_t i = 32; i < 128; i++) if (i != redirty) CHECK(page_ok(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3)));
    /* Demand arrival during a speculative write cancels it: the demand read
     * completes first, the prewritten pages stay dirty, slots return. */
    swap_out(&u, 32);
    demand_space = &u; demand_va = 0;
    for (uint32_t i = 0; i < 32 && !demand_va; i++) if (slot_at(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE)->flags & SLOT_SWAPPED) demand_va = CIUKI_MMAP_BASE + i * PAGE_SIZE;
    CHECK(demand_va);
    for (uint32_t i = 64; i < 96; i++) store_page(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3)); /* fresh dirty versions */
    release_held(UINT32_MAX); v = snapshot(); hold_until(v.high + v.high / 4);
    uint64_t cancelled = pager.stats.cancelled, reads = disk.reads;
    uint32_t used = pager.counts[ST_USED] + pager.counts[ST_RESERVED];
    disk.during = demand_hook;
    s = g_current; g_current = &pager_task;
    for (unsigned n = 0; n < 400 && disk.during; n++) {
        g_ticks += PAGER_DEMAND_QUIET_MS + PAGER_PREWRITE_GAP_MS;
        pager_prewrite_step(g_ticks); while (pager_pageout_step()) {}
    }
    g_current = s;
    CHECK(!disk.during && pager.stats.cancelled > cancelled && disk.reads == reads + 1 && disk.cancels);
    CHECK(!(slot_at(&u, demand_va)->flags & SLOT_SWAPPED) && pager.counts[ST_RESERVED] == 0);
    CHECK(pager.counts[ST_USED] + pager.counts[ST_RESERVED] <= used); /* cancelled slots returned */
    printf("TRACE M4 demand_vs_prewrite cancelled_pages=%llu demand_reads=1 demand_first=1 slots_returned=1\n",
           (unsigned long long)(pager.stats.cancelled - cancelled));
    for (uint32_t i = 0; i < 128; i++) if (i != redirty) CHECK(page_ok(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3)));
    finish_process(&u);
}

/* ---- E: races with exit, unmap, redirty, faults and removal ---- */
static struct uaddr *race_space;
static int race_kind;
static unsigned race_hits;
static int shutdown_result;
static void race_hook(struct pager_io *io)
{
    struct task *s = g_current; g_current = &task_b;
    race_hits++;
    if (race_kind == 0) { cr3 = 0; ua_destroy(race_space); }
    else if (race_kind == 1) for (uint32_t i = 0; i < 8; i++) store_page(race_space, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 55));
    else if (race_kind == 2) CHECK(!ua_munmap(race_space, CIUKI_MMAP_BASE + 2 * PAGE_SIZE, 2 * PAGE_SIZE));
    else if (race_kind == 3) shutdown_result = pager_shutdown();
    else if (race_kind == 4) {
        /* Second fault on the page being read: coalesced, no second I/O. */
        CHECK(!io->write);
        task_b.preempt_holds = 1;
        CHECK(ua_fault(race_space, CIUKI_MMAP_BASE, 4) == -EBUSY);
        task_b.preempt_holds = 0;
    }
    g_current = s;
}
static void queue_eight(struct uaddr *u)
{
    for (uint32_t i = 0; i < 8; i++) store_page(u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3));
    struct mm_ledger v = snapshot(); hold_until(v.low - 64);
    struct task *s = g_current; g_current = &reclaim_task;
    for (unsigned n = 0; n < 100000; n++) {
        reclaim_slice();
        unsigned queued = 0;
        for (unsigned i = 0; i < RECLAIM_QUEUE_SIZE; i++) queued += modified[i].page && !modified[i].taken;
        if (queued >= 8) break;
    }
    g_current = s;
}
static void races(void)
{
    /* Exit while the page-out write is in flight: frames freed once, at
     * completion; slots returned; the ledger reconciles. */
    start(128, 4096, 2048, false);
    struct uaddr *u = calloc(1, sizeof(*u)); CHECK(u && !ua_init(u, u));
    CHECK(!ua_map_at(u, CIUKI_MMAP_BASE, 8 * PAGE_SIZE, 3, 3, UA_ANON, 0));
    queue_eight(u);
    race_space = u; race_kind = 0; race_hits = 0; disk.during = race_hook;
    uint32_t free_before = pmm_free_count();
    CHECK(pageout() && race_hits == 1);
    CHECK(!pager.counts[ST_USED] && !pager.counts[ST_RESERVED] && pmm_free_count() > free_before && ledger_reconcile());
    free(u);
    release_held(UINT32_MAX);
    puts("TRACE M4 race exit_during_pageout=frames freed at completion, slots returned, double_free=0");
    /* Redirty during the write: the newer version stays resident. */
    struct uaddr w; CHECK(!ua_init(&w, &w));
    CHECK(!ua_map_at(&w, CIUKI_MMAP_BASE, 8 * PAGE_SIZE, 3, 3, UA_ANON, 0));
    queue_eight(&w);
    race_space = &w; race_kind = 1; disk.during = race_hook;
    CHECK(pageout());
    for (uint32_t i = 0; i < 8; i++) {
        CHECK(!(slot_at(&w, CIUKI_MMAP_BASE + i * PAGE_SIZE)->flags & SLOT_SWAPPED));
        CHECK(page_ok(&w, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 55)));
    }
    release_held(UINT32_MAX);
    puts("TRACE M4 race redirty_during_pageout=resident newer version kept, stale copy not trusted");
    /* Unmap during the write. */
    queue_eight(&w);
    race_kind = 2; disk.during = race_hook;
    CHECK(pageout());
    CHECK(!ua_find(&w, CIUKI_MMAP_BASE + 2 * PAGE_SIZE) && ledger_reconcile());
    release_held(UINT32_MAX);
    for (uint32_t i = 0; i < 8; i++) if (i < 2 || i > 3) CHECK(page_ok(&w, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3)));
    CHECK(!ua_map_at(&w, CIUKI_MMAP_BASE + 2 * PAGE_SIZE, 2 * PAGE_SIZE, 3, 3, UA_ANON, 0));
    puts("TRACE M4 race unmap_during_pageout=owner gone, frame freed once");
    /* Concurrent faults on one swapped page coalesce into one read. */
    swap_out(&w, 2);
    CHECK(slot_at(&w, CIUKI_MMAP_BASE)->flags & SLOT_SWAPPED);
    unsigned reads = disk.reads;
    race_kind = 4; disk.during = race_hook;
    CHECK(page_ok(&w, CIUKI_MMAP_BASE, pattern(0, 3)) && disk.reads == reads + 1);
    struct task *s = g_current; g_current = &task_b;
    CHECK(!ua_fault(&w, CIUKI_MMAP_BASE, 4) && disk.reads == reads + 1);
    g_current = s;
    puts("TRACE M4 race concurrent_faults=one page-in, second fault resolved from the resident frame");
    /* Removal/shutdown while I/O is in flight is refused; after it retires
     * the dependency is released, live slots stay readable, page-out stops,
     * and a failed storage shutdown resumes the store. */
    swap_out(&w, 8);
    queue_eight(&w);
    race_kind = 3; disk.during = race_hook; shutdown_result = 1;
    CHECK(pageout() && shutdown_result == -PAGER_EBUSY && pager.state == PAGER_ACTIVE);
    release_held(UINT32_MAX);
    CHECK(pager_volume_live());
    CHECK(!pager_shutdown() && pager.state == PAGER_QUIESCED && disk.detached && !snapshot().swap_capacity == !pager.counts[ST_USED]);
    CHECK(!pager_can_reserve());
    unsigned live_reads = disk.reads;
    for (uint32_t i = 0; i < 8; i++) CHECK(page_ok(&w, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3)));
    CHECK(disk.reads > live_reads); /* swapped pages remain readable while quiesced */
    CHECK(!pager_resume() && pager.state == PAGER_ACTIVE && !disk.detached && pager_can_reserve());
    printf("TRACE M4 shutdown inflight=refused(%d) quiesced_reads=%u resume=maps verified\n", -PAGER_EBUSY, disk.reads - live_reads);
    finish_process(&w);
}

/* ---- F: lock order, including the recursive-allocation negative control ---- */
static struct uaddr *order_space;
static unsigned order_steps;
static void order_hook(struct pager_io *io)
{
    (void)io;
    uint32_t violations = pager.stats.order_violations;
    /* Inside page-out + I/O: allocation that could need reclaim, reclaim
     * itself, the extent manager and a recursive fault must all be refused. */
    CHECK(!frame_new(false, 1));
    CHECK(pager.stats.order_violations == violations + 1);
    struct mm_ledger before = snapshot(); reclaim_slice();
    CHECK(snapshot().reclaim_scanned == before.reclaim_scanned && pager.stats.order_violations == violations + 2);
    CHECK(!pager_manager_step(g_ticks) && pager.stats.order_violations == violations + 3);
    CHECK(ua_fault(order_space, CIUKI_MMAP_BASE + 7 * PAGE_SIZE, 6) == -EDEADLK && pager.stats.order_violations == violations + 4);
    CHECK(!pager_commit_wait(1) && pager.stats.order_violations == violations + 5);
    order_steps++;
}
static void order_control(void)
{
    start(128, 4096, 2048, false);
    struct uaddr u; CHECK(!ua_init(&u, &u));
    CHECK(!ua_map_at(&u, CIUKI_MMAP_BASE, 8 * PAGE_SIZE, 3, 3, UA_ANON, 0));
    queue_eight(&u);
    order_space = &u; disk.during = order_hook;
    CHECK(pageout() && order_steps == 1 && pager.stats.order_violations == 5);
    release_held(UINT32_MAX);
    /* Nested object fault (object already held by this task) is refused. */
    swap_out(&u, 2);
    struct task *s = g_current; g_current = &task_b;
    CHECK(pager_order_enter(PAGER_ORDER_OBJECT, 0));
    CHECK(ua_fault(&u, CIUKI_MMAP_BASE, 4) == -EDEADLK);
    pager_order_exit(PAGER_ORDER_OBJECT);
    CHECK(!task_b.pager_order);
    /* Positive controls: the same operations outside pager context. */
    struct ua_frame *f = frame_new(false, 1); CHECK(f); frame_put(f);
    CHECK(!ua_fault(&u, CIUKI_MMAP_BASE, 4) && page_ok(&u, CIUKI_MMAP_BASE, pattern(0, 3)));
    g_current = s;
    CHECK(!pager_task.pager_order && !manager_task.pager_order && !reclaim_task.pager_order && !pager.host_order);
    printf("TRACE M4 order negative_control violations=%u (alloc, reclaim, manager, recursive fault, commit wait) nested_object=EDEADLK positive=ok\n",
           pager.stats.order_violations - 1);
    finish_process(&u);
}

/* LeakSanitizer window for state abandoned by the simulated power cut. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PAGER_TEST_LSAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define PAGER_TEST_LSAN 1
#endif
#ifdef PAGER_TEST_LSAN
#include <sanitizer/lsan_interface.h>
static void lsan_disable(void) { __lsan_disable(); }
static void lsan_enable(void) { __lsan_enable(); }
#else
static void lsan_disable(void) {}
static void lsan_enable(void) {}
#endif

/* ---- G: swap is untrusted across reboot ---- */
static void reboot(void)
{
    start(128, 4096, 2048, false);
    disk.fragment = true; /* 4 runs per extent, 96 slots failed capacity */
    /* The process built before the power cut is abandoned with the RAM it
     * lived in; its host allocations are expected to stay unreferenced. */
    lsan_disable();
    struct uaddr u; CHECK(!ua_init(&u, &u));
    hold_until(snapshot().high + 1024);
    uint32_t ws = (uint32_t)ledger_commit_headroom() + 2 * PAGER_EXTENT_SLOTS;
    CHECK(!ua_map_at(&u, CIUKI_MMAP_BASE, ws * PAGE_SIZE, 3, 3, UA_ANON, 0));
    uint32_t sample = ws < 4096 ? ws : 4096;
    for (uint32_t i = 0; i < sample; i++) store_page(&u, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 11));
    CHECK(pager.stats.pageouts);
    uint32_t extents = disk.extents;
    /* Power cut: no shutdown, no cleanup. RAM and pager state vanish; the
     * file keeps its extents and old bytes. */
    held_count = 0;
    resident_head = resident_cursor = prewrite_cursor = 0; /* RAM contents are gone */
    zero_frame = 0; sample_spaces = sample_space = 0; sample_va = 0;
    (void)u;
    lsan_enable();
    start(128, 4096, 2048, true);
    CHECK(pager.extents == extents && !pager.counts[ST_USED] && !pager.counts[ST_RESERVED]);
    CHECK(pager.counts[ST_BAD] == extents * 96 && pager.capacity == extents * (PAGER_EXTENT_SLOTS - 96));
    struct uaddr w; CHECK(!ua_init(&w, &w));
    CHECK(!ua_map_at(&w, CIUKI_MMAP_BASE, 256 * PAGE_SIZE, 3, 3, UA_ANON, 0));
    for (uint32_t i = 0; i < 256; i++) CHECK(!memory_word(&w, CIUKI_MMAP_BASE + i * PAGE_SIZE));
    swap_out(&w, 64);
    for (uint32_t i = 0; i < 64; i++) CHECK(page_ok(&w, CIUKI_MMAP_BASE + i * PAGE_SIZE, pattern(i, 3)));
    finish_process(&w);
    release_held(UINT32_MAX);
    g_ticks += PAGER_SHRINK_IDLE_MS + 1;
    for (unsigned n = 0; n < 2 * PAGER_MAX_EXTENTS && pager.extents > PAGER_MIN_EXTENTS; n++) { g_ticks += 1000; manager(); }
    CHECK(pager.extents == PAGER_MIN_EXTENTS);
    disk.fragment = false;
    printf("TRACE M4 reboot extents_kept=%u ownership=empty untrusted_reads=0 new_pages=zero fragmented_runs=4 failed_slots_per_extent=96 shrink_to=%u\n",
           extents, pager.extents);
}

/* ---- H: R1 bounds of pager work units ---- */
static void bounds(void)
{
    start(128, 4096, 2048, false);
    /* Worst slot search: every extent full except the last slot of the last. */
    struct task *s = g_current; g_current = &manager_task;
    for (unsigned n = 0; n < 7; n++) CHECK(!grow_one());
    g_current = s;
    for (uint32_t x = 0; x < pager.extents; x++)
        for (uint32_t i = 0; i < PAGER_EXTENT_SLOTS; i++)
            if (x + 1 < pager.extents || i + 1 < PAGER_EXTENT_SLOTS) state_set(pager.extent[x], i, ST_USED);
    pager.cursor = 0;
    for (uint32_t x = 0; x < pager.extents; x++) pager.extent[x]->hint = 0;
    uint64_t t = cpu_path_clock(); uint32_t got, h = slot_reserve(1, &got); uint64_t worst = cpu_path_clock() - t;
    /* The scan budget bounds the work even when the free slot is beyond it. */
    CHECK(!h || got == 1);
    unsigned attempts = 1;
    while (!h && attempts < 64) { h = slot_reserve(1, &got); attempts++; }
    CHECK((h & HANDLE_MASK) == pager.extents * PAGER_EXTENT_SLOTS && h >> HANDLE_BITS == pager.extent[pager.extents - 1]->generation);
    for (uint32_t x = 0; x < pager.extents; x++)
        for (uint32_t i = 0; i < PAGER_EXTENT_SLOTS; i++) state_set(pager.extent[x], i, ST_FREE);
    publish();
    printf("TRACE M4 R1 slot_scan_words<=%u attempts=%u worst_scan_us=%llu runs<=%u batch<=%u pages\n",
           SCAN_WORDS, attempts, (unsigned long long)(worst / 1000), PAGER_RUNS, PAGER_BATCH);
}

int main(void)
{
    ram = calloc(1, 512u << 20); CHECK(ram);
    held = calloc(512u * 256u, sizeof(*held)); CHECK(held);
    profile(128); profile(256); profile(512);
    exhaustion(); io_failures(); prewrite_demand(); races(); order_control(); reboot(); bounds();
    keep_maxima();
    CHECK(worst_out_us < 1000 && worst_in_us < 1000 && worst_slice_us < 1000);
    printf("PASS M4 pager checks=%u max_out_step_us=%u max_in_us=%u max_prewrite_slice_us=%u (host ASan CPU, I/O wait excluded)\n",
           checks, worst_out_us, worst_in_us, worst_slice_us);
    release_held(UINT32_MAX);
    for (unsigned i = 0; i < DISK_PAGES; i++) free(disk.page[i]);
    free(held); free(ram); return 0;
}
