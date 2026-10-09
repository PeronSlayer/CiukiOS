/* 8259A pair (master 0x20-0x27, slave 0x28-0x2F, explicit EOI) and PIT
 * channel 0 at nominal 1000 Hz (mode 2, divisor 1193).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/arch.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>

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
    uint32_t f = irq_save();
    if (irq < 8) {
        mask_master &= (uint8_t)~(1u << irq);
    } else {
        mask_slave &= (uint8_t)~(1u << (irq - 8));
        mask_master &= (uint8_t)~(1u << 2);
        outb(0xA1, mask_slave);
    }
    outb(0x21, mask_master);
    irq_restore(f);
}

void pic_mask(unsigned irq)
{
    uint32_t f = irq_save();
    if (irq < 8) {
        mask_master |= (uint8_t)(1u << irq);
        outb(0x21, mask_master);
    } else {
        mask_slave |= (uint8_t)(1u << (irq - 8));
        outb(0xA1, mask_slave);
    }
    irq_restore(f);
}

static void timer_irq(struct trap_frame *tf)
{
    (void)tf;
    g_ticks++;
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
