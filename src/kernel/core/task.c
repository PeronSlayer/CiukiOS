/* Tasks and the scheduler: fixed priorities, round-robin inside a priority,
 * preemption of ring-3 code on timer ticks; the kernel itself is
 * non-preemptible in F0 (docs/design/execution-abi.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/sync.h>

extern void switch_context(uint32_t *old_esp, uint32_t new_esp);
extern void task_first_entry(void);
extern void kthread_trampoline(void);

struct task *g_current;
uint32_t g_quantum_ticks = 10;
uint32_t g_task_count;
uint64_t g_task_switches;
uint64_t g_switch_target;          /* 0 = unlimited counting */
uint64_t g_task_switches_other;
volatile bool g_measure_stop;
uint32_t g_starvation_boosts;
volatile bool g_need_resched;

static struct task *rq_head[P_COUNT], *rq_tail[P_COUNT];
static struct task *all_tasks;
static struct task *idle_task;
static uint32_t next_id = 1;
static uint32_t slice_left;

static void rq_push(struct task *t)
{
    t->next = 0;
    t->ready_since = g_ticks;
    if (rq_tail[t->prio])
        rq_tail[t->prio]->next = t;
    else
        rq_head[t->prio] = t;
    rq_tail[t->prio] = t;
}

static struct task *rq_pop(void)
{
    for (int p = 0; p < P_COUNT; p++) {
        struct task *t = rq_head[p];
        if (t) {
            rq_head[p] = t->next;
            if (!rq_head[p])
                rq_tail[p] = 0;
            t->next = 0;
            return t;
        }
    }
    return 0;
}

static void rq_remove(struct task *t)
{
    struct task **pp = &rq_head[t->prio], *prev = 0;
    while (*pp) {
        if (*pp == t) {
            *pp = t->next;
            if (rq_tail[t->prio] == t)
                rq_tail[t->prio] = prev;
            t->next = 0;
            return;
        }
        prev = *pp;
        pp = &(*pp)->next;
    }
}

static bool rq_any(void)
{
    for (int p = 0; p < P_COUNT; p++)
        if (rq_head[p])
            return true;
    return false;
}

static struct task *task_alloc(const char *name, enum task_prio prio, bool user)
{
    struct task *t = kzalloc(sizeof(*t));
    if (!t)
        return 0;
    t->kstack = kstack_alloc();
    if (!t->kstack) {
        kfree(t);
        return 0;
    }
    t->fpu_alloc = kmalloc(512 + 16);
    if (!t->fpu_alloc) {
        kstack_free(t->kstack);
        kfree(t);
        return 0;
    }
    t->fpu_area = (uint8_t *)(((uintptr_t)t->fpu_alloc + 15) & ~(uintptr_t)15);
    *(volatile uint32_t *)t->kstack = KSTACK_CANARY;
    t->id = next_id++;
    t->generation = 1;
    t->prio = prio;
    t->user = user;
    t->state = T_BLOCKED;
    unsigned i = 0;
    for (; name[i] && i < sizeof(t->name) - 1; i++)
        t->name[i] = name[i];
    t->name[i] = 0;
    uint32_t f = irq_save();
    t->all_next = all_tasks;
    all_tasks = t;
    g_task_count++;
    irq_restore(f);
    return t;
}

/* Lay out a trap frame for the first iret plus a switch_context frame. */
static void prepare_stack(struct task *t, const struct trap_frame *tf, bool user)
{
    uint32_t top = (uint32_t)t->kstack + KSTACK_SIZE;
    uint32_t frame_size = user ? sizeof(struct trap_frame) : sizeof(struct trap_frame) - 8;
    uint32_t *sp = (uint32_t *)(top - frame_size);
    memcpy(sp, tf, frame_size);
    *--sp = (uint32_t)task_first_entry;   /* ret target of switch_context */
    *--sp = 0;                            /* ebp */
    *--sp = 0;                            /* ebx */
    *--sp = 0;                            /* esi */
    *--sp = 0;                            /* edi */
    t->saved_esp = (uint32_t)sp;
}

