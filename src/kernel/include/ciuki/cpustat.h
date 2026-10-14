/* CPU share and voluntary-preemption evidence. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_CPUSTAT_H
#define CIUKI_CPUSTAT_H
#include <stddef.h>
#include <ciuki/task.h>
void cpu_init(void);
uint64_t cpu_path_clock(void) __attribute__((weak));
void cpu_trap_enter(const struct trap_frame *tf);
void cpu_user_return(void);
uint64_t cpu_irq_enter(const struct trap_frame *tf);
void cpu_irq_leave(uint64_t start);
void cpu_stall_record(const char *text, size_t bytes) __attribute__((weak));
enum cpu_path { CPU_CONSOLE, CPU_BOOTLOG, CPU_ELF, CPU_ZERO, CPU_PATHS };
void cpu_path_add(enum cpu_path path, uint64_t cycles) __attribute__((weak));
#endif
