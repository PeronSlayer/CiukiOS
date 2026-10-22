/* Tasks and scheduling (docs/design/execution-abi.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_TASK_H
#define CIUKI_TASK_H

#include <stdint.h>
#include <stdbool.h>
#include "mm.h"
#include "arch.h"
#include "sync.h"

enum task_state { T_READY, T_RUNNING, T_BLOCKED, T_ZOMBIE, T_DEAD };
/* Stable class encodings; larger internal levels below run first. */
enum task_prio { P_DEVICE, P_INTERACTIVE, P_NORMAL, P_IDLE, P_HIGH, P_COUNT };
#define SCHED_LEVELS 32u
/* A priority level, distinct from the host's SCHED_NORMAL policy. */
#define CIUKI_SCHED_NORMAL 7u
#define SCHED_FOREGROUND 9u
#define SCHED_HIGH 13u
#define SCHED_DEVICE 24u
#define SCHED_SERVICE_MIN 16u
#define SCHED_BOOST_CAP 15u

#define EXIT_FAULT_BASE 0x100      /* exit code = 0x100 + vector for faults */
#define KSTACK_CANARY   0xC1A0CA7Au /* lowest word of every kernel stack */
#define KSTACK_FILL     0xA5u
struct task;
struct kwait;
struct sched_stats {
    uint32_t input_granted, input_expired, io_granted, io_expired;
    uint32_t starvation_granted, starvation_expired, budget_exhausted, audio_urgent;
    uint32_t wakeups;
};
void task_stack_init(void *base);  /* unused stack only, before its first frame */
/* Bytes touched since creation, excluding the reserved canary word. A damaged
 * canary reports KSTACK_SIZE. Scan only a live, unreaped task's stack; this is
 * a watermark of writes, not a bound on untouched local reservations. */
unsigned task_stack_high_water(const struct task *t);
unsigned task_canary_errors(void);
unsigned task_present_guards(void);
extern uint64_t g_task_switches_other;   /* user -> user switches not caused by the timer */
extern volatile bool g_measure_stop;     /* measured tasks are blocked once the target is reached */
extern uint32_t g_starvation_boosts;

struct task {
    uint32_t id;
    uint32_t generation;
    struct ktimer wake_timer, input_timer, ready_timer;
    char name[16];
    enum task_state state;
    enum task_prio prio;
    uint8_t base_priority, effective_priority, inherited_priority, configured_priority;
    uint8_t input_priority, io_priority, io_ticks;
    bool priority_valid, focused, input_forwarded;
    uint32_t device_worker_site; /* active deferred callback; zero outside it */
    uint64_t input_expiry, service_deadline;
    uint64_t audio_refill_expiry;
    uint32_t audio_refill_generation, audio_refill_bytes;
    struct kwait *waiting_on;      /* owner dependency; never physical device waits */
    struct kwaiter *sync_waiter;   /* one blocked wait, constant-time IRQ wake */
    bool user;
    bool measured;                 /* counted by the preemption measurement */
    bool boosted;                  /* starvation boost applied (once per episode) */
    void *kstack;                  /* lowest address of the KSTACK_SIZE stack */
    uint32_t saved_esp;
    struct aspace as;              /* user tasks only */
    uint8_t *fpu_area;             /* 16-byte aligned, 512 bytes */
    void *fpu_alloc;
    bool fpu_valid;
    bool win32;
    int exit_code;
    uint32_t fault_vector, fault_err, fault_eip, fault_cr2;
    uint64_t wake_tick;
    /* Voluntary scheduling and per-window CPU evidence (f0-21). */
    uint32_t preempt_holds;
    bool device_demoted;
    /* f1-90: audio refill class (own bounded budget, never demoted by other
     * tasks' device consumption) and the device-window consumer slot hint. */
    bool audio_class;
    uint8_t device_slot;
    /* f1-98: desktop minimum-share group (the desktop process and its window
     * server) and the reservation dependency of a polling waiter: the task
     * holding the drive reservation, writer turn or lock this task waits
     * for (sync.c ksync_depend; never a physical device wait). */
    bool share_group, share_lent; /* lent: a member waits for this task */
    uint8_t depend_reason;
    struct task *depends_on, *depend_next;
    const void *depend_object;
    uint64_t device_ticks;
    uint64_t device_cycles, device_base; /* shared-budget CPU, report windows */
    uint64_t run_cycles, window_base, kernel_ticks, user_ticks;
    uint64_t kernel_base, user_base, account_start, stretch_start, max_stretch;
    /* f2-96: firmware (SMM) time proven inside the open accounting interval;
     * the next charge leaves it out of the scheduler budgets (cpustat.c). */
    uint64_t firmware_credit;
    enum task_prio account_prio;
    uint32_t stretch_eip, active_stretch_eip;
    bool stretch_sampled;
    /* scheduler statistics */
    uint32_t dispatches, preempt_dispatches;
    uint32_t debug_bytes;              /* bytes accepted by debug_write */
    struct { uint32_t nr; int32_t result; uint32_t side_effects; } sys_log[16];
    uint32_t sys_log_n;
    uint64_t ready_since, max_wait_ticks;
    struct task *next, *rq_prev;    /* constant-time timer-driven reinsert */
    struct task *all_next;
    uint8_t pager_order;            /* f0-49 held lock-order levels (bitmask) */
};

