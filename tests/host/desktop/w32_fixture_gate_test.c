/* Real shared gate with mocked copied registry transport/spawn. MIT */
#include "desktop.h"
#include "win32_adapter.h"
#include <ciuki/spawn.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static unsigned service_ready,close_error,malformed,spawns,closes,accepted;
static int spawn_error;
static unsigned service_exited,list_bad,waits;
static int wait_error;
static struct ciuki_proc_record listed[4];
static unsigned listed_count;
static char record[128],spawn_path[128];
uint32_t desk_w32_exchange(const void *input,uint32_t n,void *output,uint32_t cap,uint32_t *used)
{
    const struct w32r_request *a=input;
    assert(n>=sizeof(*a)+2 && cap>=sizeof(struct w32r_reply));
    if(!service_ready)return 1062;
    struct w32r_reply reply={.version=malformed ? 99 : 1};
    if(a->op==W32R_OPEN){assert(a->key==W32R_HKLM);reply.key=7;}
    else {assert(a->op==W32R_CLOSE && a->key==7);closes++;reply.error=close_error;}
    memcpy(output,&reply,sizeof(reply));*used=sizeof(reply);return 0;
}
int ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *fds,uint32_t n,uint32_t flags)
{
    assert(service_ready && !close_error && !malformed && closes && argv && env && !fds && !n && !flags);
    assert(!strcmp(argv[0],path) && !argv[1]);strcpy(spawn_path,path);spawns++;return spawn_error ? -1 : 71;
}
void desk_v3_fixture_report(const char *line){assert(strlen(line)<sizeof(record));strcpy(record,line);}
int desk_v3_dialog_accept(struct desktop *d){accepted++;d->v3_dialog_kind=0;return 0;}
void desk_request_close(struct desktop *d,int i){(void)d;(void)i;}
int ciuki_proc_list(void *out,uint32_t size)
{
    struct ciuki_proc_system system={.version=CIUKI_PROC_VERSION,.size=sizeof(system),.records=listed_count};
    if(list_bad)system.size=0;
    assert(size>=sizeof(system)+listed_count*sizeof(*listed));
    memcpy(out,&system,sizeof(system));memcpy((char *)out+sizeof(system),listed,listed_count*sizeof(*listed));return (int)listed_count;
}
#define getpid fixture_getpid
#define waitpid fixture_waitpid
static int fixture_getpid(void){return 70;}
static int fixture_waitpid(int pid,int *status,int flags)
{
    assert(pid==74 && flags==WNOHANG);waits++;
    if(wait_error)return wait_error;
    *status=137;listed[1].state=CIUKI_PROC_LIVE;return pid;
}
void desk_w32_child_exited(struct desktop *d,uint32_t pid,int status)
{(void)d;assert(pid==74 && status==137);service_exited++;}
#include "../../../apps/desktop/w32_fixture_gate.c"
#undef getpid
#undef waitpid
static void reset(void)
{
    memset(&fixture,0,sizeof(fixture));service_ready=close_error=malformed=spawns=closes=accepted=0;spawn_error=0;record[0]=0;
    service_exited=list_bad=waits=0;wait_error=0;listed_count=4;
    listed[0]=(struct ciuki_proc_record){.pid=72,.ppid=71}; /* crash child */
    listed[1]=(struct ciuki_proc_record){.pid=74,.ppid=70,.state=CIUKI_PROC_LIVE,.name="w32srv"};
    listed[2]=(struct ciuki_proc_record){.pid=71,.ppid=70,.state=CIUKI_PROC_ZOMBIE,.name="registry-exercise.exe"};
    listed[3]=(struct ciuki_proc_record){.pid=75,.ppid=99,.state=CIUKI_PROC_ZOMBIE,.name="w32srv"};
    for(unsigned i=0;i<listed_count;i++){listed[i].version=CIUKI_PROC_VERSION;listed[i].size=sizeof(*listed);}
}
int main(void)
{
    setbuf(stdout,NULL);static struct desktop d;
    reset();assert(desk_w32_fixture_begin(&d,"../bad") == -EINVAL);
    assert(desk_w32_fixture_begin(&d,"kernel32-exercise.exe") == -EINVAL);
    for(unsigned i=0;i<W32_FIXTURE_COUNT;i++) {
        reset();d.now_ns=0;
        assert(!desk_w32_fixture_begin(&d,w32_fixtures[i].name));
        assert(desk_w32_fixture_begin(&d,w32_fixtures[i].name)==-EBUSY);
        desk_w32_fixture_tick(&d);assert(!spawns && !record[0]);
        service_ready=1;close_error=5;desk_w32_fixture_tick(&d);assert(!spawns);
        close_error=0;malformed=1;desk_w32_fixture_tick(&d);assert(!spawns);
        malformed=0;desk_w32_fixture_tick(&d);
        assert(spawns==1 && !strcmp(spawn_path,w32_fixtures[i].path));
        assert(strstr(record,"child=71"));
        desk_w32_fixture_tick(&d);assert(spawns==1);
    }
    reset();assert(!desk_w32_fixture_begin(&d,"kernel32-exercise"));
    d.now_ns=15000000000ull;desk_w32_fixture_tick(&d);assert(!spawns && strstr(record,"child=0"));
    service_ready=1;desk_w32_fixture_tick(&d);assert(!spawns);
    reset();d.now_ns=0;service_ready=1;spawn_error=1;
    assert(!desk_w32_fixture_begin(&d,"kernel32-exercise"));desk_w32_fixture_tick(&d);
    assert(spawns==1 && strstr(record,"child=0"));
    reset();service_ready=1;assert(!desk_w32_fixture_begin(&d,"crash-exercise"));desk_w32_fixture_tick(&d);
    d.v3_dialog_kind=1;d.v3_dialog_pid=73;desk_w32_fixture_tick(&d);assert(!accepted);
    d.v3_dialog_pid=72;desk_w32_fixture_tick(&d);assert(accepted==1 && !d.v3_dialog_kind);
    reset();service_ready=1;assert(!desk_w32_fixture_begin(&d,"registry-exercise"));desk_w32_fixture_tick(&d);
    assert(!waits && !service_exited);
    listed[1].state=CIUKI_PROC_ZOMBIE;list_bad=1;
    desk_w32_fixture_tick(&d);assert(!waits && !service_exited);
    list_bad=0;listed[1].version=99;
    desk_w32_fixture_tick(&d);assert(!waits && !service_exited);
    listed[1].version=CIUKI_PROC_VERSION;wait_error=-1;
    desk_w32_fixture_tick(&d);assert(waits==1 && !service_exited);
    wait_error=0;desk_w32_fixture_tick(&d);assert(waits==2 && service_exited==1 && spawns==1);
    desk_w32_fixture_tick(&d);assert(waits==2 && service_exited==1);
    listed[1].state=CIUKI_PROC_ZOMBIE;desk_w32_fixture_tick(&d);assert(waits==3 && service_exited==2);
    assert(listed[2].state==CIUKI_PROC_ZOMBIE); /* PE exit remains kernel-owned. */
    puts("PASS shared gate: ten names, OPEN+CLOSE, timeout, spawn failure, child-bound crash close, service-only reaping/restarts");return 0;
}
