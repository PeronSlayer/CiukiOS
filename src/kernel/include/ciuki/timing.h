/* Timing evidence (TSC, critical sections, timer gaps).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_TIMING_H
#define CIUKI_TIMING_H

#include <stdint.h>

#define CRIT_BUDGET_US 250u

struct timing_stats {
    uint32_t tsc_khz;
    uint32_t critical_us;
    uint32_t critical_ns;
    uint32_t budget_violations;
    uint32_t gaps_software, gaps_unclassified;
    uint64_t pit_irqs;
};

extern uint64_t g_tsc_per_ms;
void crit_begin(void);
void crit_end(void);
void timing_timer_irq(void);
void timing_calibrate(void);
void timing_reset(void);
void timing_snapshot(struct timing_stats *s);

#endif
