/* Production session/power coordination and guest loop, deterministic I/O.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../../../apps/w32srv/registry.h"
#include "../../../apps/w32srv/shell.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <ciuki/abi.h>
#pragma clang diagnostic pop
#include "../../../sdk/sysroot-overlay/include/ciuki/display.h"

/* Use the production guest loop; replace only its platform adapters. */
#define CIUKI_RUNTIME_H
#define CU_PTR(p) ((uintptr_t)(p))
#define CU_CALL(n,b,c,d,s,i,p) turns_syscall(CIUKI_SYS_##n,(uintptr_t)(b),(c),(d),(s),(i),(p))
static uint32_t turns_syscall(uint32_t,uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
static int turns_clock(clockid_t,struct timespec *);
static int turns_close(int);
#define clock_gettime turns_clock
#define close turns_close
#define main w32srv_main
#include "../../../apps/w32srv/main.c"
#undef main
#undef close
#undef clock_gettime
#undef __CIUKIOS__

static struct wr_server *server;
static uint64_t clock_ms, session_deadline;
static uint32_t owner, window_a, window_b;
static unsigned mode, waits, connection, release_pending, packets;
static unsigned signals[2], latch[2];

static int turns_clock(clockid_t id,struct timespec *t)
{
    assert(id==CLOCK_MONOTONIC);
    t->tv_sec=(time_t)(clock_ms/1000);t->tv_nsec=(long)(clock_ms%1000)*1000000;
    return 0;
}
static int turns_close(int fd){assert(fd==123);return 0;}
static int empty_read(void *o,const char *n,uint8_t **p,size_t *len)
{(void)o;(void)n;(void)p;(void)len;return 0;}
static int discard_write(void *o,const char *n,const void *p,size_t len)
{(void)o;(void)n;(void)p;(void)len;return 0;}
static int discard_patch(void *o,const char *n,size_t at,const void *p,size_t len)
{(void)at;return discard_write(o,n,p,len);}
static int discard_sync(void *o,const char *n){(void)o;(void)n;return 0;}
static void log_line(void *o,const char *line){(void)o;puts(line);}
void wr_posix_io(struct wr_io *io,const char *dir)
{
    (void)dir;
    *io=(struct wr_io){NULL,empty_read,discard_write,discard_patch,discard_sync,log_line};
}
static void notify(uint32_t pid,uint32_t tid)
{
    assert(pid==owner && (tid==11 || tid==22));
    unsigned i=tid==22;signals[i]++;latch[i]=1;
}
static struct w32r_reply request(uint32_t index,uint32_t tid,uint32_t key,uint32_t flags,
                                const void *data,uint32_t bytes)
{
    struct w32r_request a={.version=1,.op=W32R_ENDSESSION,.index=index,
        .type=tid,.key=key,.flags=flags,.path_bytes=1,.name_bytes=1,.data_bytes=bytes};
    uint8_t input[sizeof(a)+2+8]={0},output[sizeof(struct w32r_reply)];size_t used=0;
    assert(bytes<=8);memcpy(input,&a,sizeof(a));
    if(bytes)memcpy(input+sizeof(a)+2,data,bytes);
    assert(!wr_dispatch(server,owner,input,sizeof(a)+2+bytes,output,sizeof(output),&used));
    struct w32r_reply r;assert(used==sizeof(r));memcpy(&r,output,sizeof(r));assert(!r.error);
    return r;
}
static uint32_t window(uint32_t tid)
{
    struct w32r_request a={.op=W32R_WINDOW_ALLOC,.index=tid};
    struct w32r_reply r={0};uint8_t data[W32R_NAME+24];
    assert(!wr_directory_call(server,owner,&a,"",NULL,&r,data) && r.key);
    return r.key;
}
static void begin_session(uint32_t flags)
{
    uint32_t order[2]={window_a,owner};
    request(W32R_SESSION_BEGIN,0,0,flags,order,sizeof(order));
}
static void answer(uint32_t tid,uint32_t hwnd,uint32_t message)
{
    unsigned i=tid==22;assert(latch[i]);latch[i]=0;
    struct w32r_reply r=request(W32R_SESSION_POLL,tid,0,0,NULL,0);
    assert(r.key==hwnd && r.type==message);
    request(W32R_SESSION_REPLY,tid,hwnd,1,NULL,0);
}
static void test_replacement(void)
{
    struct wr_server s;struct wr_io io;wr_posix_io(&io,"");
    assert(wr_init(&s,&io,clock_ms));server=&s;s.compat_writer_pid=owner;
    assert(!wr_client_join(&s,owner));s.session_notify=notify;
    window_b=window(22);request(W32R_POWER_BEGIN,0,0,0,NULL,0);
    assert(signals[1]==1 && latch[1]);
    window_a=window(11);begin_session(mode<2?W32R_SESSION_HOLD:0);
    assert(signals[0]==1 && signals[1]==1);
    /* B consumes its power latch while the session belongs to A. */
    latch[1]=0;
    assert(!request(W32R_SESSION_POLL,22,0,0,NULL,0).key);
    for(unsigned i=0;i<3;i++)wr_session_wake(&s);
    assert(signals[0]==1 && signals[1]==1 && !latch[1]);
    unsigned before=signals[1];
    if(mode==0)request(W32R_SESSION_RESET,0,0,0,NULL,0);
    else if(mode==1)request(W32R_SESSION_CANCEL,0,0,0,NULL,0);
    else if(mode==2){
        answer(11,window_a,0x11);answer(22,window_b,0x11);
        answer(11,window_a,0x16);before=signals[1];
        answer(22,window_b,0x16);
    }else{
        assert(mode==3);s.now=clock_ms+10000;wr_session_wake(&s);
    }
    assert(signals[1]==before+1 && latch[1]);
    for(unsigned i=0;i<3;i++)wr_session_wake(&s);
    assert(signals[1]==before+1); /* one replacement, no periodic signals */
    assert(s.now<clock_ms+20000); /* still inside the original power window */
    answer(22,window_b,W32R_POWERBROADCAST);
    struct w32r_reply r=request(W32R_POWER_STATUS,0,0,0,NULL,0);
    assert(r.key==1 && r.count==1 && !latch[1]);
    printf("PASS replacement mode=%u signals=1 delivered_before_expiry=1\n",mode);
    wr_destroy(&s);
}

void ws_posix_fs(struct ws_fs *fs){memset(fs,0,sizeof(*fs));}
struct ws_shell *ws_create(const struct ws_fs *fs,const struct ws_config *c,struct wr_server *s)
{
    (void)fs;(void)c;server=s;
    assert(!wr_client_join(s,owner));
    window_b=window(22);request(W32R_POWER_BEGIN,0,0,0,NULL,0);window_a=window(11);
    clock_ms=20000;s->now=clock_ms;
    begin_session(mode==5?0:W32R_SESSION_HOLD);session_deadline=clock_ms+10000;
    clock_ms=20101; /* power expired, session still holds priority */
    return NULL;
}
void ws_destroy(struct ws_shell *s){assert(!s);}
void ws_profile(void *s,const char *p){(void)s;(void)p;}
void ws_exit(void *s,uint32_t pid){(void)s;(void)pid;}
uint32_t ws_call(void *s,uint32_t pid,const struct w32r_request *a,const char *p,
                 const char *n,struct w32r_reply *r,uint8_t *b,size_t len)
{(void)s;(void)pid;(void)a;(void)p;(void)n;(void)r;(void)b;(void)len;return 120;}
int ws_restart_tick(struct ws_shell *s){(void)s;return 0;}
int ciuki_display_query(struct ciuki_display_info *d)
{d->width=800;d->height=600;d->bpp=32;return 0;}
int ciuki_proc_list(void *buffer,uint32_t bytes)
{
    assert(bytes>=sizeof(struct ciuki_proc_system));
    struct ciuki_proc_system *s=buffer;s->version=CIUKI_PROC_VERSION;s->records=0;return 0;
}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{
    assert(fd==123 && flags==1);
    if(!release_pending){errno=EAGAIN;return -1;}
    release_pending=0;packets++;
    struct w32r_request a={.version=1,.op=W32R_ENDSESSION,.index=W32R_SESSION_RESET,
        .path_bytes=1,.name_bytes=1};
    struct w32r_packet p={.version=1,.id=1,.kind=W32R_BEGIN,.total=sizeof(a)+2};
    memcpy(p.bytes,&a,sizeof(a));
    memset(m,0,sizeof(*m));m->length=256;m->sender_pid=owner;memcpy(m->data,&p,256);
    return 256;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    assert(fd==123 && flags==1 && m->length==256);
    struct w32r_packet p;memcpy(&p,m->data,256);assert(!p.error && p.total==sizeof(struct w32r_reply));
    return 256;
}
static uint32_t turns_syscall(uint32_t call,uintptr_t address,uint32_t bytes,
                              uint32_t d,uint32_t si,uint32_t di,uint32_t bp)
{
    assert(call==CIUKI_SYS_W32 && bytes==sizeof(struct w32r_service));
    assert(!d&&!si&&!di&&!bp);
    struct w32r_service *a=(void *)address;
    if(a->action==W32_SVC_REGISTER){a->result=1;return 0;}
    if(a->action==W32_SVC_ACCEPT){
        if(!connection++){a->pid=owner;a->endpoint=123;a->result=1;}return 0;
    }
    if(a->action==W32_SVC_NOTIFY){notify(a->pid,a->endpoint);return 0;}
    assert(a->action==W32_SVC_WAIT);
    assert(++waits<=4); /* bounded failure even if the loop regresses to spinning */
    assert(server->now==clock_ms && a->pid!=0);
    if(waits==1){
        assert(a->pid==(mode==5?session_deadline-clock_ms:W32_INFINITE));
        assert(signals[0]==1 && !signals[1]);
        if(mode==6){uint32_t value=7;assert(!wr_put(server,server->roots[0],"IdleCommit",4,&value,4));}
        clock_ms+=100; /* one unrelated transport wake, still held */
    }else if(waits==2){
        if(mode==5){assert(a->pid==session_deadline-clock_ms);clock_ms=session_deadline;}
        else if(mode==6){assert(a->pid==1900);clock_ms+=a->pid;}
        else {assert(a->pid==W32_INFINITE);clock_ms+=1000;release_pending=1;}
    }else if(mode==6 && waits==3){
        assert(!server->hive[0].dirty && a->pid==W32_INFINITE);
        clock_ms+=1000;release_pending=1;
    }else{
        /* The same production iteration that releases priority must evaluate
         * the expired power deadline, rather than wait on it again. */
        assert(a->pid==W32_INFINITE && wr_deadline(server)==UINT64_MAX);
        assert(packets==(mode==5?0u:1u) && signals[0]==1 && !signals[1]);
        struct w32r_reply r=request(W32R_POWER_STATUS,0,0,0,NULL,0);
        assert(r.key==1 && r.count==1);
        printf("PASS main-loop overlap mode=%u waits=%u zero_time_waits=0 power_expired_on_release=1\n",mode,waits);
        stopping=1;
    }
    return 0;
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    assert(argc==2);mode=(unsigned)strtoul(argv[1],NULL,10);assert(mode<7);
    clock_ms=100;owner=(uint32_t)getppid();
    if(mode<4)test_replacement();
    else {char *args[]={"w32srv",NULL};assert(!w32srv_main(1,args));assert(waits==(mode==6?4u:3u));}
    return 0;
}
