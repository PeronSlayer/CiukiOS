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
/* One CPU on the current kernel. SMP must allocate a buffer per CPU.
 * last: low TSC word of the newest edge; a drain with an unchanged value
 * (stopped counter) is mixed but never credited. */
static struct { uint32_t words[4], count, sources, last; } fast;
static uint32_t drained_last;
static bool booted, worker_started;
static unsigned reported_reseeds;
static bool reported_failure;
static uint64_t boot_start, boot_per_ms;
/* f6-19: the jitter source is one of two credited sources. Boot tries the
 * default OSR once (no boot delay beyond one attempt); the worker retries
 * with interrupts live, raising the OSR, then retires it for this boot. */
enum { JITTER_OFF, JITTER_READY, JITTER_RETRY, JITTER_FAILED };
static const char *const jitter_names[] = {"off", "ready", "retry", "failed"};
static unsigned jitter, reported_jitter, jitter_attempts, jitter_next_osr = ENTROPY_JENT_MIN_OSR;
static uint64_t jitter_retry_ms;
#define JITTER_ATTEMPTS 4u          /* boot + three runtime attempts */
#define JITTER_FIRST_RETRY_MS 1000u
static const uint32_t jitter_backoff_ms[] = {30000u, 120000u};
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
    klog("[entropy] sources=%u irq_mask=%04x estimate_bits=%u initialised_ms=%llu health=%s reseeds=%u jitter=%s irq_bits=%u",
         pool.sources, fast.sources, pool.ready ? ENTROPY_SEED_BITS : pool.estimate_bits,
         pool.initialised_ms, pool.failed ? "fail" : "ok", pool.reseeds, jitter_names[jitter], pool.irq_bits);
    reported_reseeds = pool.reseeds; reported_failure = pool.failed; reported_jitter = jitter;
}
/* One record per attempt: the library result, the OSR reached, the first
 * health alarm (bits, stage, OSR, round) and the rounds per stage. */
static void jitter_report(const char *attempt)
{
    static const char *const stages[] = {"powerup", "memory", "sha3", "output"};
    struct entropy_jent_report r;
    entropy_jent_report(&r);
    klog("[entropy] jitter attempt=%s result=%d osr=%u alarm=%04x stage=%s alarm_osr=%u round=%u rounds=%u/%u/%u/%u refused=%u expired=%u state=%s",
         attempt, r.result, r.osr, r.alarm, stages[r.alarm_stage < 4 ? r.alarm_stage : 0], r.alarm_osr, r.alarm_round,
         r.rounds[0], r.rounds[1], r.rounds[2], r.rounds[3], (unsigned)r.refused, entropy_jent_expired(), jitter_names[jitter]);
}
/* A failed attempt retires the collector, never the pool: interrupt timing
 * remains a credited source. A permanent library health failure (not this
 * adapter's budget) or the last attempt ends jitter collection for this boot. */
static void jitter_retire(const char *attempt)
{
    struct entropy_jent_report r;
    entropy_jent_report(&r);
    entropy_jent_close();
    jitter_attempts++;
    if (r.osr >= jitter_next_osr) jitter_next_osr = r.osr < ENTROPY_JENT_MAX_OSR ? r.osr + 1 : ENTROPY_JENT_MAX_OSR;
    jitter = r.permanent || jitter_attempts >= JITTER_ATTEMPTS ? JITTER_FAILED : JITTER_RETRY;
    if (jitter == JITTER_RETRY) {
        unsigned backoff = jitter_attempts - 1;
        jitter_retry_ms = (g_current ? now_ms() : 0) +
            (backoff == 0 ? JITTER_FIRST_RETRY_MS : jitter_backoff_ms[backoff - 1 < 2 ? backoff - 1 : 1]);
    }
    jitter_report(attempt);
}
static bool collect(void)
{
    uint8_t bytes[32];
    if (entropy_jent_read(bytes)) {
        entropy_erase(bytes, sizeof(bytes));
        jitter_retire("read");
        return false;
    }
    kmutex_lock(&lock);
    entropy_pool_credit(&pool, bytes, sizeof(bytes), now_ms());
    kmutex_unlock(&lock);
    entropy_erase(bytes, sizeof(bytes));
    return true;
}
/* Runtime attempt: interrupts and scheduling now perturb the measured
 * intervals. No pool mutex is held; each round offers a preemption point. */
