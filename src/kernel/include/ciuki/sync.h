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
};

/* UP lock order: sleeping mutexes in increasing address order, then the
 * short irq_save section. Unlock in reverse order. Never sleep with IF=0,
 * in an IRQ, or while holding an irq lock. Conditions run under irq_save:
 * they must be short, nonblocking and must not acquire a mutex. Publish a
 * condition before wake_all. Queue/condition storage must outlive waiters.
 * No mutex may cross a user return or firmware entry.
 *
 * Integration: task exit/kill MUST call ksync_task_exit before changing the
 * task state or freeing its stack. It cancels waits and retires held locks.
 * task.c is outside f1-00's allowed files; its lead must install that hook. */
typedef bool (*kwait_cond_fn)(void *arg);
void kwait_init(struct kwait *q);
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
enum { KSYNC_DEPEND_DRIVE = 1, KSYNC_DEPEND_CACHE = 2 };
void ksync_depend(struct task *owner, const void *object, unsigned reason) __attribute__((weak));
void ksync_depend_release(const void *object) __attribute__((weak));
void kmutex_init(struct kmutex *m);
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
