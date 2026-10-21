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
#include <ciuki/work.h>

struct kwaiter {
    struct task *task;
    struct kwait *queue;
    struct kwaiter *next, *all_next;
    kwait_cond_fn cond;
    void *arg;
    bool donating;
    uint32_t *ticket;
};

static struct kwaiter *waiters;
static struct kmutex *held;
static struct kwait *owned_queues;
static gen_t last_gen;
static bool propagating;

/* Every live outgoing dependency is a wait on an owned lock/request queue.
 * Device waits have owner==NULL. Suppress the edge that would close a cycle. */
static struct task *wait_owner(struct task *t)
{ return t && t->waiting_on ? t->waiting_on->owner : 0; }
static bool donation_cycle(struct task *waiter, struct task *owner)
{
    struct task *slow = owner, *fast = owner;
    while (slow) {
        if (slow == waiter) return true;
        slow = wait_owner(slow);
        fast = wait_owner(wait_owner(fast));
        if (slow && slow == fast) return true;
    }
    return false;
}
static void donation_update(struct task *t)
{
    if (propagating) return;
    propagating = true;
    while (t) {
        unsigned highest = 0;
        for (struct kwaiter *w = waiters; w; w = w->all_next)
            if (w->donating && w->queue->owner == t &&
                w->task->effective_priority > highest) highest = w->task->effective_priority;
        unsigned old = t->effective_priority;
        task_set_inherited(t, highest);
        if (old == t->effective_priority) break;
        t = wait_owner(t);
    }
    propagating = false;
}
void ksync_priority_changed(struct task *t) { donation_update(wait_owner(t)); }
void ksync_task_ready(struct task *t)
{
    struct task *owner = wait_owner(t);
    if (t->sync_waiter) t->sync_waiter->donating = false;
    t->waiting_on = 0;
    donation_update(owner);
}
void kwait_set_owner(struct kwait *q, struct task *owner)
{
    uint32_t f = irq_save();
    struct task *old = q->owner;
    if (!old && owner) { q->owned_next = owned_queues; owned_queues = q; }
    if (old && !owner) {
        struct kwait **link = &owned_queues;
        while (*link && *link != q) link = &(*link)->owned_next;
        if (*link) *link = q->owned_next;
        q->owned_next = 0;
    }
    q->owner = owner;
    for (struct kwaiter *w = q->head; w; w = w->next) {
        bool cycle = owner && w->task->state == T_BLOCKED && donation_cycle(w->task, owner);
        if (cycle) klog("[sched] donation_cycle waiter=%u owner=%u", w->task->id, owner->id);
        w->donating = owner && w->task->state == T_BLOCKED && !cycle;
        w->task->waiting_on = w->donating ? q : 0;
    }
    donation_update(old); donation_update(owner);
    irq_restore(f);
}


/* F0-46 timers: AVL ordering, intrusive due FIFO, no allocation. */
static struct ktimer *timer_root, *timer_first, *timer_due_head, *timer_due_tail;
static struct ktimer_stats timer_stats;

