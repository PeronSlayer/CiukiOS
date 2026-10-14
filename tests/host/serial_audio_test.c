/* F4-14/F4-16: production serial queue and a clocked 38400 8N1 UART.
 * Deterministic IRQ preemption plus two host producers and a burst sender.
 * The mutex models the kernel's uniprocessor IRQ lock, not an SMP UART. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#define CIUKI_CPU_H
#include <ciuki/kernel.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#c); exit(1); } } while (0)

static pthread_mutex_t cpu_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local uint32_t flags = 0x200;
static _Thread_local bool playback_producer;
static void preempt_writer(void);
static uint32_t irq_save(void)
{
    uint32_t f = flags;
    if (f & 0x200) CHECK(!pthread_mutex_lock(&cpu_lock));
    flags = 0;
    return f;
}
static void irq_restore(uint32_t f)
{
    flags = f;
    if (f & 0x200) {
        CHECK(!pthread_mutex_unlock(&cpu_lock));
        preempt_writer();
    }
}

static uint8_t regs[8];
static bool present = true, ready, fifo;
static bool fifo_enabled, timed, poll_clock;
static unsigned iir_fault;
static unsigned lsr_reads, filled;
static char emitted[200000];
static unsigned emitted_count;
/* Separate holding FIFO and shift register. LSR reads never empty these.
 * Time is in baud-scaled microseconds: ten 8N1 bits cost 10,000,000 units. */
static uint8_t holding[16], shift;
static unsigned holding_used, overflows, tx_writes;
static bool shifting;
static uint64_t clock_units, shift_end;
static void advance_us(unsigned us)
{
    uint64_t target = clock_units + (uint64_t)us * 38400;
    while (shifting && shift_end <= target) {
        CHECK(emitted_count < sizeof(emitted) - 1);
        emitted[emitted_count++] = (char)shift;
        if (holding_used) {
            shift = holding[0]; holding_used--;
            memmove(holding, holding + 1, holding_used);
            shift_end += 10000000;
        } else shifting = false;
    }
    clock_units = target;
}
static void (*preemption)(void);
static unsigned preempt_at, preemptions;
static void preempt_writer(void)
{
    if ((flags & 0x200) && preemption && emitted_count >= preempt_at) {
        void (*fn)(void) = preemption;
        preemption = NULL;
        preemptions++;
        fn();
    }
}
static uint8_t inb(uint16_t p)
{
    CHECK(p >= 0x3F8 && p < 0x400);
    if (!present) return 0xFF;
    unsigned off = p - 0x3F8;
    if (off == 2) {
        if (iir_fault == 2) return 0xC0; /* stuck FIFO indication */
        if (iir_fault == 3 && regs[2] == 1) return 0; /* enable did not stick */
        return fifo && fifo_enabled ? (iir_fault == 1 ? 0x80 : 0xC0) : 0;
    }
    if (off == 5) {
        CHECK(!playback_producer && !(flags & 0x200));
        lsr_reads++;
        if (timed) {
            if (poll_clock) advance_us(1);
            return ready ? ((!holding_used ? 0x20 : 0) | (!shifting ? 0x40 : 0)) : 0;
        }
        if (ready) filled = 0;
        return ready ? 0x20 : 0;
    }
    return regs[off];
}
static void outb(uint16_t p, uint8_t value)
{
    CHECK(p >= 0x3F8 && p < 0x400);
    unsigned off = p - 0x3F8;
    if (off == 2) {
        fifo_enabled = fifo && (value & 1) && iir_fault != 1;
        /* Original 16550 advertises a broken FIFO, but cannot buffer it. */
        if (iir_fault == 1) fifo_enabled = (value & 1) != 0;
        if (value & 4) holding_used = 0; /* TX reset does not clear TSR */
    }
    if (off == 0 && !(regs[3] & 0x80)) {
        CHECK(!(flags & 0x200) && ready);
        tx_writes++;
        if (timed) {
            if (!shifting) {
                shifting = true; shift = value; shift_end = clock_units + 10000000;
            } else if (holding_used < (fifo_enabled && iir_fault != 1 ? 16u : 1u))
                holding[holding_used++] = value;
            else overflows++; /* deliberately observable loss, no assertion */
        } else {
            CHECK(emitted_count < sizeof(emitted) - 1);
            CHECK(++filled <= (fifo_enabled ? 16u : 1u));
            emitted[emitted_count++] = (char)value;
        }
    }
    regs[off] = value;
    preempt_writer();
}
#include "../../src/kernel/core/serial.c"