struct task *task_create_kernel(const char *name, void (*fn)(void *), void *arg, enum task_prio prio)
{
    struct task *t = task_alloc(name, prio, false);
    if (!t)
        return 0;
    struct trap_frame tf;
    memset(&tf, 0, sizeof(tf));
    tf.gs = tf.fs = tf.es = tf.ds = SEL_KDATA;
    tf.ebx = (uint32_t)fn;
    tf.esi = (uint32_t)arg;
    tf.eip = (uint32_t)kthread_trampoline;
    tf.cs = SEL_KCODE;
    tf.eflags = 0x202;
    prepare_stack(t, &tf, false);
    return t;
}

struct task *task_create_user(const char *name, enum task_prio prio, uint32_t entry, uint32_t esp,
                              uint32_t eax, uint32_t ebx, uint32_t ecx)
{
    struct task *t = task_alloc(name, prio, true);
    if (!t)
        return 0;
    if (as_create(&t->as) < 0) {
        t->state = T_ZOMBIE;
        task_reap(t);
        return 0;
    }
    struct trap_frame tf;
    memset(&tf, 0, sizeof(tf));
    tf.gs = tf.fs = tf.es = tf.ds = SEL_UDATA;
    tf.eax = eax;                         /* payload convention: EAX = payload id */
    tf.ebx = ebx;
    tf.ecx = ecx;
    tf.eip = entry;
    tf.cs = SEL_UCODE;
    tf.eflags = 0x202;
    tf.user_esp = esp;
    tf.user_ss = SEL_UDATA;
    prepare_stack(t, &tf, true);
    return t;
}

void task_start(struct task *t)
{
    uint32_t f = irq_save();
    t->state = T_READY;
    rq_push(t);
    irq_restore(f);
}

void schedule(void)
{
    uint32_t f = irq_save();
    struct task *prev = g_current;
    bool preempted = prev->state == T_RUNNING && g_need_resched;
    if (prev->state == T_RUNNING && prev != idle_task) {
        if (prev->measured && g_measure_stop) {
            prev->state = T_BLOCKED;      /* measurement finished: park it */
        } else {
            prev->state = T_READY;
            rq_push(prev);
        }
    }
    struct task *next = rq_pop();
    if (!next)
        next = idle_task;
    if (next->boosted) {
        next->boosted = false;            /* one boosted dispatch, then normal priority */
        next->prio = P_NORMAL;
    }
    g_need_resched = false;
    slice_left = g_quantum_ticks;
    if (next == prev) {
        prev->state = T_RUNNING;
        irq_restore(f);
        return;
    }
    if (next != idle_task) {
        uint64_t waited = g_ticks - next->ready_since;
        if (waited > next->max_wait_ticks)
            next->max_wait_ticks = waited;
    }
    next->dispatches++;
    if (prev->user && next->user && prev->measured && next->measured && !g_measure_stop) {
        if (!preempted) {
            g_task_switches_other++;
        } else {
            g_task_switches++;
            next->preempt_dispatches++;
            if (g_switch_target && g_task_switches == g_switch_target) {
                /* Exactly the target: this dispatch is the last counted
                 * switch. Park prev now; next is parked at its own
                 * preemption without being counted. */
                g_measure_stop = true;
                rq_remove(prev);
                prev->state = T_BLOCKED;
            }
        }
    }
    next->state = T_RUNNING;
    g_current = next;
    write_cr3(next->user ? next->as.pd_phys : vmm_kernel_pd());
    tss_set_kernel_stack((uint32_t)next->kstack + KSTACK_SIZE);
    fpu_task_switch(next);
    crit_end();                 /* a switch suspends the caller's critical section */
    switch_context(&prev->saved_esp, next->saved_esp);
    crit_begin();               /* time only the remainder after resuming */
    irq_restore(f);
}

