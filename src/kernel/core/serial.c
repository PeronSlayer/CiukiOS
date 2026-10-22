/* IRQ-owned 16550 transmitter; producers publish bounded whole records.
 * Research/decisions: docs/validation/f0-29-serial-interrupts.md and
 * docs/validation/f0-38-loader-uart.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/timing.h>
#include <ciuki/sync.h>
#include <ciuki/work.h>
extern bool com_legacy_attach(unsigned, irq_handler_t) __attribute__((weak));

#define UART_POLL_LIMIT 20000u   /* panic only */
#define UART_INIT_POLL_LIMIT 4096u
#define UART_DEAD_LIMIT 8u
#define UART_LSR_THRE 0x20u
#define UART_LSR_TEMT 0x40u
#define UART_IER_THRE 0x02u
#define UART_IIR_FIFO 0xC0u
#define UART_LOG_BYTES 65536u
#define UART_RECORD_BYTES (2u * SERIAL_RECORD_MAX)
#define UART_RECORD_SLOTS 1024u
#define UART_EVIDENCE_BYTES (2u * UART_RECORD_BYTES)
#define UART_EVIDENCE_SLOTS 64u

static uint16_t uart_base;
static enum serial_probe_result uart_probe_result;
static bool uart_ok, uart_irq_started, uart_armed, uart_panic_mode, uart_evidence;
static unsigned uart_irq, uart_fifo_bytes = 1;
static char uart_log[UART_LOG_BYTES];
static uint16_t uart_records[UART_RECORD_SLOTS];
static uint64_t uart_accepted, uart_sent;
static unsigned uart_head, uart_used, uart_dropped;
static unsigned uart_periodic_dropped, uart_evidence_dropped;
static unsigned uart_record_head, uart_record_count;
static struct kwait uart_space;

void serial_init(uint16_t base, uint16_t divisor)
{
    uart_ok = uart_irq_started = uart_armed = uart_panic_mode = uart_evidence = false;
    uart_base = base;
    uart_head = uart_used = uart_dropped = 0;
    uart_periodic_dropped = uart_evidence_dropped = 0;
    uart_accepted = uart_sent = 0;
    uart_record_head = uart_record_count = 0;
    uart_fifo_bytes = 1;
    kwait_init(&uart_space);
    uart_probe_result = SERIAL_PROBE_MAPPING;
    /* The boot handoff supplies no independent IRQ number. Do not guess
     * a route for a nonstandard firmware port. */
    switch (base) {
    case 0x3F8: case 0x3E8: uart_irq = 4; break;
    case 0x2F8: case 0x2E8: uart_irq = 3; break;
    default: return;
    }
    uart_probe_result = SERIAL_PROBE_SCRATCH;
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
    uart_probe_result = SERIAL_PROBE_LSR;
    if (inb(base + 5) == 0xFF) return;
    if (!divisor) divisor = 3;      /* 38400 baud, 1.8432 MHz reference */
    outb(base + 3, 0x03);           /* clear inherited DLAB before IER */
    outb(base + 1, 0x00);           /* startup hook enables TX IRQ later */
    outb(base + 3, 0x80);
    outb(base + 0, divisor & 0xFF);
    outb(base + 1, divisor >> 8);
    uint8_t low = inb(base + 0), high = inb(base + 1);
    outb(base + 3, 0x03);           /* 8N1 */
    uart_probe_result = SERIAL_PROBE_DIVISOR;
    if (low != (divisor & 0xFF) || high != (divisor >> 8)) return;
    /* Preserve the hardware off/on/sticky-enable FIFO probe. Broken or
     * unverified FIFOs use one holding byte, including an original 16550. */
    outb(base + 2, 0x00);
    uint8_t fifo_off = inb(base + 2) & UART_IIR_FIFO;
    outb(base + 2, 0x07);
    uint8_t fifo_on = inb(base + 2) & UART_IIR_FIFO;
    outb(base + 2, 0x01);
    if (!fifo_off && fifo_on == UART_IIR_FIFO &&
        (inb(base + 2) & UART_IIR_FIFO) == UART_IIR_FIFO)
        uart_fifo_bytes = 16;
    else
        outb(base + 2, 0x00);
    outb(base + 4, 0x0B);           /* DTR/RTS + PC IRQ output gate OUT2 */
    uart_probe_result = SERIAL_PROBE_THRE_TIMEOUT;
    for (unsigned i = 0; i < UART_INIT_POLL_LIMIT; i++) {
        uint8_t lsr = inb(base + 5);
        if (lsr == 0xFF) { uart_probe_result = SERIAL_PROBE_LSR; return; }
        if (lsr & UART_LSR_THRE) {
            uart_probe_result = SERIAL_PROBE_OK;
            uart_ok = true;
            return;
        }
    }
}

