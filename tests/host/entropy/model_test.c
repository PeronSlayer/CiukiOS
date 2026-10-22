/* Deterministic DRBG and pool fixtures; no dependence on host noise.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <ciuki/entropy_pool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL entropy model %u: %s\n",__LINE__,#x); abort(); } } while (0)
static uint32_t fixture = 0x12345678;
static uint64_t noise(void)
{ fixture ^= fixture << 13; fixture ^= fixture >> 17; fixture ^= fixture << 5; return fixture; }
static void feed(struct entropy_pool *p, uint64_t now)
{
    uint8_t bytes[32];
    for (unsigned i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)noise();
    for (unsigned i = 0; i < 8; i++) entropy_pool_credit(p, bytes, sizeof(bytes), now);
}
static void vectors(void)
{
    uint8_t key[32], nonce[12] = {0,0,0,9,0,0,0,0x4a,0,0,0,0}, block[64];
    for (unsigned i = 0; i < 32; i++) key[i] = (uint8_t)i;
    const uint8_t expected[64] = {
        0x10,0xf1,0xe7,0xe4,0xd1,0x3b,0x59,0x15,0x50,0x0f,0xdd,0x1f,0xa3,0x20,0x71,0xc4,
        0xc7,0xd1,0xf4,0xc7,0x33,0xc0,0x68,0x03,0x04,0x22,0xaa,0x9a,0xc3,0xd4,0x6c,0x4e,
        0xd2,0x82,0x64,0x46,0x07,0x9f,0xaa,0x09,0x14,0xc2,0xd7,0x05,0xd9,0x8b,0x02,0xa2,
        0xb5,0x12,0x9c,0xd1,0xde,0x16,0x4e,0xb9,0xcb,0xd0,0x83,0xe8,0xa2,0x50,0x3c,0x4e};
    entropy_chacha20(key, 1, nonce, block); CHECK(!memcmp(block, expected, 64));
    memset(key, 0, sizeof(key)); memset(nonce, 0, sizeof(nonce));
    entropy_chacha20(key, 0, nonce, block);
    const uint8_t zero_prefix[16] = {0x76,0xb8,0xe0,0xad,0xa0,0xf1,0x3d,0x90,0x40,0x5d,0x6a,0xe5,0x53,0x86,0xbd,0x28};
    CHECK(!memcmp(block, zero_prefix, sizeof(zero_prefix)));
    puts("PASS RFC 8439 2.3.2 full block and Appendix A.1 zero vector prefix");
}
static void schedule(void)
{
    struct entropy_pool p; uint8_t out[256], saved[256], key[32], expected[64], nonce[12] = {0};
    memset(out, 0xa5, sizeof(out)); memcpy(saved, out, sizeof(out)); entropy_pool_init(&p);
    for (unsigned i = 0; i < 2048; i++) entropy_pool_mix(&p, &i, sizeof(i), ENTROPY_SOURCE_IRQ);
    CHECK(!p.ready && !p.estimate_bits && entropy_pool_read(&p, out, 256, 0) == -EAGAIN && !memcmp(out,saved,256));
    CHECK(!entropy_pool_read(&p, NULL, 0, 0) && entropy_pool_read(&p, out, 257, 0) == -EINVAL);
    feed(&p,7); CHECK(!p.ready && !p.estimate_bits); /* upstream startup not yet successful */
    p.source_ready = true;
    for (unsigned i = 0; i < 255; i++) entropy_pool_credit(&p, "a", 1, 7);
    CHECK(!p.ready && p.estimate_bits == 255);
    entropy_pool_credit(&p, "a", 1, 7);
    CHECK(p.reseeds == 1 && p.initialised_ms == 7 && !p.estimate_bits);
    for(unsigned i=0;i+32<=sizeof(p.conditioner);i++)
        CHECK(memcmp((const uint8_t *)&p.conditioner+i,p.key,32));
    memcpy(key, p.key, 32); entropy_chacha20(key, 0, nonce, expected);
    CHECK(!entropy_pool_read(&p, out, 32, 7) && !memcmp(p.key,expected,32) && !memcmp(out,expected+32,32));
    CHECK(memcmp(p.key,key,32));
    CHECK(!entropy_pool_read(&p,out,1,60006));
    memset(out,0xa5,256); CHECK(entropy_pool_read(&p,out,256,60007)==-EAGAIN && !memcmp(out,saved,256));
    feed(&p,60007); CHECK(p.reseeds==2 && p.initialised_ms==7);
    /* Consume a full quota without fresh samples. The next read must refuse. */
    p.estimate_bits = 0;
    while (p.produced < ENTROPY_RESEED_BYTES) CHECK(!entropy_pool_read(&p,out,256,60007));
    CHECK(entropy_pool_read(&p,out,1,60007)==-EAGAIN);
    feed(&p,60007); CHECK(p.reseeds==3 && !entropy_pool_read(&p,out,256,60007));
    p.produced=ENTROPY_RESEED_BYTES-128;
    memset(out,0xa5,256);CHECK(entropy_pool_read(&p,out,256,60007)==-EAGAIN && !memcmp(out,saved,256));
    feed(&p,60007);CHECK(!entropy_pool_read(&p,out,256,60007) && p.reseeds==4);
    entropy_pool_fail(&p);
    CHECK(p.failed && !p.ready && entropy_pool_read(&p,out,1,60007)==-EAGAIN);
    for (unsigned i=0;i<32;i++) CHECK(!p.key[i]);
    puts("PASS no uncredited/startup output, key erasure, time/byte reseed thresholds, fresh reseed and failure erasure");
}
/* f6-19 Amendment 4: interrupt timing as a credited source (Linux random.c:
 * count/64 bits per fast-pool mix), bounded by the fast buffer's 128 events. */
