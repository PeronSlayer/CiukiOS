/* One multiplexed call; copies and pins are complete before effects.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/win32.h>
static uint32_t create_thread(struct proc_thread *self, struct ciuki_w32_thread *a)
{
    struct process *p = self->process;
    if (!ua_range(p->memory, a->entry, 1, PROT_READ | PROT_EXEC) ||
        !ua_range(p->memory, a->trampoline, 1, PROT_READ | PROT_EXEC)) return W32_STATUS_INVALID_PARAMETER;
    if (a->stack_bytes > CIUKI_W32_APP_LIMIT - CIUKI_W32_LOW - 2 * PAGE_SIZE) return W32_STATUS_INVALID_PARAMETER;
    /* Reserve overrides are per creation, never mutate process defaults across
     * an allocation which can yield. Passed in proc_thread.w32_reserve. */
    struct proc_thread *t;
    int err = proc_thread_prepare(p, a->entry, a->argument, a->trampoline,
        a->stack_bytes, CIUKI_THREAD_DETACHED | a->flags, self->mask, false, &t);
    if (err) return w32_status(err);
    if (p->state != PROC_LIVE) { proc_thread_discard(t); return W32_STATUS_ACCESS_DENIED; }
    a->tid = t->tid; task_start(t->task);
    return 0;
}
int32_t w32_syscall(struct trap_frame *tf)
{
    struct proc_thread *self = proc_thread_for(g_current);
    if (!self || self->process->personality != PROC_WIN32 || self->process->state != PROC_LIVE) {
        tf->eax = (uint32_t)-EPERM; return -EPERM;
    }
    if (tf->edx || tf->esi || tf->edi || tf->ebp || tf->ecx < 16 || tf->ecx > 240) {
        tf->eax = (uint32_t)-EINVAL; return -EINVAL;
    }
    struct uaddr *u = self->process->memory;
    uint32_t record[60];
    uint32_t va = tf->ebx, bytes = tf->ecx;
    struct ua_pin pin;
    int err = ua_pin(u, &pin, va, bytes, true);
    if (err) { tf->eax = (uint32_t)err; return err; }
    err = copy_from_user(record, va, bytes);
    if (!err) err = w32_record_validate(record, bytes, true);
    uint32_t status = 0;
    bool replaced = false;
    if (!err) {
        struct ciuki_w32_header *h = (void *)record;
        switch (h->operation) {
        case W32_IDENTITY: w32_identity((void *)record); break;
        case W32_COUNTER: {
            struct ciuki_w32_counter *a = (void *)record;
            uint64_t count = arch_w32_counter();
            a->value_lo = (uint32_t)count; a->value_hi = (uint32_t)(count >> 32);
            a->frequency_lo = W32_PIT_HZ; a->frequency_hi = 0; break;
        }
        case W32_VM_ALLOC: status = w32_vm_alloc(u, (void *)record); break;
        case W32_VM_FREE: status = w32_vm_free(u, (void *)record); break;
        case W32_VM_PROTECT: status = w32_vm_protect(u, (void *)record); break;
        case W32_VM_QUERY: {
            struct ciuki_w32_query *a = (void *)record;
            status = w32_vm_query(u, a->address, &a->info); break;
        }
        case W32_DISPATCHER: {
            uint32_t entry = ((struct ciuki_w32_dispatcher *)record)->entry;
            if (entry < CIUKI_W32_APP_LIMIT || entry >= CIUKI_W32_BUILTIN_LIMIT ||
                !ua_range(u, entry, 1, PROT_READ | PROT_EXEC)) status = W32_STATUS_INVALID_PARAMETER;
            else self->process->w32_dispatcher = entry;
            break;
        }
        case W32_CONTINUE: {
            struct ciuki_w32_context c;
            err = copy_from_user(&c, ((struct ciuki_w32_continue *)record)->context, sizeof(c));
            /* Release the caller buffer before returning to arbitrary restored
             * code; continue is the final effect and cannot overwrite EAX. */
            if (!err) { ua_unpin(u, &pin); w32_continue(self, tf, &c); return 0; }
            break;
        }
        case W32_RAISE: {
            const struct ciuki_w32_raise *a = (void *)record;
            struct ciuki_w32_exception e; struct ciuki_w32_context c;
            err = copy_from_user(&e, a->exception, sizeof(e));
            if (!err) err = copy_from_user(&c, a->context, sizeof(c));
            if (!err && (e.count > 15 || e.flags & ~1u || e.chain ||
                !w32_context_validate(&c, u, g_cpu_fxsr, fpu_mxcsr_mask()))) status = W32_STATUS_INVALID_PARAMETER;
            if (!err && !status) { w32_deliver(self, tf, &e, &c); replaced = true; }
            break;
        }
        case W32_THREAD: status = create_thread(self, (void *)record); break;
        case W32_DIAGNOSTIC: {
            struct ciuki_w32_diagnostic *a = (void *)record;
            a->pid = self->process->pid; a->tid = self->tid;
            /* Separate payload keeps the existing 256-byte klog sink bounded. */
            klog("[win32] diagnostic kind=%u pid=%u tid=%u status=%08x", a->kind, a->pid, a->tid, a->status);
            klog("[win32] %s", (const char *)a->payload); break;
        }
        default: err = -ENOSYS; break;
        }
    }
    if (!err && !status) err = copy_to_user(va, record, bytes);
    ua_unpin(u, &pin);
    int32_t result = err ? err : (int32_t)status;
    if (!replaced) tf->eax = (uint32_t)result;
    return result;
}
