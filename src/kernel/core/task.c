/* Tasks and the scheduler: effective priorities, round-robin inside a priority,
 * preemption of ring-3 code on timer ticks; the kernel itself is
 * non-preemptible in F0 (docs/design/execution-abi.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/clock.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/cpustat.h>
#include <ciuki/sync.h>
#include <ciuki/work.h>
#include <ciuki/biosvm.h>
#include <ciuki/process.h>
/* Optional in the standalone F0/F1 host scheduler harnesses. */
extern uint32_t dosvm_task_cr3(const struct task *, uint32_t) __attribute__((weak));
extern uint32_t dosvm_task_esp0(const struct task *, uint32_t) __attribute__((weak));
extern void dosvm_task_switch(const struct task *) __attribute__((weak)); /* f3-04 LDT/watchpoints */
extern void dosvm_tick(void) __attribute__((weak));
extern void cpu_init(void);

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

static struct task *rq_head[SCHED_LEVELS], *rq_tail[SCHED_LEVELS];
extern uint32_t dosvm_task_quantum(const struct task *, uint32_t) __attribute__((weak));
extern const struct task *dosvm_task_focus(void) __attribute__((weak));
extern const struct task *dosvm_task_irq(void) __attribute__((weak));
static const struct task *focus_task;
static struct task *ui_focus;
static struct task *input_proxy;
static uint32_t focus_ticks, ordinary_ticks;
#define FOCUS_PERIOD 10u
#define FOCUS_BUDGET 8u
#define DEVICE_BUDGET 4u
static uint16_t device_history;
static unsigned device_used;
static uint64_t device_cpu[FOCUS_PERIOD], device_cpu_total, device_cpu_tick, device_cpu_scale = 1;
static unsigned device_cpu_slot;
static bool device_cpu_refresh;
static struct sched_stats scheduler_stats;
static struct task *all_tasks;
static struct task *idle_task;
static uint32_t next_id = 1;
static uint32_t slice_left;
static struct task *last_measured;
static bool last_preempted;
uint64_t g_idle_ticks;

static void rq_remove(struct task *t)
{
    unsigned p = t->effective_priority;
    if (t->rq_prev) t->rq_prev->next = t->next;
    else if (rq_head[p] == t) rq_head[p] = t->next;
    else return;
    if (t->next) t->next->rq_prev = t->rq_prev;
    else rq_tail[p] = t->rq_prev;
    t->next = t->rq_prev = 0;
}

static unsigned priority_base(struct task *t)
{
    static const uint8_t bases[P_COUNT] = { SCHED_DEVICE, SCHED_FOREGROUND, CIUKI_SCHED_NORMAL, 0, SCHED_HIGH };
    unsigned base = t->configured_priority ? t->configured_priority : bases[t->prio];
    if (t->focused && t->prio == P_NORMAL) base += 2;
    return base > SCHED_BOOST_CAP && t->prio != P_DEVICE ? SCHED_BOOST_CAP : base;
}

static void priority_init(struct task *t)
{
    if (t->priority_valid) return;
    t->priority_valid = true;
    t->base_priority = t->effective_priority = priority_base(t);
}

