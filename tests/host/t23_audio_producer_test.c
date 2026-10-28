/* f1-101: T23 replay model of the Music player beside the desktop (trial
 * a4d9454e, 2026-10-22). Production run queues, priorities, starvation
 * boost, desktop share (f1-98), device budget (f1-90), timers, cpustat,
 * sync.c and the f1-101 producer boost; the production mixer (audio_mix)
 * consumes a production stream (audio_stream_enqueue) and reports its lead
 * (audio_stream_lead); the Music player's pump and reader follow
 * apps/music/player.h (prefill, 500 ms target, music_device_fill). Only CPU
 * time, the stack switch, the DMA clock and the workload are modeled, at
 * 1 us resolution.
 *
 * Trial facts (disk log build/codex/t23-a4d9454e-BOOT.LOG): idle_pm=0 while
 * the Music player played; device_pm 154-181; the decoder thread music#46
 * (normal class, base 7) used 282-388 permille and still produced only
 * reader_fps=35204-40960 (level records), so it costs ~8.55 us of CPU per
 * 48 kHz stereo output frame (343 permille / 40116 fps) and needs ~41 % of
 * the CPU in real time; the compositor desktop#28 (interactive, 9) used
 * 103-502 permille, desktop#30 101-197, desktop#31 64-82, the Music UI
 * music#44 (focused, 9) 58-76. The reservoir held 0/1289/4096 frames with
 * 27-37 starvations per 5 s while [audio] transport reported underruns=0.
 * Workload assumptions (not measurements, stated here and in the
 * validation record): every load is periodic over 20 ms (compositor and
 * desktop workers) or 50 ms (UI); the reader decodes MUSIC_BATCH frames per
 * batch without storage waits (its source reads are urgent and cached);
 * the device class is the f1-98 model's 19 % (refill 350 us every 5 ms,
 * device-work 800 us every 10 ms with a 2110 us section every 250 ms,
 * net-input 300 us every 10 ms).
 *
 * Policies: producer=0 is the pre-f1-101 scheduler (negative control);
 * player=0 is the pre-f1-101 player (2 s prefill, the device stream always
 * filled); literal=1 boosts only below one period of lead (the literal
 * reading of "reservoir below one period", a control).
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kheap_limits.h>
#define HOST_SYNC_TIMERS
#define main latency_fixture_main
#include "kernel_latency_test.c"
#undef main
#include "t23_trial.h"
#define ktime_cycles sync_unused_cycles
#include "../../src/kernel/core/sync.c"
#undef ktime_cycles
static void timer_fixture_reset(void)
{
    timer_root=timer_first=timer_due_head=timer_due_tail=0;
    memset(&timer_stats,0,sizeof(timer_stats));deferred_fn=0;
}
bool g_cpu_tsc;
void klog(const char *fmt,...) { (void)fmt; }

/* Production stream and mixer over a small fake page pool. */
static uint8_t page_pool[8][4096];
static bool page_used[8];
#define P2V(p) ((void *)page_pool[(p)/4096u-1u])
uint32_t pmm_alloc(void)
{
    for (unsigned i=0;i<8;i++) if (!page_used[i]) { page_used[i]=true; return (i+1)*4096u; }
    return 0;
}
void pmm_free(uint32_t phys) { CHECK(phys && page_used[phys/4096u-1u]); page_used[phys/4096u-1u]=false; }
void *(kzalloc)(size_t n) { if (n > KHEAP_MAX_REQUEST) return 0; return calloc(1,n); }
void kfree(void *p) { free(p); }
void audio_wake(void) { }
void audio_fail(int error) { (void)error; CHECK(false); }
static bool literal;
void model_lead(const void *source, bool behind);
#define sched_audio_lead model_lead
#include "../../src/kernel/audio/mixer.c"
#undef sched_audio_lead
#include "../../src/kernel/audio/stream.c"
#include "../../apps/music/player.h"
/* The literal control replaces the lead report, nothing else. */
void model_lead(const void *source, bool behind) { if (!literal) sched_audio_lead(source,behind); }
static bool literal_behind;
static void literal_update(struct audio_stream *s)
{
    if (!literal) return;
    bool behind=s->started && !s->draining &&
        audio_producer_behind(literal_behind,s->queued,AUDIO_QUANTUM);
    if (behind!=literal_behind) { literal_behind=behind; sched_audio_lead(s,behind); }
}

