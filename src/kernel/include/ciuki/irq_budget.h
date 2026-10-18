/* Shared IRQ timing policy; kernel-private, no user ABI.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_IRQ_BUDGET_H
#define CIUKI_IRQ_BUDGET_H
#include <stdint.h>
#include <stdbool.h>

#define IRQ_BUDGET_STRIKES 3u
#define IRQ_BUDGET_WINDOW_MS 100u
struct irq_budget { uint64_t strikes[IRQ_BUDGET_STRIKES]; unsigned next, count; };
struct irq_stamp { uint64_t tsc, stalls; };
struct irq_elapsed { uint64_t us, work_us, end_tsc; bool smi_attributed; };
/* Adjacent serialized reads observe no-work gaps only with IF clear.
 * Stamps nest: a NIC's observed stall also belongs to its enclosing chain. */
struct irq_stamp irq_budget_stamp(void);
struct irq_elapsed irq_budget_elapsed(struct irq_stamp);
bool irq_budget_fault(struct irq_budget *, struct irq_elapsed, unsigned limit_us);
#endif
