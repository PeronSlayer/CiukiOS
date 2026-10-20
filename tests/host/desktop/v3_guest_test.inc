/* Same raw-ABI peer and native monitor as the guest; host kernel/channel/VFS
 * boundaries are explicit. Controller summaries are validated before framing. */
#include "../../../src/probes/desktop_v3/controller.h"
#include "../../../apps/desktop/win32_adapter.h"
#include "../../../apps/w32srv/registry.h"
#include <setjmp.h>
#include <ciuki/spawn.h>
#include <ciuki/present_abi.h>
uint32_t desk_w32_record(void *,uint32_t);
#include <stdarg.h>
static struct desktop_v3_observation guest_observation;
static struct wr_server guest_registry;
static unsigned guest_ms,guest_read,guest_surface,guest_frames,guest_seq;
static unsigned guest_registered,guest_spawns,guest_roundtrips;
static jmp_buf guest_exit;
static void guest_emit(const char *probe,const char *event,const char *fmt,...)
{
    printf("CIUKI_TEST v=1 run=12345678 seq=%06u probe=%s event=%s ",++guest_seq,probe,event);
    va_list ap;va_start(ap,fmt);vprintf(fmt,ap);va_end(ap);putchar('\n');
}
void desk_v3_fixture_report(const char *line)
{
    struct desktop_v3_observation r;
    assert(sscanf(line,"case=desktop-v3 phase=%u generation=%u windows=%u errors=%u child=%u",
        &r.phase,&r.generation,&r.windows,&r.errors,&r.pid)==5);
    if(!desktop_v3_observe(&guest_observation,&r,42)){fprintf(stderr,"rejected report: %s previous=%u\n",line,guest_observation.phase);abort();}desktop_v3_publish(&r,guest_emit);
    if(r.phase==2)guest_emit("crash-isolation","ARM","server=desktop action=desktop_v3");
}
int ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *maps,unsigned count,unsigned flags)
{
    (void)env;assert(!strcmp(path,"/System/Win32/Fixtures/desktop-v3.exe") && !strcmp(argv[0],path));
    assert(guest_registered && guest_roundtrips>=2);guest_spawns++;
    assert(!maps && !count && !flags);return 42;
}
uint32_t desk_w32_exchange(const void *in,uint32_t n,void *out,uint32_t cap,uint32_t *used)
{if(!guest_registered)return 1062;guest_roundtrips++;size_t bytes=0;uint32_t e=wr_dispatch(&guest_registry,2,in,n,out,cap,&bytes);*used=(uint32_t)bytes;return e;}
int desk_w32_native_shortcut(struct desktop *p,const struct desk_launch_record *r)
{
    assert(p==&d && !strcmp(r->target,"/Programs/control") && !strcmp(r->arguments,"--panel=programs"));
    /* Native launch ownership is independently replayed by win32_supervision_test.
     * This boundary supplies the Control Panel's first painted window. */
    int root=peer(11,84),c=create(root,40,1,1,0,0);
    d.clients[c].program=DESK_APP_CONTROL;d.clients[c].awaiting_frame=0;
    return 84;
}
int desk_v3_fixture_begin(struct desktop *);
void desk_v3_fixture_tick(struct desktop *);
void desktop_v3_peer_entry(void);
intptr_t desktop_v3_host_call(unsigned op,uintptr_t b,uintptr_t c,uintptr_t arg)
{
    switch(op) {
    case CIUKI_SYS_CLOCK_GETTIME: {
        struct ciuki_timespec *t=(void *)c;t->tv_sec=guest_ms/1000;t->tv_nsec=(int)(guest_ms%1000)*1000000;return 0;
    }
    case CIUKI_SYS_NANOSLEEP:
        guest_ms++;d.now_ns=(uint64_t)guest_ms*1000000;
        desk_v3_fixture_tick(&d);
        if(guest_observation.phase==2 && guest_frames==1){key(0x38,1);key(0x0f,1);key(0x0f,0);key(0x38,0);desk_v3_fixture_tick(&d);}
        else if(guest_observation.phase==3){key(0x38,1);key(0x0f,1);key(0x0f,0);key(0x38,0);desk_v3_fixture_tick(&d);}
        flush();
        if(guest_observation.phase==8)longjmp(guest_exit,1);
        return 0;
    case CIUKI_SYS_W32: {
        struct ciuki_w32_header *h=(void *)b;
        if(h->operation==0x200){struct w32r_service *s=(void *)b;s->endpoint=10;s->result=1;return 0;}
        int e=(int)desk_w32_record((void *)b,(uint32_t)c);
        if(!e && !((struct ciuki_w32_display_args *)b)->error)guest_frames++;
        return e;
    }
    case CIUKI_SYS_SURFACE_CREATE: {
        int surface=(int)++guest_surface;fake_surface(surface,(uint32_t)b,(uint32_t)c,0);return surface;
    }
    case CIUKI_SYS_SURFACE_MAP: {
        void *pixels=fake_ops.map((int)b,PROT_READ);struct ciuki_surface_info info;assert(!fake_ops.info((int)b,&info));
        fake_ops.unmap(pixels,info.allocation_bytes);return (intptr_t)pixels;
    }
    case CIUKI_SYS_CLOSE:return 0; /* translated transferred descriptor retained by desktop */
    case CIUKI_SYS_CHANNEL_SEND: {
        assert(b==10 && arg==DONTWAIT);struct ciuki_message m=*(const struct ciuki_message *)c;m.sender_pid=42;
        int root=-1;for(int i=0;i<DESK_CLIENTS;i++)if(d.clients[i].channel==10 && !d.clients[i].v3_peer)root=i;
        if(root<0){fake_channel(10);root=desk_add_client(&d,10);assert(root>=0);}
        assert(!desk_v3_receive(&d,root,&m));flush();return 0;
    }
    case CIUKI_SYS_CHANNEL_RECV:
        while(guest_read<fake_sent){unsigned i=guest_read++;if(fake_destinations[i]==10){memcpy((void *)c,&fake_messages[i],sizeof(struct ciuki_message));return 1;}}
        return -EAGAIN;
    case CIUKI_SYS_EXIT:fprintf(stderr,"guest peer exit=%lu phase=%u\n",(unsigned long)b,guest_observation.phase);assert(!b);longjmp(guest_exit,2);
    default:assert(0);return -EINVAL;
    }
}
static void guest_fixture_run(const char *dir)
{
    setup();presenter_adapter_init();d.v3_lease=real_lease;d.display_info=presenter_desktop_info;d.v3_restart=restart;
    d.ops.present=ordered_present;guest_surface=19;
    struct wr_io io;wr_posix_io(&io,dir);assert(wr_init(&guest_registry,&io,0));
    guest_emit("crash-isolation","BEGIN","server=desktop");
    assert(desk_v3_fixture_begin(&d)==0 && !guest_spawns);
    desk_v3_fixture_tick(&d);assert(!guest_spawns);
    guest_registered=1;desk_v3_fixture_tick(&d);assert(guest_spawns==1);
    if(!setjmp(guest_exit))desktop_v3_peer_entry();
    assert(guest_observation.phase==8 && guest_frames==2 && !d.v3_lease_state);
    struct desktop_v3_observation bad=guest_observation;bad.phase=8;
    assert(!desktop_v3_observe(&guest_observation,&bad,42));bad.phase=9;assert(!desktop_v3_observe(&guest_observation,&bad,42));
    guest_emit("crash-isolation","END","server=desktop status=PASS");
    wr_destroy(&guest_registry);cleanup();
    puts("PASS authored guest fixture on host: PE peer body, production v3 server/presenter/native monitor/registry, trusted controller grammar; guest not_run");
}