#ifndef RUN_MS
#define RUN_MS 46000u /* the 23 trial windows */
#endif
#define SETTLE_MS 3000u           /* gates start after the first 3 s */
#define REFILL_US 350u
#define PUMP_US 60u
#define CAPACITY (MUSIC_RING-2u*MUSIC_DEVICE_FRAMES) /* ring minus audible history */

struct actor {
    struct task *t;
    uint64_t remaining, cpu;
    void (*done)(struct actor *);
    void (*resume)(struct actor *);
    unsigned step;
};
static struct task refill, work, net, compositor, worker30, worker31, music_ui, pump, decoder;
static struct actor actors[10];
static unsigned actor_count;
/* scenario */
static bool player_new, mirror;
static unsigned comp_us, w30_us, w31_us, ui_us, decode_ns;
/* model state */
static struct audio_stream *pcm;
static struct audio_frame quantum[AUDIO_QUANTUM];
static uint8_t zero_pcm[MUSIC_DEVICE_FRAMES*4];
static uint64_t mixed_periods, start_ms;
static unsigned reservoir, batch, decoded_total;
static bool started, submitted, reader_parked;
/* results */
static uint64_t refill_last, refill_gap_max;
static unsigned lead_min, reservoir_min, starvation_base;
static uint16_t boost_ms[RUN_MS];

static uint64_t boundary(void) { return 1 + (g_ticks + 1) * 1000; }
static uint64_t now_us(void) { return cycles - 1; }
static struct actor *actor_of(struct task *t)
{
    for (unsigned i=0;i<actor_count;i++) if (actors[i].t==t) return &actors[i];
    return 0;
}
static void park(void) { g_current->state=T_BLOCKED; g_current->wake_tick=0; schedule(); }
static void wake(struct task *t) { if (t->state==T_BLOCKED) task_start(t); }
static void sleep_then(struct actor *a, uint32_t ms_, void (*resume)(struct actor *))
{ a->resume=resume; task_sleep_ms(ms_ ? ms_ : 1); }
static void tick(void)
{
    cycles=boundary(); g_ticks++;
    struct trap_frame tf={.vector=0x20,.cs=g_current->user ? 0x1b : 8};
    uint64_t start=cpu_irq_enter(&tf); sched_tick(); cpu_irq_leave(start);
    uint32_t saved=flags; flags=0x200;
    for (unsigned n=0;deferred_fn && n<1024;n++) { kwork_fn fn=deferred_fn; deferred_fn=0; fn(deferred_arg); }
    flags=saved;
}
static void consume(struct actor *a, uint64_t step)
{
    cycles+=step; a->remaining-=step; a->cpu+=step;
    /* boosted producer CPU per ms, in 10 us units (the sliding-window bound) */
    if (producer_turn(a->t) && g_ticks<RUN_MS) boost_ms[g_ticks]+=(uint16_t)((step+5)/10);
}

/* Audio refill: the mixer consumes one 256-frame period every 5.333 ms. */
static void refill_resume(struct actor *a)
{
    uint64_t now=now_us();
    if (refill_last && now-refill_last>refill_gap_max) refill_gap_max=now-refill_last;
    refill_last=now;
    while (mixed_periods*AUDIO_QUANTUM*1000u/48u<=now) {
        if (started && g_ticks>=start_ms+SETTLE_MS && pcm->queued<lead_min) lead_min=pcm->queued;
        audio_mix(quantum,AUDIO_QUANTUM); mixed_periods++;
        literal_update(pcm);
    }
    a->remaining=REFILL_US;
}
static void refill_done(struct actor *a) { sleep_then(a,5,refill_resume); }
static void work_resume(struct actor *a) { a->remaining=++a->step%25 ? 800u : T23_DEVICE_WORK_US; }
static void work_done(struct actor *a) { sleep_then(a,10,work_resume); }
static void net_resume(struct actor *a) { a->remaining=300; }
static void net_done(struct actor *a) { sleep_then(a,10,net_resume); }
/* The trial's desktop load, window by window ([cpu] top, 2 s windows from
 * the start of the first track): permille of the CPU obtained by the
 * compositor desktop#28, the desktop workers desktop#30 and desktop#31 and
 * the Music UI music#44 (60 where the window does not list it). Used as
 * their demand: a lower bound, since every window had idle_pm=0. Windows
 * 1-15 are the first track, 16-23 the second (compositor at 32-50 %). */
