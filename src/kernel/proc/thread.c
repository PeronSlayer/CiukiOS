/* Native thread identities, stacks/TLS and retained join results.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/process.h>
#include <ciuki/signal.h>

static struct proc_thread *threads[CIUKI_THREAD_MAX];
static uint32_t next_tid = 1;

void proc_thread_cleanup(struct proc_thread *t)
{
    void (*cleanup)(void *) = t->cleanup;
    void *operation = t->operation;
    t->cleanup = 0;
    t->operation = 0;
    if (cleanup)
        cleanup(operation);
}

struct proc_thread *proc_thread_slot(unsigned slot)
{
    return slot < CIUKI_THREAD_MAX ? threads[slot] : 0;
}

struct proc_thread *proc_thread_for(const struct task *task)
{
    if (!task)
        return 0;
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++)
        if (threads[i] && threads[i]->task == task)
            return threads[i];
    return 0;
}

struct proc_thread *proc_thread_find(struct process *p, uint32_t tid)
{
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++)
        if (threads[i] && threads[i]->process == p && threads[i]->tid == tid)
            return threads[i];
    return 0;
}

static int allocate_tls(struct proc_thread *t)
{
    struct ciuki_mmap_args a = { .size = sizeof(a), .length = CIUKI_TLS_SIZE,
        .prot = PROT_READ | PROT_WRITE, .flags = MAP_PRIVATE | MAP_ANONYMOUS, .fd = -1 };
    struct uaddr *u = t->process->memory;
    int32_t result = ua_mmap(u, &a);
    if ((uint32_t)result >= (uint32_t)-CIUKI_SYSCALL_ERROR_MAX)
        return result;
    t->tls = (uint32_t)result;
    struct ua_extent *e = ua_find(u, t->tls);
    e->kind = UA_TLS;
    e->owner = t->tid;
    struct ciuki_tcb tcb = { .self = t->tls, .tid = t->tid,
        .stack_base = t->stack_base, .stack_bytes = t->stack_bytes };
    ua_write(u, t->tls, &tcb, sizeof(tcb));
    int err = ua_pin(u, &t->tls_pin, t->tls, CIUKI_TLS_SIZE, true);
    t->tls_pin.task = 0;               /* explicit owner, not the creating task */
    return err;
}

static int allocate_stack(struct proc_thread *t, bool main)
{
    struct uaddr *u = t->process->memory;
    if (main) {
        t->stack_base = CIUKI_MAIN_STACK_BASE;
        int err = ua_map_at(u, CIUKI_MAIN_STACK_RESERVATION_BASE,
                           CIUKI_MAIN_STACK_BASE - CIUKI_MAIN_STACK_RESERVATION_BASE,
                           PROT_NONE, PROT_NONE, UA_GUARD, t->tid);
        if (err)
            return err;
        return ua_map_at(u, t->stack_base, t->stack_bytes, PROT_READ | PROT_WRITE,
                         PROT_READ | PROT_WRITE, UA_STACK, t->tid);
    }
    struct ciuki_mmap_args a = { .size = sizeof(a), .length = t->stack_bytes + PAGE_SIZE,
        .prot = PROT_NONE, .flags = MAP_PRIVATE | MAP_ANONYMOUS, .fd = -1 };
    int32_t result = ua_mmap(u, &a);
    if ((uint32_t)result >= (uint32_t)-CIUKI_SYSCALL_ERROR_MAX)
        return result;
    uint32_t guard = (uint32_t)result;
    int err = ua_mprotect(u, guard + PAGE_SIZE, t->stack_bytes, PROT_READ | PROT_WRITE);
    if (err) {
        ua_munmap(u, guard, a.length);
        return err;
    }
    struct ua_extent *g = ua_find(u, guard), *s = ua_find(u, guard + PAGE_SIZE);
    g->kind = UA_GUARD;
    s->kind = UA_STACK;
    g->owner = s->owner = t->tid;
    t->stack_base = guard + PAGE_SIZE;
    return 0;
}

