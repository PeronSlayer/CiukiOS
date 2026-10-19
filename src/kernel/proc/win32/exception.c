/* Dedicated Windows delivery: no native signal-handler state.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/w32_objects.h>
#include <ciuki/signal.h>
struct w32_frame {
    uint32_t return_address, exception_address, context_address;
    struct ciuki_w32_exception exception;
    struct ciuki_w32_context context;
};
static __attribute__((noreturn)) void crash(struct proc_thread *t, struct trap_frame *tf, uint32_t status)
{
    t->task->fault_vector = tf->vector; t->task->fault_err = tf->err;
    t->task->fault_eip = tf->eip;
    t->process->fault_vector = tf->vector; t->process->fault_error = tf->err;
    t->process->fault_eip = tf->eip; t->process->fault_address = t->task->fault_cr2;
    klog("[win32] crash pid=%u tid=%u status=%08x vector=%u eip=%08x", t->process->pid, t->tid, status, tf->vector, tf->eip);
    proc_stop(t->process, (int)status, 0);
    task_exit((int)status);
}
void w32_deliver(struct proc_thread *t, struct trap_frame *tf, const struct ciuki_w32_exception *exc,
                 const struct ciuki_w32_context *ctx)
{
    struct process *p = t->process;
    if (!p->w32_dispatcher || !ua_range(p->memory, p->w32_dispatcher, 1, PROT_READ | PROT_EXEC))
        crash(t, tf, exc->code);
    uint32_t sp = tf->user_esp >= sizeof(struct w32_frame) ?
        (tf->user_esp - sizeof(struct w32_frame)) & ~15u : 0;
    /* Stack probing may need several guard transitions for a large frame. */
    while (!ua_range(p->memory, sp, sizeof(struct w32_frame), PROT_READ | PROT_WRITE) &&
           t->stack_guard && sp >= t->stack_base + 2 * PAGE_SIZE && sp <= t->stack_guard &&
           tf->user_esp <= t->stack_base + t->stack_bytes) {
        bool retry;
        if (w32_guard_fault(t, t->stack_guard, &retry) || !retry) break;
    }
    bool emergency = exc->code == W32_EXCEPTION_STACK ||
        !ua_range(p->memory, sp, sizeof(struct w32_frame), PROT_READ | PROT_WRITE);
    if (emergency) {
        for (struct w32_delivery *d = t->w32_delivery; d; d = d->next)
            if (d->emergency) crash(t, tf, W32_EXCEPTION_STACK);
        sp = (t->emergency + 2 * PAGE_SIZE - sizeof(struct w32_frame)) & ~15u;
        if (!t->emergency || !ua_range(p->memory, sp, sizeof(struct w32_frame), PROT_READ | PROT_WRITE))
            crash(t, tf, W32_EXCEPTION_STACK);
    }
    struct w32_frame frame = {0};
    frame.exception = *exc; frame.context = *ctx;
    frame.exception_address = sp + offsetof(struct w32_frame, exception);
    frame.context_address = sp + offsetof(struct w32_frame, context);
    struct w32_delivery *d = kzalloc(sizeof(*d));
    if (!d) crash(t, tf, W32_STATUS_NO_MEMORY);
    d->flags = exc->flags; d->emergency = emergency; d->next = t->w32_delivery;
    if (ua_write(p->memory, sp, &frame, sizeof(frame))) { kfree(d); crash(t, tf, W32_EXCEPTION_STACK); }
    t->w32_delivery = d;
    /* Remove pending x87/SIMD faults before running the dispatcher. The saved
     * context retains them for the handler to inspect/repair before continue. */
    fpu_signal_reset(t->task);
    tf->eip = p->w32_dispatcher; tf->user_esp = sp;
    tf->eflags = 0x202; tf->fs = CIUKI_W32_FS_SELECTOR;
    tf->gs = tf->ds = tf->es = SEL_UDATA;
}

bool w32_continue(struct proc_thread *t, struct trap_frame *tf, struct ciuki_w32_context *c)
{
    struct w32_delivery *d = t->w32_delivery;
    if (!w32_context_validate(c, t->process->memory, g_cpu_fxsr, fpu_mxcsr_mask()))
        crash(t, tf, W32_STATUS_INVALID_PARAMETER);
    /* RtlUnwind/NtContinue may restore a context outside a CPU exception.
     * User ntdll enforces SEH dispositions/noncontinuability; the kernel owns
     * selector/EFLAGS/address/FPU validation, including on a plain unwind. */
    if(d){t->w32_delivery = d->next; kfree(d);}
    fpu_w32_import(t->task, c);
    w32_context_restore(tf, c);
    return true;
}

bool w32_fault(struct trap_frame *tf, uint32_t address)
{
    struct proc_thread *t = proc_thread_for(g_current);
    if (!t || t->process->personality != PROC_WIN32) return false;
    if(tf->vector==14 && w32_view_contains(t->process->memory,address)){
        bool entered=t->in_syscall;t->in_syscall=true;
        int error=w32_view_access(t->process->memory,address,!!(tf->err&2));
        t->in_syscall=entered;
        if(!error || t->process->state!=PROC_LIVE)return true;
    }
    struct ciuki_w32_context ctx;
    w32_context_capture(&ctx, tf); fpu_w32_export(t->task, &ctx);
    bool privileged = false, sysenter = false;
    if (tf->vector == 13) {
        uint8_t code[15]; unsigned n = 0;
        while (n < sizeof(code) && tf->eip + n >= tf->eip &&
            ua_range(t->process->memory, tf->eip + n, 1, PROT_READ) &&
            !ua_read(t->process->memory, &code[n], tf->eip + n, 1)) n++;
        privileged = w32_privileged(code, n);
        sysenter = w32_sysenter(code, n);
    }
    uint32_t mxcsr = 0;
    if (g_cpu_fxsr) memcpy(&mxcsr, ctx.extended + 24, 4);
    struct ciuki_w32_exception exc = { .code = w32_exception_code(tf->vector, privileged,
        (uint16_t)ctx.floating.control, (uint16_t)ctx.floating.status, mxcsr), .address = tf->eip };
    if (sysenter) exc.code = W32_EXCEPTION_ILLEGAL;
    if (tf->vector == 3 && exc.address) exc.address--; /* resume context remains post INT3 */
    if (tf->vector == 14) {
        bool retry;
        exc.code = w32_guard_fault(t, address, &retry);
        if (retry) return true;
        exc.count = 2; exc.information[0] = !!(tf->err & 2); exc.information[1] = address;
        t->task->fault_cr2 = address;
    } else if (exc.code == W32_STATUS_ACCESS_VIOLATION) {
        exc.count = 2; exc.information[1] = UINT32_MAX;
    }
    w32_deliver(t, tf, &exc, &ctx);
    return true;
}