struct trial_window { uint16_t compositor, worker30, worker31, ui; };
static const struct trial_window trial_trace[]={
    {308,125,65,76},{214,142,64,60},{176,154,64,60},{133,162,72,58},{112,177,75,60},
    {135,170,77,60},{108,197,77,60},{116,131,79,60},{185,111,71,60},{298,76,60,63},
    {128,129,76,60},{104,126,82,60},{103,124,81,60},{111,102,75,60},{169,101,72,60},
    {157,130,76,71},{148,113,74,60},{424,46,40,60},{502,43,35,60},{321,80,56,60},
    {427,48,40,60},{424,51,44,60},{141,90,64,60},
};
#define TRIAL_WINDOWS (sizeof(trial_trace)/sizeof(*trial_trace))
static bool use_trace;
static const struct trial_window *window_now(void)
{ return &trial_trace[(g_ticks/2000u)%TRIAL_WINDOWS]; }
/* Desktop loads: work per period, then sleep to the next period start. */
static void comp_resume(struct actor *a) { a->remaining=use_trace ? window_now()->compositor*20u : comp_us; }
static void comp_done(struct actor *a) { sleep_then(a,20-(uint32_t)(g_ticks%20),comp_resume); }
static void w30_resume(struct actor *a) { a->remaining=use_trace ? window_now()->worker30*20u : w30_us; }
static void w30_done(struct actor *a) { sleep_then(a,20-(uint32_t)((g_ticks+7)%20),w30_resume); }
static void w31_resume(struct actor *a) { a->remaining=use_trace ? window_now()->worker31*20u : w31_us; }
static void w31_done(struct actor *a) { sleep_then(a,20-(uint32_t)((g_ticks+13)%20),w31_resume); }
static void ui_resume(struct actor *a) { a->remaining=use_trace ? window_now()->ui*50u : ui_us; }
static void ui_done(struct actor *a) { sleep_then(a,50-(uint32_t)(g_ticks%50),ui_resume); }

/* Music reader (player.c reader()): the prefill exactly, then batches while
 * a whole batch fits; each batch names the thread (AUDIO_REFILL_READ). */
static unsigned prefill(void)
{ return music_frames_ms(48000,player_new ? MUSIC_PREFILL_MS : 2000u); }
static void decoder_resume(struct actor *a)
{
    unsigned n=MUSIC_BATCH;
    if (!started && reservoir+n>prefill()) n=prefill()-reservoir;
    if (CAPACITY-reservoir<MUSIC_BATCH || !n) { reader_parked=true; a->resume=decoder_resume; park(); return; }
    audio_stream_producer(pcm,&decoder,SCHED_PRODUCER_READER);
    if (literal) sched_audio_lead(pcm,literal_behind);
    batch=n; a->remaining=(uint64_t)n*decode_ns/1000u;
}
static void decoder_done(struct actor *a)
{
    reservoir+=batch; decoded_total+=batch; batch=0;
    if (reservoir>MUSIC_RING/2) { a->resume=decoder_resume; g_need_resched=false; schedule(); return; }
    decoder_resume(a);
}
/* Music pump (player.c music_player_pump(), 2 ms timed wait). */
static void pump_resume(struct actor *a) { a->remaining=PUMP_US; }
static void pump_done(struct actor *a)
{
    if (!started && reservoir>=prefill()) { started=true; start_ms=g_ticks; starvation_base=(unsigned)pcm->starvation; }
    if (started) {
        unsigned target=mirror ? music_frames_ms(48000,MUSIC_TARGET_MS) : 0;
        unsigned fill=music_device_fill(reservoir,target,0), queued=pcm->queued;
        unsigned limit=submitted ? fill : 256;
        if (reservoir && queued<(submitted ? fill-256 : 256)) {
            unsigned n=limit-queued; if (n>reservoir) n=reservoir;
            audio_stream_producer(pcm,&pump,SCHED_PRODUCER_WRITER); /* audio_write_user */
            if (literal) sched_audio_lead(pcm,literal_behind);
            CHECK(audio_stream_enqueue(pcm,zero_pcm,n*4)==(int)(n*4));
            literal_update(pcm);
            reservoir-=n; submitted=true;
        }
        if (g_ticks>=start_ms+SETTLE_MS && reservoir<reservoir_min) reservoir_min=reservoir;
    }
    if (reader_parked && (started ? CAPACITY-reservoir>=MUSIC_BATCH : reservoir<prefill())) {
        reader_parked=false; wake(&decoder);
    }
    sleep_then(a,2,pump_resume);
}

