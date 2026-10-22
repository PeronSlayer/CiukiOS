/* Production sound worker with a fake device; real SDK-thread behaviour on the host.
 * SPDX-License-Identifier: MIT */
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include "desktop.h"
#include <ciuki/audio.h>
#include <ciuki/devices.h>
#include "sound_storage.h"
int ciuki_devices(struct ciuki_devices_info *s) { (void)s; errno=ENODEV; return -1; }
static pthread_mutex_t fake_lock=PTHREAD_MUTEX_INITIALIZER;
static int muted, vm_active;
static uint64_t accepted, total; static int unavailable, failing, stalled, delay_consumed, tail_frame, writes, closed, status_calls;
static int fake_sound_load_at(struct sound_scheme *s,const char *cfg,const char *legacy,const char *dir)
{
    (void)cfg;(void)legacy;(void)dir;
    return sound_load_at(s,"build/external/system-sounds/scheme.cfg",NULL,"build/host/no-sound-registry");
}
static FILE *fake_fopen(const char *path,const char *mode)
{
    char p[512];
    if (!strcmp(path,SOUND_LEGACY_CFG)) { errno=ENOENT; return NULL; }
    if (!strcmp(path,SOUND_CFG)) snprintf(p,sizeof(p),"build/external/system-sounds/scheme.cfg");
    else { assert(!strncmp(path,SOUND_DIR "/",sizeof(SOUND_DIR)));snprintf(p,sizeof(p),"build/external/system-sounds/%s",path+sizeof(SOUND_DIR)); }
    return fopen(p,mode);
}
static int fake_open(const char *path,int flags,...)
{
    if (!strcmp(path,"/dev/mixer")) { assert((flags&O_ACCMODE)==O_RDONLY); return 101; }
    assert(!strcmp(path,"/dev/audio") && (flags&O_NONBLOCK) && (flags&O_CLOEXEC));
    pthread_mutex_lock(&fake_lock);accepted=0;int bad=unavailable;pthread_mutex_unlock(&fake_lock);
    if(bad){errno=ENODEV;return -1;}return 100;
}
static int fake_close(int fd){if(fd==101)return 0;assert(fd==100);pthread_mutex_lock(&fake_lock);closed++;pthread_mutex_unlock(&fake_lock);return 0;}
static ssize_t fake_write(int fd,const void *p,size_t bytes)
{
    (void)p;assert(fd==100 && bytes<=512 && !(bytes%2));
    pthread_mutex_lock(&fake_lock);writes++;int err=failing ? EIO : stalled || writes%7==1 ? EAGAIN : writes%7==2 ? EINTR : 0;
    if(bytes>12 && writes%7==3)bytes=12;
    if(!err){accepted+=bytes/2;total+=bytes/2;}pthread_mutex_unlock(&fake_lock);
    if(err){errno=err;return -1;}return (ssize_t)bytes;
}
int ciuki_devctl(int fd,uint32_t op,void *record,uint32_t size)
{
    assert(fd==100 || fd==101);pthread_mutex_lock(&fake_lock);
    if(op==MIXER_GET_MASTER){assert(fd==101);((struct ciuki_mixer_master *)record)->mute=(unsigned)muted;}
    else if(op==MIXER_GET_DIAGNOSTICS){assert(fd==101);((struct ciuki_audio_diagnostics *)record)->flags=vm_active ? CIUKI_AUDIO_DIAG_VM_ACTIVE : 0;}
    else if(op==AUDIO_GET_CAPS){assert(size==sizeof(struct ciuki_audio_caps));struct ciuki_audio_caps *c=record;c->output_rate=48000;c->drain_ms=20;}
    else if(op==AUDIO_SET_FORMAT){struct ciuki_audio_format *f=record;assert(f->rate==SOUND_RATE && f->channels==1 && f->format==CIUKI_AUDIO_S16LE);}
    else if(op==AUDIO_GET_STATUS){
        /* queued = accepted - consumed, as the kernel reports it. tail_frame models
         * the mixer's resampled last frame, which waits for an interpolation
         * partner unless the stream drains (src/kernel/audio/mixer.c). */
        struct ciuki_audio_status *s=record;status_calls++;
        uint64_t consumed=delay_consumed ? 0 : tail_frame && accepted ? accepted-1 : accepted;
        s->accepted_frames=accepted;s->consumed_frames=consumed;s->queued_frames=(uint32_t)(accepted-consumed);
        s->output_rate=48000;s->transport_frames=256;}
    else assert(0);
    pthread_mutex_unlock(&fake_lock);return 0;
}
void desk_queue(struct desktop *d,int client,const struct ciuki_message *m){(void)d;(void)client;(void)m;}
#ifdef DESK_FOOTPRINT_TEST
#include "footprint_alloc.h"
/* Hold the newly created worker until the first-frame boundary. This makes
 * late startup allocations reproducible, independent of host scheduling. */
