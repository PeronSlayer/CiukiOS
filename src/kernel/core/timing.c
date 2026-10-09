/* Timing evidence for F0 (docs/design/device-firmware-ownership.md,
 * "Latency budgets"): TSC calibrated against the PIT, maximum
 * interrupt-disabled section length, timer gaps above 2 ms with a
 * classification (software critical section vs external stall).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/timing.h>

uint64_t g_tsc_per_ms;               /* 0 until calibrated */
static uint64_t crit_start;
static uint64_t crit_max_cycles;     /* since last reset */
static uint64_t crit_window_max;     /* since the previous timer IRQ */
static uint32_t budget_violations;
static uint64_t last_timer_tsc;
static uint32_t gaps_software, gaps_unclassified;
static uint64_t pit_irqs;

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void crit_begin(void)
{
    if (g_cpu_tsc)
        crit_start = rdtsc();
}

void crit_end(void)
{
    if (!g_cpu_tsc || !crit_start)
        return;
    uint64_t d = rdtsc() - crit_start;
    crit_start = 0;
    if (d > crit_max_cycles)
        crit_max_cycles = d;
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
    if (!g_cpu_tsc)
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
    budget_violations = 0;
    gaps_software = gaps_unclassified = 0;
    pit_irqs = 0;
    irq_restore(f);
}

void timing_snapshot(struct timing_stats *s)
{
    s->tsc_khz = (uint32_t)g_tsc_per_ms;
    s->critical_us = g_tsc_per_ms ? (uint32_t)(crit_max_cycles * 1000 / g_tsc_per_ms) : 0;
    s->critical_ns = g_tsc_per_ms ? (uint32_t)(crit_max_cycles * 1000000 / g_tsc_per_ms) : 0;
    s->budget_violations = budget_violations;
    s->gaps_software = gaps_software;
    s->gaps_unclassified = gaps_unclassified;
    s->pit_irqs = pit_irqs;
}
