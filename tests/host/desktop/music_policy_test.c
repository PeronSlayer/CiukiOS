/* SPDX-License-Identifier: MIT
 * Drive the production pump, publication policy and renderer with an independent
 * DAC clock. CPU contention withholds service for 10 ms in every 20 ms.
 * Include the private runtime to exercise its publisher without wall-clock waits. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include "../../../apps/music/view.h"
static uint64_t test_now;
static int model_clock(clockid_t id, struct timespec *t)
{ (void)id; t->tv_sec=(time_t)(test_now/1000000000); t->tv_nsec=(long)(test_now%1000000000); return 0; }
#define clock_gettime model_clock
#include "../../../apps/music/runtime.c"
#undef clock_gettime
struct device { uint64_t accepted, consumed, last, credit; unsigned queued, writes, max_request, gaps; int partial, blocked; };
static uint64_t cpu(void)
{ struct timespec t; assert(!clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static int device_open(void *ctx,unsigned *rate) { (void)ctx; *rate=48000; return 0; }
static int device_write(void *ctx,const int16_t *pcm,unsigned n)
{
    struct device *d=ctx; assert(pcm && n && n<=2048 && n<=2048-d->queued);
    if (!d->accepted) assert(n<=256);
    if (n>d->max_request) d->max_request=n;
    if (d->blocked) return -EAGAIN;
    if (d->partial && n>7) n=7;
    d->queued+=n; d->accepted+=n; d->writes++; return (int)n;
}
static int device_status(void *ctx,uint64_t *consumed,unsigned *queued,uint64_t *tail)
{
    struct device *d=ctx;
    uint64_t credit=(test_now-d->last)*48000+d->credit, want=credit/1000000000;
    d->credit=credit%1000000000; d->last=test_now;
    if (d->accepted && want>d->queued) d->gaps++;
    unsigned used=want<d->queued ? (unsigned)want : d->queued;
    d->queued-=used; d->consumed+=used;
    *consumed=d->consumed; *queued=d->queued; *tail=0; return 0;
}
static void device_close(void *ctx) { (void)ctx; }
static uint64_t device_now(void *ctx) { (void)ctx; return test_now; }
static int send_command(void *ctx,const struct music_command *c) { (void)ctx; (void)c; return 0; }
static int volume_get(void *ctx,unsigned *v) { (void)ctx; *v=65; return 0; }
static void repaint(struct du_ui *u,void *ctx)
{ (void)u; struct music_app *a=ctx; music_runtime_repaint(a->backend->ctx); }
static void prepare(struct music_player *p)
{
    p->device=p->started=p->first_pcm=1; p->view.rate=48000; p->view.state=MUSIC_PLAYING;
    p->reader_length=p->view.duration=48000*180; p->count=MUSIC_RING;
}
static void contention(struct music_runtime *r,struct device *d)
{
    struct music_player *p=&r->player; prepare(p);
    struct music_app *app=calloc(1,sizeof(*app)); struct du_ui *ui=calloc(1,sizeof(*ui));
    uint32_t *pixels=calloc(560*420,4); assert(app && ui && pixels);
    struct music_ui_backend backend={send_command,volume_get,NULL,NULL,r};
    du_init(ui,pixels,560,420); music_attach(app,ui,&backend); ui->paint=repaint; du_render(ui);
    uint64_t pump_cpu=0; unsigned turns=0, full_repaints=0;
    for (test_now=0;test_now<60000000000ull;test_now+=2000000) {
        if (test_now%20000000<10000000) continue;
        uint64_t begin=cpu(); assert(!music_player_pump(p)); pump_cpu+=cpu()-begin; turns++;
        // Synthetic producer keeps PCM available; the separate calibrated
        // scheduling model exercises reader CPU budgets and batch/yield cost.
        if (p->count<65536) p->count=MUSIC_RING-p->history;
        publish(r,&p->view);
        if (r->snapshot.publication!=app->snapshot.publication) {
            music_update(app,&r->snapshot);
            if (ui->dirty==1) full_repaints++;
            if (ui->dirty) du_render(ui);
        }
    }
    assert(!d->gaps && !p->view.starvations && d->max_request>256);
    assert(p->view.position>48000*59.4 && p->view.position<=48000*60);
    assert(atomic_load(&p->ui_publishes)<=120 && atomic_load(&p->ui_repaints)<=121);
    assert(!full_repaints);
    printf("production pump: wall=60s position=%.6fs gaps=%u starvations=%u writes=%u publishes=%u repaints=%u full_repaints=%u turns=%u pump_cpu_us=%.3f cpu_pct=%.4f\n",
        p->view.position/48000.,d->gaps,p->view.starvations,d->writes,atomic_load(&p->ui_publishes),
        atomic_load(&p->ui_repaints),full_repaints,turns,pump_cpu/1000.,pump_cpu/600000000.);
    du_release(ui); free(pixels); free(ui); free(app);
}
static void backpressure(struct music_runtime *r,struct device *d)
{
    struct music_player *p=&r->player;
    uint64_t before=p->written; unsigned count=p->count;
    test_now+=12000000; d->blocked=1; assert(!music_player_pump(p));
    assert(p->written==before && p->count==count);
    d->blocked=0; d->partial=1; assert(!music_player_pump(p));
    assert(p->written==before+7 && p->count==count-7);
    d->partial=0;
    puts("PASS pump EAGAIN and partial writes retain exact pending frames");
}
static void debounce(struct music_runtime *r,struct device *d)
{
    struct music_player *p=&r->player;
    p->head=p->count=p->history=0; p->written=p->audible=d->consumed;
    d->queued=0; d->last=test_now;
    uint64_t start=test_now;
    assert(!music_player_pump(p) && !p->view.buffering);
    test_now=start+299000000; assert(!music_player_pump(p) && !p->view.buffering);
    // A short recovery resets continuous-starvation detection.
    p->count=256; assert(!music_player_pump(p));
    test_now+=20000000; assert(!music_player_pump(p) && !p->view.buffering);
    test_now+=299000000; assert(!music_player_pump(p) && !p->view.buffering);
    test_now+=1000000; assert(!music_player_pump(p) && p->view.buffering);
    uint64_t last=test_now;
    p->count=MUSIC_RING-p->history; assert(!music_player_pump(p) && p->view.buffering);
    for (unsigned i=0;i<1000;i++) {
        test_now+=2000000; p->count=MUSIC_RING-p->history;
        assert(!music_player_pump(p));
        if (test_now-last<2000000000) assert(p->view.buffering);
    }
    assert(!p->view.buffering);
    puts("PASS continuous 300ms entry, interrupted starvation, 2s stable recovery");
}
static void publication(struct music_runtime *r)
{
    struct music_snapshot *s=&r->player.view;
    unsigned before=r->snapshot.publication, starvations=s->starvations;
    s->state=MUSIC_ERROR; s->error=-EIO; test_now+=100000000; publish(r,s);
    assert(r->snapshot.publication==before+1);
    for (unsigned i=0;i<20;i++) { test_now+=100000000; s->reservoir_frames++; s->starvations++; publish(r,s); }
    assert(r->snapshot.publication==before+1); // Diagnostics are not displayed.
    s->starvations=starvations;
    s->state=MUSIC_PLAYING; s->error=0; test_now+=500000000; publish(r,s);
    before=r->snapshot.publication;
    for (unsigned i=0;i<100;i++) {
        s->buffering=!s->buffering; test_now+=2000000; publish(r,s);
    }
    assert(r->snapshot.publication<=before+4);
    puts("PASS unchanged fields suppressed and state/buffering publications capped at 10Hz");
}
static void t23_holds(void)
{
    /* Trial histogram (no invented timestamps): isolate each refill delay,
     * restoring decoded PCM between holds. Device CPU overruns are covered
     * by the real FS mutex replay and scheduler tests, not this DAC model. */
    const struct { unsigned count,ms; } holds[]={{406,125},{91,350},{10,750},{5,1250},{1,1502},{34,1633},{1,1837},{1,602},{1,441}};
    unsigned cases=0;
    for (unsigned kind=0;kind<sizeof(holds)/sizeof(*holds);kind++)
        for (unsigned h=0;h<holds[kind].count;h++) {
            struct device d={0};
            struct music_output out={device_open,device_write,device_status,device_close,device_now,&d,NULL,NULL};
            struct music_player *p=calloc(1,sizeof(*p)); assert(p);
            test_now=0; music_player_init(p,&out,NULL,NULL); prepare(p);
            /* f1-87 C: the decoded reservoir the reader keeps once it has
             * filled the ring (>= 2 s). f1-101 starts playback after
             * MUSIC_PREFILL_MS (1 s); this case is the steady state. */
            p->count=p->view.rate*2u;
            assert(!music_player_pump(p));
            for (unsigned t=2;t<=holds[kind].ms;t+=2) {
                test_now=(uint64_t)t*1000000; assert(!music_player_pump(p));
            }
            assert(!d.gaps && !p->view.starvations && p->count);
            music_player_destroy(p); free(p); cases++;
        }
    printf("PASS F1-87 T23 hold histogram cases=%u max=1837ms decoded_start=2s DAC_gaps=0 starvations=0\n",cases);
}
/* f1-101: the kernel's producer state in the level record, and the device
 * lead that mirrors a reservoir under its 500 ms target. */