void sched_tick(void)
{
    for (struct task *t = all_tasks; t; t = t->all_next) {
        if (t->state == T_BLOCKED && t->wake_tick && g_ticks >= t->wake_tick) {
            t->wake_tick = 0;
            t->state = T_READY;
            rq_push(t);
        }
        /* Starvation guard: a normal task ready for one second is boosted
         * once to the interactive queue (execution-abi.md). */
        if (t->state == T_READY && t->prio == P_NORMAL && !t->boosted &&
            g_ticks - t->ready_since >= 1000) {
            rq_remove(t);
            t->prio = P_DEVICE;           /* top priority: one guaranteed dispatch */
            t->boosted = true;
            rq_push(t);
            g_starvation_boosts++;
            g_need_resched = true;
        }
    }
    if (g_current == idle_task) {
        if (rq_any())
            g_need_resched = true;
    } else if (slice_left && --slice_left == 0) {
        g_need_resched = true;
    }
}

void task_yield(void)
{
    g_need_resched = false;
    schedule();
}

void task_sleep_ms(uint32_t ms)
{
    uint32_t f = irq_save();
    g_current->wake_tick = g_ticks + (ms ? ms : 1);
    g_current->state = T_BLOCKED;
    schedule();
    irq_restore(f);
}

void task_exit(int code)
{
    cli();
    struct task *t = g_current;
    t->exit_code = code;
    ksync_task_exit(t);                 /* held mutex here is a kernel bug */
    fpu_task_release(t);
    t->state = T_ZOMBIE;
    schedule();
    panic("task_exit: zombie %s was scheduled", t->name);
}

void task_kill(struct task *t, int code)
{
    if (t == g_current)
        task_exit(code);
    uint32_t f = irq_save();
    if (t->state == T_READY)
        rq_remove(t);
    if (t->state != T_ZOMBIE && t->state != T_DEAD) {
        t->exit_code = code;
        ksync_task_exit(t);
        t->state = T_ZOMBIE;
        fpu_task_release(t);
    }
    irq_restore(f);
}

bool task_alive(const struct task *t) { return t->state != T_ZOMBIE && t->state != T_DEAD; }

unsigned task_present_guards(void)
{
    unsigned n = 0;
    uint32_t f = irq_save();
    for (struct task *t = all_tasks; t; t = t->all_next)
        if (vmm_kernel_pte(kstack_guard_va(t->kstack)) & PTE_P)
            n++;
    irq_restore(f);
    return n;
}

unsigned task_canary_errors(void)
{
    unsigned n = 0;
    uint32_t f = irq_save();
    for (struct task *t = all_tasks; t; t = t->all_next)
        if (*(volatile uint32_t *)t->kstack != KSTACK_CANARY)
            n++;
    irq_restore(f);
    return n;
}

void task_reap(struct task *t)
{
    if (t == g_current || t->state != T_ZOMBIE)
        panic("task_reap: task %s is not a reapable zombie", t->name);
    uint32_t f = irq_save();
    struct task **pp = &all_tasks;
    while (*pp && *pp != t)
        pp = &(*pp)->all_next;
    if (*pp)
        *pp = t->all_next;
    g_task_count--;
    irq_restore(f);
    t->state = T_DEAD;
    if (t->user)
        as_destroy(&t->as);
    kstack_free(t->kstack);
    kfree(t->fpu_alloc);
    kfree(t);
}

static void idle_fn(void *arg)
{
    (void)arg;
    for (;;) {
        sti();
        hlt();
        if (g_need_resched || rq_any())
            schedule();
    }
}

void sched_init(void)
{
    idle_task = task_create_kernel("idle", idle_fn, 0, P_IDLE);
    if (!idle_task)
        panic("sched: cannot create the idle task");
}

/* Leave the boot stack for the first task; never returns. */
__attribute__((noreturn)) void sched_start(void)
{
    static uint32_t boot_esp;
    struct task *first = rq_pop();
    if (!first)
        first = idle_task;
    first->state = T_RUNNING;
    first->dispatches++;
    g_current = first;
    slice_left = g_quantum_ticks;
    tss_set_kernel_stack((uint32_t)first->kstack + KSTACK_SIZE);
    fpu_task_switch(first);
    switch_context(&boot_esp, first->saved_esp);
    panic("sched_start returned");
}
