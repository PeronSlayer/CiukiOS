/* Timing evidence for F0 (docs/design/device-firmware-ownership.md,
 * "Latency budgets"): TSC calibrated against the PIT, maximum
 * interrupt-disabled section length, timer gaps above 2 ms with a
 * classification (software critical section vs external stall).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/timing.h>
#include <ciuki/task.h>

uint64_t g_tsc_per_ms;               /* 0 until calibrated */
static uint64_t crit_start;
static uint64_t crit_max_cycles;     /* since last reset */
static uint64_t crit_window_max;     /* since the previous timer IRQ */
static uint32_t budget_violations;
static uint64_t last_timer_tsc;
static uint32_t gaps_software, gaps_unclassified;
static uint64_t pit_irqs;
static uint32_t crit_site, crit_max_site;
static const char *crit_section = "irq_save", *crit_max_section = "none";
static uint64_t pic_mask_out_max;
static uint32_t pic_mask_writes;
static struct timing_stall stalls[TIMING_STALL_CAPACITY];
static unsigned stall_count, stall_dropped;
static bool stall_active;
static uint64_t stall_begin, stall_per_ms;
static uint64_t stall_sample_begin, stall_sample_end;
struct timing_raw { uint64_t per_ms, maximum, pic_max; };
static void snapshot_locked(struct timing_stats *, struct timing_raw *);
static void snapshot_convert(struct timing_stats *, const struct timing_raw *);

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void crit_begin(void)
{
    if (g_cpu_tsc) {
        crit_start = rdtsc();
        crit_site = (uint32_t)(uintptr_t)__builtin_return_address(0);
        crit_section = "irq_save";
    }
}
void crit_label(const char *section) { if (crit_start) crit_section=section; }

void timing_pic_mask_write(uint64_t cycles)
{
    /* Caller owns the short PIC IRQ section. No division or logging here. */
    pic_mask_writes++;
    if (cycles > pic_mask_out_max) pic_mask_out_max = cycles;
}

void crit_end(void)
{
    if (!g_cpu_tsc || !crit_start)
        return;
    uint64_t end = rdtsc(), start = crit_start;
    uint64_t d = end - start;
    crit_start = 0;
    if (d > crit_max_cycles) {
        crit_max_cycles = d;
        crit_max_site = crit_site;
        crit_max_section = crit_section;
    }
    if (d > crit_window_max)
        crit_window_max = d;
    if (g_tsc_per_ms && d * 1000 > g_tsc_per_ms * CRIT_BUDGET_US)
        budget_violations++;
    if (stall_active && start >= stall_begin && stall_per_ms && d > stall_per_ms &&
        !(start <= stall_sample_begin && end >= stall_sample_end)) {
        if (stall_count < TIMING_STALL_CAPACITY)
            stalls[stall_count++] = (struct timing_stall){start,end,crit_site,crit_section};
        else if (stall_dropped != UINT32_MAX)
            stall_dropped++;
    }
}

void timing_stalls_begin(void)
{
    uint32_t f = irq_save();
    stall_active = false;
    stall_count = stall_dropped = 0;
    stall_sample_begin = stall_sample_end = 0;
    stall_per_ms = g_tsc_per_ms;
    stall_begin = g_cpu_tsc ? rdtsc() : 0;
    stall_active = g_cpu_tsc && stall_per_ms;
    irq_restore(f);
}

void timing_stall_observe(uint64_t start, uint64_t end, const char *section)
{
    /* IRQ budget probes call with IF clear; no allocation, I/O or logging. */
    if (!stall_active || start < stall_begin || !stall_per_ms || end - start <= stall_per_ms) return;
    /* The enclosing crit_end must not count this same stall a second time. */
    stall_sample_begin = start; stall_sample_end = end;
    if (stall_count < TIMING_STALL_CAPACITY)
        stalls[stall_count++] = (struct timing_stall){start, end, 0, section};
    else if (stall_dropped != UINT32_MAX) stall_dropped++;
}

bool timing_stall_get(unsigned index, struct timing_stall *out)
{
    uint32_t f = irq_save();
    bool ok = !stall_active && index < stall_count;
    if (ok) *out = stalls[index];
    irq_restore(f);
    return ok;
}

