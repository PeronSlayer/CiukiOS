#include <ciuki/system_settings.h>
/* f0-49 backing store (M4): fixed page slots in 16 MiB extents of one
 * dynamic file, page-out of f0-48's modified queue, demand page-in, idle
 * prewrite and trailing shrink. Numbers are CiukiOS policy (pager.h).
 *
 * Slot metadata is cooperative-atomic: the F0 kernel is uniprocessor and
 * non-preemptible, no interrupt handler touches it, and no slot operation
 * sleeps or allocates. Only the ledger update takes IRQ exclusion. The
 * extent manager is the only path that allocates or enters the VFS.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/ledger.h>
#include <ciuki/reclaim.h>
#include <ciuki/pager.h>
static unsigned pager_min_extents(void)
{
    const struct system_perf *p = system_performance ? system_performance() : 0;
    return p && p->vm == 1 ? p->min_mb / 16 : PAGER_MIN_EXTENTS;
}
static unsigned pager_max_extents(void)
{
    const struct system_perf *p = system_performance ? system_performance() : 0;
    return p && p->vm == 1 ? p->max_mb / 16 : PAGER_MAX_EXTENTS;
}

#include <ciuki/sync.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/cpustat.h>

#define ST_FREE 0u
#define ST_RESERVED 1u
#define ST_USED 2u
#define ST_BAD 3u
#define SECTORS_PER_PAGE (PAGER_PAGE / 512u)
#define SCAN_WORDS 256u          /* free-map words per reservation attempt */
#define RESERVE_WAITS 2000u      /* 1 ms each for a page-in request record */
#define WRITE_ERROR_LIMIT 4u     /* consecutive failed batches: stop page-out */
#define DRAIN_MS 30000u          /* shutdown wait for in-flight page I/O */
#define RECORD_MS 30000u
#define GROW_RETRY_MS 30000u
/* Handle = extent generation (13 bits) << 19 | (extent * 4096 + slot + 1). */
#define HANDLE_BITS 19u
#define HANDLE_MASK ((1u << HANDLE_BITS) - 1u)
#define GENERATION_MASK ((1u << (32u - HANDLE_BITS)) - 1u)

struct pager_extent {
    uint32_t state[PAGER_EXTENT_SLOTS / 16];   /* 2 bits per slot */
    uint32_t free_map[PAGER_EXTENT_SLOTS / 32]; /* 1 = free */
    struct pager_map map;
    uint32_t counts[4], hint, phys, generation;
};
_Static_assert(sizeof(struct pager_extent) <= 4096, "one metadata page per extent");

static struct {
    struct pager_extent *extent[PAGER_MAX_EXTENTS];
    uint32_t extents, capacity, counts[4], inflight, cursor, want, write_errors, generation;
    enum pager_state state;
    int sticky;
    const struct pager_backend *backend;
    void *ctx;
    bool opened, boot_ready, grow_blocked, out_busy, workers, cancel_requested;
    uint64_t idle_since, last_demand, last_prewrite, last_record, last_violation, grow_retry;
    struct pager_io *speculative;
    uint8_t *bounce;
    uint32_t bounce_phys;
    struct mm_pin pin;
    struct reclaim_io retry[PAGER_BATCH * 2];
    bool retry_ok[PAGER_BATCH * 2];
    unsigned retries;
    struct kwait work, manager;
    struct pager_stats stats;
    uint8_t host_order;
} pager;

/* ---- time and order ---- */

static uint64_t pclock(void) { return cpu_path_clock ? cpu_path_clock() : ktime_cycles(); }
static uint64_t waited; /* cycles spent at safe points (I/O and reservation waits) */
/* CPU work since start, excluding the waits it contained: the R1 span. */
static uint32_t elapsed_us(uint64_t start, uint64_t waited_before)
{
    uint64_t d = pclock() - start, w = waited - waited_before;
    d = d > w ? d - w : 0;
    return g_tsc_per_ms ? (uint32_t)(d / g_tsc_per_ms * 1000 + (d % g_tsc_per_ms) * 1000 / g_tsc_per_ms) : 0;
}
static uint64_t now_ms(void) { return g_ticks; }
static uint8_t *order_mask(void) { return g_current ? &g_current->pager_order : &pager.host_order; }
bool pager_order_check(enum pager_order level, uintptr_t site)
{
    if (level < PAGER_ORDER_ASPACE || level >= PAGER_ORDER_LEVELS) return false;
    uint8_t held = *order_mask();
    if (!(held >> (level - 1))) return true;
    pager.stats.order_violations++;
    uint64_t now = now_ms();
    if (!pager.last_violation || now - pager.last_violation >= 1000) {
        pager.last_violation = now ? now : 1;
        klog("[pager] order violation want=%u held=0x%x site=%p count=%u",
             (unsigned)level, (unsigned)held, (void *)site, pager.stats.order_violations);
    }
    return false;
}
bool pager_order_enter(enum pager_order level, uintptr_t site)
{
    if (!pager_order_check(level, site)) return false;
    *order_mask() |= (uint8_t)(1u << (level - 1));
    return true;
}
void pager_order_exit(enum pager_order level)
{
    if (level >= PAGER_ORDER_ASPACE && level < PAGER_ORDER_LEVELS)
        *order_mask() &= (uint8_t)~(1u << (level - 1));
}
#define SITE ((uintptr_t)__builtin_return_address(0))