bool serial_present(void) { return uart_ok; }
unsigned serial_fifo_size(void) { return uart_fifo_bytes; }

void serial_boot_init(void)
{
    /* Keep the loader's diagnostic byte immutable and log its original
     * fields before replacing the runtime resource map. */
    uint32_t base = g_boot.uart_base, divisor = g_boot.uart_divisor;
    bool recover = g_boot.uart_state != CBI_UART_OK;
    serial_init(recover ? 0x3F8 : (uint16_t)base, recover ? 3 : (uint16_t)divisor);
    if (uart_ok) {
        g_boot.uart_base = uart_base;
        g_boot.uart_divisor = recover || !divisor ? 3 : divisor;
        if (recover) g_boot.flags |= CBI_F_SERIAL_LOG;
    } else {
        g_boot.uart_base = g_boot.uart_divisor = 0;
        g_boot.flags &= ~CBI_F_SERIAL_LOG;
    }
    klog("[serial] loader_uart state=%u base=%04x divisor=%u",
         (unsigned)g_boot.uart_state, base, divisor);
    klog("[serial] kernel_probe result=%u base=%04x divisor=%u",
         (unsigned)uart_probe_result, g_boot.uart_base, g_boot.uart_divisor);
}

/* IF is already clear. Only the UART IRQ or panic owns this cursor. */
static void serial_submit(unsigned count)
{
    if (count > uart_used) count = uart_used;
    for (unsigned i = 0; i < count; i++) {
        outb(uart_base, (uint8_t)uart_log[uart_head]);
        uart_head = (uart_head + 1) % UART_LOG_BYTES;
        uart_used--;
        if (!--uart_records[uart_record_head]) {
            /* Ticket completion means FIFO submission, not remote receipt. */
            uart_sent++;
            uart_record_head = (uart_record_head + 1) % UART_RECORD_SLOTS;
            uart_record_count--;
        }
    }
}

static void serial_irq(struct trap_frame *tf)
{
    if (!uart_ok || uart_panic_mode) return;
    if (crit_label) crit_label("serial_irq");
    if (!tf) {
        /* PIT idle restart only. No polling/readiness/data I/O. Producers
         * leave this to an IRQ context so publication never touches ports. */
        if (!uart_armed && uart_used) {
            uart_armed = true;
            outb(uart_base + 1, UART_IER_THRE);
        }
        return;
    }
    /* One identification and at most one FIFO load per interrupt, even
     * on an immediately-ready chardev. Never consume RX or issue PIC EOI. */
    if ((inb(uart_base + 2) & 0x0F) != 0x02) return;
    serial_submit(uart_fifo_bytes);
    if (!uart_used) {
        outb(uart_base + 1, 0);
        uart_armed = false;
    }
    kwait_wake_irq(&uart_space, false);
}

void serial_writer_init(void)
{
    /* Existing scheduler startup hook, after PIC/registry setup, before
     * the first STI. No writer task, allocation or synchronous drain. */
    if (!uart_ok || uart_irq_started || uart_panic_mode) return;
    uint32_t f = irq_save();
    if (!com_legacy_attach || !com_legacy_attach(uart_irq, serial_irq))
        irq_set_handler(uart_irq, serial_irq);
    uart_irq_started = true;
    pic_unmask(uart_irq);
    serial_irq(NULL);
    irq_restore(f);
}

void serial_tick(void)
{
    /* Called by IRQ0. Active TX never depends on the PIT cadence. */
    if (uart_irq_started && !uart_armed && uart_used)
        serial_irq(NULL);
}

static bool serial_room(void *arg)
{
    size_t bytes = *(const size_t *)arg;
    return !uart_ok || uart_panic_mode ||
        (bytes <= UART_LOG_BYTES - uart_used && uart_record_count < UART_RECORD_SLOTS);
}

