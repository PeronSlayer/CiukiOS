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
#define RAW_LIMIT 8192u
struct raw_sample { uint64_t delta, gcd; unsigned osr; };
static struct raw_sample *raw;
static unsigned raw_count, raw_dropped;
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
    if (!raw || g_current) return;
    if (raw_count < RAW_LIMIT) raw[raw_count++] = (struct raw_sample){delta, gcd, osr};
    else raw_dropped++;
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
    if (g_cpu_tsc) boot_per_ms = boot_clock_rate();
    boot_start = g_cpu_tsc ? jitter_clock() : 0;
    if (boot_per_ms) {
        if (raw_enabled()) {
            raw = entropy_alloc(RAW_LIMIT * sizeof(*raw));
            if (!raw) klog("[entropy] raw unavailable=allocation");
        }
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
    if (raw) {
        for (unsigned i = 0; i < raw_count; i++)
            klog("[entropy] raw index=%u delta=%llu gcd=%llu osr=%u", i, raw[i].delta, raw[i].gcd, raw[i].osr);
        klog("[entropy] raw captured=%u dropped=%u limit=%u", raw_count, raw_dropped, RAW_LIMIT);
        entropy_erase(raw, RAW_LIMIT * sizeof(*raw)); entropy_free(raw); raw = NULL;
    }
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
        if (need) {
            entropy_jent_begin(&ops);
            while (entropy_pool_wants_credit(&pool) && collect()) { }
        }
        kmutex_lock(&lock);
        if (pool.reseeds != reported_reseeds || pool.failed != reported_failure) record();
        bool failed = pool.failed;
        kmutex_unlock(&lock);
        entropy_erase(words, sizeof(words));
        if (failed) { entropy_jent_close(); return; }
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