/* ---- slot map ---- */

static uint32_t state_get(const struct pager_extent *x, uint32_t i) { return (x->state[i / 16] >> (i % 16 * 2)) & 3u; }
static void state_set(struct pager_extent *x, uint32_t i, uint32_t to)
{
    uint32_t from = state_get(x, i);
    if (from == to) return;
    x->state[i / 16] = (x->state[i / 16] & ~(3u << (i % 16 * 2))) | (to << (i % 16 * 2));
    x->counts[from]--; x->counts[to]++;
    pager.counts[from]--; pager.counts[to]++;
    if (from == ST_FREE) x->free_map[i / 32] &= ~(1u << (i % 32));
    if (to == ST_FREE) x->free_map[i / 32] |= 1u << (i % 32);
    if (to == ST_BAD) pager.capacity--;
}
static void publish(void)
{
    /* Only an active store admits new commitment; a quiesced or failed one
     * still accounts the slots that back existing pages. */
    uint32_t used = pager.counts[ST_USED] + pager.counts[ST_RESERVED];
    if (ledger_swap) ledger_swap(pager.state == PAGER_ACTIVE && !pager.sticky ? pager.capacity : used, used);
}
static struct pager_extent *slot_extent(uint32_t handle, uint32_t *index)
{
    if (!(handle & HANDLE_MASK)) return 0;
    uint32_t s = (handle & HANDLE_MASK) - 1, x = s / PAGER_EXTENT_SLOTS;
    /* A stale handle can never address a retired and republished extent. */
    if (x >= pager.extents || !pager.extent[x] || pager.extent[x]->generation != handle >> HANDLE_BITS) return 0;
    *index = s % PAGER_EXTENT_SLOTS;
    return pager.extent[x];
}
static bool slot_lba(const struct pager_extent *x, uint32_t i, uint64_t *lba)
{
    uint32_t lo = 0, hi = x->map.runs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        const struct pager_run *r = &x->map.run[mid];
        if (i < r->first) hi = mid;
        else if (i - r->first >= r->count) lo = mid + 1;
        else { *lba = r->lba + (uint64_t)(i - r->first) * SECTORS_PER_PAGE; return true; }
    }
    return false;
}
bool pager_slot_readable(uint32_t handle)
{
    uint32_t i; struct pager_extent *x = slot_extent(handle, &i);
    return x && state_get(x, i) == ST_USED && pager.state != PAGER_FAILED;
}
bool pager_slot_commit(uint32_t handle)
{
    uint32_t i; struct pager_extent *x = slot_extent(handle, &i);
    if (!x) return false;
    uint32_t st = state_get(x, i);
    if (st == ST_RESERVED) { state_set(x, i, ST_USED); publish(); }
    return st == ST_RESERVED || st == ST_USED;
}
void pager_slot_free(uint32_t handle)
{
    uint32_t i; struct pager_extent *x = slot_extent(handle, &i);
    if (!x) return;
    uint32_t st = state_get(x, i);
    if (st == ST_RESERVED || st == ST_USED) { state_set(x, i, ST_FREE); publish(); }
}
static void slot_bad(uint32_t handle)
{
    uint32_t i; struct pager_extent *x = slot_extent(handle, &i);
    if (x && state_get(x, i) != ST_BAD) { state_set(x, i, ST_BAD); publish(); }
}
bool pager_online(void) { return pager.backend && pager.state == PAGER_ACTIVE; }
bool pager_can_reserve(void)
{ return pager.state == PAGER_ACTIVE && !pager.sticky && pager.counts[ST_FREE] > 0; }
bool pager_keep_copy(void)
{
    uint32_t floor = pager.capacity / 8 > 2 * PAGER_BATCH ? pager.capacity / 8 : 2 * PAGER_BATCH;
    return pager.counts[ST_FREE] > floor;
}
/* Up to want consecutive FREE slots in one contiguous LBA run, RESERVED for
 * the caller. Bounded scan; zero when none was found in this attempt. */
