/* 16550-compatible UART output with a nonblocking record queue.
 * docs/design/device-firmware-ownership.md: presence probe, bounded polls,
 * absence disables the sink without delaying boot.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>

#define UART_POLL_LIMIT 20000u   /* per readiness wait, panic only */
#define UART_DEAD_LIMIT 8u       /* consecutive timeouts before disabling */
#define UART_LSR_THRE 0x20u
#define UART_IIR_FIFO 0xC0u

static uint16_t uart_base;
static bool uart_ok;
static unsigned uart_timeouts;
#define UART_LOG_BYTES 65536u
#define UART_RECORD_BYTES (2u * SERIAL_RECORD_MAX)
#define UART_RECORD_SLOTS 1024u
#define UART_NOTICE_BYTES 64u
static char uart_log[UART_LOG_BYTES];
static uint16_t uart_records[UART_RECORD_SLOTS];
static unsigned uart_head, uart_used, uart_dropped;
static unsigned uart_record_head, uart_record_count;
static unsigned uart_budget, uart_admitted, uart_burst_bytes;
static uint64_t uart_window, uart_burst_tick;
static bool uart_notice_sent, uart_writer_started;
static bool uart_drop_notice, uart_panic_mode;
static unsigned uart_fifo_bytes = 1;

void serial_init(uint16_t base, uint16_t divisor)
{
    uart_ok = false;
    uart_base = base;
    uart_head = uart_used = uart_dropped = 0;
    uart_record_head = uart_record_count = uart_admitted = 0;
    uart_window = g_ticks;
    uart_burst_tick = UINT64_MAX;
    uart_notice_sent = false;
    uart_writer_started = false;
    uart_drop_notice = uart_panic_mode = false;
    uart_fifo_bytes = 1;
    uart_timeouts = 0;
    if (!base)
        return;
    /* Presence/type probe: retain the firmware's scratch value. */
    uint8_t scratch = inb(base + 7);
    outb(base + 7, 0x5A);
    if (inb(base + 7) != 0x5A) {
        outb(base + 7, scratch);
        return;
    }
    outb(base + 7, 0xA5);
    if (inb(base + 7) != 0xA5) {
        outb(base + 7, scratch);
        return;
    }
    outb(base + 7, scratch);
    if (!divisor)
        divisor = 3;                /* 38400 baud with 1.8432 MHz clock */
    /* 8N1: ten bits per byte. Admit 75% of line capacity, reserving one
     * notice per second. Pace the wire too, including an always-ready UART:
     * at 38400 this is at most three bytes per PIT tick, not 16 bytes/ms. */
    unsigned line_bytes = 115200u / divisor / 10u;
    uart_budget = line_bytes * 3u / 4u;
    uart_burst_bytes = (uart_budget + 999u) / 1000u;
    if (!uart_burst_bytes) uart_burst_bytes = 1;
    outb(base + 3, 0x03);           /* clear inherited DLAB before IER */
    outb(base + 1, 0x00);           /* no UART interrupts */
    outb(base + 3, 0x80);           /* DLAB */
    outb(base + 0, divisor & 0xFF);
    outb(base + 1, divisor >> 8);
    outb(base + 3, 0x03);           /* 8N1 */
    /* FCR is write-only: verify an off/on transition through IIR, then
     * verify that FIFO enable survives clearing the self-clearing reset
     * bits. A 16450, broken original 16550 (IIR=10), or unverified FIFO
     * stays in character mode. Never assume a 16-byte holding register. */
    outb(base + 2, 0x00);
    uint8_t fifo_off = inb(base + 2) & UART_IIR_FIFO;
    outb(base + 2, 0x07);           /* enable, clear RX/TX FIFOs */
    uint8_t fifo_on = inb(base + 2) & UART_IIR_FIFO;
    outb(base + 2, 0x01);           /* leave enable explicitly asserted */
    if (!fifo_off && fifo_on == UART_IIR_FIFO &&
        (inb(base + 2) & UART_IIR_FIFO) == UART_IIR_FIFO)
        uart_fifo_bytes = 16;
    else
        outb(base + 2, 0x00);
    outb(base + 4, 0x03);           /* DTR, RTS */
    uart_ok = true;
    uart_timeouts = 0;
}

