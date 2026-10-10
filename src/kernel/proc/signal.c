/* Standard coalesced signals in the non-preemptible UP kernel. No handler
 * executes here: delivery only rewrites a user return frame after kernel work.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/signal.h>

/* Bounded sidecars use monotonically allocated IDs to reject slot reuse.
 * They own no allocations or user references and require no teardown order. */
static struct { uint32_t pid, sender[SIGCHLD + 1]; } signal_processes[CIUKI_PROCESS_MAX];
static struct {
    uint32_t tid, sender[SIGCHLD + 1];
    struct sig_active active;
} signal_threads[CIUKI_THREAD_MAX];
static uint64_t delivery_token;

static uint32_t *process_senders(struct process *p)
{
    if (signal_processes[p->slot].pid != p->pid) {
        memset(&signal_processes[p->slot], 0, sizeof(signal_processes[p->slot]));
        signal_processes[p->slot].pid = p->pid;
    }
    return signal_processes[p->slot].sender;
}

static uint32_t *thread_senders(struct proc_thread *t)
{
    if (signal_threads[t->slot].tid != t->tid) {
        memset(&signal_threads[t->slot], 0, sizeof(signal_threads[t->slot]));
        signal_threads[t->slot].tid = t->tid;
    }
    return signal_threads[t->slot].sender;
}

bool proc_signal_valid(uint32_t signo)
{
    return signo && signo <= SIGCHLD && (PROC_SIGNAL_SET & CIUKI_SIGBIT(signo));
}

static bool signal_live(const struct proc_thread *t)
{
    return t && t->process->state == PROC_LIVE && !t->stopped && !t->exiting &&
           t->task && task_alive(t->task);
}

static bool ignored(const struct process *p, uint32_t signo)
{
    uint32_t handler = p->actions[signo].handler;
    return signo != SIGKILL && (handler == SIG_IGN || (handler == SIG_DFL && signo == SIGCHLD));
}

bool proc_signal_eligible(const struct proc_thread *t, uint32_t signo)
{
    if (!signal_live(t) || !proc_signal_valid(signo))
        return false;
    if (signo == SIGKILL)
        return true;
    if ((t->mask & CIUKI_SIGBIT(signo)) || ignored(t->process, signo))
        return false;
    return t->process->actions[signo].handler == SIG_DFL || !t->in_handler;
}

static struct proc_thread *process_target(struct process *p, uint32_t signo)
{
    struct proc_thread *chosen = 0;
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (t && t->process == p && proc_signal_eligible(t, signo) &&
            (!chosen || t->tid < chosen->tid))
            chosen = t;
    }
    return chosen;
}

uint32_t proc_signal_next(struct proc_thread *t, bool *process_pending)
{
    if (!signal_live(t))
        return 0;
    for (unsigned n = 0; n <= SIGCHLD; n++) {
        uint32_t signo = n ? n : SIGKILL;
        if (!proc_signal_eligible(t, signo))
            continue;
        uint64_t bit = CIUKI_SIGBIT(signo);
        if (t->pending & bit) {
            *process_pending = false;
            return signo;
        }
        if ((t->process->pending & bit) && process_target(t->process, signo) == t) {
            *process_pending = true;
            return signo;
        }
    }
    return 0;
}

bool proc_signal_caught(struct proc_thread *t)
{
    if (!t || t->in_handler)
        return false;
    bool from_process = false;
    uint32_t signo = proc_signal_next(t, &from_process);
    return signo && signo != SIGKILL && t->process->actions[signo].handler > SIG_IGN;
}

void proc_signal_refresh(struct process *p)
{
    if (!p || !p->actions || p->state != PROC_LIVE)
        return;
    uint64_t discard = 0;
    for (unsigned signo = 1; signo <= SIGCHLD; signo++)
        if (proc_signal_valid(signo) && ignored(p, signo))
            discard |= CIUKI_SIGBIT(signo);
    p->pending &= ~discard;
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (!t || t->process != p)
            continue;
        t->pending &= ~discard;
        bool from_process = false;
        uint32_t signo = proc_signal_next(t, &from_process);
        t->interrupted = false;
        if (!signo)
            continue;
        if (signo == SIGKILL || p->actions[signo].handler == SIG_DFL) {
            proc_stop(p, 0, signo);     /* sleeping invocations unwind/quiesce */
            return;
        }
        proc_interrupt(t);             /* only a currently eligible catcher */
    }
}

