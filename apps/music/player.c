/* SPDX-License-Identifier: MIT */
#include "player.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef __i386__
#include <ciuki/raw.h>
#else
#include <sched.h>
#endif
static void reader_yield(void)
{
#ifdef __i386__
    (void)ciuki_raw_yield(0,0,0,0,0,0);
#else
    sched_yield();
#endif
}
static void diagnostic(const char *fmt, ...)
{
    /* Native debug_write accepts at most 240 bytes per record. */
    int saved=errno; char line[241]; va_list ap; va_start(ap,fmt);
    int n=vsnprintf(line,sizeof(line),fmt,ap); va_end(ap);
    if (n>0) {
        if ((size_t)n>=sizeof(line)) n=(int)sizeof(line)-1;
#ifdef __i386__
        (void)ciuki_raw_debug_write((uint32_t)(uintptr_t)line,(uint32_t)n,0,0,0,0);
#else
        fprintf(stderr,"%s\n",line);
#endif
    }
    errno=saved;
}
static void open_result(struct music_player *p, const char *path, int result)
{
    char shown[161]; size_t n=strlen(path);
    if (n>160) snprintf(shown,sizeof(shown),"%.80s...%s",path,path+n-77);
    else snprintf(shown,sizeof(shown),"%s",path);
    diagnostic("[music] open path=%s result=%d ms=%llu",shown,result,
        (unsigned long long)((p->output->now(p->output->ctx)-p->load_started)/1000000));
    /* Keep the complete path available without losing the result to the
     * diagnostic sink's per-record bound. */
    if (n>160) for (size_t i=0;i<n;i+=200)
        diagnostic("[music] path part=%u value=%.200s",(unsigned)(i/200),path+i);
}
static void loading(struct music_player *p)
{
    p->view.state=MUSIC_LOADING; p->view.error=0; p->view.error_path[0]=0;
    if (p->changed) p->changed(p->ctx,&p->view);
}
static void report_error(struct music_player *p, int error, const char *path)
{
    p->view.error=error;
    snprintf(p->view.error_path,sizeof(p->view.error_path),"%s",path ? path : "");
    diagnostic("[music] error stage=%s errno=%d",p->stage ? p->stage : "playback",-error);
}
/* f4-37: the reader adds the decoder's stage cycles to the open window. */
static void cost_add(struct music_cost *to, const struct music_cost *c)
{
    to->decode+=c->decode; to->imdct+=c->imdct; to->synth+=c->synth; to->pcm+=c->pcm;
    to->adapter+=c->adapter; to->io+=c->io;
    if (c->best && (!to->best || c->best<to->best)) to->best=c->best;
    to->frames+=c->frames; to->out+=c->out; to->tiny+=c->tiny; to->scans+=c->scans;
    to->de+=c->de; to->ue+=c->ue;
}
static int reader_cancel(void *ctx)
{
    struct music_player *p=ctx;
    return atomic_load(&p->reader_stop) || (p->cancel && p->cancel(p->ctx));
}
static void *reader(void *ctx)
{
    struct music_player *p=ctx;
    pthread_mutex_lock(&p->reservoir_lock);
    while (!atomic_load(&p->reader_stop)) {
        unsigned free_frames=MUSIC_RING-p->count-p->history;
        if (free_frames<MUSIC_BATCH) { pthread_cond_wait(&p->space,&p->reservoir_lock); continue; }
        unsigned end=(p->head+p->count)%MUSIC_RING, n=MUSIC_RING-end;
        if (n>free_frames) n=free_frames;
        if (n>MUSIC_BATCH) n=MUSIC_BATCH;
        /* Publish the startup threshold exactly, independently of batch size. */
        unsigned threshold=music_frames_ms(p->reader_rate,MUSIC_PREFILL_MS);
        if (p->count<threshold && n>threshold-p->count) n=threshold-p->count;
        /* Serialize the short demand command with pause/close and reopen;
         * decoding itself still runs without the reservoir lock. */
        if (p->device && p->output->refill) p->output->refill(p->output->ctx);
        pthread_mutex_unlock(&p->reservoir_lock);
        uint64_t begin=p->output->now(p->output->ctx);
        int got=music_decoder_read(p->decoder,p->ring+2*end,n);
        uint64_t elapsed=p->output->now(p->output->ctx)-begin;
        uint64_t length=music_decoder_length(p->decoder);
        struct music_cost cost; music_decoder_cost(p->decoder,&cost);
        pthread_mutex_lock(&p->reservoir_lock);
        if (atomic_load(&p->reader_stop)) break;
        cost_add(&p->level.cost,&cost);
        p->reader_length=length;
        if (got<0) { p->reader_error=got; break; }
        if (!got) { p->eof=1; break; }
        p->count+=(unsigned)got;
        p->level.frames+=(unsigned)got; p->level.decode_ns+=elapsed;
        int yield=p->count>MUSIC_RING/2;
        pthread_mutex_unlock(&p->reservoir_lock);
        if (yield) reader_yield();
        pthread_mutex_lock(&p->reservoir_lock);
    }
    pthread_mutex_unlock(&p->reservoir_lock); return NULL;
}
static void stop_reader(struct music_player *p)
{
    if (!p->reader_live) return;
    atomic_store(&p->reader_stop,1);
    pthread_mutex_lock(&p->reservoir_lock); pthread_cond_signal(&p->space);
    pthread_mutex_unlock(&p->reservoir_lock);
    pthread_join(p->reader,NULL); p->reader_live=0;
}
static int start_reader(struct music_player *p)
{
    if (p->sync_error) return -p->sync_error;
    atomic_store(&p->reader_stop,0); p->reader_rate=p->view.rate;
    int r=pthread_create(&p->reader,NULL,reader,p);
    if (!r) p->reader_live=1;
    return -r;
}
static void flush(struct music_player *p)
{
    /* Only after joining: a cancelled read may still own unpublished slots. */
    p->head=p->count=p->history=0; p->eof=p->reader_error=p->started=p->submitted=0;
    p->written=p->audible=0; p->tail_deadline=p->starve_started=p->last_starve=0;
    p->view.buffering=0; p->view.reservoir_frames=0;
    memset(&p->level,0,sizeof(p->level));
}
static void close_device(struct music_player *p)
{
    if (!p->device) { p->tail_deadline=0; return; }
    pthread_mutex_lock(&p->reservoir_lock);
    p->output->close(p->output->ctx);
    p->device=0; p->tail_deadline=0;
    pthread_mutex_unlock(&p->reservoir_lock);
}
static int fail(struct music_player *p, int error)
{
    report_error(p,error,p->view.playlist.count ? p->view.playlist.path[p->view.current] : NULL);
    stop_reader(p); close_device(p); flush(p); music_decoder_close(p->decoder); p->decoder=NULL; p->view.state=MUSIC_ERROR; return error;
}
static int position(struct music_player *p, uint64_t *tail)
{
    if (!p->device) { *tail=0; return 0; }
    uint64_t consumed; unsigned queued;
    int r=p->output->status(p->output->ctx,&consumed,&queued,tail); if (r) return r;
    /* status already subtracts the device latency (native adapter). */
    if (consumed>p->written) consumed=p->written;
    if (consumed<p->audible) consumed=p->audible;
    pthread_mutex_lock(&p->reservoir_lock);
    unsigned free_before=MUSIC_RING-p->count-p->history;
    p->history-=(unsigned)(consumed-p->audible); p->audible=consumed;
    /* Submitted history is still owned until audible, including DMA latency.
     * Only its retirement makes decode slots free. Avoid a wake per 2 ms turn. */
    if (free_before<MUSIC_BATCH && MUSIC_RING-p->count-p->history>=MUSIC_BATCH)
        pthread_cond_signal(&p->space);
    pthread_mutex_unlock(&p->reservoir_lock);
    p->view.position=p->base+consumed;
    if (p->view.duration && p->view.position>p->view.duration) p->view.position=p->view.duration;
    return (int)queued;
}
static int open_device(struct music_player *p)
{
    if (p->device) return 0;
    pthread_mutex_lock(&p->reservoir_lock);
    unsigned rate=0; int r=p->output->open(p->output->ctx,&rate);
    if (!r) p->device=1;
    pthread_mutex_unlock(&p->reservoir_lock);
    if (r) return r;
    if (!rate || rate>MUSIC_RING/2 || (p->view.rate && p->view.rate!=rate)) { close_device(p); return -EIO; }
    p->view.rate=rate; p->base=p->view.position; p->written=p->audible=0;
    memset(&p->producer_base,0,sizeof(p->producer_base)); /* a new stream counts from zero */
    return 0;
}
static int load(struct music_player *p)
{
    stop_reader(p); close_device(p); flush(p); music_decoder_close(p->decoder); p->decoder=NULL;
    p->view.position=p->view.duration=0; loading(p);
    if (!p->view.playlist.count) { p->view.state=MUSIC_STOPPED; return 0; }
    p->load_started=p->output->now(p->output->ctx); p->first_pcm=0; p->stage="audio-open";
    int r=open_device(p);
    const char *path=p->view.playlist.path[p->view.current];
    if (r) { open_result(p,path,r); return fail(p,r); }
    p->stage="open"; atomic_store(&p->reader_stop,0);
    r=music_decoder_open(&p->decoder,p->view.playlist.path[p->view.current],p->view.rate,reader_cancel,p);
    open_result(p,path,r);
    if (r) return fail(p,r);
    p->reader_length=p->view.duration=music_decoder_length(p->decoder);
    p->source_rate=music_decoder_rate(p->decoder); p->source_channels=music_decoder_channels(p->decoder);
    r=start_reader(p); if (r) return fail(p,r);
    return 0;
}
void music_player_init(struct music_player *p, const struct music_output *output, music_cancel cancel, void *ctx)
{
    memset(p,0,sizeof(*p)); atomic_init(&p->reader_stop,0);
    atomic_init(&p->ui_publishes,0); atomic_init(&p->ui_repaints,0); atomic_init(&p->ui_kcycles,0);
    p->output=output; p->cancel=cancel; p->ctx=ctx;
    p->sync_error=pthread_mutex_init(&p->reservoir_lock,NULL);
    if (!p->sync_error) {
        p->sync_error=pthread_cond_init(&p->space,NULL);
        if (p->sync_error) pthread_mutex_destroy(&p->reservoir_lock);
    }
}
int music_player_command(struct music_player *p, const struct music_command *c)
{
    int r=0;
    if (p->sync_error) return -p->sync_error;
    switch (c->action) {
    case MUSIC_OPEN:
        stop_reader(p); close_device(p); flush(p); music_decoder_close(p->decoder); p->decoder=NULL;
        p->view.playlist.count=0; p->view.current=0; p->view.position=p->view.duration=0;
        p->view.revision++;
        loading(p); p->stage="playlist-open";
        p->load_started=p->output->now(p->output->ctx);
        r=music_playlist_add(&p->view.playlist,c->path,p->cancel,p->ctx);
        if (r) {
            open_result(p,c->path,r);
            report_error(p,r,c->path); close_device(p); p->view.state=MUSIC_ERROR; return r;
        }
        return load(p);
    case MUSIC_ADD:
        p->stage="playlist-add";
        r=music_playlist_add(&p->view.playlist,c->path,p->cancel,p->ctx);
        if (!r) p->view.revision++;
        if (r) report_error(p,r,c->path); else { p->view.error=0; p->view.error_path[0]=0; } return r;
    case MUSIC_SAVE:
        p->stage="playlist-save"; r=music_playlist_save(&p->view.playlist,c->path,p->cancel,p->ctx);
        if (r) report_error(p,r,c->path); else { p->view.error=0; p->view.error_path[0]=0; } return r;
    case MUSIC_CLEAR:
        stop_reader(p); close_device(p); flush(p); music_decoder_close(p->decoder); p->decoder=NULL;
        p->view.playlist.count=0; p->view.current=0; p->view.position=p->view.duration=0;
        p->view.revision++; p->view.state=MUSIC_STOPPED; p->view.error=0; p->view.error_path[0]=0; return 0;
    case MUSIC_SELECT:
        if (c->value>=p->view.playlist.count) return -EINVAL;
        p->view.current=(unsigned)c->value; return load(p);
    case MUSIC_PREV: case MUSIC_NEXT:
        if (!p->view.playlist.count) return 0;
        p->view.current=(p->view.current+p->view.playlist.count+(c->action==MUSIC_NEXT ? 1 : -1))%p->view.playlist.count;
        return load(p);
    case MUSIC_PLAY:
        if (p->view.state==MUSIC_PLAYING || p->view.state==MUSIC_LOADING) return 0;
        if (!p->decoder || (p->view.duration && p->view.position>=p->view.duration)) return load(p);
        p->stage="audio-open";
        r=open_device(p); if (r) return fail(p,r);
        if (!p->reader_live && (r=start_reader(p))) return fail(p,r);
        pthread_mutex_lock(&p->reservoir_lock);
        memset(&p->level,0,sizeof(p->level));
        pthread_mutex_unlock(&p->reservoir_lock);
        p->view.state=p->started ? MUSIC_PLAYING : MUSIC_LOADING; p->view.error=0; return 0;
    case MUSIC_PAUSE:
        if (p->view.state!=MUSIC_PLAYING && p->view.state!=MUSIC_LOADING) return 0;
        p->stage="audio-status";
        { uint64_t tail; r=position(p,&tail); }
        if (r<0) return fail(p,r);
        close_device(p);
        pthread_mutex_lock(&p->reservoir_lock);
        /* Closing drops queued/DMA PCM. Replay that exact retained suffix. */
        p->head=(p->head+MUSIC_RING-p->history)%MUSIC_RING;
        p->count+=p->history; p->history=0; p->written=p->audible=0;
        p->view.reservoir_frames=p->count;
        pthread_mutex_unlock(&p->reservoir_lock);
        p->view.buffering=0; p->starve_started=p->last_starve=0; p->view.state=MUSIC_PAUSED; return 0;
    case MUSIC_STOP:
        stop_reader(p); close_device(p); flush(p); p->view.position=0; p->view.error=0;
        p->stage="seek"; atomic_store(&p->reader_stop,0);
        if (p->decoder && (r=music_decoder_seek(p->decoder,0))) return fail(p,r);
        p->view.state=MUSIC_STOPPED; return 0;
    case MUSIC_SEEK: {
        if (!p->decoder) return 0;
        enum music_state state=p->view.state;
        stop_reader(p); close_device(p); flush(p); p->stage="seek";
        p->view.position=p->view.duration && c->value>p->view.duration ? p->view.duration : c->value;
        atomic_store(&p->reader_stop,0);
        r=music_decoder_seek(p->decoder,p->view.position); if (r) return fail(p,r);
        p->reader_length=p->view.duration=music_decoder_length(p->decoder);
        if (p->view.duration && p->view.position>p->view.duration) p->view.position=p->view.duration;
        if ((r=start_reader(p))) return fail(p,r);
        if (state==MUSIC_PLAYING || state==MUSIC_LOADING) {
            p->stage="audio-open"; if ((r=open_device(p))) return fail(p,r);
            loading(p);
        }
        return 0;
    }
    }
    return -EINVAL;
}
/* f4-37: two fixed workloads per cost record, the best of three runs in TSC
 * cycles. loop: 1024 iterations of a dependent add and a counted branch;
 * on P6 the TSC counts every core clock (Intel SDM Vol. 3B, "Time-Stamp
 * Counter") and keeps counting while throttling stops the clock, so a slowed
 * or throttled core shows here. mem: a second pass over 4 KiB of the
 * player's own memory, about one load per clock when it is cacheable. */