int proc_thread_prepare(struct process *p, uint32_t entry, uint32_t argument,
                        uint32_t trampoline, uint32_t stack_bytes, uint32_t flags,
                        uint64_t mask, bool main, struct proc_thread **out)
{
    unsigned slot = 0;
    while (slot < CIUKI_THREAD_MAX && threads[slot])
        slot++;
    if (slot == CIUKI_THREAD_MAX || p->threads == CIUKI_PROCESS_THREAD_MAX || next_tid > CIUKI_ID_MAX)
        return -EAGAIN;
    struct proc_thread *t = kzalloc(sizeof(*t));
    if (!t)
        return -ENOMEM;
    t->slot = slot;
    t->tid = next_tid++;
    t->process = p;
    t->mask = mask;
    t->detached = !!(flags & CIUKI_THREAD_DETACHED);
    t->stack_bytes = stack_bytes ? stack_bytes : CIUKI_THREAD_STACK_DEFAULT;
    threads[slot] = t;
    p->threads++;
    p->live_threads++;
    int err = allocate_stack(t, main);
    if (!err)
        err = allocate_tls(t);
    uint32_t esp = t->stack_base + t->stack_bytes - 2 * sizeof(uint32_t);
    if (!err && !main) {
        uint32_t frame[2] = { trampoline, argument };
        err = ua_write(p->memory, esp, frame, sizeof(frame));
    }
    if (!err && !(t->task = task_create_native(p, entry, esp)))
        err = -ENOMEM;
    if (err) {
        proc_thread_discard(t);
        return err;
    }
    *out = t;
    return 0;
}

void proc_thread_discard(struct proc_thread *t)
{
    struct process *p = t->process;
    if (t->task == g_current && t->task)
        panic("thread: discard current kernel stack");
    if (t->task && task_alive(t->task)) {
        t->exiting = true;
        task_kill(t->task, 0);
    }
    proc_thread_cleanup(t);
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++)
        if (threads[i] && threads[i]->joiner == t)
            threads[i]->joiner = 0;
    if (p->memory) {
        ua_unpin(p->memory, &t->tls_pin);
        if (t->task)
            ua_cancel_pins(p->memory, t->task);
        if (t->task)
            fpu_task_release(t->task);
        if (p->memory->as.pd_phys)
            ua_release_owner(p->memory, t->tid);
    }
    if (t->task) {
        fpu_task_release(t->task);
        memset(&t->task->as, 0, sizeof(t->task->as));
        task_reap(t->task);
    }
    if (!t->stopped)
        p->live_threads--;
    p->threads--;
    threads[t->slot] = 0;
    kfree(t);
}

void proc_thread_collect(struct proc_thread *t)
{
    if (!t->task || t->task == g_current || !t->stopped)
        return;
    struct uaddr *u = t->process->memory;
    proc_thread_cleanup(t);
    ua_unpin(u, &t->tls_pin);
    ua_cancel_pins(u, t->task);
    /* Release any join claim made by a departing thread. */
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++)
        if (threads[i] && threads[i]->joiner == t)
            threads[i]->joiner = 0;
    fpu_task_release(t->task);
    ua_release_owner(u, t->tid);
    memset(&t->task->as, 0, sizeof(t->task->as));
    task_reap(t->task);
    t->task = 0;
    t->tls = t->stack_base = t->stack_bytes = 0;
    t->retained = true;
    kwait_wake_all(&t->process->changed);
    if (t->detached)
        proc_thread_discard(t);
}

static bool executable_image(struct uaddr *u, uint32_t va)
{
    const struct ua_extent *e = ua_find(u, va);
    return e && e->kind == UA_IMAGE && (e->prot & PROT_EXEC);
}

