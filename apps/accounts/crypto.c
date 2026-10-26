/* SPDX-License-Identifier: MIT */
#include "accounts.h"
#include <ciuki/sha256.h>
#include <errno.h>
#include <string.h>

void acct_wipe(void *p, size_t n)
{
    volatile unsigned char *bytes = p;
    while (n--) *bytes++ = 0;
}

/* No data-dependent branch or early exit. The length is public/fixed at 32 for
 * verifiers. Volatile accumulation prevents replacement with memcmp. */
int acct_equal_secret(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    volatile unsigned diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= x[i] ^ y[i];
    return diff == 0;
}

int acct_password_valid(const char *p, int allow_short)
{
    if (!p) return 0;
    for (unsigned n = 0; n < 64; ++n) {
        if (!p[n]) return allow_short || n >= 8;
        if ((unsigned char)p[n] < 0x20 || (unsigned char)p[n] > 0x7e) return 0;
    }
    return 0;
}

static void hmac(const char *password, const void *data, size_t length, uint8_t out[32])
{
    uint8_t pad[64] = {0}, inner[32];
    struct sha256_ctx ctx;
    memcpy(pad, password, strlen(password));
    for (unsigned i = 0; i < 64; ++i) pad[i] ^= 0x36;
    sha256_init(&ctx);
    sha256_update(&ctx, pad, 64);
    sha256_update(&ctx, data, length);
    sha256_final(&ctx, inner);
    for (unsigned i = 0; i < 64; ++i) pad[i] ^= 0x36 ^ 0x5c;
    sha256_init(&ctx);
    sha256_update(&ctx, pad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
    acct_wipe(&ctx, sizeof(ctx));
    acct_wipe(pad, sizeof(pad));
    acct_wipe(inner, sizeof(inner));
}

int acct_derive(const char *password, const uint8_t salt[16], uint32_t count, uint8_t out[32])
{
    /* Bound before work, including direct callers and imported parameters. */
    if (!salt || !out || !acct_password_valid(password, 1) || count != ACCT_ITERATIONS)
        return -EINVAL;
    uint8_t input[20], u[32];
    memcpy(input, salt, 16);
    memset(input + 16, 0, 3);
    input[19] = 1; /* RFC 8018 5.2, INT_32_BE(1), one derived-key block. */
    hmac(password, input, sizeof(input), u);
    memcpy(out, u, 32);
    for (uint32_t i = 1; i < count; ++i) {
        hmac(password, u, 32, u);
        for (unsigned j = 0; j < 32; ++j) out[j] ^= u[j];
    }
    acct_wipe(input, sizeof(input));
    acct_wipe(u, sizeof(u));
    return 0;
}