/* Reinsert without resetting ready_since: priority changes are not progress. */
static void rq_link(struct task *t)
{
    t->next = 0;
    t->rq_prev = rq_tail[t->effective_priority];
    if (rq_tail[t->effective_priority]) rq_tail[t->effective_priority]->next = t;
    else rq_head[t->effective_priority] = t;
    rq_tail[t->effective_priority] = t;
}
static bool device_urgent(const struct task *t);
static void device_cpu_advance(void)
{
    uint64_t elapsed = g_ticks - device_cpu_tick;
    if (elapsed >= FOCUS_PERIOD) {
        memset(device_cpu, 0, sizeof(device_cpu));
        device_cpu_total = 0; device_cpu_slot = 0;
    } else while (elapsed--) {
        if (++device_cpu_slot == FOCUS_PERIOD) device_cpu_slot = 0;
        device_cpu_total -= device_cpu[device_cpu_slot];
        device_cpu[device_cpu_slot] = 0;
    }
    device_cpu_tick = g_ticks;
}
static bool device_throttled(void)
{
    return device_used >= DEVICE_BUDGET || device_cpu_total >= DEVICE_BUDGET * device_cpu_scale;
}
void sched_charge_runtime(struct task *t, uint64_t cycles, uint64_t scale)
{
    if (!t || !cycles || !scale || t->effective_priority < SCHED_SERVICE_MIN) return;
    if (device_cpu_scale != scale) {
        memset(device_cpu, 0, sizeof(device_cpu));
        device_cpu_total = 0; device_cpu_scale = scale;
    }
    device_cpu_advance();
    device_cpu[device_cpu_slot] += cycles;
    device_cpu_total += cycles;
    if (device_throttled()) { device_cpu_refresh = true; g_need_resched = true; }
}

static void priority_update(struct task *t)
{
    priority_init(t);
    t->base_priority = priority_base(t);
    t->device_demoted = t->prio == P_DEVICE && device_throttled() && !device_urgent(t);
    if (t->input_priority) {
        unsigned input = t->base_priority + 2;
        t->input_priority = input > SCHED_BOOST_CAP ? SCHED_BOOST_CAP : input;
    }
    if (t->io_priority) {
        unsigned io = t->base_priority + 1;
        t->io_priority = io > SCHED_BOOST_CAP ? SCHED_BOOST_CAP : io;
    }
    unsigned p = t->device_demoted ? CIUKI_SCHED_NORMAL : t->base_priority;
    if (t->input_priority > p) p = t->input_priority;
    if (t->io_priority > p) p = t->io_priority;
    if (t->boosted && p < SCHED_BOOST_CAP) p = SCHED_BOOST_CAP;
    if (t->inherited_priority > p) p = t->inherited_priority;
    if (p == t->effective_priority) return;
    /* Close the old service interval before donation/boost withdrawal;
     * otherwise a short inherited turn could disappear at unlock. */
    if (t == g_current && cpu_account) cpu_account();
    if (t->state == T_READY) rq_remove(t);
    t->effective_priority = p;
    if (t->state == T_READY) rq_link(t);
    if (g_current && ((t->state == T_READY && p > g_current->effective_priority) || t == g_current))
        g_need_resched = true;
    if (ksync_priority_changed) ksync_priority_changed(t);
}

