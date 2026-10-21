/* Actual upstream health/conditioning and deterministic port clocks.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/entropy_jent.h>
#include "jitterentropy.h"
#include "jitterentropy-internal.h"
#include "jitterentropy-health.h"
#include "jitterentropy-sha3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL jent %u: %s\n",__LINE__,#x); abort(); } } while(0)
static unsigned fixture=0x12345678, yields, captured, allocated;
static uint64_t ticks, cycles;
static bool stall, timed, no_memory;
static uint64_t clock_tick(void)
{
    fixture^=fixture<<13;fixture^=fixture>>17;fixture^=fixture<<5;
    if (!stall) cycles += 1000+(fixture%100000);
    return cycles;
}
static uint64_t ms(void) { return ticks; }
static void yield(void) { yields++; if(timed) ticks += 1000; }
static void *allocate(size_t size) { if(no_memory)return NULL;allocated++;return malloc(size); }
static void release(void *p) { CHECK(allocated);allocated--;free(p); }
static void raw(uint64_t delta,uint64_t gcd,unsigned osr) { (void)delta;CHECK(gcd && osr>=3);captured++; }
static const struct entropy_jent_ops ops={clock_tick,ms,allocate,release,yield,raw};
static void health(void)
{
    struct rand_data ec={.osr=3,.fips_enabled=1};
    jent_health_init(&ec,jent_health_init_type_common);
    for(unsigned i=0;i<ec.rct_cutoff+3;i++)jent_stuck(&ec,7);
    CHECK(ec.health_failure & JENT_RCT_FAILURE);
    for(unsigned i=0;i<ec.rct_cutoff_permanent;i++)jent_stuck(&ec,7);
    CHECK(ec.health_failure & JENT_RCT_FAILURE_PERMANENT);
    memset(&ec,0,sizeof(ec));ec.osr=3;ec.fips_enabled=1;
    jent_health_init(&ec,jent_health_init_type_common);
    for(unsigned i=0;i<512;i++)jent_stuck(&ec,i%10 != 9 ? 100 : 200+i);
    CHECK(ec.health_failure & (JENT_APT_FAILURE | JENT_APT_FAILURE_PERMANENT));
    CHECK(ec.rct_count<ec.rct_cutoff);
    memset(&ec,0,sizeof(ec));ec.osr=3;ec.fips_enabled=1;
    jent_health_init(&ec,jent_health_init_type_common);
    for(unsigned i=0;i<4096;i++){fixture^=fixture<<13;fixture^=fixture>>17;fixture^=fixture<<5;jent_stuck(&ec,fixture);}
    CHECK(!jent_health_failure(&ec));
    CHECK(!jent_sha3_tester());
    puts("PASS upstream SHA3/SHAKE known answers, RCT permanent/stuck, biased APT and synthetic healthy stream");
}
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);health();
    uint8_t bytes[32];
    entropy_jent_begin(&ops);CHECK(!entropy_jent_init());
    CHECK(!entropy_jent_read(bytes) && captured>1024 && yields>captured);
    entropy_jent_begin(&ops);ticks+=ENTROPY_COLLECT_MS;memset(bytes,0xa5,sizeof(bytes));
    CHECK(entropy_jent_read(bytes) && entropy_jent_expired());
    for(unsigned i=0;i<sizeof(bytes);i++)CHECK(bytes[i]==0xa5);
    entropy_jent_close();CHECK(!allocated);
    timed=true;entropy_jent_begin(&ops);CHECK(entropy_jent_init());
    CHECK(entropy_jent_expired() && entropy_jent_rounds()<=5);
    entropy_jent_close();CHECK(!allocated);timed=false;
    entropy_jent_begin(&ops);
    struct rand_data ec={0};
    for(unsigned i=0;i<ENTROPY_COLLECT_ROUNDS;i++)CHECK(!ciuki_jent_round(&ec));
    CHECK(ciuki_jent_round(&ec) && entropy_jent_expired() && (ec.health_failure & JENT_RCT_FAILURE_PERMANENT));
    stall=true;entropy_jent_begin(&ops);CHECK(entropy_jent_init());entropy_jent_close();CHECK(!allocated);stall=false;
    no_memory=true;entropy_jent_begin(&ops);CHECK(entropy_jent_init());entropy_jent_close();CHECK(!allocated);
    puts("PASS actual library synthetic-clock init/read, bounded time/rounds, stalled clock and allocation refusal; all allocations freed");
    return 0;
}
