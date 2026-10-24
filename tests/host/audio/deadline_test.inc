/* F4-24: production refill replay and CPU cost at the modeled boundary.
 * CLOCK_THREAD_CPUTIME_ID excludes host scheduling at nice 19. This is a
 * regression budget, not a claim of measured QEMU/physical latency.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <time.h>
static uint64_t audio_cpu_ns(void)
{
    struct timespec t; CHECK(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));
    return (uint64_t)t.tv_sec*1000000000u+(uint64_t)t.tv_nsec;
}
static void test_service_budget(void)
{
    late_setup();
    struct audio_stream *sources[2]={late_stream(),late_stream()};
    audio.master_in_codec=true; audio.master.left=audio.master.right=50;
    late_start();
    uint64_t cost[3]={0}, peak=0;
    unsigned calls[3]={0};
    struct audio_frame input[AUDIO_QUANTUM];
    for(unsigned i=0;i<AUDIO_QUANTUM;i++) input[i]=(struct audio_frame){-12345,23456};
    unsigned logs=audio_log_count;
    for(unsigned callback=0;callback<400;callback++) {
        for(unsigned s=0;s<2;s++) {
            while(sources[s]->queued<=AUDIO_RING-AUDIO_QUANTUM)
                CHECK(audio_stream_enqueue(sources[s],input,sizeof(input))==(int)sizeof(input));
            if(callback==200) sources[s]->gain[0]=sources[s]->gain[1]=s ? 25 : 50;
        }
        /* QEMU default timer: 10 ms = 480 stereo frames, not 256. */
        mock_advance(480*2); g_ticks+=10; hw.tick+=10;
        uint64_t before=ac97.published, begin=audio_cpu_ns();
        audio_service();
        uint64_t ns=audio_cpu_ns()-begin;
        unsigned periods=(unsigned)(ac97.published-before);
        CHECK(periods==1 || periods==2);
        calls[periods]++; cost[periods]+=ns; if(ns>peak) peak=ns;
        CHECK(!audio.error && audio.running && ac97.published-ac97.retired==AUDIO_LEAD);
        CHECK(audio.max_service_gap_us<=10000 && audio_log_count==logs);
    }
    CHECK(calls[1]==50 && calls[2]==350);
    CHECK(sources[0]->consumed==192000 && sources[1]->consumed==192000);
    CHECK(!sources[0]->starvation && !sources[1]->starvation && !audio.halts && !audio.deadlines);
    /* Generous on a sanitized build; optimized run is the reported cost. */
    CHECK(cost[1]/calls[1]<100000 && cost[2]/calls[2]<200000 && peak<1000000);
    printf("audio cost replay: single=50 mean_ns=%llu double=350 mean_ns=%llu max_ns=%llu gap_us=%u PASS\n",
        (unsigned long long)(cost[1]/calls[1]),(unsigned long long)(cost[2]/calls[2]),
        (unsigned long long)peak,audio.max_service_gap_us);
    for(unsigned s=0;s<2;s++) audio_stream_close(sources[s]);
    late_cleanup();
}
static void test_cost_breakdown(void)
{
    late_setup(); struct audio_stream *sources[2]={late_stream(),late_stream()};
    static struct audio_frame out[AUDIO_QUANTUM];
    const char *names[]={"mix_two_48k_unity","mix_two_48k_gain","mix_two_44k_to_48k","silence_fill","cursor_fetch","descriptor_publish","exhausted_resync"};
    for(unsigned stage=0;stage<ARRAY_SIZE(names);stage++) {
        uint64_t cost=0;
        unsigned count=stage==6 ? 32 : 2000;
        for(unsigned iteration=0;iteration<count;iteration++) {
            for(unsigned s=0;s<2;s++) {
                sources[s]->queued=AUDIO_RING; sources[s]->head=sources[s]->phase=0;
                sources[s]->rate=stage==2 ? 44100 : 48000;
                sources[s]->gain[0]=sources[s]->gain[1]=stage==1 ? (s ? 25 : 50) : 100;
            }
            audio.master_in_codec=true; audio.master.left=audio.master.right=50;
            if(stage>=4) {
                struct ac97_bd *bdl=ac97.bdl; struct audio_frame *pcm[32];
                memcpy(pcm,ac97.pcm,sizeof(pcm));
                hardware_setup(&ac97); hw.any_pcm=true; ac97.ready=true; ac97.bdl=bdl;
                memcpy(ac97.pcm,pcm,sizeof(pcm));
            }
            if(stage>=4) {
                ac97.running=true; ac97.published=AUDIO_LEAD;
                hw.cr=1; hw.sr=0; hw.civ=0; hw.piv=1; hw.lvi=AUDIO_LEAD-1;
                hw.remaining=512;
            }
            if(stage==6) {
                audio.running=true; audio.error=0; idle_since=0; non_silent_end=audio.output;
                ac97.frame_base=audio.output-AUDIO_LEAD*AUDIO_QUANTUM;
                hw.civ=hw.piv=hw.lvi; hw.remaining=0; hw.sr=AC97_DCH|AC97_CELV;
            }
            uint64_t begin=audio_cpu_ns();
            if(stage<3) audio_mix(out,AUDIO_QUANTUM);
            else if(stage==3) memset(out,0,sizeof(out));
            else if(stage==4) CHECK(!ac97_progress(&ac97));
            else if(stage==5) CHECK(!ac97_publish(&ac97,out));
            /* 2026-10-24 lead decision: the underrun grows the lead to the
             * gap budget, so the resync primes 28 silent periods, not 8. */
            else { audio_service(); CHECK(!audio.error && audio.running && ac97.published==audio_gap_lead(AUDIO_GAP_BUDGET_US,AUDIO_RATE)); }
            cost+=audio_cpu_ns()-begin;
        }
        printf("audio cost stage=%s mean_ns=%llu\n",names[stage],(unsigned long long)(cost/count));
    }
    for(unsigned s=0;s<2;s++) audio_stream_close(sources[s]);
    late_cleanup();
}
/* Independent wide oracle pins truncation, signed extrema and master clipping. */
static void test_cheap_arithmetic(void)
{
    const unsigned rates[]={44100,48000};
    uint32_t seed=0x243f6a88;
    for(unsigned n=0;n<20000;n++) {
        seed=seed*1664525u+1013904223u; int16_t a=(int16_t)(seed>>16);
        seed=seed*1664525u+1013904223u; int16_t b=(int16_t)(seed>>16);
        unsigned rate=rates[n&1],phase=seed%rate;
        int64_t expected=a+((int64_t)b-a)*phase/rate;
        CHECK(interpolate(a,b,phase,rate)==expected);
    }
    const int16_t ends[]={-32768,32767};
    for(unsigned a=0;a<2;a++) for(unsigned b=0;b<2;b++)
        for(unsigned r=0;r<2;r++) for(unsigned phase=0;phase<rates[r];phase++)
            CHECK(interpolate(ends[a],ends[b],phase,rates[r])==
                ends[a]+((int64_t)ends[b]-ends[a])*phase/rates[r]);
    const int64_t values[]={-4000000,-3276801,-3276800,-3276799,-65536,-32768,-101,-1,
        0,1,101,32767,65534,3276699,3276700,3276701,4000000};
    for(unsigned gain=0;gain<=100;gain++) {
        for(unsigned i=0;i<ARRAY_SIZE(values);i++)
            CHECK(master_sample(values[i],gain)==saturate(values[i]*gain/100));
        CHECK(master_sample(INT64_MAX,gain)==(gain ? 32767 : 0));
        CHECK(master_sample(INT64_MIN,gain)==(gain ? -32768 : 0));
    }
    puts("audio arithmetic: rational signed-extrema interpolation, exact percentage truncation, wide VM master clipping PASS");
}
static void test_deferred_service_logs(void)
{
    late_setup(); late_start();
    unsigned logs=audio_log_count;
    first_playback=true; playback_start=playback_next=g_ticks-1000;
    audio_flush_logs(); CHECK(audio_log_count==logs);
    /* Periodic protected record is published while RUN remains established. */
    transport_next=g_ticks; audio_flush_logs();
    CHECK(audio_log_count==logs+1 && strstr(audio_log,"transport running"));
    CHECK(strstr(audio_log,"lead_periods=8") && audio.running);
    late_cleanup();
    puts("audio service logs: protected running snapshot without waiting for idle PASS");
}

/* Execute the real worker wait. Its wake predicate is intentionally already
 * true in the last row, reproducing an IRQ racing a long refill. */
static void test_fullscreen_refill_wait(void)
{
    late_setup();late_start();
    uint64_t before=g_ticks;
    audio.pending=false;fullscreen_hook=false;
    audio_worker_wait();CHECK(g_ticks-before==4);
    before=g_ticks;fullscreen_hook=true;
    audio_worker_wait();CHECK(g_ticks-before==2);
    audio.pending=true;before=g_ticks;
    audio_worker_wait();CHECK(g_ticks-before==1 && audio.pending);
    /* The next iteration consumes pending work; an idle full screen retains
     * the cheap 50 ms wait and ordinary audio keeps its 4 ms policy. */
    audio.pending=false;audio.running=false;ac97.published=0;before=g_ticks;
    audio_worker_wait();CHECK(g_ticks-before==50);
    fullscreen_hook=false;late_cleanup();
    puts("f3-39 refill wait: running window=4ms full-screen=2ms, pending IRQ forces real 1ms block without lost wake; idle=50ms PASS");
}
