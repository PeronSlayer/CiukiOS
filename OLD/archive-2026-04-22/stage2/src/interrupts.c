#include "interrupts.h"
#include "serial.h"

#define IDT_ENTRIES 256
#define IDT_INTERRUPT_GATE 0x8E

typedef struct {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 type_attr;
    u16 offset_mid;
    u32 offset_high;
    u32 zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) idtr_t;

extern void stage2_interrupt_default_stub(void);
extern void stage2_exception_ud_stub(void);
extern void stage2_exception_gp_stub(void);
extern void stage2_exception_pf_stub(void);
extern void stage2_irq0_stub(void);
extern void stage2_irq1_stub(void);
extern void stage2_irq12_stub(void);

static idt_entry_t g_idt[IDT_ENTRIES] __attribute__((aligned(16)));

static inline u16 read_cs(void) {
    u16 cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    return cs;
}

static inline void load_idtr(const idtr_t *idtr) {
    __asm__ volatile ("lidt %0" : : "m"(*idtr));
}

static void set_idt_entry(u8 vector, void (*handler)(void), u16 selector, u8 ist) {
    u64 address = (u64)(unsigned long long)handler;
    idt_entry_t *entry = &g_idt[vector];

    entry->offset_low = (u16)(address & 0xFFFF);
    entry->selector = selector;
    entry->ist = (u8)(ist & 0x07);
    entry->type_attr = IDT_INTERRUPT_GATE;
    entry->offset_mid = (u16)((address >> 16) & 0xFFFF);
    entry->offset_high = (u32)((address >> 32) & 0xFFFFFFFF);
    entry->zero = 0;
}

void stage2_init_idt(void) {
    idtr_t idtr;
    u16 cs_selector = read_cs();

    for (u16 i = 0; i < IDT_ENTRIES; i++) {
        set_idt_entry((u8)i, stage2_interrupt_default_stub, cs_selector, 0);
    }

    set_idt_entry(6, stage2_exception_ud_stub, cs_selector, 1);
    set_idt_entry(13, stage2_exception_gp_stub, cs_selector, 1);
    set_idt_entry(14, stage2_exception_pf_stub, cs_selector, 1);
    set_idt_entry(32, stage2_irq0_stub, cs_selector, 0);
    set_idt_entry(33, stage2_irq1_stub, cs_selector, 0);
    set_idt_entry(44, stage2_irq12_stub, cs_selector, 0);

    idtr.limit = (u16)(sizeof(g_idt) - 1);
    idtr.base = (u64)(unsigned long long)&g_idt[0];

    load_idtr(&idtr);
}

void stage2_enable_interrupts(void) {
    __asm__ volatile ("sti");
}

static const char *exception_name(u64 vector) {
    switch (vector) {
        case 6:
            return "#UD";
        case 13:
            return "#GP";
        case 14:
            return "#PF";
        default:
            return "#UNK";
    }
}

void stage2_exception_panic(u64 vector, u64 error_code) {
    serial_write("\n[ exception ] ");
    serial_write(exception_name(vector));
    serial_write(" vector=0x");
    serial_write_hex64(vector);
    serial_write(" error=0x");
    serial_write_hex64(error_code);
    serial_write("\n");

    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}
