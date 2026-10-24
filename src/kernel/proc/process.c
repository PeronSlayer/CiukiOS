/* Process lifecycle. The supervisor reclaims resources after the departing
 * task has switched away. Only the small process record survives as zombie.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/process.h>
#include <ciuki/signal.h>
#include <ciuki/supervisor.h>

extern void w32_process_cleanup(struct process *) __attribute__((weak));
extern void w32_process_died(struct process *) __attribute__((weak));
extern void w32_thread_died(struct proc_thread *) __attribute__((weak));
extern void w32_process_stopping(struct process *, uint32_t) __attribute__((weak));
extern void net_fd_changed(struct process *, unsigned) __attribute__((weak));

static struct process *processes[CIUKI_PROCESS_MAX];
static struct process supervisor;
static struct task *supervisor_task;
static uint32_t next_pid = 2;
static bool collect_pending;
static bool collecting;

struct process *proc_supervisor(void) { return &supervisor; }

struct process *proc_find(uint32_t pid)
{
    for (unsigned i = 0; i < CIUKI_PROCESS_MAX; i++)
        if (processes[i] && processes[i]->published && processes[i]->pid == pid)
            return processes[i];
    return 0;
}

struct process *proc_at_slot(unsigned slot)
{
    struct process *p=slot<CIUKI_PROCESS_MAX ? processes[slot] : 0;
    return p && p->published ? p : 0;
}

struct process *proc_current(void)
{
    struct proc_thread *t = proc_thread_for(g_current);
    return t ? t->process : 0;
}

void proc_snapshot(struct proc_ledger *out)
{
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < CIUKI_PROCESS_MAX; i++) {
        struct process *p = processes[i];
        if (!p)
            continue;
        out->processes++;
        out->threads += p->threads;
        out->zombies += p->state == PROC_ZOMBIE;
        if (p->fds)
            for (unsigned j = 0; j < CIUKI_OPEN_MAX; j++)
                out->handles += p->fds[j].object != 0;
        if (p->memory) {
            out->extents += p->memory->extents;
            out->backing += p->memory->backing;
            out->tables += p->memory->as.tables;
        }
    }
}

int proc_prepare(struct process *parent, struct process **out)
{
    unsigned slot = 1;
    while (slot < CIUKI_PROCESS_MAX && processes[slot])
        slot++;
    if (slot == CIUKI_PROCESS_MAX || next_pid > CIUKI_ID_MAX)
        return -EAGAIN;
    struct process *p = kzalloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    p->slot = slot;
    p->pid = next_pid++;
    p->ppid = parent->pid;
    p->pgid = parent->pgid;
    p->state = PROC_PREPARING;
    processes[slot] = p;                /* private reservation, not findable */
    kwait_init(&p->changed);
    p->fds = kzalloc(CIUKI_OPEN_MAX * sizeof(*p->fds));
    p->actions = kzalloc((SIGCHLD + 1) * sizeof(*p->actions));
    p->memory = kzalloc(sizeof(*p->memory));
    if (!p->fds || !p->actions || !p->memory || ua_init(p->memory, p)) {
        proc_discard(p);
        return -ENOMEM;
    }
    p->cwd = parent->cwd;
    p->cwd_retain = parent->cwd_retain;
    p->cwd_release = parent->cwd_release;
    if (p->cwd && p->cwd_retain)
        p->cwd_retain(p->cwd);
    if (parent->actions)
        for (unsigned i = 0; i <= SIGCHLD; i++)
            if (parent->actions[i].handler == SIG_IGN)
                p->actions[i].handler = SIG_IGN;
    *out = p;
    return 0;
}

