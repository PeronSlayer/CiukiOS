#include "cpu_tables.h"
#include "types.h"

#define GDT_ENTRY_COUNT 5
#define STAGE2_CODE_SELECTOR 0x08
#define STAGE2_DATA_SELECTOR 0x10
#define STAGE2_TSS_SELECTOR  0x18
#define STAGE2_INTERRUPT_STACK_SIZE 4096

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) gdtr_t;

typedef struct {
    u32 reserved0;
    u64 rsp0;
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist1;
    u64 ist2;
    u64 ist3;
    u64 ist4;
    u64 ist5;
    u64 ist6;
    u64 ist7;
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;
} __attribute__((packed)) tss64_t;

extern void stage2_gdt_apply(const gdtr_t *gdtr, u64 data_selector, u64 code_selector, u64 tss_selector);

static u64 g_gdt[GDT_ENTRY_COUNT] __attribute__((aligned(16)));
static tss64_t g_tss __attribute__((aligned(16)));
static u8 g_interrupt_stack[STAGE2_INTERRUPT_STACK_SIZE] __attribute__((aligned(16)));

static void set_tss_descriptor(u16 index, u64 base, u32 limit) {
    u64 low = 0;
    u64 high = 0;

    low |= (u64)(limit & 0xFFFFU);
    low |= (u64)(base & 0xFFFFFFULL) << 16;
    low |= (u64)0x89ULL << 40;
    low |= (u64)((limit >> 16) & 0x0FU) << 48;
    low |= (u64)((base >> 24) & 0xFFULL) << 56;

    high |= (base >> 32) & 0xFFFFFFFFULL;

    g_gdt[index] = low;
    g_gdt[index + 1] = high;
}

void stage2_init_gdt_tss(void) {
    gdtr_t gdtr;
    u64 tss_base = (u64)(unsigned long long)&g_tss;
    u32 tss_limit = (u32)(sizeof(g_tss) - 1);

    for (u16 i = 0; i < GDT_ENTRY_COUNT; i++) {
        g_gdt[i] = 0;
    }

    g_gdt[1] = 0x00AF9A000000FFFFULL;
    g_gdt[2] = 0x00AF92000000FFFFULL;

    for (u16 i = 0; i < (u16)sizeof(g_tss); i++) {
        ((volatile u8 *)&g_tss)[i] = 0;
    }

    g_tss.rsp0 = (u64)(unsigned long long)&g_interrupt_stack[STAGE2_INTERRUPT_STACK_SIZE];
    g_tss.ist1 = (u64)(unsigned long long)&g_interrupt_stack[STAGE2_INTERRUPT_STACK_SIZE];
    g_tss.iomap_base = (u16)sizeof(g_tss);

    set_tss_descriptor(3, tss_base, tss_limit);

    gdtr.limit = (u16)(sizeof(g_gdt) - 1);
    gdtr.base = (u64)(unsigned long long)&g_gdt[0];

    stage2_gdt_apply(&gdtr, STAGE2_DATA_SELECTOR, STAGE2_CODE_SELECTOR, STAGE2_TSS_SELECTOR);
}
