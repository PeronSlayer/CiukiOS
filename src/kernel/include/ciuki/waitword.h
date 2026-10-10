/* Private wait words; a bound key is never resolved a second time.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_WAITWORD_H
#define CIUKI_WAITWORD_H
#include <ciuki/uaddr.h>
struct task;
struct ww_key { void *process; uint64_t generation; uint32_t address; };
struct ww_waiter {
    struct ww_key key;
    struct task *task;
    struct ww_waiter *next;
    int result;
    bool queued;
};
int ww_bind(struct uaddr *u, uint32_t address, struct ww_key *key);
int ww_enqueue(struct ww_waiter *w, const struct ww_key *key, struct task *task,
               uint32_t observed, uint32_t expected);
uint32_t ww_wake(const struct ww_key *key, uint32_t count);
void ww_cancel(void *process, uint64_t generation, uint32_t base, uint32_t end);
void ww_interrupt(struct ww_waiter *w, int error);
int proc_wait_word(uint32_t word, uint32_t expected, uint32_t deadline, uint32_t clock);
int proc_wake_word(uint32_t word, uint32_t count);
/* f2-03 installs its qualified seed offset, without new CMOS port access. */
void proc_clock_seed(int64_t utc_seconds, uint64_t monotonic_tick);
int proc_clock_now(uint32_t clock, struct ciuki_timespec *out);
bool proc_deadline_passed(const struct ciuki_timespec *deadline, uint32_t clock);
#endif