static void actor_add(struct task *t, const char *name, enum task_prio prio, uint32_t id,
                      void (*resume)(struct actor *), void (*done)(struct actor *))
{
    *t=(struct task){.id=id,.generation=1,.prio=prio,.state=T_BLOCKED,.user=prio!=P_DEVICE};
    strcpy(t->name,name);
    struct actor *a=&actors[actor_count++];
    *a=(struct actor){.t=t,.resume=resume,.done=done};
    t->all_next=idle.all_next; idle.all_next=t;
}
static void model_reset(bool producer)
{
    reset(); audio_producer=producer;
    normal.state=hog.state=ui.state=T_BLOCKED; idle.all_next=0;
    waiters=0; held=0; owned_queues=0; propagating=false; dependents=0;
    actor_count=0;
    if (pcm) audio_stream_close(pcm);
    memset(&audio,0,sizeof(audio)); audio.ready=true; audio.caps.output_rate=AUDIO_RATE;
    pcm=0; CHECK(!audio_stream_open(&pcm));
    mixed_periods=start_ms=0; reservoir=batch=decoded_total=0;
    started=submitted=reader_parked=literal_behind=false;
    refill_last=refill_gap_max=0; lead_min=UINT32_MAX; reservoir_min=UINT32_MAX; starvation_base=0;
    memset(boost_ms,0,sizeof(boost_ms));
    actor_add(&refill,"audio-refill",P_DEVICE,17,refill_resume,refill_done);
    actor_add(&work,"device-work",P_DEVICE,9,work_resume,work_done);
    actor_add(&net,"net-input",P_DEVICE,22,net_resume,net_done);
    actor_add(&compositor,"desktop",P_INTERACTIVE,28,comp_resume,comp_done);
    actor_add(&worker30,"desktop",P_NORMAL,30,w30_resume,w30_done);
    actor_add(&worker31,"desktop",P_NORMAL,31,w31_resume,w31_done);
    actor_add(&music_ui,"music",P_NORMAL,44,ui_resume,ui_done);
    actor_add(&pump,"music",P_NORMAL,45,pump_resume,pump_done);
    actor_add(&decoder,"music",P_NORMAL,46,decoder_resume,decoder_done);
    task_set_audio_class(&refill);
    task_set_share_group(&compositor,true); task_set_share_group(&worker30,true);
    task_set_share_group(&worker31,true);
    task_focus(&music_ui);
    task_start(&refill); task_start(&work); task_start(&net); task_start(&compositor);
    if (w30_us) task_start(&worker30);
    if (w31_us) task_start(&worker31);
    if (ui_us) task_start(&music_ui);
    task_start(&pump); task_start(&decoder);
    schedule();
}
static void run_until(uint64_t end_tick)
{
    while (g_ticks<end_tick) {
        struct actor *a=actor_of(g_current);
        if (!a) { tick(); if (g_need_resched || rq_any()) schedule(); continue; }
        if (!a->remaining) {
            void (*resume)(struct actor *)=a->resume; a->resume=0;
            CHECK(resume); resume(a);
            continue;
        }
        uint64_t room=boundary()-cycles;
        uint64_t step=a->remaining<room ? a->remaining : room;
        if (step) consume(a,step);
        if (!a->remaining) { a->done(a); continue; }
        tick();
        kpreempt_point();
    }
}
/* Largest boosted producer CPU in any sliding 100 ms window, in us. */
static unsigned boost_window_max(void)
{
    unsigned best=0, sum=0;
    for (unsigned i=0;i<RUN_MS;i++) {
        sum+=boost_ms[i]; if (i>=100) sum-=boost_ms[i-100];
        if (sum>best) best=sum;
    }
    return best*10;
}