static void reset_uart(bool with_fifo)
{
    CHECK(flags == 0x200 && !preemption);
    memset(regs, 0, sizeof(regs));
    fifo = with_fifo; present = ready = true; fifo_enabled = timed = poll_clock = false;
    iir_fault = 0; holding_used = overflows = tx_writes = 0; shifting = false;
    clock_units = shift_end = 0;
    emitted_count = filled = lsr_reads = preemptions = 0;
    serial_init(0x3F8, 3);
    CHECK(serial_present() && uart_fifo_bytes == (fifo ? 16u : 1u));
    CHECK(fifo_enabled == fifo && regs[2] == (fifo ? 1 : 0));
}
static void tick(void)
{
    uint32_t f = irq_save();
    unsigned before = lsr_reads, at = tx_writes;
    serial_tick();
    CHECK(lsr_reads == before + (uart_ok ? 1u : 0u));
    CHECK(tx_writes - at <= uart_fifo_bytes);
    irq_restore(f);
}
static void drain_uart(void)
{
    ready = true;
    for (unsigned i = 0; i < sizeof(emitted) && (uart_used || uart_drop_notice); i++) tick();
    CHECK(!uart_used && !uart_drop_notice);
    emitted[emitted_count] = 0;
}

static const char task_record[] = "[task] complete record A with a long payload\n";
static const char irq_record[] = "[irq] complete record B\n";
static const char expected_pair[] =
    "[task] complete record A with a long payload\r\n[irq] complete record B\r\n";
static unsigned preempt_mode;
static void second_producer(void)
{
    if (preempt_mode == 3) {
        /* An IF-enabled producer queues while the older synchronous writer
         * is suspended; the timer sends a burst before that writer resumes. */
        serial_buffered(true);
        serial_write(irq_record, sizeof(irq_record) - 1);
        tick();
        return;
    }
    uint32_t f = irq_save();
    if (preempt_mode == 1) serial_buffered(true);
    if (preempt_mode == 2) serial_panic();
    serial_write(irq_record, sizeof(irq_record) - 1);
    serial_tick();
    irq_restore(f);
}
static void preemption_tests(bool with_fifo)
{
    /* Request an interrupt at every byte boundary of a synchronous record.
     * Actual preemption waits for IF restoration after the atomic burst. */
    for (preempt_mode = 0; preempt_mode < 4; preempt_mode++) {
        for (unsigned at = 1; at < sizeof(task_record) - 1; at++) {
            reset_uart(with_fifo);
            preempt_at = at; preemption = second_producer;
            serial_write(task_record, sizeof(task_record) - 1);
            drain_uart();
            CHECK(preemptions == 1 && !strcmp(emitted, expected_pair));
        }
    }
}

