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
        if (!free_frames) { pthread_cond_wait(&p->space,&p->reservoir_lock); continue; }
        unsigned end=(p->head+p->count)%MUSIC_RING, n=MUSIC_RING-end;
        if (n>free_frames) n=free_frames;
        if (n>1024) n=1024;
        /* Publish the startup threshold exactly, independently of batch size. */
        unsigned threshold=p->reader_rate/10;
        if (p->count<threshold && n>threshold-p->count) n=threshold-p->count;
        pthread_mutex_unlock(&p->reservoir_lock);
        int got=music_decoder_read(p->decoder,p->ring+2*end,n);
        uint64_t length=music_decoder_length(p->decoder);
        pthread_mutex_lock(&p->reservoir_lock);
        if (atomic_load(&p->reader_stop)) break;
        p->reader_length=length;
        if (got<0) { p->reader_error=got; break; }
        if (!got) { p->eof=1; break; }
        p->count+=(unsigned)got;
        pthread_mutex_unlock(&p->reservoir_lock);
        reader_yield();
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
    p->head=p->count=p->history=0; p->eof=p->reader_error=p->started=0;
    p->written=p->audible=0; p->tail_deadline=p->starve_started=0;
    p->view.buffering=0; p->view.reservoir_frames=0;
}
static void close_device(struct music_player *p)
{
    if (p->device) p->output->close(p->output->ctx);
    p->device=0; p->tail_deadline=0;
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
    p->history-=(unsigned)(consumed-p->audible); p->audible=consumed;
    pthread_cond_signal(&p->space); pthread_mutex_unlock(&p->reservoir_lock);
    p->view.position=p->base+consumed;
    if (p->view.duration && p->view.position>p->view.duration) p->view.position=p->view.duration;
    return (int)queued;
}
static int open_device(struct music_player *p)
{
    if (p->device) return 0;
    unsigned rate=0; int r=p->output->open(p->output->ctx,&rate);
    if (r) return r;
    p->device=1;
    if (!rate || rate>MUSIC_RING/2 || (p->view.rate && p->view.rate!=rate)) { close_device(p); return -EIO; }
    p->view.rate=rate; p->base=p->view.position; p->written=p->audible=0; return 0;
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
        p->view.buffering=0; p->starve_started=0; p->view.state=MUSIC_PAUSED; return 0;
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
        if (p->count<p->view.rate/10 && !p->eof) { pthread_mutex_unlock(&p->reservoir_lock); return 0; }
        p->started=1; p->view.state=MUSIC_PLAYING;
    }
    if (p->count && !p->first_pcm) {
        const char *path=p->view.playlist.path[p->view.current], *ext=strrchr(path,'.');
        diagnostic("[music] decode format=%s rate=%u channels=%u first_pcm_ms=%llu",ext ? ext+1 : "unknown",
            p->source_rate,p->source_channels,
            (unsigned long long)((p->output->now(p->output->ctx)-p->load_started)/1000000));
        p->first_pcm=1;
    }
    int eof=p->eof && !p->count;
    if (!p->count && !p->eof && !queued) {
        uint64_t now=p->output->now(p->output->ctx);
        if (!p->starve_started) { p->starve_started=now+1; p->view.starvations++; }
        p->view.buffering=1;
        if (!p->starve_log_valid || now-p->starve_logged>=1000000000) {
            uint64_t ms=(now-(p->starve_started-1))/1000000;
            if (ms>UINT32_MAX) ms=UINT32_MAX;
            diagnostic("[music] starve ms=%u reservoir_frames=%u",(unsigned)ms,p->count);
            p->starve_logged=now; p->starve_log_valid=1;
        }
        /* The existing kernel stream renders silence when empty. Do not
         * submit synthetic source frames or repeat an already written slot. */
    } else { p->view.buffering=0; p->starve_started=0; }
    if (p->count && queued<256) {
        unsigned n=MUSIC_RING-p->head; if (n>p->count) n=p->count;
        if (n>256u-(unsigned)queued) n=256u-(unsigned)queued;
        p->stage="audio-write";
        int r=p->output->write(p->output->ctx,p->ring+2*p->head,n);
        if ((r<0 && r!=-EAGAIN && r!=-EINTR) || r>(int)n) {
            pthread_mutex_unlock(&p->reservoir_lock); return fail(p,r<0 ? r : -EIO);
        }
        if (r>0) {
            p->head=(p->head+(unsigned)r)%MUSIC_RING; p->count-=(unsigned)r;
            p->history+=(unsigned)r; p->written+=(unsigned)r;
        }
    }
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