struct result {
    unsigned starvations, lead_min, reservoir_min, boost_window_us;
    uint64_t refill_gap_us;
    double decoder_pct, comp_pct, desktop_pct, device_pct;
    struct sched_producer_stats stats;
    unsigned refill_overruns, refill_device_overruns;
    bool started;
};
static struct result scenario(bool producer, bool player, bool literal_)
{
    player_new=player; mirror=player; literal=literal_;
    model_reset(producer);
    run_until(RUN_MS);
    struct result r={0};
    r.started=started;
    r.starvations=started ? (unsigned)(pcm->starvation-starvation_base) : UINT32_MAX;
    r.lead_min=lead_min; r.reservoir_min=reservoir_min; r.refill_gap_us=refill_gap_max;
    r.boost_window_us=boost_window_max();
    double total=(double)RUN_MS*1000;
    r.decoder_pct=100*actors[8].cpu/total; r.comp_pct=100*actors[3].cpu/total;
    r.desktop_pct=100*(actors[4].cpu+actors[5].cpu)/total;
    r.device_pct=100*(actors[0].cpu+actors[1].cpu+actors[2].cpu)/total;
    sched_audio_producer_stats(pcm,&r.stats);
    /* f1-90: the refill's own budget never overran and it was never demoted. */
    r.refill_overruns=audio_used;
    for (unsigned i=0;i<overrun_used;i++) if (overrun_events[i].id==refill.id) r.refill_device_overruns++;
    return r;
}
static void row(const char *name, const struct result *r)
{
    printf("  %-34s starvations=%-5u lead_min=%-5u reservoir_min=%-6u decoder=%4.1f%% compositor=%4.1f%% desktop=%4.1f%% device=%4.1f%% boosts=%u over_budget=%u boost_ms=%llu window_max_us=%u refill_gap_us=%llu\n",
        name,r->starvations,r->lead_min==UINT32_MAX ? 0 : r->lead_min,r->reservoir_min==UINT32_MAX ? 0 : r->reservoir_min,
        r->decoder_pct,r->comp_pct,r->desktop_pct,r->device_pct,r->stats.boosts,r->stats.over_budget,
        (unsigned long long)(r->stats.boost_cycles/(r->stats.cycles_per_ms ? r->stats.cycles_per_ms : 1)),
        r->boost_window_us,(unsigned long long)r->refill_gap_us);
}
/* Every scenario: the refill keeps its own budget and is never demoted;
 * the producer boost stays within 20 ms of boosted CPU in every sliding
 * 100 ms window, give or take the 1 ms PIT granularity of the charge (CPU
 * is charged at the next tick) and of the enforcement (the boost ends at
 * the charge that crosses the budget): BOOST_SLACK_US. */
#define BOOST_SLACK_US 2000u
static uint64_t control_gap; /* the negative control's longest refill gap */
static void common_gates(const struct result *r)
{
    CHECK(!r->refill_overruns && !r->refill_device_overruns);
    CHECK(r->boost_window_us<=PRODUCER_BUDGET_MS*1000u+BOOST_SLACK_US);
    /* below device service: the refill's longest service gap does not grow */
    CHECK(r->refill_gap_us<=control_gap);
}

/* Trial a4d9454e: the trial's desktop windows and the trial's decoder cost
 * (8550 ns per frame, 41 % of the CPU). The CPU is oversubscribed in most
 * windows (device 18 % + desktop 30-64 % + decoder 41 %): the negative
 * control reproduces the trial's starvations; f1-101 cuts them but cannot
 * remove them within a 20 % budget (the validation record lists the
 * budget and decoder-cost sweeps). At 7000 ns per frame (34 %) the same
 * windows are schedulable with the boost and f1-101 removes every gap. */
static void trial_replay(void)
{
    use_trace=true; comp_us=w30_us=w31_us=ui_us=1;
    static const unsigned costs[]={8550,7000};
    for (unsigned i=0;i<sizeof(costs)/sizeof(*costs);i++) {
        decode_ns=costs[i];
        struct result off=scenario(false,false,false); control_gap=off.refill_gap_us;
        struct result player_only=scenario(false,true,false);
        struct result fill=scenario(true,false,false);
        struct result on=scenario(true,true,false);
        struct result lit=scenario(true,true,true);
        printf("F1-101 replay a4d9454e trial windows, decoder %u ns/frame (%.0f%% of the CPU at 48 kHz), %u s:\n",
            decode_ns,decode_ns*48.0/1000/10,RUN_MS/1000);
        row("pre-f1-101 (negative control)",&off); row("player only (no kernel boost)",&player_only);
        row("boost, pre-f1-101 player",&fill); row("f1-101: boost + player",&on);
        row("literal: boost below one period",&lit);
        common_gates(&off); common_gates(&player_only); common_gates(&fill); common_gates(&on); common_gates(&lit);
        /* The negative control reproduces the trial: the decoder starves. */
        CHECK(off.started && off.starvations>0 && on.started && on.stats.boosts>0);
        if (decode_ns==8550) {
            /* Oversubscribed: at least halves the gaps, never worse than the
             * literal reading of the threshold or a boost without the player. */
            CHECK(on.starvations*2<off.starvations && on.starvations<lit.starvations &&
                  on.starvations<fill.starvations && on.decoder_pct>off.decoder_pct);
            printf("  gates (trial cost): negative control starves, f1-101 halves the gaps (%u -> %u), beats the literal threshold (%u) PASS\n",
                off.starvations,on.starvations,lit.starvations);
        } else {
            CHECK(!on.starvations && on.lead_min>=AUDIO_QUANTUM);
            printf("  gates (%u ns/frame): negative control starves, f1-101 keeps the lead >= one period, no gap PASS\n",decode_ns);
        }
    }
    use_trace=false;
}

