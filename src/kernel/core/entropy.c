/* Boot seed and bounded UP IRQ handoff. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/arch.h>
#include <ciuki/cpu.h>
#include <ciuki/mm.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/entropy.h>
#include <ciuki/entropy_pool.h>
#include <ciuki/entropy_jent.h>

static struct entropy_pool pool;
static struct kmutex lock;
/* One CPU on the current kernel. SMP must allocate a buffer per CPU. */
static struct { uint32_t words[4], count, sources; } fast;
static bool booted, worker_started;
static unsigned reported_reseeds;
static bool reported_failure;
static uint64_t boot_start, boot_per_ms;
#define RAW_LIMIT 1048576u
#define RAW_FRAME 64u
/* One bounded attempt cannot produce more samples than measurement rounds.
 * Allocate only in the opt-in worker, never on the boot/IRQ paths. */
static uint8_t *raw;
static unsigned raw_count, raw_sent;
static bool raw_requested;
static uint64_t jitter_clock(void) { return read_tsc_serialized(); }
static uint64_t now_ms(void)
{
    uint32_t flags = irq_save(); uint64_t now = g_ticks; irq_restore(flags);
    return now;
}
static uint64_t collection_ms(void)
{
    return !g_current && boot_per_ms ? (jitter_clock() - boot_start) / boot_per_ms : now_ms();
}
static void yield(void) { if (g_current) kpreempt_point(); }
static void capture(uint64_t delta, uint64_t gcd, unsigned osr)
{
    (void)gcd; (void)osr;
    if (raw && raw_count < ENTROPY_COLLECT_ROUNDS && raw_count < RAW_LIMIT - raw_sent)
        raw[raw_count++] = (uint8_t)delta;
}
static const struct entropy_jent_ops ops = {jitter_clock, collection_ms, entropy_alloc, entropy_free, yield, capture};
static bool raw_enabled(void)
{
    const char *s = g_boot.options;
    size_t i = 0, size = sizeof(g_boot.options);
    while (i < size && s[i]) {
        while (i < size && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        size_t start = i;
        while (i < size && s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') i++;
        if (i - start == 12 && !memcmp(s + start, "entropyraw=1", 12)) return true;
    }
    return false;
}
static void raw_release(void)
{
    if (raw) { entropy_erase(raw, ENTROPY_COLLECT_ROUNDS); entropy_free(raw); raw = NULL; }
    raw_count = 0; raw_requested = false;
}
static uint64_t raw_publish(const char *line, size_t bytes)
{
    /* Serial only: avoid a million-sample console/disk log. Wait for queue
     * capacity in task context, with neither pool mutex nor IRQ lock held. */
    uint32_t flags = serial_evidence_begin ? serial_evidence_begin(bytes + 1) : irq_save();
    uint64_t ticket = serial_write_ticket(line, bytes);
    irq_restore(flags);
    return ticket;
}
static void raw_flush(void)
{
    static const char hex[] = "0123456789abcdef";
    char line[224], data[2 * RAW_FRAME + 1];
    uint64_t ticket = 0;
    for (unsigned at = 0; at < raw_count;) {
        unsigned n = raw_count - at;
        if (n > RAW_FRAME) n = RAW_FRAME;
        for (unsigned i = 0; i < n; i++) {
            uint8_t byte = raw[at + i];
            data[2 * i] = hex[byte >> 4]; data[2 * i + 1] = hex[byte & 15];
        }
        data[2 * n] = 0;
        int bytes = ksnprintf(line, sizeof(line), "[entropy] raw index=%u count=%u data=%s\n",
                             raw_sent, n, data);
        ticket = raw_publish(line, (size_t)bytes);
        if (!ticket) goto unavailable;
        raw_sent += n; at += n;
    }
    entropy_erase(raw, raw_count); raw_count = 0;
    if (raw_sent == RAW_LIMIT) {
        int bytes = ksnprintf(line, sizeof(line), "[entropy] raw complete samples=%u bits=8\n", raw_sent);
        ticket = raw_publish(line, (size_t)bytes);
        if (!ticket) goto unavailable;
    }
    /* Queue acceptance is not wire submission. Drain this batch before
     * sampling again; only the existing UART IRQ transmits bytes. */
    while (ticket) {
        struct serial_stats stats;
        serial_snapshot(&stats);
        if (!stats.present) goto unavailable;
        if (stats.sent >= ticket) break;
        task_sleep_ms(10);
    }
    if (raw_sent == RAW_LIMIT) raw_release();
    return;
unavailable:
    raw_release();
    klog("[entropy] raw stopped samples=%u reason=serial", raw_sent);
}
/* Private early-boot measurement: PIT IRQs/global calibration do not exist
 * yet. Mode-0 channel 2 is not used by a driver at this guard safe point.
 * Restore the speaker/gate byte on every exit; never touch channel 0. */
static uint64_t boot_clock_rate(void)
{
    uint8_t saved = inb(0x61);
    outb(0x61, saved & ~3u);
    outb(0x43, 0xb0);
    outb(0x42, 11932 & 255); outb(0x42, 11932 >> 8);
    uint64_t start = jitter_clock();
    outb(0x61, (saved & ~2u) | 1u);
    bool saw_low = false, complete = false;
    unsigned i;
    for (i = 0; i < 1000000; i++) {
        bool high = (inb(0x61) & 0x20) != 0;
        if (!high) saw_low = true;
        else if (saw_low) { complete = true; break; }
    }
    uint64_t end = jitter_clock();
    outb(0x61, saved);
    return complete && end > start ? (end - start) / 10 : 0;
}
static void record(void)
{
    klog("[entropy] sources=%u irq_mask=%04x estimate_bits=%u initialised_ms=%llu health=%s reseeds=%u",
         pool.sources, fast.sources, pool.ready ? ENTROPY_SEED_BITS : pool.estimate_bits,
         pool.initialised_ms, pool.failed ? "fail" : "ok", pool.reseeds);
    reported_reseeds = pool.reseeds; reported_failure = pool.failed;
}
static bool collect(void)
{
    uint8_t bytes[32];
    if (entropy_jent_read(bytes)) {
        kmutex_lock(&lock); entropy_pool_fail(&pool); kmutex_unlock(&lock);
        entropy_erase(bytes, sizeof(bytes));
        return false;
    }
    kmutex_lock(&lock);
    entropy_pool_credit(&pool, bytes, sizeof(bytes), now_ms());
    kmutex_unlock(&lock);
    entropy_erase(bytes, sizeof(bytes));
    return true;
}
void entropy_boot(void)
{
    if (booted) return;
    booted = true; entropy_pool_init(&pool); kmutex_init(&lock);
    raw_requested = raw_enabled();
    if (g_cpu_tsc) boot_per_ms = boot_clock_rate();
    boot_start = g_cpu_tsc ? jitter_clock() : 0;
    if (boot_per_ms) {
        entropy_jent_begin(&ops);
        pool.source_ready = entropy_jent_init() == 0;
        while (pool.source_ready && !pool.ready && !pool.failed) {
            /* No scheduler/mutex owner exists before stack-guard seeding. */
            uint8_t bytes[32];
            int e = entropy_jent_read(bytes);
            if (e) entropy_pool_fail(&pool);
            else entropy_pool_credit(&pool, bytes, sizeof(bytes), 0);
            entropy_erase(bytes, sizeof(bytes));
        }
    }
    if (!pool.ready) { entropy_pool_fail(&pool); entropy_jent_close(); }
    uint64_t cycles = g_cpu_tsc ? jitter_clock() - boot_start : 0;
    klog("[entropy] boot rounds=%u cycles=%llu ms=%llu", entropy_jent_rounds(), cycles,
         boot_per_ms ? cycles / boot_per_ms : 0);
    record();
}
void entropy_irq(unsigned irq)
{
    if (!booted || !g_cpu_tsc || irq >= 16) return;
    uint64_t cycles = read_tsc_unserialized();
    unsigned at = fast.count++ & 3;
    uint32_t word = fast.words[at];
    fast.words[at] = ((word << 7) | (word >> 25)) ^ (uint32_t)cycles ^
                     (uint32_t)(cycles >> 32) ^ (irq << 24);
    fast.sources |= 1u << irq;
}
int entropy_get(void *out, size_t bytes)
{
    if (g_current) kmutex_lock(&lock);
    int e = entropy_pool_read(&pool, out, bytes, now_ms());
    if (g_current) kmutex_unlock(&lock);
    return e;
}
static void entropy_worker(void *unused)
{
    (void)unused;
    if (raw_requested && !raw) {
        raw = entropy_alloc(ENTROPY_COLLECT_ROUNDS);
        if (!raw) { raw_requested = false; klog("[entropy] raw unavailable=allocation"); }
    }
    for (;;) {
        uint32_t words[6];
        uint32_t flags = irq_save();
        memcpy(words, fast.words, sizeof(fast.words));
        words[4] = fast.count; words[5] = fast.sources;
        fast.count = 0;
        irq_restore(flags);
        kmutex_lock(&lock);
        if (words[4]) entropy_pool_mix(&pool, words, sizeof(words), ENTROPY_SOURCE_IRQ);
        bool need = entropy_pool_wants_credit(&pool);
        kmutex_unlock(&lock);
        if (need || raw) {
            entropy_jent_begin(&ops);
            do {
                if (!collect()) break;
            } while (entropy_pool_wants_credit(&pool));
            /* Fill normal fresh credit before serial waits. The next batch
             * gets a new budget, excluding this attempt's later drain. */
            if (raw) raw_flush();
        }
        kmutex_lock(&lock);
        if (pool.reseeds != reported_reseeds || pool.failed != reported_failure) record();
        bool failed = pool.failed;
        kmutex_unlock(&lock);
        entropy_erase(words, sizeof(words));
        if (failed) {
            if (raw) { raw_release(); klog("[entropy] raw stopped samples=%u reason=collector", raw_sent); }
            entropy_jent_close(); return;
        }
        task_sleep_ms(10);
    }
}
void entropy_start(void)
{
    if (!booted || worker_started || pool.failed) return;
    struct task *t = task_create_kernel("entropy", entropy_worker, NULL, P_NORMAL);
    if (!t) { klog("[entropy] worker=unavailable error=%d", -ENOMEM); return; }
    worker_started = true; task_start(t);
}
