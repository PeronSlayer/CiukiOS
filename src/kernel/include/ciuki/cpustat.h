/* CPU share and voluntary-preemption evidence. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_CPUSTAT_H
#define CIUKI_CPUSTAT_H
#include <stddef.h>
#include <ciuki/task.h>
void cpu_init(void);
enum cpu_irqoff_phase { CPU_IRQOFF_NONE, CPU_IRQOFF_PICK, CPU_IRQOFF_FPU,
    CPU_IRQOFF_ADDRESS, CPU_IRQOFF_LDT_DEBUG, CPU_IRQOFF_VM_ENTER, CPU_IRQOFF_OTHER,
    /* f1-94: finer scheduler phases, the resumed caller, the IRQ path and
     * the PIT top half. Appended: the values above keep their record names. */
    CPU_IRQOFF_QUEUE, CPU_IRQOFF_PROC, CPU_IRQOFF_RESUME,
    CPU_IRQOFF_IRQ_ENTRY, CPU_IRQOFF_IRQ_HANDLER, CPU_IRQOFF_IRQ_EOI, CPU_IRQOFF_IRQ_EXIT,
    CPU_IRQOFF_TICK_TIMING, CPU_IRQOFF_TICK_SCHED, CPU_IRQOFF_TICK_TIMERS,
    CPU_IRQOFF_TICK_SERIAL, CPU_IRQOFF_PHASES };
/* Phases that mark straight-line code with a fixed small bound: a gap of a
 * millisecond between two such marks is not our work (f1-94 TSC rule). The
 * resumed caller and a generic device handler are not bounded here. */
static inline bool cpu_irqoff_bounded(enum cpu_irqoff_phase p)
{ return p != CPU_IRQOFF_NONE && p != CPU_IRQOFF_RESUME && p != CPU_IRQOFF_IRQ_HANDLER && p < CPU_IRQOFF_PHASES; }
/* IF-clear scalar accounting only; the cpustat task publishes the maximum
 * once per window. timing.c supplies its existing section boundary stamps. */
void cpu_irqoff_begin(uint64_t start,uint32_t site) __attribute__((weak));
void cpu_irqoff_end(uint64_t end) __attribute__((weak));
void cpu_irqoff_mark(enum cpu_irqoff_phase) __attribute__((weak));
void cpu_irqoff_tasks(const struct task *,const struct task *) __attribute__((weak));
uint64_t cpu_path_clock(void) __attribute__((weak));
/* f2-96: cumulative firmware (SMM) time proven so far, in TSC cycles; the
 * scheduler budgets and the audio service deadline leave it out. */
uint64_t cpu_firmware_cycles(void) __attribute__((weak));
void cpu_trap_enter(const struct trap_frame *tf);
void cpu_user_return(void);
uint64_t cpu_irq_enter(const struct trap_frame *tf);
void cpu_irq_leave(uint64_t start);
void cpu_stall_record(const char *text, size_t bytes) __attribute__((weak));
enum cpu_path { CPU_CONSOLE, CPU_BOOTLOG, CPU_ELF, CPU_ZERO, CPU_PATHS };
void cpu_path_add(enum cpu_path path, uint64_t cycles) __attribute__((weak));
/* Snapshot the same window maximum used by stretch_max_ms diagnostics.
 * Task name is copied under IRQ exclusion; never return a task pointer. */
void cpu_stretch_snapshot(char name[16], uint32_t *duration_ms) __attribute__((weak));
#endif