/* A decoder under a 50 % interactive load beside a busy desktop worker. */
static void interactive_load(void)
{
    comp_us=10000; w30_us=3000; w31_us=0; ui_us=3000;
    static const unsigned costs[]={2000,4000,5000,8550};
    printf("F1-101 replay, compositor 50 %% (10 ms of every 20 ms), desktop worker 15 %%, UI 6 %%, device 18 %%:\n");
    for (unsigned i=0;i<sizeof(costs)/sizeof(*costs);i++) {
        decode_ns=costs[i];
        struct result off=scenario(false,false,false); control_gap=off.refill_gap_us;
        struct result on=scenario(true,true,false);
        char name[64];
        snprintf(name,sizeof(name),"decoder %u ns/frame, pre-f1-101",decode_ns); row(name,&off);
        snprintf(name,sizeof(name),"decoder %u ns/frame, f1-101",decode_ns); row(name,&on);
        common_gates(&off); common_gates(&on);
        if (decode_ns<=5000) {
            /* Up to 24 % of the CPU for the decoder the boost is sufficient:
             * the mixer's lead never falls below one period. */
            CHECK(on.started && !on.starvations && on.lead_min>=AUDIO_QUANTUM);
            if (decode_ns>=4000) CHECK(off.starvations>0); /* and necessary */
        } else CHECK(on.starvations<off.starvations); /* 41 %: unschedulable in 20 % */
    }
    printf("  gates: 2000-5000 ns/frame no gap and lead >= one period, 4000-5000 negative control starves; 8550 fewer gaps PASS\n");
}

/* Budget: a producer that can never catch up stays boosted only 20 % of
 * every 100 ms; the boost returns when the window slides. */
static void budget_bound(void)
{
    comp_us=10000; w30_us=3000; w31_us=0; ui_us=3000; decode_ns=30000;
    struct result r=scenario(true,true,false);
    row("budget: decoder 30000 ns/frame (never catches up)",&r);
    common_gates(&r);
    CHECK(r.stats.over_budget>0 && r.stats.boosts>1);
    CHECK(r.boost_window_us>=PRODUCER_BUDGET_MS*1000u-1000u);
    printf("F1-101 budget bound: boosted producer CPU max %u us per 100 ms (budget %u ms), boosts=%u over_budget=%u PASS\n",
        r.boost_window_us,PRODUCER_BUDGET_MS,r.stats.boosts,r.stats.over_budget);
}

