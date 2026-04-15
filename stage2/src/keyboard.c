#include "keyboard.h"
#include "serial.h"

#define PIC1_CMD       0x20
#define PIC_EOI        0x20
#define KBD_DATA_PORT  0x60
#define KBD_STAT_PORT  0x64
#define KBD_OBF_MASK   0x01

static volatile u64 g_keyboard_irq_count = 0;

static inline void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void stage2_keyboard_init(void) {
    g_keyboard_irq_count = 0;

    for (u8 i = 0; i < 32; i++) {
        u8 status = inb(KBD_STAT_PORT);
        if ((status & KBD_OBF_MASK) == 0) {
            break;
        }
        (void)inb(KBD_DATA_PORT);
    }
}

void stage2_keyboard_on_irq1(void) {
    u8 scancode = inb(KBD_DATA_PORT);
    g_keyboard_irq_count++;

    if (g_keyboard_irq_count <= 4 || (g_keyboard_irq_count % 32ULL) == 0) {
        serial_write("[ key ] scancode=0x");
        serial_write_hex8(scancode);
        serial_write(" irq1#");
        serial_write_hex64(g_keyboard_irq_count);
        serial_write("\n");
    }

    outb(PIC1_CMD, PIC_EOI);
}

u64 stage2_keyboard_irq_count(void) {
    return g_keyboard_irq_count;
}
