/* sec-user-06: the account service's verifier derivation for the self-test
 * check security.kdf (selftest_checks.c), on its own so the host suite can
 * prove it equal to apps/accounts/crypto.c acct_derive and to PBKDF2-HMAC-
 * SHA-256 (RFC 8018 5.2): HMAC-SHA-256 keyed by the password, recomputed per
 * iteration (four compressions each, as acct_derive), one 32-byte block,
 * the kernel's lib/sha256.c that the account service links too. A password
 * of at most 63 bytes, as the account policy allows (HMAC keys up to the
 * 64-byte block are used as they are). SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/sha256.h>
#include <ciuki/selftest.h>

static void kdf_hmac(const char *key, size_t key_len, const void *data, size_t length, uint8_t out[32])
{
    uint8_t pad[64] = {0}, inner[32];
    struct sha256_ctx ctx;
    memcpy(pad, key, key_len);
    for (unsigned i = 0; i < 64; ++i) pad[i] ^= 0x36;
    sha256_init(&ctx); sha256_update(&ctx, pad, 64); sha256_update(&ctx, data, length); sha256_final(&ctx, inner);
    for (unsigned i = 0; i < 64; ++i) pad[i] ^= 0x36 ^ 0x5c;
    sha256_init(&ctx); sha256_update(&ctx, pad, 64); sha256_update(&ctx, inner, 32); sha256_final(&ctx, out);
}
void selftest_kdf_derive(const char *password, const uint8_t salt[16], uint32_t iterations, uint8_t out[32])
{
    size_t n = strlen(password);
    if (n > 63) n = 63;   /* the account policy's maximum; never past the HMAC block */
    uint8_t input[20], u[32];
    memcpy(input, salt, 16);
    input[16] = input[17] = input[18] = 0; input[19] = 1;
    kdf_hmac(password, n, input, sizeof(input), u);
    memcpy(out, u, 32);
    for (uint32_t i = 1; i < iterations; ++i) {
        kdf_hmac(password, n, u, 32, u);
        for (unsigned j = 0; j < 32; ++j) out[j] ^= u[j];
    }
}
