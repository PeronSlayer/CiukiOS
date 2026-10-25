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
    uint8_t io_bitmap[8193]; /* all ports plus mandatory FFh sentinel */
} __attribute__((packed));

extern struct tss g_tss;
void gdt_init(void);
void idt_init(void);
void tss_set_kernel_stack(uint32_t esp0);
struct arch_io_range;
void arch_io_install(const struct arch_io_range *, unsigned count); /* IF=0 */
void arch_vga_io(bool enabled); /* current task only; IOPL remains zero */
void df_tss_init(uint32_t cr3);

/* f3-04: the LDT descriptor of the running DOS VM's DPMI clients
 * (execution-abi.md, "F3-04 amendment"). NULL unloads it (LDTR = 0). Debug
 * registers are VM-local watchpoints, loaded only while that VM's worker runs.
 * 2026-10-25 (execution-abi.md, "Selector 40h amendment"): GDT index 8,
 * selector 40h, is the BIOS data area of the running VM for its DPMI clients
 * (linear 400h, 64 KiB, DPL 3 data, as the real-mode segment 0040h and the
 * INT 31h 0002h alias of it), the "bimodal" selector 40h that Windows
 * provides and the DOS/16M kernel of DOS/4GW loads in its INT 21h 2Ah/2Ch
 * handlers; it is present only while the LDT is loaded. The LDT descriptor
 * moved after the Win32 FS slot, to index 10 (selector 50h). */
#define ARCH_BIOS_GDT_INDEX 8
#define SEL_BIOS_DATA 0x40
#define ARCH_BIOS_DATA_BASE 0x400u
#define ARCH_BIOS_DATA_LIMIT 0xFFFFu
#define ARCH_BIOS_DATA_ACCESS 0xF2u   /* present, DPL 3, data, writable */
#define ARCH_LDT_GDT_INDEX 10
#define SEL_LDT 0x50
void arch_w32_fs_switch(uint32_t base);
/* Byte-granular DPL3 data segment, preserving the exact 8 KiB TEB limit. */
static inline void arch_w32_fs_encode(uint8_t out[8], uint32_t base)
{
    out[0] = 0xff; out[1] = 0x1f;
    out[2] = (uint8_t)base; out[3] = (uint8_t)(base >> 8);
    out[4] = (uint8_t)(base >> 16); out[5] = 0xf2;
    out[6] = 0x40; out[7] = (uint8_t)(base >> 24);
}
void arch_ldt_load(const void *base, unsigned entries);
void arch_debug_load(const uint32_t address[4], uint32_t dr7);
uint32_t arch_debug_take(void);          /* DR6, then cleared */
bool arch_debug_kernel_trap(struct trap_frame *tf);
uint8_t arch_cpu_family(void);          /* CPUID family, 3..6, for DPMI CL */

/* PIC / IRQ */
void pic_init(void);
void pic_pit_resume(void); /* after a firmware suspend: ICWs, current masks, PIT rate */
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
