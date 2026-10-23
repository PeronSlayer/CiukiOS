/* SPDX-License-Identifier: MIT -- actual native services and commit boundary. */
#include "../../../apps/desktop/firstboot_native.c"
#include <assert.h>
static unsigned synced,fail_sync,net_calls,audio_calls,accel_caps=1;
static int entropy_pending,network_pending;
int __real_fsync(int);
int __wrap_fsync(int fd){synced++;if(fail_sync){errno=EIO;return -1;}return __real_fsync(fd);}
int ciuki_display_accel_info(int fd,uint32_t generation,struct ciuki_accel_info *out)
{assert(fd==61&&generation==7);out->capabilities=accel_caps;return 0;}
int ciuki_net_status(struct ciuki_net_state *out)
{net_calls++;out->present=1;out->address_configured=!network_pending;out->ipv4[0]=192;out->ipv4[1]=0;out->ipv4[2]=2;out->ipv4[3]=5;return 0;}
int ciuki_devctl(int fd,uint32_t op,void *out,uint32_t bytes)
{(void)fd;(void)bytes;audio_calls++;if(op==AUDIO_SET_FORMAT)return 0;if(op==AUDIO_GET_STATUS){struct ciuki_audio_status *s=out;s->queued_frames=0;return 0;}errno=ENOTSUP;return -1;}
int ciuki_dosvm(uint32_t op,struct ciuki_dosvm_command *out)
{assert(op==CIUKI_DOSVM_GAME_QUERY);out->state=1;return 0;}
int getentropy(void *out,size_t bytes)
{assert(bytes==1);if(entropy_pending){errno=EAGAIN;return -1;}memset(out,0,bytes);return 0;}
int ciuki_devices(struct ciuki_devices_info *out){(void)out;errno=ENOSYS;return -1;}
int sound_load_at(struct sound_scheme *s,const char *a,const char *b,const char *c)
{(void)a;(void)b;(void)c;sound_defaults(s);return 0;}
static void put(const char *path,const void *bytes,size_t count)
{FILE *f=fopen(path,"wb");assert(f);assert(fwrite(bytes,1,count,f)==count);assert(!fclose(f));}
static void dir(const char *path){assert(!mkdir(path,0700));}
int main(void)
{
    dir(FB_ROOT "/SYSTEM");dir(FB_ROOT "/System");dir(FB_ROOT "/System/Registry");
    dir(FB_ROOT "/system");dir(FB_ROOT "/system/settings");dir(FB_ROOT "/system/sounds");dir(FB_ROOT "/dev");
    struct desktop d={.display=61,.accel_generation=7};struct native_fb n={.desktop=&d,.audio=-1};
    struct fb_host h={&n,state_read,append,service,cancel_service,record,action};struct firstboot f;
    assert(fb_begin(&f,&h)==0);
    const char pending[]="phase=installed\nfirstboot=pending\n";
    put(FB_ROOT "/SYSTEM/SETUP.STATE",pending,sizeof(pending)-1);
    assert(fb_begin(&f,&h)==1);fb_tick(&f,0);assert(synced==1&&f.result[0]==FB_OK);
    fail_sync=1;assert(append(&n,"\nstep=sound result=ok\n",22)==-EIO);fail_sync=0;
    char text[192];assert(!service(&n,FB_ACCEL,1,text,sizeof(text))&&strstr(text,"is on"));
    accel_caps=0;assert(service(&n,FB_ACCEL,1,text,sizeof(text))==1&&strstr(text,"Software"));
    put(FB_ROOT "/system/settings/accel.cfg","accel=attempt\n",14);accel_caps=1;
    assert(service(&n,FB_ACCEL,1,text,sizeof(text))==1&&strstr(text,"Software"));
    assert(!cp_accel_retry_at(FB_ROOT "/system/settings/accel.cfg"));struct stat st;assert(!stat(FB_ROOT "/system/settings/accel.cfg",&st)&&st.st_size==0);
    assert(!service(&n,FB_NETWORK,1,text,sizeof(text))&&strstr(text,"192.0.2.5"));
    network_pending=1;assert(service(&n,FB_NETWORK,1,text,sizeof(text))==-EAGAIN&&net_calls==2);
    entropy_pending=1;assert(service(&n,FB_ENTROPY,1,text,sizeof(text))==-EAGAIN&&strstr(text,"HTTPS"));entropy_pending=0;
    assert(!service(&n,FB_ENTROPY,0,text,sizeof(text))&&strstr(text,"ready"));
    const char cfg[]="version=1\nrtc_local=1\noffset_minutes=60\n";put(FB_ROOT "/system/settings/clock.cfg",cfg,sizeof(cfg)-1);
    assert(!service(&n,FB_CLOCK,1,text,sizeof(text))&&strstr(text,"RTC local"));
    /* A small real WAV goes through the production parser/pump and audio
     * service, never a blocking AUDIO_DRAIN or a kernel-generated tone. */
    unsigned char wav[48]={'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,0x22,0x56,0,0,0x44,0xac,0,0,2,0,16,0,'d','a','t','a',4,0,0,0,0,0,1,0};
    struct sound_scheme scheme;sound_defaults(&scheme);char path[160],full[512];assert(!sound_path(&scheme,SOUND_STARTUP,path,sizeof(path)));snprintf(full,sizeof(full),"%s%s",FB_ROOT,path);put(full,wav,sizeof(wav));put(FB_ROOT "/dev/audio","",0);
    int r=service(&n,FB_SOUND,1,text,sizeof(text));for(unsigned i=0;i<10&&r==-EAGAIN;i++)r=service(&n,FB_SOUND,0,text,sizeof(text));assert(!r&&audio_calls);cancel_service(&n,FB_SOUND);assert(n.audio==-1&&!n.wav);
    assert(!service(&n,FB_GAMES,1,text,sizeof(text))&&strstr(text,"3 bundled"));
    puts("PASS native firstboot: fsync failures, marker outcomes/retry, real WAV/audio, network, entropy, RTC, games");
    return 0;
}
