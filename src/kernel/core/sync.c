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
/* f1-98: tasks with a reservation dependency (task.depends_on), unordered. */
static struct task *dependents;

/* Every live outgoing dependency is a wait on an owned lock/request queue,
 * or (f1-98) a polling waiter's reservation dependency. Device waits have
 * owner==NULL. Suppress the edge that would close a cycle. */
static struct task *wait_owner(struct task *t)
{ return !t ? 0 : t->waiting_on ? t->waiting_on->owner : t->depends_on; }
/* The kmutex lock-order checks keep the pre-f1-98 graph of kwait edges only:
 * a reservation dependency never turns a mutex wait into a panic path. */
static struct task *lock_owner(struct task *t)
{ return t && t->waiting_on ? t->waiting_on->owner : 0; }
static bool owner_cycle(struct task *waiter, struct task *owner, struct task *(*next)(struct task *))
{
    struct task *slow = owner, *fast = owner;
    while (slow) {
        if (slow == waiter) return true;
        slow = next(slow);
        fast = next(next(fast));
        if (slow && slow == fast) return true;
    }
    return false;
}
static bool donation_cycle(struct task *waiter, struct task *owner)
{ return owner_cycle(waiter, owner, wait_owner); }
static bool lock_cycle(struct task *waiter, struct task *owner)
{ return owner_cycle(waiter, owner, lock_owner); }
static void donation_update(struct task *t)
{
    if (propagating) return;
    propagating = true;
    while (t) {
        unsigned highest = 0, dependency = 0;
        bool lend = false; /* f1-98: a desktop-share member waits for t */
        struct task *source = 0; /* f1-98: the reservation waiter that sets it */
        for (struct kwaiter *w = waiters; w; w = w->all_next)
            if (w->donating && w->queue->owner == t) {
                /* f1-101: a boosted audio producer lends at most SCHED_BOOST_CAP. */
                if (task_donor_priority(w->task) > highest) highest = task_donor_priority(w->task);
                lend |= w->task->share_group || w->task->share_lent;
            }
        for (struct task *d = dependents; d; d = d->depend_next)
            if (d->depends_on == t) {
                if (task_donor_priority(d) > dependency) { dependency = task_donor_priority(d); source = d; }
                lend |= d->share_group || d->share_lent;
            }
        if (dependency > highest) highest = dependency;
        else source = 0; /* a kwaiter sets it, or ties with it */
        unsigned old = t->effective_priority;
        bool was_lent = t->share_lent;
        task_set_inherited(t, highest);
        if (was_lent != lend) task_set_share_lent(t, lend);
        /* f1-98: record a reservation dependency that raised the holder. */
        if (source && t->effective_priority > old && t->effective_priority == highest && cpu_inherit)
            cpu_inherit(source, t, highest, source->depend_reason);
        if (old == t->effective_priority && was_lent == t->share_lent) break;
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
/* f1-98 (R2, Windows VMM Begin_Critical_Section boost; Win95 SDK priority
 * inversion): a request that polls for a drive reservation, a writer turn
 * or a filesystem lock (fs_port.h fs_depend) sleeps in its own pause, not on
 * a kwait, so it names the holder here. The edge stays across the waiter's
 * pause wakeups; it ends when the waiter withdraws (admitted, cancelled,
 * timed out), when the holder releases that object, or at either task's
 * exit, so the inheritance never outlives the hold. */
static void depend_unlink(struct task *t)
{
    struct task **p = &dependents;
    while (*p && *p != t) p = &(*p)->depend_next;
    if (*p) *p = t->depend_next;
    t->depend_next = 0; t->depends_on = 0; t->depend_object = 0; t->depend_reason = 0;
}
void ksync_depend(struct task *owner, const void *object, unsigned reason)
{
    struct task *t = g_current;
    if (!t) return;
    uint32_t f = irq_save();
    if (owner == t || (owner && (owner->state == T_ZOMBIE || owner->state == T_DEAD))) owner = 0;
    if (t->depends_on == owner && (!owner || t->depend_object == object)) { irq_restore(f); return; }
    struct task *old = t->depends_on;
    if (old) depend_unlink(t);
    /* A dependency that would close a cycle donates nothing; the waiter's own
     * request deadline bounds it, as for a cyclic kwait. */
    if (owner && donation_cycle(t, owner)) owner = 0;
    if (owner) {
        t->depends_on = owner; t->depend_object = object; t->depend_reason = (uint8_t)reason;
        t->depend_next = dependents; dependents = t;
    }
    donation_update(old);
    donation_update(owner); /* records the raise ([sched] inherit) */
    irq_restore(f);
}
void ksync_depend_release(const void *object)
{
    struct task *owner = g_current;
    if (!owner || !dependents) return;
    uint32_t f = irq_save();
    bool released = false;
    for (struct task **p = &dependents; *p;) {
        struct task *d = *p;
        if (d->depends_on == owner && d->depend_object == object) {
            *p = d->depend_next;
            d->depend_next = 0; d->depends_on = 0; d->depend_object = 0; d->depend_reason = 0;
            released = true;
        } else p = &d->depend_next;
    }
    if (released) donation_update(owner);
    irq_restore(f);
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
    while (!(ready = cond(arg)) && (d == UINT64_MAX || !deadline_passed(d))) {
        if (q->owner) {
            w.donating = !donation_cycle(w.task, q->owner);
            if (!w.donating) klog("[sched] donation_cycle waiter=%u owner=%u", w.task->id, q->owner->id);
            w.task->waiting_on = w.donating ? q : 0;
            donation_update(q->owner);
        }
        if (d != UINT64_MAX) ktimer_wait_task(w.task, d);
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
    struct kwaiter *only = q->head;
    if (!io && only && !only->next && !q->notify_active && only->task->audio_class) {
        /* f1-90 (R1/R3): the audio DMA refill is this interrupt's own
         * deferred work. Its single waiter is woken here in O(1), as a
         * Windows 9x wave driver refills at interrupt time, never behind the
         * shared device worker that another device's consumption may have
         * demoted. No predicate runs here; the waiter rechecks its own. */
        if (only->task->state == T_BLOCKED) { task_start(only->task); g_need_resched = true; }
        irq_restore(f);
        return;
    }
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

/* fx-5: noinline keeps __builtin_return_address(0) the caller's site, also
 * where this unit is included by a host harness. */
__attribute__((noinline)) void kmutex_init(struct kmutex *m)
{
    m->owner = 0;
    m->held_next = 0;
    /* fx-5: the init call site is the class of an unnamed mutex. */
    m->name = 0;
    m->init_site = (uintptr_t)__builtin_return_address(0);
    m->site = 0;
    m->level = 0;
    kwait_init(&m->wait);
}

static bool mutex_free(void *arg) { return !((struct kmutex *)arg)->owner; }

/* fx-5 lock-order records name both locks: CLASS/ADDRESS@SITE, where CLASS is
 * the kmutex_init_named name or init@ADDRESS of the kmutex_init call. Fixed
 * 24-byte class text, no formatter dependency; panic() bounds the line. */
struct mutex_label { char text[24]; };
static const char *mutex_class(const struct kmutex *m, struct mutex_label *l)
{
    if (m->name) return m->name;
    static const char digits[] = "0123456789abcdef";
    memcpy(l->text, "init@", 5);
    uint32_t v = (uint32_t)m->init_site;
    for (unsigned i = 0; i < 8; i++) l->text[5 + i] = digits[(v >> (28 - 4 * i)) & 15];
    l->text[13] = 0;
    return l->text;
}
static __attribute__((noreturn)) void mutex_panic(const char *what, const struct kmutex *want, uintptr_t site,
                                                  const struct kmutex *held)
{
    struct mutex_label a, b;
    panic("mutex: %s want=%s/%08lx@%08lx held=%s/%08lx@%08lx task=%s", what,
          mutex_class(want, &a), (unsigned long)(uint32_t)(uintptr_t)want, (unsigned long)(uint32_t)site,
          mutex_class(held, &b), (unsigned long)(uint32_t)(uintptr_t)held, (unsigned long)(uint32_t)held->site,
          g_current ? g_current->name : "boot");
}
/* fx-5 follow-up, lead decision 2026-10-23: an acquisition-order report
 * (want/held) is fatal only where a test can act on it: a boot carrying a
 * test request (CBI_F_TEST_REQUEST, the fw_cfg selector) and hosted host
 * harnesses (__STDC_HOSTED__, or set ksync_order_fatal). A normal boot logs
 * the named record once per (want class, held class) pair, counts every
 * report and continues, as production kernels treat lockdep. Recursion,
 * unlock order, non-owner unlock and the cyclic-wait timeout stay fatal. */
#ifndef KSYNC_ORDER_FATAL_DEFAULT
#define KSYNC_ORDER_FATAL_DEFAULT (__STDC_HOSTED__ != 0)
#endif
bool ksync_order_fatal = KSYNC_ORDER_FATAL_DEFAULT;
struct order_class { const char *name; uintptr_t init_site; };
static struct { struct order_class want, held; } order_pairs[KSYNC_ORDER_PAIRS];
static struct ksync_order_stats order_stats;
static bool order_reporting;
static bool class_is(const struct order_class *c, const struct kmutex *m)
{
    if (c->name || m->name) return c->name && m->name && !strncmp(c->name, m->name, 64);
    return c->init_site == m->init_site;
}
/* True when this (want, held) class pair is new and gets its one record. */
static bool order_note(const struct kmutex *want, const struct kmutex *held_lock)
{
    uint32_t f = irq_save();
    bool fresh = true;
    for (unsigned i = 0; fresh && i < order_stats.pairs; i++)
        if (class_is(&order_pairs[i].want, want) && class_is(&order_pairs[i].held, held_lock)) fresh = false;
    if (fresh && order_stats.pairs == KSYNC_ORDER_PAIRS) { order_stats.dropped++; fresh = false; }
    else if (fresh) {
        order_pairs[order_stats.pairs].want = (struct order_class){ want->name, want->init_site };
        order_pairs[order_stats.pairs].held = (struct order_class){ held_lock->name, held_lock->init_site };
        order_stats.pairs++;
    }
    irq_restore(f);
    return fresh;
}
static void order_report(const struct kmutex *want, uintptr_t site, const struct kmutex *held_lock)
{
    if (!order_note(want, held_lock) || order_reporting) return;
    order_reporting = true; /* klog takes no kmutex; never recurse regardless */
    struct mutex_label a, b;
    klog("[kernel] lock_order want=%s/%08lx@%08lx held=%s/%08lx@%08lx task=%s pairs=%u action=continue",
         mutex_class(want, &a), (unsigned long)(uint32_t)(uintptr_t)want, (unsigned long)(uint32_t)site,
         mutex_class(held_lock, &b), (unsigned long)(uint32_t)(uintptr_t)held_lock, (unsigned long)(uint32_t)held_lock->site,
         g_current ? g_current->name : "boot", order_stats.pairs);
    order_reporting = false;
}
void ksync_order_snapshot(struct ksync_order_stats *out)
{
    uint32_t f = irq_save(); *out = order_stats; irq_restore(f);
}
static bool mutex_acquire(struct kmutex *m, uint64_t deadline, uintptr_t site)
{
    thread_context();
    bool reported = false;
    for (struct kmutex *p = held; p; p = p->held_next)
        if (p->owner == g_current && kmutex_order_conflict(p, m)) {
            if (p == m) mutex_panic("recursive acquisition", m, site, p);
            /* Cyclic waits remain bounded by the caller's timeout. Never
             * recurse around the donation graph, or change lock ordering. */
            if (lock_cycle(g_current, m->owner)) continue;
            if (ksync_order_fatal || (g_boot.flags & CBI_F_TEST_REQUEST)) mutex_panic("lock order", m, site, p);
            if (!reported) { uint32_t f = irq_save(); order_stats.violations++; irq_restore(f); reported = true; }
            order_report(m, site, p);
        }
    if (m->owner && !kwait_wait_until(&m->wait, mutex_free, m, deadline)) return false;
    uint32_t f = irq_save();
    m->owner = g_current;
    m->site = site;
    kwait_set_owner(&m->wait, g_current);
    kpreempt_hold();
    m->held_next = held;
    held = m;
    irq_restore(f);
    return true;
}
__attribute__((noinline)) bool kmutex_lock_until(struct kmutex *m, uint64_t deadline)
{
    return mutex_acquire(m, deadline, (uintptr_t)__builtin_return_address(0));
}
__attribute__((noinline)) void kmutex_lock(struct kmutex *m)
{
    uintptr_t site = (uintptr_t)__builtin_return_address(0);
    while (!mutex_acquire(m, deadline_after_ms(1000), site)) {
        /* The void API cannot return an acquisition failure. After the
         * protected cycle record and one real bounded wait, fail closed
         * rather than retrying an impossible cyclic dependency forever. */
        if (lock_cycle(g_current, m->owner)) mutex_panic("cyclic wait timeout", m, site, m);
    }
}

__attribute__((noinline)) void kmutex_unlock(struct kmutex *m)
{
    thread_context();
    if (m->owner != g_current)
        mutex_panic("unlock by non-owner", m, (uintptr_t)__builtin_return_address(0), m);
    uint32_t f = irq_save();
    struct kmutex **p = &held;
    while (*p != m) {
        if ((*p)->owner == g_current)
            mutex_panic("unlock order", m, (uintptr_t)__builtin_return_address(0), *p);
        p = &(*p)->held_next;
    }
    *p = m->held_next;
    m->held_next = 0;
    m->owner = 0;
    m->site = 0;
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
        *link = m->held_next; m->held_next = 0; m->owner = 0; m->site = 0;
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
    /* f1-98: the dying task's own reservation dependency, and every edge that
     * names it as holder (its waiters poll again and find the next holder). */
    if (t->depends_on) { struct task *was = t->depends_on; depend_unlink(t); donation_update(was); }
    for (struct task **p = &dependents; *p;) {
        struct task *d = *p;
        if (d->depends_on == t) {
            *p = d->depend_next;
            d->depend_next = 0; d->depends_on = 0; d->depend_object = 0; d->depend_reason = 0;
        } else p = &d->depend_next;
    }
    ksync_task_ready(t);
    t->wake_tick = 0;
    irq_restore(f);
}
