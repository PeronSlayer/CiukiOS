/* Trap dispatcher: exceptions, PIC IRQs with dispatcher-owned EOI and
 * spurious IRQ7/15 handling, syscalls (docs/design/device-firmware-ownership.md,
 * docs/design/execution-abi.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/arch.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/signal.h>
#include <ciuki/biosvm.h>
#include <ciuki/cpustat.h>
#include <ciuki/registry.h>
extern bool dosvm_trap(struct trap_frame *) __attribute__((weak));
extern bool dosvm_trap_fast(struct trap_frame *) __attribute__((weak));
extern void dosvm_perf_enter(void) __attribute__((weak));
extern bool dosvm_client(const struct task *) __attribute__((weak));
extern void entropy_irq(unsigned) __attribute__((weak));
extern bool ndis_execution_exception(struct trap_frame *) __attribute__((weak));
extern void ndis_execution_timer(struct trap_frame *) __attribute__((weak));
extern void ndis_execution_irq_enter(void) __attribute__((weak));
extern void ndis_execution_irq_leave(void) __attribute__((weak));
extern bool ndis_user_trap(struct trap_frame *) __attribute__((weak));
extern void ndis_user_timer(void) __attribute__((weak));

extern char copy_user_fault_start[], copy_user_fault_end[], copy_user_fixup[];

extern bool w32_fault(struct trap_frame *, uint32_t) __attribute__((weak));
struct expected_fault g_expect;
volatile uint32_t g_spurious_irq7, g_spurious_irq15;
static irq_handler_t irq_handlers[16];

void irq_set_handler(unsigned irq, irq_handler_t h)
{
    if (irq < 16)
        irq_handlers[irq] = h;
}

static void pic_eoi(unsigned irq)
{
    if (irq >= 8)
        outb(0xA0, 0x60 | (irq - 8));   /* specific EOI on the slave */
    outb(0x20, 0x60 | (irq >= 8 ? 2 : irq));
}

static uint8_t pic_isr(uint16_t cmd)
{
    outb(cmd, 0x0B);
    return inb(cmd);
}

static void handle_irq(struct trap_frame *tf)
{
    static const char *const sections[16] = {
        "irq0", "irq1", "irq2", "irq3", "irq4", "irq5", "irq6", "irq7",
        "irq8", "irq9", "irq10", "irq11", "irq12", "irq13", "irq14", "irq15"
    };
    unsigned irq = tf->vector - 0x20;
    if (ndis_execution_irq_enter) ndis_execution_irq_enter();
    if (ledger_irq_context) ledger_irq_context(true);
    if (crit_label) crit_label(sections[irq]);
    if (crit_owner) crit_owner(sections[irq], (uint32_t)(uintptr_t)irq_handlers[irq]);
    if (irq == 7 && !(pic_isr(0x20) & 0x80)) {
        g_spurious_irq7++;              /* spurious: no handler, no EOI */
        irq_chain_spurious(7);
        if (ledger_irq_context) ledger_irq_context(false);
        if (ndis_execution_irq_leave) ndis_execution_irq_leave();
        return;
    }
    if (irq == 15 && !(pic_isr(0xA0) & 0x80)) {
        g_spurious_irq15++;
        irq_chain_spurious(15);
        outb(0x20, 0x62);               /* acknowledge the master cascade only */
        if (ledger_irq_context) ledger_irq_context(false);
        if (ndis_execution_irq_leave) ndis_execution_irq_leave();
        return;
    }
    if (entropy_irq) entropy_irq(irq);
    biosvm_account_irq(irq, false, irq_handlers[irq] != 0);
    /* f1-94: the section label/owner name the vector and handler; these
     * marks split handler time from dispatch, EOI and accounting. */
    if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_IRQ_HANDLER);
    if (irq_handlers[irq])
        irq_handlers[irq](tf);
    if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_IRQ_EOI);
    pic_eoi(irq);
    biosvm_account_irq(irq, true, irq_handlers[irq] != 0);
    if (ledger_irq_context) ledger_irq_context(false);
    if (ndis_execution_irq_leave) ndis_execution_irq_leave();
    if (irq==0 && ndis_user_timer) ndis_user_timer(); /* all hosts, after EOI */
}

static bool try_expected(struct trap_frame *tf)
{
    if (tf->vector == 14 && tf->eip >= (uint32_t)copy_user_fault_start &&
        tf->eip < (uint32_t)copy_user_fault_end) {
        tf->eip = (uint32_t)copy_user_fixup;
        return true;
    }
    if (!g_expect.armed || tf->vector != g_expect.vector)
        return false;
    if (tf->eip < g_expect.eip_start || tf->eip >= g_expect.eip_end)
        return false;
    uint32_t cr2 = read_cr2();
    if (tf->vector == 14 && g_expect.addr && cr2 != g_expect.addr)
        return false;
    g_expect.hit = true;
    g_expect.got_err = tf->err;
    g_expect.got_eip = tf->eip;
    g_expect.got_cr2 = cr2;
    g_expect.armed = false;
    tf->eip = g_expect.resume_eip;
    return true;
}

