/* Single collector, never used from IRQs or concurrently. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/entropy_jent.h>
#include "jitterentropy.h"
#include "jitterentropy-internal.h"
static const struct entropy_jent_ops *ops;
static struct rand_data *collector;
static uint64_t begun;
static unsigned rounds;
static bool expired;
void ciuki_jent_erase(void *buffer, size_t size)
{
    volatile uint8_t *p = buffer;
    while (size--) *p++ = 0;
}
void ciuki_jent_clock(uint64_t *out) { *out = ops->clock(); }
void *ciuki_jent_alloc(size_t size)
{
    if (expired || size > 262144) return NULL;
    void *p = ops->alloc(size);
    if (p) memset(p, 0, size);
    return p;
}
void ciuki_jent_free(void *p, size_t size)
{
    if (p) { ciuki_jent_erase(p, size); ops->free(p); }
}
bool ciuki_jent_round(struct rand_data *ec)
{
    /* No pool mutex is held. Combined-noise deltas span consecutive calls,
     * so this yield (and any dispatched work) contributes to the next delta. */
    if (ops->yield) ops->yield();
    if (expired || rounds >= ENTROPY_COLLECT_ROUNDS || ops->ms() - begun >= ENTROPY_COLLECT_MS) {
        expired = true;
        ec->health_failure |= JENT_RCT_FAILURE_PERMANENT;
        return true;
    }
    rounds++;
    return false;
}
void ciuki_jent_raw(struct rand_data *ec, uint64_t delta)
{
    if (ops->raw) ops->raw(delta, ec->jent_common_timer_gcd, ec->osr);
}
void entropy_jent_begin(const struct entropy_jent_ops *new_ops)
{
    ops = new_ops; begun = ops->ms(); rounds = 0; expired = false;
}
int entropy_jent_init(void)
{
    if (jent_entropy_init()) return -1;
    collector = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS | JENT_DISABLE_INTERNAL_TIMER | JENT_MAX_MEMSIZE_256kB);
    if (ops->ms() - begun >= ENTROPY_COLLECT_MS) expired = true;
    return collector && !expired ? 0 : -1;
}
int entropy_jent_read(uint8_t out[32])
{
    /* Never expose a partial failed library read. */
    uint8_t bytes[32];
    ssize_t n = jent_read_entropy(collector, (char *)bytes, sizeof(bytes));
    if (ops->ms() - begun >= ENTROPY_COLLECT_MS) expired = true;
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
