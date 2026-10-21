/* SPDX-License-Identifier: MIT
 * Real codecs, a blocking seekable VFS, and an independently clocked device.
 * -DMUSIC_LEGACY builds this same harness against the pre-F4-34 player. */
#define _GNU_SOURCE
#include "player.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
struct volume { int fd; uint64_t next_stall; };
static atomic_uint stalls, bulk_reads;
static atomic_int stall_enabled, force_stall, entered;
static uint64_t ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static void ms(unsigned n)
{ struct timespec t={n/1000,(long)(n%1000)*1000000}; nanosleep(&t,NULL); }
static ssize_t volume_read(void *ctx,char *buf,size_t n)
{
    struct volume *v=ctx;
    if (n==256*1024) atomic_fetch_add(&bulk_reads,1);
    if (atomic_load(&stall_enabled) && ns()>=v->next_stall) {
        atomic_fetch_add(&stalls,1); ms(1502); v->next_stall=ns()+2000000000;
    }
    if (atomic_load(&force_stall)) { atomic_store(&entered,1); while (atomic_load(&force_stall)) ms(1); }
    return read(v->fd,buf,n);
}
static int volume_seek(void *ctx,off64_t *at,int whence)
{ struct volume *v=ctx; off_t r=lseek(v->fd,*at,whence); if (r<0) return -1; *at=r; return 0; }
static int volume_close(void *ctx)
{ struct volume *v=ctx; int r=close(v->fd); free(v); return r; }
FILE *__real_fopen(const char *,const char *);
FILE *__wrap_fopen(const char *path,const char *mode)
{
    if (!strstr(path,"/stall.")) return __real_fopen(path,mode);
    struct volume *v=calloc(1,sizeof(*v)); assert(v); v->fd=open(path,O_RDONLY); assert(v->fd>=0);
    v->next_stall=ns()+1000000000;
    FILE *f=fopencookie(v,mode,(cookie_io_functions_t){volume_read,NULL,volume_seek,volume_close}); assert(f); return f;
}
struct device {
    uint64_t written, audible, last, credit, started, expected;
    uint64_t hash, expected_hash; unsigned gaps, max_queue; int gap;
};
static int output_open(void *ctx,unsigned *rate)
{ struct device *d=ctx; d->written=d->audible=d->last=d->credit=d->started=0; *rate=48000; return 0; }
static int output_write(void *ctx,const int16_t *pcm,unsigned n)
{
    struct device *d=ctx; assert(n<=2048); if (!d->written) assert(n<=256);
    if (!d->started) { d->started=ns(); d->last=d->started+43000000; }
    for (unsigned i=0;i<n*2;i++) { d->hash^=(uint16_t)pcm[i]; d->hash*=1099511628211ull; }
    d->written+=n; return (int)n;
}
static int output_status(void *ctx,uint64_t *consumed,unsigned *queued,uint64_t *tail)
{
    struct device *d=ctx; uint64_t now=ns();
    if (d->started && now>d->last) {
        uint64_t credit=(now-d->last)*48000+d->credit;
        uint64_t want=credit/1000000000; d->credit=credit%1000000000; d->last=now;
        uint64_t available=d->written-d->audible;
        if (want>available && d->audible+available<d->expected) {
            if (!d->gap) d->gaps++;
            d->gap=1;
        } else if (available) d->gap=0;
        d->audible+=want<available ? want : available;
    }
    uint64_t pending=d->written-d->audible;
    *consumed=d->audible; *queued=pending>2048 ? (unsigned)(pending-2048) : 0;
    assert(*queued<=2048); if (*queued>d->max_queue) d->max_queue=*queued;
    *tail=43000000; return 0;
}
static void output_close(void *ctx) { (void)ctx; }
static uint64_t output_now(void *ctx) { (void)ctx; return ns(); }
static void reference(const char *path,struct device *d)
{
    atomic_store(&stall_enabled,0);
    struct music_decoder *decoder=NULL; assert(!music_decoder_open(&decoder,path,48000,NULL,NULL));
    d->expected_hash=d->hash=1469598103934665603ull;
    int16_t pcm[2048]; int n;
    while ((n=music_decoder_read(decoder,pcm,1024))>0) {
        d->expected+=(unsigned)n;
        for (int i=0;i<n*2;i++) { d->expected_hash^=(uint16_t)pcm[i]; d->expected_hash*=1099511628211ull; }
    }
    assert(!n && d->expected>=480000); music_decoder_close(decoder);
}
static void play(const char *path)
{
    struct device d={0}; reference(path,&d); atomic_store(&stalls,0); atomic_store(&bulk_reads,0);
    atomic_store(&stall_enabled,1);
    struct music_output out={output_open,output_write,output_status,output_close,output_now,&d};
    struct music_runtime *rt=music_runtime_start(&out); assert(rt);
    struct music_command c={.action=MUSIC_OPEN}; snprintf(c.path,sizeof(c.path),"%s",path); assert(!music_runtime_send(rt,&c));
    struct music_snapshot *s=calloc(1,sizeof(*s)); assert(s); uint64_t previous=0, start=ns(); int playing=0;
    unsigned max_reservoir=0;
    while (ns()-start<20000000000ull) {
        music_runtime_snapshot(rt,s); assert(s->state!=MUSIC_ERROR);
        assert(s->position>=previous); previous=s->position;
#ifndef MUSIC_LEGACY
        if (s->reservoir_frames>max_reservoir) max_reservoir=s->reservoir_frames;
#endif
        if (s->state==MUSIC_PLAYING) playing=1;
        if (playing && s->state==MUSIC_STOPPED) break;
        ms(1);
    }
    assert(playing && s->state==MUSIC_STOPPED && s->position==d.expected && s->duration==d.expected);
    music_runtime_destroy(rt);
    assert(d.written==d.expected && d.hash==d.expected_hash && atomic_load(&stalls)>0);
#ifdef MUSIC_LEGACY
    assert(d.gaps>0);
#else
    assert(!d.gaps && !s->starvations && atomic_load(&bulk_reads)>0);
    assert(max_reservoir>=96000 && max_reservoir<=MUSIC_RING);
#endif
    printf("%s gaps=%u stalls=%u bulk_reads=%u reservoir_max=%u written=%llu EOF=%llu PCM=exact queue_max=%u\n",
        path,d.gaps,atomic_load(&stalls),atomic_load(&bulk_reads),max_reservoir,
        (unsigned long long)d.written,(unsigned long long)s->position,d.max_queue);
    free(s);
}
#ifndef MUSIC_LEGACY
static void starvation(const char *path)
{
    /* Deliberately exceed the reservoir; writer/status/UI must keep moving,
     * then resume with the exact original PCM, with no stale-slot replay. */
    struct device d={0}; reference(path,&d); atomic_store(&stall_enabled,0);
    struct music_output out={output_open,output_write,output_status,output_close,output_now,&d};
    struct music_runtime *rt=music_runtime_start(&out); assert(rt);
    struct music_command c={.action=MUSIC_OPEN}; snprintf(c.path,sizeof(c.path),"%s",path); assert(!music_runtime_send(rt,&c));
    struct music_snapshot *s=calloc(1,sizeof(*s)); assert(s); uint64_t start=ns();
    while (ns()-start<3000000000ull) {
        music_runtime_snapshot(rt,s); if (s->reservoir_frames>=96000) break; ms(1);
    }
    assert(s->reservoir_frames>=96000); atomic_store(&force_stall,1);
    start=ns();
    while (!s->buffering && ns()-start<8000000000ull) { music_runtime_snapshot(rt,s); ms(1); }
    assert(atomic_load(&entered) && s->buffering && s->starvations==1);
    uint64_t at=s->position; ms(1100); music_runtime_snapshot(rt,s);
    assert(s->buffering && s->position>=at && s->starvations==1);
    atomic_store(&force_stall,0); start=ns();
    while (s->state!=MUSIC_STOPPED && ns()-start<15000000000ull) { music_runtime_snapshot(rt,s); ms(1); }
    assert(s->state==MUSIC_STOPPED && s->position==d.expected && !s->buffering && s->starvations==1);
    music_runtime_destroy(rt); assert(d.written==d.expected && d.hash==d.expected_hash);
    puts("PASS visible starvation, bounded records, exact PCM recovery and EOF"); free(s);
}
#endif
int main(int argc,char **argv)
{
    assert(argc==4 || argc==5);
    for (int i=1;i<4;i++) play(argv[i]);
#ifndef MUSIC_LEGACY
    if (argc==5) starvation(argv[1]);
#endif
    return 0;
}
