/* Single collector, never used from IRQs or concurrently. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/entropy_jent.h>
#include "jitterentropy.h"
#include "jitterentropy-internal.h"
/* jitterentropy(3), v3.7.0: health tests forced (JENT_FORCE_FIPS), the TSC
 * only (no timer thread), and the pinned 256 KiB memory-access buffer. The
 * same flags go to jent_entropy_init_ex and jent_entropy_collector_alloc. */
#define CIUKI_JENT_FLAGS (JENT_FORCE_FIPS | JENT_DISABLE_INTERNAL_TIMER | JENT_MAX_MEMSIZE_256kB)
static const struct entropy_jent_ops *ops;
static struct rand_data *collector;
static uint64_t begun;
static unsigned rounds, stage;
static bool expired, callback_set;
static struct entropy_jent_report report;
void ciuki_jent_erase(void *buffer, size_t size)
{
    volatile uint8_t *p = buffer;
    while (size--) *p++ = 0;
}
void ciuki_jent_clock(uint64_t *out) { *out = ops->clock(); }
/* f6-21: the runtime collector works in turns of at most 1 ms. Zeroing or
 * erasing the pinned 256 KiB buffer is a byte loop (about 0.5 ms on the T23,
 * far longer under QEMU TCG), so it runs in 16 KiB chunks with a boundary
 * between them, outside any measured interval. */
#define CIUKI_JENT_CHUNK 16384u
static void clear(void *buffer, size_t size)
{
    volatile uint8_t *p = buffer;
    for (size_t at = 0; at < size;) {
        size_t end = size - at > CIUKI_JENT_CHUNK ? at + CIUKI_JENT_CHUNK : size;
        while (at < end) p[at++] = 0;
        if (at < size && ops->point) ops->point();
    }
}
void *ciuki_jent_alloc(size_t size)
{
    /* A startup alarm makes the library's own reset ask for the next memory
     * step (512 KiB): refused, so that reset fails at once without a round,
     * and entropy_jent_init recovers with osr+1 at the pinned size. */
    if (expired) return NULL;
    if (size > 262144) { if (size > report.refused) report.refused = size; return NULL; }
    if (ops->point) ops->point();      /* the pool's search is its own unit of work */
    void *p = ops->alloc(size);
    if (p) clear(p, size);
    return p;
}
void ciuki_jent_free(void *p, size_t size)
{
    if (p) { clear(p, size); ops->free(p); }
}
/* No source patch: a collector whose output loop has never run (the power-up
 * test's private instance) has rct_mem_nosr == 0; otherwise startup_state
 * names the stage jent_random_data samples. */
static unsigned stage_of(const struct rand_data *ec)
{
    if (!ec->rct_mem_nosr) return ENTROPY_JENT_POWERUP;
    return ec->startup_state == jent_startup_memory ? ENTROPY_JENT_MEMORY :
           ec->startup_state == jent_startup_sha3 ? ENTROPY_JENT_SHA3 : ENTROPY_JENT_OUTPUT;
}
bool ciuki_jent_round(struct rand_data *ec)
{
    /* No pool mutex is held. Combined-noise deltas span consecutive calls,
     * so this yield (and any dispatched work) contributes to the next delta. */
    stage = stage_of(ec);
    if (ops->yield) ops->yield();
    if (expired || rounds >= ENTROPY_COLLECT_ROUNDS || ops->ms() - begun >= ENTROPY_COLLECT_MS) {
        expired = true;
        ec->health_failure |= JENT_RCT_FAILURE_PERMANENT;
        return true;
    }
    rounds++; report.rounds[stage]++;
    return false;
}
void ciuki_jent_raw(struct rand_data *ec, uint64_t delta)
{
    if (ops->raw) ops->raw(delta, ec->jent_common_timer_gcd, ec->osr);
}
static void health_alarm(struct rand_data *ec, unsigned int failure)
{
    if (expired || report.alarm) return;   /* the adapter's budget is not a health result */
    report.alarm = failure; report.alarm_stage = stage_of(ec);
    report.alarm_osr = ec->osr; report.alarm_round = rounds;
}
void entropy_jent_begin(const struct entropy_jent_ops *new_ops)
{
    /* Documented order: the callback before the first jent_entropy_init*. */
    if (!callback_set) { (void)jent_set_fips_failure_callback(health_alarm); callback_set = true; }
    ops = new_ops; begun = ops->ms(); rounds = 0; expired = false;
    memset(&report, 0, sizeof(report));
}
/* jent_entropy_init_ex errors that a lower assumed entropy rate cannot change. */
static bool osr_bound(int e)
{ return e == EMINVARVAR || e == EHEALTH || e == ERCT; }
int entropy_jent_init(unsigned first_osr, unsigned last_osr)
{
    if (first_osr < ENTROPY_JENT_MIN_OSR) first_osr = ENTROPY_JENT_MIN_OSR;
    if (last_osr > ENTROPY_JENT_MAX_OSR) last_osr = ENTROPY_JENT_MAX_OSR;
    report.result = -1;
    for (unsigned osr = first_osr; osr <= last_osr && !expired && !collector; osr++) {
        report.osr = osr;
        int e = jent_entropy_init_ex(osr, CIUKI_JENT_FLAGS);
        report.result = e;
        if (e && !osr_bound(e)) break;
        if (!e) {
            collector = jent_entropy_collector_alloc(osr, CIUKI_JENT_FLAGS);
            if (!collector) report.result = -1;
        }
        if (ops->ms() - begun >= ENTROPY_COLLECT_MS) expired = true;
    }
    if (collector && expired) entropy_jent_close();
    return collector ? 0 : -1;
}
int entropy_jent_read(uint8_t out[32])
{
    /* Never expose a partial failed library read. */
    uint8_t bytes[32];
    ssize_t n = collector ? jent_read_entropy_safe(&collector, (char *)bytes, sizeof(bytes)) : -1;
    if (ops->ms() - begun >= ENTROPY_COLLECT_MS) expired = true;
    if (collector) report.osr = collector->osr;
    report.result = n == (ssize_t)sizeof(bytes) ? 0 : (int)n;
    report.permanent = !expired && (n == -6 || n == -7 || n == -8 || n == -10);
    if (n == (ssize_t)sizeof(bytes) && !expired) memcpy(out, bytes, sizeof(bytes));
    ciuki_jent_erase(bytes, sizeof(bytes));
    return n == (ssize_t)sizeof(bytes) && !expired ? 0 : -1;
}
void entropy_jent_close(void)
{
    jent_entropy_collector_free(collector); collector = NULL;
}
unsigned entropy_jent_rounds(void) { return rounds; }
bool entropy_jent_expired(void) { return expired; }
unsigned entropy_jent_stage(void) { return stage; }
void entropy_jent_report(struct entropy_jent_report *out) { *out = report; }
