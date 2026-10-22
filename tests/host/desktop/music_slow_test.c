/* SPDX-License-Identifier: MIT
 * Real codecs over a seekable, latency-limited removable FILE stream. */
#define _GNU_SOURCE
#include "../../../apps/music/player.h"
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
struct volume { int fd, fail; };
static pthread_t ui_thread;
static atomic_uint reads, handles, writes;
static atomic_ullong first_write;
static uint64_t clock_ns(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000+t.tv_nsec; }
static ssize_t volume_read(void *arg, char *buf, size_t bytes)
{
    struct volume *v=arg;
    assert(!pthread_equal(pthread_self(),ui_thread));
    struct timespec delay={0,25000000}; nanosleep(&delay,NULL); atomic_fetch_add(&reads,1);
    /* A late error follows actual playback, including the two-second
     * startup reservoir. The separate fail==1 case covers priming errors. */
    if (v->fail==1 || (v->fail==2 && atomic_load(&reads)>3 && atomic_load(&writes))) { errno=EIO; return -1; }
    if (bytes>8192) bytes=8192;
    return read(v->fd,buf,bytes);
}
static int volume_seek(void *arg, off64_t *offset, int whence)
{
    struct volume *v=arg; off_t n=lseek(v->fd,(off_t)*offset,whence);
    if (n<0) return -1;
    *offset=n; return 0;
}
static int volume_close(void *arg)
{ struct volume *v=arg; int r=close(v->fd); free(v); atomic_fetch_sub(&handles,1); return r; }
FILE *__real_fopen(const char *,const char *);
FILE *__wrap_fopen(const char *path,const char *mode)
{
    if (!strstr(path,"/mnt/e/")) return __real_fopen(path,mode);
    assert(!pthread_equal(pthread_self(),ui_thread));
    if (strstr(path,"denied")) { errno=EACCES; return NULL; }
    struct volume *v=calloc(1,sizeof(*v)); assert(v);
    v->fd=open(path,O_RDONLY); if (v->fd<0) { free(v); return NULL; }
    v->fail=strstr(path,"read-error") ? 1 : strstr(path,"late-error") ? 2 : 0;
    FILE *f=fopencookie(v,mode,(cookie_io_functions_t){.read=volume_read,.seek=volume_seek,.close=volume_close});
    assert(f); atomic_fetch_add(&handles,1); return f;
}
static int sink_open(void *ctx,unsigned *rate) { (void)ctx; *rate=48000; return 0; }
static int sink_write(void *ctx,const int16_t *pcm,unsigned n)
{
    (void)ctx; assert(pcm && n);
    if (!atomic_fetch_add(&writes,1)) atomic_store(&first_write,clock_ns());
    return (int)n;
}
static int sink_status(void *ctx,uint64_t *consumed,unsigned *queued,uint64_t *tail)
/* This sink consumes writes immediately. A permanent 1024-frame source
 * queue would correctly block the player's new one-period prefill policy. */
{ (void)ctx; *consumed=0; *queued=0; *tail=0; return 0; }
static void sink_close(void *ctx) { (void)ctx; }
static uint64_t sink_now(void *ctx) { (void)ctx; return clock_ns(); }
static void pause_ms(unsigned ms) { struct timespec t={ms/1000,(long)(ms%1000)*1000000}; nanosleep(&t,NULL); }
static void close_worker(struct music_runtime *rt)
{
    uint64_t start=clock_ns(); music_runtime_close(rt);
    while (!music_runtime_done(rt) && clock_ns()-start<500000000) pause_ms(1);
    assert(music_runtime_done(rt)); music_runtime_destroy(rt); assert(!atomic_load(&handles));
}
static void run(const char *file, int expected)
{
    atomic_store(&reads,0); atomic_store(&writes,0); atomic_store(&first_write,0);
    struct music_output out={sink_open,sink_write,sink_status,sink_close,sink_now,NULL,NULL,NULL};
    struct music_runtime *rt=music_runtime_start(&out); assert(rt);
    struct music_command c={.action=MUSIC_OPEN}; snprintf(c.path,sizeof(c.path),"%s",file);
    struct music_snapshot *s=calloc(1,sizeof(*s)); assert(s);
    uint64_t start=clock_ns(); assert(!music_runtime_send(rt,&c));
    assert(clock_ns()-start<50000000);
    int loading=0; unsigned snapshots=0;
    while (clock_ns()-start<2000000000) {
        uint64_t t=clock_ns(); music_runtime_snapshot(rt,s); assert(clock_ns()-t<50000000); snapshots++;
        loading|=s->state==MUSIC_LOADING;
        if (s->state==MUSIC_ERROR || (!expected && atomic_load(&writes))) break;
        pause_ms(1);
    }
    if (!expected) assert(loading && snapshots>1);
    if (expected) {
        assert(s->state==MUSIC_ERROR && s->error==expected && !strcmp(s->error_path,file));
        if (strstr(file,"late-error")) assert(atomic_load(&writes)); else assert(!atomic_load(&writes));
    } else {
        assert(atomic_load(&writes) && atomic_load(&first_write)-start<2000000000);
        assert(atomic_load(&reads)>0);
        printf("slow-volume %s first_play_ms=%llu reads=%u snapshots=%u\n",strrchr(file,'/')+1,
            (unsigned long long)((atomic_load(&first_write)-start)/1000000),atomic_load(&reads),snapshots);
    }
    close_worker(rt); free(s);
}
int main(int argc,char **argv)
{
    assert(argc==2); ui_thread=pthread_self();
    const char *names[]={"A long name with spaces - original synthetic tone.mp3","tone.wav","tone.ogg","denied.mp3","read-error.mp3","invalid.mp3","cover-art.mp3","late-error.mp3","missing.mp3","read-error.wav","read-error.ogg"};
    for (unsigned i=0;i<sizeof(names)/sizeof(*names);i++) {
        char file[MUSIC_PATH]; snprintf(file,sizeof(file),"%s/mnt/e/%s",argv[1],names[i]);
        run(file,i==3 ? -EACCES : i==4 || i==7 || i>=9 ? -EIO : i==5 ? -EINVAL : i==8 ? -ENOENT : 0);
    }
    char nested[MUSIC_PATH], component[161]; memset(component,'x',160); component[160]=0;
    snprintf(nested,sizeof(nested),"%s/mnt/e/Long removable folder %s/Original tone.mp3",argv[1],component);
    run(nested,0);
    /* Closing during a volume read remains asynchronous and cancellable. */
    struct music_output out={sink_open,sink_write,sink_status,sink_close,sink_now,NULL,NULL,NULL};
    struct music_runtime *rt=music_runtime_start(&out); assert(rt);
    struct music_command c={.action=MUSIC_OPEN}; snprintf(c.path,sizeof(c.path),"%s/mnt/e/%s",argv[1],names[0]);
    atomic_store(&reads,0); assert(!music_runtime_send(rt,&c));
    uint64_t start=clock_ns(); while (!atomic_load(&reads) && clock_ns()-start<1000000000) pause_ms(1);
    assert(atomic_load(&reads)); close_worker(rt);
    puts("PASS slow removable MP3/WAV/OGG, responsive Loading snapshots, open/read/format errors and cancellation");
}
