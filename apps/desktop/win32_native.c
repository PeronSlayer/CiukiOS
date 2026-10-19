/* Native f5-04 service/channel transport. SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "win32_adapter.h"
#include <ciuki/channel.h>
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
    if(!error)*used=total;
    pthread_mutex_unlock(&rpc_lock);return error;
}
static struct desktop *owner;
static int authorized;
static struct desk_w32_supervisor supervisor;
static int spawn_service(void *ctx)
{
    (void)ctx;char *argv[]={"/System/Win32/w32srv",NULL};
    char *env[]={"LC_ALL=C","TZ=UTC0","PATH=/Programs",NULL};
    int pid=ciuki_spawn(argv[0],argv,env,NULL,0,0);return pid<0 ? -errno : pid;
}
static void record_service(void *ctx,const char *event,int pid,uint64_t elapsed)
{(void)ctx;fprintf(stderr,"%s pid=%d elapsed_ms=%llu\n",event,pid,(unsigned long long)(elapsed/1000000));}
static const struct desk_w32_supervisor_ops supervision={spawn_service,record_service,NULL};
/* One bounded transfer from the sleeping shortcut worker to the shell thread. */
static pthread_mutex_t launch_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {int ready,fd,pid,kind;uint32_t show;char title[64];struct desk_launch_record record;} pending;
int desk_w32_launch(enum desk_w32_kind kind,const struct desk_launch_record *r)
{
    int pair[2]={-1,-1},report[2]={-1,-1};
    if(kind!=DESK_W32_PE && !owner)return -EPIPE;
    pthread_mutex_lock(&launch_lock);
    if(pending.ready){pthread_mutex_unlock(&launch_lock);return -EBUSY;}
    int result=-EIO;
    if(kind==DESK_W32_ELF){
        pending.ready=1;pending.kind=kind;pending.record=*r;
        pthread_mutex_unlock(&launch_lock);return 0;
    }
    if(kind!=DESK_W32_PE && ciuki_channel_pair(pair)<0){result=-errno;goto done;}
    if(ciuki_channel_pair(report)<0){result=-errno;goto done;}
    struct ciuki_spawn_fd maps[2];unsigned count=0;
    if(pair[1]>=0){if(fcntl(pair[1],F_SETFD,0)<0){result=-errno;goto done;}maps[count++]=(struct ciuki_spawn_fd){pair[1],3};}
    if(fcntl(report[1],F_SETFD,0)<0){result=-errno;goto done;}
    maps[count++]=(struct ciuki_spawn_fd){report[1],4};
    char type[8];snprintf(type,sizeof(type),"%u",(unsigned)kind);
    char *argv[]={"/System/Win32/shell-launch",type,(char *)r->target,(char *)r->arguments,(char *)r->directory,NULL};
    char *env[]={"LC_ALL=C","TZ=UTC0","PATH=/Programs","HOME=/Documents",NULL};
    int helper=ciuki_spawn(argv[0],argv,env,maps,count,0);
    if(helper<0){result=-errno;goto done;}
    uint64_t deadline=clock_ns()+UINT64_C(1500000000);
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
    pthread_mutex_unlock(&launch_lock);return result;
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
    int pid=d->launch(d,&program),error=errno;
    d->launch_arg=arg;memcpy(d->launch_argv,saved,sizeof(saved));
    if(pid>=0)return pid;
    return pid==-1 ? -(error ? error : EIO) : pid;
}
/* SIGTERM runs the production f5-07A wr_flush exit boundary. Reap must
 * confirm success before power-off; no endpoint permission is needed here. */
static int shutdown_pending,shutdown_done,shutdown_error;
int desk_w32_shutdown(void)
{
    if(!shutdown_pending){
        supervisor.stopping=1;shutdown_pending=1;shutdown_done=0;shutdown_error=0;
        if(!supervisor.pid)shutdown_error=-EIO;
        else if(kill((int)supervisor.pid,SIGTERM)<0)shutdown_error=-errno;
    }
    return shutdown_error ? shutdown_error : shutdown_done ? 1 : 0;
}
void desk_w32_start(struct desktop *d)
{
    owner=d;struct w32r_service a;uint32_t e=service(1,1,&a);
    if(e || !a.result){fprintf(stderr,"[win32] desktop endpoint unavailable error=%u\n",e);return;}
    authorized=1;desk_w32_supervise(&supervisor,&supervision,clock_ns());
}
void desk_w32_poll(struct desktop *d)
{
    if(!authorized)return;
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
    if(!pthread_mutex_trylock(&launch_lock)){
        if(pending.ready && pending.kind==DESK_W32_ELF){
            int result=desk_w32_native_shortcut(d,&pending.record);
            if(result<0)desk_shell_message(d,"Programs","The native shortcut could not be started with its requested arguments or working directory.",1);
            pending.ready=0;
        }
        if(pending.ready){
            int i=desk_add_client(d,pending.fd);
            if(i<0)close(pending.fd);
            else{
                d->clients[i].pid=(uint32_t)pending.pid;d->clients[i].awaiting_frame=1;
                snprintf(d->clients[i].application,sizeof(d->clients[i].application),"%s",pending.title);
                d->clients[i].dosbox=pending.kind==DESK_W32_DOS;
            }
            pending.ready=0;
        }
        pthread_mutex_unlock(&launch_lock);
    }
}
void desk_w32_child_exited(struct desktop *d,uint32_t pid,int status)
{
    (void)d;
    if(desk_w32_service_exited(&supervisor,&supervision,pid,status,clock_ns())){
        if(supervisor.stopping){
            shutdown_done=1;
            shutdown_error=WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : -EIO;
            fprintf(stderr,"[win32] w32srv shutdown-flush result=%d\n",shutdown_error);
        }else desk_w32_supervise(&supervisor,&supervision,clock_ns());
    }
}
