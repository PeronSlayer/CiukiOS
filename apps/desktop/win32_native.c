/* Native f5-04 service/channel transport. SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "win32_adapter.h"
#include "win32_restart.h"
#include <ciuki/channel.h>
#include <ciuki/proc.h>
#include <ciuki/spawn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unistd.h>
#include <stdlib.h>
#include "win32_crash.h"
uint32_t desk_w32_record(void *,uint32_t);
extern int desk_w32_crash_store(const char *,const struct ciuki_w32_fault *) __attribute__((weak));
extern void desk_w32_fault_dialog(struct desktop *,const struct ciuki_w32_fault *) __attribute__((weak));
extern void desk_v3_revoke(struct desktop *) __attribute__((weak));
extern void desk_v3_pointer_release(struct desktop *) __attribute__((weak));
extern void desk_links_refresh(struct desktop *) __attribute__((weak));
extern void desk_request_close(struct desktop *,int) __attribute__((weak));
static pthread_mutex_t crash_disk_lock=PTHREAD_MUTEX_INITIALIZER;
static void *crash_disk_worker(void *p)
{
    pthread_mutex_lock(&crash_disk_lock);
    /* G13: native program faults live with the other logs (BOOT.LOG, NETSURF.LOG). */
    const struct ciuki_w32_fault *f=p;
    int error=desk_w32_crash_store?desk_w32_crash_store(f->kind==W32_REPORT_NATIVE_FAULT?"/system/logs":"/WINDOWS/CRASH",p):-ENOSYS;
    pthread_mutex_unlock(&crash_disk_lock);
    if(error)fprintf(stderr,"[win32] warning check=crash-file-write error=%d\n",error);
    free(p);return NULL;
}
static int fault_close(uint32_t pid)
{
    struct ciuki_w32_report_args a={.header={1,sizeof(a),W32_REPORT,0},.action=W32_REPORT_CLOSE,.code=pid};
    return desk_w32_record(&a,sizeof(a))?-EIO:0;
}
static void fault_poll(struct desktop *d)
{
    if(d->v3_dialog_kind||!desk_w32_fault_dialog)return;
    struct ciuki_w32_fault f={0};
    struct ciuki_w32_report_args a={.header={1,sizeof(a),W32_REPORT,0},.action=W32_REPORT_NEXT,
        .buffer=(uint32_t)(uintptr_t)&f,.bytes=sizeof(f)};
    if(desk_w32_record(&a,sizeof(a))||!a.result)return;
    if(f.kind==W32_REPORT_CRASH||f.kind==W32_REPORT_NATIVE_FAULT){
        struct ciuki_w32_fault *copy=malloc(sizeof(*copy));
        if(copy){*copy=f;pthread_t thread;if(pthread_create(&thread,NULL,crash_disk_worker,copy)){free(copy);fprintf(stderr,"[win32] warning check=crash-file-worker\n");}else pthread_detach(thread);}
        else fprintf(stderr,"[win32] warning check=crash-file-allocation\n");
    }
    d->v3_fault_close=fault_close;desk_w32_fault_dialog(d,&f);
}
uint32_t desk_w32_record(void *,uint32_t);
static uint64_t clock_ns(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+(uint32_t)t.tv_nsec;}
static void pause_io(void){struct timespec t={.tv_sec=0,.tv_nsec=1000000};nanosleep(&t,NULL);}
static uint32_t service(unsigned action,unsigned role,struct w32r_service *a)
{
    *a=(struct w32r_service){.version=1,.size=sizeof(*a),.operation=0x200,.action=action,.service=role};
    uint32_t e=desk_w32_record(a,sizeof(*a));
    return e ? 1062 : a->error;
}
static uint32_t packet(int fd,uint32_t pid,const struct w32r_packet *in,struct w32r_packet *out)
{
    struct ciuki_message m={.length=256};memcpy(m.data,in,256);
    uint64_t deadline=clock_ns()+UINT64_C(500000000);
    for(;;){
        if(ciuki_channel_send(fd,&m,DONTWAIT)>=0)break;
        if(errno!=EAGAIN && errno!=EINTR)return 1062;
        if(clock_ns()>=deadline)return 1062;
        pause_io();
    }
    for(;;){
        int n=ciuki_channel_recv(fd,&m,DONTWAIT);
        if(n<0 && (errno==EAGAIN || errno==EINTR)){
            if(clock_ns()>=deadline)return 1062;
            pause_io();continue;
        }
        if(n!=1)return 1062;
        if(m.fd_count){for(unsigned i=0;i<m.fd_count && i<4;i++)close(m.fds[i]);return 13;}
        if(m.length!=256 || (pid && m.sender_pid!=pid))return 13;
        memcpy(out,m.data,256);
        if(out->version!=1)return 13;
        /* A timed-out read-only operation may leave a late reply; a fresh
         * BEGIN resets its session. Never mistake that reply for this one. */
        if(out->id!=in->id){if(clock_ns()>=deadline)return 1062;continue;}
        if(out->kind!=in->kind)return 13;
        return out->error;
    }
}
static pthread_mutex_t rpc_lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t serial;
static int service_registered_once;
uint32_t desk_w32_exchange(const void *input,uint32_t n,void *output,uint32_t cap,uint32_t *used)
{
    if(!input || !output || !used || n>W32R_MAX || n<sizeof(struct w32r_request)+2)return 87;
    *used=0;pthread_mutex_lock(&rpc_lock);
    struct w32r_service a;uint32_t error=service(2,2,&a);
    if(!error && !a.result)error=1062;
    uint32_t id=++serial;if(!id)id=++serial;
    struct w32r_packet in,out;uint32_t at=0,total=0;
    while(!error && at<n){
        in=(struct w32r_packet){.version=1,.id=id,.kind=at?W32R_WRITE:W32R_BEGIN,.total=n,.offset=at};
        uint32_t bytes=n-at;if(bytes>232)bytes=232;memcpy(in.bytes,(const uint8_t *)input+at,bytes);
        error=packet((int)a.endpoint,0,&in,&out);if(error)break;
        at+=bytes;if(out.offset!=at){error=13;break;}total=out.total;
    }
    if(!error && (total<sizeof(struct w32r_reply) || total>cap || total>W32R_MAX))error=13;
    at=0;
    while(!error && at<total){
        in=(struct w32r_packet){.version=1,.id=id,.kind=W32R_READ,.total=total,.offset=at};
        error=packet((int)a.endpoint,0,&in,&out);if(error)break;
        uint32_t bytes=total-at;if(bytes>232)bytes=232;
        if(out.total!=total || out.offset!=at+bytes){error=13;break;}
        memcpy((uint8_t *)output+at,out.bytes,bytes);at+=bytes;
    }
    if(!error){*used=total;__atomic_store_n(&service_registered_once,1,__ATOMIC_RELEASE);}
    pthread_mutex_unlock(&rpc_lock);return error;
}
static struct desktop *owner;
static int authorized;
static struct desk_w32_supervisor supervisor;
/* The first server of this desktop resets the directory's client identities
 * only on a cold boot; after a desktop death in this boot the surviving
 * Win32 programs keep theirs (desk_w32_fresh_clients, Codex review P2). */