void timing_stalls_end(struct timing_stall_stats *out, struct timing_stats *timing)
{
    struct timing_raw raw;
    uint32_t f = irq_save();
    stall_active = false;
    *out = (struct timing_stall_stats){.start_tsc=stall_begin,
        .end_tsc=g_cpu_tsc ? rdtsc() : 0,.per_ms=stall_per_ms,
        .count=stall_count,.dropped=stall_dropped};
    if (timing) snapshot_locked(timing,&raw);
    irq_restore(f);
    if (timing) snapshot_convert(timing,&raw);
    /* Frozen records, all arithmetic with IF restored. End-to-end spacing
     * describes observed sections, not necessarily every physical SMI. */
    uint64_t total=0, minimum=UINT64_MAX, maximum=0, previous=0;
    for (unsigned i=0;i<out->count;i++) {
        struct timing_stall event;
        if (!timing_stall_get(i,&event)) break;
        if (i) {
            uint64_t delta=event.end_tsc-previous;
            total+=delta;
            if (delta<minimum) minimum=delta;
            if (delta>maximum) maximum=delta;
        }
        previous=event.end_tsc;
    }
    if (out->count>1 && out->per_ms) {
        out->period_min_us=minimum*1000/out->per_ms;
        out->period_max_us=maximum*1000/out->per_ms;
        out->period_mean_us=total*1000/out->per_ms/(out->count-1);
        out->jitter_us=(maximum-minimum)*1000/out->per_ms;
    }
}

void timing_timer_irq(void)
{
    pit_irqs++;
    if (!g_cpu_tsc)
        return;
    uint64_t now = rdtsc();
    if (last_timer_tsc && g_tsc_per_ms) {
        uint64_t gap = now - last_timer_tsc;
        if (gap > 2 * g_tsc_per_ms) {
            if (crit_window_max > 2 * g_tsc_per_ms)
                gaps_software++;
            else
                gaps_unclassified++;   /* no software cause measured: unresolved */
        }
    }
    last_timer_tsc = now;
    crit_window_max = 0;
}

void timing_calibrate(void)
{
    /* Only boot, before the first dispatch, may perform the PIT measurement.
     * Runtime calls must never wait or overwrite the calibrated frequency. */
    if (!g_cpu_tsc || g_tsc_per_ms || g_current || !(read_eflags() & 0x200))
        return;
    uint64_t t0 = g_ticks;
    while (g_ticks == t0)
        hlt();
    uint64_t a = rdtsc(), s = g_ticks;
    while (g_ticks < s + 100)
        hlt();
    uint64_t b = rdtsc();
    /* 100 ticks of 1193 PIT cycles at 1.193182 MHz */
    g_tsc_per_ms = (b - a) * 1193182ull / (100ull * 1193ull * 1000ull);
}

void timing_reset(void)
{
    uint32_t f = irq_save();
    crit_max_cycles = 0;
    crit_max_site = 0; crit_max_section = "none";
    crit_window_max = last_timer_tsc = 0;
    budget_violations = 0;
    gaps_software = gaps_unclassified = 0;
    pit_irqs = 0;
    pic_mask_out_max = 0; pic_mask_writes = 0;
    irq_restore(f);
}

static void snapshot_locked(struct timing_stats *s, struct timing_raw *raw)
{
    *raw=(struct timing_raw){g_tsc_per_ms,crit_max_cycles,pic_mask_out_max};
    s->pic_mask_writes = pic_mask_writes;
    s->budget_violations = budget_violations;
    s->gaps_software = gaps_software;
    s->gaps_unclassified = gaps_unclassified;
    s->pit_irqs = pit_irqs;
    s->critical_site = crit_max_site; s->critical_section = crit_max_section;
}

static void snapshot_convert(struct timing_stats *s, const struct timing_raw *r)
{
    s->tsc_khz = (uint32_t)r->per_ms;
    s->critical_us = r->per_ms ? (uint32_t)(r->maximum * 1000 / r->per_ms) : 0;
    s->critical_ns = r->per_ms ? (uint32_t)(r->maximum * 1000000 / r->per_ms) : 0;
    s->pic_mask_out_max_us = r->per_ms ? (uint32_t)(r->pic_max * 1000 / r->per_ms) : 0;
}

void timing_snapshot(struct timing_stats *s)
{
    struct timing_raw raw;
    uint32_t f=irq_save();
    snapshot_locked(s,&raw);
    irq_restore(f);
    snapshot_convert(s,&raw);
}
