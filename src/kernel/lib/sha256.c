/* FIPS 180-4 sections 4.1.2, 4.2.2, 5.1.1, 5.3.3 and 6.2:
 * https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf
 * Unsigned 32-bit arithmetic, big-endian words, no FPU/SIMD or allocation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/sha256.h>

static const uint32_t constants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

static void compress(struct sha256_ctx *c)
{
    uint32_t w[64];
    for (unsigned i = 0; i < 16; i++) {
        const uint8_t *p = c->block + i * 4;
        w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    }
    for (unsigned i = 16; i < 64; i++) {
        uint32_t x = w[i - 15], y = w[i - 2];
        w[i] = w[i - 16] + (ror(x, 7) ^ ror(x, 18) ^ (x >> 3)) + w[i - 7] +
               (ror(y, 17) ^ ror(y, 19) ^ (y >> 10));
    }
    uint32_t a = c->state[0], b = c->state[1], d = c->state[3], e = c->state[4];
    uint32_t cc = c->state[2], f = c->state[5], g = c->state[6], h = c->state[7];
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) +
                      ((e & f) ^ (~e & g)) + constants[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void sha256_init(struct sha256_ctx *c)
{
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    for (unsigned i = 0; i < 8; i++)
        c->state[i] = initial[i];
    c->bytes = 0;
    c->used = 0;
}

void sha256_update(struct sha256_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->bytes += n;
    while (n--) {
        c->block[c->used++] = *p++;
        if (c->used == 64) {
            compress(c);
            c->used = 0;
        }
    }
}

void sha256_final(struct sha256_ctx *c, uint8_t digest[32])
{
    uint64_t bits = c->bytes << 3;
    c->block[c->used++] = 0x80;
    if (c->used > 56) {
        while (c->used < 64)
            c->block[c->used++] = 0;
        compress(c);
        c->used = 0;
    }
    while (c->used < 56)
        c->block[c->used++] = 0;
    for (unsigned i = 0; i < 8; i++)
        c->block[63 - i] = (uint8_t)(bits >> (8 * i));
    compress(c);
    for (unsigned i = 0; i < 32; i++)
        digest[i] = (uint8_t)(c->state[i / 4] >> (24 - (i % 4) * 8));
}

void sha256(const void *data, size_t n, uint8_t digest[32])
{
    struct sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, n);
    sha256_final(&c, digest);
}

void sha256_hex(const uint8_t digest[32], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[64] = 0;
}