static pthread_mutex_t start_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_changed=PTHREAD_COND_INITIALIZER;
static int start_held=1;
static void *(*start_function)(void *);
static void *start_argument;
static void *held_worker(void *unused)
{
    (void)unused; pthread_mutex_lock(&start_lock);
    while (start_held) pthread_cond_wait(&start_changed,&start_lock);
    pthread_mutex_unlock(&start_lock); return start_function(start_argument);
}
static int footprint_thread_create(pthread_t *t,const pthread_attr_t *a,void *(*fn)(void *),void *arg)
{ start_function=fn; start_argument=arg; return pthread_create(t,a,held_worker,NULL); }
#define pthread_create footprint_thread_create
#endif
#define sound_load_at fake_sound_load_at
#define fopen fake_fopen
#define open fake_open
#define close fake_close
#define write fake_write
#include "../../../apps/desktop/sound_runtime.c"
#undef sound_load_at
#undef fopen
#undef open
#undef close
#undef write
#ifdef DESK_FOOTPRINT_TEST
#undef pthread_create
#endif
static int complete(void)
{
    uint64_t deadline=now_ns()+3000000000ull;
    while(now_ns()<deadline){pthread_mutex_lock(&rt.lock);int done=rt.hardware_done;pthread_mutex_unlock(&rt.lock);if(done) return 1;
        struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
    return 0;
}
static void wait_idle(void)
{
    uint64_t deadline=now_ns()+3000000000ull;
    for(;;){pthread_mutex_lock(&rt.lock);int busy=rt.busy;pthread_mutex_unlock(&rt.lock);if(!busy)return;assert(now_ns()<deadline);
        struct timespec pause={0,1000000};nanosleep(&pause,NULL);}
}
int main(void)
{
    rt.live=1;
    for(unsigned i=0;i<50;i++)assert(!push(SOUND_VOLUME,0));
    assert(rt.count==1);rt.count=rt.head=rt.busy=0;rt.queued=(struct sound_throttle){0};rt.live=0;
    static struct desktop d;d.clients[0].window=1;d.clients[0].window_id=1;d.now_ns=now_ns();desk_sound_init(&d);
#ifdef DESK_FOOTPRINT_TEST
    footprint_begin("sound_after_init");
    pthread_mutex_lock(&start_lock); start_held=0; pthread_cond_signal(&start_changed); pthread_mutex_unlock(&start_lock);
#endif
    assert(d.sound_request && d.sound_shutdown); /* defaults loading may still be in progress */
    assert(request(&d,0,DESK_SETTINGS_AUDIO_TEST,0,1)==DESK_SOUND_PENDING);
    assert(request(&d,0,DESK_SETTINGS_AUDIO_TEST,0,2)==-EBUSY);
    assert(complete());pthread_mutex_lock(&rt.lock);assert(!rt.hardware_result);pthread_mutex_unlock(&rt.lock);
    pthread_mutex_lock(&fake_lock);assert(total==SOUND_RATE);pthread_mutex_unlock(&fake_lock);
    desk_sound_results(&d);wait_idle();
    uint64_t before;
    for (int vm=0;vm<2;vm++) {
        pthread_mutex_lock(&fake_lock);before=total;muted=!vm;vm_active=vm;pthread_mutex_unlock(&fake_lock);
        notify(&d,SOUND_ERROR);assert(!request(&d,0,DESK_SETTINGS_SOUND_TEST,SOUND_VOLUME,1));wait_idle();
        pthread_mutex_lock(&fake_lock);assert(total==before);muted=vm_active=0;pthread_mutex_unlock(&fake_lock);
        wait_idle();assert(total==before); /* skipped events do not resume */
    }
    before=total;notify(&d,SOUND_ERROR);wait_idle();assert(total>before);
    /* A newly active VM or mute cancels an already admitted UI stream. */
    for(int vm=0;vm<2;vm++) {
        pthread_mutex_lock(&fake_lock);stalled=1;int previous=writes;pthread_mutex_unlock(&fake_lock);
        notify(&d,SOUND_ERROR);
        uint64_t deadline=now_ns()+1000000000ull;
        for(;;) {
            pthread_mutex_lock(&fake_lock);int advanced=writes>previous;pthread_mutex_unlock(&fake_lock);
            if(advanced)break;
            assert(now_ns()<deadline);struct timespec pause={0,1000000};nanosleep(&pause,NULL);
        }
        pthread_mutex_lock(&fake_lock);before=total;muted=!vm;vm_active=vm;pthread_mutex_unlock(&fake_lock);
        wait_idle();pthread_mutex_lock(&fake_lock);assert(total==before);muted=vm_active=stalled=0;pthread_mutex_unlock(&fake_lock);
    }
    /* Holding the queue mutex while filling a burst makes admission deterministic. */
    pthread_mutex_lock(&rt.lock);rt.queued=(struct sound_throttle){.volume_seen=1,.last_volume=now_ns()};pthread_mutex_unlock(&rt.lock);
    for(unsigned i=0;i<50;i++)notify(&d,SOUND_VOLUME);
    wait_idle();pthread_mutex_lock(&rt.lock);assert(!rt.count);pthread_mutex_unlock(&rt.lock);
    pthread_mutex_lock(&fake_lock);unavailable=1;pthread_mutex_unlock(&fake_lock);
    assert(request(&d,0,DESK_SETTINGS_AUDIO_TEST,0,3)==DESK_SOUND_PENDING);assert(complete());assert(rt.hardware_result==-ENODEV);
    desk_sound_results(&d);wait_idle();
    pthread_mutex_lock(&fake_lock);unavailable=0;failing=1;pthread_mutex_unlock(&fake_lock);
    assert(request(&d,0,DESK_SETTINGS_AUDIO_TEST,0,4)==DESK_SOUND_PENDING);assert(complete());assert(rt.hardware_result==-EIO);
    desk_sound_results(&d);wait_idle();
    pthread_mutex_lock(&fake_lock);failing=0;stalled=1;pthread_mutex_unlock(&fake_lock);
    assert(request(&d,0,DESK_SETTINGS_AUDIO_TEST,0,5)==DESK_SOUND_PENDING);assert(complete());assert(rt.hardware_result==-ETIMEDOUT);
    desk_sound_results(&d);wait_idle();
    pthread_mutex_lock(&fake_lock);stalled=0;delay_consumed=1;pthread_mutex_unlock(&fake_lock);
    assert(!shutdown_wait(&d));uint64_t start=d.now_ns; d.now_ns=start+1999999999ull;assert(!shutdown_wait(&d));
    d.now_ns=start+2000000000ull;assert(shutdown_wait(&d));wait_idle();cancel(&d);wait_idle();
    pthread_mutex_lock(&fake_lock);delay_consumed=0;pthread_mutex_unlock(&fake_lock);
    d.now_ns=now_ns();assert(!shutdown_wait(&d));wait_idle();d.now_ns=now_ns();assert(shutdown_wait(&d));cancel(&d);wait_idle();
    pthread_mutex_lock(&fake_lock);assert(closed>=5);pthread_mutex_unlock(&fake_lock);
    /* f2-95: a 22050 Hz clip whose last frame stays queued in the mixer still
     * ends: the device closes within the drain allowance and the worker then
     * sleeps on its queue instead of polling every 2 ms. */
    pthread_mutex_lock(&fake_lock);tail_frame=1;int closed_before=closed;before=total;pthread_mutex_unlock(&fake_lock);
    uint64_t tail_begin=now_ns();notify(&d,SOUND_ERROR);wait_idle();uint64_t tail_ms=(now_ns()-tail_begin)/1000000ull;
    pthread_mutex_lock(&fake_lock);assert(total>before && closed==closed_before+1);int calls=status_calls;pthread_mutex_unlock(&fake_lock);
    struct timespec quiet={0,100000000};nanosleep(&quiet,NULL);
    pthread_mutex_lock(&fake_lock);int idle_calls=status_calls-calls;tail_frame=0;pthread_mutex_unlock(&fake_lock);
    assert(!idle_calls);
    printf("PASS F2-95 sound tail: last resampled frame queued, device closed after %llu ms, status polls in 100 ms idle=%d\n",
           (unsigned long long)tail_ms,idle_calls);
#ifdef DESK_FOOTPRINT_TEST
    unsigned allocations=footprint_end();
    if (!getenv("CIUKI_FOOTPRINT_AUDIT")) assert(!allocations);
#endif
    puts("PASS sound worker: full tone acceptance, EAGAIN/EINTR/short writes, absent device, failure, timeout, shutdown drain/deadline/cancel");
    return 0;
}
