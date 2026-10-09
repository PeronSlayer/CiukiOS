/* Tasks and scheduling (docs/design/execution-abi.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_TASK_H
#define CIUKI_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "mm.h"
#include "arch.h"

enum task_state { T_READY, T_RUNNING, T_BLOCKED, T_ZOMBIE, T_DEAD };
enum task_prio { P_DEVICE, P_INTERACTIVE, P_NORMAL, P_IDLE, P_COUNT };

#define EXIT_FAULT_BASE 0x100      /* exit code = 0x100 + vector for faults */
#define KSTACK_CANARY   0xC1A0CA7Au /* lowest word of every kernel stack */
unsigned task_canary_errors(void);
unsigned task_present_guards(void);
extern uint64_t g_task_switches_other;   /* user -> user switches not caused by the timer */
extern volatile bool g_measure_stop;     /* measured tasks are blocked once the target is reached */
extern uint32_t g_starvation_boosts;

struct task {
    uint32_t id;
    uint32_t generation;
    char name[16];
    enum task_state state;
    enum task_prio prio;
    bool user;
    bool measured;                 /* counted by the preemption measurement */
    bool boosted;                  /* starvation boost applied (once per episode) */
    void *kstack;                  /* lowest address of the 8 KiB stack */
    uint32_t saved_esp;
    struct aspace as;              /* user tasks only */
    uint8_t *fpu_area;             /* 16-byte aligned, 512 bytes */
    void *fpu_alloc;
    bool fpu_valid;
    int exit_code;
    uint32_t fault_vector, fault_err, fault_eip, fault_cr2;
    uint64_t wake_tick;
    /* scheduler statistics */
    uint32_t dispatches, preempt_dispatches;
    uint32_t debug_bytes;              /* bytes accepted by debug_write */
    struct { uint32_t nr; int32_t result; uint32_t side_effects; } sys_log[16];
    uint32_t sys_log_n;
    uint64_t ready_since, max_wait_ticks;
    struct task *next;             /* run queue / list link */
    struct task *all_next;
};

extern struct task *g_current;
extern uint32_t g_quantum_ticks;
extern uint32_t g_task_count;
extern uint64_t g_task_switches;   /* user task -> different user task, by preemption */
extern volatile bool g_need_resched;

void sched_init(void);
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio);
struct task *task_create_user(const char *name, enum task_prio prio, uint32_t entry, uint32_t esp,
                              uint32_t eax, uint32_t ebx, uint32_t ecx);
__attribute__((noreturn)) void sched_start(void);
extern uint64_t g_switch_target;
void task_start(struct task *t);
void schedule(void);
void sched_tick(void);
void task_yield(void);
void task_sleep_ms(uint32_t ms);
__attribute__((noreturn)) void task_exit(int code);
void task_kill(struct task *t, int code);
void task_reap(struct task *t);    /* frees a zombie */
bool task_alive(const struct task *t);

/* fpu */
void fpu_init(void);
void fpu_handle_nm(void);
void fpu_task_switch(struct task *next);
void fpu_task_release(struct task *t);
bool fpu_is_owner(const struct task *t);
extern uint32_t g_fpu_policy;      /* 1 = lazy, FXSR or FNSAVE */

/* syscalls */
void syscall_dispatch(struct trap_frame *tf);

#endif
