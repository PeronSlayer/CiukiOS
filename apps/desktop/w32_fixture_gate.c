/* Shared, runtime-only Win32 fixture launcher. SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "win32_adapter.h"
#include "../../src/probes/win32_core/controller.h"
#include "../../src/probes/win32_restart/controller.h"
#include <ciuki/spawn.h>
#include <ciuki/proc.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

void desk_v3_fixture_report(const char *);
static struct { unsigned id; int pid, active, pending; uint64_t deadline; } fixture;
/* Restart uses the same allowlisted gate, with a multi-instance coordinator. */
#define RESTART_FIXTURE_ID W32_FIXTURE_COUNT
static struct {unsigned active,pid,phase,resumed;uint64_t deadline,next;} restart_fixture;
static void restart_tick(struct desktop *);
int desk_w32_fixture_active(void) { return fixture.active; }
int desk_w32_fixture_begin(struct desktop *d,const char *name)
{
    if (fixture.active) return -EBUSY;
    unsigned id;
    for(id=0;id<W32_FIXTURE_COUNT;id++)
        if(!strcmp(name,w32_fixtures[id].name))break;
    if(id==W32_FIXTURE_COUNT && strcmp(name,"restart-exercise"))return -EINVAL;
    if(id==RESTART_FIXTURE_ID){
        memset(&restart_fixture,0,sizeof(restart_fixture));restart_fixture.active=1;
        restart_fixture.deadline=d->now_ns+360000000000ull;
    }
    fixture.id=id;fixture.active=1;fixture.pending=id!=RESTART_FIXTURE_ID;
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
static uint8_t processes[CIUKI_PROC_LIST_MIN+CIUKI_PROCESS_MAX*sizeof(struct ciuki_proc_record)];
static int process_list(void)
{
    int count=ciuki_proc_list(processes,sizeof(processes));
    struct ciuki_proc_system system;
    if(count<=0 || (unsigned)count>CIUKI_PROCESS_MAX)return 0;
    memcpy(&system,processes,sizeof(system));
    if(system.version!=CIUKI_PROC_VERSION || system.size!=sizeof(system) || system.records!=(unsigned)count)return 0;
    for(int i=0;i<count;i++) {
        struct ciuki_proc_record record;
        memcpy(&record,processes+sizeof(system)+(unsigned)i*sizeof(record),sizeof(record));
        if(record.version!=CIUKI_PROC_VERSION || record.size!=sizeof(record))return 0;
    }
    return count;
}
static int fixture_child(uint32_t pid)
{
    if(fixture.pid<=0)return 0;
    int count=process_list();
    for(int i=0;i<count;i++) {
        struct ciuki_proc_record record;
        memcpy(&record,processes+sizeof(struct ciuki_proc_system)+(unsigned)i*sizeof(record),sizeof(record));
        if(record.pid==pid)return record.ppid==(unsigned)fixture.pid;
    }
    return 0;
}
static void service_exits(struct desktop *d)
{
    /* Gate mode leaves the PE zombie for the kernel observer. Reap only the
     * desktop's service child, then use production supervision to restart it.
     * waitpid(-1) here could consume the fixture's required exit evidence. */
    int count=process_list();
    uint32_t parent=(uint32_t)getpid();
    for(int i=0;i<count;i++) {
        struct ciuki_proc_record record;
        memcpy(&record,processes+sizeof(struct ciuki_proc_system)+(unsigned)i*sizeof(record),sizeof(record));
        if(record.ppid==parent && record.state==CIUKI_PROC_ZOMBIE &&
           !memcmp(record.name,"w32srv",7)) {
            int status;
            if(waitpid((int)record.pid,&status,WNOHANG)==(int)record.pid)
                desk_w32_child_exited(d,record.pid,status);
        }
    }
}
void desk_w32_fixture_tick(struct desktop *d)
{
    if(!fixture.active)return;
    service_exits(d);
    if(fixture.id==RESTART_FIXTURE_ID){restart_tick(d);return;}
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

/* Read-only registry round trips for final queue-consumption evidence. */
static int registry_key(unsigned op,unsigned key,const char *path,unsigned *handle)
{
    uint8_t input[256]={0},output[128];uint32_t used=0;
    uint32_t size=(uint32_t)strlen(path)+1;
    if(sizeof(struct w32r_request)+size+1>sizeof(input))return -EOVERFLOW;
    struct w32r_request a={.version=W32R_VERSION,.op=op,.key=key,.access=0x20019,.path_bytes=size,.name_bytes=1};
    memcpy(input,&a,sizeof(a));memcpy(input+sizeof(a),path,size);
    uint32_t error=desk_w32_exchange(input,sizeof(a)+size+1,output,sizeof(output),&used);
    struct w32r_reply reply;
    if(error || used!=sizeof(reply))return -EIO;
    memcpy(&reply,output,sizeof(reply));
    if(reply.version!=W32R_VERSION || reply.error || (op==W32R_OPEN && !reply.key))return -EIO;
    if(handle)*handle=reply.key;
    return 0;
}
static uint32_t restart_service_pid(void)
{
    int count=process_list();uint32_t parent=(uint32_t)getpid();
    for(int i=0;i<count;i++){
        struct ciuki_proc_record record;
        memcpy(&record,processes+sizeof(struct ciuki_proc_system)+(unsigned)i*sizeof(record),sizeof(record));
        if(record.ppid==parent && record.state==CIUKI_PROC_LIVE && !memcmp(record.name,"w32srv",7))return record.pid;
    }
    return 0;
}
/* Only confirmation and phased relaunch are automated. The production
 * desktop/session coordinator performs every restart and journal operation. */
extern int desk_restart_process_alive(uint32_t);
void desk_restart_fixture_notice(unsigned kind,uint32_t pid,uint32_t flags)
{
    if(!restart_fixture.active)return;
    if(kind==2)restart_fixture.resumed=0;
    if(kind==3)restart_fixture.resumed=1;
    char line[128];snprintf(line,sizeof(line),"case=w32-restart notice=%u child=%u flags=%u",kind,pid,flags);
    desk_v3_fixture_report(line);
}
static unsigned restart_registry_consumed(void)
{
    static const char *const names[]={"F507D-S","F507D-M","F507D-U","F507D-R","F507D-T"};
    static const unsigned queues[]={0,1,2,0,1};
    unsigned absent=0;
    for(unsigned i=0;i<5;i++){
        unsigned key=0;const char *path=queues[i]==0?"Software\\Microsoft\\Windows\\CurrentVersion\\RunServicesOnce":"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
        if(registry_key(W32R_OPEN,queues[i]==2?W32R_HKCU:W32R_HKLM,path,&key))return 0;
        uint8_t input[128]={0},output[2048];uint32_t used=0;
        struct w32r_request a={.version=1,.op=W32R_QUERY,.key=key,.path_bytes=1,.name_bytes=(uint32_t)strlen(names[i])+1};
        memcpy(input,&a,sizeof(a));memcpy(input+sizeof(a)+1,names[i],a.name_bytes);
        uint32_t e=desk_w32_exchange(input,sizeof(a)+1+a.name_bytes,output,sizeof(output),&used);
        struct w32r_reply reply={0};if(!e&&used==sizeof(reply))memcpy(&reply,output,sizeof(reply));
        int close=registry_key(W32R_CLOSE,key,"",NULL);
        if(e||close||reply.version!=1||reply.error!=2)return 0;
        absent++;
    }
    return absent;
}
static void restart_tick(struct desktop *d)
{
    if(!restart_fixture.active || d->now_ns<restart_fixture.next)return;
    restart_fixture.next=d->now_ns+20000000ull;
    if(d->now_ns>=restart_fixture.deadline){desk_v3_fixture_report("case=w32-restart failed=1");restart_fixture.active=0;return;}
    unsigned service_pid=restart_service_pid();
    if(restart_fixture.pid && d->v3_dialog_kind==2 && d->v3_dialog_pid==restart_fixture.pid){
        unsigned pid=d->v3_dialog_pid,flags=d->v3_restart_flags;
        d->v3_dialog_kind=0;d->v3_dialog_pid=0;
        for(int i=0;i<DESK_CLIENTS;i++)if(d->clients[i].internal==DESK_INTERNAL_MESSAGE)desk_request_close(d,i);
        if(!d->v3_restart || d->v3_restart(d,pid,flags)<0){desk_v3_fixture_report("case=w32-restart failed=1");restart_fixture.active=0;return;}
    }
    /* Query the actual service directory on every new launch (including
     * recovery). An alive native PID is not a registry readiness check. */
    if(restart_fixture.pid){
        if(!restart_fixture.resumed || desk_restart_process_alive(restart_fixture.pid))return;
        char line[128];snprintf(line,sizeof(line),"case=w32-restart exited=%u phase=%u",restart_fixture.pid,restart_fixture.phase);
        desk_v3_fixture_report(line); /* kernel observes the zombie before reap */
        int status;if(waitpid((int)restart_fixture.pid,&status,WNOHANG)!=(int)restart_fixture.pid || !WIFEXITED(status)||WEXITSTATUS(status)){
            desk_v3_fixture_report("case=w32-restart failed=1");restart_fixture.active=0;return;
        }
        restart_fixture.pid=0;restart_fixture.phase++;restart_fixture.resumed=0;
    }
    if(!service_pid || !ready())return;
    if(restart_fixture.phase==3){
        char line[64];snprintf(line,sizeof(line),"case=w32-restart registry=%u",restart_registry_consumed());desk_v3_fixture_report(line);
    }
    char *argv[]={W32_RESTART_PATH,NULL};char *env[]={"LC_ALL=C","TZ=UTC0",NULL};
    int pid=ciuki_spawn(argv[0],argv,env,NULL,0,CIUKI_SPAWN_NEW_GROUP);
    char line[128];snprintf(line,sizeof(line),"case=w32-restart child=%u phase=%u service=%u",pid>0?(unsigned)pid:0,restart_fixture.phase,service_pid);
    desk_v3_fixture_report(line);if(pid<=0){restart_fixture.active=0;return;}
    restart_fixture.pid=(unsigned)pid;
}
