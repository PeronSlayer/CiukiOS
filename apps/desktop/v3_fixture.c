/* Runtime-only guest probe controller participant. The kernel observer owns
 * CIUKI_TEST framing and validates our PID/child/order. SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "gate.h"
#include "win32_adapter.h"
#include "../../src/probes/desktop_v3/controller.h"
#include "../../src/probes/win32_dialogs/controller.h"
#include "../../src/probes/win32_upper/controller.h"
#include "../../src/probes/win32_media/controller.h"
#include <ciuki/spawn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct {unsigned pid,phase,windows,token,mode,control;uint64_t deadline;int active,pending;} fixture;
void desk_v3_fixture_report(const char *);
int desk_v3_fixture_active(void){return fixture.active;}
static void report(struct desktop *d,int error)
{
    char line[192];
    snprintf(line,sizeof(line),"case=desktop-v3 phase=%u generation=%u windows=%u errors=%u child=%u",
        fixture.phase,d->v3_lease_generation,fixture.windows,error ? 1u : 0u,fixture.pid);
    desk_v3_fixture_report(line);
    fixture.deadline=d->now_ns+15000000000ull;
    if(error || fixture.phase==8)fixture.active=0;
}
int desk_v3_fixture_begin(struct desktop *d)
{
    memset(&fixture,0,sizeof(fixture));
    fixture.active=fixture.pending=1;fixture.deadline=d->now_ns+15000000000ull;
    return 0;
}
/* Registry fixture goes through precisely the copied w32srv endpoint. */
static int registry(unsigned op,unsigned key,const char *path,const char *name,const char *text,unsigned *handle)
{
    unsigned p=(unsigned)strlen(path)+1,n=(unsigned)strlen(name)+1,t=text ? ((unsigned)strlen(text)+1)*2 : 0;
    uint8_t input[768]={0},output[768];uint32_t used=0;
    if(sizeof(struct w32r_request)+p+n+t>sizeof(input))return -EOVERFLOW;
    struct w32r_request a={.version=1,.op=op,.key=key,.access=0xf003f,.type=op==W32R_SET ? 1u : 0u,.path_bytes=p,.name_bytes=n,.data_bytes=t};
    memcpy(input,&a,sizeof(a));memcpy(input+sizeof(a),path,p);memcpy(input+sizeof(a)+p,name,n);
    for(unsigned i=0;i<t/2;i++)input[sizeof(a)+p+n+i*2]=(uint8_t)text[i];
    uint32_t e=desk_w32_exchange(input,sizeof(a)+p+n+t,output,sizeof(output),&used);
    if(e || used<sizeof(struct w32r_reply))return -EIO;
    struct w32r_reply r;memcpy(&r,output,sizeof(r));
    if(r.version!=1 || r.error)return -EIO;
    if(handle)*handle=r.key;
    return 0;
}
static void cancel_dialog(struct desktop *d)
{
    d->v3_dialog_kind=0;d->v3_dialog_pid=0;
    for(int i=0;i<DESK_CLIENTS;i++)if(d->clients[i].internal==DESK_INTERNAL_MESSAGE)desk_request_close(d,i);
}
static int uninstall_fixture(void)
{
    const char *path="Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CiukiV3Fixture";
    unsigned h=0;int e=registry(W32R_CREATE,W32R_HKLM,path,"",NULL,&h);
    if(!e)e=registry(W32R_SET,h,"","DisplayName","Ciuki v3 fixture",NULL);
    if(!e)e=registry(W32R_SET,h,"","UninstallString","C:\\System\\Win32\\Fixtures\\desktop-v3.exe /remove",NULL);
    if(!e)e=registry(W32R_SET,h,"","DisplayVersion","1.0",NULL);
    if(!e)e=registry(W32R_SET,h,"","Publisher","CiukiOS",NULL);
    if(h){int close=registry(W32R_CLOSE,h,"","",NULL,NULL);if(!e)e=close;}
    if(e)return e;
    struct desk_uninstall_entry *entries=calloc(DESK_UNINSTALL_MAX,sizeof(*entries));if(!entries)return -ENOMEM;
    int count=ciuki_win32_uninstall_list(entries,DESK_UNINSTALL_MAX),found=0;
    for(int i=0;i<count;i++)if(!strcmp(entries[i].key,"CiukiV3Fixture") && !strcmp(entries[i].name,"Ciuki v3 fixture") &&
        !strcmp(entries[i].version,"1.0") && !strcmp(entries[i].publisher,"CiukiOS"))found=1;
    free(entries);
    return count<0 ? count : !found ? -ENOENT : 0;
}
void desk_v3_fixture_tick(struct desktop *d)
{
    if(!fixture.active)return;
    if(d->now_ns>=fixture.deadline){fixture.phase++;report(d,-ETIMEDOUT);return;}
    if(fixture.pending){
        unsigned key=0;int e=registry(W32R_OPEN,W32R_HKLM,"Software","",NULL,&key);
        if(!e && key)e=registry(W32R_CLOSE,key,"","",NULL,NULL);
        if(e)return;
        char *argv[]={"/System/Win32/Fixtures/desktop-v3.exe",NULL};
        char *env[]={"LC_ALL=C","TZ=UTC0","PATH=/Programs",NULL};
        int pid=ciuki_spawn(argv[0],argv,env,NULL,0,0);
        if(pid<0){report(d,-errno);return;}
        fixture.pid=(unsigned)pid;fixture.pending=0;
    }
    int holder=-1;unsigned windows=0;
    for(unsigned i=0;i<DESK_CLIENTS;i++)if(d->clients[i].pid==fixture.pid && d->clients[i].window && d->clients[i].version==3){
        windows++;if(d->clients[i].v3_id==4)holder=(int)i;
    }
    fixture.windows=windows;
    if(fixture.phase && windows!=4){fixture.phase++;report(d,-EIO);return;}
    if(fixture.phase==0 && windows==4 && holder>=0 && d->v3_lease_state==1 && d->v3_lease_pid==fixture.pid){
        fixture.phase=1;fixture.token=d->v3_lease_tuple.token;fixture.mode=d->v3_lease_tuple.mode;report(d,0);
    }
    if(fixture.phase==1 && holder>=0 && d->clients[holder].cursor==DESK_CURSOR_HAND && d->v3_pointer_window &&
        (d->clients[holder].v3_pointer&15)==15){fixture.phase=2;report(d,0);}
    if(fixture.phase==2 && !d->v3_lease_state && !d->v3_pointer_window){fixture.phase=3;report(d,0);}
    if(fixture.phase==3 && d->v3_lease_tuple.token>fixture.token){
        fixture.phase=4;report(d,d->v3_lease_tuple.mode!=fixture.mode);
        if(!fixture.active)return;
    }
    if(fixture.phase==4 && !d->v3_lease_state){
        struct shortcut {struct desk_launch_record record,decoded;uint8_t bytes[DESK_LINK_BYTES];};
        struct shortcut *s=calloc(1,sizeof(*s));int e=s ? uninstall_fixture() : -ENOMEM;
        if(!e) {
            s->record=(struct desk_launch_record){.label="Fixture shortcut",.target="/Programs/control",.arguments="--panel=programs",.show=1};
            unsigned n=desk_link_encode(&s->record,s->bytes,sizeof(s->bytes));
            e=!n || !desk_link_decode(s->bytes,n,&s->decoded) ? -EINVAL : desk_w32_native_shortcut(d,&s->decoded);
            if(e>0)fixture.control=(unsigned)e;
        }
        free(s);fixture.phase=5;report(d,e<0 ? e : 0);
        if(!fixture.active)return;
    }
    if(fixture.phase==5)for(unsigned i=0;i<DESK_CLIENTS;i++) {
        struct desk_client *c=&d->clients[i];
        if(c->pid==fixture.control && c->window && !c->awaiting_frame && c->program==DESK_APP_CONTROL) {
            fixture.phase=6;report(d,0);break;
        }
    }
    if(fixture.phase==6 && d->v3_dialog_kind==1 && d->v3_dialog_pid==fixture.pid){
        fixture.phase=7;report(d,0);cancel_dialog(d);
        if(holder>=0){struct ciuki_message m;desk_v3_message(&m,DESK_PING,4,0,4,1);desk_put32(m.data+16,0xf5090008);desk_queue(d,holder,&m);}
    }
    if(fixture.phase==7 && d->v3_dialog_kind==2 && d->v3_dialog_pid==fixture.pid){
        fixture.phase=8;
        int e=registry(W32R_DELETE_KEY,W32R_HKLM,"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CiukiV3Fixture","",NULL,NULL);
        report(d,e);cancel_dialog(d);
    }
}
/* f5-10C2 w32-dialogs: the desktop (already supervising w32srv) starts the
 * authored PE fixture and names it to the kernel observer, which validates
 * the child and reads the fixture's result block itself. GATE_DIALOGS
 * arrives with w32srv just spawned; the fixture's own startup connects to
 * the registry service and exits c0000142 if no server has registered yet
 * (first QEMU run). The spawn is therefore deferred to the tick until one
 * registry round trip succeeds, bounded by the kernel probe's wait. */