static void task_timer_event(void *, uint32_t, unsigned);
static void task_ready_timer(struct task *t)
{
    if (t->state == T_READY && t->prio == P_NORMAL && !t->boosted)
        ktimer_arm(&t->ready_timer, t->ready_since + 100, 2, t, &t->generation, task_timer_event);
    else ktimer_cancel(&t->ready_timer);
}
void task_set_class(struct task *t, enum task_prio prio)
{
    uint32_t f = irq_save();
    t->prio = prio;
    t->configured_priority = 0;
    priority_update(t); task_ready_timer(t);
    irq_restore(f);
}
void task_set_base(struct task *t, enum task_prio prio, unsigned level)
{
    uint32_t f = irq_save();
    t->prio = prio;
    t->configured_priority = level < SCHED_LEVELS ? level : SCHED_LEVELS - 1;
    priority_update(t); task_ready_timer(t);
    irq_restore(f);
}
void task_set_inherited(struct task *t, unsigned p)
{
    uint32_t f = irq_save();
    t->inherited_priority = p < SCHED_LEVELS ? p : SCHED_LEVELS - 1;
    priority_update(t);
    irq_restore(f);
}
static void task_timer_event(void *owner, uint32_t generation, unsigned kind)
{
    struct task *t = owner;
    if (t->generation != generation || t->state == T_ZOMBIE || t->state == T_DEAD) return;
    uint32_t f = irq_save();
    if (kind == 1 && t->input_priority) {
        t->input_priority = 0; t->input_expiry = 0;
        t->input_forwarded=false;
        if (input_proxy==t) input_proxy=0;
        scheduler_stats.input_expired++; priority_update(t);
    } else if (kind == 2 && t->state == T_READY && t->prio == P_NORMAL && !t->boosted) {
        t->boosted = true; priority_update(t);
        g_starvation_boosts++; scheduler_stats.starvation_granted++; g_need_resched = true;
    }
    irq_restore(f);
}
void task_focus(struct task *t)
{
    uint32_t f = irq_save();
    if (ui_focus != t) {
        if (input_proxy) task_input_withdraw(input_proxy);
        struct task *old = ui_focus;
        ui_focus = t;
        if (old) {
            old->focused = false;
            if (old->input_priority) scheduler_stats.input_expired++;
            old->input_priority = 0; old->input_expiry = 0;
            ktimer_cancel(&old->input_timer);
            priority_update(old);
        }
        if (t) { t->focused = true; priority_update(t); }
    }
    irq_restore(f);
}
void task_input_withdraw(struct task *target)
{
    uint32_t f=irq_save();
    if (target && target==input_proxy) {
        input_proxy=0; target->input_forwarded=false;
        if (target->input_priority) scheduler_stats.input_expired++;
        target->input_priority=0; target->input_expiry=0;
        ktimer_cancel(&target->input_timer); priority_update(target);
    }
    irq_restore(f);
}
void task_input_forward(struct task *source,struct task *target)
{
    uint32_t f=irq_save();
    if (source && source==ui_focus && target && target->state<T_ZOMBIE && target->prio==P_NORMAL) {
        if (input_proxy && input_proxy!=target) task_input_withdraw(input_proxy);
        input_proxy=target; target->input_forwarded=true;
        priority_init(target);
        unsigned p=target->base_priority+2;
        target->input_priority=p>SCHED_BOOST_CAP ? SCHED_BOOST_CAP : p;
        target->input_expiry=g_ticks+20;
        ktimer_arm(&target->input_timer,target->input_expiry,1,target,&target->generation,task_timer_event);
        scheduler_stats.input_granted++; priority_update(target);
    }
    irq_restore(f);
}
void task_input_delivered(struct task *t)
{
    uint32_t f = irq_save();
    /* The compositor owns shell/internal windows after the last client
     * leaves. Its owned input grant is the delivery boundary in that case. */
    if (t && !ui_focus && t->prio == P_INTERACTIVE) task_focus(t);
    if (t && t == ui_focus && t->prio != P_DEVICE && t->prio != P_IDLE) {
        priority_init(t);
        unsigned p = t->base_priority + 2;
        t->input_priority = p > SCHED_BOOST_CAP ? SCHED_BOOST_CAP : p;
        t->input_expiry = g_ticks + 20;
        ktimer_arm(&t->input_timer, t->input_expiry, 1, t, &t->generation, task_timer_event);
        scheduler_stats.input_granted++;
        priority_update(t);
    }
    irq_restore(f);
}
void task_io_completed(struct task *t)
{
    uint32_t f = irq_save();
    if (t && t->state == T_BLOCKED && t->prio != P_DEVICE && t->prio != P_IDLE) {
        priority_init(t);
        unsigned p = t->base_priority + 1;
        t->io_priority = p > SCHED_BOOST_CAP ? SCHED_BOOST_CAP : p;
        t->io_ticks = 10;
        scheduler_stats.io_granted++;
        priority_update(t);
    }
    irq_restore(f);
}
void task_device_deadline(struct task *t, uint64_t deadline)
{
    uint32_t f = irq_save(); t->service_deadline = deadline; irq_restore(f);
}
void sched_snapshot(struct sched_stats *out)
{
    uint32_t f = irq_save(); *out = scheduler_stats; irq_restore(f);
}
static void rq_push(struct task *t)
{
    priority_init(t);
    t->ready_since = g_ticks;
    if (t->prio == P_NORMAL && !t->boosted)
        ktimer_arm(&t->ready_timer, g_ticks + 100, 2, t, &t->generation, task_timer_event);
    rq_link(t);
}
static bool tick_reached(uint64_t deadline)
{ return g_ticks - deadline < (1ull << 63); }
static bool device_urgent(const struct task *t)
{
    return t->prio == P_DEVICE && t->service_deadline && tick_reached(t->service_deadline);
}
static struct task *rq_take(unsigned p)
{
    struct task *t = rq_head[p];
    ktimer_cancel(&t->ready_timer);
    rq_remove(t);
    return t;
}
static struct task *rq_pop(void)
{
    const struct task *focus = dosvm_task_irq ? dosvm_task_irq() : 0;
    if (!focus) focus = dosvm_task_focus ? dosvm_task_focus() : 0;
    focus_task = focus;
    /* Starvation dispatch and native input/inheritance take precedence over
     * DOS preference. Service budget is aggregate, including voluntary yields. */
    for (int p = SCHED_LEVELS - 1; p >= 0; p--) {
        for (struct task *t = rq_head[p]; t; t = t->next) if (device_urgent(t) || t->inherited_priority >= SCHED_SERVICE_MIN) {
            rq_remove(t);
            if (device_throttled()) scheduler_stats.audio_urgent++;
            return t;
        }
    }
    /* Reserve a bounded overdue dispatch before nonurgent device work;
     * actual service deadlines above retain precedence. */
    for (struct task *t = rq_head[SCHED_BOOST_CAP]; t; t = t->next)
        if (t->boosted) { rq_remove(t); return t; }
    for (int p = SCHED_LEVELS - 1; p >= (int)SCHED_BOOST_CAP; p--)
        if (rq_head[p]) return rq_take(p);
    if (focus && focus->state == T_READY && focus_ticks < FOCUS_BUDGET &&
        focus->effective_priority >= CIUKI_SCHED_NORMAL) {
        /* A foreground native input boost or donated lock owner beats a VM. */
        for (int p = SCHED_BOOST_CAP - 1; p > (int)focus->effective_priority; p--)
            if (rq_head[p] && (p > (int)SCHED_FOREGROUND || rq_head[p]->input_priority || rq_head[p]->inherited_priority)) return rq_take(p);
        struct task *t = (struct task *)focus; rq_remove(t); return t;
    }
    /* Forwarded input shares the VM's bounded preference. Repeated key
     * delivery must not promote a base-7 VM over the desktop indefinitely
     * after that preference has been spent. WAIT dependency donation retains
     * its normal precedence and withdrawal. */
    if (focus && focus->state==T_READY && focus_ticks>=FOCUS_BUDGET &&
        focus->input_forwarded && !focus->inherited_priority && !focus->boosted) {
        for (int p=SCHED_LEVELS-1;p>=(int)focus->base_priority;p--)
            for (struct task *t=rq_head[p];t;t=t->next) if (t!=focus) {
                ktimer_cancel(&t->ready_timer); rq_remove(t); return t;
            }
    }
    for (int p = SCHED_LEVELS - 1; p >= 0; p--) if (rq_head[p]) return rq_take(p);
    return 0;
}
static bool rq_any(void)
{
    for (unsigned p = 0; p < SCHED_LEVELS; p++) if (rq_head[p]) return true;
    return false;
}