int proc_fd_validate(const struct process *parent, const struct ciuki_spawn_fd *list, uint32_t count)
{
    if (count > CIUKI_OPEN_MAX)
        return -EINVAL;
    bool targets[CIUKI_OPEN_MAX] = { false };
    for (unsigned i = 0; i < count; i++) {
        int32_t source = list[i].source, target = list[i].target;
        if (target < 0 || target >= CIUKI_OPEN_MAX || targets[target])
            return -EINVAL;
        if (source < -1 || source >= CIUKI_OPEN_MAX)
            return -EBADF;
        targets[target] = true;
        if (source >= 0) {
            if (!parent->fds || !parent->fds[source].object || (parent->fds[source].flags & FD_CLOEXEC))
                return -EBADF;
            if (!parent->fds[source].object->inheritable)
                return -EPERM;
        }
    }
    return 0;
}

int proc_inherit(struct process *child, const struct process *parent,
                 const struct ciuki_spawn_fd *list, uint32_t count)
{
    int err = proc_fd_validate(parent, list, count);
    if (err)
        return err;
    bool targets[CIUKI_OPEN_MAX] = { false };
    for (unsigned i = 0; i < count; i++)
        targets[list[i].target] = true;
    /* No allocation or callback before the complete source/target validation.
     * Retain callbacks cannot fail or block; f2-03 provides object operations. */
    if (parent->fds)
        for (unsigned i = 0; i < 3; i++)
            if (!targets[i] && parent->fds[i].object && !(parent->fds[i].flags & FD_CLOEXEC) &&
                parent->fds[i].object->inheritable) {
                child->fds[i].object = parent->fds[i].object;
                child->fds[i].object->retain(child->fds[i].object);
            }
    for (unsigned i = 0; i < count; i++)
        if (list[i].source >= 0) {
            struct proc_object *object = parent->fds[list[i].source].object;
            child->fds[list[i].target].object = object;
            object->retain(object);
        }
    return 0;
}

/* Image basename for enumeration: printable ASCII only, at most 31 bytes,
 * NUL-terminated and zero-filled. Never part of any authorization decision. */
void proc_set_name(struct process *p, const char *path)
{
    unsigned n = 0;
    memset(p->name, 0, sizeof(p->name));
    for (const char *c = path; c && *c; c++) {
        if (*c == '/' || *c == '\\') { n = 0; memset(p->name, 0, sizeof(p->name)); continue; }
        if (n < sizeof(p->name) - 1)
            p->name[n++] = *c >= 32 && *c <= 126 ? *c : '?';
    }
}

/* Consistent copy of published processes in ascending PID order (PID
 * allocation is monotonic, so slot order is not). Only timer-owned counters
 * need an IRQ snapshot. Sorting and record construction have no scheduling
 * points, so process lifetimes remain stable with IF enabled on this UP. */
unsigned proc_list_snapshot(struct ciuki_proc_record *out, unsigned capacity, unsigned *total)
{
    uint64_t ticks[CIUKI_PROCESS_MAX];
    uint32_t flags = irq_save();
    for (unsigned i = 0; i < CIUKI_PROCESS_MAX; i++) ticks[i] = processes[i] ? processes[i]->cpu_ticks : 0;
    irq_restore(flags);
    unsigned written = 0, present = 0;
    uint32_t last = 0;
    for (;;) {
        struct process *next = 0;
        unsigned slot = 0;
        for (unsigned i = 0; i < CIUKI_PROCESS_MAX; i++) {
            struct process *p = processes[i];
            if (p && p->published && p->pid > last && (!next || p->pid < next->pid))
                { next = p; slot = i; }
        }
        if (!next)
            break;
        last = next->pid;
        present++;
        if (written == capacity)
            continue;
        struct ciuki_proc_record *r = &out[written++];
        memset(r, 0, sizeof(*r));
        r->version = CIUKI_PROC_VERSION;
        r->size = sizeof(*r);
        r->pid = next->pid;
        r->ppid = next == &supervisor ? 0 : next->ppid;
        r->pgid = next->pgid;
        r->state = next->state == PROC_ZOMBIE ? CIUKI_PROC_ZOMBIE :
                   next->state == PROC_STOPPING ? CIUKI_PROC_STOPPING : CIUKI_PROC_LIVE;
        /* PID 1 is the kernel reaper task; native processes count live threads. */
        r->threads = next == &supervisor ? 1 : next->live_threads;
        r->resident_pages = next->memory ? next->memory->backing : 0;
        r->cpu_ticks = ticks[slot];
        r->start_tick = next->start_tick;
        memcpy(r->name, next->name, sizeof(r->name));
        r->name[sizeof(r->name) - 1] = 0;
    }
    if (total)
        *total = present;
    return written;
}

