/* f3-46 (contract D1): the production f0-48 replacement (core/reclaim.c)
 * and f0-49 backing store (core/pager.c, PAGER_HOST_MODEL) linked beside the
 * production DOS VM. Only the M1 ledger, the block-cache hook and the
 * pagefile storage are host models here; the PMM is the harness's (with
 * production-shaped PFN descriptors). Workers are driven explicitly.
 * Included by dosvm_test.c after the VM sources. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/reclaim.h>
#include <ciuki/pager.h>

/* ---- M1 ledger model: pressure, commitment by owner, swap publication ---- */
static bool host_pressure;
static int64_t host_reserve[MM_OWNERS], host_commit[MM_OWNERS], host_commit_limit = -1;
static uint32_t host_swap_capacity, host_swap_used, host_reclaimed, host_normal_free;
void ledger_snapshot(struct mm_ledger *out)
{
    memset(out, 0, sizeof(*out));
    out->pressure = host_pressure;
    out->zone[ZONE_NORMAL].free = host_normal_free ? host_normal_free : pmm_free_count();
    out->low = 1024; out->high = 2048;
    out->swap_capacity = host_swap_capacity; out->swap_used = host_swap_used;
    for (unsigned o = 0; o < MM_OWNERS; o++) out->commit_ram[o] = (uint32_t)host_commit[o];
}
bool ledger_pressure(void) { return host_pressure; }
void ledger_reclaim_account(const struct reclaim_result *r, unsigned episodes, unsigned faults, unsigned stalls)
{ (void)episodes; (void)faults; (void)stalls; if (r) host_reclaimed += r->reclaimed; }
void ledger_reclaim_owner_delta(unsigned owner, int32_t delta) { (void)owner; (void)delta; }
void ledger_swap(uint32_t capacity, uint32_t used) { host_swap_capacity = capacity; host_swap_used = used; }
void ledger_promise(enum mm_owner owner, int32_t reserved, int32_t committed)
{
    CHECK((unsigned)owner < MM_OWNERS);
    host_reserve[owner] += reserved; host_commit[owner] += committed;
    CHECK(host_reserve[owner] >= 0 && host_commit[owner] >= 0); /* never below baseline */
}
static int64_t host_committed(void)
{ int64_t n = 0; for (unsigned o = 0; o < MM_OWNERS; o++) n += host_commit[o]; return n; }
bool ledger_commit(enum mm_owner owner, uint32_t pages)
{
    if (host_commit_limit >= 0 && host_committed() + pages > host_commit_limit + host_swap_capacity) return false;
    host_commit[owner] += pages;
    return true;
}
int64_t ledger_commit_headroom(void)
{ return host_commit_limit < 0 ? (int64_t)1 << 30 : host_commit_limit + host_swap_capacity - host_committed(); }
void ledger_page_activity(uint32_t phys, bool hot)
{
    struct mm_page_descriptor *d = pmm_descriptor(phys);
    if (d) d->reserved = (uint16_t)((d->reserved & ~MM_PAGE_HOT) | (hot ? MM_PAGE_HOT : 0));
}
void kwait_wake_irq(struct kwait *q, bool io) { (void)q; (void)io; }
void cache_reclaim_step(struct block_cache *c, bool speculative, struct reclaim_result *r)
{ (void)c; (void)speculative; *r = (struct reclaim_result){.scanned = 1, .done = true}; }
#include "../../src/kernel/core/reclaim.c"
#define PAGER_HOST_MODEL 1
#include "../../src/kernel/core/pager.c"

