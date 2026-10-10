/* Sleeping services for the UP, non-preemptible kernel.
 * Research: Linux wait.h v6.12 (enqueue/check/block without a lost wake),
 * https://raw.githubusercontent.com/torvalds/linux/v6.12/include/linux/wait.h
 * and https://docs.kernel.org/locking/mutex-design.html (owner-only unlock,
 * no IRQ acquisition, no exit with a mutex held). Short atomic pacing:
 * https://docs.kernel.org/timers/delay_sleep_functions.html. Bounds here
 * are Ciuki policy; calibration remains timing.c's PIT/TSC measurement.
 * No Linux code is copied.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/sync.h>

struct kwaiter {
    struct task *task;
    struct kwait *queue;
    struct kwaiter *next, *all_next;
};

static struct kwaiter *waiters;
static struct kmutex *held;
static gen_t last_gen;

static void thread_context(void)
{
    if (!(read_eflags() & 0x200) || !g_current || g_current->state != T_RUNNING)
        panic("sync: blocking operation outside thread context");
}

uint64_t deadline_after_ms(uint32_t ms)
{
    uint32_t f = irq_save();
    uint64_t d = g_ticks + ms;
    irq_restore(f);
    return d;
}

bool deadline_passed(uint64_t d)
{
    uint32_t f = irq_save();
    bool passed = g_ticks - d < (1ull << 63);
    irq_restore(f);
    return passed;
}

uint64_t ktime_cycles(void)
{
    if (!g_cpu_tsc)
        return 0;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t us_cycles(uint32_t us)
{
    /* Round up, without multiplying the whole calibration by us. */
    return (g_tsc_per_ms / 1000) * us + ((g_tsc_per_ms % 1000) * us + 999) / 1000;
}

bool ktime_elapsed_us(uint64_t start, uint32_t us)
{
    return g_cpu_tsc && g_tsc_per_ms && ktime_cycles() - start >= us_cycles(us);
}

int udelay(uint32_t us)
{
    if (us > 1000 || (!(read_eflags() & 0x200) && us > 50))
        return -EINVAL;
    if (!us)
        return 0;
    if (!g_cpu_tsc || !g_tsc_per_ms)
        return -ENOSYS;
    uint64_t start = ktime_cycles(), cycles = us_cycles(us);
    /* A broken clock must not turn pacing into an infinite port-I/O wait. */
    for (unsigned n = 0; n < 1000000; n++)
        if (ktime_cycles() - start >= cycles)
            return 0;
    return -EFAULT;
}

gen_t gen_alloc(void)
{
    uint32_t f = irq_save();
    gen_t g = last_gen == UINT32_MAX ? 0 : ++last_gen;
    irq_restore(f);
    return g;
}

bool gen_matches(gen_t current, gen_t supplied)
{
    return supplied && current == supplied;
}

void kwait_init(struct kwait *q) { q->head = 0; }

static void wait_unlink(struct kwaiter *w)
{
    struct kwaiter **p = &w->queue->head;
    while (*p && *p != w)
        p = &(*p)->next;
    if (*p)
        *p = w->next;
    p = &waiters;
    while (*p && *p != w)
        p = &(*p)->all_next;
    if (*p)
        *p = w->all_next;
}

bool kwait_wait_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t d)
{
    thread_context();
    struct kwaiter w = { .task = g_current, .queue = q };
    uint32_t f = irq_save();
    w.next = q->head;
    q->head = &w;
    w.all_next = waiters;
    waiters = &w;
    bool ready;
    while (!(ready = cond(arg)) && !deadline_passed(d)) {
        w.task->wake_tick = d ? d : 1;
        w.task->state = T_BLOCKED;
        schedule();
    }
    w.task->wake_tick = 0;
    wait_unlink(&w);
    irq_restore(f);
    return ready;
}

void kwait_wake_all(struct kwait *q)
{
    uint32_t f = irq_save();
    for (struct kwaiter *w = q->head; w; w = w->next) {
        if (w->task->state == T_BLOCKED) {
            w->task->wake_tick = 0;
            /* task_start is the public wrapper around rq_push. Never put
             * an already READY task in the queue a second time. */
            task_start(w->task);
            g_need_resched = true;
        }
    }
    irq_restore(f);
}

void kmutex_init(struct kmutex *m)
{
    m->owner = 0;
    m->held_next = 0;
    kwait_init(&m->wait);
}

static bool mutex_free(void *arg) { return !((struct kmutex *)arg)->owner; }

void kmutex_lock(struct kmutex *m)
{
    thread_context();
    for (struct kmutex *p = held; p; p = p->held_next)
        if (p->owner == g_current && (uintptr_t)p >= (uintptr_t)m)
            panic("mutex: recursive acquisition or lock order");
    while (m->owner)
        kwait_wait_until(&m->wait, mutex_free, m, deadline_after_ms(1000));
    m->owner = g_current;
    m->held_next = held;
    held = m;
}

void kmutex_unlock(struct kmutex *m)
{
    thread_context();
    if (m->owner != g_current)
        panic("mutex: unlock by non-owner");
    struct kmutex **p = &held;
    while (*p != m) {
        if ((*p)->owner == g_current)
            panic("mutex: unlock order");
        p = &(*p)->held_next;
    }
    *p = m->held_next;
    m->held_next = 0;
    m->owner = 0;
    kwait_wake_all(&m->wait);
}

void ksync_task_exit(struct task *t)
{
    uint32_t f = irq_save();
    for (struct kmutex *m = held; m; m = m->held_next)
        if (m->owner == t)
            panic("mutex: owner %s exited", t->name);
    struct kwaiter *w = waiters;
    while (w) {
        struct kwaiter *next = w->all_next;
        if (w->task == t)
            wait_unlink(w);
        w = next;
    }
    t->wake_tick = 0;
    irq_restore(f);
}
