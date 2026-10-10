/* Signal syscall validation. No waits between the single input snapshot,
 * output validation/copy and commit in this non-preemptible kernel.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/signal.h>

int proc_sigaction(uint32_t signo, uint32_t new_va, uint32_t old_va)
{
    struct process *p = proc_current();
    if (!proc_signal_valid(signo) || (new_va && signo == SIGKILL))
        return -EINVAL;
    struct ciuki_sigaction action;
    if (new_va) {
        int err = copy_from_user(&action, new_va, sizeof(action));
        if (err)
            return err;
        if (action.reserved || (action.flags & ~SA_SIGINFO) || (action.mask & ~PROC_SIGNAL_SET))
            return -EINVAL;
        if (action.handler > SIG_IGN &&
            (!sigframe_executable(p->memory, action.handler, true) ||
             !sigframe_executable(p->memory, action.restorer, true)))
            return -EINVAL;
        action.mask &= ~CIUKI_SIGBIT(SIGKILL);
        if (action.handler <= SIG_IGN)
            action.restorer = 0;
    }
    if (old_va && copy_to_user(old_va, &p->actions[signo], sizeof(action)))
        return -EFAULT;
    if (new_va) {
        p->actions[signo] = action;
        proc_signal_refresh(p);
        if (signo == SIGCHLD && action.handler == SIG_IGN)
            proc_notify();              /* reap already exited children too */
    }
    return 0;
}

int proc_sigprocmask(uint32_t how, uint32_t set_va, uint32_t old_va)
{
    struct proc_thread *t = proc_thread_for(g_current);
    uint64_t mask = t->mask, set;
    if (set_va) {
        if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK)
            return -EINVAL;
        int err = copy_from_user(&set, set_va, sizeof(set));
        if (err)
            return err;
        if (set & ~PROC_SIGNAL_SET)
            return -EINVAL;
        mask = how == SIG_BLOCK ? mask | set : how == SIG_UNBLOCK ? mask & ~set : set;
        mask &= ~CIUKI_SIGBIT(SIGKILL);
    }
    if (old_va && copy_to_user(old_va, &t->mask, sizeof(t->mask)))
        return -EFAULT;
    if (set_va) {
        t->mask = mask;
        proc_signal_refresh(t->process);
    }
    return 0;
}

/* Intercept before the ordinary dispatcher so successful sigreturn cannot
 * be overwritten by syscall_dispatch's final EAX assignment. */
bool proc_signal_syscall(struct trap_frame *tf)
{
    struct process *p = proc_current();
    if (!p)
        return false;
    int32_t result;
    switch (tf->eax) {
    case CIUKI_SYS_SIGACTION: result = proc_sigaction(tf->ebx, tf->ecx, tf->edx); break;
    case CIUKI_SYS_SIGPROCMASK: result = proc_sigprocmask(tf->ebx, tf->ecx, tf->edx); break;
    case CIUKI_SYS_KILL: result = proc_signal_kill(p, (int32_t)tf->ebx, tf->ecx); break;
    case CIUKI_SYS_THREAD_KILL: result = proc_signal_thread_kill(p, tf->ebx, tf->ecx); break;
    case CIUKI_SYS_SIGRETURN:
        proc_signal_sigreturn(tf, tf->ebx);
        return true;
    default: return false;
    }
    tf->eax = (uint32_t)result;
    return true;
}