static struct music_producer kernel_producer;
static unsigned producer_calls;
static int producer_state(void *ctx,struct music_producer *out)
{ (void)ctx; producer_calls++; *out=kernel_producer; return 0; }
static void producer_level(void)
{
    struct device d={0};
    struct music_output out={device_open,device_write,device_status,device_close,device_now,&d,NULL,producer_state};
    struct music_player *p=calloc(1,sizeof(*p)); assert(p);
    test_now=0; music_player_init(p,&out,NULL,NULL); prepare(p);
    for (unsigned i=0;i<20;i++) {
        p->count=music_frames_ms(48000,MUSIC_TARGET_MS); test_now+=2000000; assert(!music_player_pump(p));
    }
    assert(d.queued>MUSIC_DEVICE_BEHIND); /* at the target: the device stream is filled */
    p->count=music_frames_ms(48000,MUSIC_TARGET_MS)-1;
    unsigned high=0;
    for (unsigned i=0;i<100;i++) {
        test_now+=2000000; assert(!music_player_pump(p));
        if (i>=40 && d.queued>high) high=d.queued; /* after the device drained to the mark */
        p->count=music_frames_ms(48000,MUSIC_TARGET_MS)-1;
    }
    assert(high<=MUSIC_DEVICE_BEHIND && high+256>MUSIC_DEVICE_BEHIND && !d.gaps);
    /* EOF: the tail is not a deficit, the stream fills */
    p->eof=1; test_now+=2000000; assert(!music_player_pump(p)); assert(d.queued>MUSIC_DEVICE_BEHIND);
    p->eof=0;
    /* the level record carries the kernel's boost state as window deltas */
    FILE *capture=tmpfile(); assert(capture);
    fflush(stderr); int saved=dup(2); assert(saved>=0 && dup2(fileno(capture),2)==2);
    kernel_producer=(struct music_producer){.flags=3,.boosts=4,.over_budget=1,.boost_us=12500};
    memset(&p->level,0,sizeof(p->level)); p->producer_base=(struct music_producer){0};
    for (unsigned i=0;i<2600;i++) { test_now+=2000000; p->count=MUSIC_RING/2; assert(!music_player_pump(p)); }
    kernel_producer=(struct music_producer){.flags=0,.boosts=6,.over_budget=1,.boost_us=20500};
    for (unsigned i=0;i<2600;i++) { test_now+=2000000; p->count=MUSIC_RING/2; assert(!music_player_pump(p)); }
    fflush(stderr); assert(dup2(saved,2)==2); close(saved);
    char text[4096]={0}; rewind(capture); size_t n=fread(text,1,sizeof(text)-1,capture); fclose(capture); text[n]=0;
    assert(strstr(text," boost=3 boosts=4 boost_ms=12 over=1\n"));
    assert(strstr(text," boost=0 boosts=2 boost_ms=8 over=0\n"));
    assert(producer_calls>=2);
    music_player_destroy(p); free(p);
    puts("PASS F1-101 device lead mirrors a reservoir under 500 ms (<=1280 frames), fills at the target and at EOF; level record boost=flags boosts/boost_ms/over deltas");
}
int main(void)
{
    t23_holds();
    producer_level();
    struct device d={0}; struct music_output out={device_open,device_write,device_status,device_close,device_now,&d,NULL,NULL};
    struct music_runtime *r=calloc(1,sizeof(*r)); assert(r);
    assert(!pthread_mutex_init(&r->lock,NULL)); music_player_init(&r->player,&out,NULL,NULL);
    contention(r,&d); backpressure(r,&d); debounce(r,&d); publication(r);
    music_player_destroy(&r->player); pthread_mutex_destroy(&r->lock); free(r); return 0;
}