static unsigned timer_height(struct ktimer *t) { return t ? t->height : 0; }
static void timer_height_update(struct ktimer *t)
{
    unsigned a = timer_height(t->left), b = timer_height(t->right);
    t->height = 1 + (a > b ? a : b);
}
static void timer_replace(struct ktimer *a, struct ktimer *b)
{
    if (!a->parent) timer_root = b;
    else if (a->parent->left == a) a->parent->left = b;
    else a->parent->right = b;
    if (b) b->parent = a->parent;
}
static struct ktimer *timer_rotate(struct ktimer *t, bool left)
{
    struct ktimer *r = left ? t->right : t->left;
    timer_replace(t, r);
    if (left) {
        t->right = r->left;
        if (t->right) t->right->parent = t;
        r->left = t;
    } else {
        t->left = r->right;
        if (t->left) t->left->parent = t;
        r->right = t;
    }
    t->parent = r;
    timer_height_update(t); timer_height_update(r);
    return r;
}
static void timer_balance(struct ktimer *t)
{
    while (t) {
        timer_height_update(t);
        int balance = (int)timer_height(t->left) - (int)timer_height(t->right);
        if (balance > 1) {
            if (timer_height(t->left->right) > timer_height(t->left->left))
                timer_rotate(t->left, true);
            t = timer_rotate(t, false);
        } else if (balance < -1) {
            if (timer_height(t->right->left) > timer_height(t->right->right))
                timer_rotate(t->right, false);
            t = timer_rotate(t, true);
        }
        t = t->parent;
    }
}
static void timer_remove(struct ktimer *t)
{
    struct ktimer *from = t->parent;
    if (!t->left || !t->right) timer_replace(t, t->left ? t->left : t->right);
    else {
        struct ktimer *s = t->right;
        while (s->left) s = s->left;
        if (s->parent != t) {
            from = s->parent;
            timer_replace(s, s->right);
            s->right = t->right; s->right->parent = s;
        } else from = s;
        timer_replace(t, s);
        s->left = t->left; s->left->parent = s;
        timer_height_update(s);
    }
    if (t->prev) t->prev->next = t->next;
    else timer_first = t->next;
    if (t->next) t->next->prev = t->prev;
    t->left = t->right = t->parent = t->prev = t->next = 0;
    timer_stats.armed--;
    timer_balance(from);
}
static void timer_due_remove(struct ktimer *t)
{
    if (t->prev) t->prev->next = t->next;
    else timer_due_head = t->next;
    if (t->next) t->next->prev = t->prev;
    else timer_due_tail = t->prev;
    t->prev = t->next = 0;
    timer_stats.due--;
}
bool ktimer_cancel(struct ktimer *t)
{
    uint32_t f = irq_save();
    if (t->state == KTIMER_ARMED) timer_remove(t);
    else if (t->state == KTIMER_DUE) timer_due_remove(t);
    if (t->state != KTIMER_IDLE && t->state != KTIMER_CANCELLED) timer_stats.cancelled++;
    t->revision++; t->state = KTIMER_CANCELLED;
    bool quiet = !t->executing;
    irq_restore(f);
    return quiet;
}
void ktimer_arm(struct ktimer *t, uint64_t deadline, unsigned kind, void *owner,
                const uint32_t *generation, ktimer_fn callback)
{
    uint32_t f = irq_save();
    ktimer_cancel(t);
    t->deadline = deadline; t->kind = kind; t->owner = owner;
    t->owner_generation = generation; t->generation = generation ? *generation : 0;
    t->callback = callback; t->height = 1;
    struct ktimer **link = &timer_root, *parent = 0, *prev = 0, *next = 0;
    while (*link) {
        parent = *link; timer_stats.comparisons++;
        if (deadline - parent->deadline >= (1ull << 63)) {
            next = parent; link = &parent->left;
        } else { prev = parent; link = &parent->right; }
    }
    *link = t; t->parent = parent; t->prev = prev; t->next = next;
    if (prev) prev->next = t; else timer_first = t;
    if (next) next->prev = t;
    t->state = KTIMER_ARMED; timer_stats.armed++;
    timer_balance(parent);
    irq_restore(f);
}
static void timer_work(void *arg);
void ktimer_tick(uint64_t now)
{
    uint32_t f = irq_save();
    unsigned moved = 0;
    while (moved < KTIMER_IRQ_BATCH && timer_first && now - timer_first->deadline < (1ull << 63)) {
        struct ktimer *t = timer_first;
        timer_remove(t); t->state = KTIMER_DUE;
        t->prev = timer_due_tail;
        if (timer_due_tail) timer_due_tail->next = t;
        else timer_due_head = t;
        timer_due_tail = t; timer_stats.due++; timer_stats.transferred++; moved++;
    }
    if (moved > timer_stats.max_irq_batch) timer_stats.max_irq_batch = moved;
    if (timer_due_head) kwork_queue(timer_work, 0); /* full ring: next tick retries */
    irq_restore(f);
}
bool ktimer_run_one(void)
{
    if (!(read_eflags() & 0x200)) return false;
    uint32_t f = irq_save();
    struct ktimer *t = timer_due_head;
    if (!t) { irq_restore(f); return false; }
    timer_due_remove(t); t->state = KTIMER_IN_FLIGHT; t->executing = true;
    uint64_t revision = t->revision;
    bool live = t->owner_generation && t->generation && *t->owner_generation == t->generation;
    ktimer_fn callback = t->callback;
    irq_restore(f);
    /* No task switch between validation and completion: callbacks cannot
     * sleep/yield. IRQ cancellation may mark this entry but cannot free it. */
    if (live && callback) callback(t->owner, t->generation, t->kind);
    f = irq_save();
    if (live) timer_stats.completed++; else timer_stats.stale++;
    t->executing = false;
    if (t->revision == revision && t->state == KTIMER_IN_FLIGHT) t->state = KTIMER_IDLE;
    irq_restore(f);
    return true;
}
static void timer_work(void *arg)
{
    (void)arg;
    ktimer_run_one();
    ktimer_tick(deadline_after_ms(0)); /* continuation, never drain a notification chain */
}
void ktimer_snapshot(struct ktimer_stats *out)
{
    uint32_t f = irq_save(); *out = timer_stats; irq_restore(f);
}
static void timer_task_wake(void *owner, uint32_t generation, unsigned kind)
{
    (void)kind;
    struct task *t = owner;
    if (t->generation != generation || t->state != T_BLOCKED) return;
    t->wake_tick = 0;
    task_start(t);
}
void ktimer_wait_task(struct task *t, uint64_t deadline)
{
    t->wake_tick = deadline;
    ktimer_arm(&t->wake_timer, deadline, 0, t, &t->generation, timer_task_wake);
}
/* End F0-46 timers. */

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

