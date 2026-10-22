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
/* f6-21: the desktop presented its first full frame, or no desktop frame
 * will come (test boots, a missing or stopped desktop). Gates jitter retries. */
static volatile bool desktop_ready;
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
 * Allocate only in the opt-in collector, never on the boot/IRQ paths. */
static uint8_t *raw;
static unsigned raw_count, raw_sent;
static bool raw_requested;
static uint64_t jitter_clock(void) { return read_tsc_serialized(); }
static uint64_t now_ms(void)
{
    uint32_t flags = irq_save(); uint64_t now = g_ticks; irq_restore(flags);
    return now;
}
/* f6-21 turns: begin (tests/host/test_kernel_latency.py extracts this block)
 * Runtime jitter collection runs in turns of at most ENTROPY_TURN_US of
 * collection, measured in TSC cycles at every boundary (each measurement
 * round, and each chunk of the adapter's own long work). The collector
 * sleeps one tick when the next round could take its turn past the bound. */
#define ENTROPY_TURN_US 1000u
enum { TURN_CONTINUE, TURN_PAUSE };
struct entropy_turns {
    uint64_t bound;               /* cycles of collection in one turn */
    uint64_t start, last;         /* TSC: start of this turn, latest boundary */
    uint64_t predict;             /* next round's cost: the longest recent round, decaying */
    uint64_t round_max, turn_max; /* longest round (or chunk) and turn, this session */
    uint64_t active;              /* collection cycles, never reset: the runtime budget clock */
    uint32_t dispatches;          /* collector dispatches at the latest boundary */
    unsigned turns, pauses;       /* this session */
};
static void turns_open(struct entropy_turns *s, uint64_t now, uint32_t dispatches)
{
    s->start = s->last = now; s->dispatches = dispatches;
    s->predict = s->round_max = s->turn_max = 0; s->turns = 1; s->pauses = 0;
}
/* The collector runs again after a pause or a preemption: a new turn. */
static void turns_resume(struct entropy_turns *s, uint64_t now, uint32_t dispatches)
{
    s->start = s->last = now; s->dispatches = dispatches; s->turns++;
}
/* Account one boundary; returns the cycles used by the current turn. A
 * dispatch the collector did not see (a blocking wait inside a round)
 * began a new turn: the part of that interval which ran after it is
 * unknown and counts as the predicted round. A longer round raises the
 * prediction at once; shorter ones lower it by a quarter of the gap. */
static uint64_t turns_account(struct entropy_turns *s, uint64_t now, uint32_t dispatches)
{
    uint64_t round = now - s->last;
    if (dispatches != s->dispatches) {
        if (round > s->predict) round = s->predict;
        s->start = now - round; s->dispatches = dispatches; s->turns++;
    } else {
        if (round > s->round_max) s->round_max = round;
        s->predict = round >= s->predict ? round : s->predict - (s->predict - round) / 4;
    }
    s->active += round; s->last = now;
    uint64_t used = now - s->start;
    if (used > s->turn_max) s->turn_max = used;
    return used;
}
/* Pause when the next round (with an eighth of margin) could cross the bound. */
static unsigned turns_round(struct entropy_turns *s, uint64_t now, uint32_t dispatches)
{
    if (turns_account(s, now, dispatches) + s->predict + s->predict / 8 <= s->bound) return TURN_CONTINUE;
    s->pauses++;
    return TURN_PAUSE;
}
static struct entropy_turns turns;
static bool collecting;         /* a runtime collection session is open */
/* The adapter's yield/point callback. Boot has no scheduler and no turns.
 * The collector is idle class: any runnable task is dispatched first at the
 * preemption point; a full turn sleeps one tick, so the CPU also idles. */
static void jitter_boundary(void)
{
    if (!g_current) return;
    if (!collecting) { kpreempt_point(); return; }
    uint32_t seen = g_current->dispatches;
    if (turns_round(&turns, jitter_clock(), seen) == TURN_PAUSE) task_sleep_ms(1);
    else kpreempt_point();
    if (g_current->dispatches != seen) turns_resume(&turns, jitter_clock(), g_current->dispatches);
}
/* f6-21 turns: end */
/* Boot: TSC time since entropy_boot. Runtime (f6-21): the collector's own
 * collection time, so pauses and other tasks' work never spend its budget. */
static uint64_t collection_ms(void)
{
    if (!boot_per_ms) return now_ms();
    return g_current ? turns.active / boot_per_ms : (jitter_clock() - boot_start) / boot_per_ms;
}
static uint64_t turn_us(uint64_t cycles) { return boot_per_ms ? cycles * 1000u / boot_per_ms : 0; }
static void capture(uint64_t delta, uint64_t gcd, unsigned osr)
{
    (void)gcd; (void)osr;
    if (raw && raw_count < ENTROPY_COLLECT_ROUNDS && raw_count < RAW_LIMIT - raw_sent)
        raw[raw_count++] = (uint8_t)delta;
}
static const struct entropy_jent_ops ops = {jitter_clock, collection_ms, entropy_alloc, entropy_free,
                                            jitter_boundary, capture, jitter_boundary};
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
 * health alarm (bits, stage, OSR, round) and the rounds per stage; then the
 * collection session's turns (f6-21; zero at boot, before the scheduler). */