void proc_publish(struct process *p, uint32_t flags)
{
    if (flags & CIUKI_SPAWN_NEW_GROUP)
        p->pgid = p->pid;
    p->start_tick = g_ticks;
    p->state = PROC_LIVE;
    p->published = true;
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->process == p && t->task)
            task_start(t->task);
    }
}

void proc_notify(void)
{
    uint32_t flags = irq_save();
    collect_pending = true;
    if (supervisor_task && supervisor_task->state == T_BLOCKED)
        task_start(supervisor_task);
    irq_restore(flags);
}

void proc_interrupt(struct proc_thread *t)
{
    if (t->in_handler)
        return;                         /* f2-04 calls only for eligible catchers */
    uint32_t flags = irq_save();
    t->interrupted = true;
    ww_interrupt(&t->word_wait, -EINTR);
    /* A native poll waiter tests the shared sequence inside kwait. Merely
     * making it runnable would leave that predicate false on resumption. */
    if (net_fd_changed) net_fd_changed(0, 0);
    if (t->task && t->task->state == T_BLOCKED) {
        t->task->wake_tick = 0;
        task_start(t->task);
    }
    irq_restore(flags);
}

void proc_stop(struct process *p, int raw_code, uint32_t signal)
{
    if (p == &supervisor || p->state == PROC_STOPPING || p->state == PROC_ZOMBIE)
        return;
    if(w32_process_stopping)w32_process_stopping(p,signal);
    p->state = PROC_STOPPING;            /* no new syscall/creation can publish */
    if (net_fd_changed) net_fd_changed(0, 0);
    p->raw_exit = raw_code;
    p->status = signal ? (int32_t)(signal & 0x7f) : (raw_code & 0xff) << 8;
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->process == p && t->task && t->task != g_current) {
            if (t->in_syscall && task_alive(t->task)) {
                /* A sleeping kernel invocation may own a device request or
                 * mutex. Let it unwind/drain; the trap boundary cannot return
                 * it to user mode once STOPPING is set. Never destroy a held
                 * kernel mutex by killing its owner's suspended C stack. */
                ww_interrupt(&t->word_wait, -ECANCELED);
                if (t->task->state == T_BLOCKED) {
                    t->task->wake_tick = 0;
                    task_start(t->task);
                }
            } else {
                task_kill(t->task, raw_code);
            }
        }
    }
    proc_notify();
}

bool proc_task_exiting(struct task *task, int code)
{
    struct proc_thread *t = proc_thread_for(task);
    if (!t)
        return false;
    struct process *p = t->process;
    if (!t->exiting && p->state == PROC_LIVE) {
        uint32_t sig = 0;
        if (task->fault_vector) {
            uint32_t v = task->fault_vector;
            sig = v == 6 ? SIGILL : (v == 0 || v == 16 || v == 19) ? SIGFPE :
                  v == 17 ? SIGBUS : SIGSEGV;
            p->fault_vector = v;
            p->fault_error = task->fault_err;
            p->fault_eip = task->fault_eip;
            p->fault_address = task->fault_cr2;
        }
        proc_stop(p, code, sig);
    }
    if (!t->stopped) {
        if(!t->exiting)t->value = (uint32_t)code;
        if(w32_thread_died)w32_thread_died(t);
        t->stopped = t->exiting = true;
        p->live_threads--;
        /* Cancellation cannot requeue a victim already removed by task_kill. */
        t->word_wait.task = 0;
        ww_interrupt(&t->word_wait, -ECANCELED);
        task->wake_tick = 0;
    }
    if (!p->live_threads && p->state == PROC_LIVE)
        proc_stop(p, 0, 0);
    proc_notify();
    return true;                        /* FPU release belongs to deferred teardown */
}

