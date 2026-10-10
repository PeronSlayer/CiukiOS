/* SHA-256, FIPS 180-4. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SHA256_H
#define CIUKI_SHA256_H
#include <stdint.h>
#include <stddef.h>
struct sha256_ctx {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t block[64];
    unsigned used;
};
void sha256_init(struct sha256_ctx *ctx);
void sha256_update(struct sha256_ctx *ctx, const void *data, size_t length);
void sha256_final(struct sha256_ctx *ctx, uint8_t digest[32]);
void sha256(const void *data, size_t length, uint8_t digest[32]);
void sha256_hex(const uint8_t digest[32], char hex[65]);
#endif
