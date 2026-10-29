/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _POSIX_C_SOURCE 200809L
#include "registry.h"
#include "shell.h"
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
#include <ciuki/security.h>
/* Private transport constants are not installed in the native SDK. */
#include "../../src/kernel/include/ciuki/w32_transport.h"
#endif
/* W32SRV_GUEST: the image's service, linked with libfont (build_w32srv.py);
 * host replays of this loop build without the font engine. */
#ifdef W32SRV_GUEST
#include <ciuki/raw.h>
#include "fonts.h"
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
static void session_notify(uint32_t pid,uint32_t tid)
{
    struct w32r_service a={.version=1,.size=sizeof(a),.operation=W32_SERVICE,
        .action=W32_SVC_NOTIFY,.service=W32_SERVICE_W32SRV,.pid=pid,.endpoint=tid};
    (void)CU_CALL(W32,CU_PTR(&a),sizeof(a),0,0,0,0);
}
static int wait_work(uint64_t deadline)
{
    uint64_t time=now(),left=deadline>time ? deadline-time : 0;
    struct w32r_service a={.version=1,.size=sizeof(a),.operation=W32_SERVICE,
        .action=W32_SVC_WAIT,.service=W32_SERVICE_W32SRV,
        .pid=deadline==UINT64_MAX ? W32_INFINITE : left>=W32_INFINITE ? W32_INFINITE-1 : (uint32_t)left};
    uint32_t status=CU_CALL(W32,CU_PTR(&a),sizeof(a),0,0,0,0);
    return status || (a.error && a.error!=995) ? -1 : 0;
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
extern void ws_profile(void *,const char *);
int main(int argc,char **argv)
{
    /* Also used by the image payload recipe to create fresh default hives.
     * The recipe runs a host build of this exact parser/writer, never ELF32. */
    int seed=argc==3&&!strcmp(argv[1],"--seed");
    const char *dir=seed?argv[2]:"/WINDOWS";
    struct wr_io io;wr_posix_io(&io,dir);struct wr_server s;
#if defined(__CIUKIOS__) && !defined(__linux__)
    char session_directory[160];
    int initialized=seed?wr_init(&s,&io,now()):wr_user_start(&s,&io,session_directory,now());
#else
    int initialized=wr_init(&s,&io,now());
#endif
    if(!initialized){fprintf(stderr,"[win32] registry load-failed\n");return 1;}
    if(seed){uint32_t e=wr_flush(&s);wr_destroy(&s);return e?1:0;}
    s.compat_writer_pid=(uint32_t)getppid();
    /* sec-user-03: launched by LOGON, the parent is LOGON; the writer is the
     * session's DESKTOP, bound below from a kernel-attested link. */
    const char *launch_purpose=getenv("CIUKI_LAUNCH_PURPOSE");
    int logon_launched=launch_purpose&&!strcmp(launch_purpose,"2");
    if(logon_launched)s.compat_writer_pid=0;
    const char *user=getenv("CIUKI_SESSION_USER"), *profile=getenv("CIUKI_SESSION_ROOT");
    if (!s.session_only && ((user && *user) || (profile && *profile))) {
        uint32_t e=wr_select_profile(&s,user ? user : "",profile ? profile : "");
        if (e) { fprintf(stderr,"[win32] profile load-failed error=%u\n",e); wr_destroy(&s); return 1; }
    }
    if(argc==2 && !strcmp(argv[1],"--fresh-clients") && wr_clients_reset(&s)){
        fprintf(stderr,"[win32] client identity reset-failed\n");wr_destroy(&s);return 1;
    }
#ifdef __CIUKIOS__
    struct ciuki_display_info display={.version=CIUKI_DISPLAY_VERSION,.size=sizeof(display)};
    /* Start failures are said on the console (io.log is the kernel debug
     * write; stderr is not collected): q3crash 2026-10-25 had 323 silent exits. */
    char failure[96];
    if(!ciuki_display_query(&display)&&!s.session_only&&!wr_defaults(&s,display.width,display.height,display.bpp)){
        io.log(io.opaque,"[win32] w32srv start-failed stage=defaults");wr_destroy(&s);return 1;
    }
    struct w32r_service a;
    int records=ciuki_proc_list(&snapshot,sizeof(snapshot));
    if(records<0||(uint32_t)records!=snapshot.system.records||snapshot.system.records>CIUKI_PROCESS_MAX||
       snapshot.system.version!=CIUKI_PROC_VERSION||wr_reconcile(&s,alive,NULL)){
        snprintf(failure,sizeof(failure),"[win32] w32srv start-failed stage=reconcile records=%d",records);
        io.log(io.opaque,failure);wr_destroy(&s);return 1;
    }
#ifdef W32SRV_GUEST
    /* f5-10C2 font bridge: system faces read-only, AddFontResource copies
     * under C:\WINDOWS\FONTS. A missing engine refuses font requests (120). */
    struct wf_config font_config={.system_dir="/System/fonts",.install_dir="/WINDOWS/FONTS"};
    struct wf_service *fonts=wf_create(&font_config);
    if(fonts){s.font_call=wf_call;s.fonts=fonts;}
    else fprintf(stderr,"[win32] fonts unavailable\n");
#endif
    /* f5-07B shell services: PROGMAN DDE effects and the virtual restart,
     * over the Start-menu folder and C:\WINDOWS. A failure leaves them off
     * (typed refusal 120) without stopping the registry. */
    struct ws_fs shell_fs;ws_posix_fs(&shell_fs);
    const struct ws_config shell_config={.programs="/WINDOWS/Start Menu/Programs",.windows="/WINDOWS"};
    struct ws_shell *shell=ws_create(&shell_fs,&shell_config,&s);
    if(shell){s.shell_call=ws_call;s.shell_exit=ws_exit;s.shell_profile=ws_profile;s.shell=shell;ws_profile(shell,s.user_path);}
    else fprintf(stderr,"[win32] shell services unavailable\n");
    /* The registration is refused (error 5) while another live w32srv holds
     * it: the dead desktop's orphan did, 323 times, in q3crash 2026-10-25. */
    uint32_t flush_error=wr_flush(&s);int registered=0;
    if(flush_error)snprintf(failure,sizeof(failure),"[win32] w32srv start-failed stage=flush error=%u",flush_error);
    else if((registered=service(1,&a))<0)snprintf(failure,sizeof(failure),"[win32] w32srv start-failed stage=register error=%u",a.error);
    if(flush_error||registered<0){
        io.log(io.opaque,failure);
#ifdef W32SRV_GUEST
        wf_destroy(fonts);
#endif
        ws_destroy(shell);wr_destroy(&s);return 1;
    }
    /* --- sec-user-03: launched by LOGON (SESSION_LAUNCH W32SRV), the hive is
     * loaded and the service registered: one 16-byte readiness message
     * {1, launch id, launch generation, 0} on the kernel's readiness
     * endpoint. Labels are descriptive; the kernel checks the sender. */
    {
        const char *fd_text=getenv("CIUKI_LAUNCH_READY_FD"),*id_text=getenv("CIUKI_LAUNCH_ID"),
                   *generation_text=getenv("CIUKI_LAUNCH_GENERATION"),*purpose=getenv("CIUKI_LAUNCH_PURPOSE");
        if(fd_text&&id_text&&generation_text&&purpose&&!strcmp(purpose,"2")){
            struct ciuki_message ready;memset(&ready,0,sizeof(ready));
            uint32_t words[4]={1,(uint32_t)strtoul(id_text,NULL,10),(uint32_t)strtoul(generation_text,NULL,10),0};
            memcpy(ready.data,words,16);ready.length=16;
            int sent=ciuki_channel_send(atoi(fd_text),&ready,DONTWAIT);
            snprintf(failure,sizeof(failure),"[win32] w32srv session ready launch=%u result=%d",words[1],sent<0?-errno:0);
            io.log(io.opaque,failure);
        }
    }
    /* --- sec-user-03 (end) --- */
    s.session_notify=session_notify;
    static struct {int fd;struct wr_session session;} clients[64];
    for(unsigned i=0;i<64;i++){clients[i].fd=-1;memset(&clients[i].session,0,sizeof(clients[i].session));}
    signal(SIGTERM,stop);signal(SIGINT,stop);
    /* Readiness covers both the next fragment and newly connected clients. */
    while(!stopping){
        int busy=0;
#if !defined(__linux__)
        /* --- sec-user-03: the session DESKTOP connects once to call-101
         * service 6; the ACCEPT record's peer role/session (kernel output)
         * names the end-session writer. No pid or name is trusted. --- */
        if(logon_launched){
            struct ciuki_credential own;
            for(unsigned n=0;n<4&&!ciuki_security_self(&own);n++){
                struct ciuki_security_link in={.version=1,.size=64,.service=CIUKI_SECURITY_SERVICE_W32SRV},out;
                memset(&out,0,sizeof(out));
                struct ciuki_security_args sa={.version=1,.size=64,.operation=CIUKI_SECURITY_SERVICE_ACCEPT,
                    .spawn_address=CU_PTR(&in),.output_address=CU_PTR(&out),.output_bytes=64};
                if(ciuki_security(&sa))break;
                if(out.peer_role==CIUKI_SECURITY_ROLE_DESKTOP&&out.session_id==own.session_id&&own.session_id)
                    s.compat_writer_pid=out.peer_pid;
                close(out.endpoint_fd);
            }
        }
#endif
        /* --- sec-user-03 (end) --- */
        while(service(3,&a)>0){
            unsigned i=0;while(i<64&&clients[i].fd>=0)i++;
            if(i==64){close((int)a.endpoint);continue;}
#if !defined(__linux__)
            struct ciuki_credential peer,self;
            if(ciuki_security_self(&self)||ciuki_security_peer((int)a.endpoint,&peer)||memcmp(&self,&peer,sizeof(peer))){close((int)a.endpoint);continue;}
#endif
            clients[i].fd=(int)a.endpoint;clients[i].session.pid=a.pid;
        }
        for(unsigned i=0;i<64;i++)if(clients[i].fd>=0){
#if !defined(__linux__)
            struct ciuki_credential peer,self;
            if(ciuki_security_self(&self)||ciuki_security_peer(clients[i].fd,&peer)||memcmp(&self,&peer,sizeof(peer))){
                wr_exit(&s,clients[i].session.pid);wr_session_close(&clients[i].session);close(clients[i].fd);clients[i].fd=-1;continue;
            }
#endif
            struct ciuki_message m;int r=ciuki_channel_recv(clients[i].fd,&m,1);
            if(r<0&&errno==EAGAIN)continue;
            if(r<=0){wr_exit(&s,clients[i].session.pid);wr_session_close(&clients[i].session);close(clients[i].fd);clients[i].fd=-1;continue;}
            if(m.fd_count){for(unsigned j=0;j<m.fd_count;j++)close(m.fds[j]);}
            if(m.length!=256||m.fd_count||m.sender_pid!=clients[i].session.pid)continue;
            struct w32r_packet packet,reply;memcpy(&packet,m.data,256);wr_packet(&s,&clients[i].session,&packet,&reply);busy=1;
            memset(&m,0,sizeof(m));m.length=256;memcpy(m.data,&reply,256);
            if(ciuki_channel_send(clients[i].fd,&m,1)<0){wr_exit(&s,clients[i].session.pid);wr_session_close(&clients[i].session);close(clients[i].fd);clients[i].fd=-1;}
        }
        uint64_t time=now();
        int restart_work=shell&&ws_restart_tick(shell); /* one WININIT.INI line per iteration */
        if(wr_tick(&s,time)){
            fprintf(stderr,"[win32] registry periodic-commit-failed\n");
            /* A failed write retries at the commit interval, never a zero-time loop. */
            for(unsigned h=0;h<2;h++)if(s.hive[h].dirty)s.hive[h].dirty_since=time;
        }
        if(busy){
            struct ciuki_display_info active={.version=CIUKI_DISPLAY_VERSION,.size=sizeof(active)};
            if(!ciuki_display_query(&active)&&(active.width!=display.width||active.height!=display.height||active.bpp!=display.bpp)){
                if(!s.session_only&&wr_defaults(&s,active.width,active.height,active.bpp))display=active;
            }
        }
        wr_session_wake(&s);
        if(restart_work)continue;
        if(wait_work(wr_deadline(&s))<0){io.log(io.opaque,"[win32] w32srv wait-failed");break;}
    }
    for(unsigned i=0;i<64;i++)if(clients[i].fd>=0){wr_session_close(&clients[i].session);close(clients[i].fd);}
#ifdef W32SRV_GUEST
    wf_destroy(fonts);
#endif
    ws_destroy(shell);
#else
    (void)stopping;(void)stop;
    fprintf(stderr,"w32srv: host binary supports --seed only\n");
#endif
    uint32_t error=wr_flush(&s);wr_destroy(&s);return error?1:0;
}