/* Unit checks of the scheduler interface on the same fixture. */
static void running(struct task *t)
{
    if (t->state==T_READY) rq_remove(t);
    t->state=T_RUNNING; g_current=t; priority_init(t); t->account_start=cycles;
}
static char cache_object;
static void unit_checks(void)
{
    comp_us=w30_us=w31_us=ui_us=0; decode_ns=1000;
    model_reset(true);
    /* a registered producer is not boosted until the mixer reports behind */
    audio_stream_producer(pcm,&decoder,SCHED_PRODUCER_READER);
    CHECK(!decoder.producer_priority && decoder.effective_priority==CIUKI_SCHED_NORMAL);
    pcm->started=true; pcm->queued=AUDIO_PRODUCER_TARGET; audio_stream_lead(pcm);
    CHECK(!pcm->producer_behind);
    pcm->queued=AUDIO_PRODUCER_BEHIND; audio_stream_lead(pcm); CHECK(!pcm->producer_behind);
    pcm->queued=AUDIO_PRODUCER_BEHIND-1; audio_stream_lead(pcm);
    CHECK(pcm->producer_behind && decoder.effective_priority==SCHED_AUDIO_PRODUCER);
    /* hysteresis: refilled only at the target lead */
    pcm->queued=AUDIO_PRODUCER_TARGET-1; audio_stream_lead(pcm); CHECK(decoder.producer_priority);
    pcm->queued=AUDIO_PRODUCER_TARGET; audio_stream_lead(pcm);
    CHECK(!decoder.producer_priority && decoder.effective_priority==CIUKI_SCHED_NORMAL);
    /* a writer registered while the stream is behind is raised at once */
    pcm->queued=0; audio_stream_lead(pcm);
    audio_stream_producer(pcm,&pump,SCHED_PRODUCER_WRITER);
    CHECK(pump.effective_priority==SCHED_AUDIO_PRODUCER && decoder.effective_priority==SCHED_AUDIO_PRODUCER);
    /* a replaced reader loses the boost; a draining stream needs nobody */
    audio_stream_producer(pcm,&music_ui,SCHED_PRODUCER_READER);
    CHECK(!decoder.producer_priority && music_ui.producer_priority);
    pcm->draining=true; audio_stream_lead(pcm);
    CHECK(!pump.producer_priority && !music_ui.producer_priority); pcm->draining=false;
    /* device-class tasks are never producer-boosted */
    audio_stream_producer(pcm,&work,SCHED_PRODUCER_READER); audio_stream_lead(pcm);
    CHECK(pump.producer_priority && !work.producer_priority && work.effective_priority==SCHED_DEVICE);
    audio_stream_producer(pcm,&decoder,SCHED_PRODUCER_READER);
    /* donation: a boosted waiter lends the top of the ordinary band only */
    running(&decoder);
    CHECK(decoder.effective_priority==SCHED_AUDIO_PRODUCER && task_donor_priority(&decoder)==SCHED_BOOST_CAP);
    ksync_depend(&worker30,&cache_object,KSYNC_DEPEND_CACHE);
    CHECK(worker30.inherited_priority==SCHED_BOOST_CAP && worker30.effective_priority==SCHED_BOOST_CAP);
    ksync_depend(0,0,0);
    CHECK(!worker30.inherited_priority && worker30.effective_priority==CIUKI_SCHED_NORMAL);
    /* the boosted CPU is the producer budget, never device service */
    uint64_t device_before=device_cpu_total;
    running(&decoder);
    for (unsigned i=0;i<PRODUCER_BUDGET_MS+2 && !producer_over;i++) run_tick(0);
    CHECK(producer_over && device_cpu_total==device_before && !decoder.producer_priority);
    CHECK(decoder.producer_behind && decoder.effective_priority==CIUKI_SCHED_NORMAL);
    struct sched_producer_stats st; sched_audio_producer_stats(pcm,&st);
    CHECK((st.flags&SCHED_PRODUCER_OVER) && st.over_budget==1 && st.boosts>=1 && st.producers==2);
    for (unsigned i=0;i<PRODUCER_WINDOW && producer_over;i++) run_tick(0);
    CHECK(!producer_over && decoder.producer_priority);
    sched_audio_producer_stats(pcm,&st); CHECK(st.flags&SCHED_PRODUCER_BOOSTED);
    /* the record names the producers */
    record_bytes=0; records[0]=0; g_boot.flags=CBI_F_VERBOSE; cpu_emit(); g_boot.flags=0;
    CHECK(strstr(records,"[sched] audio_producer task=music#46 boosts="));
    CHECK(strstr(records,"over_budget=1"));
    /* the scheduler negative control: no boost at all */
    audio_producer=false; producer_refresh_all(); CHECK(!decoder.producer_priority && !pump.producer_priority);
    audio_producer=true; producer_refresh_all();
    /* closing (or resetting) the stream ends every boost */
    audio_stream_reset(pcm);
    CHECK(!pump.producer_priority && !decoder.producer_priority && !pump.producer_behind);
    sched_audio_producer_stats(pcm,&st); CHECK(!st.producers && !st.flags);
    puts("F1-101 producer unit checks: lead hysteresis 1536/1792, late registration, roles, draining, device class, donation cap 15, budget withholding and return, record, reset PASS");
}
int main(void)
{
    g_tsc_per_ms=1000;
    unit_checks();
    trial_replay();
    interactive_load();
    budget_bound();
    return 0;
}