static void jitter_restart(void)
{
    entropy_jent_begin(&ops);
    if (entropy_jent_init(jitter_next_osr, ENTROPY_JENT_MAX_OSR)) { jitter_retire("runtime"); return; }
    struct entropy_jent_report r;
    entropy_jent_report(&r);
    jitter = JITTER_READY; jitter_next_osr = r.osr;
    jitter_report("runtime");
}
void entropy_boot(void)
{
    if (booted) return;
    booted = true; entropy_pool_init(&pool); kmutex_init(&lock);
    jitter = JITTER_OFF; jitter_attempts = 0; jitter_next_osr = ENTROPY_JENT_MIN_OSR;
    raw_requested = raw_enabled();
    if (g_cpu_tsc) boot_per_ms = boot_clock_rate();
    boot_start = g_cpu_tsc ? jitter_clock() : 0;
    /* A TSC validated against the PIT backs both credited sources. */
    pool.source_ready = boot_per_ms != 0;
    if (boot_per_ms) {
        entropy_jent_begin(&ops);
        if (entropy_jent_init(ENTROPY_JENT_MIN_OSR, ENTROPY_JENT_MIN_OSR)) jitter_retire("boot");
        else jitter = JITTER_READY;
        while (jitter == JITTER_READY && !pool.ready) {
            /* No scheduler/mutex owner exists before stack-guard seeding. */
            uint8_t bytes[32];
            if (entropy_jent_read(bytes)) jitter_retire("boot-read");
            else entropy_pool_credit(&pool, bytes, sizeof(bytes), 0);
            entropy_erase(bytes, sizeof(bytes));
        }
    }
    if (!pool.source_ready) entropy_pool_fail(&pool);
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
    fast.last = (uint32_t)cycles;
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
        uint32_t words[7];
        uint32_t flags = irq_save();
        memcpy(words, fast.words, sizeof(fast.words));
        words[4] = fast.count; words[5] = fast.sources; words[6] = fast.last;
        fast.count = 0;
        irq_restore(flags);
        /* Credit only edges stamped by a counter that moved since the last drain. */
        unsigned events = words[4] && words[6] != drained_last ? words[4] : 0;
        drained_last = words[6];
        kmutex_lock(&lock);
        if (events) entropy_pool_credit_irq(&pool, words, sizeof(words), events, now_ms());
        else if (words[4]) entropy_pool_mix(&pool, words, sizeof(words), ENTROPY_SOURCE_IRQ);
        bool need = entropy_pool_wants_credit(&pool);
        kmutex_unlock(&lock);
        if (jitter == JITTER_RETRY && (need || raw) && now_ms() >= jitter_retry_ms) jitter_restart();
        if (jitter == JITTER_READY && (need || raw)) {
            entropy_jent_begin(&ops);
            do {
                if (!collect()) break;
            } while (entropy_pool_wants_credit(&pool));
            /* Fill normal fresh credit before serial waits. The next batch
             * gets a new budget, excluding this attempt's later drain. */
            if (raw && jitter == JITTER_READY) raw_flush();
        }
        if (raw && jitter == JITTER_FAILED) {
            raw_release(); klog("[entropy] raw stopped samples=%u reason=collector", raw_sent);
        }
        kmutex_lock(&lock);
        if (pool.reseeds != reported_reseeds || pool.failed != reported_failure || jitter != reported_jitter) record();
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
    /* A boot failure retries once the PIT and device interrupts run. */
    if (jitter == JITTER_RETRY) jitter_retry_ms = now_ms() + JITTER_FIRST_RETRY_MS;
    worker_started = true; task_start(t);
}
