/* CPU helpers for Ciuki VMM (i386, uniprocessor).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_CPU_H
#define CIUKI_CPU_H

#include <stdint.h>

#define KERNEL_VBASE    0xC0000000u
#define P2V(p)          ((void *)((uintptr_t)(p) + KERNEL_VBASE))
#define V2P(v)          ((uint32_t)(uintptr_t)(v) - KERNEL_VBASE)

#define SEL_KCODE       0x08
#define SEL_KDATA       0x10
#define SEL_UCODE       0x1B
#define SEL_UDATA       0x23
#define SEL_TSS         0x28
#define SEL_DF_TSS      0x30

#define CR0_PE  (1u << 0)
#define CR0_MP  (1u << 1)
#define CR0_EM  (1u << 2)
#define CR0_TS  (1u << 3)
#define CR0_NE  (1u << 5)
#define CR0_WP  (1u << 16)
#define CR0_NW  (1u << 29)
#define CR0_CD  (1u << 30)
#define CR0_PG  (1u << 31)
#define CR4_PSE (1u << 4)
#define CR4_PAE (1u << 5)
#define CR4_PGE (1u << 7)
#define CR4_OSFXSR     (1u << 9)
#define CR4_OSXMMEXCPT (1u << 10)

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void cpu_outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint16_t cpu_inw(uint16_t port) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint32_t inl(uint16_t port) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }

static inline uint32_t read_cr0(void) { uint32_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint32_t v) { __asm__ volatile("mov %0, %%cr0" :: "r"(v) : "memory"); }
static inline uint32_t read_cr2(void) { uint32_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint32_t read_cr3(void) { uint32_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint32_t v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static inline uint32_t read_cr4(void) { uint32_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr4(uint32_t v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory"); }
static inline void invlpg(uint32_t va) { __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); }
static inline uint32_t read_eflags(void) { uint32_t v; __asm__ volatile("pushfl; popl %0" : "=r"(v)); return v; }
static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt" ::: "memory"); }
static inline void clts(void) { __asm__ volatile("clts" ::: "memory"); }

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0) : "memory");
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr) : "memory");
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)) : "memory");
}
static inline void wbinvd(void) { __asm__ volatile("wbinvd" ::: "memory"); }

/* Integer-only WC drain; valid on the P6 baseline without SSE/SFENCE. */
static inline void cpu_write_drain(void)
{
    uint32_t fence = 0;
    __asm__ volatile("lock; addl $0, %0" : "+m"(fence) :: "memory", "cc");
}
static inline uint64_t read_tsc_serialized(void)
{
    uint32_t a, b, c, d, lo, hi;
    cpuid(0, &a, &b, &c, &d);
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    /* Prevent following copy stores from passing the timestamp as well. */
    cpuid(0, &a, &b, &c, &d);
    return ((uint64_t)hi << 32) | lo;
}

/* Save EFLAGS and disable interrupts; restore. Outermost sections are
 * timed for the F0 latency evidence (core/timing.c). */
void crit_begin(void);
void crit_end(void);
void crit_label(const char *section) __attribute__((weak));
static inline uint32_t irq_save_at(const char *section)
{
    uint32_t f = read_eflags();
    cli();
    if (f & 0x200) {
        crit_begin();
        if (crit_label) crit_label(section);
    }
    return f;
}
#define irq_save() irq_save_at(__func__)
static inline void irq_restore(uint32_t f)
{
    if (f & 0x200) {
        crit_end();
        sti();
    }
}

#endif
