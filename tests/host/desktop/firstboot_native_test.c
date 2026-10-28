/* SPDX-License-Identifier: MIT -- actual native services and commit boundary. */
#include "../../../apps/desktop/firstboot_native.c"
#include <assert.h>
static unsigned synced,fail_sync,net_calls,audio_calls,accel_caps=1;
static const char *accel_reason="active";
static int entropy_pending,network_pending;
int __real_fsync(int);
int __wrap_fsync(int fd){synced++;if(fail_sync){errno=EIO;return -1;}return __real_fsync(fd);}
/* The registry's engine record, as the kernel fills it: a bound driver
 * (never named by first boot) with its readiness and reason. */
int ciuki_display_engine_info(int fd,uint32_t generation,struct ciuki_display_engine_info *out)
{
    assert(fd==61&&generation==7&&out->version==CIUKI_DISPLAY_ENGINE_VERSION&&out->size==sizeof(*out));
    memset(out,0,sizeof(*out));out->version=CIUKI_DISPLAY_ENGINE_VERSION;out->size=sizeof(*out);
    out->kind=accel_caps?CIUKI_ENGINE_NATIVE:CIUKI_ENGINE_SOFTWARE;out->ready=accel_caps!=0;out->capabilities=accel_caps;
    snprintf(out->driver_id,sizeof(out->driver_id),"trident");out->pci_vendor=0x1023;out->pci_device=0x8820;
    snprintf(out->reason,sizeof(out->reason),"%s",accel_reason);return 0;
}
static int read_file(const char *path,char *out,size_t cap)
{FILE *f=fopen(path,"rb");if(!f)return -1;size_t n=fread(out,1,cap-1,f);out[n]=0;fclose(f);return (int)n;}
int ciuki_net_status(struct ciuki_net_state *out)
{net_calls++;out->present=1;out->address_configured=!network_pending;out->ipv4[0]=192;out->ipv4[1]=0;out->ipv4[2]=2;out->ipv4[3]=5;return 0;}
int ciuki_devctl(int fd,uint32_t op,void *out,uint32_t bytes)
{(void)fd;(void)bytes;audio_calls++;if(op==AUDIO_SET_FORMAT)return 0;if(op==AUDIO_GET_STATUS){struct ciuki_audio_status *s=out;s->queued_frames=0;return 0;}errno=ENOTSUP;return -1;}
int ciuki_dosvm(uint32_t op,struct ciuki_dosvm_command *out)
{assert(op==CIUKI_DOSVM_GAME_QUERY);out->state=1;return 0;}
int getentropy(void *out,size_t bytes)
{assert(bytes==1);if(entropy_pending){errno=EAGAIN;return -1;}memset(out,0,bytes);return 0;}
int ciuki_devices(struct ciuki_devices_info *out){(void)out;errno=ENOSYS;return -1;}
/* sec-user-02: the session credential (call 101 QUERY_SELF). */
static int credential_groups=2,credential_error;
int ciuki_security_self(struct ciuki_credential *c)
{
    if(credential_error){errno=credential_error;return -1;}
    memset(c,0,sizeof(*c));c->version=1;c->size=64;c->uid=credential_groups==2?1000:1001;c->primary_gid=545;
    c->group_count=(uint32_t)credential_groups;c->groups[0]=545;if(credential_groups==2)c->groups[1]=544;return 0;
}
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
    struct fb_host h={&n,state_read,append,service,cancel_service,record,action,NULL};struct firstboot f;
    assert(fb_begin(&f,&h)==0);
    const char pending[]="phase=installed\nfirstboot=pending\n";
    put(FB_ROOT "/SYSTEM/SETUP.STATE",pending,sizeof(pending)-1);
    /* sec-user-02: only an administrator session runs the pending wizard;
     * Standard users and an unreadable credential leave it pending. */
    struct fb_host gated=h;gated.administrator=administrator;
    credential_groups=1;assert(fb_begin(&f,&gated)==0&&!f.active);
    credential_error=EPERM;assert(fb_begin(&f,&gated)==0&&!f.active);credential_error=0;
    credential_groups=2;assert(fb_begin(&f,&gated)==1&&f.active);
    assert(fb_begin(&f,&h)==1);fb_tick(&f,0);assert(synced==1&&f.result[0]==FB_OK);
    fail_sync=1;assert(append(&n,"\nstep=sound result=ok\n",22)==-EIO);fail_sync=0;
    char text[192];assert(!service(&n,FB_ACCEL,1,text,sizeof(text))&&strstr(text,"is on"));
    accel_caps=0;assert(service(&n,FB_ACCEL,1,text,sizeof(text))==1&&strstr(text,"Software"));
    /* DM8: the registry's retained-attempt status, not a marker file, makes
     * this boot software; a leftover shared marker alone changes nothing. */
    accel_caps=0;accel_reason="attempt_retained";
    assert(service(&n,FB_ACCEL,1,text,sizeof(text))==1&&strstr(text,"Software"));
    put(FB_ROOT "/system/settings/accel.cfg","accel=attempt\n",14);accel_caps=1;accel_reason="active";
    assert(!service(&n,FB_ACCEL,1,text,sizeof(text))&&strstr(text,"is on"));
    /* Retry: one request keyed by the engine record, for any bound driver. */
    struct ciuki_display_engine_info e={.version=CIUKI_DISPLAY_ENGINE_VERSION,.size=sizeof(e),.pci_vendor=0x5333,.pci_device=0x8c2e};
    char request[64];const char *retry=FB_ROOT "/system/settings/accel-retry.cfg";
    snprintf(e.driver_id,sizeof(e.driver_id),"s3savage");
    assert(!cp_accel_retry_request(retry,&e)&&read_file(retry,request,sizeof(request))>0&&!strcmp(request,"driver=s3savage\npci=5333:8c2e\n"));
    snprintf(e.driver_id,sizeof(e.driver_id),"trident");e.pci_vendor=0x1023;e.pci_device=0x8820;
    assert(!cp_accel_retry_request(retry,&e)&&read_file(retry,request,sizeof(request))>0&&!strcmp(request,"driver=trident\npci=1023:8820\n"));
    unsigned before=synced;snprintf(e.driver_id,sizeof(e.driver_id),"swdib");e.pci_vendor=0;
    assert(cp_accel_retry_request(retry,&e)==-ENODEV&&synced==before&&read_file(retry,request,sizeof(request))>0&&strstr(request,"trident"));
    e.pci_vendor=0x1023;snprintf(e.driver_id,sizeof(e.driver_id),"Bad/id");assert(cp_accel_retry_request(retry,&e)==-EINVAL);
    memset(e.driver_id,'a',sizeof(e.driver_id));assert(cp_accel_retry_request(retry,&e)==-EINVAL);
    e.version=2;snprintf(e.driver_id,sizeof(e.driver_id),"trident");assert(cp_accel_retry_request(retry,&e)==-EINVAL);
    e.version=CIUKI_DISPLAY_ENGINE_VERSION;fail_sync=1;assert(cp_accel_retry_request(retry,&e)==-EIO);fail_sync=0;
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
    assert(!service(&n,FB_GAMES,1,text,sizeof(text))&&strstr(text,"5 bundled"));
    puts("PASS native firstboot: fsync failures, registry status/keyed retry request, real WAV/audio, network, entropy, RTC, games");
    return 0;
}
