/* f6-19: the actual pinned library and port on a synthetic clock calibrated
 * to the T23 1.0.0 alpha serial capture ("[entropy] boot rounds=2644
 * cycles=335678959 ms=335", then health=fail). A model, not a hardware
 * qualification: it reproduces those numbers under the reading recorded in
 * docs/validation/f6-19-entropy-t23.md (the power-up test passes, the FIPS
 * startup memory stage passes with a third of its samples stuck, the SHA-3
 * stage measures a constant interval with interrupts off and raises the RCT
 * alarm), then shows the runtime recovery with interrupt edges perturbing
 * the measured intervals, at the T23's cost per round.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/entropy_jent.h>
#include <ciuki/entropy_pool.h>
#include "jitterentropy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL t23 %u: %s\n",__LINE__,#x); abort(); } } while(0)
#define T23_CYCLES_PER_MS 1002026ull      /* 335678959 cycles / 335 ms */
#define T23_ROUNDS 2644u
#define T23_CYCLES 335678959ull
#define READ_COST 40u                       /* one serialized TSC read */
#define MEM_STUCK 465u                      /* stuck samples in the first 1428 memory rounds */
#define MEM_SPAN 1427u
#define SHA3_INTERVAL 90000u                /* constant with interrupts off */
static uint64_t cycles, start, plan_s, pair;
static int paired;
static unsigned stage = ~0u, reads, rounds_seen, mem_index, allocated, irq_edges;
static uint64_t h0, h1;                     /* the collector's last two deltas */
static uint32_t prng = 0x2a2b2c2du;
static int runtime;                         /* interrupts live: edges perturb intervals */
static uint32_t next(void) { prng ^= prng << 13; prng ^= prng >> 17; prng ^= prng << 5; return prng; }
static uint64_t round_cost(void) { return 126958u + (rounds_seen < 2007u); }   /* sums to T23_CYCLES */
static uint64_t edge(uint64_t interval)
{
    /* PIT 1 kHz plus device edges, about 2 kHz together at 1.002 GHz: an
     * edge lands in an interval with probability interval x 2000 / s, and
     * its service adds 1.5-7.5 us to that interval. */
    if (!runtime || next() % 1000000u >= interval * 2000000u / T23_CYCLES_PER_MS) return 0;
    irq_edges++; return 1500u + next() % 6000u;
}
static uint64_t memory_interval(void)
{
    /* The deterministic stride walk: two values in three distinct, one in
     * three a repeat of the previous interval (stuck, delta2 == 0). */
    unsigned n = mem_index++;
    int stuck = n && (uint64_t)(n + 1) * MEM_STUCK / MEM_SPAN != (uint64_t)n * MEM_STUCK / MEM_SPAN;
    uint64_t v = h0;
    if (!stuck) {
        v = 20000u + next() % 6000u;
        while (v == h0 || v == 2 * h0 - h1) v++;
    }
    return v + edge(v);
}
static void yield(void)
{
    /* Round boundary: the port names the stage before any measurement read. */
    unsigned now = entropy_jent_stage();
    if (now == ENTROPY_JENT_MEMORY && stage != ENTROPY_JENT_MEMORY) { h0 = h1 = 0; mem_index = 0; }
    stage = now; reads = 0;
    uint64_t cost = round_cost();
    if (stage == ENTROPY_JENT_MEMORY || stage == ENTROPY_JENT_SHA3) {
        plan_s = stage == ENTROPY_JENT_MEMORY ? memory_interval() : SHA3_INTERVAL + edge(SHA3_INTERVAL);
        CHECK(plan_s + READ_COST < cost);
        cycles += cost - READ_COST - plan_s;
        h1 = h0; h0 = plan_s;
    } else {
        /* Combined noise: the pseudorandom memory walk varies every round
         * (paired +x/-x, so the attempt's total stays the T23 total). */
        if (paired) { cycles += cost - 17u * READ_COST - pair; paired = 0; }
        else { pair = next() % 1024u; cycles += cost - 17u * READ_COST + pair; paired = 1; }
    }
    rounds_seen++;
}
static uint64_t clock_tick(void)
{
    reads++;
    cycles += (stage == ENTROPY_JENT_MEMORY || stage == ENTROPY_JENT_SHA3) && reads == 2 ? plan_s : READ_COST;
    return cycles;
}
static uint64_t ms(void) { return cycles / T23_CYCLES_PER_MS; }
static void *allocate(size_t size) { void *p = malloc(size); if (p) allocated++; return p; }
static void release(void *p) { CHECK(allocated); allocated--; free(p); }
static const struct entropy_jent_ops ops = {clock_tick, ms, allocate, release, yield, NULL, NULL};
int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct entropy_jent_report r;
    /* Boot: default OSR only, interrupts off. */
    entropy_jent_begin(&ops); start = cycles;
    CHECK(entropy_jent_init(ENTROPY_JENT_MIN_OSR, ENTROPY_JENT_MIN_OSR));
    entropy_jent_report(&r);
    uint64_t spent = cycles - start;
    printf("T23 boot rounds=%u cycles=%llu ms=%llu stages=%u/%u/%u/%u alarm=%04x stage=%u osr=%u round=%u result=%d refused=%zu\n",
           entropy_jent_rounds(), (unsigned long long)spent, (unsigned long long)(spent / T23_CYCLES_PER_MS),
           r.rounds[0], r.rounds[1], r.rounds[2], r.rounds[3], r.alarm, r.alarm_stage, r.alarm_osr, r.alarm_round, r.result, r.refused);
    CHECK(entropy_jent_rounds() == T23_ROUNDS && spent / T23_CYCLES_PER_MS == 335 && !entropy_jent_expired());
    CHECK(spent > T23_CYCLES - 2048u && spent < T23_CYCLES + 2048u);
    CHECK(r.rounds[ENTROPY_JENT_POWERUP] == 1125 && r.rounds[ENTROPY_JENT_MEMORY] == 1428 && r.rounds[ENTROPY_JENT_SHA3] == 91);
    CHECK(r.alarm == JENT_RCT_FAILURE && r.alarm_stage == ENTROPY_JENT_SHA3 && r.alarm_osr == 3 && r.alarm_round == T23_ROUNDS);
    CHECK(r.result == -1 && r.osr == 3 && !r.permanent && !allocated && r.refused == 524288);
    puts("PASS T23 boot reproduction: 2644 rounds in 335 ms; power-up 1125, memory 1428, SHA-3 91; RCT alarm at the "
         "SHA-3 startup stage; the library's own reset asks for 512 KiB, refused before any round; all memory freed");
    /* Same profile, more OSR, interrupts still off: the constant SHA-3 interval
     * keeps failing; each attempt stays inside the 5 s budget. */
    entropy_jent_begin(&ops); start = cycles;
    CHECK(entropy_jent_init(4, ENTROPY_JENT_MAX_OSR));
    entropy_jent_report(&r);
    printf("T23 boot escalation rounds=%u ms=%llu osr=%u expired=%u\n", entropy_jent_rounds(),
           (unsigned long long)((cycles - start) / T23_CYCLES_PER_MS), r.osr, entropy_jent_expired());
    CHECK(entropy_jent_expired() && (cycles - start) / T23_CYCLES_PER_MS <= ENTROPY_COLLECT_MS + 1 && !allocated);
    /* Runtime (the worker after boot): interrupt edges land inside some of
     * the measured intervals. The OSR resumes where boot stopped. */
    runtime = 1; irq_edges = 0;
    entropy_jent_begin(&ops); start = cycles;
    CHECK(!entropy_jent_init(4, ENTROPY_JENT_MAX_OSR));
    entropy_jent_report(&r);
    unsigned init_rounds = entropy_jent_rounds();
    uint64_t init_ms = (cycles - start) / T23_CYCLES_PER_MS;
    CHECK(r.osr == 4 && !r.alarm && !entropy_jent_expired());
    struct entropy_pool pool; entropy_pool_init(&pool); pool.source_ready = true;
    entropy_jent_begin(&ops); start = cycles;
    while (!pool.ready) {
        uint8_t bytes[32];
        CHECK(!entropy_jent_read(bytes));
        entropy_pool_credit(&pool, bytes, sizeof(bytes), cycles / T23_CYCLES_PER_MS);
        entropy_erase(bytes, sizeof(bytes));
    }
    unsigned read_rounds = entropy_jent_rounds();
    uint64_t read_ms = (cycles - start) / T23_CYCLES_PER_MS;
    printf("T23 runtime init rounds=%u ms=%llu osr=4 reads rounds=%u ms=%llu edges=%u pool=ready sources=%u\n",
           init_rounds, (unsigned long long)init_ms, read_rounds, (unsigned long long)read_ms, irq_edges, pool.sources);
    CHECK(pool.ready && pool.reseeds == 1 && pool.sources == ENTROPY_SOURCE_JITTER && read_ms < ENTROPY_COLLECT_MS);
    entropy_jent_close(); CHECK(!allocated);
    puts("PASS T23 runtime: with interrupt edges inside the measured intervals the startup stages pass at OSR 4, "
         "eight 32-byte reads seed the pool inside one 5 s budget; collector freed");
    return 0;
}
