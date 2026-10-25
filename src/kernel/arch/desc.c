/* GDT, TSS, double-fault TSS and IDT.
 * GDT: null, kcode 0x08, kdata 0x10, ucode 0x1B, udata 0x23, TSS 0x28,
 * double-fault TSS 0x30, native GS TLS 0x3B, DPMI BIOS data 0x40, Win32 FS
 * TEB 0x4B, DOS VM LDT 0x50 (f3-04; 0x40 and 0x50 are null unless a DPMI VM
 * worker runs). Ordinary segments remain flat.
 * Every switch installs a complete TSS bitmap; only the active full-screen
 * VGA owner or a generation-validated NDIS host may have port grants.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/arch.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include "vga_io.h"
#include "io_permissions.h"

struct gdt_entry { uint16_t lim0, base0; uint8_t base1, access, gran, base2; } __attribute__((packed));
struct idt_entry { uint16_t off0, sel; uint8_t zero, type; uint16_t off1; } __attribute__((packed));
struct dtr { uint16_t limit; uint32_t base; } __attribute__((packed));

static struct gdt_entry gdt[ARCH_LDT_GDT_INDEX + 1];
_Static_assert(ARCH_BIOS_GDT_INDEX == CIUKI_TLS_GDT_INDEX + 1, "BIOS data slot follows TLS");
_Static_assert(CIUKI_W32_FS_INDEX == ARCH_BIOS_GDT_INDEX + 1, "FS follows the BIOS data slot");
_Static_assert(ARCH_LDT_GDT_INDEX == CIUKI_W32_FS_INDEX + 1, "LDT slot follows FS");
_Static_assert(SEL_BIOS_DATA == ARCH_BIOS_GDT_INDEX * 8 && SEL_LDT == ARCH_LDT_GDT_INDEX * 8, "GDT selectors");
static struct idt_entry idt[256];
struct tss g_tss;
static struct tss df_tss;
static uint8_t df_stack[8192] __attribute__((aligned(16)));

extern uint32_t isr_table[];          /* stubs for 0-31 and 32-47 */
extern void isr_syscall(void);
extern void df_task_entry(void);

static void set_gdt(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[i].lim0 = limit & 0xFFFF;
    gdt[i].base0 = base & 0xFFFF;
    gdt[i].base1 = (base >> 16) & 0xFF;
    gdt[i].access = access;
    gdt[i].gran = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].base2 = (base >> 24) & 0xFF;
}

void gdt_init(void)
{
    memset(gdt, 0, sizeof(gdt));
    set_gdt(1, 0, 0xFFFFF, 0x9A, 0xC0);   /* kernel code */
    set_gdt(2, 0, 0xFFFFF, 0x92, 0xC0);   /* kernel data */
    set_gdt(3, 0, 0xFFFFF, 0xFA, 0xC0);   /* user code, DPL 3 */
    set_gdt(4, 0, 0xFFFFF, 0xF2, 0xC0);   /* user data, DPL 3 */
    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.ss0 = SEL_KDATA;
    g_tss.iomap_base = __builtin_offsetof(struct tss, io_bitmap);
    memset(g_tss.io_bitmap, 0xFF, sizeof(g_tss.io_bitmap));
    /* F1 V86 also checks this absent bitmap for EVERY IN/OUT, even at
     * IOPL=3 (which we never grant). Intel SDM Vol. 3B 23.2.8.1:
     * https://cdrdv2-public.intel.com/874250/253669-090-sdm-vol-3b.pdf */
    set_gdt(5, (uint32_t)&g_tss, sizeof(g_tss) - 1, 0x89, 0x00);
    set_gdt(6, (uint32_t)&df_tss, sizeof(df_tss) - 1, 0x89, 0x00);
    set_gdt(CIUKI_TLS_GDT_INDEX, 0, CIUKI_TLS_SIZE - 1, 0xF2, 0x40);

    struct dtr d = { sizeof(gdt) - 1, (uint32_t)gdt };
    __asm__ volatile(
        "lgdt %0\n"
        "ljmp $0x08, $1f\n"
        "1: mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n mov %%ax, %%es\n mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n mov %%ax, %%gs\n"
        "mov $0x28, %%ax\n ltr %%ax\n"
        :: "m"(d) : "eax", "memory");
}

void tss_set_kernel_stack(uint32_t esp0) { g_tss.esp0 = esp0; }

void arch_io_install(const struct arch_io_range *ranges, unsigned count)
{
    (void)arch_io_bitmap(g_tss.io_bitmap, ranges, count);
}

void arch_vga_io(bool enabled)
{
    /* Byte-aligned allowlist. Every other bit and the sentinel stay set.
     * Called with interrupts disabled on each task switch/ownership change. */
    arch_vga_bitmap(g_tss.io_bitmap,enabled);
}

/* f3-04 per-VM LDT. Intel SDM Vol. 3A 3.5.1 and LLDT: the descriptor is a
 * system segment of type 2 in the GDT; LLDT with a null selector marks the
 * LDTR invalid. Only the DOS VM switch hook and the DPMI entry call this;
 * the LDT itself lives in kernel memory, supervisor-only. Selector 40h, the
 * VM's BIOS data area for its DPMI clients (arch.h), is present exactly
 * while an LDT is loaded: the running VM's memory starts at linear 0. */
