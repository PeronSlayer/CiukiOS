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
    struct ciuki_proc_system system={.version=CIUKI_PROC_VERSION,.records=1};
    struct ciuki_proc_record child={.pid=72,.ppid=71};
    assert(size>=sizeof(system)+sizeof(child));
    memcpy(out,&system,sizeof(system));memcpy((char *)out+sizeof(system),&child,sizeof(child));return 1;
}
#include "../../../apps/desktop/w32_fixture_gate.c"
static void reset(void)
{memset(&fixture,0,sizeof(fixture));service_ready=close_error=malformed=spawns=closes=accepted=0;spawn_error=0;record[0]=0;}
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
    puts("PASS shared gate: ten names, OPEN+CLOSE, timeout, spawn failure, child-bound crash close");return 0;
}