bool serial_present(void) { return uart_ok; }

static void serial_window(void)
{
    if (g_ticks - uart_window >= 1000) {
        uart_window = g_ticks;
        uart_admitted = 0;
        uart_notice_sent = false;
    }
}

/* All callers hold the UP IRQ lock. Publish notices only between records. */
static void serial_notice(void)
{
    serial_window();
    if (!uart_used && uart_drop_notice && (!uart_notice_sent || uart_panic_mode)) {
        char notice[64];
        unsigned n = (unsigned)ksnprintf(notice, sizeof(notice), "[serial] dropped=%u\r\n", uart_dropped);
        memcpy(uart_log, notice, n);
        uart_head = 0;
        uart_used = n;
        uart_record_head = 0;
        uart_record_count = 1;
        uart_records[0] = (uint16_t)n;
        uart_drop_notice = false;
        uart_notice_sent = true;
    }
}

static bool serial_burst(void)
{
    /* The log-writer alone consumes normally. IRQs only publish. Panic
     * takes over with IF clear and never resumes the suspended task.
     * Select one record's remaining bytes; no FIFO gate crosses records. */
    uint32_t flags = irq_save();
    bool panic_owner = uart_panic_mode;
    if (!uart_ok || !uart_used || (!uart_panic_mode && uart_burst_tick == g_ticks)) {
        irq_restore(flags);
        return false;
    }
    unsigned count = uart_records[uart_record_head];
    uint64_t tick = g_ticks;
    if (count > uart_fifo_bytes) count = uart_fifo_bytes;
    if (!uart_panic_mode && count > uart_burst_bytes) count = uart_burst_bytes;
    irq_restore(flags);
    /* Port reads can be slow on hardware. No other normal consumer can
     * invalidate this observation, so readiness needs no IRQ lock. */
    uint8_t lsr = inb(uart_base + 5);
    if (lsr == 0xFF || !(lsr & UART_LSR_THRE)) return false;
    uart_burst_tick = tick;
    for (unsigned i = 0; i < count; i++) {
        flags = irq_save();
        if (uart_panic_mode != panic_owner) {
            irq_restore(flags);
            return false; /* panic takeover never returns on the kernel */
        }
        if ((flags & 0x200) && crit_label) crit_label("serial_tx_byte");
        /* Only one port write per IRQ lock, including its cursor update.
         * A panic at the next IF boundary sees exactly the unsent suffix. */
        outb(uart_base, (uint8_t)uart_log[uart_head]);
        uart_head = (uart_head + 1) % UART_LOG_BYTES;
        uart_used--;
        if (!--uart_records[uart_record_head]) {
            uart_record_head = (uart_record_head + 1) % UART_RECORD_SLOTS;
            uart_record_count--;
        }
        irq_restore(flags);
    }
    uart_timeouts = 0;
    return true;
}

static void serial_drain(void)
{
    unsigned polls = 0;
    for (;;) {
        uint32_t flags = irq_save();
        if (!uart_ok) {
            irq_restore(flags);
            return;
        }
        serial_notice();
        if (!uart_used) {
            irq_restore(flags);
            return;
        }
        irq_restore(flags);
        /* Panic only: normal producers and IRQs never enter this loop. */
        if (serial_burst()) {
            polls = 0;
        } else if (++polls == UART_POLL_LIMIT) {
            polls = 0;
            if (++uart_timeouts >= UART_DEAD_LIMIT) uart_ok = false;
        }
    }
}