static int fresh_clients(void)
{
    if (__atomic_load_n(&service_registered_once,__ATOMIC_ACQUIRE)) return 0;
    static struct {struct ciuki_proc_system system;struct ciuki_proc_record records[CIUKI_PROCESS_MAX];} snapshot;
    int records=ciuki_proc_list(&snapshot,sizeof(snapshot));
    /* An unusable snapshot keeps the identities: the reconcile against the
     * live processes is the non-destructive choice (Codex re-review P2). */
    if (records<0 || (uint32_t)records!=snapshot.system.records || snapshot.system.records>CIUKI_PROCESS_MAX) return 0;
    return desk_w32_fresh_clients(&snapshot.system,snapshot.records,(uint32_t)getpid());
}
static int spawn_service(void *ctx)
{
    (void)ctx;char *argv[]={"/System/Win32/w32srv",fresh_clients()?"--fresh-clients":NULL,NULL};
    char *env[]={"LC_ALL=C","PATH=/Programs",NULL};
    int pid=ciuki_spawn(argv[0],argv,env,NULL,0,0);return pid<0 ? -errno : pid;
}
static void record_service(void *ctx,const char *event,int pid,uint64_t elapsed)
{(void)ctx;fprintf(stderr,"%s pid=%d elapsed_ms=%llu\n",event,pid,(unsigned long long)(elapsed/1000000));}
static const struct desk_w32_supervisor_ops supervision={spawn_service,record_service,NULL};
/* One bounded transfer from the sleeping shortcut worker to the shell thread. */
static pthread_mutex_t launch_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned launch_inflight;
static int launch_published;
static struct {int ready,fd,pid,kind;uint32_t show;char title[64];struct desk_launch_record record;} pending;
static int launch_execute(enum desk_w32_kind kind,const struct desk_launch_record *r,int wait)
{
    int pair[2]={-1,-1},report[2]={-1,-1},helper=-1;
    if(wait && kind!=DESK_W32_PE)return -EOPNOTSUPP;
    if(kind==DESK_W32_PE){
        struct {struct w32r_request r;char empty[2];} ping={.r={.version=1,.op=W32R_OPEN,.key=W32R_HKLM,.access=0x20019,.path_bytes=1,.name_bytes=1}};
        struct w32r_reply reply;uint32_t used=0;
        if(desk_w32_exchange(&ping,sizeof(ping.r)+2,&reply,sizeof(reply),&used) || used!=sizeof(reply) || reply.error)return -EAGAIN;
        ping.r.op=W32R_CLOSE;ping.r.key=reply.key;ping.r.access=0;
        desk_w32_exchange(&ping,sizeof(ping.r)+2,&reply,sizeof(reply),&used);
    }
    if(kind!=DESK_W32_PE && !owner)return -EPIPE;
    /* The sleeping launch worker may run before w32srv's spawned process
     * registers. Prove readiness on every PE launch, including supervision
     * restarts; a retained PID is not endpoint-registration evidence. */
    if(kind==DESK_W32_PE){
        uint64_t deadline=clock_ns()+UINT64_C(5000000000);int e;
        while((e=desk_w32_ready())!=0){
            if(e!=-EPIPE || clock_ns()>=deadline)return e;
            pause_io();
        }
    }
    pthread_mutex_lock(&launch_lock);
    if(pending.ready){pthread_mutex_unlock(&launch_lock);return -EBUSY;}
    int result=-EIO;
    if(kind==DESK_W32_ELF || kind==DESK_W32_DOS){
        pending.ready=1;pending.kind=kind;pending.record=*r;
        pthread_mutex_unlock(&launch_lock);
        __atomic_store_n(&launch_published,1,__ATOMIC_RELEASE);desk_wake(owner);return 0;
    }
    if(kind!=DESK_W32_PE && ciuki_channel_pair(pair)<0){result=-errno;goto done;}
    if(ciuki_channel_pair(report)<0){result=-errno;goto done;}
    struct ciuki_spawn_fd maps[2];unsigned count=0;
    if(pair[1]>=0){if(fcntl(pair[1],F_SETFD,0)<0){result=-errno;goto done;}maps[count++]=(struct ciuki_spawn_fd){pair[1],3};}
    if(fcntl(report[1],F_SETFD,0)<0){result=-errno;goto done;}
    maps[count++]=(struct ciuki_spawn_fd){report[1],4};
    char type[8],show[2]={(char)('0'+r->show),0};snprintf(type,sizeof(type),"%u",(unsigned)kind);
    char *argv[]={"/System/Win32/shell-launch",type,(char *)r->target,(char *)r->arguments,(char *)r->directory,show,wait ? "--wait" : NULL,NULL};
    char *env[]={"LC_ALL=C","PATH=/Programs","HOME=/Documents",NULL};
    helper=ciuki_spawn(argv[0],argv,env,maps,count,0);
    if(helper<0){result=-errno;goto done;}
    uint64_t deadline=clock_ns()+(wait ? UINT64_C(300000000000) : UINT64_C(1500000000));
    struct ciuki_message m;
    for(;;){
        int n=ciuki_channel_recv(report[0],&m,DONTWAIT);
        if(n==1){
            if(m.sender_pid!=(uint32_t)helper || m.length!=8 || m.fd_count){result=-EIO;break;}
            result=(int32_t)desk_get32(m.data);break;
        }
        if(n==0 || (n<0 && errno!=EINTR && errno!=EAGAIN)){result=-EIO;break;}
        if(clock_ns()>=deadline){result=-ETIMEDOUT;break;}pause_io();
    }
    if(result>0 && pair[0]>=0){
        pending.ready=1;pending.fd=pair[0];pending.pid=result;pending.kind=kind;pending.show=r->show;
        snprintf(pending.title,sizeof(pending.title),"%s",r->label);pair[0]=-1;
    }
done:
    for(unsigned i=0;i<2;i++){if(pair[i]>=0)close(pair[i]);if(report[i]>=0)close(report[i]);}
    if(wait && helper>0 && result!=-ETIMEDOUT){
        int status=0,joined;do {joined=waitpid(helper,&status,0);} while(joined<0 && errno==EINTR);
        if(joined<0 && !result)result=-errno;
    }
    int publish=pending.ready;pthread_mutex_unlock(&launch_lock);
    if(publish){__atomic_store_n(&launch_published,1,__ATOMIC_RELEASE);desk_wake(owner);}
    return result;
}
int desk_w32_launch(enum desk_w32_kind kind,const struct desk_launch_record *r)
{
    __atomic_add_fetch(&launch_inflight,1,__ATOMIC_RELEASE);
    int result=launch_execute(kind,r,0);
    __atomic_sub_fetch(&launch_inflight,1,__ATOMIC_RELEASE);
    return result;
}
int desk_w32_launch_wait(enum desk_w32_kind kind,const struct desk_launch_record *r)
{
    __atomic_add_fetch(&launch_inflight,1,__ATOMIC_RELEASE);
    int result=launch_execute(kind,r,1);
    __atomic_sub_fetch(&launch_inflight,1,__ATOMIC_RELEASE);
    return result;
}
/* Preserve the existing launcher's program identity, task authority, child
 * ledger and native-client permissions. Its six-argument bound remains. */
