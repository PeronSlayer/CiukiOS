/* 16550-compatible UART output with bounded polling.
 * docs/design/device-firmware-ownership.md: presence probe, bounded polls,
 * absence disables the sink without delaying boot.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>

#define UART_POLL_LIMIT 20000u   /* per byte; ~2.6 ms at 38400 baud worst case */
#define UART_DEAD_LIMIT 8u       /* consecutive timeouts before disabling */

static uint16_t uart_base;
static bool uart_ok;
static unsigned uart_timeouts;

void serial_init(uint16_t base, uint16_t divisor)
{
    uart_ok = false;
    uart_base = base;
    if (!base)
        return;
    /* Presence probe: scratch register round trip. */
    outb(base + 7, 0x5A);
    if (inb(base + 7) != 0x5A)
        return;
    outb(base + 7, 0xA5);
    if (inb(base + 7) != 0xA5)
        return;
    if (!divisor)
        divisor = 3;                /* 38400 baud with 1.8432 MHz clock */
    outb(base + 1, 0x00);           /* no UART interrupts */
    outb(base + 3, 0x80);           /* DLAB */
    outb(base + 0, divisor & 0xFF);
    outb(base + 1, divisor >> 8);
    outb(base + 3, 0x03);           /* 8N1 */
    outb(base + 2, 0x07);           /* FIFO on, cleared */
    outb(base + 4, 0x03);           /* DTR, RTS */
    uart_ok = true;
    uart_timeouts = 0;
}

bool serial_present(void) { return uart_ok; }

static void serial_putc(char c)
{
    unsigned n = 0;
    while (!(inb(uart_base + 5) & 0x20)) {
        if (++n >= UART_POLL_LIMIT) {
            if (++uart_timeouts >= UART_DEAD_LIMIT)
                uart_ok = false;
            return;
        }
    }
    uart_timeouts = 0;
    outb(uart_base, (uint8_t)c);
}

void serial_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n && uart_ok; i++) {
        if (s[i] == '\n')
            serial_putc('\r');
        serial_putc(s[i]);
    }
}