static void jitter_report(const char *attempt)
{
    static const char *const stages[] = {"powerup", "memory", "sha3", "output"};
    struct entropy_jent_report r;
    entropy_jent_report(&r);
    klog("[entropy] jitter attempt=%s result=%d osr=%u alarm=%04x stage=%s alarm_osr=%u round=%u rounds=%u/%u/%u/%u refused=%u expired=%u state=%s turns=%u pauses=%u turn_max_us=%llu round_max_us=%llu",
         attempt, r.result, r.osr, r.alarm, stages[r.alarm_stage < 4 ? r.alarm_stage : 0], r.alarm_osr, r.alarm_round,
         r.rounds[0], r.rounds[1], r.rounds[2], r.rounds[3], (unsigned)r.refused, entropy_jent_expired(), jitter_names[jitter],
         collecting ? turns.turns : 0, collecting ? turns.pauses : 0,
         collecting ? turn_us(turns.turn_max) : 0, collecting ? turn_us(turns.round_max) : 0);
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
 * intervals. No pool mutex is held; each round is a turn boundary. */
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
/* Record on a reseed, a health change or a jitter state change. */
static void report(void)
{
    kmutex_lock(&lock);
    if (pool.reseeds != reported_reseeds || pool.failed != reported_failure || jitter != reported_jitter) record();
    kmutex_unlock(&lock);
}
/* Interrupt timing: drain the fast buffer every 10 ms at P_NORMAL. Never
 * waits for the jitter collector (f6-21). */
static void entropy_worker(void *unused)
{
    (void)unused;
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
        bool failed = pool.failed;
        kmutex_unlock(&lock);
        entropy_erase(words, sizeof(words));
        report();
        if (failed) return;
        task_sleep_ms(10);
    }
}
/* f6-21: CPU jitter collection, in its own idle-class task (level 0): it
 * runs only when no other task is runnable, is never boosted, and works in
 * turns of at most ENTROPY_TURN_US (jitter_boundary). The first runtime
 * attempt also waits for the desktop's first frame. */
static void jitter_collector(void *unused)
{
    (void)unused;
    if (raw_requested && !raw) {
        raw = entropy_alloc(ENTROPY_COLLECT_ROUNDS);
        if (!raw) { raw_requested = false; klog("[entropy] raw unavailable=allocation"); }
    }
    for (;;) {
        kmutex_lock(&lock);
        bool need = entropy_pool_wants_credit(&pool), failed = pool.failed;
        kmutex_unlock(&lock);
        if (failed) {
            if (raw) { raw_release(); klog("[entropy] raw stopped samples=%u reason=collector", raw_sent); }
            entropy_jent_close(); return;
        }
        bool want = need || raw;
        bool retry = jitter == JITTER_RETRY && want && desktop_ready && now_ms() >= jitter_retry_ms;
        if (retry || (jitter == JITTER_READY && want)) {
            turns_open(&turns, jitter_clock(), g_current->dispatches); collecting = true;
            if (retry) jitter_restart();
            if (jitter == JITTER_READY) {
                entropy_jent_begin(&ops);
                do {
                    if (!collect()) break;
                } while (entropy_pool_wants_credit(&pool));
            }
            turns_account(&turns, jitter_clock(), g_current->dispatches); /* the session's tail */
            collecting = false;
            /* Fill normal fresh credit before serial waits. The next batch
             * gets a new budget, excluding this attempt's later drain. */
            if (raw && jitter == JITTER_READY) raw_flush();
        }
        if (raw && jitter == JITTER_FAILED) {
            raw_release(); klog("[entropy] raw stopped samples=%u reason=collector", raw_sent);
        }
        report();
        task_sleep_ms(10);
    }
}
void entropy_desktop_presented(void) { desktop_ready = true; }
void entropy_start(void)
{
    if (!booted || worker_started || pool.failed) return;
    struct task *t = task_create_kernel("entropy", entropy_worker, NULL, P_NORMAL);
    if (!t) { klog("[entropy] worker=unavailable error=%d", -ENOMEM); return; }
    /* A collector is needed while jitter can still run, or to stop a capture. */
    struct task *c = NULL;
    if (jitter != JITTER_FAILED || raw_requested) {
        c = task_create_kernel("jitter", jitter_collector, NULL, P_IDLE);
        if (!c) {
            klog("[entropy] collector=unavailable error=%d", -ENOMEM);
            entropy_jent_close(); jitter = JITTER_FAILED; raw_requested = false;
        }
    }
    /* Runtime diagnostic boots launch no desktop (as the pager assumes). */
    if (g_boot.flags & (CBI_F_TEST_REQUEST | CBI_F_HW_CHECK)) desktop_ready = true;
    turns.bound = boot_per_ms * ENTROPY_TURN_US / 1000u;
    /* A boot failure retries once the PIT and device interrupts run. */
    if (jitter == JITTER_RETRY) jitter_retry_ms = now_ms() + JITTER_FIRST_RETRY_MS;
    worker_started = true; task_start(t);
    if (c) task_start(c);
}