int desk_w32_native_shortcut(struct desktop *d,const struct desk_launch_record *r)
{
    if(!d->launch || !desk_launch_valid(r))return -EINVAL;
    if(d->power.state)return -EBUSY;
    struct desk_program program={0};
    int e=desk_w32_native_path(r->target,program.path,sizeof(program.path));if(e)return e;
    const char *name=strrchr(program.path,'/');name=name ? name+1 : program.path;
    if(!*name || strlen(name)>=sizeof(program.name))return -ENAMETOOLONG;
    strcpy(program.name,name);
    /* Reserved launch identities confer desktop protocol permissions (and
     * Tasks receives a termination fd). A similarly named user ELF must not
     * acquire those rights through a shortcut. */
    int privileged=desk_program_kind(name) || !strcmp(name,"tasks") || !strcmp(name,"gfxdemo") ||
        !strcmp(name,"dosbox") || !strcmp(name,"mixer") || !strcmp(name,"music");
    if(privileged){
        char trusted[80];snprintf(trusted,sizeof(trusted),"/Programs/%s",name);
        if(strcmp(program.path,trusted))return -EPERM;
    }
    if(r->directory[0]){
        char requested[260],current[260];
        e=desk_w32_native_path(r->directory,requested,sizeof(requested));if(e)return e;
        if(!getcwd(current,sizeof(current)))return -errno;
        /* No process-wide chdir on the input thread. A distinct cwd needs a
         * native spawn-cwd interface; refuse rather than lose capabilities. */
        if(strcmp(requested,current))return -EOPNOTSUPP;
    }
    char text[1024];strcpy(text,r->arguments);char *words[6]={0};
    int n=desk_w32_arguments(text,words,6);if(n<0)return n;
    const char *saved[6],*arg=d->launch_arg;
    memcpy(saved,d->launch_argv,sizeof(saved));
    d->launch_arg=NULL;
    for(unsigned k=0;k<6;k++)d->launch_argv[k]=k<(unsigned)n ? words[k] : NULL;
    int saved_show=d->launch_show;d->launch_show=(int)r->show;
    int pid=d->launch(d,&program),error=errno;
    d->launch_show=saved_show;
    d->launch_arg=arg;memcpy(d->launch_argv,saved,sizeof(saved));
    if(pid>=0)return pid;
    return pid==-1 ? -(error ? error : EIO) : pid;
}
/* SIGTERM runs the production f5-07A wr_flush exit boundary. Reap must
 * confirm success before power-off; no endpoint permission is needed here.
 * Review P2-7 (T23 c42f556e): the wait is bounded. A service that has not
 * exited DESK_W32_SHUTDOWN_MS after SIGTERM gets SIGKILL (its flush is
 * lost, recorded); one that is not reaped DESK_W32_SHUTDOWN_MS after that
 * (a thread in a kernel wait) is abandoned and the shutdown proceeds (call
 * 73 then reports its holders as EBUSY and the user may Force). */
