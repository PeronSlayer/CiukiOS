/* Native syscall rows owned by f2-02. Wire records/constants come from abi.h.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/process.h>

extern char copy_user_fault_start[], copy_user_fault_end[], copy_user_fixup[];
static const struct { const char *start, *end, *fixup; } copy_fixups[] = {
    { copy_user_fault_start, copy_user_fault_end, copy_user_fixup }
};

void proc_trap_dispatch(struct trap_frame *tf)
{
    /* The complete frame has already been saved by isr_common. Match only a
     * kernel #PF at the copy instruction; every other fault follows F0's
     * unchanged expected-fault/panic/user-fault dispatcher. */
    if (tf->vector == 14 && !(tf->cs & 3))
        for (unsigned i = 0; i < ARRAY_SIZE(copy_fixups); i++)
            if (tf->eip >= (uint32_t)(uintptr_t)copy_fixups[i].start &&
                tf->eip < (uint32_t)(uintptr_t)copy_fixups[i].end) {
                tf->eip = (uint32_t)(uintptr_t)copy_fixups[i].fixup;
                return;
            }
    struct proc_thread *thread = tf->vector == 0x80 && (tf->cs & 3) == 3 ? proc_thread_for(g_current) : 0;
    if (thread)
        thread->in_syscall = true;
    trap_dispatch(tf);
    if (thread) {
        thread->in_syscall = false;
        if (thread->process->state == PROC_STOPPING)
            task_exit(thread->process->raw_exit);
    }
}

int32_t proc_syscall(struct trap_frame *tf)
{
    struct process *p = proc_current();
    if (!p)
        return -ENOSYS;                 /* F0 tasks retain their frozen interface */
    switch (tf->eax) {
    case CIUKI_SYS_SPAWN: return proc_spawn(tf->ebx);
    case CIUKI_SYS_WAITPID: return proc_waitpid((int32_t)tf->ebx, tf->ecx, tf->edx);
    case CIUKI_SYS_GETPID: return (int32_t)p->pid;
    case CIUKI_SYS_GETPPID: return (int32_t)p->ppid;
    case CIUKI_SYS_THREAD_CREATE: return proc_thread_create(tf->ebx);
    case CIUKI_SYS_THREAD_EXIT: proc_thread_exit(tf->ebx);
    case CIUKI_SYS_TLS_SET: return proc_tls_set(tf->ebx, tf->ecx);
    case CIUKI_SYS_WAIT_WORD: return proc_wait_word(tf->ebx, tf->ecx, tf->edx, tf->esi);
    case CIUKI_SYS_WAKE_WORD: return proc_wake_word(tf->ebx, tf->ecx);
    case CIUKI_SYS_MMAP: {
        struct ciuki_mmap_args a;
        int err = copy_from_user(&a, tf->ebx, sizeof(a));
        return err ? err : ua_mmap(p->memory, &a);
    }
    case CIUKI_SYS_MUNMAP: return ua_munmap(p->memory, tf->ebx, tf->ecx);
    case CIUKI_SYS_MPROTECT: return ua_mprotect(p->memory, tf->ebx, tf->ecx, tf->edx);
    case CIUKI_SYS_BRK: return ua_brk(p->memory, tf->ebx);
    case CIUKI_SYS_THREAD_JOIN: return proc_thread_join(tf->ebx, tf->ecx);
    case CIUKI_SYS_THREAD_DETACH: return proc_thread_detach(tf->ebx);
    default: return -ENOSYS;
    }
}