void disk_process_exit(struct process *) __attribute__((weak));
void net_resolve_process_exit(struct process *) __attribute__((weak));
static void close_resources(struct process *p)
{
    if(w32_process_cleanup)w32_process_cleanup(p);
    /* f9-02: death revokes a destination-disk lease (no disk I/O). */
    if (disk_process_exit) disk_process_exit(p);
    if (net_resolve_process_exit) net_resolve_process_exit(p);
    if (p->fds) {
        for (unsigned i = 0; i < CIUKI_OPEN_MAX; i++) {
            struct proc_object *o = p->fds[i].object;
            p->fds[i].object = 0;        /* detach before potentially blocking release */
            if (o)
                o->release(o);
        }
        kfree(p->fds);
        p->fds = 0;
    }
    if (p->cwd) {
        void *cwd = p->cwd;
        p->cwd = 0;
        if (p->cwd_release)
            p->cwd_release(cwd);
    }
    kfree(p->actions);
    p->actions = 0;
}

static void release_threads(struct process *p)
{
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->process == p) {
            if (t->task) {
                ua_unpin(p->memory, &t->tls_pin);
                ua_cancel_pins(p->memory, t->task);
                fpu_task_release(t->task);
            }
        }
    }
    if (p->memory)
        ua_destroy(p->memory);
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->process == p)
            proc_thread_discard(t);
    }
    kfree(p->memory);
    p->memory = 0;
}

void proc_discard(struct process *p)
{
    p->state = PROC_STOPPING;
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->process == p && t->task)
            task_kill(t->task, 0);
    }
    close_resources(p);
    release_threads(p);
    processes[p->slot] = 0;
    kfree(p);
}

int proc_wait_select(struct process *parent, int32_t pid, struct process **out)
{
    if (pid == 0 || pid < -1)
        return -EINVAL;
    bool child = false;
    *out = 0;
    for (unsigned i = 1; i < CIUKI_PROCESS_MAX; i++) {
        struct process *p = processes[i];
        if (!p || !p->published || p->ppid != parent->pid || (pid != -1 && (uint32_t)pid != p->pid))
            continue;
        child = true;
        if (p->state == PROC_ZOMBIE && (!*out || p->pid < (*out)->pid))
            *out = p;
    }
    return !child ? -ECHILD : *out ? (int)(*out)->pid : 0;
}

int proc_reap(struct process *parent, struct process *child)
{
    /* Membership test precedes dereference: double-reap refuses stale pointers. */
    unsigned slot = 1;
    while (slot < CIUKI_PROCESS_MAX && processes[slot] != child)
        slot++;
    if (slot == CIUKI_PROCESS_MAX || !child || child->ppid != parent->pid ||
        child->state != PROC_ZOMBIE || !child->published)
        return -ECHILD;
    processes[slot] = 0;
    kfree(child);
    return 0;
}