#define DESK_W32_SHUTDOWN_MS 10000u
static int shutdown_pending,shutdown_done,shutdown_error,shutdown_killed;
static uint64_t shutdown_sent_ms;
int desk_w32_shutdown(void)
{
    uint64_t now=clock_ns()/1000000;
    if(!shutdown_pending){
        supervisor.stopping=1;shutdown_pending=1;shutdown_done=0;shutdown_error=0;shutdown_killed=0;shutdown_sent_ms=now;
        if(!supervisor.pid)shutdown_error=-EIO;
        else if(kill((int)supervisor.pid,SIGTERM)<0)shutdown_error=-errno;
    }
    if(shutdown_error)return shutdown_error;
    if(shutdown_done)return 1;
    if(!shutdown_killed && now>=shutdown_sent_ms+DESK_W32_SHUTDOWN_MS){
        shutdown_killed=1;shutdown_sent_ms=now;
        fprintf(stderr,"[win32] w32srv shutdown-flush timeout after %u ms: SIGKILL\n",DESK_W32_SHUTDOWN_MS);
        if(supervisor.pid && kill((int)supervisor.pid,SIGKILL)<0 && errno==ESRCH){shutdown_done=1;return 1;}
    }else if(shutdown_killed && now>=shutdown_sent_ms+DESK_W32_SHUTDOWN_MS){
        fprintf(stderr,"[win32] w32srv shutdown-flush abandoned: not reaped %u ms after SIGKILL\n",DESK_W32_SHUTDOWN_MS);
        shutdown_done=1;return 1;
    }
    return 0;
}
/* f5-07B native binding. The state machine never waits for a process or a
 * launch worker on the input thread. A registry round trip gates all PE use. */