void proc_signal_post(struct process *p, struct proc_thread *t, uint32_t signo, uint32_t sender)
{
    if (!p || p->state != PROC_LIVE || !p->actions || !proc_signal_valid(signo) ||
        (t && (t->process != p || !signal_live(t))) || ignored(p, signo))
        return;
    uint64_t bit = CIUKI_SIGBIT(signo);
    uint64_t *pending = t ? &t->pending : &p->pending;
    uint32_t *senders = t ? thread_senders(t) : process_senders(p);
    if (!(*pending & bit))
        senders[signo] = sender;        /* coalescing keeps the first sender */
    *pending |= bit;
    proc_signal_refresh(p);
}

void proc_signal_child(struct process *parent, uint32_t child_pid)
{
    proc_signal_post(parent, 0, SIGCHLD, child_pid);
}

void proc_signal_pipe(struct proc_thread *t)
{
    proc_signal_post(t->process, t, SIGPIPE, 0);
}

int proc_signal_thread_kill(struct process *caller, uint32_t tid, uint32_t signo)
{
    if (signo && !proc_signal_valid(signo))
        return -EINVAL;
    struct proc_thread *t = proc_thread_find(caller, tid);
    if (!signal_live(t))
        return -ESRCH;
    if (signo)
        proc_signal_post(caller, t, signo, caller->pid);
    return 0;
}

int proc_signal_kill(struct process *caller, int32_t pid, uint32_t signo)
{
    if ((signo && !proc_signal_valid(signo)) || pid == -1)
        return -EINVAL;
    if (pid == 1)
        return -EPERM;
    if (pid > 0) {
        struct process *p = proc_find((uint32_t)pid);
        if (!p)
            return -ESRCH;
        if (p->pgid != caller->pgid)
            return -EPERM;
        if (p->state != PROC_LIVE)
            return -ESRCH;
        if (signo)
            proc_signal_post(p, 0, signo, caller->pid);
        return 0;
    }
    if (pid < 0 && (uint32_t)(-(int64_t)pid) != caller->pgid)
        return -EPERM;
    /* Snapshot identities first: posting a fatal signal may stop all of a
     * process's threads. No freed pointer or changed loop membership is used. */
    struct process *targets[CIUKI_PROCESS_MAX];
    unsigned count = 0;
    for (unsigned i = 0; i < CIUKI_THREAD_MAX; i++) {
        struct proc_thread *t = proc_thread_slot(i);
        if (!signal_live(t) || t->process->pid == 1 || t->process->pgid != caller->pgid)
            continue;
        unsigned j = 0;
        while (j < count && targets[j] != t->process)
            j++;
        if (j == count)
            targets[count++] = t->process;
    }
    for (unsigned i = 0; signo && i < count; i++)
        proc_signal_post(targets[i], 0, signo, caller->pid);
    return count ? 0 : -ESRCH;
}

enum signal_wait_decision proc_signal_decide(bool caught, enum signal_wait_class kind,
                                            bool committed, bool issued, uint32_t progress)
{
    if (!caught || progress || kind == SIGNAL_WAIT_DEFER || (kind == SIGNAL_WAIT_D && (committed || issued)))
        return SIGNAL_WAIT_CONTINUE;
    return kind == SIGNAL_WAIT_I && issued ? SIGNAL_WAIT_DRAIN : SIGNAL_WAIT_EINTR;
}

enum signal_wait_decision proc_signal_wait(struct proc_thread *t, enum signal_wait_class kind,
                                          bool committed, bool issued, uint32_t progress)
{
    return proc_signal_decide(proc_signal_caught(t), kind, committed, issued, progress);
}

int32_t proc_signal_read_result(uint32_t progress, bool interrupted, bool drain_ok, int32_t result)
{
    return progress ? (int32_t)progress : interrupted ? (drain_ok ? -EINTR : -EIO) : result;
}

void proc_signal_fault_info(const struct trap_frame *tf, uint32_t cr2, struct ciuki_siginfo *info)
{
    *info = (struct ciuki_siginfo){ .signo = SIGSEGV, .vector = tf->vector,
        .trap_error = tf->err, .fault_addr = tf->eip, .code = SEGV_ACCERR };
    switch (tf->vector) {
    case 0: info->signo = SIGFPE; info->code = FPE_INTDIV; break;
    case 6: info->signo = SIGILL; info->code = ILL_ILLOPC; break;
    case 14: info->fault_addr = cr2; info->code = tf->err & 1 ? SEGV_ACCERR : SEGV_MAPERR; break;
    case 16: info->signo = SIGFPE; info->code = CIUKI_SI_X87; break;
    case 17: info->signo = SIGBUS; info->code = BUS_ADRALN; info->fault_addr = 0; break;
    default: break;
    }
}