int proc_thread_create(uint32_t args_va)
{
    struct ciuki_thread_args a;
    int err = copy_from_user(&a, args_va, sizeof(a));
    if (err)
        return err;
    if (a.size != sizeof(a) || a.reserved[0] || a.reserved[1] ||
        (a.flags & ~CIUKI_THREAD_DETACHED) || (a.stack_bytes &&
        ((a.stack_bytes & (PAGE_SIZE - 1)) || a.stack_bytes < CIUKI_THREAD_STACK_MIN ||
         a.stack_bytes > CIUKI_THREAD_STACK_MAX)))
        return -EINVAL;
    struct proc_thread *parent = proc_thread_for(g_current), *t;
    if (!executable_image(parent->process->memory, a.entry) ||
        !executable_image(parent->process->memory, a.return_trampoline))
        return -EINVAL;
    err = proc_thread_prepare(parent->process, a.entry, a.argument, a.return_trampoline,
                              a.stack_bytes, a.flags, parent->mask, false, &t);
    if (err)
        return err;
    task_start(t->task);
    return (int)t->tid;
}

void proc_thread_exit(uint32_t value)
{
    struct proc_thread *t = proc_thread_for(g_current);
    t->value = value;
    t->exiting = true;
    task_exit(0);
}

int proc_thread_join(uint32_t tid, uint32_t value_va)
{
    struct proc_thread *self = proc_thread_for(g_current);
    struct proc_thread *target = proc_thread_find(self->process, tid);
    if (!target)
        return -ESRCH;
    if (target == self)
        return -EDEADLK;
    if (target->detached || target->joiner)
        return -EINVAL;
    struct ua_pin pin;
    int err = value_va ? ua_pin(self->process->memory, &pin, value_va, sizeof(uint32_t), true) : 0;
    if (err)
        return err;
    target->joiner = self;
    while (!target->retained) {
        if (self->process->state == PROC_STOPPING || proc_signal_caught(self)) {
            err = -EINTR;
            break;
        }
        task_sleep_ms(1);
    }
    if (!err && value_va)
        err = copy_to_user(value_va, &target->value, sizeof(target->value));
    if (value_va)
        ua_unpin(self->process->memory, &pin);
    target->joiner = 0;
    if (!err)
        proc_thread_discard(target);
    return err;
}

int proc_thread_detach(uint32_t tid)
{
    struct proc_thread *t = proc_thread_find(proc_current(), tid);
    if (!t)
        return -ESRCH;
    if (t->detached || t->joiner)
        return -EINVAL;
    t->detached = true;
    if (t->retained)
        proc_thread_discard(t);
    return 0;
}

int proc_tls_set(uint32_t base, uint32_t bytes)
{
    if ((base & 3) || bytes != CIUKI_TLS_SIZE)
        return -EINVAL;
    struct proc_thread *t = proc_thread_for(g_current);
    struct uaddr *u = t->process->memory;
    if (t->in_handler)
        return -EBUSY;
    if (!ua_range(u, base, bytes, PROT_READ | PROT_WRITE))
        return -EFAULT;
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *other = threads[i];
        if (other && other != t && other->process == t->process && other->tls &&
            base < other->tls + CIUKI_TLS_SIZE && base + bytes > other->tls)
            return -EBUSY;
    }
    for (uint32_t va = base; va < base + bytes;) {
        struct ua_extent *e = ua_find(u, va);
        if (e->kind != UA_ANON && !(e->kind == UA_TLS && e->owner == t->tid))
            return -EINVAL;
        va = e->end < base + bytes ? e->end : base + bytes;
    }
    /* All fields were validated without yielding. Preserve library-owned TCB
     * fields, replacing only the four kernel-initialized identity fields. */
    uint32_t identity[2] = { base, t->tid }, stack[2] = { t->stack_base, t->stack_bytes };
    int err = copy_to_user(base + offsetof(struct ciuki_tcb, self), identity, sizeof(identity));
    if (!err)
        err = copy_to_user(base + offsetof(struct ciuki_tcb, stack_base), stack, sizeof(stack));
    if (err)
        return err;
    uint32_t flags = irq_save();
    struct ua_extent *old = ua_find(u, t->tls);
    if (old && old->kind == UA_TLS && t->tls != base) {
        old->kind = UA_ANON;
        old->owner = 0;
    }
    ua_unpin(u, &t->tls_pin);
    ua_pin(u, &t->tls_pin, base, bytes, true);
    t->tls_pin.task = 0;
    t->tls = base;
    arch_tls_switch(base, true);
    irq_restore(flags);
    return 0;
}