static struct desk_restart restart;
static unsigned restart_closing_pending;
static uint32_t restart_progress_window;
static uint32_t restart_call(void *ctx,unsigned op,unsigned action,uint32_t flags,
                             struct w32r_reply *reply,void *data,size_t cap)
{
    (void)ctx;
    struct {struct w32r_request r;char empty[2];} a={.r={.version=1,.op=op,
        .index=action,.flags=flags,.path_bytes=1,.name_bytes=1}};
    /* The compositor's authoritative order is copied, with authenticated
     * PID+HWND pairs. w32srv validates each pair against its window directory. */
    uint8_t order[DESK_CLIENTS*8];unsigned count=0;
    if(op==W32R_ENDSESSION && action==W32R_SESSION_BEGIN && owner){
        int indices[DESK_CLIENTS];
        for(unsigned i=0;i<DESK_CLIENTS;i++)if(owner->clients[i].window && owner->clients[i].v3_win32_hwnd){
            unsigned at=count++;while(at && owner->clients[indices[at-1]].order<owner->clients[i].order){indices[at]=indices[at-1];at--;}indices[at]=(int)i;
        }
        for(unsigned i=0;i<count;i++){const struct desk_client *c=&owner->clients[indices[i]];desk_put32(order+i*8,c->v3_win32_hwnd);desk_put32(order+i*8+4,c->pid);}
    }
    a.r.data_bytes=count*8;
    uint32_t size=(uint32_t)sizeof(a.r)+2+a.r.data_bytes;
    uint8_t *input=malloc(size),*out=malloc(sizeof(*reply)+cap);
    if(!input || !out){free(input);free(out);return 8;}
    memcpy(input,&a,sizeof(a.r)+2);if(count)memcpy(input+sizeof(a.r)+2,order,count*8);
    uint32_t used=0,e=desk_w32_exchange(input,size,out,(uint32_t)(sizeof(*reply)+cap),&used);
    free(input);
    if(!e){
        if(used<sizeof(*reply))e=13;
        else{memcpy(reply,out,sizeof(*reply));e=reply->error;
            if(!e && (reply->version!=1 || reply->name_bytes || reply->data_bytes>cap || used!=sizeof(*reply)+reply->data_bytes))e=13;
            if(!e && reply->data_bytes)memcpy(data,out+sizeof(*reply),reply->data_bytes);
        }
    }
    free(out);return e;
}
/* kill(pid,0) reports EPERM across groups, including zombies. Restart
 * participants may have CIUKI_SPAWN_NEW_GROUP; use the process directory. */
int desk_restart_process_alive(uint32_t pid)
{
    static struct {struct ciuki_proc_system system;struct ciuki_proc_record records[CIUKI_PROCESS_MAX];} list;
    int count=ciuki_proc_list(&list,sizeof(list));
    if(count<0 || (unsigned)count!=list.system.records || list.system.records>CIUKI_PROCESS_MAX)return 1;
    for(unsigned i=0;i<list.system.records;i++)if(list.records[i].pid==pid)
        return list.records[i].state==CIUKI_PROC_LIVE || list.records[i].state==CIUKI_PROC_STOPPING;
    return 0;
}
static int restart_alive(void *ctx,uint32_t pid)
{(void)ctx;return desk_restart_process_alive(pid);}
static int restart_kill(void *ctx,uint32_t pid)
{
    (void)ctx;return owner && owner->terminate?owner->terminate(pid,SIGKILL):-ENOSYS;
}
static void restart_record(void *ctx,const char *event,uint32_t n)
{(void)ctx;fprintf(stderr,"%s value=%u\n",event,n);}
extern void desk_restart_fixture_notice(unsigned,uint32_t,uint32_t) __attribute__((weak));
static void restart_notice(void *ctx,unsigned kind,uint32_t pid,uint32_t flags)
{
    (void)ctx;if(!owner)return;
    if(desk_restart_fixture_notice)desk_restart_fixture_notice(kind,pid,flags);
    if(kind==1){
        if(desk_v3_revoke)desk_v3_revoke(owner);if(desk_v3_pointer_release)desk_v3_pointer_release(owner);
        owner->v3_dialog_kind=2;owner->v3_dialog_pid=pid;owner->v3_restart_flags=flags;
        desk_shell_message(owner,"Restart CiukiOS","Setup wants to restart CiukiOS. CiukiOS will close all Windows programs and finish the installation.",0);
    }else if(kind==2){
        if(desk_v3_revoke)desk_v3_revoke(owner);if(desk_v3_pointer_release)desk_v3_pointer_release(owner);
        restart_closing_pending=1;
    }else if(kind==3){
        restart_closing_pending=0;
        if(desk_request_close)for(unsigned i=0;i<DESK_CLIENTS;i++)if(restart_progress_window && owner->clients[i].window_id==restart_progress_window && owner->clients[i].internal==DESK_INTERNAL_MESSAGE)desk_request_close(owner,(int)i);
        restart_progress_window=0;
        owner->v3_links_next=0;owner->v3_links_force|=DESK_LINKS_SCAN|DESK_LINKS_GAMES;if(desk_links_refresh)desk_links_refresh(owner);
        fprintf(stderr,"[win32] restart desktop-input-restored\n");
    }else fprintf(stderr,"[win32] end-session desktop-power-decision flags=%u\n",flags);
}
static pthread_mutex_t restart_launch_lock=PTHREAD_MUTEX_INITIALIZER;
static int restart_launch_busy,restart_launch_ready,restart_launch_result;
static char restart_command[2048];
static void *restart_launch_worker(void *unused)
{
    (void)unused;
    int report[2]={-1,-1},result=-EIO;
    if(ciuki_channel_pair(report)<0)result=-errno;
    else if(fcntl(report[1],F_SETFD,0)<0)result=-errno;
    else{
        struct ciuki_spawn_fd map={report[1],4};
        char *argv[]={"/System/Win32/runonce.exe",restart_command,NULL};
        char *env[]={"LC_ALL=C","PATH=/Programs","HOME=/Documents",NULL};
        int helper=ciuki_spawn(argv[0],argv,env,&map,1,0);
        if(helper<0)result=-errno;
        else{
            uint64_t deadline=clock_ns()+UINT64_C(300000000000);struct ciuki_message m;
            for(;;){
                int n=ciuki_channel_recv(report[0],&m,DONTWAIT);
                if(n==1){
                    if(m.sender_pid==(uint32_t)helper && m.length==8 && !m.fd_count){result=(int32_t)desk_get32(m.data);if(result==-EAGAIN)result=-EIO;}
                    break;
                }
                if(n==0 || (n<0 && errno!=EAGAIN && errno!=EINTR))break;
                if(clock_ns()>=deadline){result=-ETIMEDOUT;restart_kill(NULL,(uint32_t)helper);break;}pause_io();
            }
        }
    }
    for(unsigned i=0;i<2;i++)if(report[i]>=0)close(report[i]);
    pthread_mutex_lock(&restart_launch_lock);restart_launch_result=result;restart_launch_ready=1;desk_wake(owner);pthread_mutex_unlock(&restart_launch_lock);return NULL;
}
static int restart_launch(void *ctx,const char *command)
{
    (void)ctx;pthread_mutex_lock(&restart_launch_lock);
    if(restart_launch_ready){int result=restart_launch_result;restart_launch_ready=restart_launch_busy=0;pthread_mutex_unlock(&restart_launch_lock);return result;}
    if(!restart_launch_busy){
        if(strlen(command)>=sizeof(restart_command)){pthread_mutex_unlock(&restart_launch_lock);return -E2BIG;}
        strcpy(restart_command,command);restart_launch_busy=1;
        pthread_t thread;
        if(pthread_create(&thread,NULL,restart_launch_worker,NULL)){restart_launch_busy=0;pthread_mutex_unlock(&restart_launch_lock);return -EIO;}
        pthread_detach(thread);
    }
    pthread_mutex_unlock(&restart_launch_lock);return -EAGAIN;
}
static const struct desk_restart_ops restart_ops={NULL,restart_call,restart_alive,restart_kill,restart_launch,restart_notice,restart_record};
/* One sleeping worker for the entire desktop lifetime. The mailbox is never
 * held during RPC, nor while the desktop applies a result or draws a frame.
 * Scheduling is driven by desktop ticks; missed frames never queue RPCs. */
