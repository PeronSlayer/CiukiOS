/* SPDX-License-Identifier: MIT -- native pre-session adapter. */
#include "firstboot.h"
#include "desktop.h"
#include "sound_storage.h"
#include "catalog.h"
#include "../../sdk/sysroot-overlay/include/ciuki/audio.h"
#include <ciuki/display.h>
#include "../../sdk/sysroot-overlay/include/ciuki/net.h"
#include <ciuki/surface.h>
#include "../../sdk/sysroot-overlay/include/ciuki/entropy.h"
#include "../../sdk/sysroot-overlay/include/ciuki/dosvm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __linux__
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#endif
#include <unistd.h>
#include "../control/accel_retry.h"
#ifndef FB_ROOT
#define FB_ROOT ""
#endif
struct native_fb {
    struct desktop *desktop;
    struct sound_player player;
    uint8_t *wav;
    int audio;
    unsigned actions;
};
static uint64_t milliseconds(void)
{struct timespec t={0};if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(unsigned)t.tv_nsec/1000000;}
static int read_path(const char *path,char *text,size_t cap,size_t *length)
{
    int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return -errno;
    size_t n=0;int r=0;
    while(n<cap){ssize_t k=read(fd,text+n,cap-n);if(k<0&&errno==EINTR)continue;if(k<0){r=-errno;break;}if(!k)break;n+=(size_t)k;}
    if(n==cap && !r)r=-EFBIG;
    if(close(fd)&&!r)r=-errno;*length=n;return r;
}
static int state_read(void *ctx,char *text,size_t cap,size_t *length)
{
    (void)ctx;
    return read_path(FB_ROOT "/SYSTEM/SETUP.STATE",text,cap,length);
}
static int append(void *ctx,const char *text,size_t length)
{
    (void)ctx;
    /* Never create a new install record or touch another drive. */
    int fd=open(FB_ROOT "/SYSTEM/SETUP.STATE",O_WRONLY|O_APPEND|O_CLOEXEC);if(fd<0)return -errno;
    struct stat st;int r=fstat(fd,&st)?-errno:0;
    if(!r && (st.st_size<0 || (uint64_t)st.st_size+length>=FB_STATE_BYTES))r=-EFBIG;
    while(!r && length){ssize_t k=write(fd,text,length);if(k<0&&errno==EINTR)continue;if(k<=0){r=k<0?-errno:-EIO;break;}text+=k;length-=(size_t)k;}
    if(!r && fsync(fd))r=-errno;
    if(close(fd)&&!r)r=-errno;return r;
}
static void record(void *ctx,const char *line)
{
    (void)ctx;char text[256];int n=snprintf(text,sizeof(text),"%s\n",line);
    if(n>0 && (size_t)n<sizeof(text)) { ssize_t ignored=write(2,text,(size_t)n);(void)ignored; }
}
static int sink(void *ctx,const int16_t *pcm,unsigned count)
{struct native_fb *n=ctx;ssize_t k=write(n->audio,pcm,count*2);return k<0?-errno:k%2?-EIO:(int)k/2;}
static int sound(struct native_fb *n,int start,char *text,size_t cap)
{
    if(start){
        struct sound_scheme scheme;
        (void)sound_load_at(&scheme,FB_ROOT SOUND_CFG,FB_ROOT SOUND_LEGACY_CFG,FB_ROOT "/System/Registry");
        if(!scheme.enabled || !scheme.volume){snprintf(text,cap,"Startup sound is disabled in settings.");return 1;}
        char path[160];if(sound_path(&scheme,SOUND_STARTUP,path,sizeof(path)))return -EINVAL;
        char full[192];snprintf(full,sizeof(full),"%s%s",FB_ROOT,path);
        n->wav=malloc(SOUND_FILE_MAX+1);if(!n->wav)return -ENOMEM;
        size_t len=0;int r=read_path(full,(char *)n->wav,SOUND_FILE_MAX+1,&len);if(r)return r;
        struct sound_clip clip;if(sound_wav(n->wav,len,&clip))return -EINVAL;
        n->audio=open(FB_ROOT "/dev/audio",O_WRONLY|O_NONBLOCK|O_CLOEXEC);
        if(n->audio<0){snprintf(text,cap,"No audio device is available.");return errno==ENODEV||errno==ENOENT?1:-errno;}
        struct ciuki_audio_format fmt={.version=1,.size=sizeof(fmt),.rate=SOUND_RATE,.format=CIUKI_AUDIO_S16LE,.channels=1};
        if(ciuki_devctl(n->audio,AUDIO_SET_FORMAT,&fmt,sizeof(fmt)))return -errno;
        sound_reset(&n->player);n->player.volume=(unsigned)scheme.volume;
        if(sound_play(&n->player,clip))return -EINVAL;
    }
    struct ciuki_audio_status status={.version=1,.size=sizeof(status)};
    if(ciuki_devctl(n->audio,AUDIO_GET_STATUS,&status,sizeof(status)))return -errno;
    if(status.last_error)return -EIO;
    if(sound_active(&n->player)){
        int r=sound_pump(&n->player,sink,n);if(r<0&&r!=-EAGAIN&&r!=-EINTR)return r;
        snprintf(text,cap,"Playing the startup sound...");return -EAGAIN;
    }
    if(status.queued_frames>1)return -EAGAIN;
    snprintf(text,cap,"Audio device present; startup sound played.");return 0;
}
static int game_query(void *ctx,unsigned game,int remove,uint32_t *bytes)
{
    (void)ctx;if(remove)return -EPERM;
    struct ciuki_dosvm_command c={.version=CIUKI_DOSVM_VERSION,.size=sizeof(c)};c.launch.game=game;
    if(ciuki_dosvm(CIUKI_DOSVM_GAME_QUERY,&c))return -errno;
    *bytes=c.changed;return (int)c.state;
}
static int service(void *ctx,unsigned step,int start,char *text,size_t cap)
{
    struct native_fb *n=ctx;
    if(step==FB_ACCEL){
        struct ciuki_accel_info a={0};
        if(ciuki_display_accel_info(n->desktop->display,n->desktop->accel_generation,&a))return -errno;
        /* A leftover attempt from an interrupted boot is software mode too.
         * Do not write or interpret driver-specific register status here. */
        struct stat marker;int pending=!stat(FB_ROOT "/system/settings/accel.cfg",&marker)&&marker.st_size>0;
        return fb_accel_outcome(a.capabilities,a.quarantined,pending,text,cap);
    }
    if(step==FB_SOUND)return sound(n,start,text,cap);
    if(step==FB_NETWORK){
        struct ciuki_net_state net={0};if(ciuki_net_status(&net))return -errno;
        if(!net.present){snprintf(text,cap,"No network adapter; you can configure networking later.");return 1;}
        if(net.address_configured){snprintf(text,cap,"Network address: %u.%u.%u.%u",net.ipv4[0],net.ipv4[1],net.ipv4[2],net.ipv4[3]);return 0;}
        snprintf(text,cap,"Waiting for the configured network service (at most 10 seconds)...");return -EAGAIN;
    }
    if(step==FB_GAMES)return fb_register_games(FB_ROOT "/System/Registry",game_query,NULL,text,cap);
    if(step==FB_ENTROPY){
        unsigned char byte;int r=getentropy(&byte,1);byte=0;
        if(!r){snprintf(text,cap,"Kernel entropy is ready. HTTPS also needs an RTC-set clock dated 2026 or later.");return 0;}
        snprintf(text,cap,"Kernel entropy is not ready; HTTPS will become available when entropy and the clock are ready.");
        return errno==EAGAIN?-EAGAIN:-errno;
    }
    if(step==FB_CLOCK){
        time_t seconds=time(NULL);struct tm *tm=gmtime(&seconds);if(!tm)return -EIO;
        char clock[64],config[160]={0};size_t length=0;
        if(!strftime(clock,sizeof(clock),"%Y-%m-%d %H:%M:%S",tm))return -EIO;
        int r=read_path(FB_ROOT "/system/settings/clock.cfg",config,sizeof(config)-1,&length);
        config[length]=0;
        const char *convention=r?"convention unavailable":strstr(config,"rtc_local=1\n")?"RTC local time":"RTC UTC";
        snprintf(text,cap,"System time: %s (%s). Change opens Date & Time after Finish.",clock,convention);return 0;
    }
    /* Keep the summary prepared by fb_tick. */
    return 0;
}
static void cancel_service(void *ctx,unsigned step)
{struct native_fb *n=ctx;if(step==FB_SOUND){if(n->audio>=0)close(n->audio);n->audio=-1;free(n->wav);n->wav=NULL;}}
static void action(void *ctx,unsigned a)
{
    struct native_fb *n=ctx;
    n->actions|=1u<<a;
    /* The button opens Display after Finish. Retry itself is an explicit
     * Control Panel action, with its restart explanation, not automatic. */
}
int desk_firstboot(struct desktop *d)
{
    struct native_fb native={.desktop=d,.audio=-1};
    struct fb_host h={&native,state_read,append,service,cancel_service,record,action};
    struct firstboot f;int r=fb_begin(&f,&h);
    if(r<=0){if(r<0)record(NULL,"[firstboot] state=unreadable result=failed");return 0;}
    if(d->width<640 || d->height<480){record(NULL,"[firstboot] display=too_small result=failed");return 0;}
    int mx=320,my=240,ox=(d->width-640)/2,oy=(d->height-480)/2;
    while(!f.done){
        fb_tick(&f,milliseconds());
        struct ciuki_input_event events[16];int count=ciuki_input_read(d->input,events,16);
        if(count<0 && errno!=EAGAIN && errno!=EINTR){record(NULL,"[firstboot] input=closed result=failed");break;}
        for(int i=0;i<count;i++){
            struct ciuki_input_event *e=&events[i];
            if(e->type==CIUKI_INPUT_MOTION){mx+=e->value;my+=e->value2;if(mx<0)mx=0;if(mx>639)mx=639;if(my<0)my=0;if(my>479)my=479;}
            int activate=0;
            if(e->type==CIUKI_INPUT_KEY && e->value==1){
                if(e->code==0x0f)f.focus=(f.focus+1)%3;
                if(e->code==0x44)fb_finish(&f);
                if(e->code==0x1c)activate=1;
            }
            if(e->type==CIUKI_INPUT_BUTTON && e->code==1 && e->value==1 && my>=396&&my<426){
                if(mx>=184&&mx<332){f.focus=0;activate=1;}
                if(mx>=344&&mx<478){f.focus=1;activate=1;}
                if(mx>=492&&mx<608){f.focus=2;activate=1;}
            }
            if(activate){if(f.focus==2)fb_finish(&f);else fb_action(&f,f.focus);}
        }
        struct du_canvas canvas={d->pixels,d->width,d->height,{0,0,d->width,d->height}};
        fb_draw(&f,&canvas,d->portrait);
        for(int y=0;y<12;y++)for(int x=0;x<=y/2;x++)du_fill(&canvas,(struct du_rect){ox+mx+x,oy+my+y,1,1},!x||x==y/2?0:0xffffff);
        struct ciuki_rect rect={.width=(unsigned)d->width,.height=(unsigned)d->height};
        if(ciuki_present(d->display,d->output,&rect)){record(NULL,"[firstboot] present=failed result=failed");break;}
        struct timespec pause={0,20000000};nanosleep(&pause,NULL);
    }
    cancel_service(&native,FB_SOUND);
    return (int)native.actions;
}
