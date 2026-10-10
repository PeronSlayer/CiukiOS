/* Trap dispatcher: exceptions, PIC IRQs with dispatcher-owned EOI and
 * spurious IRQ7/15 handling, syscalls (docs/design/device-firmware-ownership.md,
 * docs/design/execution-abi.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/arch.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/biosvm.h>

extern char copy_user_fault_start[], copy_user_fault_end[], copy_user_fixup[];

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
    unsigned irq = tf->vector - 0x20;
    if (irq == 7 && !(pic_isr(0x20) & 0x80)) {
        g_spurious_irq7++;              /* spurious: no handler, no EOI */
        return;
    }
    if (irq == 15 && !(pic_isr(0xA0) & 0x80)) {
        g_spurious_irq15++;
        outb(0x20, 0x62);               /* acknowledge the master cascade only */
        return;
    }
    if (irq_handlers[irq])
        irq_handlers[irq](tf);
    pic_eoi(irq);
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
    /* F1: VM has CPL3 regardless of the low bits in real-mode CS.
     * Dispatch/EOI physical IRQs exactly once before firmware reflection.
     * Non-V86 frames retain the original F0 path below. */
    if (tf->eflags & V86_VM) {
        if (tf->vector >= 0x20 && tf->vector < 0x30) {
            crit_begin();
            handle_irq(tf);
            crit_end();
        }
        biosvm_trap(tf);
        return;
    }
    bool from_user = (tf->cs & 3) == 3;

    if (tf->vector >= 0x20 && tf->vector < 0x30) {
        crit_begin();
        handle_irq(tf);
        if (from_user && g_need_resched)
            schedule();               /* still inside the IF-disabled interval */
        crit_end();
        return;
    } else if (tf->vector == 0x80) {
        sti();                          /* frame saved: interrupts may resume */
        syscall_dispatch(tf);
        cli();
    } else if (tf->vector == 7) {
        fpu_handle_nm();
    } else if (!from_user) {
        if (!try_expected(tf))
            panic_frame(tf, "kernel_exception");
    } else {
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
}