static uint32_t slot_reserve(uint32_t want, uint32_t *count)
{
    *count = 0;
    if (!pager_can_reserve() || !want) return 0;
    unsigned budget = SCAN_WORDS;
    for (uint32_t n = 0; n < pager.extents && budget; n++) {
        uint32_t xi = (pager.cursor + n) % pager.extents;
        struct pager_extent *x = pager.extent[xi];
        if (!x || !x->counts[ST_FREE]) continue;
        for (uint32_t w = 0; w < PAGER_EXTENT_SLOTS / 32 && budget; w++, budget--) {
            uint32_t word = (x->hint / 32 + w) % (PAGER_EXTENT_SLOTS / 32), bits = x->free_map[word];
            if (!bits) continue;
            uint32_t i = word * 32 + (uint32_t)__builtin_ctz(bits);
            uint64_t lba;
            if (!slot_lba(x, i, &lba)) { state_set(x, i, ST_BAD); continue; } /* defensive */
            uint32_t got = 1;
            for (uint64_t next; got < want && i + got < PAGER_EXTENT_SLOTS &&
                 state_get(x, i + got) == ST_FREE && slot_lba(x, i + got, &next) &&
                 next == lba + (uint64_t)got * SECTORS_PER_PAGE; got++) {}
            for (uint32_t k = 0; k < got; k++) state_set(x, i + k, ST_RESERVED);
            x->hint = (i + got) % PAGER_EXTENT_SLOTS; pager.cursor = xi;
            *count = got; publish();
            return (x->generation << HANDLE_BITS) | (xi * PAGER_EXTENT_SLOTS + i + 1);
        }
    }
    return 0;
}

/* ---- I/O ---- */

static int io_run(struct pager_io *io)
{
    if (!pager.backend) return -PAGER_EIO;
    int e; unsigned tries = 0;
    uint64_t begin = pclock();
    pager.inflight++;
    while ((e = pager.backend->submit(pager.ctx, io)) == -PAGER_EAGAIN && tries++ < RESERVE_WAITS)
        task_sleep_ms(1); /* the reserved record returns at its owner's completion */
    if (!e) {
        if (io->cls == PAGER_PREWRITE) {
            pager.speculative = io;
            if (pager.cancel_requested && pager.backend->cancel) pager.backend->cancel(pager.ctx, io);
        }
        e = pager.backend->wait(pager.ctx, io);
        if (pager.speculative == io) pager.speculative = 0;
    }
    pager.inflight--;
    waited += pclock() - begin;
    pager.idle_since = now_ms();
    return e;
}
void pager_demand(void)
{
    pager.last_demand = now_ms();
    /* Demand arrival cancels or deprioritises speculative writeback only;
     * mandatory page-out keeps its f1-83 aging and promotion. */
    if (pager.speculative && pager.backend && pager.backend->cancel) {
        if (pager.backend->cancel(pager.ctx, pager.speculative)) pager.stats.cancelled++;
    } else pager.cancel_requested = true;
}
int pager_page_in(uint32_t handle, void *buffer)
{
    uint64_t start = pclock(), before = waited;
    if (!pager_order_enter(PAGER_ORDER_SLOTS, SITE)) return -PAGER_EDEADLK;
    uint32_t i; uint64_t lba = 0;
    struct pager_extent *x = slot_extent(handle, &i);
    bool ok = x && buffer && state_get(x, i) == ST_USED && slot_lba(x, i, &lba) &&
              pager.backend && pager.state != PAGER_FAILED;
    pager_order_exit(PAGER_ORDER_SLOTS);
    int e = ok ? 0 : -PAGER_EIO;
    if (ok) {
        pager_demand();
        if (!pager_order_enter(PAGER_ORDER_IO, SITE)) return -PAGER_EDEADLK;
        struct pager_io io = { .buffer = buffer, .lba = lba, .bytes = PAGER_PAGE, .cls = PAGER_DEMAND };
        e = io_run(&io);
        pager_order_exit(PAGER_ORDER_IO);
    }
    uint32_t us = elapsed_us(start, before);
    pager.stats.in_us += us;
    if (us > pager.stats.max_in_us) pager.stats.max_in_us = us;
    if (e) {
        pager.stats.read_errors++;
        klog("[pager] page-in failure slot=%u result=%d state=%u", handle, e, (unsigned)pager.state);
        return e == -PAGER_EDEADLK ? e : -PAGER_EIO;
    }
    pager.stats.pageins++;
    pager.cancel_requested = false;
    return 0;
}

/* ---- page-out worker ---- */