static const char drop_notice[] = "[serial] result=log_dropped\r\n";
static void basic_tests(bool with_fifo)
{
    reset_uart(with_fifo);
    serial_write("boot\n", 5);
    emitted[emitted_count] = 0;
    CHECK(!strcmp(emitted, "boot\r\n"));
    serial_buffered(true); ready = false;
    unsigned before = lsr_reads;
    playback_producer = true;
    serial_write(task_record, sizeof(task_record) - 1);
    uint32_t f = irq_save();
    serial_write(irq_record, sizeof(irq_record) - 1);
    irq_restore(f);
    playback_producer = false;
    CHECK(lsr_reads == before && flags == 0x200);
    tick();
    CHECK(lsr_reads == before + 1 && emitted_count == 6);
    ready = true; tick();
    CHECK(filled == (fifo ? 16u : 1u));
    serial_buffered(false);
    before = lsr_reads;
    serial_write("tail\n", 5); /* Queue backlog remains nonblocking and ordered. */
    CHECK(lsr_reads == before);
    drain_uart();
    CHECK(!strcmp(emitted, "boot\r\n[task] complete record A with a long payload\r\n"
                          "[irq] complete record B\r\ntail\r\n"));

    /* The queue owns the source bytes after serial_write returns. */
    reset_uart(with_fifo); serial_buffered(true);
    char source[] = "[source] original record\n";
    serial_write(source, sizeof(source) - 1);
    memset(source, '?', sizeof(source));
    drain_uart();
    CHECK(!strcmp(emitted, "[source] original record\r\n"));

    /* Advance the head near the end, then wrap one entire record. */
    reset_uart(with_fifo); serial_buffered(true);
    static char padding[UART_LOG_BYTES - 32];
    memset(padding, 'P', sizeof(padding)); padding[sizeof(padding) - 1] = '\n';
    serial_write(padding, sizeof(padding)); drain_uart();
    CHECK(uart_head + sizeof(task_record) > UART_LOG_BYTES);
    unsigned start = emitted_count;
    serial_write(task_record, sizeof(task_record) - 1); drain_uart();
    CHECK(!strcmp(emitted + start, "[task] complete record A with a long payload\r\n"));

    /* Exact capacity includes CRLF; overflow cannot append even a prefix. */
    reset_uart(with_fifo); serial_buffered(true); ready = false;
    static char full[UART_LOG_BYTES];
    memset(full, 'X', sizeof(full)); full[sizeof(full) - 2] = '\n';
    serial_write(full, sizeof(full) - 1);
    CHECK(uart_used == UART_LOG_BYTES);
    serial_write("DROP\n", 5);
    serial_write("DROP-AGAIN\n", 11);
    CHECK(uart_dropped == 2 && uart_used == UART_LOG_BYTES && !lsr_reads);
    drain_uart();
    CHECK(emitted_count == UART_LOG_BYTES + sizeof(drop_notice) - 1);
    for (unsigned i = 0; i < UART_LOG_BYTES - 2; i++) CHECK(emitted[i] == 'X');
    CHECK(!strcmp(emitted + UART_LOG_BYTES - 2, "\r\n[serial] result=log_dropped\r\n"));

    /* CRLF expansion, including many newlines, can make a record too big. */
    reset_uart(with_fifo); serial_buffered(true); ready = false;
    memset(full, '\n', sizeof(full)); serial_write(full, sizeof(full));
    serial_write(full, SIZE_MAX);
    CHECK(!uart_used && uart_dropped == 2 && !lsr_reads);
    drain_uart(); CHECK(!strcmp(emitted, drop_notice));
}

static void panic_tests(bool with_fifo)
{
    reset_uart(with_fifo); serial_buffered(true); ready = false;
    static char full[UART_LOG_BYTES];
    memset(full, 'X', sizeof(full)); full[sizeof(full) - 2] = '\n';
    serial_write(full, sizeof(full) - 1); serial_write("DROP\n", 5);
    ready = true; tick(); /* Panic after a partial queued FIFO burst. */
    uint32_t f = irq_save();
    serial_panic();
    CHECK(!uart_used && !uart_drop_notice && !uart_buffering && flags == 0);
    serial_write("panic\n", 6);
    CHECK(!uart_used && flags == 0);
    irq_restore(f);
    emitted[emitted_count] = 0;
    CHECK(emitted_count == UART_LOG_BYTES + sizeof(drop_notice) - 1 + 7);
    for (unsigned i = 0; i < UART_LOG_BYTES - 2; i++) CHECK(emitted[i] == 'X');
    CHECK(!strcmp(emitted + UART_LOG_BYTES - 2, "\r\n[serial] result=log_dropped\r\npanic\r\n"));

    /* A busy UART remains bounded; unsent bytes are never consumed. */
    reset_uart(with_fifo); serial_buffered(true); ready = false;
    serial_write(task_record, sizeof(task_record) - 1);
    f = irq_save(); serial_panic(); irq_restore(f);
    CHECK(!serial_present() && !emitted_count && uart_used == sizeof(task_record));
    CHECK(lsr_reads == UART_POLL_LIMIT * UART_DEAD_LIMIT);
}

