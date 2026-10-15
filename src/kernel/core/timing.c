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
    uint64_t d = rdtsc() - crit_start;
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

void timing_snapshot(struct timing_stats *s)
{
    uint32_t f=irq_save();
    uint64_t per_ms=g_tsc_per_ms, maximum=crit_max_cycles;
    uint64_t pic_max=pic_mask_out_max;
    s->pic_mask_writes = pic_mask_writes;
    s->budget_violations = budget_violations;
    s->gaps_software = gaps_software;
    s->gaps_unclassified = gaps_unclassified;
    s->pit_irqs = pit_irqs;
    s->critical_site = crit_max_site; s->critical_section = crit_max_section;
    irq_restore(f);
    s->tsc_khz = (uint32_t)per_ms;
    s->critical_us = per_ms ? (uint32_t)(maximum * 1000 / per_ms) : 0;
    s->critical_ns = per_ms ? (uint32_t)(maximum * 1000000 / per_ms) : 0;
    s->pic_mask_out_max_us = per_ms ? (uint32_t)(pic_max * 1000 / per_ms) : 0;
}