static uint32_t calibration_buffer[1024];
static void calibrate(unsigned *loop, unsigned *mem)
{
    uint64_t best_loop=UINT64_MAX, best_mem=UINT64_MAX;
    for (unsigned run=0;run<3;run++) {
        uint64_t begin=music_cycles();
#if defined(__i386__) || defined(__x86_64__)
        unsigned x=0, n=1024;
        __asm__ volatile("1:\n\taddl $1, %0\n\tdecl %1\n\tjnz 1b" : "+r"(x), "+r"(n));
#endif
        uint64_t middle=music_cycles();
        volatile uint32_t *b=calibration_buffer; uint32_t sum=0;
        for (unsigned i=0;i<1024;i++) sum+=b[i];
        uint64_t warm=music_cycles();
        for (unsigned i=0;i<1024;i++) sum+=b[i];
        uint64_t end=music_cycles(); b[0]=sum&1u;
        if (middle-begin<best_loop) best_loop=middle-begin;
        if (end-warm<best_mem) best_mem=end-warm;
    }
    *loop=(unsigned)(best_loop<UINT32_MAX ? best_loop : UINT32_MAX);
    *mem=(unsigned)(best_mem<UINT32_MAX ? best_mem : UINT32_MAX);
}
/* Called with reservoir_lock held. Diagnostics use the protected raw debug
 * sink on CiukiOS, independently of stdio and decoder/VFS locks. */
