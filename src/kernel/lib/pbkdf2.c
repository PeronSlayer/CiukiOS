/* sec-k08: PBKDF2-HMAC-SHA-256 for the self-test KDF timing (ciuki/pbkdf2.h).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/pbkdf2.h>
#include <ciuki/sha256.h>
#include <ciuki/kernel.h>

static void wipe(void *p, size_t n) { volatile uint8_t *b = p; while (n--) *b++ = 0; }
static void hmac(const uint8_t key[64], const void *data, size_t length, uint8_t out[32])
{
    uint8_t pad[64], inner[32];
    struct sha256_ctx ctx;
    for (unsigned i = 0; i < 64; i++) pad[i] = key[i] ^ 0x36;
    sha256_init(&ctx); sha256_update(&ctx, pad, 64); sha256_update(&ctx, data, length); sha256_final(&ctx, inner);
    for (unsigned i = 0; i < 64; i++) pad[i] = key[i] ^ 0x5c;
    sha256_init(&ctx); sha256_update(&ctx, pad, 64); sha256_update(&ctx, inner, 32); sha256_final(&ctx, out);
    wipe(&ctx, sizeof(ctx)); wipe(pad, sizeof(pad)); wipe(inner, sizeof(inner));
}
int pbkdf2_hmac_sha256(const void *key, size_t key_bytes, const void *salt, size_t salt_bytes,
                       uint32_t iterations, uint8_t out[32])
{
    if ((!key && key_bytes) || key_bytes > 64 || (!salt && salt_bytes) || salt_bytes > 60 || !iterations || !out) return -1;
    uint8_t k[64] = {0}, input[64], u[32];
    memcpy(k, key, key_bytes);
    memcpy(input, salt, salt_bytes);
    input[salt_bytes] = 0; input[salt_bytes + 1] = 0; input[salt_bytes + 2] = 0; input[salt_bytes + 3] = 1;
    hmac(k, input, salt_bytes + 4, u);
    memcpy(out, u, 32);
    for (uint32_t i = 1; i < iterations; i++) {
        hmac(k, u, 32, u);
        for (unsigned j = 0; j < 32; j++) out[j] ^= u[j];
    }
    wipe(k, sizeof(k)); wipe(input, sizeof(input)); wipe(u, sizeof(u));
    return 0;
}