uint32_t serial_evidence_begin(size_t bytes)
{
    for (;;) {
        uint32_t f = irq_save();
        if ((f & 0x200) && crit_label) crit_label("serial_queue");
        if (!uart_irq_started || bytes > UART_RECORD_BYTES || serial_room(&bytes) ||
            !(f & 0x200) || !g_current || g_current->state != T_RUNNING ||
            g_current->preempt_holds || (kwork_current && kwork_current())) {
            uart_evidence = true;
            return f; /* caller publishes exactly once, then restores IF */
        }
        irq_restore(f);
        /* kwait registers and rechecks atomically, including a refill that
         * happens between our capacity check and waiter registration. */
        kwait_wait_until(&uart_space, serial_room, &bytes, deadline_after_ms(1000));
    }
}

/* Panic alone polls, with IF already clear. No cursor rollback or FIFO reset. */
static void serial_drain(void)
{
    unsigned polls = 0;
    while (uart_ok && uart_used) {
        uint8_t lsr = inb(uart_base + 5);
        if (lsr != 0xFF && (lsr & UART_LSR_THRE)) {
            serial_submit(uart_fifo_bytes);
            polls = 0; /* finite queue, finite wait for each FIFO load */
        } else if (++polls == UART_POLL_LIMIT * UART_DEAD_LIMIT) {
            uart_ok = false;
        }
    }
    if (!uart_ok) return;
    for (unsigned i = 0; i < UART_POLL_LIMIT; i++) {
        uint8_t lsr = inb(uart_base + 5);
        if (lsr != 0xFF && (lsr & UART_LSR_TEMT)) return;
    }
    uart_ok = false;
}

uint64_t serial_write_ticket(const char *s, size_t n)
{
    size_t bytes = n;
    if (n <= SERIAL_RECORD_MAX)
        for (size_t i = 0; i < n; i++) if (s[i] == '\n') bytes++;
    uint32_t f = irq_save();
    if ((f & 0x200) && crit_label) crit_label("serial_queue");
    bool evidence = uart_evidence || uart_panic_mode;
    uart_evidence = false;
    if (!uart_ok || !n) {
        irq_restore(f);
        return 0;
    }
    unsigned capacity = evidence ? UART_LOG_BYTES : UART_LOG_BYTES - UART_EVIDENCE_BYTES;
    unsigned slots = evidence ? UART_RECORD_SLOTS : UART_RECORD_SLOTS - UART_EVIDENCE_SLOTS;
    uint64_t ticket = 0;
    if (n > SERIAL_RECORD_MAX || bytes > capacity || uart_used > capacity - bytes ||
        uart_record_count >= slots) {
        uart_dropped++;
        if (evidence) uart_evidence_dropped++;
        else uart_periodic_dropped++;
    } else {
        ticket = ++uart_accepted;
        uart_records[(uart_record_head + uart_record_count) % UART_RECORD_SLOTS] = (uint16_t)bytes;
        uart_record_count++;
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '\n') {
                uart_log[(uart_head + uart_used) % UART_LOG_BYTES] = '\r';
                uart_used++;
            }
            uart_log[(uart_head + uart_used) % UART_LOG_BYTES] = s[i];
            uart_used++;
        }
    }
    irq_restore(f);
    if (uart_panic_mode) serial_drain();
    return ticket;
}
void serial_write(const char *s, size_t n) { (void)serial_write_ticket(s, n); }
void serial_buffered(bool enabled) { (void)enabled; }

void serial_panic(void)
{
    /* All production panic entries already have IF clear. */
    uart_panic_mode = true;
    if (uart_ok) outb(uart_base + 1, 0);
    uart_armed = false;
    serial_drain();
}

uint32_t serial_dropped(void)
{
    uint32_t f = irq_save(), n = uart_dropped;
    irq_restore(f);
    return n;
}
void serial_snapshot(struct serial_stats *out)
{
    uint32_t f = irq_save();
    *out = (struct serial_stats){ .accepted = uart_accepted, .sent = uart_sent,
        .queued = uart_used, .dropped = uart_dropped, .present = uart_ok,
        .periodic_dropped = uart_periodic_dropped, .evidence_dropped = uart_evidence_dropped,
        .resent = 0, .probe_result = uart_probe_result };
    /* Accepted bytes have one forward-only submission cursor. */
    irq_restore(f);
}