static void level(struct music_player *p, uint64_t now, unsigned queued)
{
    if (!p->level.start) {
        p->level.start=now+1;
        p->level.low=p->level.high=p->count;
        p->level.queued_low=p->level.queued_high=queued;
        p->level.publishes=atomic_load(&p->ui_publishes);
        p->level.repaints=atomic_load(&p->ui_repaints);
        p->level.tsc=music_cycles(); p->level.ui_kcycles=atomic_load(&p->ui_kcycles);
    }
    if (p->count<p->level.low) p->level.low=p->count;
    if (p->count>p->level.high) p->level.high=p->count;
    if (queued<p->level.queued_low) p->level.queued_low=queued;
    if (queued>p->level.queued_high) p->level.queued_high=queued;
    p->level.sum+=p->count; p->level.samples++;
    uint64_t elapsed=now-(p->level.start-1);
    if (elapsed<5000000000ull) return;
    uint64_t ms=elapsed/1000000;
    uint64_t decode=p->level.frames ? p->level.decode_ns/p->level.frames : 0;
    unsigned publishes=atomic_load(&p->ui_publishes), repaints=atomic_load(&p->ui_repaints);
    /* f1-101: the kernel's producer boost of this stream over the window:
     * current flags (1 behind, 2 boosted, 4 withheld by the budget), boost
     * episodes, CPU at the boost level and budget withholdings. */
    struct music_producer boost=p->producer_base;
    if (!p->device || !p->output->producer || p->output->producer(p->output->ctx,&boost)) {
        boost=p->producer_base; boost.flags=0;
    }
    if (boost.boosts<p->producer_base.boosts || boost.over_budget<p->producer_base.over_budget ||
        boost.boost_us<p->producer_base.boost_us) memset(&p->producer_base,0,sizeof(p->producer_base));
    diagnostic("[music] level ms=%llu reservoir=%u/%u/%u reader_fps=%u decode_us_frame=%u.%03u writes_ps=%u bytes=%llu queued=%u/%u starvations=%u ui_mps=%u repaint_mps=%u boost=%u boosts=%u boost_ms=%u over=%u",
        (unsigned long long)ms,p->level.low,(unsigned)(p->level.sum/p->level.samples),p->level.high,
        (unsigned)(p->level.frames*1000/ms),(unsigned)(decode/1000),(unsigned)(decode%1000),
        (unsigned)(p->level.writes*1000/ms),(unsigned long long)p->level.bytes,
        p->level.queued_low,p->level.queued_high,p->level.starvations,
        (unsigned)((uint64_t)(publishes-p->level.publishes)*1000000/ms),
        (unsigned)((uint64_t)(repaints-p->level.repaints)*1000000/ms),
        boost.flags,boost.boosts-p->producer_base.boosts,
        (unsigned)((boost.boost_us-p->producer_base.boost_us)/1000),boost.over_budget-p->producer_base.over_budget);
    p->producer_base=boost;
    /* f4-37: where the time went in this window, one record (240 bytes). The
     * stages are TSC cycles (decoder.h) converted with the window's own
     * TSC rate; mhz is that rate, so a clock below the CPU's rating shows. */
    uint64_t tsc=music_cycles(), mhz=(tsc-p->level.tsc)*1000/elapsed;
    unsigned ui=atomic_load(&p->ui_kcycles), ui_k=ui-p->level.ui_kcycles;
    const struct music_cost *c=&p->level.cost;
#define MUSIC_US(cycles) (unsigned)(mhz ? (cycles)/mhz : 0)
    unsigned loop, mem; calibrate(&loop,&mem);
    diagnostic("[music] cost decode_us=%u synth_us=%u imdct_us=%u resample_us=%u ui_us=%u repaints=%u frames=%u tiny_values=%u "
               "pcm_us=%u io_us=%u pump_us=%u out=%u mhz=%u de=%u ue=%u best_kc=%u loop=%u mem=%u",
        MUSIC_US(c->decode),MUSIC_US(c->synth),MUSIC_US(c->imdct),MUSIC_US(c->adapter),MUSIC_US((uint64_t)ui_k*1024u),
        repaints-p->level.repaints,c->frames,c->tiny,MUSIC_US(c->pcm),MUSIC_US(c->io),MUSIC_US(p->pump_cycles),
        c->out,(unsigned)mhz,c->de,c->ue,(unsigned)(c->best/1000),loop,mem);
#undef MUSIC_US
    p->pump_cycles=0;
    memset(&p->level,0,sizeof(p->level));
    p->level.start=now+1; p->level.low=MUSIC_RING;
    p->level.queued_low=MUSIC_DEVICE_FRAMES;
    p->level.publishes=publishes; p->level.repaints=repaints;
    p->level.tsc=tsc; p->level.ui_kcycles=ui;
}
int music_player_pump(struct music_player *p)
{
    if (p->view.state!=MUSIC_PLAYING && p->view.state!=MUSIC_LOADING) return 0;
    p->stage="audio-status";
    uint64_t tail; int queued=position(p,&tail);
    if (queued<0) return fail(p,queued);
    pthread_mutex_lock(&p->reservoir_lock);
    int error=p->reader_error;
    p->view.duration=p->reader_length; p->view.reservoir_frames=p->count;
    if (error) { pthread_mutex_unlock(&p->reservoir_lock); p->stage="decode"; return fail(p,error); }
    if (!p->started) {
        if (p->count<music_frames_ms(p->view.rate,MUSIC_PREFILL_MS) && !p->eof) { pthread_mutex_unlock(&p->reservoir_lock); return 0; }
        p->started=1; p->view.state=MUSIC_PLAYING;
        memset(&p->level,0,sizeof(p->level));
    }
    if (p->count && !p->first_pcm) {
        const char *path=p->view.playlist.path[p->view.current], *ext=strrchr(path,'.');
        diagnostic("[music] decode format=%s rate=%u channels=%u first_pcm_ms=%llu",ext ? ext+1 : "unknown",
            p->source_rate,p->source_channels,
            (unsigned long long)((p->output->now(p->output->ctx)-p->load_started)/1000000));
        p->first_pcm=1;
    }
    uint64_t now=p->output->now(p->output->ctx);
    int eof=p->eof && !p->count;
    if (!p->count && !p->eof && !queued) {
        if (!p->starve_started) { p->starve_started=now+1; p->view.starvations++; p->level.starvations++; }
        p->last_starve=now+1;
        if (now-(p->starve_started-1)>=300000000) p->view.buffering=1;
        if (!p->starve_log_valid || now-p->starve_logged>=1000000000) {
            uint64_t ms=(now-(p->starve_started-1))/1000000;
            if (ms>UINT32_MAX) ms=UINT32_MAX;
            diagnostic("[music] starve ms=%u reservoir_frames=%u",(unsigned)ms,p->count);
            p->starve_logged=now; p->starve_log_valid=1;
        }
        /* The existing kernel stream renders silence when empty. Do not
         * submit synthetic source frames or repeat an already written slot. */
    } else {
        p->starve_started=0;
        if (p->last_starve && now-(p->last_starve-1)>=2000000000) {
            p->view.buffering=0; p->last_starve=0;
        }
    }
    level(p,now,(unsigned)queued);
    /* f1-101: under its 500 ms target the reservoir shows the kernel a
     * stream behind its lead (music_device_fill); the mixer's real lead,
     * not a flag, then raises the decoder and this pump. */
    unsigned fill=music_device_fill(p->count,music_frames_ms(p->view.rate,MUSIC_TARGET_MS),p->eof);
    unsigned limit=p->submitted ? fill : 256;
    if (p->count && (unsigned)queued<(p->submitted ? fill-256 : 256)) {
        unsigned n=MUSIC_RING-p->head; if (n>p->count) n=p->count;
        if (n>limit-(unsigned)queued) n=limit-(unsigned)queued;
        p->stage="audio-write";
        int r=p->output->write(p->output->ctx,p->ring+2*p->head,n);
        if ((r<0 && r!=-EAGAIN && r!=-EINTR) || r>(int)n) {
            pthread_mutex_unlock(&p->reservoir_lock); return fail(p,r<0 ? r : -EIO);
        }
        if (r>0) {
            p->head=(p->head+(unsigned)r)%MUSIC_RING; p->count-=(unsigned)r;
            p->history+=(unsigned)r; p->written+=(unsigned)r;
            p->submitted=1; p->level.writes++; p->level.bytes+=(unsigned)r*4;
            if ((unsigned)queued+(unsigned)r>p->level.queued_high) p->level.queued_high=(unsigned)queued+(unsigned)r;
        }
    }
    p->view.reservoir_frames=p->count;
    pthread_mutex_unlock(&p->reservoir_lock);
    if (eof && !queued) {
        uint64_t now=p->output->now(p->output->ctx);
        if (!p->tail_deadline) p->tail_deadline=now+tail+1;
        if (now>=p->tail_deadline) {
            p->view.position=p->base+p->written;
            stop_reader(p); close_device(p); flush(p);
            if (p->view.current+1<p->view.playlist.count) { p->view.current++; return load(p); }
            p->view.state=MUSIC_STOPPED;
        }
    }
    return 0;
}
void music_player_destroy(struct music_player *p)
{
    stop_reader(p); close_device(p); flush(p); music_decoder_close(p->decoder); p->decoder=NULL;
    diagnostic("[music] close starvations=%u",p->view.starvations);
    if (!p->sync_error) { pthread_cond_destroy(&p->space); pthread_mutex_destroy(&p->reservoir_lock); }
    p->view.state=MUSIC_CLOSING;
}