enum { PRODUCERS = 2, RECORDS = 300 };
static atomic_uint producers_done;
static size_t producer_record(char *s, size_t size, unsigned id, unsigned seqno)
{
    int n = snprintf(s, size, "[producer=%u] seq=%03u payload=", id, seqno);
    CHECK(n > 0 && (size_t)n + 106 < size);
    unsigned payload = 32 + seqno % 73;
    memset(s + n, 'A' + id, payload);
    n += (int)payload;
    s[n++] = '\n'; s[n] = 0;
    return (size_t)n;
}
static void *producer(void *arg)
{
    unsigned id = *(unsigned *)arg;
    playback_producer = true;
    for (unsigned seqno = 0; seqno < RECORDS; seqno++) {
        char record[192];
        size_t n = producer_record(record, sizeof(record), id, seqno);
        /* One producer models task context, the other ordinary IF-clear
         * IRQ/critical-section context. Neither may poll even a ready UART. */
        if (id) {
            uint32_t f = irq_save(); serial_write(record, n); irq_restore(f);
        } else serial_write(record, n);
        sched_yield();
    }
    playback_producer = false;
    atomic_fetch_add(&producers_done, 1);
    return NULL;
}
static void *sender(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t f = irq_save();
        bool done = atomic_load(&producers_done) == PRODUCERS && !uart_used && !uart_drop_notice;
        irq_restore(f);
        if (done) break;
        tick();
        sched_yield();
    }
    return NULL;
}
static void concurrent_tests(bool with_fifo)
{
    reset_uart(with_fifo); serial_buffered(true);
    atomic_store(&producers_done, 0);
    pthread_t workers[PRODUCERS], consumer;
    unsigned ids[PRODUCERS] = {0, 1};
    CHECK(!pthread_create(&consumer, NULL, sender, NULL));
    for (unsigned i = 0; i < PRODUCERS; i++) CHECK(!pthread_create(&workers[i], NULL, producer, &ids[i]));
    for (unsigned i = 0; i < PRODUCERS; i++) CHECK(!pthread_join(workers[i], NULL));
    CHECK(!pthread_join(consumer, NULL));
    CHECK(!uart_used && !uart_drop_notice && flags == 0x200);
    emitted[emitted_count] = 0;
    bool seen[PRODUCERS][RECORDS] = {{false}};
    unsigned kept = 0, notices = 0, last[PRODUCERS] = {0};
    for (const char *line = emitted; *line;) {
        const char *end = strstr(line, "\r\n");
        CHECK(end);
        size_t n = (size_t)(end + 2 - line);
        if (n == sizeof(drop_notice) - 1 && !memcmp(line, drop_notice, n)) notices++;
        else {
            unsigned id, seqno;
            CHECK(sscanf(line, "[producer=%u] seq=%u", &id, &seqno) == 2);
            CHECK(id < PRODUCERS && seqno < RECORDS && !seen[id][seqno]);
            CHECK(!last[id] || seqno + 1 > last[id]);
            last[id] = seqno + 1; seen[id][seqno] = true;
            char expected[192];
            size_t bytes = producer_record(expected, sizeof(expected), id, seqno);
            expected[bytes - 1] = '\r'; expected[bytes++] = '\n';
            CHECK(n == bytes && !memcmp(line, expected, bytes));
            kept++;
        }
        line = end + 2;
    }
    CHECK(kept + uart_dropped == PRODUCERS * RECORDS);
    CHECK((uart_dropped != 0) == (notices != 0));
    printf("serial concurrent %s: %u complete records, %u whole drops, %u notices PASS\n",
           fifo ? "16550" : "16450", kept, uart_dropped, notices);
}