extern struct task *g_current;
extern uint32_t g_quantum_ticks;
extern uint32_t g_task_count;
extern uint64_t g_task_switches;   /* user task -> different user task, by preemption */
extern uint64_t g_idle_ticks;      /* timer ticks taken while the idle task ran (f2-33) */
extern volatile bool g_need_resched;

void sched_init(void);
struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio);
struct task *task_create_user(const char *name, enum task_prio prio, uint32_t entry, uint32_t esp,
                              uint32_t eax, uint32_t ebx, uint32_t ecx);
__attribute__((noreturn)) void sched_start(void);
extern uint64_t g_switch_target;
void task_start(struct task *t);
void task_set_class(struct task *t, enum task_prio prio);
/* Kernel-only translation result; callers enforce elevation authority. */
void task_set_base(struct task *t, enum task_prio prio, unsigned level);
void task_set_inherited(struct task *t, unsigned priority);
void task_focus(struct task *t);  /* NULL withdraws foreground promotion */
void task_input_delivered(struct task *t);
/* Native focused endpoint -> its bound VM; no permanent focus/base change. */
void task_input_forward(struct task *source, struct task *target);
void task_input_withdraw(struct task *target);
void task_io_completed(struct task *t);
void task_device_deadline(struct task *t, uint64_t deadline);
/* f1-90: the audio DMA refill worker; its device CPU uses its own budget. */
void task_set_audio_class(struct task *t);
/* f1-98 (R2): members of the desktop group keep a minimum CPU share (25 %,
 * at most 2.5 ms banked) against any ready task below the device band that
 * would otherwise run first, such as a focused DOS VM or a Win32 game. */
void task_set_share_group(struct task *t, bool member);
/* sync.c: a member (or a task lent the share) waits for t through a kwait
 * owner or a reservation dependency, so t runs on the group's share. */
void task_set_share_lent(struct task *t, bool lent);
struct sched_share_stats {
    uint32_t turns;          /* dispatches made by the share preference */
    uint32_t lent_turns;     /* of which to a non-member lent the share */
    uint64_t turn_cycles;    /* CPU those turns used */
    uint64_t group_cycles;   /* CPU debited to the share (members, lent holders) */
};
void sched_share_snapshot(struct sched_share_stats *out);
void sched_snapshot(struct sched_stats *out);
/* cpustat charges actual execution at IRQ/switch boundaries. Tick accounting
 * remains a conservative fallback before a calibrated clock is available. */
void sched_charge_runtime(struct task *t, uint64_t cycles, uint64_t cycles_per_ms);
void cpu_account(void) __attribute__((weak));
void schedule(void);
void sched_tick(void);
void task_yield(void);
void kpreempt_point(void); /* IF=1, outside IRQ/unsafe publication sections */
static inline void kpreempt_hold(void) { if (g_current) g_current->preempt_holds++; }
static inline void kpreempt_release(void) { if (g_current) g_current->preempt_holds--; }
/* Only traverse with interrupts disabled; no pointer survives that section. */
struct task *sched_tasks(void);
/* Optional only for standalone scheduler fixtures. Linked in the kernel. */
void cpu_switch(struct task *prev, struct task *next) __attribute__((weak));
void cpu_reap(struct task *t) __attribute__((weak));
void cpu_point(void) __attribute__((weak));
/* consumed: the task's own device CPU in the 10 ms budget window. */
void cpu_overrun(struct task *t, uint64_t consumed) __attribute__((weak));
void cpu_audio_overrun(struct task *t, uint64_t consumed) __attribute__((weak));
/* f1-98: a reservation dependency raised `to` to `level` (reason: sync.h
 * KSYNC_DEPEND_*); called with interrupts disabled, aggregated per window. */
void cpu_inherit(const struct task *from, const struct task *to, unsigned level,
                 unsigned reason) __attribute__((weak));
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