static const void *ldt_base;
static unsigned ldt_count;
void arch_ldt_load(const void *base, unsigned entries)
{
    if (!entries)
        base = 0;
    if (base == ldt_base && entries == ldt_count)
        return;
    uint16_t selector = 0;
    if (base) {
        set_gdt(ARCH_LDT_GDT_INDEX, (uint32_t)base, entries * 8 - 1, 0x82, 0x00);
        set_gdt(ARCH_BIOS_GDT_INDEX, ARCH_BIOS_DATA_BASE, ARCH_BIOS_DATA_LIMIT, ARCH_BIOS_DATA_ACCESS, 0x00);
        selector = SEL_LDT;
    }
    __asm__ volatile("lldt %0" :: "r"(selector) : "memory");
    if (!base) {
        memset(&gdt[ARCH_LDT_GDT_INDEX], 0, sizeof(gdt[0]));
        memset(&gdt[ARCH_BIOS_GDT_INDEX], 0, sizeof(gdt[0]));
    }
    ldt_base = base;
    ldt_count = entries;
}

/* VM-local DPMI watchpoints (Intel SDM Vol. 3B 17.2): local enables only,
 * never kernel addresses (validated by the DPMI host). */
static bool debug_active;
void arch_debug_load(const uint32_t a[4], uint32_t dr7)
{
    if (!dr7) {
        if (debug_active)
            __asm__ volatile("mov %0, %%dr7" :: "r"(0u));
        debug_active = false;
        return;
    }
    __asm__ volatile("mov %0, %%dr7" :: "r"(0u));
    __asm__ volatile("mov %0, %%dr0" :: "r"(a[0]));
    __asm__ volatile("mov %0, %%dr1" :: "r"(a[1]));
    __asm__ volatile("mov %0, %%dr2" :: "r"(a[2]));
    __asm__ volatile("mov %0, %%dr3" :: "r"(a[3]));
    __asm__ volatile("mov %0, %%dr7" :: "r"(dr7));
    debug_active = true;
}
uint32_t arch_debug_take(void)
{
    uint32_t dr6;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
    __asm__ volatile("mov %0, %%dr6" :: "r"(0xFFFF0FF0u));
    return dr6;
}
uint8_t arch_cpu_family(void)
{
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    if (a < 1) return 4;
    cpuid(1, &a, &b, &c, &d);
    unsigned family = (a >> 8) & 15;
    return (uint8_t)(family > 6 ? 6 : family < 3 ? 3 : family);
}
/* A watchpoint on a VM linear address hit by kernel code running in that
 * worker: record nothing, resume (RF for an instruction breakpoint). */
bool arch_debug_kernel_trap(struct trap_frame *tf)
{
    if (!debug_active)
        return false;
    uint32_t dr6;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
    if (!(dr6 & 15) || (dr6 & 0x4000))
        return false;
    __asm__ volatile("mov %0, %%dr6" :: "r"(0xFFFF0FF0u));
    tf->eflags |= 1u << 16;
    return true;
}

void arch_tls_switch(uint32_t base, bool native)
{
    /* Intel SDM Vol. 3: replacing a GDT entry does not invalidate a segment
     * register's hidden cache. Reload even when the selector is unchanged. */
    set_gdt(CIUKI_TLS_GDT_INDEX, base, CIUKI_TLS_SIZE - 1, 0xF2, 0x40);
    uint16_t selector = native ? CIUKI_TLS_SELECTOR : SEL_KDATA;
    __asm__ volatile("mov %0, %%gs" :: "r"(selector) : "memory");
}

void arch_w32_fs_switch(uint32_t base)
{
    if (base) arch_w32_fs_encode((uint8_t *)&gdt[CIUKI_W32_FS_INDEX], base);
    /* Load the flat selector before revoking the idle descriptor. Reload on
     * every switch even when selector 4B stays the same (hidden segment cache). */
    uint16_t selector = base ? CIUKI_W32_FS_SELECTOR : SEL_KDATA;
    __asm__ volatile("mov %0, %%fs" :: "r"(selector) : "memory");
    if (!base) memset(&gdt[CIUKI_W32_FS_INDEX], 0, sizeof(gdt[0]));
}

void df_tss_init(uint32_t cr3)
{
    memset(&df_tss, 0, sizeof(df_tss));
    df_tss.cr3 = cr3;
    df_tss.eip = (uint32_t)df_task_entry;
    df_tss.eflags = 0x2;
    df_tss.esp = df_tss.esp0 = (uint32_t)(df_stack + sizeof(df_stack));
    df_tss.cs = SEL_KCODE;
    df_tss.ss = df_tss.ss0 = df_tss.ds = df_tss.es = df_tss.fs = df_tss.gs = SEL_KDATA;
    df_tss.iomap_base = sizeof(struct tss);
    gdt[6].access = 0x89;                 /* clear busy bit */
}

static void set_idt(int v, uint32_t off, uint16_t sel, uint8_t type)
{
    idt[v].off0 = off & 0xFFFF;
    idt[v].sel = sel;
    idt[v].zero = 0;
    idt[v].type = type;
    idt[v].off1 = off >> 16;
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    for (int v = 0; v < 48; v++)
        set_idt(v, isr_table[v], SEL_KCODE, 0x8E);   /* interrupt gate, DPL 0 */
    set_idt(3, isr_table[3], SEL_KCODE, 0xEE);       /* #BP, DPL 3 */
    set_idt(8, 0, SEL_DF_TSS, 0x85);                 /* double fault: task gate */
    set_idt(0x80, (uint32_t)isr_syscall, SEL_KCODE, 0xEE); /* syscall: interrupt gate, DPL 3 */
    struct dtr d = { sizeof(idt) - 1, (uint32_t)idt };
    __asm__ volatile("lidt %0" :: "m"(d));
}