void kwait_init(struct kwait *q) { memset(q, 0, sizeof(*q)); }

static void wait_unlink(struct kwaiter *w)
{
    struct task *owner = w->donating ? w->queue->owner : 0;
    if (w->queue->notify_cursor == w) w->queue->notify_cursor = w->next;
    w->donating = false;
    if (w->task->sync_waiter == w) w->task->sync_waiter = 0;
    w->task->waiting_on = 0;
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
    donation_update(owner);
}

static bool wait_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t d, uint32_t *ticket)
{
    thread_context();
    struct kwaiter w = { .task = g_current, .queue = q, .cond = cond, .arg = arg, .ticket = ticket };
    uint32_t f = irq_save();
    w.next = q->head;
    q->head = &w;
    w.all_next = waiters;
    waiters = &w;
    w.task->sync_waiter = &w;
    bool ready;
    while (!(ready = cond(arg)) && !deadline_passed(d)) {
        if (q->owner) {
            w.donating = !donation_cycle(w.task, q->owner);
            if (!w.donating) klog("[sched] donation_cycle waiter=%u owner=%u", w.task->id, q->owner->id);
            w.task->waiting_on = w.donating ? q : 0;
            donation_update(q->owner);
        }
        ktimer_wait_task(w.task, d);
        w.task->state = T_BLOCKED;
        /* The waiter is visible before IF opens. A wake here changes it
         * to READY and queues it once; schedule handles that state without
         * requeueing. Never carry the queue lock over the sleep. */
        irq_restore(f);
        schedule();
        f = irq_save();
    }
    w.task->wake_tick = 0;
    ktimer_cancel(&w.task->wake_timer);
    wait_unlink(&w);
    irq_restore(f);
    return ready;
}
bool kwait_wait_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t d)
{ return wait_until(q, cond, arg, d, 0); }
bool kwait_wait_ticket_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t d, uint32_t *ticket)
{ return wait_until(q, cond, arg, d, ticket); }

static void wait_wake(struct kwait *q, bool io, bool conditional)
{
    uint32_t f = irq_save();
    for (struct kwaiter *w = q->head; w; w = w->next) {
        if (w->task->state == T_BLOCKED) {
            /* Check completion/producer wakes. Generic wakes may be cancellation
             * or a notification whose predicate is still false. */
            if (conditional && !w->cond(w->arg)) continue;
            if (io) task_io_completed(w->task);
            ksync_task_ready(w->task);
            w->task->wake_tick = 0;
            ktimer_cancel(&w->task->wake_timer);
            /* task_start is the public wrapper around rq_push. Never put
             * an already READY task in the queue a second time. */
            task_start(w->task);
            g_need_resched = true;
        }
    }
    irq_restore(f);
}

static void wait_irq_deferred(void *);
static void wait_irq_work(void *owner, uint32_t generation, unsigned kind)
{
    (void)kind;
    struct kwait *q = owner;
    uint32_t f = irq_save();
    if (q->notify_generation != generation || !q->notify_active) { irq_restore(f); return; }
    ktimer_cancel(&q->notification);
    struct kwaiter *w = q->notify_cursor;
    if (w) {
        q->notify_cursor = w->next;
        if (w->task->state == T_BLOCKED && (!q->notify_io || w->cond(w->arg))) {
            if (q->notify_io) task_io_completed(w->task);
            task_start(w->task);
            g_need_resched = true;
        }
    }
    if (!q->notify_cursor && q->notify_again) {
        q->notify_again = false;
        q->notify_cursor = q->head;
    }
    if (q->notify_cursor) {
        if (!kwork_queue(wait_irq_deferred, q))
            ktimer_arm(&q->notification, g_ticks, 3, q, &q->notify_generation, wait_irq_work);
    } else q->notify_active = false;
    irq_restore(f);
}
static void wait_irq_deferred(void *arg)
{
    struct kwait *q = arg;
    wait_irq_work(q, q->notify_generation, 0);
}
void kwait_wake_irq(struct kwait *q, bool io)
{
    uint32_t f = irq_save();
    if (q->notifications != UINT32_MAX) q->notifications++;
    if (q->notify_active) {
        q->notify_again = true;
        q->notify_io &= io;
    } else if (q->head) {
        if (!q->notify_generation) q->notify_generation = gen_alloc();
        q->notify_active = true; q->notify_io = io; q->notify_again = false;
        q->notify_cursor = q->head;
        if (!kwork_queue(wait_irq_deferred, q))
            ktimer_arm(&q->notification, g_ticks, 3, q, &q->notify_generation, wait_irq_work);
    }
    irq_restore(f);
}

