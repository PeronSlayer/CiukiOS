/* Boot seed and bounded UP IRQ handoff. No user memory or hashing in IRQs.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/arch.h>
#include <ciuki/cpu.h>
#include <ciuki/task.h>
#include <ciuki/sync.h>
#include <ciuki/entropy.h>
#include <ciuki/entropy_pool.h>

static struct entropy_pool pool;
static struct entropy_jitter jitter;
static struct kmutex lock;
/* One CPU on the current kernel. SMP must allocate a buffer per CPU. */
static struct { uint32_t words[4], count, sources; } fast;
static bool booted, worker_started;
static unsigned reported_reseeds;
static bool reported_failure;
static uint64_t jitter_clock(void) { return read_tsc_serialized(); }
static uint64_t now_ms(void)
{
    uint32_t flags = irq_save(); uint64_t now = g_ticks; irq_restore(flags);
    return now;
}
static void record(void)
{
    klog("[entropy] sources=%u irq_mask=%04x estimate_bits=%u initialised_ms=%llu health=%s reseeds=%u",
         pool.sources, fast.sources, pool.ready ? ENTROPY_SEED_BITS : pool.estimate_bits,
         pool.initialised_ms, pool.failed ? "fail" : "ok", pool.reseeds);
    reported_reseeds = pool.reseeds; reported_failure = pool.failed;
}
void entropy_boot(void)
{
    if (booted) return;
    booted = true; entropy_pool_init(&pool); kmutex_init(&lock);
    if (g_cpu_tsc) {
        /* Finite boot-only budget, before scheduler/guard initialization.
         * Failure never delays boot indefinitely or creates a weak seed. */
        for (unsigned i = 0; i < 131072 && !pool.ready && !pool.failed; i++)
            entropy_pool_sample(&pool, entropy_jitter_sample(&jitter, jitter_clock), 0);
    }
    if (!pool.ready) entropy_pool_fail(&pool);
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
    /* At the guard safe point no task/mutex owner can exist. */
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
        /* Lock spans one sample only; no IRQ exclusion across conditioning. */
        for (unsigned i = 0; i < 64 && !pool.failed; i++) {
            kmutex_unlock(&lock);
            kpreempt_point();
            uint64_t delta = entropy_jitter_sample(&jitter, jitter_clock);
            kmutex_lock(&lock);
            entropy_pool_sample(&pool, delta, now_ms());
        }
        if (pool.reseeds != reported_reseeds || pool.failed != reported_failure) record();
        kmutex_unlock(&lock);
        entropy_erase(words, sizeof(words));
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
