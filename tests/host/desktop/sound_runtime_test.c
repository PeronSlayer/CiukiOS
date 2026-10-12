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
static pthread_mutex_t fake_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t accepted, total; static int unavailable, failing, stalled, delay_consumed, writes, closed;
static FILE *fake_fopen(const char *path,const char *mode)
{
    char p[512];
    if (!strcmp(path,SOUND_CFG)) snprintf(p,sizeof(p),"build/external/system-sounds/sounds.cfg");
    else { assert(!strncmp(path,SOUND_DIR "/",sizeof(SOUND_DIR)));snprintf(p,sizeof(p),"build/external/system-sounds/%s",path+sizeof(SOUND_DIR)); }
    return fopen(p,mode);
}
static int fake_open(const char *path,int flags,...)
{
    assert(!strcmp(path,"/dev/audio") && (flags&O_NONBLOCK) && (flags&O_CLOEXEC));
    pthread_mutex_lock(&fake_lock);accepted=0;int bad=unavailable;pthread_mutex_unlock(&fake_lock);
    if(bad){errno=ENODEV;return -1;}return 100;
}
static int fake_close(int fd){assert(fd==100);pthread_mutex_lock(&fake_lock);closed++;pthread_mutex_unlock(&fake_lock);return 0;}
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
    assert(fd==100);pthread_mutex_lock(&fake_lock);
    if(op==AUDIO_GET_CAPS){assert(size==sizeof(struct ciuki_audio_caps));struct ciuki_audio_caps *c=record;c->output_rate=48000;c->drain_ms=20;}
    else if(op==AUDIO_SET_FORMAT){struct ciuki_audio_format *f=record;assert(f->rate==SOUND_RATE && f->channels==1 && f->format==CIUKI_AUDIO_S16LE);}
    else if(op==AUDIO_GET_STATUS){struct ciuki_audio_status *s=record;s->accepted_frames=accepted;s->consumed_frames=delay_consumed?0:accepted;s->output_rate=48000;s->transport_frames=256;}
    else assert(0);
    pthread_mutex_unlock(&fake_lock);return 0;
}
void desk_queue(struct desktop *d,int client,const struct ciuki_message *m){(void)d;(void)client;(void)m;}
#define fopen fake_fopen
#define open fake_open
#define close fake_close
#define write fake_write
#include "../../../apps/desktop/sound_runtime.c"
#undef fopen
#undef open
#undef close
#undef write
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
    static struct desktop d;d.clients[0].window=1;d.clients[0].window_id=1;d.now_ns=now_ns();desk_sound_init(&d);
    assert(d.sound_request && d.sound_shutdown); /* defaults loading may still be in progress */
    assert(request(&d,0,DESK_SETTINGS_AUDIO_TEST,0,1)==DESK_SOUND_PENDING);
    assert(request(&d,0,DESK_SETTINGS_AUDIO_TEST,0,2)==-EBUSY);
    assert(complete());pthread_mutex_lock(&rt.lock);assert(!rt.hardware_result);pthread_mutex_unlock(&rt.lock);
    pthread_mutex_lock(&fake_lock);assert(total==SOUND_RATE);pthread_mutex_unlock(&fake_lock);
    desk_sound_results(&d);wait_idle();
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
    puts("PASS sound worker: full tone acceptance, EAGAIN/EINTR/short writes, absent device, failure, timeout, shutdown drain/deadline/cancel");
    return 0;
}