static void complete(struct reclaim_io *io, bool ok)
{
    if (reclaim_writeback_complete(io, ok)) return;
    /* A fault held the object: retry on a later step, never drop the hold. */
    if (reclaim_writeback_live(io) && pager.retries < ARRAY_SIZE(pager.retry)) {
        pager.retry[pager.retries] = *io; pager.retry_ok[pager.retries++] = ok;
    }
}
static bool retry_completions(void)
{
    unsigned n = pager.retries; bool progress = false;
    pager.retries = 0;
    for (unsigned i = 0; i < n; i++) {
        struct reclaim_io io = pager.retry[i]; bool ok = pager.retry_ok[i];
        if (reclaim_writeback_complete(&io, ok)) progress = true;
        else if (reclaim_writeback_live(&io)) { pager.retry[pager.retries] = io; pager.retry_ok[pager.retries++] = ok; }
    }
    return progress;
}
static void write_group(struct reclaim_io *io, bool *ok, unsigned first, unsigned last, bool speculative)
{
    uint32_t i; uint64_t lba = 0;
    struct pager_extent *x = slot_extent(io[first].slot, &i);
    int e = x && slot_lba(x, i, &lba) ? 0 : -PAGER_EIO;
    if (!e) {
        if (!pager_order_enter(PAGER_ORDER_IO, SITE)) e = -PAGER_EDEADLK;
        else {
            struct pager_io r = { .buffer = pager.bounce + first * PAGER_PAGE, .lba = lba,
                .bytes = (last - first) * PAGER_PAGE, .cls = speculative ? PAGER_PREWRITE : PAGER_MANDATORY,
                .write = true };
            e = io_run(&r);
            pager_order_exit(PAGER_ORDER_IO);
        }
    }
    for (unsigned k = first; k < last; k++) {
        ok[k] = !e; io[k].written = !e;
        /* A failed write retires its slot; the resident dirty page survives.
         * A cancelled speculative write leaves the slot reusable. */
        if (e && e != -PAGER_ECANCELED && e != -PAGER_EDEADLK) slot_bad(io[k].slot);
    }
    if (!e) { pager.write_errors = 0; return; }
    if (e == -PAGER_ECANCELED) { pager.stats.cancelled += last - first; return; }
    pager.stats.write_errors += last - first;
    if (e != -PAGER_EDEADLK && ++pager.write_errors >= WRITE_ERROR_LIMIT && !pager.sticky) {
        pager.sticky = -PAGER_EIO; publish();
        klog("[pager] sticky write error result=%d capacity=%u used=%u", e, pager.capacity, pager.counts[ST_USED]);
    }
}
bool pager_pageout_step(void)
{
    if (!pager.backend || !pager.bounce || pager.out_busy) return false;
    bool progress = retry_completions();
    struct reclaim_io io[PAGER_BATCH];
    bool ok[PAGER_BATCH] = { 0 };
    unsigned n = 0;
    bool speculative = false;
    if (!pager_order_enter(PAGER_ORDER_PAGEOUT, SITE)) return progress;
    pager.out_busy = true;
    uint64_t start = pclock(), before = waited;
    /* Mandatory entries first; a batch never mixes speculative writes. */
    while (n < PAGER_BATCH && reclaim_writeback_take_class(&io[n], false)) n++;
    if (!n) {
        speculative = true;
        while (n < PAGER_BATCH && reclaim_writeback_take_class(&io[n], true)) n++;
    }
    if (!n) goto out;
    progress = true;
    bool admit = pager.state == PAGER_ACTIVE && !pager.sticky &&
                 (!pager.backend->writable || pager.backend->writable(pager.ctx));
    for (unsigned k = 0; k < n; k++) {
        io[k].slot = 0; io[k].written = false;
        if (!reclaim_writeback_prepare(&io[k], admit ? pager.bounce + k * PAGER_PAGE : 0))
            io[k].outcome = RECLAIM_DEFER;
    }
    if (pager_order_enter(PAGER_ORDER_SLOTS, SITE)) {
        for (unsigned k = 0; k < n;) {
            if (io[k].outcome != RECLAIM_WRITE || io[k].slot) { k++; continue; }
            unsigned m = k;
            while (m < n && io[m].outcome == RECLAIM_WRITE && !io[m].slot) m++;
            uint32_t got, h = slot_reserve(m - k, &got);
            if (!h) { for (; k < m; k++) io[k].outcome = RECLAIM_DEFER; continue; }
            for (uint32_t j = 0; j < got; j++) io[k + j].slot = h + j;
            k += got;
        }
        pager_order_exit(PAGER_ORDER_SLOTS);
    } else for (unsigned k = 0; k < n; k++) if (io[k].outcome == RECLAIM_WRITE && !io[k].slot) io[k].outcome = RECLAIM_DEFER;
    for (unsigned k = 0; k < n;) {
        if (io[k].outcome != RECLAIM_WRITE) {
            ok[k] = io[k].outcome == RECLAIM_CLEAN;
            if (io[k].outcome == RECLAIM_CLEAN) pager.stats.clean_discards += !io[k].prewrite;
            if (io[k].outcome == RECLAIM_DEFER) pager.stats.deferred++;
            k++; continue;
        }
        uint32_t i; uint64_t lba = 0, next;
        struct pager_extent *x = slot_extent(io[k].slot, &i);
        if (x) slot_lba(x, i, &lba);
        unsigned m = k + 1;
        while (m < n && io[m].outcome == RECLAIM_WRITE && (x = slot_extent(io[m].slot, &i)) &&
               slot_lba(x, i, &next) && next == lba + (uint64_t)(m - k) * SECTORS_PER_PAGE) m++;
        write_group(io, ok, k, m, speculative);
        k = m;
    }
    for (unsigned k = 0; k < n; k++) {
        bool was_out = !io[k].prewrite && io[k].outcome != RECLAIM_DEFER && io[k].outcome != RECLAIM_GONE && ok[k];
        if (io[k].prewrite && io[k].written) pager.stats.prewrites++;
        if (was_out && io[k].written) pager.stats.pageouts++;
        complete(&io[k], ok[k]);
    }
    {
        uint32_t us = elapsed_us(start, before);
        pager.stats.out_us += us;
        if (us > pager.stats.max_out_us) pager.stats.max_out_us = us;
    }
out:
    pager.out_busy = false;
    pager_order_exit(PAGER_ORDER_PAGEOUT);
    return progress;
}

