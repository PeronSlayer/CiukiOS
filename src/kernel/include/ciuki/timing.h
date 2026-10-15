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