void task_stack_init(void *base)
{
    memset(base, KSTACK_FILL, KSTACK_SIZE);
    *(volatile uint32_t *)base = KSTACK_CANARY;
}

unsigned task_stack_high_water(const struct task *t)
{
    if (!t || !t->kstack)
        return 0;                       /* boot/host tasks have no owned stack */
    const volatile uint8_t *bytes = t->kstack;
    if (*(const volatile uint32_t *)bytes != KSTACK_CANARY)
        return KSTACK_SIZE;
    unsigned untouched = sizeof(uint32_t);
    while (untouched < KSTACK_SIZE && bytes[untouched] == KSTACK_FILL)
        untouched++;
    return KSTACK_SIZE - untouched;
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
    task_stack_init(t->kstack);
    t->id = next_id++;
    t->generation = gen_alloc();
    if (!t->generation) {
        kstack_free(t->kstack); kfree(t->fpu_alloc); kfree(t); return 0;
    }
    t->prio = prio;
    priority_init(t);
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
    if (t->state != T_BLOCKED) { irq_restore(f); return; }
    ktimer_cancel(&t->wake_timer);
    t->wake_tick = 0;
    if (ksync_task_ready) ksync_task_ready(t);
    priority_update(t);
    scheduler_stats.wakeups++;
    t->state = T_READY;
    rq_push(t);
    if (g_current && t->effective_priority > g_current->effective_priority) g_need_resched = true;
    irq_restore(f);
}

