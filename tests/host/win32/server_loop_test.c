/* Replay the production guest main loop with a deterministic clock/transport.
 * Registry, session/power coordination and packet dispatch remain production.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include "../../../apps/w32srv/registry.h"
#include "../../../apps/w32srv/shell.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <ciuki/abi.h>
#pragma clang diagnostic pop
#include "../../../sdk/sysroot-overlay/include/ciuki/display.h"

/* Replace only the native syscall gateway, clock and fake descriptor close.
 * The Linux branch of the guest loop omits the native credential plumbing. */
#define CIUKI_RUNTIME_H
#define CU_PTR(p) ((uintptr_t)(p))
#define CU_CALL(n,b,c,d,s,i,p) loop_syscall(CIUKI_SYS_##n,(uintptr_t)(b),(c),(d),(s),(i),(p))
static uint32_t loop_syscall(uint32_t,uintptr_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
static int loop_clock(clockid_t,struct timespec *);
static int loop_close(int);
#define clock_gettime loop_clock
#define close loop_close
#define main w32srv_main
#include "../../../apps/w32srv/main.c"
#undef main
#undef close
#undef clock_gettime
#undef __CIUKIOS__

static struct wr_server *server;
static uint64_t clock_ms, request_time;
static unsigned waits, packets, notifications, mode, connection;
static struct w32r_packet request_packet;
static int loop_clock(clockid_t id,struct timespec *t)
{
    assert(id==CLOCK_MONOTONIC);
    t->tv_sec=(time_t)(clock_ms/1000);t->tv_nsec=(long)(clock_ms%1000)*1000000;
    return 0;
}
static int loop_close(int fd){assert(fd==123);return 0;}
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
void ws_posix_fs(struct ws_fs *fs){memset(fs,0,sizeof(*fs));}
struct ws_shell *ws_create(const struct ws_fs *fs,const struct ws_config *c,struct wr_server *s)
{
    (void)fs;(void)c;server=s;
    uint32_t pid=(uint32_t)getppid();
    assert(!wr_client_join(s,pid));
    struct w32r_request a={.op=W32R_WINDOW_ALLOC,.index=11};
    struct w32r_reply r={0};uint8_t data[W32R_NAME+24];
    assert(!wr_directory_call(s,pid,&a,"",NULL,&r,data) && r.key);
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
static void prepare_request(void)
{
    struct {struct w32r_request r;char empty[2];} a={
        .r={.version=1,.op=W32R_ENDSESSION,.index=W32R_SESSION_BEGIN,
            .path_bytes=1,.name_bytes=1}};
    if(mode==1)a.r.index=W32R_POWER_BEGIN;
    if(mode==2){a.r.op=W32R_SHUTDOWN;a.r.index=W32R_SHUTDOWN_REQUEST;a.r.type=15;}
    request_packet=(struct w32r_packet){.version=1,.id=1,.kind=W32R_BEGIN,.total=sizeof(a.r)+2};
    memcpy(request_packet.bytes,&a,request_packet.total);
}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{
    assert(fd==123 && flags==1);
    if(waits!=1 || packets){errno=EAGAIN;return -1;}
    /* Transport work can itself take time: dispatch must refresh after it. */
    clock_ms+=17;request_time=clock_ms;
    memset(m,0,sizeof(*m));m->length=256;m->sender_pid=(uint32_t)getppid();
    memcpy(m->data,&request_packet,256);packets++;return 256;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{
    assert(fd==123 && flags==1 && m->length==256);
    struct w32r_packet p;memcpy(&p,m->data,256);assert(!p.error);
    /* Reading the completed transaction below must succeed too. */
    assert(p.total==sizeof(struct w32r_reply));
    return 256;
}
static uint32_t loop_syscall(uint32_t call,uintptr_t address,uint32_t bytes,
                             uint32_t d,uint32_t si,uint32_t di,uint32_t bp)
{
    assert(call==CIUKI_SYS_W32 && bytes==sizeof(struct w32r_service));
    assert(!d&&!si&&!di&&!bp);
    struct w32r_service *a=(void *)address;
    if(a->action==W32_SVC_REGISTER){a->result=1;return 0;}
    if(a->action==W32_SVC_ACCEPT){
        if(!connection++){a->pid=(uint32_t)getppid();a->endpoint=123;a->result=1;}
        return 0;
    }
    if(a->action==W32_SVC_NOTIFY){
        assert(a->pid==(uint32_t)getppid() && a->endpoint==11);notifications++;return 0;
    }
    assert(a->action==W32_SVC_WAIT);
    if(!waits++){
        assert(a->pid==W32_INFINITE);
        clock_ms+=60000;return 0; /* the idle service really sleeps */
    }
    assert(packets==1 && server->now==request_time);
    if(mode==2){
        assert(server->session.due==request_time+15000);
        assert(!notifications);
        puts("PASS main-loop shutdown countdown=15000 ms after 60000 ms idle");
    }else{
        uint64_t window=mode==1?20000:10000;
        assert(wr_deadline(server)==request_time+window && a->pid==window);
        assert(notifications==1);
        /* At the end of the window the undelivered turn expires, not before. */
        server->now=request_time+window-1;wr_session_wake(server);
        assert(wr_deadline(server)==request_time+window && notifications==1);
        server->now++;wr_session_wake(server);assert(wr_deadline(server)==UINT64_MAX);
        printf("PASS main-loop %s window=%llu ms after 60000 ms idle, one notification\n",
               mode==1?"power":"session",(unsigned long long)window);
    }
    stopping=1;return 0;
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    assert(argc==2);mode=(unsigned)strtoul(argv[1],NULL,10);assert(mode<3);
    clock_ms=100;prepare_request();
    char *args[]={"w32srv",NULL};
    assert(!w32srv_main(1,args));assert(waits==2 && packets==1);
    puts("TRACE fx-w32idle server idle requests=0 runnable_grace_ms=0 sleep_ms=60000 wait_enrollments=1");
    return 0;
}