/* ---- idle prewrite ---- */

bool pager_prewrite_step(uint64_t now)
{
    if (!pager_online() || pager.sticky || !pager.bounce) return false;
    if (now - pager.last_prewrite < PAGER_PREWRITE_GAP_MS) return false;
    if (pager.last_demand && now - pager.last_demand < PAGER_DEMAND_QUIET_MS) return false;
    struct mm_ledger v; ledger_snapshot(&v);
    /* Above the high watermark nothing approaches reclaim; under pressure
     * mandatory reclaim owns the queue. Keep a batch of slots for it. */
    if (v.pressure || v.zone[ZONE_NORMAL].free > v.high + v.high / 2) return false;
    if (pager.counts[ST_FREE] <= PAGER_BATCH) return false;
    if (pager.backend->idle && !pager.backend->idle(pager.ctx)) return false;
    pager.last_prewrite = now;
    pager.cancel_requested = false;
    uint64_t start = pclock(), before = waited;
    unsigned queued = reclaim_prewrite_slice(PAGER_PREWRITE_QUEUE);
    uint32_t us = elapsed_us(start, before);
    if (us > pager.stats.max_slice_us) pager.stats.max_slice_us = us;
    return queued != 0;
}

/* ---- extent manager (sleepable, PAGER_ORDER_MANAGER) ---- */

static uint64_t disk_reserve(uint64_t capacity)
{
    uint64_t percent = capacity / 100 * PAGER_RESERVE_PERCENT;
    return percent > PAGER_RESERVE_FLOOR ? percent : PAGER_RESERVE_FLOOR;
}
/* Validate a map and publish its slots: runs sorted, disjoint, in range;
 * every slot outside a run is failed capacity, never admitted. */
