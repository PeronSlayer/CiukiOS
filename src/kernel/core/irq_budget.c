/* F0-39: conservative no-work stall attribution and repeated-overrun policy.
 * Sources and limits: device-firmware-ownership.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/timing.h>
#include <ciuki/irq_budget.h>

static uint64_t irq_stall_cycles;

struct irq_stamp irq_budget_stamp(void)
{
    if (!g_cpu_tsc || !g_tsc_per_ms) return (struct irq_stamp){0};
    uint64_t a = read_tsc_serialized(), b = read_tsc_serialized();
    uint64_t us = (b - a) * 1000 / g_tsc_per_ms;
    if (!(read_eflags() & 0x200) && us >= 2000 && us <= 2300) {
        irq_stall_cycles += b - a;
        if (timing_stall_observe) timing_stall_observe(a, b, "smi_attributed");
    }
    return (struct irq_stamp){b, irq_stall_cycles};
}

struct irq_elapsed irq_budget_elapsed(struct irq_stamp start)
{
    struct irq_stamp end = irq_budget_stamp();
    if (!g_cpu_tsc || !g_tsc_per_ms) return (struct irq_elapsed){0};
    uint64_t elapsed = end.tsc - start.tsc, stalls = end.stalls - start.stalls;
    if (stalls > elapsed) stalls = 0; /* a bad stamp cannot excuse work */
    return (struct irq_elapsed){elapsed * 1000 / g_tsc_per_ms,
        (elapsed - stalls) * 1000 / g_tsc_per_ms, end.tsc, stalls != 0};
}

bool irq_budget_fault(struct irq_budget *budget, struct irq_elapsed elapsed, unsigned limit_us)
{
    if (!g_tsc_per_ms || elapsed.work_us <= limit_us) return false;
    budget->strikes[budget->next] = elapsed.end_tsc;
    budget->next = (budget->next + 1) % IRQ_BUDGET_STRIKES;
    if (budget->count < IRQ_BUDGET_STRIKES) budget->count++;
    return budget->count == IRQ_BUDGET_STRIKES &&
        elapsed.end_tsc - budget->strikes[budget->next] <= g_tsc_per_ms * IRQ_BUDGET_WINDOW_MS;
}