struct task *task_create_native(struct process *p, uint32_t entry, uint32_t esp)
{
    struct task *t = task_alloc(p->name[0] ? p->name : "native", P_NORMAL, true);
    if (!t)
        return 0;
    task_set_base(t, P_NORMAL, p->personality == PROC_WIN32 ?
        (unsigned)proc_priority_win32(0x20, 0, false) : (unsigned)proc_priority_posix(0, 0, 0));
    t->as = p->memory->as;           /* borrowed directory, never destroyed by task */
    struct trap_frame tf;
    memset(&tf, 0, sizeof(tf));
    t->win32 = p->personality == PROC_WIN32;
    tf.gs = t->win32 ? SEL_UDATA : CIUKI_TLS_SELECTOR;
    tf.fs = t->win32 ? CIUKI_W32_FS_SELECTOR : SEL_UDATA;
    tf.es = tf.ds = SEL_UDATA;
    tf.eip = entry;
    tf.cs = SEL_UCODE;
    tf.eflags = CIUKI_INITIAL_EFLAGS;
    tf.user_esp = esp;
    tf.user_ss = SEL_UDATA;
    prepare_stack(t, &tf, true);
    return t;
}

void task_native_frame(struct task *t, uint32_t esp)
{
    struct trap_frame *tf = (struct trap_frame *)((uint8_t *)t->kstack + KSTACK_SIZE - sizeof(*tf));
    tf->user_esp = esp;
}

