#include "serial.h"
#include "types.h"

#define COM1 0x3F8

static inline void outb(u16 port, u8 value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static int serial_can_transmit(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_write_char(char c) {
    while (!serial_can_transmit()) { }
    outb(COM1, (u8)c);
}

void serial_write(const char *s) {
    while (*s) {
        if (*s == '\n') {
            serial_write_char('\r');
        }
        serial_write_char(*s++);
    }
}

void serial_write_hex64(u64 value) {
    static const char *hex = "0123456789ABCDEF";
    for (unsigned int i = 15; i < 16; --i) {  // Loop 16 volte (15, 14, ..., 1, 0)
        u8 nibble = (value >> (i * 4)) & 0xF;
        serial_write_char(hex[nibble]);
    }
}
