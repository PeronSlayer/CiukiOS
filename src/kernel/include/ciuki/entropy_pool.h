/* Host-testable entropy state machine. See docs/design/kernel-entropy.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_ENTROPY_POOL_H
#define CIUKI_ENTROPY_POOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <ciuki/sha256.h>
#define ENTROPY_SEED_BITS 256u
#define ENTROPY_RESEED_MS 60000u
#define ENTROPY_RESEED_BYTES 1048576u
#define ENTROPY_SOURCE_JITTER 1u
#define ENTROPY_SOURCE_IRQ 2u
struct entropy_pool {
    struct sha256_ctx conditioner;
    uint8_t key[32];
    uint64_t seeded_ms, initialised_ms;
    unsigned estimate_bits, produced, reseeds, sources;
    bool source_ready, ready, failed;
};
void entropy_erase(void *, size_t);
void entropy_chacha20(const uint8_t key[32], uint32_t counter,
                      const uint8_t nonce[12], uint8_t block[64]);
void entropy_pool_init(struct entropy_pool *);
void entropy_pool_fail(struct entropy_pool *);
void entropy_pool_mix(struct entropy_pool *, const void *, size_t, unsigned source);
bool entropy_pool_wants_credit(const struct entropy_pool *);
void entropy_pool_credit(struct entropy_pool *, const void *, size_t, uint64_t now_ms);
int entropy_pool_read(struct entropy_pool *, void *, size_t, uint64_t now_ms);
#endif