void schedule(void)
{
    uint32_t f = irq_save();
    bool own_irqoff=!(f&0x200) && crit_begin_inactive && crit_begin_inactive();
    if (own_irqoff && crit_label) crit_label("schedule");
    if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_PICK);
    /* Capture short turns that ended between PIT ticks before choosing the
     * next task. This also covers repeated voluntary device yields. */
    if (cpu_account) cpu_account();
    device_cpu_advance();
    if (device_cpu_refresh) {
        device_cpu_refresh = false;
        for (struct task *t = all_tasks; t; t = t->all_next) if (t->prio == P_DEVICE) {
            bool was_demoted = t->device_demoted;
            priority_update(t);
            if (!was_demoted && t->device_demoted) {
                scheduler_stats.budget_exhausted++;
                if (cpu_overrun) cpu_overrun(t);
            }
        }
    }
    struct task *prev = g_current;
    priority_init(prev);
    bool preempted = prev->state == T_RUNNING && g_need_resched;
    if (prev->boosted) {
        prev->boosted = false;
        scheduler_stats.starvation_expired++;
        priority_update(prev);
    }
    if (prev->state == T_BLOCKED) {
        prev->device_ticks = 0;
    }
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
    g_need_resched = false;
    slice_left = dosvm_task_quantum ? dosvm_task_quantum(next, g_quantum_ticks) : g_quantum_ticks;
    if (next->boosted || next->prio == P_DEVICE || next->effective_priority >= SCHED_SERVICE_MIN) {
        if (slice_left > 2) slice_left = 2;
    }
    if (next == focus_task && focus_ticks < FOCUS_BUDGET && slice_left > FOCUS_BUDGET - focus_ticks)
        slice_left = FOCUS_BUDGET - focus_ticks;
    if (focus_task && focus_task->state == T_READY && next != focus_task && next->prio != P_DEVICE && slice_left > 2)
        slice_left = 2;
    if (device_urgent(next)) {
        next->state = T_RUNNING; /* already removed from the ready queue */
        next->device_demoted = false;
        priority_update(next);
    }
    next->service_deadline = 0; /* a dispatch serves the annotated wake deadline */
    if (cpu_switch) cpu_switch(prev, next);
    if (cpu_irqoff_tasks) cpu_irqoff_tasks(prev,next);
    if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_OTHER);
    if (next == prev) {
        prev->state = T_RUNNING;
        if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_NONE);
        if (own_irqoff) crit_end();
        irq_restore(f);
        return;
    }
    if (next != idle_task) {
        uint64_t waited = g_ticks - next->ready_since;
        if (waited > next->max_wait_ticks)
            next->max_wait_ticks = waited;
    }
    next->dispatches++;
    if (prev->user) {
        last_measured = prev->measured ? prev : 0;
        last_preempted = preempted;
    }
    struct task *measured = last_measured;
    if (next->user) last_measured = 0;
    if (measured && measured != next && next->user && next->measured && !g_measure_stop) {
        if (!last_preempted) {
            g_task_switches_other++;
        } else {
            g_task_switches++;
            next->preempt_dispatches++;
            if (g_switch_target && g_task_switches == g_switch_target) {
                /* Exactly the target: this dispatch is the last counted
                 * switch. Park the preceding measured user now; next is
                 * parked at its own preemption without being counted. */
                g_measure_stop = true;
                if (measured->state == T_READY) {
                    rq_remove(measured);
                    measured->state = T_BLOCKED;
                }
            }
        }
    }
    next->state = T_RUNNING;
    g_current = next;
    if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_ADDRESS);
    /* F1 V86 worker: preserve its private directory and live C continuation.
     * Both hooks return the unmodified F0 values for all other tasks. */
    uint32_t cr3 = biosvm_task_cr3(next, next->user ? next->as.pd_phys : vmm_kernel_pd());
    uint32_t esp0 = biosvm_task_esp0(next, (uint32_t)next->kstack + KSTACK_SIZE);
    write_cr3(dosvm_task_cr3 ? dosvm_task_cr3(next, cr3) : cr3);
    if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_VM_ENTER);
    tss_set_kernel_stack(dosvm_task_esp0 ? dosvm_task_esp0(next, esp0) : esp0);
    if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_LDT_DEBUG);
    if (dosvm_task_switch) dosvm_task_switch(next);
    proc_task_switch(next);
    if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_FPU);
    fpu_task_switch(next);
    if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_NONE);
    crit_end();                 /* a switch suspends the caller's critical section */
    switch_context(&prev->saved_esp, next->saved_esp);
    crit_begin();               /* time only the remainder after resuming */
    if (own_irqoff) crit_end();
    irq_restore(f);
}

static void sched_vm_tick_work(void *arg)
{
    (void)arg;
    if (dosvm_tick) dosvm_tick(); /* computes elapsed from the current absolute tick */
}

void sched_tick(void)
{
    bool was_throttled = device_throttled();
    device_cpu_advance();
    bool charged = g_current && g_current->effective_priority >= SCHED_SERVICE_MIN;
    if (device_history & (1u << 9)) device_used--;
    device_history = ((device_history << 1) | charged) & 1023u;
    if (charged) device_used++;
    if (g_current && g_current != idle_task && !charged) {
        ordinary_ticks++;
        if (g_current == focus_task) focus_ticks++;
        if (ordinary_ticks == FOCUS_PERIOD) ordinary_ticks = focus_ticks = 0;
    }
    file_clock_tick();
    if (dosvm_tick) kwork_queue(sched_vm_tick_work, 0);
    ktimer_tick(g_ticks);
    /* Running-time decay concerns only the current task; deadline events
     * and ready-age promotion are dispatched by the ordered timer queue. */
    struct task *t = g_current;
    if (t && t->io_ticks && !--t->io_ticks) {
        t->io_priority = 0; scheduler_stats.io_expired++; priority_update(t);
    }
    if (t && t->boosted) {
        t->boosted = false; scheduler_stats.starvation_expired++; priority_update(t);
    }
    /* The existing dispatch refresh owns device reclassification. No task
     * population walk occurs in the timer top half. */
    if (t && t->prio == P_DEVICE) {
        bool demote = device_throttled() && !device_urgent(t);
        if (demote != t->device_demoted) {
            t->device_demoted = demote; priority_update(t);
            if (demote) { scheduler_stats.budget_exhausted++; if (cpu_overrun) cpu_overrun(t); }
        }
    }
    if (was_throttled != device_throttled()) {
        device_cpu_refresh = true; g_need_resched = true;
    }
    if (g_current == idle_task) {
        g_idle_ticks++;
        if (rq_any()) g_need_resched = true;
    } else if (slice_left && --slice_left == 0) g_need_resched = true;
    if (g_current == focus_task && focus_ticks >= FOCUS_BUDGET) g_need_resched = true;
}

