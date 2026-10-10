/* FIFO private wait words. Compare/enqueue, wake and cancellation use one
 * short irq lock. Waiting never retains a user page pin or a VA lock.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/process.h>

static struct ww_waiter *head, *tail;
static int64_t seed_seconds;
static uint64_t seed_tick;

int ww_bind(struct uaddr *u, uint32_t address, struct ww_key *key)
{
    if (address & 3)
        return -EINVAL;
    if (!ua_range(u, address, sizeof(uint32_t), PROT_READ))
        return -EFAULT;
    struct ua_extent *e = ua_find(u, address);
    *key = (struct ww_key){ u->identity, e->generation, address };
    return 0;
}

/* Caller holds irq lock from validation/read through enqueue. Exposed so T0
 * forces the actual queue's compare/enqueue/wake/unmap interleavings. */
int ww_enqueue(struct ww_waiter *w, const struct ww_key *key, struct task *task,
               uint32_t observed, uint32_t expected)
{
    if (observed != expected)
        return -EAGAIN;
    *w = (struct ww_waiter){ .key = *key, .task = task, .queued = true };
    if (tail)
        tail->next = w;
    else
        head = w;
    tail = w;
    return 0;
}

static void remove_waiter(struct ww_waiter **at, struct ww_waiter *previous, int result)
{
    struct ww_waiter *w = *at;
    *at = w->next;
    if (tail == w)
        tail = previous;
    w->next = 0;
    w->queued = false;
    w->result = result;
    if (w->task && w->task->state == T_BLOCKED) {
        w->task->wake_tick = 0;
        task_start(w->task);
        g_need_resched = true;
    }
}

uint32_t ww_wake(const struct ww_key *key, uint32_t count)
{
    uint32_t flags = irq_save(), n = 0;
    struct ww_waiter **at = &head, *prev = 0;
    while (*at && n < count) {
        struct ww_waiter *w = *at;
        if (w->key.process == key->process && w->key.generation == key->generation &&
            w->key.address == key->address) {
            remove_waiter(at, prev, 0);
            n++;
        } else {
            prev = w;
            at = &w->next;
        }
    }
    irq_restore(flags);
    return n;
}

void ww_cancel(void *process, uint64_t generation, uint32_t base, uint32_t end)
{
    uint32_t flags = irq_save();
    struct ww_waiter **at = &head, *prev = 0;
    while (*at) {
        struct ww_waiter *w = *at;
        if (w->key.process == process && w->key.generation == generation &&
            w->key.address >= base && w->key.address < end)
            remove_waiter(at, prev, -ECANCELED);
        else {
            prev = w;
            at = &w->next;
        }
    }
    irq_restore(flags);
}

void ww_interrupt(struct ww_waiter *w, int error)
{
    uint32_t flags = irq_save();
    struct ww_waiter **at = &head, *prev = 0;
    while (*at && *at != w) {
        prev = *at;
        at = &(*at)->next;
    }
    if (*at)
        remove_waiter(at, prev, error);
    irq_restore(flags);
}

void proc_clock_seed(int64_t seconds, uint64_t tick)
{
    uint32_t flags = irq_save();
    seed_seconds = seconds;
    seed_tick = tick;
    irq_restore(flags);
}

int proc_clock_now(uint32_t clock, struct ciuki_timespec *out)
{
    if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME)
        return -EINVAL;
    uint32_t flags = irq_save();
    uint64_t ticks = g_ticks;
    int64_t seconds = 0;
    if (clock == CLOCK_REALTIME) {
        ticks -= seed_tick;
        seconds = seed_seconds;
    }
    irq_restore(flags);
    *out = (struct ciuki_timespec){ .tv_sec = seconds + (int64_t)(ticks / 1000),
        .tv_nsec = (int32_t)(ticks % 1000) * 1000000 };
    return 0;
}

bool proc_deadline_passed(const struct ciuki_timespec *deadline, uint32_t clock)
{
    struct ciuki_timespec now;
    if (proc_clock_now(clock, &now))
        return false;
    return now.tv_sec > deadline->tv_sec ||
           (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec);
}

int proc_wait_word(uint32_t word, uint32_t expected, uint32_t deadline_va, uint32_t clock)
{
    if ((word & 3) || (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME))
        return -EINVAL;
    struct ciuki_timespec deadline;
    if (deadline_va) {
        int err = copy_from_user(&deadline, deadline_va, sizeof(deadline));
        if (err)
            return err;
        if (deadline.reserved || deadline.tv_sec < 0 || deadline.tv_nsec < 0 || deadline.tv_nsec >= 1000000000)
            return -EINVAL;
    }
    struct proc_thread *t = proc_thread_for(g_current);
    struct ww_key key;
    uint32_t observed, flags = irq_save();
    int err = ww_bind(t->process->memory, word, &key);
    if (!err)
        err = copy_from_user(&observed, word, sizeof(observed));
    if (!err && observed != expected)
        err = -EAGAIN;
    if (!err && deadline_va && proc_deadline_passed(&deadline, clock))
        err = -ETIMEDOUT;
    if (!err && t->interrupted && !t->in_handler)
        err = -EINTR;
    if (err) {
        irq_restore(flags);
        return err;
    }
    struct ww_waiter *w = &t->word_wait;
    ww_enqueue(w, &key, g_current, observed, expected);
    while (w->queued) {
        if (deadline_va && proc_deadline_passed(&deadline, clock)) {
            ww_interrupt(w, -ETIMEDOUT);
            break;
        }
        if (t->process->state == PROC_STOPPING || (t->interrupted && !t->in_handler)) {
            ww_interrupt(w, -EINTR);
            break;
        }
        /* Poll the absolute domain each timer quantum; no conversion can
         * overflow and no timeout can be reported before its deadline. */
        g_current->wake_tick = deadline_va ? g_ticks + 1 : 0;
        g_current->state = T_BLOCKED;
        schedule();
    }
    g_current->wake_tick = 0;
    irq_restore(flags);
    return w->result;
}

int proc_wake_word(uint32_t word, uint32_t count)
{
    struct process *p = proc_current();
    struct ww_key key;
    uint32_t flags = irq_save();
    int err = ww_bind(p->memory, word, &key);
    if (!err)
        err = (int)ww_wake(&key, count);
    irq_restore(flags);
    return err;
}