void proc_signal_die(struct proc_thread *t, uint32_t signo)
{
    proc_stop(t->process, t->process->raw_exit, signo);
    task_exit(t->process->raw_exit);
}

static void enter_handler(struct proc_thread *t, struct trap_frame *tf, const struct ciuki_siginfo *info)
{
    (void)thread_senders(t);
    struct sig_active *a = &signal_threads[t->slot].active;
    struct ciuki_sigaction action = t->process->actions[info->signo];
    /* Fail closed on token exhaustion; never make an old active frame valid. */
    if (delivery_token == UINT64_MAX)
        proc_signal_die(t, SIGSEGV);
    struct ciuki_signal_frame frame;
    uint32_t address;
    if (sigframe_build(t, tf, &action, info, ++delivery_token, &frame, &address) ||
        copy_to_user(address, &frame, sizeof(frame)))
        proc_signal_die(t, SIGSEGV);
    *a = (struct sig_active){ .tid = t->tid, .address = address, .tls = t->tls, .original = frame };
    t->in_handler = true;
    t->interrupted = false;
    t->mask = (t->mask | action.mask | CIUKI_SIGBIT(info->signo)) & ~CIUKI_SIGBIT(SIGKILL);
    fpu_signal_reset(t->task);
    tf->eip = action.handler;
    tf->user_esp = address;
    tf->eflags &= ~((1u << 8) | (1u << 10) | (1u << 18)); /* TF DF AC */
    /* cdecl arguments live at frame+4/+8/+12. General registers are saved
     * untouched in the context; no register-argument ABI is invented. */
    proc_signal_refresh(t->process);
    if (t->process->state == PROC_STOPPING)
        task_exit(t->process->raw_exit);
}

void proc_signal_fault(struct trap_frame *tf, uint32_t cr2)
{
    struct proc_thread *t = proc_thread_for(g_current);
    struct ciuki_siginfo info;
    proc_signal_fault_info(tf, cr2, &info);
    struct process *p = t->process;
    p->fault_vector = tf->vector;
    p->fault_error = tf->err;
    p->fault_eip = tf->eip;
    p->fault_address = info.fault_addr;
    p->raw_exit = EXIT_FAULT_BASE + (int)tf->vector;
    if (p->state != PROC_LIVE || t->in_handler || (t->mask & CIUKI_SIGBIT(info.signo)) ||
        p->actions[info.signo].handler <= SIG_IGN)
        proc_signal_die(t, (uint32_t)info.signo);
    enter_handler(t, tf, &info);
}

void proc_signal_return_to_user(struct trap_frame *tf)
{
    if ((tf->cs & 3) != 3)
        return;
    struct proc_thread *t = proc_thread_for(g_current);
    if (!t)
        return;                        /* F0 */
    proc_signal_refresh(t->process);
    if (t->process->state == PROC_STOPPING)
        task_exit(t->process->raw_exit);
    bool from_process = false;
    uint32_t signo = proc_signal_next(t, &from_process);
    if (!signo)
        return;
    if (signo == SIGKILL || t->process->actions[signo].handler == SIG_DFL)
        proc_signal_die(t, signo);
    struct ciuki_siginfo info = { .signo = (int32_t)signo,
        .sender_pid = (int32_t)(from_process ? process_senders(t->process)[signo] : thread_senders(t)[signo]) };
    if (from_process)
        t->process->pending &= ~CIUKI_SIGBIT(signo);
    else
        t->pending &= ~CIUKI_SIGBIT(signo);
    enter_handler(t, tf, &info);
}

void proc_signal_sigreturn(struct trap_frame *tf, uint32_t address)
{
    struct proc_thread *t = proc_thread_for(g_current);
    (void)thread_senders(t);
    struct sig_active *a = &signal_threads[t->slot].active;
    struct ciuki_signal_frame frame;
    struct trap_frame restored;
    if (copy_from_user(&frame, address, sizeof(frame)) || sigframe_validate(t, a, address, &frame, &restored))
        proc_signal_die(t, SIGSEGV);
    fpu_signal_import(t->task, frame.context.fp_state);
    t->mask = frame.context.mask;
    t->in_handler = false;
    t->interrupted = false;
    memset(a, 0, sizeof(*a));
    *tf = restored;                     /* includes EAX: never ordinary return */
    proc_signal_refresh(t->process);
}
