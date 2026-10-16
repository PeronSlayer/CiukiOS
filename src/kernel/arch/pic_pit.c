/* 8259A pair (master 0x20-0x27, slave 0x28-0x2F, explicit EOI) and PIT
 * channel 0 at nominal 1000 Hz (mode 2, divisor 1193).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/arch.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/win32.h>
extern void w32_shared_tick(void) __attribute__((weak));
extern void serial_tick(void) __attribute__((weak));

/* Drivers open a line when publishing its handler, then gate requests at
 * the device. Runtime IMR changes are quarantine, chain deferral/rearm or
 * explicit hardware checks; every changed mask remains counted below. */
static uint8_t mask_master = 0xFF, mask_slave = 0xFF;
volatile uint64_t g_ticks;

void pic_init(void)
{
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    outb(0x20, 0x11); io_wait();         /* ICW1: edge, cascade, ICW4 */
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();         /* ICW2: vector bases */
    outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();         /* ICW3: slave on IRQ2 */
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();         /* ICW4: 8086, normal EOI */
    outb(0xA1, 0x01); io_wait();
    mask_master = mask_slave = 0xFF;
    outb(0x21, mask_master);
    outb(0xA1, mask_slave);
}

void pic_unmask(unsigned irq)
{
    if (irq >= 16) return;
    /* Open the cascade once, separately. Each mask transaction is one OUT;
     * ordinary updates follow Linux i8259.c, not the ICW init delays. */
    if (irq >= 8) pic_unmask(2);
    uint32_t f = irq_save();
    uint8_t *mask = irq < 8 ? &mask_master : &mask_slave;
    uint8_t next = *mask & (uint8_t)~(1u << (irq & 7));
    if (next != *mask) {
        *mask = next;
        uint64_t start = g_cpu_tsc ? read_tsc_serialized() : 0;
        outb(irq < 8 ? 0x21 : 0xA1, next);
        uint64_t elapsed = g_cpu_tsc ? read_tsc_serialized() - start : 0;
        if (timing_pic_mask_write) timing_pic_mask_write(elapsed);
    }
    irq_restore(f);
}

void pic_mask(unsigned irq)
{
    if (irq >= 16) return;
    uint32_t f = irq_save();
    uint8_t *mask = irq < 8 ? &mask_master : &mask_slave;
    uint8_t next = *mask | (uint8_t)(1u << (irq & 7));
    if (next != *mask) {
        *mask = next;
        uint64_t start = g_cpu_tsc ? read_tsc_serialized() : 0;
        outb(irq < 8 ? 0x21 : 0xA1, next);
        uint64_t elapsed = g_cpu_tsc ? read_tsc_serialized() - start : 0;
        if (timing_pic_mask_write) timing_pic_mask_write(elapsed);
    }
    irq_restore(f);
}

static void timer_irq(struct trap_frame *tf)
{
    (void)tf;
    g_ticks++;
    if (serial_tick) serial_tick();  /* idle TX rearm only; no polling or FIFO writes */
    if (w32_shared_tick) w32_shared_tick();
    timing_timer_irq();
    sched_tick();
}

void pit_init(void)
{
    outb(0x43, 0x34);                    /* channel 0, lo/hi, mode 2 */
    outb(0x40, 1193 & 0xFF);
    outb(0x40, 1193 >> 8);
    irq_set_handler(0, timer_irq);
    pic_unmask(0);
}

uint32_t ticks_lo(void) { return (uint32_t)g_ticks; }

static uint16_t pit_latch(void)
{
    outb(0x43, 0); /* channel 0 count latch, preserves mode */
    uint16_t count = inb(0x40);
    return count | (uint16_t)inb(0x40) << 8;
}
uint64_t arch_w32_counter(void)
{
    static uint64_t previous;
    uint32_t flags = irq_save();
    uint64_t ticks = g_ticks;
    uint16_t before = pit_latch();
    outb(0x20, 0x0a); /* master IRR, no EOI or interrupt-mask mutation */
    bool pending = !!(inb(0x20) & 1);
    uint16_t after = pit_latch();
    uint64_t sample = w32_counter_sample(ticks, before, pending, after);
    /* The PIC cannot count multiple missed periods. A long IRQ-disabled
     * interval may lose elapsed time, but never makes QPC run backwards. */
    if (sample < previous) sample = previous;
    previous = sample;
    irq_restore(flags);
    return sample;
}
