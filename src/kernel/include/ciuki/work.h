/* Bounded deferred work, one P_DEVICE kernel thread.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_WORK_H
#define CIUKI_WORK_H

#include <stdint.h>
#include <stdbool.h>

#define KWORK_CAPACITY 64u
typedef void (*kwork_fn)(void *arg);
struct kwork_stats {
    uint64_t queued, completed, lost, yields, budget_violations;
    uint64_t max_run_us;
    unsigned pending;
};

/* Call init once in thread context before publishing IRQ producers; repeat
 * calls are harmless. Queue never allocates or logs, and returns false on
 * overflow/uninitialized use (counted, reported by the worker). Callback
 * storage must remain valid until completion. Drivers carry their own
 * generation with each request and reject stale completions.
 *
 * Callbacks run with IF=1. Each must return or call kwork_yield within 1 ms;
 * this non-preemptible kernel cannot interrupt a single long callback.
 * The worker also yields after every callback, so short callbacks cannot
 * accumulate into an unbounded batch. */
int kwork_init(void);
bool kwork_queue(kwork_fn fn, void *arg);
void kwork_yield(void);
bool kwork_run_one(void);          /* worker step, also usable by host tests */
void kwork_snapshot(struct kwork_stats *out);

#endif