static int dialogs_active,dialogs_pending;
static int media_fixture;
static uint64_t dialogs_deadline;
int desk_dialogs_fixture_active(void){return dialogs_active;}
static int dialogs_spawn(void)
{
    char *argv[]={media_fixture ? W32_MEDIA_PATH : W32_DIALOGS_PATH,NULL};
    char *env[]={"LC_ALL=C","TZ=UTC0","PATH=/Programs",NULL};
    int pid=ciuki_spawn(argv[0],argv,env,NULL,0,0);
    char line[64];snprintf(line,sizeof(line),"case=%s child=%d",media_fixture ? "w32-media" : "w32-dialogs",pid<0 ? 0 : pid);
    desk_v3_fixture_report(line);
    return pid<0 ? -errno : pid;
}
int desk_dialogs_fixture_begin(struct desktop *d)
{
    media_fixture=0;
    dialogs_active=1;dialogs_pending=1;dialogs_deadline=d->now_ns+15000000000ull;
    return 0;
}
int desk_media_fixture_begin(struct desktop *d)
{
    desk_dialogs_fixture_begin(d); media_fixture=1; return 0;
}
void desk_dialogs_fixture_tick(struct desktop *d)
{
    if(!dialogs_pending)return;
    unsigned h=0;int e=registry(W32R_OPEN,W32R_HKLM,"Software","",NULL,&h);
    if(!e && h)registry(W32R_CLOSE,h,"","",NULL,NULL);
    if(e && d->now_ns<dialogs_deadline)return;
    dialogs_pending=0;
    if(e){desk_v3_fixture_report(media_fixture ? "case=w32-media child=0" : "case=w32-dialogs child=0");return;}
    dialogs_spawn();
}
/* f5-20D w32-upper-dlls: the same deferral as the dialogs fixture. The reused
 * Wine DLLs (comctl32, comdlg32, riched20, riched32) register window classes
 * in DllMain, which needs the user service registered, so the spawn waits
 * for one registry round trip to succeed (15 s bound). */
