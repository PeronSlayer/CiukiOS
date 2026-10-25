/* Driver synchronization and elapsed-time helpers (F1).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SYNC_H
#define CIUKI_SYNC_H

#include <stdint.h>
#include <stdbool.h>

struct task;
struct kwaiter;
/* Intrusive, preallocated timers. All deadlines are <2^63 ms away.
 * Cancel removes armed AND due entries. A false cancel result means a
 * callback is executing: retain owner/storage until it returns. Callbacks
 * may rearm themselves but must not sleep/yield or destroy their entry.
 * The owner generation address remains live until successful cancellation. */
enum ktimer_state { KTIMER_IDLE, KTIMER_ARMED, KTIMER_DUE, KTIMER_IN_FLIGHT, KTIMER_CANCELLED };
typedef void (*ktimer_fn)(void *owner, uint32_t generation, unsigned kind);
struct ktimer {
    struct ktimer *left, *right, *parent, *prev, *next;
    uint64_t deadline, revision;
    void *owner;
    const uint32_t *owner_generation;
    ktimer_fn callback;
    uint32_t generation;
    unsigned kind, height;
    enum ktimer_state state;
    bool executing;
};
struct ktimer_stats {
    uint64_t comparisons, transferred, completed, cancelled, stale;
    unsigned armed, due, max_irq_batch;
};
#define KTIMER_IRQ_BATCH 2u
void ktimer_arm(struct ktimer *, uint64_t deadline, unsigned kind, void *owner,
                const uint32_t *generation, ktimer_fn);
bool ktimer_cancel(struct ktimer *);
void ktimer_tick(uint64_t now);
bool ktimer_run_one(void); /* IF=1, one bounded callback; no batch chaining */
void ktimer_snapshot(struct ktimer_stats *);
void ktimer_wait_task(struct task *, uint64_t deadline);

struct kwait {
    struct kwaiter *head;
    struct task *owner;            /* mutex/ticket-lock or producer request dependency */
    bool owner_preemptible;        /* producer owns no lock/preemption hold */
    struct kwait *owned_next;
    void (*owner_died)(struct kwait *);
    void (*ticket_cancelled)(struct kwait *);
    struct ktimer notification;
    struct kwaiter *notify_cursor;
    uint32_t notify_generation, notifications;
    bool notify_io, notify_again, notify_active;
};
struct kmutex {
    struct task *owner;
    struct kwait wait;
    struct kmutex *held_next;
    /* fx-5 lock-order evidence, kernel internal (no ABI). `name` is the lock
     * class (kmutex_init_named); unnamed mutexes are identified by the call
     * site of kmutex_init (`init_site`), as Linux lockdep keys a class by
     * its init site. `site` is the current owner's acquisition site. */
    const char *name;
    uintptr_t init_site, site;
    /* fx-5 class level: 0 (zeroed or kmutex_init) for every ordinary mutex;
     * a negative level marks an outer class. See kmutex_order_conflict. */
    signed char level;
};

/* UP lock order: sleeping mutexes in increasing (level, address) order,
 * then the short irq_save section. Unlock in reverse order. Never sleep
 * with IF=0, in an IRQ, or while holding an irq lock. Conditions run under irq_save:
 * they must be short, nonblocking and must not acquire a mutex. Publish a
 * condition before wake_all. Queue/condition storage must outlive waiters.
 * No mutex may cross a user return or firmware entry.
 * fx-5: an order report names both locks, `want=CLASS/ADDRESS@SITE
 * held=CLASS/ADDRESS@SITE task=NAME` (CLASS: the name, or init@ADDRESS of
 * kmutex_init's caller; SITE: the acquiring call); it is the panic
 * `mutex: lock order …` where fatal, else `[kernel] lock_order …` (below).
 * Resolve addresses with build/f0/VMM.map.
 *
 * Integration: task exit/kill MUST call ksync_task_exit before changing the
 * task state or freeing its stack. It cancels waits and retires held locks.
 * task.c is outside f1-00's allowed files; its lead must install that hook. */
typedef bool (*kwait_cond_fn)(void *arg);
void kwait_init(struct kwait *q);
/* UINT64_MAX waits for the predicate without a deadline timer. */
bool kwait_wait_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t deadline_ticks);
/* FIFO tickets stay reserved across safety timeouts. Task cancellation closes
 * the abandoned ticket and shifts later live tickets, preserving their order. */
bool kwait_wait_ticket_until(struct kwait *q, kwait_cond_fn cond, void *arg,
                             uint64_t deadline_ticks, uint32_t *ticket);
void kwait_wake_all(struct kwait *q);
/* Producer notification: wake satisfied predicates without an I/O boost. */
void kwait_wake_ready(struct kwait *q);
/* Completion-only wake; false predicates and already-ready tasks earn nothing. */
void kwait_wake_io(struct kwait *q);
/* Static device queues only: O(log timers) capture, no predicates/run-queue
 * or donation walks in IRQ context. Repeated notifications coalesce with a
 * saturating count; the worker visits one waiter per continuation. Cancel
 * q->notification before retiring queue storage. */
