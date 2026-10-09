/* Driver synchronization and elapsed-time helpers (F1).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SYNC_H
#define CIUKI_SYNC_H

#include <stdint.h>
#include <stdbool.h>

struct task;
struct kwaiter;
struct kwait { struct kwaiter *head; };
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
 * task state or freeing its stack. It cancels waits and panics on held locks.
 * task.c is outside f1-00's allowed files; its lead must install that hook. */
typedef bool (*kwait_cond_fn)(void *arg);
void kwait_init(struct kwait *q);
bool kwait_wait_until(struct kwait *q, kwait_cond_fn cond, void *arg, uint64_t deadline_ticks);
void kwait_wake_all(struct kwait *q);
void kmutex_init(struct kmutex *m);
void kmutex_lock(struct kmutex *m);
void kmutex_unlock(struct kmutex *m);
void ksync_task_exit(struct task *t);

/* Deadlines are modulo 2^64, less than 2^63 ticks into the future; zero is
 * a real deadline. PIT ticks are nominal milliseconds, as in task_sleep_ms.
 * The F0 scheduler treats wake_tick==0 as untimed: a wait expiring exactly
 * at wrap may resume one tick late. Deadline arithmetic itself is exact. */
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
