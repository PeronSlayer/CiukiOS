/* Measured availability/sanity only, never a deterministic host gate.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <ciuki/entropy_jent.h>
#include <ciuki/entropy_pool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static unsigned raw[256], samples;
static uint64_t ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) abort();
    return (uint64_t)t.tv_sec*1000 + (unsigned)t.tv_nsec/1000000;
}
static uint64_t cycles(void)
{
#if defined(__i386__) || defined(__x86_64__)
    uint32_t a,b,c,d,lo,hi;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(0) : "memory");
    __asm__ volatile("rdtsc" : "=a"(lo),"=d"(hi) :: "memory");
    uint64_t n = (uint64_t)hi<<32 | lo;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(0) : "memory");
    return n;
#else
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC,&t)) abort();
    return (uint64_t)t.tv_sec*1000000000 + (unsigned)t.tv_nsec;
#endif
}
static void capture(uint64_t delta,uint64_t gcd,unsigned osr)
{ (void)gcd;(void)osr;raw[(uint8_t)delta]++;samples++; }
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    struct entropy_jent_ops ops={cycles,ms,malloc,free,NULL,capture};
    uint64_t start=ms();struct entropy_pool pool;entropy_pool_init(&pool);
    entropy_jent_begin(&ops);
    int error=entropy_jent_init();pool.source_ready=!error;
    for(unsigned i=0;i<8 && !error;i++){
        uint8_t bytes[32];error=entropy_jent_read(bytes);
        if(!error)entropy_pool_credit(&pool,bytes,sizeof(bytes),0);
        entropy_erase(bytes,sizeof(bytes));
    }
    unsigned distinct=0,maximum=0;
    for(unsigned i=0;i<256;i++){if(raw[i])distinct++;if(raw[i]>maximum)maximum=raw[i];}
    printf("host jitter availability=%s rounds=%u ms=%llu raw_samples=%u symbols=%u max=%u expired=%u\n",
           !error && pool.ready ? "available" : "unavailable",entropy_jent_rounds(),
           (unsigned long long)(ms()-start),samples,distinct,maximum,entropy_jent_expired());
    entropy_jent_close();
    if(error || !pool.ready){puts("SKIP statistical output sanity: host source unavailable; no target qualification claimed");return 0;}
    unsigned bins[256]={0},ones=0,outmax=0;uint8_t out[256];
    for(unsigned i=0;i<256;i++){
        if(entropy_pool_read(&pool,out,sizeof(out),0))abort();
        for(unsigned j=0;j<sizeof(out);j++){bins[out[j]]++;for(unsigned bit=0;bit<8;bit++)ones+=(out[j]>>bit)&1;}
    }
    for(unsigned i=0;i<256;i++)if(bins[i]>outmax)outmax=bins[i];
    bool sane=distinct>16 && maximum<(uint64_t)samples*4/5 && ones>235929 && ones<288359 && outmax<512;
    printf("%s statistical sanity ONLY: output_bytes=65536 ones=%u/524288 output_max=%u/65536; hardware non-IID/restart qualification pending\n",sane ? "PASS" : "FAIL",ones,outmax);
    return sane ? 0 : 1;
}
