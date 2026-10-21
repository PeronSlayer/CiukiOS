/* Original Ciuki entropy model, not an import of Linux or jitterentropy.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/entropy_pool.h>

void entropy_erase(void *buffer, size_t bytes)
{
    volatile uint8_t *p = buffer;
    while (bytes--) *p++ = 0;
}
static uint32_t rotate(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
static uint32_t load32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
#define QR(a,b,c,d) do { \
    a += b; d = rotate(d ^ a,16); c += d; b = rotate(b ^ c,12); \
    a += b; d = rotate(d ^ a, 8); c += d; b = rotate(b ^ c, 7); \
} while (0)
void entropy_chacha20(const uint8_t key[32], uint32_t counter,
                      const uint8_t nonce[12], uint8_t block[64])
{
    uint32_t initial[16] = {0x61707865,0x3320646e,0x79622d32,0x6b206574};
    for (unsigned i = 0; i < 8; i++) initial[4 + i] = load32(key + 4 * i);
    initial[12] = counter;
    for (unsigned i = 0; i < 3; i++) initial[13 + i] = load32(nonce + 4 * i);
    uint32_t x[16]; memcpy(x, initial, sizeof(x));
    for (unsigned i = 0; i < 10; i++) {
        QR(x[0],x[4],x[8],x[12]); QR(x[1],x[5],x[9],x[13]);
        QR(x[2],x[6],x[10],x[14]); QR(x[3],x[7],x[11],x[15]);
        QR(x[0],x[5],x[10],x[15]); QR(x[1],x[6],x[11],x[12]);
        QR(x[2],x[7],x[8],x[13]); QR(x[3],x[4],x[9],x[14]);
    }
    for (unsigned i = 0; i < 16; i++) {
        uint32_t v = x[i] + initial[i];
        for (unsigned j = 0; j < 4; j++) block[4 * i + j] = (uint8_t)(v >> (8 * j));
    }
    entropy_erase(initial, sizeof(initial)); entropy_erase(x, sizeof(x));
}
#undef QR
bool entropy_health_sample(struct entropy_health *h, uint8_t sample)
{
    if (h->failed) return false;
    h->run = h->run && h->previous == sample ? h->run + 1 : 1;
    h->previous = sample;
    if (!h->window) { h->reference = sample; h->matches = 0; }
    if (sample == h->reference) h->matches++;
    h->window++;
    /* SP 800-90B 4.4, nonbinary H=.5, alpha=2^-20. */
    if (h->run >= 41 || h->matches >= 410) h->failed = true;
    if (h->window == 512) h->window = 0;
    return !h->failed;
}
void entropy_pool_init(struct entropy_pool *p)
{
    memset(p, 0, sizeof(*p)); sha256_init(&p->conditioner);
    static const char domain[] = "Ciuki entropy pool v1";
    sha256_update(&p->conditioner, domain, sizeof(domain));
}
void entropy_pool_fail(struct entropy_pool *p)
{
    p->failed = true; p->ready = false; p->estimate_bits = p->fraction = 0;
    entropy_erase(p->key, sizeof(p->key));
    entropy_erase(&p->conditioner, sizeof(p->conditioner));
}
void entropy_pool_mix(struct entropy_pool *p, const void *data, size_t bytes, unsigned source)
{
    if (p->failed) return;
    p->sources |= source;
    sha256_update(&p->conditioner, &source, sizeof(source));
    sha256_update(&p->conditioner, data, bytes);
}
static bool due(const struct entropy_pool *p, uint64_t now)
{ return !p->ready || now - p->seeded_ms >= ENTROPY_RESEED_MS || p->produced >= ENTROPY_RESEED_BYTES; }
static void reseed(struct entropy_pool *p, uint64_t now)
{
    uint8_t seed[32];
    /* Bind fresh noise to the previous secret without crediting that secret. */
    sha256_update(&p->conditioner, p->key, sizeof(p->key));
    sha256_update(&p->conditioner, &p->reseeds, sizeof(p->reseeds));
    sha256_final(&p->conditioner, seed);
    memcpy(p->key, seed, sizeof(seed));
    /* SHA's buffered last block can contain the previous key. Do not leave
     * either a copy of that key or the new seed in the persistent pool. */
    entropy_erase(&p->conditioner, sizeof(p->conditioner));
    sha256_init(&p->conditioner);
    static const char domain[] = "Ciuki entropy pool v1";
    sha256_update(&p->conditioner, domain, sizeof(domain));
    entropy_erase(seed, sizeof(seed));
    if (!p->ready) p->initialised_ms = now;
    p->ready = true; p->seeded_ms = now; p->produced = 0;
    p->estimate_bits = p->fraction = 0; p->reseeds++;
}
void entropy_pool_sample(struct entropy_pool *p, uint64_t delta, uint64_t now)
{
    if (p->failed) return;
    entropy_pool_mix(p, &delta, sizeof(delta), ENTROPY_SOURCE_JITTER);
    /* Tests see raw noise, never hashed/mixed/filtered samples. */
    if (!entropy_health_sample(&p->health, (uint8_t)delta)) { entropy_pool_fail(p); return; }
    uint64_t delta2 = delta - p->previous_delta;
    uint64_t delta3 = delta2 - p->previous_delta2;
    p->previous_delta = delta; p->previous_delta2 = delta2;
    if (p->samples < ENTROPY_STARTUP_SAMPLES) { p->samples++; return; }
    if (!delta || !delta2 || !delta3) return;
    if (p->estimate_bits < ENTROPY_SEED_BITS && ++p->fraction == ENTROPY_SAMPLES_PER_BIT) {
        p->fraction = 0; p->estimate_bits++;
    }
    if (p->estimate_bits == ENTROPY_SEED_BITS && due(p, now)) reseed(p, now);
}
int entropy_pool_read(struct entropy_pool *p, void *buffer, size_t bytes, uint64_t now)
{
    if (bytes > 256) return -EINVAL;
    if (!bytes) return 0;
    if (!buffer) return -EFAULT;
    if (p->failed || !p->ready) return -EAGAIN;
    if (due(p, now) || bytes > ENTROPY_RESEED_BYTES - p->produced) {
        if (p->estimate_bits < ENTROPY_SEED_BITS) return -EAGAIN;
        reseed(p, now);
    }
    uint8_t block[64], nonce[12] = {0}; uint8_t *out = buffer;
    p->produced += (unsigned)bytes;
    while (bytes) {
        /* Fast key erasure: never expose the part used as the next key. */
        entropy_chacha20(p->key, 0, nonce, block);
        memcpy(p->key, block, sizeof(p->key));
        unsigned n = bytes < 32 ? (unsigned)bytes : 32;
        memcpy(out, block + 32, n); out += n; bytes -= n;
        entropy_erase(block, sizeof(block));
    }
    return 0;
}
/* Like upstream jitterentropy's noise core, retain the actual instruction
 * workload even when the kernel/model is otherwise compiled at -O2/-O1. */
__attribute__((optnone, noinline))
uint64_t entropy_jitter_sample(struct entropy_jitter *j, uint64_t (*clock)(void))
{
    uint64_t start = clock();
    /* Volatile dependent memory accesses cannot be elided or vectorized.
     * Timing is the input; memory contents and loop count receive no credit. */
    for (unsigned i = 0; i < 128; i++) {
        j->index = (j->index + 31 + j->memory[j->index]) & 2047;
        j->memory[j->index]++;
    }
    return clock() - start;
}
