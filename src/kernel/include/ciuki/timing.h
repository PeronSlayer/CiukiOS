/* Timing evidence (TSC, critical sections, timer gaps).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_TIMING_H
#define CIUKI_TIMING_H

#include <stdint.h>
#include <stdbool.h>

#define CRIT_BUDGET_US 250u

/* A ten-second window contains fewer than 10000 disjoint >1 ms sections.
 * Static diagnostic storage; no allocation, formatting or I/O in crit_end. */
#define TIMING_STALL_CAPACITY 10000u
struct timing_stall {
    uint64_t start_tsc, end_tsc;
    uint32_t site;
    const char *section;
};
struct timing_stall_stats {
    uint64_t start_tsc, end_tsc, per_ms;
    uint64_t period_min_us, period_max_us, period_mean_us, jitter_us;
    uint32_t count, dropped;
};
struct timing_stats;
void timing_stalls_begin(void);
/* Freeze counters with the trace, before period analysis can add latency.
 * The optional second output is the existing timing snapshot for this window. */
void timing_stalls_end(struct timing_stall_stats *, struct timing_stats *);
/* Frozen trace only; copies one record under a fixed-size IRQ section. */
bool timing_stall_get(unsigned index, struct timing_stall *);

struct timing_stats {
    uint32_t tsc_khz;
    uint32_t critical_us;
    uint32_t critical_ns;
    uint32_t budget_violations;
    uint32_t gaps_software, gaps_unclassified;
    uint64_t pit_irqs;
    uint32_t critical_site;
    const char *critical_section; /* static label, otherwise "irq_save" */
    uint32_t pic_mask_writes, pic_mask_out_max_us;
};

extern uint64_t g_tsc_per_ms;
void crit_begin(void);
void crit_end(void);
void crit_label(const char *section) __attribute__((weak));
void timing_timer_irq(void);
void timing_calibrate(void);
void timing_reset(void);
void timing_snapshot(struct timing_stats *s);
void timing_pic_mask_write(uint64_t cycles) __attribute__((weak));

#endif