/* ---- pagefile storage model: two 16 MiB extents, fault injection ---- */
#define VMP_SLOTS (2u * PAGER_EXTENT_SLOTS)
static uint8_t *vmp_disk[VMP_SLOTS];
static unsigned vmp_writes, vmp_reads, vmp_fail_read, vmp_fail_write;
static void (*vmp_during)(struct pager_io *);
static int vmp_open(void *ctx, uint32_t *existing) { (void)ctx; *existing = 0; return 0; }
static int vmp_grow(void *ctx, uint32_t extent, struct pager_map *map)
{
    (void)ctx;
    if (extent >= 2) return -PAGER_ENOSPC;
    memset(map, 0, sizeof(*map));
    map->runs = 1;
    map->run[0] = (struct pager_run){0, PAGER_EXTENT_SLOTS, 2048u + (uint64_t)extent * PAGER_EXTENT_SLOTS * 8};
    return 0;
}
static int vmp_shrink(void *ctx, uint32_t extents) { (void)ctx; (void)extents; return 0; }
static int vmp_space(void *ctx, uint64_t *free, uint64_t *capacity)
{ (void)ctx; *free = 1ull << 30; *capacity = 1ull << 31; return 0; }
static int vmp_submit(void *ctx, struct pager_io *io) { (void)ctx; io->token = io; return 0; }
static int vmp_wait(void *ctx, struct pager_io *io)
{
    (void)ctx;
    CHECK(io->token == io && io->buffer && io->bytes && !(io->bytes % PAGER_PAGE) && io->lba >= 2048);
    void (*hook)(struct pager_io *) = vmp_during;
    vmp_during = 0;
    if (hook) hook(io);  /* other tasks run while the request is in flight */
    io->token = 0;
    unsigned pages = io->bytes / PAGER_PAGE;
    uint64_t index = (io->lba - 2048u) / 8u;
    CHECK(index + pages <= VMP_SLOTS);
    if (io->write) {
        if (vmp_fail_write) { vmp_fail_write--; return -PAGER_EIO; }
        for (unsigned k = 0; k < pages; k++) {
            if (!vmp_disk[index + k]) { vmp_disk[index + k] = malloc(PAGER_PAGE); CHECK(vmp_disk[index + k]); }
            memcpy(vmp_disk[index + k], (uint8_t *)io->buffer + k * PAGER_PAGE, PAGER_PAGE);
        }
        vmp_writes += pages;
        return 0;
    }
    if (vmp_fail_read) { vmp_fail_read--; return -PAGER_EIO; }
    /* Never a slot without a written copy: no fabricated page-in. */
    CHECK(pages == 1 && vmp_disk[index]);
    memcpy(io->buffer, vmp_disk[index], PAGER_PAGE);
    vmp_reads++;
    return 0;
}
static bool vmp_cancel(void *ctx, struct pager_io *io) { (void)ctx; (void)io; return false; }
static bool vmp_idle(void *ctx) { (void)ctx; return true; }
static bool vmp_writable(void *ctx) { (void)ctx; return true; }
static const struct pager_backend vmp_backend = {.open = vmp_open, .grow = vmp_grow, .shrink = vmp_shrink,
    .space = vmp_space, .submit = vmp_submit, .wait = vmp_wait, .cancel = vmp_cancel,
    .idle = vmp_idle, .writable = vmp_writable};
static struct task vmp_reclaim_task, vmp_pager_task, vmp_manager_task;
static uint64_t vmp_saved_tsc;
static void vmp_attach(void)
{
    memset(&pager, 0, sizeof(pager));
    vmp_saved_tsc = g_tsc_per_ms; g_tsc_per_ms = 1000;
    host_pressure = false; host_normal_free = 0; host_commit_limit = -1;
    vmp_writes = vmp_reads = vmp_fail_read = vmp_fail_write = 0;
    CHECK(!pager_attach(&vmp_backend, 0));
    struct task *saved = g_current; g_current = &vmp_manager_task;
    /* f0-49c: store opening waits for the desktop's first frame (or
     * console fallback). This VM fixture explicitly models that release. */
    CHECK(!pager_manager_step(g_ticks));
    CHECK(!pager.opened && !pager_online() && !pager.capacity);
    pager_bootstrap_complete();
    CHECK(pager_manager_step(g_ticks));
    g_current = saved;
    CHECK(pager_online() && pager.capacity == PAGER_EXTENT_SLOTS && host_swap_capacity == PAGER_EXTENT_SLOTS);
}
/* What the reclaim worker and the pager worker do in one pressure cycle. */
static unsigned vmp_cycle(unsigned slices)
{
    unsigned steps = 0;
    struct task *saved = g_current;
    for (unsigned n = 0; n < slices; n++) {
        g_current = &vmp_reclaim_task; reclaim_slice();
        g_current = &vmp_pager_task;
        while (pager_pageout_step()) steps++;
    }
    g_current = saved;
    return steps;
}
/* Pressure until the pool's unpinned resident pages are paged out. */
static void vmp_page_out(struct dosvm *v, uint32_t keep)
{
    host_pressure = true;
    for (unsigned round = 0; round < 64 && v->memory.pool.resident > keep; round++) vmp_cycle(2048);
    host_pressure = false;
    CHECK(v->memory.pool.resident == keep);
}
static void vmp_detach(void)
{
    struct task *saved = g_current; g_current = &vmp_manager_task;
    CHECK(!pager.inflight && !reclaim_writeback_waiting());
    CHECK(!pager.counts[ST_USED] && !pager.counts[ST_RESERVED]); /* every slot returned */
    while (pager.extents) retire_extent(--pager.extents);
    for (unsigned i = 0; i < PAGER_BATCH; i++) mm_free(pager.bounce_phys + i * PAGE_SIZE, MM_PAGER);
    memset(&pager, 0, sizeof(pager));
    g_current = saved;
    for (unsigned i = 0; i < VMP_SLOTS; i++) { free(vmp_disk[i]); vmp_disk[i] = 0; }
    host_swap_capacity = host_swap_used = 0;
    g_tsc_per_ms = vmp_saved_tsc;
    CHECK(!resident_head && !prewrite_queued);
}