static bool map_valid(const struct pager_map *m)
{
    if (m->runs > PAGER_RUNS) return false;
    uint32_t end = 0;
    for (uint32_t r = 0; r < m->runs; r++) {
        const struct pager_run *run = &m->run[r];
        if (!run->count || run->first < end || run->first > PAGER_EXTENT_SLOTS ||
            run->count > PAGER_EXTENT_SLOTS - run->first) return false;
        end = run->first + run->count;
    }
    return true;
}
static int publish_extent(uint32_t index, const struct pager_map *map)
{
    if (index != pager.extents || index >= pager_max_extents() || !map_valid(map)) return -PAGER_EIO;
    uint32_t phys = pmm_alloc();
    if (!phys) return -PAGER_ENOMEM;
    mm_claim(phys, 1, MM_PAGER);
    struct pager_extent *x = P2V(phys);
    memset(x, 0, sizeof(*x));
    x->phys = phys; x->map = *map;
    pager.generation = (pager.generation + 1) & GENERATION_MASK;
    if (!pager.generation) pager.generation = 1;
    x->generation = pager.generation;
    for (uint32_t i = 0; i < PAGER_EXTENT_SLOTS / 16; i++) x->state[i] = 0xffffffffu; /* all BAD */
    x->counts[ST_BAD] = PAGER_EXTENT_SLOTS;
    pager.counts[ST_BAD] += PAGER_EXTENT_SLOTS;
    for (uint32_t r = 0; r < map->runs; r++)
        for (uint32_t i = map->run[r].first; i < map->run[r].first + map->run[r].count; i++) {
            x->state[i / 16] &= ~(3u << (i % 16 * 2));
            x->free_map[i / 32] |= 1u << (i % 32);
            x->counts[ST_BAD]--; x->counts[ST_FREE]++;
            pager.counts[ST_BAD]--; pager.counts[ST_FREE]++;
        }
    pager.extent[index] = x;
    pager.capacity += x->counts[ST_FREE];
    pager.extents++;
    publish();
    return 0;
}
static void retire_extent(uint32_t index)
{
    struct pager_extent *x = pager.extent[index];
    for (unsigned s = 0; s < 4; s++) pager.counts[s] -= x->counts[s];
    pager.capacity -= x->counts[ST_FREE];
    pager.extent[index] = 0;
    if (pager.cursor >= index) pager.cursor = 0;
    publish();
    uint32_t phys = x->phys;
    mm_free(phys, MM_PAGER);
}
static int grow_one(void)
{
    if (pager.extents >= pager_max_extents()) return -PAGER_ENOSPC;
    uint64_t free = 0, capacity = 0;
    int e = pager.backend->space ? pager.backend->space(pager.ctx, &free, &capacity) : -PAGER_EIO;
    if (!e && free < disk_reserve(capacity) + PAGER_EXTENT_BYTES) e = -PAGER_ENOSPC;
    struct pager_map map;
    if (!e) e = pager.backend->grow(pager.ctx, pager.extents, &map);
    if (!e) e = publish_extent(pager.extents, &map);
    if (e) {
        pager.stats.grow_failures++;
        klog("[pager] grow extents=%u result=%d free_mib=%u reserve_mib=%u",
             pager.extents, e, (unsigned)(free >> 20), (unsigned)(disk_reserve(capacity) >> 20));
        return e;
    }
    pager.stats.grows++;
    klog("[pager] grow extents=%u capacity=%u runs=%u free_mib=%u",
         pager.extents, pager.capacity, map.runs, (unsigned)((free - PAGER_EXTENT_BYTES) >> 20));
    return 0;
}
static bool open_store(void)
{
    uint32_t existing = 0;
    int e = pager.backend->open ? pager.backend->open(pager.ctx, &existing) : 0;
    if (e) {
        pager.opened = true; /* completed failure must not spin the manager */
        pager.state = PAGER_FAILED; publish();
        klog("[pager] attach result=%d swap=none commit=ram", e);
        return false;
    }
    if (existing > pager_max_extents()) {
        /* At boot no slot has an owner yet. Enforce a lowered manual maximum
         * on the physical backing file before mapping any retained extent. */
        e = pager.backend->shrink ? pager.backend->shrink(pager.ctx, pager_max_extents()) : -PAGER_EIO;
        if (e) {
            pager.opened = true; pager.state = PAGER_FAILED; publish();
            klog("[pager] manual maximum resize result=%d swap=none commit=ram", e);
            return false;
        }
        existing = pager_max_extents();
    }
    /* Existing extents (kept across boots) are mapped, never trusted:
     * every slot starts FREE, newly exposed anonymous pages are zeroed. */
    for (uint32_t i = 0; i < existing; i++) {
        struct pager_map map;
        e = pager.backend->grow(pager.ctx, i, &map);
        if (!e) e = publish_extent(i, &map);
        if (e) break;
    }
    pager.opened = true;
    pager.state = PAGER_ACTIVE;
    while (!e && pager.extents < pager_min_extents()) e = grow_one();
    if (!pager.extents) {
        pager.state = PAGER_FAILED; publish();
        klog("[pager] attach result=%d swap=none commit=ram", e ? e : -PAGER_ENOSPC);
        return false;
    }
    pager.idle_since = now_ms();
    publish();
    klog("[pager] attach extents=%u capacity=%u result=%d", pager.extents, pager.capacity, e);
    return true;
}
static bool shrink_one(uint64_t now)
{
    if (pager.extents <= pager_min_extents() || pager.want > pager.extents - 1) return false;
    uint32_t index = pager.extents - 1;
    struct pager_extent *x = pager.extent[index];
    /* Never relocate: only a trailing extent without live slots goes, and
     * never below the commitment it backs plus one extent of headroom. */
    if (x->counts[ST_USED] || x->counts[ST_RESERVED]) return false;
    int64_t headroom = ledger_commit_headroom ? ledger_commit_headroom() : 0;
    if (headroom - (int64_t)x->counts[ST_FREE] < (int64_t)PAGER_EXTENT_SLOTS) return false;
    pager.extents--;
    struct pager_map saved = x->map;
    retire_extent(index);
    int e = pager.backend->shrink ? pager.backend->shrink(pager.ctx, pager.extents) : 0;
    if (e) {
        pager.stats.shrink_failures++;
        /* The file still owns the clusters: republish the same extent. */
        if (publish_extent(index, &saved)) pager.grow_blocked = true;
    } else pager.stats.shrinks++;
    (void)now; /* consecutive trailing extents may follow on later steps */
    klog("[pager] shrink extents=%u capacity=%u result=%d", pager.extents, pager.capacity, e);
    return true;
}
bool pager_manager_step(uint64_t now)
{
    if (!pager.backend || !pager.boot_ready) return false;
    if (!pager_order_enter(PAGER_ORDER_MANAGER, SITE)) return false;
    bool progress = false;
    if (!pager.opened) { progress = open_store(); goto out; }
    if (pager.state != PAGER_ACTIVE || pager.sticky) goto out;
    int64_t headroom = ledger_commit_headroom ? ledger_commit_headroom() : 0;
    bool wanted = pager.want > pager.extents || headroom < (int64_t)PAGER_EXTENT_SLOTS;
    if (wanted && pager.grow_blocked && now - pager.grow_retry >= GROW_RETRY_MS) pager.grow_blocked = false;
    if (wanted && !pager.grow_blocked && pager.extents < pager_max_extents()) {
        /* Grow before admission needs the slots: one extent per step. */
        int e = grow_one();
        if (e) { pager.grow_blocked = true; pager.grow_retry = now; }
        progress = true;
        pager.idle_since = now;
    } else if (!wanted && now - pager.idle_since >= PAGER_SHRINK_IDLE_MS && !pager.inflight &&
               !reclaim_writeback_waiting()) progress = shrink_one(now);
out:
    pager_order_exit(PAGER_ORDER_MANAGER);
    return progress;
}
bool pager_commit_wait(uint32_t pages)
{
    if (!pager.backend || !pager.boot_ready || pager.sticky || (g_current && g_current->preempt_holds)) return false;
    if (!pager_order_check(PAGER_ORDER_MANAGER, SITE)) return false;
    uint64_t start = now_ms(), deadline = start + PAGER_COMMIT_WAIT_MS;
    /* The desktop never waits for this open. Later admission may need a
     * store whose manager has not run yet; opening shares the growth bound. */
    while (!pager.opened && pager.state == PAGER_OFF && now_ms() < deadline) {
        if (pager.workers) kwait_wake_all(&pager.manager);
        task_sleep_ms(10);
    }
    if (!pager_online() || now_ms() >= deadline) return false;
    int64_t headroom = ledger_commit_headroom ? ledger_commit_headroom() : 0;
    if (headroom >= (int64_t)pages) return true;
    uint64_t deficit = (uint64_t)((int64_t)pages - headroom);
    uint64_t want = pager.extents + (deficit + PAGER_EXTENT_SLOTS - 1) / PAGER_EXTENT_SLOTS;
    if (want > pager_max_extents()) want = pager_max_extents();
    /* A refused growth is retried only after GROW_RETRY_MS (no hammering). */
    if (want <= pager.extents || (pager.grow_blocked && start - pager.grow_retry < GROW_RETRY_MS)) return false;
    if (want > pager.want) pager.want = (uint32_t)want;
    pager.stats.commit_waits++;
    uint32_t before = pager.extents;
    while (pager.extents < want && pager_online() && now_ms() < deadline) {
        if (pager.grow_blocked && pager.grow_retry >= start) break; /* failed since we asked */
        if (pager.workers) kwait_wake_all(&pager.manager);
        task_sleep_ms(10);
    }
    if (pager.want <= pager.extents || pager.grow_blocked) pager.want = 0;
    if (pager.extents > before) pager.stats.commit_growths++;
    return pager.extents > before;
}