void trap_dispatch(struct trap_frame *tf)
{
    /* Recover before the pinned-context assertion/fatal exception path.
     * No registered foreign callback means this hook is inert. */
    if (ndis_execution_exception && ndis_execution_exception(tf)) return;
    if (tf->vector==14 && ledger_assert_pinned_context) ledger_assert_pinned_context(true,tf->eip);
    /* fx-doomwin: a DOS VM's plane select (V86 or its DPMI client at ring
     * 3) retires here, IF clear to the IRET, without the per-exit task
     * stamps; otherwise dosvm_trap_fast is false with no VM state changed
     * (it may have taken the monitor's entry stamp, which dosvm_trap keeps). */
    if (tf->vector == 13 && !tf->err && dosvm_trap_fast &&
        ((tf->eflags & V86_VM) || (tf->cs & 3) == 3) && dosvm_trap_fast(tf)) return;
    cpu_trap_enter(tf);
    /* F1: VM has CPL3 regardless of the low bits in real-mode CS.
     * Dispatch/EOI physical IRQs exactly once before firmware reflection.
     * Non-V86 frames retain the original F0 path below. */
    if (tf->eflags & V86_VM) {
        if (dosvm_perf_enter) dosvm_perf_enter();
        if (tf->vector >= 0x20 && tf->vector < 0x30) {
            crit_begin();
            if (crit_label) crit_label(tf->vector == 0x20 ? "irq_timer" : "irq_dispatch");
            if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_IRQ_ENTRY);
            uint64_t irq_start = cpu_irq_enter(tf);
            handle_irq(tf);
            cpu_irq_leave(irq_start);
            if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_IRQ_EXIT);
            crit_end();
        }
        if (!dosvm_trap || !dosvm_trap(tf)) biosvm_trap(tf);
        cpu_user_return();
        return;
    }
    bool from_user = (tf->cs & 3) == 3;
    if (from_user && ndis_user_trap && ndis_user_trap(tf)) {
        cpu_user_return();
        return;
    }
    /* f3-04: a DOS VM worker running a DPMI client at ring 3 (no VM flag).
     * Physical IRQs keep their single dispatch/EOI here; every other vector,
     * INT 80h included, belongs to the VM monitor and never reaches the
     * native syscall or signal paths (dos-dpmi-contract.md, execution-abi.md). */
    if (from_user && dosvm_client && dosvm_client(g_current)) {
        if (dosvm_perf_enter) dosvm_perf_enter();
        if (tf->vector >= 0x20 && tf->vector < 0x30) {
            crit_begin();
            if (crit_label) crit_label(tf->vector == 0x20 ? "irq_timer" : "irq_dispatch");
            if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_IRQ_ENTRY);
            uint64_t irq_start = cpu_irq_enter(tf);
            handle_irq(tf);
            cpu_irq_leave(irq_start);
            if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_IRQ_EXIT);
            crit_end();
        }
        dosvm_trap(tf);
        cpu_user_return();
        return;
    }

    if (tf->vector >= 0x20 && tf->vector < 0x30) {
        crit_begin();
        if (crit_label) crit_label(tf->vector == 0x20 ? "irq_timer" : "irq_dispatch");
        if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_IRQ_ENTRY);
        uint64_t irq_start = cpu_irq_enter(tf);
        handle_irq(tf);
        cpu_irq_leave(irq_start);
        if (cpu_irqoff_mark) cpu_irqoff_mark(CPU_IRQOFF_IRQ_EXIT);
        if (from_user && g_need_resched)
            schedule();               /* still inside the IF-disabled interval */
        crit_end();
        if (ndis_execution_timer) ndis_execution_timer(tf);
        if (from_user) cpu_user_return();
        return;
    } else if (tf->vector == 0x80) {
        sti();                          /* frame saved: interrupts may resume */
        if (!proc_signal_syscall(tf))
            syscall_dispatch(tf);
        cli();
    } else if (tf->vector == 7) {
        fpu_handle_nm();
    } else if (!from_user) {
        if (tf->vector == 1 && arch_debug_kernel_trap(tf))
            return;                     /* VM watchpoint touched by kernel code */
        if (!try_expected(tf))
            panic_frame(tf, "kernel_exception");
    } else {
        if (proc_thread_for(g_current)) {
            uint32_t cr2 = tf->vector == 14 ? read_cr2() : 0;
            sti();
            struct proc_thread *thread=proc_thread_for(g_current);
            bool saved_syscall=thread->in_syscall;
            thread->in_syscall=true; /* a sleeping pager must unwind on exit */
            int page=tf->vector==14?ua_fault(thread->process->memory,cr2,tf->err):-EFAULT;
            thread->in_syscall=saved_syscall;
            if (thread->process->state==PROC_STOPPING) task_exit(thread->process->raw_exit);
            if (page && (!w32_fault || !w32_fault(tf, cr2))) proc_signal_fault(tf, cr2);
            cli();
            cpu_user_return();
            return;
        }
        /* Ring-3 fault: terminate only the offending task. */
        struct task *t = g_current;
        t->fault_vector = tf->vector;
        t->fault_err = tf->err;
        t->fault_eip = tf->eip;
        t->fault_cr2 = tf->vector == 14 ? read_cr2() : 0;
        if (tf->vector == 16)           /* clear pending x87 state of the task */
            __asm__ volatile("fnclex");
        sti();
        task_exit(EXIT_FAULT_BASE + (int)tf->vector);
    }

    if (from_user && g_need_resched)
        schedule();
    if (from_user) cpu_user_return();
}