struct task *sched_tasks(void) { return all_tasks; }

void kpreempt_point(void)
{
    if (!(read_eflags() & 0x200) || !g_current || g_current->preempt_holds)
        return;
    if (cpu_point) cpu_point();
    /* A virtual timer/SB IRQ is an earlier service deadline than another
     * same-class presentation tile, even before that tile's slice expires.
     * Never renew the focused VM's 8/10 budget or bypass device work. */
    const struct task *urgent = dosvm_task_irq ? dosvm_task_irq() : 0;
    if (urgent && urgent != g_current && urgent->state == T_READY &&
        urgent->effective_priority >= g_current->effective_priority &&
        (urgent != focus_task || focus_ticks < FOCUS_BUDGET))
        g_need_resched = true;
    if (g_need_resched) schedule();
}

void task_yield(void)
{
    g_need_resched = false;
    schedule();
}

void task_sleep_ms(uint32_t ms)
{
    uint32_t f = irq_save();
    ktimer_cancel(&g_current->ready_timer);
    ktimer_wait_task(g_current, g_ticks + (ms ? ms : 1));
    g_current->state = T_BLOCKED;
    irq_restore(f);
    schedule();
}

void task_exit(int code)
{
    cli();
    struct task *t = g_current;
    bool native = proc_task_exiting(t, code);
    t->exit_code = code;
    task_input_withdraw(t);
    if (ui_focus == t) task_focus(0);
    ksync_task_exit(t);
    if (!native)
        fpu_task_release(t);
    t->state = T_ZOMBIE;
    schedule();
    panic("task_exit: zombie %s was scheduled", t->name);
}

void task_kill(struct task *t, int code)
{
    if (t == g_current)
        task_exit(code);
    struct proc_thread *native_thread = proc_thread_for(t);
    if (native_thread && native_thread->in_syscall && task_alive(t)) {
        proc_stop(native_thread->process, code, 0);
        return;
    }
    uint32_t f = irq_save();
    if (t->state != T_ZOMBIE && t->state != T_DEAD) {
        bool native = proc_task_exiting(t, code);
        t->exit_code = code;
        task_input_withdraw(t);
        if (ui_focus == t) task_focus(0);
        ksync_task_exit(t);
        if (t->state == T_READY) rq_remove(t);
        t->state = T_ZOMBIE;
        if (!native)
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
    ktimer_cancel(&t->wake_timer);
    ktimer_cancel(&t->input_timer);
    ktimer_cancel(&t->ready_timer);
    g_task_count--;
    if (last_measured == t) last_measured = 0;
    if (cpu_reap) cpu_reap(t);
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
    proc_init();
    serial_writer_init();
    console_worker_init();
    cpu_init();
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
    if (cpu_switch) cpu_switch(0, first);
    slice_left = g_quantum_ticks;
    tss_set_kernel_stack((uint32_t)first->kstack + KSTACK_SIZE);
    proc_task_switch(first);
    fpu_task_switch(first);
    switch_context(&boot_esp, first->saved_esp);
    panic("sched_start returned");
}
