/* 16550-compatible UART output with bounded polling.
 * docs/design/device-firmware-ownership.md: presence probe, bounded polls,
 * absence disables the sink without delaying boot.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>

#define UART_POLL_LIMIT 20000u   /* per readiness wait, only boot/panic */
#define UART_DEAD_LIMIT 8u       /* consecutive timeouts before disabling */

static uint16_t uart_base;
static bool uart_ok;
static unsigned uart_timeouts;
#define UART_LOG_BYTES 8192u
static char uart_log[UART_LOG_BYTES];
static unsigned uart_head, uart_used, uart_dropped;
static bool uart_buffering, uart_drop_notice;
static unsigned uart_fifo_bytes = 1;

void serial_init(uint16_t base, uint16_t divisor)
{
    uart_ok = false;
    uart_base = base;
    uart_head = uart_used = uart_dropped = 0;
    uart_buffering = uart_drop_notice = false;
    uart_fifo_bytes = 1;
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
    if ((inb(base + 2) & 0xC0) == 0xC0) uart_fifo_bytes = 16;
    outb(base + 4, 0x03);           /* DTR, RTS */
    uart_ok = true;
    uart_timeouts = 0;
}

bool serial_present(void) { return uart_ok; }

/* All callers hold the UP IRQ lock. Publish notices only between records. */
static void serial_notice(void)
{
    if (!uart_used && uart_drop_notice) {
        static const char notice[] = "[serial] result=log_dropped\r\n";
        memcpy(uart_log, notice, sizeof(notice) - 1);
        uart_head = 0;
        uart_used = sizeof(notice) - 1;
        uart_drop_notice = false;
    }
}

static void serial_burst(void)
{
    unsigned count = uart_used < uart_fifo_bytes ? uart_used : uart_fifo_bytes;
    for (unsigned i = 0; i < count; i++) {
        outb(uart_base, (uint8_t)uart_log[uart_head]);
        uart_head = (uart_head + 1) % UART_LOG_BYTES;
        uart_used--;
    }
    uart_timeouts = 0;
}

static void serial_drain(void)
{
    unsigned polls = 0;
    for (;;) {
        uint32_t flags = irq_save();
        /* An interrupt may have started playback since this drain began. */
        if (!uart_ok || uart_buffering) {
            irq_restore(flags);
            return;
        }
        serial_notice();
        if (!uart_used) {
            irq_restore(flags);
            return;
        }
        /* Release IF between polls, but keep THRE, FIFO writes and head
         * advancement together. A nested sender also uses this same head;
         * no sender retains a producer-buffer byte across an interrupt. */
        if (inb(uart_base + 5) & 0x20) {
            serial_burst();
            polls = 0;
        } else if (++polls == UART_POLL_LIMIT) {
            polls = 0;
            if (++uart_timeouts >= UART_DEAD_LIMIT) uart_ok = false;
        }
        irq_restore(flags);
    }
}

void serial_write(const char *s, size_t n)
{
    uint32_t flags = irq_save();
    if (!uart_ok || !n) {
        irq_restore(flags);
        return;
    }
    bool synchronous = !uart_buffering && !uart_used && !uart_drop_notice;
    size_t bytes = n;
    if (n <= UART_LOG_BYTES)
        for (size_t i = 0; i < n; i++) if (s[i] == '\n') bytes++;
    /* Append a whole record under one lock, including CRLF expansion.
     * Physical IF clear also describes ordinary IRQ/critical-section logs:
     * playback must queue those without waiting for the UART. */
    if (bytes > UART_LOG_BYTES - uart_used) {
        uart_dropped++;
        uart_drop_notice = true;
    } else for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            uart_log[(uart_head + uart_used) % UART_LOG_BYTES] = '\r';
            uart_used++;
        }
        uart_log[(uart_head + uart_used) % UART_LOG_BYTES] = s[i];
        uart_used++;
    }
    irq_restore(flags);
    if (synchronous) serial_drain();
}
void serial_buffered(bool enabled)
{
    uint32_t flags = irq_save(); uart_buffering = enabled; irq_restore(flags);
}

void serial_panic(void)
{
    /* Explicit panic entry, called with physical IF clear. Make room for
     * panic records by draining all older records and the drop notice. */
    serial_buffered(false);
    serial_drain();
}

void serial_tick(void)
{
    /* Called from the existing 1 ms timer. No waits, allocation, logging or
     * formatting; THRE admits a burst only into an empty detected FIFO. */
    uint32_t flags = irq_save();
    if (uart_ok && (inb(uart_base + 5) & 0x20)) {
        serial_notice();
        serial_burst();
    }
    irq_restore(flags);
}
