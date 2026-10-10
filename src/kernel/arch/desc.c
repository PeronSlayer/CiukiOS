/* GDT, TSS, double-fault TSS and IDT.
 * GDT: null, kcode 0x08, kdata 0x10, ucode 0x1B, udata 0x23, TSS 0x28,
 * double-fault TSS 0x30. All segments flat; protection is by paging.
 * The TSS has no I/O bitmap, so every port is denied to ring 3 (IOPL 0).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/arch.h>
#include <ciuki/cpu.h>

struct gdt_entry { uint16_t lim0, base0; uint8_t base1, access, gran, base2; } __attribute__((packed));
struct idt_entry { uint16_t off0, sel; uint8_t zero, type; uint16_t off1; } __attribute__((packed));
struct dtr { uint16_t limit; uint32_t base; } __attribute__((packed));

static struct gdt_entry gdt[7];
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
    g_tss.iomap_base = sizeof(struct tss);  /* beyond the limit: no bitmap */
    /* F1 V86 also checks this absent bitmap for EVERY IN/OUT, even at
     * IOPL=3 (which we never grant). Intel SDM Vol. 3B 23.2.8.1:
     * https://cdrdv2-public.intel.com/874250/253669-090-sdm-vol-3b.pdf */
    set_gdt(5, (uint32_t)&g_tss, sizeof(g_tss) - 1, 0x89, 0x00);
    set_gdt(6, (uint32_t)&df_tss, sizeof(df_tss) - 1, 0x89, 0x00);

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
