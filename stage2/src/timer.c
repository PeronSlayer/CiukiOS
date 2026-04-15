#include "timer.h"
#include "serial.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIT_CMD   0x43
#define PIT_CH0   0x40

#define PIC_EOI   0x20

static volatile u64 g_timer_ticks = 0;

static inline void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void) {
    __asm__ volatile ("outb %%al, $0x80" : : "a"((u8)0));
}

static void pic_remap(u8 offset_master, u8 offset_slave) {
    u8 mask_master = inb(PIC1_DATA);
    u8 mask_slave = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11);
    io_wait();
    outb(PIC2_CMD, 0x11);
    io_wait();

    outb(PIC1_DATA, offset_master);
    io_wait();
    outb(PIC2_DATA, offset_slave);
    io_wait();

    outb(PIC1_DATA, 0x04);
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();

    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    (void)mask_master;
    (void)mask_slave;

    outb(PIC1_DATA, 0xFC);
    outb(PIC2_DATA, 0xFF);
}

static void pit_set_rate_hz(u32 hz) {
    u32 divisor = 0;

    if (hz == 0) {
        hz = 100;
    }

    divisor = 1193182U / hz;
    if (divisor == 0) {
        divisor = 1;
    }

    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (u8)(divisor & 0xFFU));
    outb(PIT_CH0, (u8)((divisor >> 8) & 0xFFU));
}

void stage2_timer_init(void) {
    g_timer_ticks = 0;
    pic_remap(0x20, 0x28);
    pit_set_rate_hz(100);
}

void stage2_timer_on_irq0(void) {
    g_timer_ticks++;

    if (g_timer_ticks == 1 || (g_timer_ticks % 100ULL) == 0) {
        serial_write("[ tick ] irq0 #");
        serial_write_hex64(g_timer_ticks);
        serial_write("\n");
    }

    outb(PIC1_CMD, PIC_EOI);
}

u64 stage2_timer_ticks(void) {
    return g_timer_ticks;
}