static pthread_mutex_t idle_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle_wake=PTHREAD_COND_INITIALIZER;
static struct {
    int started,request,ready,busy;
    unsigned flags,epoch,result_epoch;
    uint32_t error;
    struct w32r_reply reply;
} idle;
static unsigned idle_epoch;
static uint64_t idle_last;
static int idle_polled;
static void *restart_idle_worker(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&idle_lock);
    for(;;){
        while(!idle.request)pthread_cond_wait(&idle_wake,&idle_lock);
        unsigned flags=idle.flags,epoch=idle.epoch;idle.request=0;
        pthread_mutex_unlock(&idle_lock);
        struct w32r_reply reply={0};char message[W32R_SESSION_MESSAGE];
        uint32_t error=restart_call(NULL,W32R_SHUTDOWN,W32R_SHUTDOWN_POLL,flags,&reply,message,sizeof(message));
        pthread_mutex_lock(&idle_lock);
        idle.reply=reply;idle.error=error;idle.result_epoch=epoch;idle.ready=1;desk_wake(owner);
    }
    return NULL;
}
static void restart_idle_poll(struct desktop *d,uint64_t now)
{
    int active=!!__atomic_load_n(&launch_inflight,__ATOMIC_ACQUIRE);
    for(unsigned i=0;i<DESK_CLIENTS;i++)if(d->clients[i].v3_win32_hwnd){active=1;break;}
    if(!pthread_mutex_trylock(&launch_lock)){
        active|=pending.ready;pthread_mutex_unlock(&launch_lock);
    }else active=1;
    pthread_mutex_lock(&idle_lock);
    if(!idle.started){
        pthread_t thread;
        if(pthread_create(&thread,NULL,restart_idle_worker,NULL)){
            pthread_mutex_unlock(&idle_lock);return;
        }
        pthread_detach(thread);idle.started=1;
    }
    struct w32r_reply reply;int take=0;
    if(idle.ready && (idle.result_epoch!=idle_epoch || (!restart.stage && !d->v3_dialog_kind))){
        take=!idle.error && idle.result_epoch==idle_epoch;
        reply=idle.reply;idle.ready=idle.busy=0;
    }
    /* A direct restart supersedes a completed idle poll. */
    if(idle.ready && restart.stage){idle.ready=idle.busy=0;}
    pthread_mutex_unlock(&idle_lock);
    if(take)desk_restart_idle_result(&restart,&restart_ops,&reply,now);
    if(restart.stage || d->v3_dialog_kind)return;
    unsigned interval=active?DESK_RESTART_IDLE_MS:DESK_RESTART_QUIET_MS;
    if(idle_polled && now-idle_last<interval)return;
    pthread_mutex_lock(&idle_lock);
    if(!idle.busy){
        idle.busy=idle.request=1;idle.flags=restart.boot_checked?0:1;idle.epoch=idle_epoch;
        idle_last=now;idle_polled=1;pthread_cond_signal(&idle_wake);
    }
    pthread_mutex_unlock(&idle_lock);
}
/* G3: the desktop's end-of-session turn for every Win32 window (broker
 * HOLD session). The values of the desktop's seam are the W32R ones. */
