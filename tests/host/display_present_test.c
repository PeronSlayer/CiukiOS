/* Production lease state machine, deterministic revocation schedules at every
 * sleeping/pinning/copy boundary. No firmware or framebuffer emulation claims.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/kernel.h>
#include <ciuki/display.h>
#include <ciuki/present_abi.h>
#define CIUKI_CPU_H
static uint8_t page_memory[4096];
#define P2V(p) ((void)(p),(void *)page_memory)
volatile uint64_t g_ticks;
struct ciuki_boot_info g_boot;
static struct process desktop={.pid=2,.state=PROC_LIVE}, game={.pid=3,.state=PROC_LIVE,.personality=PROC_WIN32};
static struct proc_thread dt={.process=&desktop}, gt={.process=&game};
static struct fb_device device={.present=true,.width=640,.height=480,.pitch=2560,.bpp=32,
    .red_size=8,.red_pos=16,.green_size=8,.green_pos=8,.blue_size=8,.generation=1,.mode_id=0x118};
static uint8_t user[70000],scanout[640*480*4];
static bool suspended,connected=true,grant_available=true,mode_fail,allocation_fail,pin_fail,read_fail;
static unsigned allocations,pins,writes,returns,mode_calls,revoked_writes;
static uint32_t fresh;
static void (*on_pin)(void),(*on_read)(void),(*on_yield)(void),(*on_sleep)(void),(*on_write)(void);
const struct fb_device *fbdev_get(void){return &device;}
struct process *proc_find(uint32_t pid){return pid==desktop.pid?&desktop:pid==game.pid?&game:NULL;}
uint32_t supervisor_desktop_pid(void){return 2;}
uint64_t deadline_after_ms(uint32_t n){return g_ticks+n;}
bool deadline_passed(uint64_t d){return g_ticks-d<(UINT64_C(1)<<63);}
bool grants_display_owner(const struct process *p){return p==&desktop&&grant_available;}
bool w32_service_desktop_client(struct process *p,uint32_t pid){return p==&desktop&&pid==game.pid&&connected;}
bool display_fullscreen_active(void){return false;}
int display_worker_start(void){return 0;}
void fbdev_cursor_hide(void){assert(suspended);}
void display_present_returned(void){returns++;}
int display_set_for_present(struct process *p,uint16_t mode,bool delegated){
    assert(p==&desktop&&delegated);mode_calls++;
    if(mode_fail){mode_fail=false;return -EIO;}
    if(device.mode_id!=mode){device.mode_id=mode;device.generation++;}
    return 0;
}
int grants_display_suspend(bool on){assert(!on||!suspended);suspended=on;return 0;}
gen_t gen_alloc(void){return ++fresh;}
static pthread_mutex_t host_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rendezvous=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool copy_entered,copy_release,revoke_attempted,revoke_done;
static pthread_t revoker;
static bool racing;
static _Thread_local bool revoking_thread;
void kmutex_lock(struct kmutex *m){
    if(racing && revoking_thread){
        assert(!pthread_mutex_lock(&rendezvous));revoke_attempted=true;
        assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&rendezvous));
    }
    assert(!pthread_mutex_lock(&host_mutex));assert(!m->owner);m->owner=(void *)1;
}
void kmutex_unlock(struct kmutex *m){assert(m->owner);m->owner=0;assert(!pthread_mutex_unlock(&host_mutex));}
void klog(const char *fmt,...){(void)fmt;}
uint32_t pmm_alloc(void){if(allocation_fail)return 0;assert(!allocations);allocations++;return 4096;}
void pmm_free(uint32_t p){assert(p==4096&&allocations==1);allocations--;}
int ua_pin(struct uaddr *u,struct ua_pin *pin,uint32_t base,uint32_t n,bool write){
    (void)u;(void)pin;assert(!write);if(on_pin)on_pin();if(pin_fail||base>sizeof(user)||n>sizeof(user)-base)return -EFAULT;pins++;return 0;
}
void ua_unpin(struct uaddr *u,struct ua_pin *pin){(void)u;(void)pin;assert(pins);pins--;}
int ua_read(const struct uaddr *u,void *dst,uint32_t base,uint32_t n){
    (void)u;if(on_read)on_read();if(read_fail||base>sizeof(user)||n>sizeof(user)-base)return -EFAULT;memcpy(dst,user+base,n);return 0;
}
int copy_from_user(void *dst,uint32_t base,uint32_t n){return ua_read(NULL,dst,base,n);}
void kpreempt_point(void){if(on_yield)on_yield();}
void task_sleep_ms(uint32_t ms){g_ticks+=ms;if(on_sleep)on_sleep();}
int fbdev_present_native(uint32_t generation,unsigned x,unsigned y,const void *p,unsigned count){
    assert(generation==device.generation&&suspended&&x+count<=640&&y<480);
    writes++;if(revoked_writes)assert(writes==revoked_writes);
    memcpy(scanout+y*device.pitch+x*4,p,count*4);if(on_write)on_write();return 0;
}
#include "../../src/kernel/core/display_present.c"
static struct ciuki_w32_display_args grant_args(void){return (struct ciuki_w32_display_args){
    .action=W32_DISPLAY_GRANT,.pid=3,.mode=0x111,.scale=2,
    .surface={320,200,320,64000,CIUKI_PRESENT_INDEX8}};}
static struct ciuki_w32_display_args holder_args(const struct ciuki_w32_display_args *g,unsigned action){
    return (struct ciuki_w32_display_args){.action=action,.pid=3,.token=g->token,.generation=g->generation};
}
static void revoke_now(void){
    on_pin=on_read=on_yield=on_sleep=NULL;
    assert(display_present_revoke_kernel()==1);revoked_writes=writes;
}
static void die_now(void){on_yield=NULL;game.state=PROC_STOPPING;display_present_poll();revoked_writes=writes;}
static void complete(void){assert(!allocations&&!pins&&!frame_running&&!gt.cleanup&&!gt.operation);}
static void paused_copy(void){
    on_write=NULL;
    assert(!pthread_mutex_lock(&rendezvous));copy_entered=true;
    assert(!pthread_cond_broadcast(&changed));
    while(!copy_release)assert(!pthread_cond_wait(&changed,&rendezvous));
    assert(!pthread_mutex_unlock(&rendezvous));
}
static void *race_frame(void *arg){w32_display_call(&gt,arg);return NULL;}
static void *race_revoke(void *arg){
    (void)arg;revoking_thread=true;
    assert(display_present_revoke_kernel()==1);
    assert(!pthread_mutex_lock(&rendezvous));revoke_done=true;
    assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&rendezvous));return NULL;
}
static void native_formats(void){
    uint8_t *expected=malloc(sizeof(scanout));assert(expected);
    for(unsigned format=1;format<=4;format++)for(unsigned n=1;n<=3;n++){
        struct ciuki_w32_display_args a=grant_args();a.scale=n;
        unsigned bytes=cvp_surface_bytes(format);
        a.surface=(struct ciuki_present_surface){7,5,7*bytes+3,(7*bytes+3)*5,format};
        for(unsigned i=0;i<a.surface.size;i++)user[i]=(uint8_t)(i*19);
        w32_display_call(&dt,&a);assert(!a.error);
        struct ciuki_present_gamma ramp;
        for(unsigned i=0;i<256;i++){ramp.red[i]=(uint16_t)((255-i)*257);ramp.green[i]=(uint16_t)(i*257);ramp.blue[i]=(uint16_t)(i*257);}
        memcpy(user+65000,&ramp,sizeof(ramp));
        struct ciuki_w32_display_args q=holder_args(&a,W32_DISPLAY_GAMMA);q.address=65000;
        w32_display_call(&gt,&q);assert(!q.error);
        q=holder_args(&a,W32_DISPLAY_FRAME);w32_display_call(&gt,&q);assert(!q.error);
        cvp_format f={.width=640,.height=480,.pitch=2560,.bytes=4,.red_size=8,.red_pos=16,.green_size=8,.green_pos=8,.blue_size=8};
        assert(!cvp_surface_blit(&a.surface,user,palette,&ramp,&f,n,NULL,expected,sizeof(scanout)));
        assert(!memcmp(scanout,expected,sizeof(scanout)));
        assert(display_present_revoke_kernel()==1);complete();
    }
    free(expected);puts("PASS production frame tiling: four formats, three scales, gamma, asymmetric borders and tile boundaries");
}
static void race(void){
    struct ciuki_w32_display_args a=grant_args();w32_display_call(&dt,&a);assert(!a.error);
    struct ciuki_w32_display_args q=holder_args(&a,W32_DISPLAY_FRAME);
    on_write=paused_copy;pthread_t renderer;
    assert(!pthread_create(&renderer,NULL,race_frame,&q));
    assert(!pthread_mutex_lock(&rendezvous));
    while(!copy_entered)assert(!pthread_cond_wait(&changed,&rendezvous));
    racing=true;
    assert(!pthread_create(&revoker,NULL,race_revoke,NULL));
    while(!revoke_attempted)assert(!pthread_cond_wait(&changed,&rendezvous));
    assert(!revoke_done);copy_release=true;
    assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&rendezvous));
    assert(!pthread_join(revoker,NULL));unsigned count=writes;
    assert(!pthread_join(renderer,NULL));assert(writes==count);
    assert(!q.error||q.error==EACCES);racing=false;complete();
    puts("PASS pthread revocation race: revoke waits for paused native write; zero writes after return");
}
static void render3d_frames(void)
{
    uint32_t pixels[64*64];
    for (unsigned i=0;i<64*64;i++) pixels[i]=0xff123456;
    for (unsigned schedule=0;schedule<5;schedule++) {
        game.state=PROC_LIVE;revoked_writes=0;
        struct ciuki_w32_display_args a=grant_args();
        a.surface=(struct ciuki_present_surface){64,64,256,sizeof(pixels),CIUKI_PRESENT_XRGB8888};
        a.scale=1; w32_display_call(&dt,&a); assert(!a.error);
        struct ciuki_w32_render3d_args r={.token=a.token,.generation=a.generation,
            .width=64,.height=64,.pitch=256};
        unsigned before=writes;
        if (schedule==1) r.token++;
        if (schedule==2) on_yield=revoke_now;
        if (schedule==3) on_yield=die_now;
        if (schedule==4) r.pitch++;
        int e=display_present_render3d(0,3,&r,pixels,sizeof(pixels));
        if (!schedule) assert(!e && writes>before && !awaiting_first_frame);
        else if (schedule==4) assert(e==-EOPNOTSUPP && writes==before);
        else assert(e==-EACCES);
        if (active) assert(display_present_revoke_kernel()==1);
        before=writes;
        assert(display_present_render3d(0,3,&r,pixels,sizeof(pixels))==-EACCES && writes==before);
        assert(!suspended && device.mode_id==0x118); complete();
    }
    game.state=PROC_LIVE;
    puts("PASS render3d common presenter: completed frame, token/layout refusal, mid-frame revoke/death, desktop restoration, zero writes after revoke");
}
int main(void)
{
    memset(user,1,sizeof(user));
    struct ciuki_w32_display_args a=grant_args();connected=false;w32_display_call(&dt,&a);assert(a.error==EPERM&&!mode_calls);connected=true;
    a=grant_args();w32_display_call(&gt,&a);assert(a.error==EPERM&&!mode_calls);
    a=grant_args();mode_fail=true;w32_display_call(&dt,&a);assert(a.error==EIO&&!active&&!suspended);
    strcpy(g_boot.test_request,"f5:q3-lease-eio run=1234abcd");
    g_boot.test_request_len=sizeof("f5:q3-lease-eio run=1234abcd")-1;
    g_boot.flags=CBI_F_SMBIOS_QEMU|CBI_F_TEST_REQUEST;
    unsigned before_modes=mode_calls;
    for (unsigned i=0;i<3;i++) {
        a=grant_args();w32_display_call(&dt,&a);
        assert(a.error==EIO && !a.result && !a.token && !a.generation);
        assert(!active && !transfer && !suspended && !frame_running);
        assert(device.mode_id==0x118 && mode_calls==before_modes && !writes);
        display_present_poll();complete();
    }
    g_boot.flags=CBI_F_TEST_REQUEST;assert(!supervisor_interactive_test(&g_boot));
    g_boot.flags=CBI_F_SMBIOS_QEMU;assert(!supervisor_interactive_test(&g_boot));
    g_boot.flags=CBI_F_SMBIOS_QEMU|CBI_F_TEST_REQUEST;
    g_boot.test_request_len--;assert(!supervisor_interactive_test(&g_boot));
    g_boot.flags=0;
    puts("PASS fx-q3hang three EIO grants: rollback, desktop resumed, no held lock/frame, runtime QEMU-only fault gating");
    a=grant_args();a.scale=3;w32_display_call(&dt,&a);assert(a.error==EOPNOTSUPP&&!active&&!suspended&&device.mode_id==0x118);
    struct ciuki_display_info modes={.mode_count=3};
    modes.modes[0]=(struct ciuki_display_mode){.id=0x115,.width=800,.height=600,.bpp=16,.pitch=1600,.linear=1};
    modes.modes[1]=(struct ciuki_display_mode){.id=0x111,.width=640,.height=480,.bpp=16,.pitch=1280,.linear=1};
    modes.modes[2]=(struct ciuki_display_mode){.id=0x112,.width=640,.height=480,.bpp=32,.pitch=2560,.linear=1};
    struct ciuki_present_surface request={640,480,0,0,CIUKI_PRESENT_XRGB8888};
    assert(ciuki_present_choose_mode(&modes,&request)==2);
    request.width=320;request.height=240;request.format=CIUKI_PRESENT_INDEX8;
    assert(ciuki_present_choose_mode(&modes,&request)==1);
    request.height=200;assert(ciuki_present_choose_mode(&modes,&request)==1);
    request.width=1024;assert(ciuki_present_choose_mode(&modes,&request)==-ENODEV);
    uint32_t last=0;
    for(unsigned schedule=0;schedule<9;schedule++){
        game.state=PROC_LIVE;revoked_writes=0;
        a=grant_args();w32_display_call(&dt,&a);assert(!a.error&&a.result&&a.token>last&&active&&suspended);last=a.token;
        struct ciuki_w32_display_args q=holder_args(&a,W32_DISPLAY_PALETTE);q.first=1;q.count=1;q.address=65000;
        uint32_t red=0xff0000;memcpy(user+65000,&red,4);w32_display_call(&gt,&q);assert(!q.error);
        q=holder_args(&a,W32_DISPLAY_FRAME);q.token--;unsigned before=writes;w32_display_call(&gt,&q);assert(q.error==EACCES&&writes==before);
        q=holder_args(&a,W32_DISPLAY_FRAME);
        if(schedule==0)on_pin=revoke_now;
        if(schedule==1)on_read=revoke_now;
        if(schedule==2)on_yield=revoke_now;
        if(schedule==3){next_frame=(g_ticks+17)*UINT64_C(1000000);on_sleep=revoke_now;}
        if(schedule==4)on_yield=die_now;
        if(schedule==5)pin_fail=true;
        if(schedule==6)allocation_fail=true;
        if(schedule==7)read_fail=true;
        w32_display_call(&gt,&q);
        if(schedule<5)assert(q.error==EACCES);
        if(schedule==5||schedule==7)assert(q.error==EFAULT);
        if(schedule==6)assert(q.error==ENOMEM);
        if(schedule==8){assert(!q.error);assert(scanout[40*2560+2]==255&&scanout[0]==0);}
        pin_fail=allocation_fail=read_fail=false;
        if(active)assert(display_present_revoke_kernel()==1);
        unsigned after=writes;q=holder_args(&a,W32_DISPLAY_FRAME);w32_display_call(&gt,&q);assert(q.error==EACCES&&writes==after);
        assert(!suspended&&!active&&device.mode_id==0x118);complete();
    }
    game.state=PROC_LIVE;revoked_writes=0;a=grant_args();w32_display_call(&dt,&a);assert(!a.error);
    struct ciuki_w32_display_args q=holder_args(&a,W32_DISPLAY_REVOKE);q.pid=3;
    w32_display_call(&gt,&q);assert(q.error==EPERM&&active);
    q=holder_args(&a,W32_DISPLAY_REVOKE);q.token--;w32_display_call(&dt,&q);assert(q.error==EACCES&&active);
    q=holder_args(&a,W32_DISPLAY_REVOKE);w32_display_call(&dt,&q);assert(!q.error&&!active);
    complete();
    a=grant_args();w32_display_call(&dt,&a);assert(!a.error);
    g_ticks+=9999;display_present_poll();assert(active);
    g_ticks++;display_present_poll();assert(!active&&!suspended);complete();
    a=grant_args();w32_display_call(&dt,&a);assert(!a.error);
    connected=false;display_present_poll();assert(!active&&!suspended);connected=true;complete();
    a=grant_args();w32_display_call(&dt,&a);assert(!a.error);
    q=holder_args(&a,W32_DISPLAY_FRAME);unsigned prior=writes;grant_available=false;
    w32_display_call(&gt,&q);assert(q.error==EACCES&&writes==prior);grant_available=true;
    assert(display_present_revoke_kernel()==1);complete();
    native_formats();race();render3d_frames();printf("PASS production lease: revoke at pin/read/copy/pacing, death, stale tokens, authority, mode failure/rollback, cleanup; writes=%u returns=%u\n",writes,returns);
    return 0;
}
#ifdef DESKTOP_ADAPTER_FIXTURE
/* Export the very same f5-08 fixture to the desktop translation units. */
uint32_t desk_w32_record(void *record,uint32_t bytes)
{
    assert(bytes==sizeof(struct ciuki_w32_display_args));struct ciuki_w32_display_args *a=record;
    if(a->action==W32_DISPLAY_FRAME){
        assert(a->address && a->address>sizeof(user));memcpy(user,(const void *)(uintptr_t)a->address,64000);a->address=0;
        return w32_display_call(&gt,a);
    }
    return w32_display_call(&dt,a);
}
int presenter_desktop_info(int fd,struct ciuki_framebuffer_info *out)
{(void)fd;if(suspended)return -EBUSY;*out=(struct ciuki_framebuffer_info){.generation=device.generation,.width=800,.height=600};return 0;}
void presenter_adapter_init(void)
{assert(!active);game.pid=42;game.state=PROC_LIVE;connected=grant_available=true;revoked_writes=0;}
void presenter_adapter_failure(int kind)
{if(kind==1)mode_fail=true;else if(kind==2)connected=false;}
int presenter_adapter_suspended(void){return suspended;}
void presenter_adapter_timeout(void){g_ticks+=10000;display_present_poll();}
void presenter_adapter_frame(void)
{
    struct ciuki_w32_display_args a={.action=W32_DISPLAY_FRAME,.token=token,.generation=fb_generation};
    w32_display_call(&gt,&a);assert(!a.error && !awaiting_first_frame);
}
#endif
