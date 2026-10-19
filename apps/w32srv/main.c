/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "registry.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#ifdef __CIUKIOS__
#include <ciuki/runtime.h>
#include <ciuki/channel.h>
#include <ciuki/display.h>
#include <ciuki/proc.h>
#endif
static volatile sig_atomic_t stopping;
static void stop(int signo){(void)signo;stopping=1;}
static uint64_t now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000+(uint32_t)t.tv_nsec/1000000;}
#ifdef __CIUKIOS__
static int service(unsigned action,struct w32r_service *a)
{
    *a=(struct w32r_service){.version=1,.size=sizeof(*a),.operation=0x200,.action=action,.service=2};
    uint32_t r=CU_CALL(W32,CU_PTR(a),sizeof(*a),0,0,0,0);return r||a->error?-1:(int)a->result;
}
static struct {struct ciuki_proc_system system;struct ciuki_proc_record records[CIUKI_PROCESS_MAX];} snapshot;
static int alive(void *unused,uint32_t pid)
{
    (void)unused;
    for(uint32_t i=0;i<snapshot.system.records;i++)if(snapshot.records[i].pid==pid)
        return snapshot.records[i].state==CIUKI_PROC_LIVE||snapshot.records[i].state==CIUKI_PROC_STOPPING;
    return 0;
}
#endif
int main(int argc,char **argv)
{
    /* Also used by the image payload recipe to create fresh default hives.
     * The recipe runs a host build of this exact parser/writer, never ELF32. */
    int seed=argc==3&&!strcmp(argv[1],"--seed");
    const char *dir=seed?argv[2]:"/WINDOWS";
    struct wr_io io;wr_posix_io(&io,dir);struct wr_server s;
    if(!wr_init(&s,&io,now())){fprintf(stderr,"[win32] registry load-failed\n");return 1;}
    if(seed){uint32_t e=wr_flush(&s);wr_destroy(&s);return e?1:0;}
    s.compat_writer_pid=(uint32_t)getppid();
#ifdef __CIUKIOS__
    struct ciuki_display_info display={.version=CIUKI_DISPLAY_VERSION,.size=sizeof(display)};
    if(!ciuki_display_query(&display)&&!wr_defaults(&s,display.width,display.height,display.bpp)){wr_destroy(&s);return 1;}
    struct w32r_service a;
    int records=ciuki_proc_list(&snapshot,sizeof(snapshot));
    if(records<0||(uint32_t)records!=snapshot.system.records||snapshot.system.records>CIUKI_PROCESS_MAX||
       snapshot.system.version!=CIUKI_PROC_VERSION||wr_reconcile(&s,alive,NULL)){wr_destroy(&s);return 1;}
    if(wr_flush(&s)||service(1,&a)<0){wr_destroy(&s);return 1;}
    uint64_t display_deadline=now()+2000;
    static struct {int fd;struct wr_session session;} clients[64];
    for(unsigned i=0;i<64;i++){clients[i].fd=-1;memset(&clients[i].session,0,sizeof(clients[i].session));}
    signal(SIGTERM,stop);signal(SIGINT,stop);
    while(!stopping){
        while(service(3,&a)>0){
            unsigned i=0;while(i<64&&clients[i].fd>=0)i++;
            if(i==64){close((int)a.endpoint);continue;}
            clients[i].fd=(int)a.endpoint;clients[i].session.pid=a.pid;
        }
        for(unsigned i=0;i<64;i++)if(clients[i].fd>=0){
            struct ciuki_message m;int r=ciuki_channel_recv(clients[i].fd,&m,1);
            if(r<0&&errno==EAGAIN)continue;
            if(r<=0){wr_exit(&s,clients[i].session.pid);wr_session_close(&clients[i].session);close(clients[i].fd);clients[i].fd=-1;continue;}
            if(m.fd_count){for(unsigned j=0;j<m.fd_count;j++)close(m.fds[j]);}
            if(m.length!=256||m.fd_count||m.sender_pid!=clients[i].session.pid)continue;
            struct w32r_packet packet,reply;memcpy(&packet,m.data,256);wr_packet(&s,&clients[i].session,&packet,&reply);
            memset(&m,0,sizeof(m));m.length=256;memcpy(m.data,&reply,256);
            if(ciuki_channel_send(clients[i].fd,&m,1)<0){wr_exit(&s,clients[i].session.pid);wr_session_close(&clients[i].session);close(clients[i].fd);clients[i].fd=-1;}
        }
        uint64_t time=now();
        if(wr_tick(&s,time))fprintf(stderr,"[win32] registry periodic-commit-failed\n");
        if(time>=display_deadline){
            struct ciuki_display_info active={.version=CIUKI_DISPLAY_VERSION,.size=sizeof(active)};
            if(!ciuki_display_query(&active)&&(active.width!=display.width||active.height!=display.height||active.bpp!=display.bpp)){
                if(wr_defaults(&s,active.width,active.height,active.bpp))display=active;
            }
            display_deadline=time+2000;
        }
        struct timespec delay={.tv_sec=0,.tv_nsec=10000000};nanosleep(&delay,NULL);
    }
    for(unsigned i=0;i<64;i++)if(clients[i].fd>=0){wr_session_close(&clients[i].session);close(clients[i].fd);}
#else
    (void)stopping;(void)stop;
    fprintf(stderr,"w32srv: host binary supports --seed only\n");
#endif
    uint32_t error=wr_flush(&s);wr_destroy(&s);return error?1:0;
}