_Static_assert(DESK_W32_SESSION_BEGIN==W32R_SESSION_BEGIN && DESK_W32_SESSION_STATUS==W32R_SESSION_STATUS &&
               DESK_W32_SESSION_RESET==W32R_SESSION_RESET && DESK_W32_SESSION_CANCEL==W32R_SESSION_CANCEL &&
               DESK_W32_SESSION_COMMIT==W32R_SESSION_COMMIT && DESK_W32_SESSION_HOLD==W32R_SESSION_HOLD,"G3 seam values");
static int w32_session_open;   /* a HOLD session may still deliver WM_ENDSESSION */
static int w32_session(struct desktop *d,unsigned action,uint32_t flags,struct desk_w32_status *st)
{
    (void)d;
    if(!authorized)return -ENODEV;
    if(restart.stage)return -EBUSY;   /* an ExitWindowsEx restart owns the broker */
    struct w32r_reply r;
    if(action==W32R_SESSION_BEGIN){   /* a finished or abandoned earlier session is cleared first */
        if(restart_call(NULL,W32R_ENDSESSION,W32R_SESSION_RESET,0,&r,NULL,0))return -EIO;
    }
    uint32_t e=restart_call(NULL,W32R_ENDSESSION,action,flags,&r,NULL,0);
    if(e)return e==1115?-EBUSY:-EIO;
    if(action==W32R_SESSION_BEGIN)w32_session_open=1;
    st->state=r.key;st->pid=r.count;st->hwnd=r.type;st->delivered=r.disposition;
    return 0;
}
/* G11 (docs/design/power-policy.md): WM_POWERBROADCAST through the same
 * broker; the values of the desktop's seam are the W32R ones. */
_Static_assert(DESK_W32_POWER_BEGIN==W32R_POWER_BEGIN && DESK_W32_POWER_STATUS==W32R_POWER_STATUS &&
               DESK_W32_POWER_RESET==W32R_POWER_RESET,"G11 seam values");
static int w32_power(struct desktop *d,unsigned action,uint32_t event,uint32_t lparam,struct desk_w32_status *st)
{
    (void)d;
    if(!authorized)return -ENODEV;
    if(restart.stage)return -EBUSY;   /* an ExitWindowsEx restart owns the broker */
    struct {struct w32r_request r;char empty[2];} a={.r={.version=1,.op=W32R_ENDSESSION,.index=action,
        .type=event,.flags=lparam,.path_bytes=1,.name_bytes=1}};
    struct w32r_reply r;uint32_t used=0;
    uint32_t e=desk_w32_exchange(&a,(uint32_t)sizeof(a.r)+2,&r,(uint32_t)sizeof(r),&used);
    if(!e && (used!=sizeof(r) || r.version!=1 || r.name_bytes || r.data_bytes))e=13;
    if(!e)e=r.error;
    if(e)return e==1115?-EBUSY:-EIO;
    st->state=r.key;st->pid=r.count;st->hwnd=0;st->delivered=0;
    return 0;
}
/* After the desktop's session ended (cancelled, or a log off), free the
 * broker once its WM_ENDSESSION deliveries are done. */
static void w32_session_reap(struct desktop *d,uint64_t ms)
{
    static uint64_t next;
    if(!w32_session_open || d->session.state || d->power.state || ms<next)return;
    next=ms+DESK_SESSION_POLL_NS/1000000;
    struct w32r_reply r;
    if(restart_call(NULL,W32R_ENDSESSION,W32R_SESSION_STATUS,0,&r,NULL,0) || r.key==1 || r.key==2){
        (void)restart_call(NULL,W32R_ENDSESSION,W32R_SESSION_RESET,0,&r,NULL,0);w32_session_open=0;
    }else if(r.key==3)(void)restart_call(NULL,W32R_ENDSESSION,W32R_SESSION_CANCEL,0,&r,NULL,0);
}
/* T23 c42f556e: the live Win32 processes for the power sequence (the
 * broker's authenticated clients, W32R_SHUTDOWN_LIST, as the ExitWindowsEx
 * restart uses), also those whose desktop connection ended. */
