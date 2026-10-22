/* Timing evidence for F0 (docs/design/device-firmware-ownership.md,
 * "Latency budgets"): TSC calibrated against the PIT, maximum
 * interrupt-disabled section length, timer gaps above 2 ms with a
 * classification (software critical section vs external stall).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/timing.h>
#include <ciuki/task.h>
#include <ciuki/cpustat.h>
#include <ciuki/smi.h>

uint64_t g_tsc_per_ms;               /* 0 until calibrated */
static uint64_t crit_start, crit_firmware_start, firmware_cycles;
static uint64_t crit_work_max, firmware_max, firmware_window;
static uint64_t raw_max_work, raw_max_firmware;
static uint32_t firmware_gaps, unclassified_irqoff;
static char unclassified_task[16];
static uint32_t unclassified_task_id, unclassified_worker_site;
static char interrupt_owner[16], maximum_owner[16];
static char work_owner[16];
static uint32_t work_site;
static const char *work_section = "none";
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
struct timing_raw { uint64_t per_ms, maximum, pic_max, work_max, firmware_max, raw_work, raw_firmware; };
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
        crit_firmware_start = firmware_cycles;
        interrupt_owner[0] = 0;
        crit_site = (uint32_t)(uintptr_t)__builtin_return_address(0);
        crit_section = "irq_save";
        if (cpu_irqoff_begin) cpu_irqoff_begin(crit_start,crit_site);
    }
}
void crit_label(const char *section) { if (crit_start) crit_section=section; }
/* A scheduler entry can follow a raw CLI, without an irq_save owner.
 * Preserve a caller's outer section and tell schedule which one it owns. */
bool crit_begin_inactive(void)
{
    if (crit_start) return false;
    crit_begin();
    return crit_start!=0;
}

void crit_owner(const char *owner, uint32_t site)
{
    if (!crit_start) return;
    unsigned i = 0;
    for (; i < sizeof(interrupt_owner) - 1 && owner && owner[i]; i++) interrupt_owner[i] = owner[i];
    for (; i < sizeof(interrupt_owner); i++) interrupt_owner[i] = 0;
    crit_site = site;
}

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
    if (cpu_irqoff_end) cpu_irqoff_end(end);
    uint64_t d = end - start, firmware = firmware_cycles - crit_firmware_start;
    if (firmware > d) firmware = 0;
    uint64_t work = d - firmware;
    if (work > crit_work_max) {
        crit_work_max = work; work_site = crit_site; work_section = crit_section;
        memcpy(work_owner, interrupt_owner, sizeof(work_owner));
    }
    if (firmware > firmware_max) firmware_max = firmware;
    if (firmware > firmware_window) firmware_window = firmware;
    if (firmware) firmware_gaps++;
    if (g_tsc_per_ms && d * 1000 > g_tsc_per_ms * CRIT_BUDGET_US && !firmware) {
        unclassified_irqoff++;
        /* Ending context, including preemption inside a worker callback.
         * Copy bounded values now; no task pointer survives a task exit. */
        memset(unclassified_task,0,sizeof(unclassified_task));
        if (g_current) memcpy(unclassified_task,g_current->name,sizeof(unclassified_task)-1);
        unclassified_task_id=g_current ? g_current->id : 0;
        unclassified_worker_site=g_current ? g_current->device_worker_site : 0;
    }
    crit_start = 0;
    if (d > crit_max_cycles) {
        crit_max_cycles = d;
        raw_max_work = work; raw_max_firmware = firmware;
        crit_max_site = crit_site;
        crit_max_section = crit_section;
        memcpy(maximum_owner, interrupt_owner, sizeof(maximum_owner));
    }
    if (work > crit_window_max)
        crit_window_max = work;
    if (g_tsc_per_ms && work * 1000 > g_tsc_per_ms * CRIT_BUDGET_US)
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
    /* Attribution is independent of whether diagnostic tracing is active.
     * Only adjacent serialized no-work probes supply this heuristic tag. */
    if (section && !strncmp(section, "smi_attributed", sizeof("smi_attributed"))) {
        firmware_cycles += end - start;
        if (smi_adjacent_gap) smi_adjacent_gap(start, end); /* f1-94 TSC evidence */
    }
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
    /* f1-94: SMI_STS sample (ICH3-M only) and blackout/activity relation. */
    if (smi_tick) smi_tick(now, last_timer_tsc);
    if (last_timer_tsc && g_tsc_per_ms) {
        uint64_t gap = now - last_timer_tsc;
        if (gap > 2 * g_tsc_per_ms) {
            if (crit_window_max > 2 * g_tsc_per_ms)
                gaps_software++;
            else if (firmware_window <= 2 * g_tsc_per_ms)
                gaps_unclassified++;   /* no software cause measured: unresolved */
        }
    }
    last_timer_tsc = now;
    crit_window_max = firmware_window = 0;
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
    crit_max_cycles = crit_work_max = firmware_max = firmware_window = 0;
    raw_max_work = raw_max_firmware = 0;
    firmware_gaps = unclassified_irqoff = 0;
    memset(unclassified_task,0,sizeof(unclassified_task));
    unclassified_task_id=unclassified_worker_site=0;
    maximum_owner[0] = 0;
    work_owner[0] = 0; work_site = 0; work_section = "none";
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
    *raw=(struct timing_raw){g_tsc_per_ms,crit_max_cycles,pic_mask_out_max,crit_work_max,firmware_max,raw_max_work,raw_max_firmware};
    s->firmware_gaps = firmware_gaps; s->unclassified_irqoff = unclassified_irqoff;
    memcpy(s->unclassified_task,unclassified_task,sizeof(unclassified_task));
    s->unclassified_task_id=unclassified_task_id;
    s->unclassified_worker_site=unclassified_worker_site;
    memcpy(s->interrupt_owner, maximum_owner, sizeof(maximum_owner));
    s->work_site = work_site; s->work_section = work_section;
    memcpy(s->work_owner, work_owner, sizeof(work_owner));
    s->pic_mask_writes = pic_mask_writes;
    s->budget_violations = budget_violations;
    s->gaps_software = gaps_software;
    s->gaps_unclassified = gaps_unclassified;
    s->pit_irqs = pit_irqs;
    s->critical_site = crit_max_site; s->critical_section = crit_max_section;
}

static void snapshot_convert(struct timing_stats *s, const struct timing_raw *r)
{
    s->critical_work_us = r->per_ms ? (uint32_t)(r->work_max * 1000 / r->per_ms) : 0;
    s->raw_work_us = r->per_ms ? (uint32_t)(r->raw_work * 1000 / r->per_ms) : 0;
    s->raw_firmware_us = r->per_ms ? (uint32_t)(r->raw_firmware * 1000 / r->per_ms) : 0;
    s->firmware_us = r->per_ms ? (uint32_t)(r->firmware_max * 1000 / r->per_ms) : 0;
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