uint32_t serial_evidence_begin(size_t bytes)
{
    for (;;) {
        uint32_t flags = irq_save();
        serial_window();
        unsigned budget = uart_budget > UART_NOTICE_BYTES ? uart_budget - UART_NOTICE_BYTES : 0;
        bool full = bytes > UART_LOG_BYTES - uart_used || uart_record_count == UART_RECORD_SLOTS;
        bool limited = bytes > budget || uart_admitted > budget - bytes;
        if (!uart_ok || uart_panic_mode || bytes > budget || !(flags & 0x200) ||
            !g_current || g_current->preempt_holds || (!full && !limited))
            return flags; /* caller restores IF after bounded publication */
        irq_restore(flags);
        task_sleep_ms(1); /* sole consumer remains the log-writer */
    }
}

void serial_write(const char *s, size_t n)
{
    /* Count/expand outside the IRQ lock. Current record producers are
     * bounded; an oversized caller is dropped before inspecting its bytes. */
    size_t bytes = n;
    if (n <= SERIAL_RECORD_MAX)
        for (size_t i = 0; i < n; i++) if (s[i] == '\n') bytes++;
    uint32_t flags = irq_save();
    if ((flags & 0x200) && crit_label) crit_label("serial_queue");
    if (!uart_ok || !n) {
        irq_restore(flags);
        return;
    }
    serial_window();
    /* Append a whole record under one lock, including CRLF expansion.
     * Physical IF clear also describes ordinary IRQ/critical-section logs:
     * playback must queue those without waiting for the UART. */
    unsigned data_budget = uart_budget > UART_NOTICE_BYTES ? uart_budget - UART_NOTICE_BYTES : 0;
    bool oversize = n > SERIAL_RECORD_MAX || bytes > UART_RECORD_BYTES ||
                    (!uart_panic_mode && bytes > data_budget);
    bool full = bytes > UART_LOG_BYTES - uart_used || uart_record_count == UART_RECORD_SLOTS;
    bool limited = !uart_panic_mode && (bytes > data_budget || uart_admitted > data_budget - bytes);
    if (oversize || full || limited) {
        uart_dropped++;
        uart_drop_notice = true;
    } else {
        uart_records[(uart_record_head + uart_record_count) % UART_RECORD_SLOTS] = (uint16_t)bytes;
        uart_record_count++;
        if (!uart_panic_mode) uart_admitted += (unsigned)bytes;
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '\n') {
                uart_log[(uart_head + uart_used) % UART_LOG_BYTES] = '\r';
                uart_used++;
            }
            uart_log[(uart_head + uart_used) % UART_LOG_BYTES] = s[i];
            uart_used++;
        }
    }
    irq_restore(flags);
    if (uart_panic_mode) serial_drain();
}
void serial_buffered(bool enabled)
{
    (void)enabled; /* compatibility: normal producers always queue */
}

void serial_panic(void)
{
    /* Explicit panic entry, called with physical IF clear. Make room for
     * panic records by draining all older records and the drop notice. */
    uart_panic_mode = true;
    serial_buffered(false);
    serial_drain();
}

void serial_tick(void)
{
    /* Compatibility entry: timer/IRQ paths never drain the UART. */
}

uint32_t serial_dropped(void)
{
    uint32_t f = irq_save(), n = uart_dropped;
    irq_restore(f);
    return n;
}

static void serial_writer(void *arg)
{
    (void)arg;
    for (;;) {
        /* One paced refill, even when an emulator keeps THRE asserted. */
        uint32_t f = irq_save();
        if (uart_ok && !uart_panic_mode) serial_notice();
        irq_restore(f);
        if (!uart_panic_mode) serial_burst();
        task_sleep_ms(1);
    }
}
void serial_writer_init(void)
{
    if (!uart_ok || uart_writer_started) return;
    struct task *t = task_create_kernel("log-writer", serial_writer, 0, P_DEVICE);
    if (!t) panic("serial: cannot create log-writer");
    uart_writer_started = true;
    task_start(t);
}