static void line_rate_case(bool with_fifo, unsigned fault)
{
    reset_uart(with_fifo);
    iir_fault = fault;
    regs[7] = 0x39;
    serial_init(0x3F8, 3);
    CHECK(regs[7] == 0x39);
    timed = true; serial_buffered(true);
    char expected[8192]; unsigned length = 0;
    for (unsigned i = 0; i < 64; i++) {
        char record[96];
        int n = snprintf(record, sizeof(record), "[usb] controller=%02u payload=abcdefghijklmnopqrstuvwxyz0123456789\n", i);
        serial_write(record, (size_t)n);
        memcpy(expected + length, record, (size_t)n - 1); length += (unsigned)n - 1;
        expected[length++] = '\r'; expected[length++] = '\n';
    }
    CHECK(!uart_dropped && !lsr_reads);
    unsigned milliseconds = 0, busy_ticks = 0;
    for (; milliseconds < 10000 && (uart_used || shifting);) {
        unsigned before = tx_writes;
        bool busy = holding_used != 0;
        tick();
        if (busy) { CHECK(tx_writes == before); busy_ticks++; }
        /* Repeated readiness reads within one tick must not free space. */
        before = holding_used;
        uint32_t f = irq_save(); (void)inb(0x3FD); (void)inb(0x3FD); irq_restore(f);
        CHECK(holding_used == before);
        advance_us(1000); milliseconds++;
        CHECK((uint64_t)emitted_count * 100 <= (uint64_t)milliseconds * 384);
        CHECK(tx_writes - emitted_count <= (with_fifo && !fault ? 17u : 2u));
    }
    printf("serial 38400 %s IIR-fault=%u: %u wire bytes/%u ms, %u busy ticks, %u overflows\n",
           with_fifo ? "16550A" : "16450", fault, emitted_count, milliseconds, busy_ticks, overflows);
    CHECK(!uart_used && !shifting && !overflows && emitted_count == length);
    CHECK(!memcmp(emitted, expected, length));
    CHECK(uart_fifo_bytes == (with_fifo && !fault ? 16u : 1u));
    if (with_fifo && !fault) CHECK(busy_ticks); /* 16 bytes cannot drain in 1 ms */
}

static void line_rate_tests(void)
{
    /* Fault injection reproduces the chunk-loss hypothesis on a real FIFO:
     * force unchecked 16-bytes/ms bursts, including overfull writes. */
    reset_uart(true); timed = true;
    char original[4096]; unsigned n = 0;
    for (unsigned i = 0; i < 32; i++)
        n += (unsigned)snprintf(original + n, sizeof(original) - n,
                              "[usb] controller=%02u payload=abcdefghijklmnopqrstuvwxyz0123456789\r\n", i);
    for (unsigned at = 0; at < n;) {
        uint32_t f = irq_save();
        for (unsigned burst = 0; burst < 16 && at < n; burst++) outb(0x3F8, (uint8_t)original[at++]);
        irq_restore(f); advance_us(1000);
    }
    while (shifting) advance_us(1000);
    emitted[emitted_count] = 0;
    CHECK(overflows && emitted_count < n && strstr(emitted, "[usb]") && memcmp(emitted, original, emitted_count));
    printf("serial unchecked 16 bytes/ms: %u bytes lost, chunk-spliced records reproduced PASS\n", overflows);
    line_rate_case(true, 0);
    line_rate_case(false, 0);
    line_rate_case(true, 1);   /* broken original 16550, FIFO bits = 10 */
    line_rate_case(false, 2);  /* stuck IIR=11 without a working FIFO */
    line_rate_case(true, 3);   /* reset readback passed, persistent enable failed */

    /* Boot/panic use the same hardware gate while polling a running line. */
    reset_uart(true); timed = poll_clock = true;
    serial_write(task_record, sizeof(task_record) - 1);
    while (shifting) advance_us(1000);
    CHECK(!overflows && emitted_count == sizeof(task_record));
    CHECK(!memcmp(emitted, "[task] complete record A with a long payload\r\n", emitted_count));
}

int main(void)
{
    line_rate_tests();
    for (unsigned kind = 0; kind < 2; kind++) {
        preemption_tests(kind);
        basic_tests(kind);
        panic_tests(kind);
        concurrent_tests(kind);
    }
    reset_uart(true); present = false; serial_init(0x3F8, 3);
    CHECK(!serial_present());
    unsigned before = lsr_reads;
    serial_buffered(true); serial_write("absent\n", 7); tick();
    uint32_t f = irq_save(); serial_panic(); irq_restore(f);
    CHECK(lsr_reads == before && !uart_used);
    puts("serial audio: IRQ preemption/mode transition, no producer polls, FIFO bursts, "
         "source ownership/wrap/whole drops, ordered panic/timeout/absence PASS");
    return 0;
}
