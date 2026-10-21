/* Shared, runtime-only Win32 fixture launcher. SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "win32_adapter.h"
#include "../../src/probes/win32_core/controller.h"
#include <ciuki/spawn.h>
#include <ciuki/proc.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

void desk_v3_fixture_report(const char *);
static struct { unsigned id; int pid, active, pending; uint64_t deadline; } fixture;
int desk_w32_fixture_active(void) { return fixture.active; }
int desk_w32_fixture_begin(struct desktop *d,const char *name)
{
    if (fixture.active) return -EBUSY;
    unsigned id;
    for(id=0;id<W32_FIXTURE_COUNT;id++)
        if(!strcmp(name,w32_fixtures[id].name))break;
    if(id==W32_FIXTURE_COUNT)return -EINVAL;
    fixture.id=id;fixture.active=fixture.pending=1;
    fixture.deadline=d->now_ns+15000000000ull;
    return 0;
}
static void report(int pid)
{
    char line[112];
    snprintf(line,sizeof(line),"case=w32-fixture fixture=%u child=%d",fixture.id,pid>0 ? pid : 0);
    desk_v3_fixture_report(line);
}
/* OPEN and CLOSE must both complete successfully. Merely starting w32srv
 * does not mean that its copied registry endpoint is accepting requests. */
static int ready(void)
{
    unsigned char input[sizeof(struct w32r_request)+10]={0},output[128];
    struct w32r_request a={.version=W32R_VERSION,.op=W32R_OPEN,.key=W32R_HKLM,
        .access=0x20019,.path_bytes=9,.name_bytes=1};
    memcpy(input,&a,sizeof(a));memcpy(input+sizeof(a),"Software",9);
    uint32_t used=0;
    uint32_t error=desk_w32_exchange(input,sizeof(input),output,sizeof(output),&used);
    struct w32r_reply reply;
    if(error || used!=sizeof(reply))return 0;
    memcpy(&reply,output,sizeof(reply));
    if(reply.version!=W32R_VERSION || reply.error || !reply.key)return 0;
    a.op=W32R_CLOSE;a.key=reply.key;a.path_bytes=a.name_bytes=1;
    memset(input,0,sizeof(input));memcpy(input,&a,sizeof(a));
    used=0;error=desk_w32_exchange(input,sizeof(a)+2,output,sizeof(output),&used);
    if(error || used!=sizeof(reply))return 0;
    memcpy(&reply,output,sizeof(reply));
    return reply.version==W32R_VERSION && !reply.error;
}
static int fixture_child(uint32_t pid)
{
    if(fixture.pid<=0)return 0;
    static uint8_t bytes[CIUKI_PROC_LIST_MIN+CIUKI_PROCESS_MAX*sizeof(struct ciuki_proc_record)];
    int count=ciuki_proc_list(bytes,sizeof(bytes));
    struct ciuki_proc_system system;
    if(count<=0 || (unsigned)count>CIUKI_PROCESS_MAX)return 0;
    memcpy(&system,bytes,sizeof(system));
    if(system.version!=CIUKI_PROC_VERSION || system.records!=(unsigned)count)return 0;
    for(int i=0;i<count;i++) {
        struct ciuki_proc_record record;
        memcpy(&record,bytes+sizeof(system)+(unsigned)i*sizeof(record),sizeof(record));
        if(record.pid==pid)return record.ppid==(unsigned)fixture.pid;
    }
    return 0;
}
void desk_w32_fixture_tick(struct desktop *d)
{
    /* The authored crash coordinator waits for each child's real unhandled
     * fault and the production crash-file worker. Accept only its own fault
     * dialogs in this runtime gate; the normal desktop remains interactive. */
    if(fixture.active && fixture.id==W32_FIXTURE_CRASH && d->v3_dialog_kind==1 && fixture_child(d->v3_dialog_pid)) {
        (void)desk_v3_dialog_accept(d);
        for(unsigned i=0;i<DESK_CLIENTS;i++)
            if(d->clients[i].internal==DESK_INTERNAL_MESSAGE)desk_request_close(d,(int)i);
    }
    if(!fixture.pending)return;
    if(d->now_ns>=fixture.deadline){fixture.pending=0;report(0);return;}
    if(!ready())return;
    fixture.pending=0;
    char *argv[]={(char *)w32_fixtures[fixture.id].path,NULL};
    char *env[]={"LC_ALL=C","TZ=UTC0","PATH=/Programs",NULL};
    fixture.pid=ciuki_spawn(argv[0],argv,env,NULL,0,0);
    report(fixture.pid);
}