static int w32_power_pids(struct desktop *d,uint32_t *pids,unsigned cap)
{
    (void)d;
    if(!authorized)return -ENODEV;
    if(!pids || !cap)return -EINVAL;
    struct w32r_reply r;uint8_t bytes[4*64];
    uint32_t e=restart_call(NULL,W32R_SHUTDOWN,W32R_SHUTDOWN_LIST,0,&r,bytes,sizeof(bytes));
    /* Review P2-4: an error is reported, never "no programs". */
    if(e || r.count>64 || r.data_bytes!=r.count*4){fprintf(stderr,"[win32] power list unavailable error=%u\n",e);return e==1115?-EBUSY:-EIO;}
    unsigned n=0;
    for(unsigned i=0;i<r.count;i++){
        uint32_t pid=(uint32_t)bytes[i*4]|(uint32_t)bytes[i*4+1]<<8|(uint32_t)bytes[i*4+2]<<16|(uint32_t)bytes[i*4+3]<<24;
        if(!pid)continue;
        if(n>=cap){fprintf(stderr,"[win32] power list truncated count=%u cap=%u\n",r.count,cap);return -ENOSPC;}
        pids[n++]=pid;
    }
    return (int)n;
}
int ciuki_win32_shell_restart(uint32_t pid,uint32_t flags)
{return !authorized || !owner?-EPIPE:desk_restart_begin(&restart,&restart_ops,pid,flags,clock_ns()/1000000);}
void desk_w32_start(struct desktop *d)
{
    owner=d;struct w32r_service a;uint32_t e=service(1,1,&a);
    if(e || !a.result){fprintf(stderr,"[win32] desktop endpoint unavailable error=%u\n",e);return;}
    authorized=1;d->w32_session=w32_session;d->w32_power=w32_power;d->power_w32_pids=w32_power_pids;desk_w32_supervise(&supervisor,&supervision,clock_ns());
}
void desk_w32_poll(struct desktop *d)
{
    if(!authorized)return;
    fault_poll(d);
    if(restart_closing_pending && !d->v3_dialog_kind){
        desk_shell_message(d,"Restart CiukiOS","Closing Windows programs...",0);
        for(unsigned i=0;i<DESK_CLIENTS;i++)if(d->clients[i].window && d->clients[i].internal==DESK_INTERNAL_MESSAGE)restart_progress_window=d->clients[i].window_id;
        restart_closing_pending=0;
    }
    uint64_t ms=clock_ns()/1000000;
    restart_idle_poll(d,ms);
    if(restart.stage)desk_restart_tick(&restart,&restart_ops,ms);
    w32_session_reap(d,ms);
    uint64_t now=clock_ns();
    if(!d->power.state){
        shutdown_pending=shutdown_done=shutdown_error=0;
        supervisor.stopping=0;
    }
    desk_w32_supervise(&supervisor,&supervision,now);
    struct w32r_service a;
    for(unsigned k=0;k<DESK_CLIENTS;k++){
        if(service(3,1,&a) || !a.result)break;
        int i=desk_add_client(d,(int)a.endpoint);
        if(i<0)close((int)a.endpoint);else d->clients[i].pid=a.pid;
    }
    if(__atomic_load_n(&launch_published,__ATOMIC_ACQUIRE)){
        pthread_mutex_lock(&launch_lock);
        if(pending.ready && (pending.kind==DESK_W32_ELF || pending.kind==DESK_W32_DOS)){
            int result;
            if(pending.kind==DESK_W32_DOS){
                char command[260];result=desk_w32_dos_command(&pending.record,command,sizeof(command));
                int saved_show=d->launch_show;d->launch_show=(int)pending.record.show;
                if(!result)result=desk_task_launch(d,"dosbox",command);
                d->launch_show=saved_show;
            }else result=desk_w32_native_shortcut(d,&pending.record);
            if(result<0)desk_shell_message(d,"Programs","The program could not be started with its requested arguments or working directory.",1);
            pending.ready=0;
        }
        if(pending.ready){
            int i=desk_add_client(d,pending.fd);
            if(i<0)close(pending.fd);
            else{
                d->clients[i].pid=(uint32_t)pending.pid;d->clients[i].awaiting_frame=1;
                snprintf(d->clients[i].application,sizeof(d->clients[i].application),"%s",pending.title);
                d->clients[i].dosbox=pending.kind==DESK_W32_DOS;
                d->clients[i].initial_show=(int)pending.show;
            }
            pending.ready=0;
        }
        __atomic_store_n(&launch_published,0,__ATOMIC_RELEASE);
        pthread_mutex_unlock(&launch_lock);
    }
}
void desk_w32_child_exited(struct desktop *d,uint32_t pid,int status)
{
    (void)d;
    if(desk_w32_service_exited(&supervisor,&supervision,pid,status,clock_ns())){
        restart.boot_checked=0;
        idle_epoch++;idle_polled=0;
        if(supervisor.stopping){
            shutdown_done=1;
            shutdown_error=WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EIO;
            /* Review P2-7: ended by the desktop's own SIGKILL after the bound,
             * its flush is lost (recorded) and the shutdown goes on. */
            if(shutdown_killed && shutdown_error){fprintf(stderr,"[win32] w32srv shutdown-flush lost: ended by SIGKILL after the %u ms bound\n",DESK_W32_SHUTDOWN_MS);shutdown_error=0;}
            fprintf(stderr,"[win32] w32srv shutdown-flush result=%d\n",shutdown_error);
        }else desk_w32_supervise(&supervisor,&supervision,clock_ns());
    }
}
