/* Deterministic model fixtures plus measured host jitter sanity (not a proof).
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
    unsigned i;
    for (i = 0; i < 100000 && !p->failed && (!p->ready || p->estimate_bits < 256); i++)
        entropy_pool_sample(p, noise(), now);
    CHECK(i < 100000 && !p->failed && p->ready);
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
static void health(void)
{
    struct entropy_health h = {0};
    for (unsigned i = 0; i < 40; i++) CHECK(entropy_health_sample(&h, 7));
    CHECK(!entropy_health_sample(&h, 7) && !entropy_health_sample(&h, 8));
    memset(&h, 0, sizeof(h)); unsigned i;
    for (i = 0; i < 512 && entropy_health_sample(&h, (i % 10 == 9) ? (uint8_t)(i / 10 + 1) : 0); i++);
    CHECK(i < 512 && h.failed && h.run < 41); /* biased, not stuck */
    memset(&h, 0, sizeof(h));
    for (i = 0; i < 4096; i++) CHECK(entropy_health_sample(&h, (uint8_t)i));
    CHECK(h.window == 0 && !h.failed);
    struct entropy_pool p; entropy_pool_init(&p);
    for (i = 0; i < 2048; i++) entropy_pool_sample(&p, 100, 0);
    CHECK(p.failed && !p.ready && !p.estimate_bits);
    entropy_pool_init(&p);
    for (i = 1; i < 32768; i++) entropy_pool_sample(&p, i, 0);
    CHECK(!p.failed && !p.ready && !p.estimate_bits); /* linear timer drift */
    puts("PASS raw RCT cutoff, biased APT, window reset, latched failure, third-difference rejection");
}
static void schedule(void)
{
    struct entropy_pool p; uint8_t out[256], saved[256], key[32], expected[64], nonce[12] = {0};
    memset(out, 0xa5, sizeof(out)); memcpy(saved, out, sizeof(out)); entropy_pool_init(&p);
    for (unsigned i = 0; i < 2048; i++) entropy_pool_mix(&p, &i, sizeof(i), ENTROPY_SOURCE_IRQ);
    CHECK(!p.ready && !p.estimate_bits && entropy_pool_read(&p, out, 256, 0) == -EAGAIN && !memcmp(out,saved,256));
    CHECK(!entropy_pool_read(&p, NULL, 0, 0) && entropy_pool_read(&p, out, 257, 0) == -EINVAL);
    for (unsigned i = 0; i < ENTROPY_STARTUP_SAMPLES; i++) entropy_pool_sample(&p, noise(), 7);
    CHECK(!p.ready && !p.estimate_bits);
    while (!p.ready) entropy_pool_sample(&p, noise(), 7);
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
    p.estimate_bits = p.fraction = 0;
    while (p.produced < ENTROPY_RESEED_BYTES) CHECK(!entropy_pool_read(&p,out,256,60007));
    CHECK(entropy_pool_read(&p,out,1,60007)==-EAGAIN);
    feed(&p,60007); CHECK(p.reseeds==3 && !entropy_pool_read(&p,out,256,60007));
    for (unsigned i=0;i<41;i++) entropy_pool_sample(&p,42,60007);
    CHECK(p.failed && !p.ready && entropy_pool_read(&p,out,1,60007)==-EAGAIN);
    for (unsigned i=0;i<32;i++) CHECK(!p.key[i]);
    puts("PASS no uncredited/startup output, key erasure, time/byte reseed thresholds, fresh reseed and failure erasure");
}
static uint64_t host_clock(void)
{
#if defined(__i386__) || defined(__x86_64__)
    uint32_t a,b,c,d,lo,hi;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(0) : "memory");
    __asm__ volatile("rdtsc" : "=a"(lo),"=d"(hi) :: "memory");
    uint64_t value = (uint64_t)hi << 32 | lo;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(0) : "memory");
    return value;
#else
    struct timespec t; CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));
    return (uint64_t)t.tv_sec*1000000000 + (unsigned)t.tv_nsec;
#endif
}
static void sanity(void)
{
    struct entropy_pool p; struct entropy_jitter j = {0};
    unsigned raw[256]={0}, bins[256]={0}, maximum=0, output_maximum=0, distinct=0, ones=0;
    entropy_pool_init(&p);
    for (unsigned i=0;i<65536;i++) {
        uint64_t delta=entropy_jitter_sample(&j,host_clock); raw[(uint8_t)delta]++;
        entropy_pool_sample(&p,delta,0);
    }
    for (unsigned i=0;i<256;i++) { if(raw[i])distinct++; if(raw[i]>maximum)maximum=raw[i]; }
    printf("host jitter: ready=%u failed=%u samples=%u credit=%u raw_symbols=%u raw_max=%u\n",
           p.ready,p.failed,p.samples,p.estimate_bits,distinct,maximum); fflush(stdout);
    CHECK(p.ready && !p.failed && distinct>16 && maximum<52429);
    uint8_t out[256];
    for(unsigned i=0;i<256;i++) {
        CHECK(!entropy_pool_read(&p,out,sizeof(out),0));
        for(unsigned k=0;k<256;k++) { bins[out[k]]++; for(unsigned b=0;b<8;b++)ones+=(out[k]>>b)&1; }
    }
    for(unsigned i=0;i<256;i++){CHECK(bins[i]);if(bins[i]>output_maximum)output_maximum=bins[i];}
    CHECK(ones>235929 && ones<288359 && output_maximum<512);
    printf("PASS host jitter sanity only: samples=65536 raw_symbols=%u raw_max=%u/65536 output_bytes=65536 ones=%u/524288 output_max=%u/65536\n",distinct,maximum,ones,output_maximum);
}
int main(void) { setvbuf(stdout,NULL,_IOLBF,0); vectors(); health(); schedule(); sanity(); return 0; }