void kwait_wake_all(struct kwait *q) { wait_wake(q, false, false); }
void kwait_wake_ready(struct kwait *q) { wait_wake(q, false, true); }
void kwait_wake_io(struct kwait *q) { wait_wake(q, true, true); }

void kmutex_init(struct kmutex *m)
{
    m->owner = 0;
    m->held_next = 0;
    kwait_init(&m->wait);
}

static bool mutex_free(void *arg) { return !((struct kmutex *)arg)->owner; }

bool kmutex_lock_until(struct kmutex *m, uint64_t deadline)
{
    thread_context();
    for (struct kmutex *p = held; p; p = p->held_next)
        if (p->owner == g_current && (uintptr_t)p >= (uintptr_t)m) {
            if (p == m) panic("mutex: recursive acquisition");
            /* Cyclic waits remain bounded by the caller's timeout. Never
             * recurse around the donation graph, or change lock ordering. */
            if (!donation_cycle(g_current, m->owner)) panic("mutex: lock order");
        }
    if (m->owner && !kwait_wait_until(&m->wait, mutex_free, m, deadline)) return false;
    uint32_t f = irq_save();
    m->owner = g_current;
    kwait_set_owner(&m->wait, g_current);
    kpreempt_hold();
    m->held_next = held;
    held = m;
    irq_restore(f);
    return true;
}
void kmutex_lock(struct kmutex *m)
{
    while (!kmutex_lock_until(m, deadline_after_ms(1000))) {
        /* The void API cannot return an acquisition failure. After the
         * protected cycle record and one real bounded wait, fail closed
         * rather than retrying an impossible cyclic dependency forever. */
        if (donation_cycle(g_current, m->owner)) panic("mutex: cyclic wait timeout");
    }
}

void kmutex_unlock(struct kmutex *m)
{
    thread_context();
    if (m->owner != g_current)
        panic("mutex: unlock by non-owner");
    uint32_t f = irq_save();
    struct kmutex **p = &held;
    while (*p != m) {
        if ((*p)->owner == g_current)
            panic("mutex: unlock order");
        p = &(*p)->held_next;
    }
    *p = m->held_next;
    m->held_next = 0;
    m->owner = 0;
    kwait_set_owner(&m->wait, 0);
    kpreempt_release();
    irq_restore(f);
    kwait_wake_all(&m->wait);
}

void ksync_task_exit(struct task *t)
{
    ktimer_cancel(&t->wake_timer);
    ktimer_cancel(&t->input_timer);
    ktimer_cancel(&t->ready_timer);
    uint32_t f = irq_save();
    /* Retire dependencies before a killed task's stack can disappear. */
    struct kmutex **link = &held;
    while (*link) {
        struct kmutex *m = *link;
        if (m->owner != t) { link = &m->held_next; continue; }
        *link = m->held_next; m->held_next = 0; m->owner = 0;
        kwait_set_owner(&m->wait, 0);
        if (t->preempt_holds) t->preempt_holds--;
        kwait_wake_all(&m->wait);
    }
    struct kwait *q = owned_queues;
    while (q) {
        struct kwait *next = q->owned_next;
        if (q->owner == t) {
            if (q->owner_died) q->owner_died(q);
            kwait_set_owner(q, 0);
            if (!q->owner_preemptible && t->preempt_holds) t->preempt_holds--;
            kwait_wake_all(q);
        }
        q = next;
    }
    struct kwaiter *w = waiters;
    while (w) {
        struct kwaiter *next = w->all_next;
        if (w->task == t) {
            struct kwait *queue = w->queue;
            if (w->ticket && queue->ticket_cancelled) {
                uint32_t cancelled = *w->ticket;
                for (struct kwaiter *later = queue->head; later; later = later->next)
                    if (later->ticket && later != w &&
                        (uint32_t)(*later->ticket - cancelled - 1) < (1u << 31))
                        --*later->ticket;
                queue->ticket_cancelled(queue);
            }
            wait_unlink(w);
            if (queue->ticket_cancelled) kwait_wake_all(queue);
        }
        w = next;
    }
    ksync_task_ready(t);
    t->wake_tick = 0;
    irq_restore(f);
}
