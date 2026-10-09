/* Descriptors, traps, interrupts (docs/design/execution-abi.md).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ARCH_H
#define CIUKI_ARCH_H

#include <stdint.h>
#include <stdbool.h>

struct trap_frame {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t vector, err;
    uint32_t eip, cs, eflags;
    uint32_t user_esp, user_ss;     /* valid only when (cs & 3) == 3 */
};

struct tss {
    uint32_t link, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

extern struct tss g_tss;
void gdt_init(void);
void idt_init(void);
void tss_set_kernel_stack(uint32_t esp0);
void df_tss_init(uint32_t cr3);

/* PIC / IRQ */
void pic_init(void);
void pic_unmask(unsigned irq);
void pic_mask(unsigned irq);
extern volatile uint32_t g_spurious_irq7, g_spurious_irq15;
typedef void (*irq_handler_t)(struct trap_frame *tf);
void irq_set_handler(unsigned irq, irq_handler_t h);

/* expected-fault fixups used by probes and copy_user */
struct expected_fault {
    bool armed;
    uint32_t vector;
    uint32_t eip_start, eip_end;    /* faulting instruction range */
    uint32_t resume_eip;
    uint32_t addr;                  /* expected CR2 for #PF, 0 = any */
    /* results */
    bool hit;
    uint32_t got_err, got_eip, got_cr2;
};
extern struct expected_fault g_expect;

void trap_dispatch(struct trap_frame *tf);

#endif
