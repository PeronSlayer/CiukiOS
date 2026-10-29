/* A single UP publication point for atomic multi-object acquisition.
 * A wait retains its resolved objects; closing a handle cannot recycle them.
 * No allocation, user copy or callback occurs in the acquisition pass.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/w32_objects.h>
#include <ciuki/security.h>
struct w32_wait {
    struct w32_wait *next;
    struct proc_thread *thread;
    struct w32_object *objects[W32_WAIT_MAX];
    uint32_t count, mask, result, generation;
    bool all, alertable, new_input, services, desktop;
};
static struct w32_wait *waiters, **wait_tail = &waiters;
static uint32_t w32_change_generation;
static bool object_ready(struct w32_object *o, uint32_t tid)
{
    if (o->type == W32_OBJ_MUTEX)
        return !o->u.mutex.owner || o->u.mutex.owner == tid;
    if (o->type == W32_OBJ_SEMAPHORE)
        return o->u.semaphore.count != 0;
    if (o->type == W32_OBJ_PROCESS || o->type == W32_OBJ_THREAD || o->type == W32_OBJ_EVENT
        || o->type == W32_OBJ_TIMER || o->type == W32_OBJ_FILE)
        return o->signalled;
    return false;
}
static bool consume(struct w32_object *o, uint32_t tid)
{
    bool abandoned = false;
    if (o->type == W32_OBJ_MUTEX) {
        abandoned = o->abandoned;
        o->abandoned = false;
        if (!o->u.mutex.owner) {
            o->u.mutex.owner = tid;
            w32_retain(o);
        }
        o->u.mutex.recursion++;
    } else if (o->type == W32_OBJ_SEMAPHORE)
        o->u.semaphore.count--;
    else if ((o->type == W32_OBJ_EVENT || o->type == W32_OBJ_TIMER) && !o->manual)
        o->signalled = false;
    return abandoned;
}
static uint32_t acquire(struct w32_wait *w)
{
    if (w->thread->process->state != PROC_LIVE || !security_process_live(w->thread->process) ||
        w->thread->stopped || w->thread->w32->terminate_requested)
        return W32_WAIT_FAILED;
    if (w->alertable && w->thread->w32->head)
        return W32_WAIT_IO_COMPLETION;
    if (w->services && w32_service_ready(w->thread, w->desktop))
        return W32_WAIT_SERVICE;
    bool queue = !!(w->mask & (w->new_input ? w32_queue_changed(w->thread) : w32_queue_bits(w->thread)));
    if (!w->count && !w->mask)
        return W32_WAIT_PENDING;
    if (w->all) {
        if (w->mask && !queue)
            return W32_WAIT_PENDING;
        for (unsigned i = 0; i < w->count; i++)
            if (!object_ready(w->objects[i], w->thread->tid))
                return W32_WAIT_PENDING;
        uint32_t result = 0;
        for (unsigned i = 0; i < w->count; i++)
            if (consume(w->objects[i], w->thread->tid) && !result)
                result = W32_WAIT_ABANDONED_0 + i;
        return result;
    }
    for (unsigned i = 0; i < w->count; i++)
        if (object_ready(w->objects[i], w->thread->tid))
            return (consume(w->objects[i], w->thread->tid) ? W32_WAIT_ABANDONED_0 : 0) + i;
    return queue ? w->count : W32_WAIT_PENDING;
}
void w32_wait_pump(void)
{
    uint32_t f = irq_save();
    for (struct w32_wait *w = waiters; w; w = w->next)
        if (w->result == W32_WAIT_PENDING)
            w->result = acquire(w);
    irq_restore(f);
}
void w32_changed_wake(void)
{
    w32_change_generation++;
    w32_wait_pump();
    kwait_wake_all(&w32_changed);
}
static uint32_t resolve(struct w32_wait *w, const uint32_t *handles)
{
    for (unsigned i = 0; i < w->count; i++) {
        struct w32_object *o = w32_lookup(w->thread, handles[i], 0);
        if (!o
            || !(o->type == W32_OBJ_PROCESS || o->type == W32_OBJ_THREAD || o->type == W32_OBJ_EVENT
                || o->type == W32_OBJ_MUTEX || o->type == W32_OBJ_SEMAPHORE || o->type == W32_OBJ_TIMER
                || o->type == W32_OBJ_FILE))
            return 6;
        if (!w32_handle_access(w->thread, handles[i], 0x100000u))
            return 5;
        for (unsigned j = 0; j < i; j++)
            if (w->objects[j] == o)
                return 87;
        w->objects[i] = o;
        w32_retain(o);
    }
    return 0;
}
static void drop_wait(struct w32_wait *w)
{
    for (unsigned i = 0; i < w->count; i++)
        w32_release(w->objects[i]);
}
uint32_t w32_wait_try(struct proc_thread *t, const uint32_t *handles, unsigned count, bool all, uint32_t mask)
{
    if (count > 64 || (mask && count == 64) || mask & ~W32_QS_ALL)
        return W32_WAIT_FAILED;
    struct w32_wait w = { .thread = t, .count = count, .all = all, .mask = mask };
    uint32_t error = resolve(&w, handles), f = irq_save();
    uint32_t result = error ? W32_WAIT_FAILED : acquire(&w);
    irq_restore(f);
    drop_wait(&w);
    return result;
}
static bool wait_ready(void *arg)
{
    struct w32_wait *w = arg;
    return w->result != W32_WAIT_PENDING || w->generation != w32_change_generation ||
        w->thread->process->state != PROC_LIVE || w->thread->stopped;
}
uint32_t w32_wait_call(struct proc_thread *t, struct ciuki_w32_wait_args *a)
{
    a->result = W32_WAIT_FAILED;
    if (!security_process_live(t->process)) { a->error=5; return 0; }
    if (a->count > 64 || (a->wake_mask && a->count == 64) || a->all > 1 || a->alertable > 1
        || a->wake_mask & ~W32_QS_ALL || a->reserved & ~(W32_WAIT_NEW_INPUT | W32_WAIT_SERVICES | W32_WAIT_SESSION)) {
        a->error = 87;
        return 0;
    }
    uint32_t handles[64];
    if (a->count && copy_from_user(handles, a->handles, a->count * 4)) {
        a->error = 998;
        return 0;
    }
    struct w32_wait w = { .thread = t,
        .count = a->count,
        .all = !!a->all,
        .new_input = !!(a->reserved & W32_WAIT_NEW_INPUT),
        .services = !!(a->reserved & (W32_WAIT_SERVICES | W32_WAIT_SESSION)),
        .desktop = !!(a->reserved & W32_WAIT_SERVICES),
        .alertable = !!a->alertable,
        .mask = a->wake_mask,
        .result = W32_WAIT_PENDING };
    a->error = resolve(&w, handles);
    if (a->error) {
        drop_wait(&w);
        return 0;
    }
    w32_timer_poll();
    if (a->alertable && t->w32->head && !t->process->w32->apc_dispatcher) {
        a->error = 87;
        drop_wait(&w);
        return 0;
    }
    uint64_t deadline = a->timeout == W32_INFINITE ? UINT64_MAX : w32_now() + a->timeout;
    /* Signal and enrollment share one publication interval. The signal row
     * has no user copies and does not allocate. Never expose a lost-wakeup gap. */
    kpreempt_hold();
    if (a->signal) {
        struct w32_object *o = w32_lookup(t, a->signal, 0);
        if (!o || (o->type != W32_OBJ_EVENT && o->type != W32_OBJ_MUTEX && o->type != W32_OBJ_SEMAPHORE))
            a->error = 6;
        else {
            struct ciuki_w32_object_args signal = { .handle = a->signal,
                .action = o->type == W32_OBJ_EVENT ? W32_OBJ_SET : W32_OBJ_RELEASE,
                .value = 1 };
            w32_object_call(t, &signal);
            a->error = signal.error;
        }
    }
    if (!a->error) {
        *wait_tail = &w;
        wait_tail = &w.next;
        w32_wait_pump();
    }
    kpreempt_release();
    while (!a->error && w.result == W32_WAIT_PENDING) {
        w.generation = w32_change_generation;
        uint64_t now = w32_now();
        if (a->timeout != W32_INFINITE && (int64_t)(now - deadline) >= 0) {
            w.result = W32_WAIT_TIMEOUT;
            break;
        }
        w32_timer_poll();
        w32_wait_pump();
        if (w.result != W32_WAIT_PENDING)
            break;
        /* Initial GUI idle is a process latch, never an inference from a
         * window existing or a Sleep. Yielding PeekMessage publishes idle
         * separately; PM_NOYIELD and zero-time waits do not.
         * The whole queue must be empty, even for a filtered input wait. */
        if (a->wake_mask && t->w32->queue && !w32_queue_bits(t) && t->process->w32->gui)
            t->process->w32->input_idle = true;
        uint64_t wake = deadline;
        for (struct w32_object *o = w32_objects; o; o = o->next)
            if (o->type == W32_OBJ_TIMER && o->u.timer.armed && o->u.timer.due < wake)
                wake = o->u.timer.due;
        kwait_wait_until(&w32_changed, wait_ready, &w, wake);
        if (t->process->state != PROC_LIVE || t->stopped) {
            w.result = W32_WAIT_FAILED;
            a->error = 995;
            break;
        }
    }
    struct w32_wait **at = &waiters;
    while (*at && *at != &w)
        at = &(*at)->next;
    if (*at)
        *at = w.next;
    wait_tail = &waiters;
    while (*wait_tail)
        wait_tail = &(*wait_tail)->next;
    a->result = w.result;
    drop_wait(&w);
    return 0;
}
void w32_timer_poll(void)
{
    uint64_t now = w32_now();
    bool changed = false;
    for (struct w32_object *o = w32_objects; o; o = o->next) {
        if (o->type != W32_OBJ_TIMER || !o->u.timer.armed || (int64_t)(now - o->u.timer.due) < 0)
            continue;
        o->signalled = true;
        changed = true;
        if (o->u.timer.period) {
            uint64_t late = now - o->u.timer.due;
            o->u.timer.due += ((late / o->u.timer.period) + 1) * o->u.timer.period;
        } else
            o->u.timer.armed = false;
        if (o->u.timer.routine) {
            for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
                struct proc_thread *t = proc_thread_slot(i);
                if (t && t->tid == o->u.timer.tid && t->w32) {
                    /* Coalesce a periodic timer's outstanding APC. */
                    bool queued = false;
                    for (struct w32_apc_node *n = t->w32->head; n; n = n->next)
                        if (n->routine == o->u.timer.routine && n->argument == o->u.timer.argument)
                            queued = true;
                    uint64_t system = w32_system_time();
                    if (!queued)
                        w32_apc_queue(t, o->u.timer.routine, o->u.timer.argument, (uint32_t)system,
                            (uint32_t)(system >> 32));
                    break;
                }
            }
        }
    }
    if (changed) w32_changed_wake();
}