void kwait_wake_irq(struct kwait *q, bool io);
void kwait_set_owner(struct kwait *q, struct task *owner);
void ksync_task_ready(struct task *t) __attribute__((weak));
void ksync_priority_changed(struct task *t) __attribute__((weak));
/* f1-98 (R2): reservation dependencies of polling waiters. The calling task
 * waits for `owner`, the holder of `object` (a drive reservation or writer
 * turn, or a filesystem lock taken by a request); a NULL owner withdraws the
 * caller's edge. The owner inherits the waiter's effective priority through
 * the same donation walk as a kwait owner, transitively, until the waiter
 * withdraws, the owner calls ksync_depend_release(object) for that object,
 * or either task exits. Weak: harnesses without sync.c skip both calls. */
/* KSYNC_DEPEND_IO (T23 trial 2ca3b0b6): a requester waiting for its block
 * request lends its priority to the queue's executor task (blkreq.c). */
enum { KSYNC_DEPEND_DRIVE = 1, KSYNC_DEPEND_CACHE = 2, KSYNC_DEPEND_IO = 3 };
void ksync_depend(struct task *owner, const void *object, unsigned reason) __attribute__((weak));
void ksync_depend_release(const void *object) __attribute__((weak));
void kmutex_init(struct kmutex *m);
/* A named class for lock-order records; same mutex semantics. */
static inline void kmutex_init_named(struct kmutex *m, const char *name)
{ kmutex_init(m); m->name = name; }
/* An outer class (level < 0) that nests mutexes of other objects, e.g. a
 * subsystem's service lock around a driver's register lock. */
#define KMUTEX_LEVEL_OUTER (-1)
static inline void kmutex_init_level(struct kmutex *m, const char *name, signed char level)
{ kmutex_init(m); m->name = name; m->level = level; }
/* The order predicate of kmutex_lock_until, shared with host lock models:
 * a task holding `held` may not acquire `want` (equal: recursion). Keys
 * compare (level, address). Mutexes of one level nest in increasing address
 * order, which only C guarantees for members of ONE struct declared in
 * acquisition order (C17 6.7.2.1p15). Separate static objects are placed
 * by the compiler and the linker, in build_kernel.py's object order, which
 * any new source file shifts. A nesting across objects must therefore not
 * rest on their addresses: the outer class takes a lower level (fx-5, T23
 * trial of image 9a60215f; inventory in tests/host/test_lock_levels.py). */
static inline bool kmutex_order_conflict(const struct kmutex *held, const struct kmutex *want)
{
    if (held->level != want->level) return held->level > want->level;
    return (uintptr_t)held >= (uintptr_t)want;
}
/* Static initializer of a named class: a zeroed kmutex is a valid unlocked
 * mutex, so `static struct kmutex x = KMUTEX_CLASS("x", level);` needs no
 * kmutex_init before its first use. */
#define KMUTEX_CLASS(name_, level_) { .name = (name_), .level = (level_) }
/* fx-5 follow-up (lead decision 2026-10-23): in a normal boot an order
 * report is logged once per (want class, held class) pair and counted; the
 * boot continues. It is fatal with a test request (CBI_F_TEST_REQUEST) and
 * in hosted builds (ksync_order_fatal). The cpustat window publishes these
 * counters as `[kernel] lock_order violations=… pairs=… dropped=…`. */
#define KSYNC_ORDER_PAIRS 16u
struct ksync_order_stats { uint32_t violations, pairs, dropped; };
extern bool ksync_order_fatal;
void ksync_order_snapshot(struct ksync_order_stats *out);
void kmutex_lock(struct kmutex *m);
bool kmutex_lock_until(struct kmutex *m, uint64_t deadline_ticks);
void kmutex_unlock(struct kmutex *m);
void ksync_task_exit(struct task *t);

/* Deadlines are modulo 2^64, less than 2^63 ticks into the future; zero is
 * a real deadline. PIT ticks are nominal milliseconds, as in task_sleep_ms.
 * The queue's explicit state also permits expiry exactly at tick zero. */
uint64_t deadline_after_ms(uint32_t ms);
bool deadline_passed(uint64_t deadline_ticks);
uint64_t ktime_cycles(void);
bool ktime_elapsed_us(uint64_t start, uint32_t us);
/* 0..1000 us, at most 50 us with IF=0. Returns -EINVAL for an invalid
 * bound, -ENOSYS before TSC calibration, -EFAULT for a stalled TSC. Does
 * not change IF. Callers must include pacing in their total IRQ budget. */
int udelay(uint32_t us);

typedef uint32_t gen_t;
gen_t gen_alloc(void);             /* zero on exhaustion; never wraps/reuses */
bool gen_matches(gen_t current, gen_t supplied);

#endif