void pager_bootstrap_complete(void)
{
    if (pager.boot_ready) return;
    pager.boot_ready = true;
    if (pager.workers) kwait_wake_all(&pager.manager);
}

/* ---- lifecycle ---- */

void pager_snapshot(struct pager_stats *out)
{
    *out = pager.stats;
    out->state = pager.state; out->extents = pager.extents; out->capacity = pager.capacity;
    out->free = pager.counts[ST_FREE]; out->reserved = pager.counts[ST_RESERVED];
    out->used = pager.counts[ST_USED]; out->bad = pager.counts[ST_BAD];
    out->inflight = pager.inflight; out->sticky = pager.sticky; out->grow_blocked = pager.grow_blocked;
    out->generation = pager.generation;
}
static bool resources(void)
{
    if (pager.bounce) return true;
    /* Minimum I/O resources stay resident and pinned: one batch of bounce
     * pages for page-out snapshots (one f0-43 token, CPU and device use). */
    uint32_t phys = pmm_alloc_contig(PAGER_BATCH);
    if (!phys) return false;
    mm_claim(phys, PAGER_BATCH, MM_PAGER);
    pager.pin = (struct mm_pin){ .generation = 1, .first_pfn = phys / PAGE_SIZE, .pages = PAGER_BATCH,
        .address_mask = UINT32_MAX, .alignment = PAGE_SIZE, .direction = MM_PIN_BIDIRECTIONAL };
    if (mm_pin && mm_pin(&pager.pin)) {
        klog("[pager] bounce pin refused");
        pager.pin.ticket = 0;
    }
    pager.bounce_phys = phys;
    pager.bounce = P2V(phys);
    return true;
}
int pager_attach(const struct pager_backend *backend, void *ctx)
{
    if (!backend || !backend->grow || !backend->submit || !backend->wait) return -PAGER_EIO;
    if (pager.backend) return -PAGER_EBUSY;
    if (!resources()) return -PAGER_ENOMEM;
    pager.backend = backend; pager.ctx = ctx;
    pager.state = PAGER_OFF;
    if (pager.workers) kwait_wake_all(&pager.manager);
    return 0;
}
static void drain_queue(void)
{
    struct reclaim_io io;
    while (reclaim_writeback_take(&io)) { io.outcome = RECLAIM_DEFER; io.slot = 0; complete(&io, false); }
}
int pager_shutdown(void)
{
    if (!pager.backend || pager.state == PAGER_OFF) return 0;
    if (pager.state == PAGER_ACTIVE) pager.state = PAGER_QUIESCED;
    publish();
    uint64_t deadline = now_ms() + DRAIN_MS;
    while ((pager.inflight || pager.out_busy) && now_ms() < deadline) task_sleep_ms(1);
    if (pager.inflight || pager.out_busy) {
        if (pager.state == PAGER_QUIESCED) pager.state = PAGER_ACTIVE;
        publish();
        klog("[pager] shutdown live=%u inflight=%u result=%d", pager.counts[ST_USED], pager.inflight, -PAGER_EBUSY);
        return -PAGER_EBUSY;
    }
    drain_queue();
    int e = pager.backend->detach ? pager.backend->detach(pager.ctx) : 0;
    klog("[pager] shutdown live=%u inflight=0 result=%d", pager.counts[ST_USED], e);
    return e;
}
int pager_resume(void)
{
    if (!pager.backend || pager.state != PAGER_QUIESCED) return 0;
    if (!pager_order_enter(PAGER_ORDER_MANAGER, SITE)) return -PAGER_EDEADLK;
    int e = pager.backend->reattach ? pager.backend->reattach(pager.ctx) : 0;
    /* The reopened file must still own exactly the published runs. */
    for (uint32_t i = 0; !e && i < pager.extents; i++) {
        struct pager_map map;
        e = pager.backend->grow(pager.ctx, i, &map);
        if (!e && memcmp(&map, &pager.extent[i]->map, sizeof(map))) e = -PAGER_EIO;
    }
    pager.state = e ? PAGER_FAILED : PAGER_ACTIVE;
    publish();
    pager_order_exit(PAGER_ORDER_MANAGER);
    klog("[pager] resume extents=%u result=%d", pager.extents, e);
    return e;
}
void pager_media_lost(void)
{
    if (!pager.backend) return;
    pager.state = PAGER_FAILED; pager.sticky = -PAGER_EIO;
    publish();
    klog("[pager] media lost live=%u", pager.counts[ST_USED]);
}
bool pager_volume_live(void)
{ return pager.backend && (pager.counts[ST_USED] || pager.counts[ST_RESERVED] || pager.inflight); }
void pager_wake(void)
{
    if (pager.workers) kwait_wake_irq(&pager.work, false);
}
void pager_poll(uint64_t now)
{
    if (!pager.backend || (pager.last_record && now - pager.last_record < RECORD_MS)) return;
    pager.last_record = now ? now : 1;
    struct pager_stats s; pager_snapshot(&s);
    klog("[pager] stats state=%u extents=%u capacity=%u free=%u used=%u reserved=%u bad=%u out=%llu in=%llu pre=%llu clean=%llu err=%llu/%llu viol=%u max_us=%u/%u",
         s.state, s.extents, s.capacity, s.free, s.used, s.reserved, s.bad,
         (unsigned long long)s.pageouts, (unsigned long long)s.pageins, (unsigned long long)s.prewrites,
         (unsigned long long)s.clean_discards, (unsigned long long)s.write_errors,
         (unsigned long long)s.read_errors, s.order_violations, s.max_out_us, s.max_in_us);
}
#ifndef PAGER_HOST_MODEL
static bool work_ready(void *arg)
{ (void)arg; return reclaim_writeback_waiting() || pager.retries; }
static void pageout_worker(void *arg)
{
    (void)arg;
    for (;;) {
        kwait_wait_until(&pager.work, work_ready, 0, deadline_after_ms(PAGER_PREWRITE_GAP_MS));
        for (unsigned n = 0; n < 4 && pager_pageout_step(); n++) {}
        pager_prewrite_step(now_ms());
        kpreempt_point();
    }
}
static bool manager_ready(void *arg)
{ (void)arg; return pager.boot_ready && pager.backend && (!pager.opened || pager.want > pager.extents); }
static void manager_worker(void *arg)
{
    (void)arg;
    for (;;) {
        kwait_wait_until(&pager.manager, manager_ready, 0, deadline_after_ms(1000));
        pager_manager_step(now_ms());
    }
}
/* Pin the worker stack's PFNs through its kernel PTE vector (one token). */
static void pin_stack(struct task *t, struct mm_pin *pin)
{
    uint32_t va = (uint32_t)(uintptr_t)t->kstack, pages = KSTACK_SIZE / PAGE_SIZE;
    if (!mm_pin || ((va >> 22) != ((va + KSTACK_SIZE - 1) >> 22))) return;
    uint32_t *pd = P2V(vmm_kernel_pd()), table = pd[va >> 22] & ~4095u;
    *pin = (struct mm_pin){ .generation = 1, .first_pfn = vmm_kernel_pte(va) / PAGE_SIZE, .pages = pages,
        .address_mask = UINT32_MAX, .alignment = PAGE_SIZE, .direction = MM_PIN_CPU,
        .pfn_table = table, .first_entry = (va >> 12) & 1023u };
    if (mm_pin(pin)) { klog("[pager] stack pin refused task=%s", t->name); pin->ticket = 0; }
}
void pager_init(void)
{
    static struct mm_pin stack_pins[2];
    kwait_init(&pager.work); kwait_init(&pager.manager);
    /* Runtime diagnostic boots do not launch the normal desktop. */
    if (g_boot.flags & (CBI_F_TEST_REQUEST | CBI_F_HW_CHECK)) pager_bootstrap_complete();
    if (!resources()) { klog("[pager] resources unavailable"); return; }
    struct task *out = task_create_kernel("pager", pageout_worker, 0, P_NORMAL);
    struct task *man = out ? task_create_kernel("pagerman", manager_worker, 0, P_NORMAL) : 0;
    if (!out || !man) { klog("[pager] workers unavailable"); return; }
    pin_stack(out, &stack_pins[0]); pin_stack(man, &stack_pins[1]);
    pager.workers = true;
    task_start(out); task_start(man);
}
#endif