static int upper_active,upper_pending;
static uint64_t upper_deadline;
int desk_upper_fixture_active(void){return upper_active;}
static int upper_spawn(void)
{
    char *argv[]={W32_UPPER_PATH,NULL};
    char *env[]={"LC_ALL=C","TZ=UTC0","PATH=/Programs",NULL};
    int pid=ciuki_spawn(argv[0],argv,env,NULL,0,0);
    char line[64];snprintf(line,sizeof(line),"case=w32-upper-dlls child=%d",pid<0 ? 0 : pid);
    desk_v3_fixture_report(line);
    return pid<0 ? -errno : pid;
}
int desk_upper_fixture_begin(struct desktop *d)
{
    upper_active=1;upper_pending=1;upper_deadline=d->now_ns+15000000000ull;
    return 0;
}
void desk_upper_fixture_tick(struct desktop *d)
{
    if(!upper_pending)return;
    unsigned h=0;int e=registry(W32R_OPEN,W32R_HKLM,"Software","",NULL,&h);
    if(!e && h)registry(W32R_CLOSE,h,"","",NULL,NULL);
    if(e && d->now_ns<upper_deadline)return;
    upper_pending=0;
    if(e){desk_v3_fixture_report("case=w32-upper-dlls child=0");return;}
    upper_spawn();
}