void proc_collect(void)
{
    if (collecting) {
        proc_notify();
        return;
    }
    collecting = true;
    for (unsigned i = 1; i < CIUKI_PROCESS_MAX; i++) {
        struct process *p = processes[i];
        if (!p || !p->published)
            continue;
        if (p->state == PROC_STOPPING) {
            bool active = false;
            for (unsigned j = 0; j < CIUKI_THREAD_MAX; j++) {
                struct proc_thread *t = proc_thread_slot(j);
                if (t && t->process == p && t->task &&
                    (t->task == g_current || task_alive(t->task)))
                    active = true;
            }
            if (active)
                continue;
            for (unsigned j = 0; j < CIUKI_THREAD_MAX; j++) {
                struct proc_thread *t = proc_thread_slot(j);
                if (t && t->process == p)
                    proc_thread_cleanup(t);
            }
            close_resources(p);
            release_threads(p);
            /* Adopt both live children and zombies, before publishing exit. */
            for (unsigned j = 1; j < CIUKI_PROCESS_MAX; j++)
                if (processes[j] && processes[j]->ppid == p->pid)
                    processes[j]->ppid = supervisor.pid;
            p->state = PROC_ZOMBIE;
            struct process *parent = proc_find(p->ppid);
            if (parent) {
                proc_signal_child(parent, p->pid);
                kwait_wake_all(&parent->changed);
            }
        }
    }
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->process->state == PROC_LIVE && t->stopped && !t->retained)
            proc_thread_collect(t);
    }
    for (unsigned i = 1; i < CIUKI_PROCESS_MAX; i++) {
        struct process *p = processes[i];
        if (!p || p->state != PROC_ZOMBIE)
            continue;
        struct process *parent = proc_find(p->ppid);
        if (parent == &supervisor || (parent && parent->actions && parent->actions[SIGCHLD].handler == SIG_IGN))
            proc_reap(parent, p);
    }
    collecting = false;
}

int proc_waitpid(int32_t pid, uint32_t status_va, uint32_t options)
{
    if (pid == 0 || pid < -1 || (options & ~WNOHANG))
        return -EINVAL;
    struct proc_thread *t = proc_thread_for(g_current);
    struct process *p = t->process;
    struct ua_pin pin;
    int err = status_va ? ua_pin(p->memory, &pin, status_va, sizeof(int32_t), true) : 0;
    if (err)
        return err;
    for (;;) {
        struct process *child;
        err = proc_wait_select(p, pid, &child);
        if (err > 0) {
            int copy = status_va ? copy_to_user(status_va, &child->status, sizeof(child->status)) : 0;
            if (copy) err = copy;
            else proc_reap(p, child);
            break;
        }
        if (err < 0 || (options & WNOHANG))
            break;
        if (p->state == PROC_STOPPING || proc_signal_caught(t)) {
            err = -EINTR;
            break;
        }
        task_sleep_ms(1);              /* predicate rechecked after every wake */
    }
    if (status_va)
        ua_unpin(p->memory, &pin);
    return err;
}

extern void arch_w32_fs_switch(uint32_t base) __attribute__((weak));
void proc_task_switch(struct task *next)
{
    struct proc_thread *t = proc_thread_for(next);
    arch_tls_switch(t ? t->tls : 0, t && t->process->personality == PROC_NATIVE);
    if (arch_w32_fs_switch) arch_w32_fs_switch(t ? t->teb : 0);
}

static void supervisor_main(void *arg)
{
    (void)arg;
    for (;;) {
        supervisor_poll();
        proc_collect();
        uint32_t flags = irq_save();
        if (collect_pending) {
            collect_pending = false;
        } else {
            g_current->state = T_BLOCKED;
            irq_restore(flags);
            schedule();
            continue;
        }
        irq_restore(flags);
    }
}

void proc_init(void)
{
    supervisor.pid = supervisor.ppid = supervisor.pgid = 1;
    proc_set_name(&supervisor, "supervisor");
    supervisor.state = PROC_LIVE;
    supervisor.published = true;
    kwait_init(&supervisor.changed);
    processes[0] = &supervisor;
    supervisor_task = task_create_kernel("pid1-reaper", supervisor_main, 0, P_NORMAL);
    if (!supervisor_task)
        panic("proc: cannot create PID 1 supervisor");
    task_start(supervisor_task);
}

extern void w32_shared_update(struct process *) __attribute__((weak));
void w32_shared_tick(void)
{
    if (!w32_shared_update) return;
    for (unsigned i = 1; i < CIUKI_PROCESS_MAX; i++) {
        struct process *p = processes[i];
        if (p && p->state == PROC_LIVE && p->personality == PROC_WIN32) w32_shared_update(p);
    }
}