static void irq_credit(void)
{
    struct entropy_pool p; entropy_pool_init(&p);
    uint32_t words[7] = {1, 2, 3, 4, 128, 0x4801, 99};
    entropy_pool_credit_irq(&p, words, sizeof(words), 128, 0);   /* no validated TSC yet */
    CHECK(p.sources == ENTROPY_SOURCE_IRQ && !p.estimate_bits && !p.irq_events && !p.irq_bits);
    p.source_ready = true;
    entropy_pool_credit_irq(&p, words, sizeof(words), 63, 0); CHECK(!p.estimate_bits && p.irq_events == 63);
    entropy_pool_credit_irq(&p, words, sizeof(words), 1, 0); CHECK(p.estimate_bits == 1 && !p.irq_events && p.irq_bits == 1);
    entropy_pool_credit_irq(&p, words, sizeof(words), 100000, 0); CHECK(p.estimate_bits == 3 && p.irq_bits == 3);
    entropy_pool_credit_irq(&p, words, sizeof(words), 0, 0); CHECK(p.estimate_bits == 3);
    unsigned drains = 0;
    while (!p.ready) { entropy_pool_credit_irq(&p, words, sizeof(words), 128, 9); CHECK(++drains <= 128); }
    CHECK(drains == 127 && p.reseeds == 1 && p.initialised_ms == 9 && p.irq_bits == 256 && !p.estimate_bits);
    CHECK(p.sources == ENTROPY_SOURCE_IRQ);   /* IRQ-only initialisation */
    uint8_t out[32]; CHECK(!entropy_pool_read(&p, out, sizeof(out), 9));
    while (entropy_pool_wants_credit(&p)) entropy_pool_credit_irq(&p, words, sizeof(words), 128, 10);
    struct entropy_pool saved = p;
    entropy_pool_credit_irq(&p, words, sizeof(words), 128, 10);   /* saturated: mixed only */
    CHECK(p.estimate_bits == 256 && p.irq_bits == saved.irq_bits && !p.irq_events && memcmp(&p.conditioner, &saved.conditioner, sizeof(p.conditioner)));
    /* Both sources: jitter bytes and IRQ edges share one conditioner and one estimate. */
    CHECK(!entropy_pool_read(&p, out, 1, ENTROPY_RESEED_MS + 10) && p.reseeds == 2 && !p.estimate_bits);
    feed(&p, ENTROPY_RESEED_MS + 10); CHECK(p.estimate_bits == 256 && p.sources == (ENTROPY_SOURCE_IRQ | ENTROPY_SOURCE_JITTER));
    entropy_pool_fail(&p); unsigned bits = p.irq_bits;
    entropy_pool_credit_irq(&p, words, sizeof(words), 128, 11); CHECK(p.failed && p.irq_bits == bits && !p.ready);
    puts("PASS f6-19 IRQ credit: none before a validated TSC, 1 bit per 64 edges, 128-edge drain cap, IRQ-only seed after 127 drains, saturation mixes only, shared estimate with jitter, none after failure");
}
int main(void) { setvbuf(stdout,NULL,_IOLBF,0); vectors(); schedule(); irq_credit();
    struct entropy_pool p; entropy_pool_init(&p); p.source_ready = true; feed(&p, 0);
    CHECK(p.ready && entropy_pool_wants_credit(&p)); feed(&p, 0);
    CHECK(p.estimate_bits == 256 && !entropy_pool_wants_credit(&p));
    struct entropy_pool saved = p; feed(&p, 0); CHECK(!memcmp(&p,&saved,sizeof(p)));
    uint8_t byte; CHECK(!entropy_pool_read(&p,&byte,1,ENTROPY_RESEED_MS));
    CHECK(p.reseeds==2 && !p.estimate_bits && entropy_pool_wants_credit(&p));
    puts("PASS source-init prerequisite, one-bit/byte credit, saturation idle and reseed demand"); return 0; }
