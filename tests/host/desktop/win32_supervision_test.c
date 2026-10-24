/* Real production server binary, production native RPC and supervisor. */
#include "win32_adapter.h"
#include <ciuki/channel.h>
#include <ciuki/spawn.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The host socket header declares its own SOCK_RAW enum member. */
#undef SOCK_RAW
#undef MSG_TRUNC
#undef MSG_PEEK
#undef MSG_DONTWAIT
#undef MSG_NOSIGNAL
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
ciuki_pid_t ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *maps,uint32_t count,uint32_t flags)
{(void)path;(void)argv;(void)env;(void)maps;(void)count;(void)flags;assert(0);return -1;}
/* This fixture exercises the real registry transport/supervision, never UI
 * or process launching. ASan retains the restart callback table as a root. */
int ciuki_channel_pair(int *fds){(void)fds;assert(0);return -1;}
void desk_shell_message(struct desktop *d,const char *title,const char *text,int error)
{(void)d;(void)title;(void)text;(void)error;assert(0);}
int ciuki_proc_list(void *p,uint32_t n){(void)p;(void)n;assert(0);return -1;}
static const char *binary,*directory;
static int endpoint=-1;
static unsigned registrations,reconnects,records;
static uint64_t now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+(uint32_t)t.tv_nsec;}
uint32_t desk_w32_record(void *record,uint32_t bytes)
{
    struct w32r_service *a=record;assert(bytes==sizeof(*a) && a->action==2 && a->service==2);
    if(endpoint<0){a->error=1062;return 0;}
    reconnects++;a->endpoint=(uint32_t)endpoint;a->result=1;return 0;
}
int ciuki_channel_send(int fd,const struct ciuki_message *m,uint32_t flags)
{(void)flags;ssize_t n=write(fd,m->data,m->length);if(n<0)fprintf(stderr,"host send fd=%d errno=%d\n",fd,errno);return n==(ssize_t)m->length?0:-1;}
int ciuki_channel_recv(int fd,struct ciuki_message *m,uint32_t flags)
{
    (void)flags;memset(m,0,sizeof(*m));ssize_t n=read(fd,m->data,256);
    if(n<=0){if(n==0 || errno!=EAGAIN)fprintf(stderr,"host recv fd=%d n=%ld errno=%d pid=%ld\n",fd,(long)n,errno,(long)getpid());return (int)n;}m->length=(uint32_t)n;return 1;
}
static int spawn(void *unused)
{
    (void)unused;int pair[2],ready[2];assert(!socketpair(AF_UNIX,SOCK_SEQPACKET,0,pair));assert(!pipe(ready));
    assert(!fcntl(pair[0],F_SETFL,O_NONBLOCK));assert(!fcntl(pair[1],F_SETFL,O_NONBLOCK));
    int pid=fork();assert(pid>=0);
    if(!pid){
        close(pair[0]);close(ready[0]);char fd[32],signal_fd[32];snprintf(fd,sizeof(fd),"%d",pair[1]);snprintf(signal_fd,sizeof(signal_fd),"%d",ready[1]);
        setenv("W32_SOCKET",fd,1);setenv("W32_READY",signal_fd,1);setenv("W32_HIVES",directory,1);
        execl(binary,binary,(char *)NULL);_exit(127);
    }
    close(pair[1]);close(ready[1]);char byte;assert(read(ready[0],&byte,1)==1 && byte=='R');close(ready[0]);
    if(endpoint>=0)close(endpoint);endpoint=pair[0];registrations++;return pid;
}
static void record(void *unused,const char *text,int pid,uint64_t elapsed)
{(void)unused;assert(strstr(text,"[win32] w32srv restart") && pid>0 && elapsed<2000000000ull);records++;}
static uint32_t rpc(unsigned op,uint32_t key,const char *path,const char *name,const void *data,unsigned bytes)
{
    uint8_t request[2048],reply[2048];uint32_t size=0;
    if(!path)path="";if(!name)name="";
    struct w32r_request a={.version=1,.op=op,.key=key,.access=0xf003f,.type=op==W32R_SET?1:0,
        .path_bytes=(uint32_t)strlen(path)+1,.name_bytes=(uint32_t)strlen(name)+1,.data_bytes=bytes};
    memcpy(request,&a,sizeof(a));memcpy(request+sizeof(a),path,a.path_bytes);memcpy(request+sizeof(a)+a.path_bytes,name,a.name_bytes);
    if(bytes)memcpy(request+sizeof(a)+a.path_bytes+a.name_bytes,data,bytes);
    uint32_t error=desk_w32_exchange(request,sizeof(a)+a.path_bytes+a.name_bytes+bytes,reply,sizeof(reply),&size);
    if(error)fprintf(stderr,"RPC op=%u error=%u errno=%d\n",op,error,errno);assert(!error);
    assert(size>=sizeof(struct w32r_reply));struct w32r_reply *r=(void *)reply;assert(!r->error);return r->key;
}
static void set(uint32_t key,const char *name,const char *value)
{
    uint8_t utf16[1024];size_t n=strlen(value);assert(n<sizeof(utf16)/2);
    for(size_t i=0;i<=n;i++){utf16[i*2]=(uint8_t)value[i];utf16[i*2+1]=0;}
    rpc(W32R_SET,key,NULL,name,utf16,(unsigned)(n+1)*2);
}
int main(int argc,char **argv)
{
    assert(argc==3);binary=argv[1];directory=argv[2];
    struct desk_w32_supervisor s={0};struct desk_w32_supervisor_ops ops={spawn,record,NULL};
    uint64_t start=now();desk_w32_supervise(&s,&ops,start);assert(s.pid && now()-start<2000000000ull);
    uint32_t key=rpc(W32R_CREATE,W32R_HKLM,"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Fixture",NULL,NULL,0);
    set(key,"DisplayName","Fixture Application");set(key,"UninstallString","C:\\fixture.exe /remove");
    set(key,"DisplayVersion","1.0");set(key,"Publisher","Ciuki fixture");rpc(W32R_CLOSE,key,NULL,NULL,NULL,0);
    assert(!desk_w32_flush()); /* committed state before both hard failures */
    for(unsigned i=0;i<2;i++){
        int pid=(int)s.pid,status;assert(!kill(pid,i?SIGKILL:SIGSEGV));assert(waitpid(pid,&status,0)==pid);
        assert(WIFSIGNALED(status) && WTERMSIG(status)==(i?SIGKILL:SIGSEGV));
        start=now();assert(desk_w32_service_exited(&s,&ops,(uint32_t)pid,status,start));
        close(endpoint);endpoint=-1;struct desk_uninstall_entry list[32];assert(ciuki_win32_uninstall_list(list,32)==-EPIPE);
        desk_w32_supervise(&s,&ops,now());assert(s.pid!=(uint32_t)pid);
        assert(ciuki_win32_uninstall_list(list,32)==1);
        assert(!strcmp(list[0].name,"Fixture Application") && !strcmp(list[0].version,"1.0"));
        uint64_t elapsed=now()-start;assert(elapsed<2000000000ull);
        printf("PASS real w32srv %s: restart/register/reconnect/committed-key recovery in %llu ms\n",i?"SIGKILL":"crash",(unsigned long long)(elapsed/1000000));
    }
    assert(registrations==3 && records==2 && reconnects>10);
    assert(!desk_w32_flush());s.stopping=1;
    assert(!kill((int)s.pid,SIGTERM));int status;assert(waitpid((int)s.pid,&status,0)==(int)s.pid && WIFEXITED(status) && !WEXITSTATUS(status));
    close(endpoint);puts("PASS real w32srv flush before orderly shutdown; no server process left running");return 0;
}
