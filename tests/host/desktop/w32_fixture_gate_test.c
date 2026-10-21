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
static unsigned restart_mode,restart_live,restart_calls,restart_reaps,queries;
static unsigned installer_begins,installer_ticks,installer_active;
static int installer_error;
int desk_installer_fixture_active(void){return (int)installer_active;}
int desk_installer_fixture_begin(struct desktop *d)
{(void)d;installer_begins++;installer_active=!installer_error;return installer_error;}
void desk_installer_fixture_tick(struct desktop *d){(void)d;installer_ticks++;}
static struct ciuki_proc_record listed[4];
static unsigned listed_count;
static char record[128],spawn_path[128];
uint32_t desk_w32_exchange(const void *input,uint32_t n,void *output,uint32_t cap,uint32_t *used)
{
    const struct w32r_request *a=input;
    assert(n>=sizeof(*a)+2 && cap>=sizeof(struct w32r_reply));
    if(!service_ready)return 1062;
    struct w32r_reply reply={.version=malformed ? 99 : 1};
    if(a->op==W32R_OPEN){assert(a->key==W32R_HKLM || (restart_mode && a->key==W32R_HKCU));reply.key=7;}
    else if(a->op==W32R_QUERY){assert(restart_mode && a->key==7);queries++;reply.error=2;}
    else {assert(a->op==W32R_CLOSE && a->key==7);closes++;reply.error=close_error;}
    memcpy(output,&reply,sizeof(reply));*used=sizeof(reply);return 0;
}
int ciuki_spawn(const char *path,char *const argv[],char *const env[],const struct ciuki_spawn_fd *fds,uint32_t n,uint32_t flags)
{
    assert(service_ready && !close_error && !malformed && closes && argv && env && !fds && !n);
    assert(flags==(restart_mode ? CIUKI_SPAWN_NEW_GROUP : 0));
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
    if(restart_mode && pid==71){assert(flags==WNOHANG && strstr(record,"exited=71"));restart_reaps++;*status=0;return pid;}
    assert(pid==74 && flags==WNOHANG);waits++;
    if(wait_error)return wait_error;
    *status=137;listed[1].state=CIUKI_PROC_LIVE;return pid;
}
void desk_w32_child_exited(struct desktop *d,uint32_t pid,int status)
{(void)d;assert(pid==74 && status==137);service_exited++;}
int desk_restart_process_alive(uint32_t pid){assert(pid==71);return restart_live;}
static int restart_call(struct desktop *d,uint32_t pid,uint32_t flags)
{(void)d;assert(pid==71 && flags==2);restart_calls++;return 0;}
#include "../../../apps/desktop/w32_fixture_gate.c"
#undef getpid
#undef waitpid
static void reset(void)
{
    memset(&fixture,0,sizeof(fixture));service_ready=close_error=malformed=spawns=closes=accepted=0;spawn_error=0;record[0]=0;
    service_exited=list_bad=waits=0;wait_error=0;listed_count=4;
    memset(&restart_fixture,0,sizeof(restart_fixture));
    restart_mode=restart_live=restart_calls=restart_reaps=queries=0;
    installer_begins=installer_ticks=installer_active=0;installer_error=0;
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
    reset();d.now_ns=0;
    assert(desk_w32_fixture_begin(&d,"gl-exercise.exe")==-EINVAL);
    assert(!desk_w32_fixture_begin(&d,"gl-exercise"));
    assert(fixture.id==W32_FIXTURE_GL && desk_w32_fixture_active());
    assert(desk_w32_fixture_begin(&d,"restart-exercise")==-EBUSY);
    desk_w32_fixture_tick(&d);assert(!spawns);
    service_ready=1;close_error=5;desk_w32_fixture_tick(&d);assert(!spawns);
    close_error=0;malformed=1;desk_w32_fixture_tick(&d);assert(!spawns);
    malformed=0;desk_w32_fixture_tick(&d);
    assert(spawns==1 && !strcmp(spawn_path,W32_GL_PATH));
    char expected[64];snprintf(expected,sizeof(expected),"case=w32-fixture fixture=%u child=71",W32_FIXTURE_GL);
    assert(!strcmp(record,expected));
    desk_w32_fixture_tick(&d);assert(spawns==1 && !waits);
    reset();d.now_ns=0;assert(!desk_w32_fixture_begin(&d,"gl-exercise"));
    d.now_ns=15000000000ull;desk_w32_fixture_tick(&d);assert(!spawns && strstr(record,"child=0"));
    service_ready=1;desk_w32_fixture_tick(&d);assert(!spawns);
    reset();d.now_ns=0;service_ready=1;spawn_error=1;
    assert(!desk_w32_fixture_begin(&d,"gl-exercise"));desk_w32_fixture_tick(&d);
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
    reset();restart_mode=1;d.now_ns=0;d.v3_restart=restart_call;
    assert(desk_w32_fixture_begin(&d,"restart-exercise.exe")==-EINVAL);
    assert(!desk_w32_fixture_begin(&d,"restart-exercise"));
    assert(desk_w32_fixture_active() && desk_w32_fixture_begin(&d,"kernel32-exercise")==-EBUSY);
    desk_w32_fixture_tick(&d);assert(!spawns);
    service_ready=1;close_error=5;d.now_ns+=20000000ull;
    desk_w32_fixture_tick(&d);assert(!spawns);
    close_error=0;d.now_ns+=20000000ull;desk_w32_fixture_tick(&d);
    assert(spawns==1 && !strcmp(spawn_path,W32_RESTART_PATH) && strstr(record,"phase=0 service=74"));
    for(unsigned phase=0;phase<3;phase++){
        restart_live=1;d.v3_dialog_kind=2;d.v3_dialog_pid=99;d.v3_restart_flags=2;
        d.now_ns+=20000000ull;desk_w32_fixture_tick(&d);assert(restart_calls==phase);
        d.v3_dialog_pid=71;d.now_ns+=20000000ull;desk_w32_fixture_tick(&d);
        assert(restart_calls==phase+1 && !d.v3_dialog_kind);
        desk_restart_fixture_notice(2,71,2);desk_restart_fixture_notice(3,71,2);
        d.now_ns+=20000000ull;desk_w32_fixture_tick(&d);assert(spawns==phase+1 && restart_reaps==phase);
        restart_live=0;d.now_ns+=20000000ull;desk_w32_fixture_tick(&d);
        assert(spawns==phase+2 && restart_reaps==phase+1 && restart_fixture.phase==phase+1);
    }
    assert(queries==5 && strstr(record,"phase=3 service=74"));
    assert(W32_RESTART_RESULT_VA==0x100d0000u);
    for(unsigned i=0;i<W32_FIXTURE_COUNT;i++)assert(W32_RESTART_RESULT_VA!=w32_fixtures[i].address);
    assert(W32_GL_RESULT_VA==0x100e0000u && W32_GL_RESULT_VA!=W32_RESTART_RESULT_VA);
    assert(W32_FIXTURE_GL!=RESTART_FIXTURE_ID);
    for(unsigned i=0;i<W32_FIXTURE_COUNT;i++)assert(W32_GL_RESULT_VA!=w32_fixtures[i].address);
    reset();restart_mode=service_ready=1;d.now_ns=0;
    assert(!desk_w32_fixture_begin(&d,"restart-exercise"));
    d.now_ns=360000000000ull;desk_w32_fixture_tick(&d);assert(!spawns && strstr(record,"failed=1"));
    reset();d.now_ns=0;installer_error=-ENOMEM;
    assert(desk_w32_fixture_begin(&d,INSTALLER_FIXTURE_NAME)==-ENOMEM && !desk_w32_fixture_active());
    installer_error=0;assert(!desk_w32_fixture_begin(&d,INSTALLER_FIXTURE_NAME));
    assert(installer_begins==2 && desk_w32_fixture_active());
    assert(fixture.id==W32_FIXTURE_INSTALLER && !fixture.pending);
    assert(W32_FIXTURE_INSTALLER!=W32_FIXTURE_RESTART && W32_FIXTURE_INSTALLER!=W32_FIXTURE_GL);
    assert(desk_w32_fixture_begin(&d,"kernel32-exercise")==-EBUSY);
    desk_w32_fixture_tick(&d);assert(installer_ticks==1 && !spawns);
    installer_active=0;desk_w32_fixture_tick(&d);assert(!desk_w32_fixture_active());
    assert(INSTALLER_RESULT_VA==0x100f0000u && INSTALLER_RESULT_VA!=W32_RESTART_RESULT_VA);
    assert(INSTALLER_RESULT_VA!=W32_GL_RESULT_VA);
    for(unsigned i=0;i<W32_FIXTURE_COUNT;i++)assert(INSTALLER_RESULT_VA!=w32_fixtures[i].address);
    puts("PASS shared gate: ten core names plus restart, GL and installer, OPEN+CLOSE, timeout, spawn failure, distinct result pages, child-bound dialogs, service-only reaping, phased restart and registry consumption");return 0;
}
