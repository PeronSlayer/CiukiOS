/* Authored oracles, production dispatcher/filesystem and desktop driver.
 * SPDX-License-Identifier: MIT */
#include "../../../../apps/w32srv/shell.h"
#include "../../../../apps/desktop/win32_restart.h"
#include "../../../../apps/desktop/shell_link.h"
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#undef _SC_PAGESIZE
#undef _SC_OPEN_MAX
#undef _SC_ARG_MAX
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include <unistd.h>
static struct wr_server server;
static struct ws_shell *shell;
static struct ws_fs fs;
static struct wr_io io;
static char root[1024],programs[1024];
static unsigned logs,fail_commit,fail_journal;
static int (*real_sync)(void *,const char *);
static int (*real_write)(void *,const char *,const void *,size_t);
static int (*real_rename)(void *,const char *,const char *);
static void log_line(void *ctx,const char *s){(void)ctx;logs++;puts(s);}
static int sync_fault(void *ctx,const char *path){if(fail_commit)return -1;return real_sync(ctx,path);}
static int write_fault(void *ctx,const char *path,const void *b,size_t n){if(fail_journal && strstr(path,"CIUKIRST"))return EIO;return real_write(ctx,path,b,n);}
static int rename_fault(void *ctx,const char *from,const char *to){if(strstr(to,"readonly"))return EACCES;return real_rename(ctx,from,to);}
static void file(const char *name,const char *text)
{char p[2048];snprintf(p,sizeof(p),"%s/%s",root,name);assert(!fs.write(fs.opaque,p,text,strlen(text)));}
static int present(const char *name)
{char p[2048];int dir;uint64_t n;snprintf(p,sizeof(p),"%s/%s",root,name);return !fs.stat(fs.opaque,p,&dir,&n);}
static uint32_t rpc(uint32_t pid,unsigned op,unsigned index,uint32_t key,uint32_t type,uint32_t flags,
                    const char *path,const char *name,const void *data,unsigned n,struct w32r_reply *reply,void *out,size_t cap)
{
    size_t p=strlen(path)+1,m=strlen(name)+1,total=sizeof(struct w32r_request)+p+m+n;
    uint8_t *wire=malloc(total),*response=malloc(W32R_MAX);assert(wire&&response);
    struct w32r_request r={.version=1,.op=op,.index=index,.key=key,.type=type,.access=(op==W32R_OPEN||op==W32R_CREATE)?0xf003f:0,.flags=flags,.path_bytes=p,.name_bytes=m,.data_bytes=n};
    memcpy(wire,&r,sizeof(r));memcpy(wire+sizeof(r),path,p);memcpy(wire+sizeof(r)+p,name,m);
    if(n)memcpy(wire+sizeof(r)+p+m,data,n);
    size_t used=0;assert(!wr_dispatch(&server,pid,wire,total,response,W32R_MAX,&used));
    assert(used>=sizeof(*reply));memcpy(reply,response,sizeof(*reply));
    if(!reply->error && reply->data_bytes){assert(reply->data_bytes<=cap);memcpy(out,response+sizeof(*reply),reply->data_bytes);}
    free(wire);free(response);return reply->error;
}
static unsigned open_conv(uint32_t pid,const char *app,const char *topic)
{struct w32r_reply r;assert(!rpc(pid,W32R_PROGMAN_OPEN,0,0,0,0,app,topic,NULL,0,&r,NULL,0));assert(r.key);return r.key;}
static unsigned exec(uint32_t pid,unsigned id,const char *text,unsigned expected)
{struct w32r_reply r;assert(!rpc(pid,W32R_PROGMAN_EXECUTE,id,0,0,0,text,"",NULL,0,&r,NULL,0));printf("TRANSCRIPT pid=%u execute=%s ack=%04x commands=%u\n",pid,text,r.key,r.type);assert(r.key==expected);return r.type;}
static void dde(void)
{
    unsigned a=open_conv(10,"PROGMAN","PROGMAN"),b=open_conv(11,"Shell","AppProperties");
    struct w32r_reply r;
    assert(rpc(10,W32R_PROGMAN_OPEN,0,0,0,0,"other","PROGMAN",NULL,0,&r,NULL,0)==2);
    char target[1500];snprintf(target,sizeof(target),"%s/tool.exe",root);file("tool.exe","MZ");
    exec(10,a,"[CreateGroup(\"Games, old\")][ShowGroup(\"Games, old\",1)]",0x8000);
    exec(11,b,"[AddItem(no.exe)]",0);
    char command[4096];snprintf(command,sizeof(command),"[AddItem(\"%s -x\",\"Tool, one\",icon.ico,2,3,4,,0,1)]",target);
    exec(10,a,command,0x8000);
    char linkpath[2048];snprintf(linkpath,sizeof(linkpath),"%s/Games, old/Tool, one.lnk",programs);
    uint8_t *bytes;size_t n;assert(!fs.read(fs.opaque,linkpath,&bytes,&n,4096));
    struct desk_launch_record link;assert(desk_link_decode(bytes,n,&link));assert(!strcmp(link.arguments,"-x")&&link.show==7&&link.icon_index==2);
    uint8_t encoded[4096];assert(desk_link_encode(&link,encoded,sizeof(encoded))==n&&!memcmp(encoded,bytes,n));free(bytes);
    char out[4096];assert(!rpc(10,W32R_PROGMAN_REQUEST,a,0,0,0,"","Groups",NULL,0,&r,out,sizeof(out)));
    assert(!strcmp(out,"Games, old\r\n"));
    assert(!rpc(10,W32R_PROGMAN_REQUEST,a,0,0,0,"","Games, old",NULL,0,&r,out,sizeof(out)) && strstr(out,"\"Tool, one\",\"") && strstr(out,",1,3\r\n"));
    exec(10,a,"[ReplaceItem(\"Tool, one\")][Reload(\"Games, old\")][ExitProgman(1)]",0x8000);
    snprintf(command,sizeof(command),"[AddItem(\"%s\",Two)][DeleteItem(Two)]",target);exec(10,a,command,0x8000);
    const char *bad[]={"[", "[AddItem(\"abc)]", "[CreateGroup()", "[ShowGroup(\"Games, old\")]", "[Bogus(x)]", "[AddItem(missing.exe)]", "[DeleteItem(no)]", "[CreateGroup(..)]"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)exec(10,a,bad[i],0);
    exec(10,a,"[DeleteGroup(\"Games, old\")]",0x8000);
    assert(rpc(11,W32R_PROGMAN_EXECUTE,a,0,0,0,"[CreateGroup(X)]","",NULL,0,&r,NULL,0)==6);
    assert(!rpc(10,W32R_PROGMAN_CLOSE,a,0,0,0,"","",NULL,0,&r,NULL,0));
    assert(rpc(10,W32R_PROGMAN_EXECUTE,a,0,0,0,"[CreateGroup(X)]","",NULL,0,&r,NULL,0)==6);
    wr_exit(&server,11);assert(rpc(11,W32R_PROGMAN_REQUEST,b,0,0,0,"","Groups",NULL,0,&r,out,sizeof(out))==6);
    unsigned c=open_conv(10,"PROGMAN","PROGMAN");server.now+=300001;open_conv(12,"PROGMAN","PROGMAN");
    assert(rpc(10,W32R_PROGMAN_CLOSE,c,0,0,0,"","",NULL,0,&r,NULL,0)==6);
    puts("PASS DDE command transcripts malformed ownership timeout exact link encoder");
}
static void drive(void){for(unsigned i=0;ws_restart_phase(shell)==1&&i<600;i++)ws_restart_tick(shell);assert(ws_restart_phase(shell)==2);}
static void recreate(void)
{ws_destroy(shell);struct ws_config config={programs,root};shell=ws_create(&fs,&config,&server);assert(shell);server.shell=shell;}
static void finish_restart(void)
{char command[2048];uint32_t type,n;assert(ws_restart_next(shell,command,sizeof(command),&type,&n)==259);assert(!ws_restart_end(shell));}
static void wininit(void)
{
    file("source long.txt","copy");file("SHORT~1.TXT","short");file("delete.txt","delete");file("readonly","old");file("locked.txt","new");
    file("protected-delete","keep");char protected_path[2048];snprintf(protected_path,sizeof(protected_path),"%s/protected-delete",root);assert(!chmod(protected_path,0444));
    file("WININIT.INI","[rename]\nNUL=protected-delete\nfirst.txt=source long.txt\nfinal.txt=first.txt\nNUL=delete.txt\nshort.txt=SHORT~1.TXT\nreadonly=locked.txt\nmissing.txt=absent\n../escape=locked.txt\n");
    assert(!ws_restart_begin(shell,2));assert(ws_restart_end(shell)==170);
    ws_restart_tick(shell);assert(present("protected-delete"));
    ws_restart_tick(shell);assert(present("first.txt")&&!present("source long.txt"));
    recreate();assert(ws_restart_phase(shell)==1);drive();
    assert(present("final.txt")&&!present("first.txt")&&!present("delete.txt")&&present("short.txt")&&present("locked.txt"));
    assert(present("WININIT.BAK")&&!present("WININIT.INI"));finish_restart();assert(!present("CIUKIRST.LOG"));
    file("WININIT.INI","[rename]\nNUL=locked.txt\n");fail_journal=1;assert(ws_restart_begin(shell,2)==29);fail_journal=0;
    assert(!ws_restart_begin(shell,2));fail_journal=1;ws_restart_tick(shell);fail_journal=0;
    assert(present("CIUKIRST.LOG"));recreate();drive();finish_restart();
    char *huge=malloc(WS_WININIT_LIMIT+2);assert(huge);memset(huge,'x',WS_WININIT_LIMIT+1);huge[WS_WININIT_LIMIT+1]=0;file("WININIT.INI",huge);free(huge);
    assert(!ws_restart_begin(shell,2));drive();finish_restart();assert(present("WININIT.BAK"));
    puts("PASS WININIT move overwrite delete chains short spelling missing readonly bounds interrupted replay");
}
static struct wr_key *key(unsigned hive,const char *suffix)
{
    const char *prefix=hive?".DEFAULT\\Software\\Microsoft\\Windows\\CurrentVersion":"Software\\Microsoft\\Windows\\CurrentVersion";
    char text[256];snprintf(text,sizeof(text),"%s\\%s",prefix,suffix);
    struct wr_key *k=server.roots[hive];char *part=strtok(text,"\\");
    while(part){struct wr_key *child=wr_child(&server,k,part);if(!child)child=wr_add_key(&server,k,part,0);assert(child);k=child;part=strtok(NULL,"\\");}return k;
}
static void value(struct wr_key *k,const char *name,const char *command)
{uint8_t w[2048];size_t n=strlen(command)+1;assert(n*2<=sizeof(w));for(size_t i=0;i<n;i++){w[i*2]=command[i];w[i*2+1]=0;}assert(!wr_put(&server,k,name,1,w,(uint32_t)n*2));}
static void runonce(void)
{
    struct wr_key *service=key(0,"RunServicesOnce"),*machine=key(0,"RunOnce"),*user=key(1,"RunOnce"),*run=key(0,"Run");
    value(service,"first","one.exe");value(machine,"second","two.exe");value(machine,"third","three.exe");value(user,"fourth","four.exe");value(run,"untouched","never.exe");
    fail_commit=1;assert(ws_restart_begin(shell,2));fail_commit=0;assert(!ws_restart_begin(shell,2));drive();
    char cmd[2048];uint32_t type,handed;
    fail_commit=1;assert(ws_restart_next(shell,cmd,sizeof(cmd),&type,&handed));fail_commit=0;
    assert(!ws_restart_next(shell,cmd,sizeof(cmd),&type,&handed)&&type==0&&!strcmp(cmd,"one.exe")&&!service->values);
    assert(!ws_restart_next(shell,cmd,sizeof(cmd),&type,&handed)&&type==1&&!strcmp(cmd,"two.exe"));
    recreate();assert(ws_restart_phase(shell)==2);
    assert(!ws_restart_next(shell,cmd,sizeof(cmd),&type,&handed)&&type==1&&!strcmp(cmd,"three.exe"));
    assert(!ws_restart_next(shell,cmd,sizeof(cmd),&type,&handed)&&type==2&&!strcmp(cmd,"four.exe"));
    assert(run->values && !machine->values && !user->values);finish_restart();
    puts("PASS RunServicesOnce HKLM/HKCU insertion order deletion before handoff commit failure replay no Run");
}
static uint32_t desktop_call(void *o,unsigned op,unsigned action,uint32_t flags,struct w32r_reply *r,void *data,size_t cap)
{(void)o;assert(op!=W32R_SHUTDOWN || action!=W32R_SHUTDOWN_POLL);return rpc(99,op,action,0,0,flags,"","",NULL,0,r,data,cap);}
static unsigned live=1,killed,launched,last_notice;
static int living(void *o,uint32_t pid){(void)o;(void)pid;return live;}
static int terminate(void *o,uint32_t pid){(void)o;(void)pid;live=0;killed++;return 0;}
static int launch(void *o,const char *cmd){(void)o;assert(!strcmp(cmd,"once.exe"));launched++;return 80;}
static void notice(void *o,unsigned kind,uint32_t pid,uint32_t flags){(void)o;(void)pid;(void)flags;last_notice=kind;}
static void record(void *o,const char *event,uint32_t n){(void)o;printf("TRACE %s %u\n",event,n);}
static void tick(struct desk_restart *d,const struct desk_restart_ops *ops,uint64_t time)
{wr_tick(&server,time);ws_restart_tick(shell);desk_restart_tick(d,ops,time);}
static int alive_twelve(void *o,uint32_t pid){(void)o;return pid==12;}
static void session(void)
{
    server.compat_writer_pid=99;wr_client_join(&server,10);
    struct w32r_reply r;
    assert(!rpc(10,W32R_WINDOW_ALLOC,100,0,1,0,"","test",NULL,0,&r,NULL,0));uint32_t hwnd=r.key;
    const struct desk_restart_ops ops={NULL,desktop_call,living,terminate,launch,notice,record};
    struct desk_restart idle={0};struct w32r_reply idle_result={.version=1};
    desk_restart_tick(&idle,&ops,server.now);assert(!idle.boot_checked);
    desk_restart_idle_result(&idle,&ops,&idle_result,server.now);assert(idle.boot_checked && !idle.stage);
    idle_result.data_bytes=1;idle_result.key=10;idle_result.type=2;
    desk_restart_idle_result(&idle,&ops,&idle_result,server.now);assert(last_notice==1 && !idle.stage);
    struct desk_restart boot={0};struct w32r_reply pending_boot={.version=1,.disposition=1};
    desk_restart_idle_result(&boot,&ops,&pending_boot,server.now);
    assert(boot.boot_checked && boot.stage==1 && boot.flags==2 && !boot.pid);
    assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_RESET,0,&r,NULL,0));
    for(unsigned force=0;force<3;force++){
        struct desk_restart d={.boot_checked=1};live=1;killed=0;
        assert(!desk_restart_begin(&d,&ops,10,force==1?4:0,server.now));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==0x11&&r.key==hwnd&&r.disposition==0x80000000u);
        assert(rpc(11,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0)==5);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,force==2,"","",NULL,0,&r,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==0x16&&r.count==(force!=0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        tick(&d,&ops,server.now+20);
        if(force==0){assert(!d.stage&&!killed);continue;}
        assert(d.stage==2&&!killed);tick(&d,&ops,server.now+10000);assert(killed==1);
        for(unsigned i=0;i<20&&d.stage;i++)tick(&d,&ops,server.now+20);
        assert(!d.stage && last_notice==3);
    }
    {   /* G3: the desktop's HOLD session: no deadline, the window in turn,
         * held after the queries, COMMIT or CANCEL. */
        struct w32r_reply st;
        #define STATUS() assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_STATUS,0,&st,NULL,0))
        assert(desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_COMMIT,0,&st,NULL,0)==87);   /* no session */
        assert(rpc(10,W32R_ENDSESSION,W32R_SESSION_CANCEL,0,0,0,"","",NULL,0,&st,NULL,0)==5); /* desktop only */
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_BEGIN,9|W32R_SESSION_HOLD,&r,NULL,0));
        STATUS(); assert(st.key==0 && st.count==10 && st.type==hwnd && !st.disposition);
        wr_tick(&server,server.now+60000);          /* far past the ExitWindowsEx 10 s bound */
        STATUS(); assert(st.key==0 && st.count==10 && st.type==hwnd && !st.disposition);
        assert(desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_COMMIT,0,&st,NULL,0)==87);   /* not all asked yet */
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==0x11&&r.key==hwnd&&!r.disposition);
        STATUS(); assert(st.key==0 && st.disposition==1);   /* fetched: its handler (and save prompt) runs */
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        STATUS(); assert(st.key==3 && !st.count);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&!r.key);  /* no WM_ENDSESSION while held */
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_COMMIT,0,&st,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==0x16&&r.count==1);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        STATUS(); assert(st.key==1);
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_RESET,0,&r,NULL,0));
        /* Another program vetoed after this one agreed: WM_ENDSESSION(FALSE). */
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_BEGIN,2|W32R_SESSION_HOLD,&r,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==0x11);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        STATUS(); assert(st.key==3);
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_CANCEL,0,&st,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==0x16&&r.count==0);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        STATUS(); assert(st.key==2);
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_RESET,0,&r,NULL,0));
        /* Cancel while the window's save prompt is still open: its late answer is refused. */
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_BEGIN,9|W32R_SESSION_HOLD,&r,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==0x11);
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_CANCEL,0,&st,NULL,0));
        STATUS(); assert(st.key==2);
        assert(rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0)==6);
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_RESET,0,&r,NULL,0));
        /* A Win32 veto (WM_QUERYENDSESSION FALSE) ends a HOLD session at once. */
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_BEGIN,9|W32R_SESSION_HOLD,&r,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==0x11);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,0,"","",NULL,0,&r,NULL,0));
        STATUS(); assert(st.key==0 && !st.count);   /* no hold: WM_ENDSESSION(FALSE) follows */
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==0x16&&r.count==0);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        STATUS(); assert(st.key==2);
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_RESET,0,&r,NULL,0));
        assert(desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_BEGIN,0x200,&r,NULL,0)==87);
        #undef STATUS
        puts("PASS G3 HOLD end session: no deadline, window in turn and fetch, held until COMMIT, CANCEL/veto send FALSE, late answer refused, desktop authority");
    }
    {   /* G11 WM_POWERBROADCAST (docs/design/power-policy.md): desktop only,
         * every top-level window in turn on its USER thread, a query stops
         * at BROADCAST_QUERY_DENY, an end of session is served first. */
        struct w32r_reply st;
        #define PSTATUS() assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_POWER_STATUS,0,&st,NULL,0))
        assert(rpc(10,W32R_ENDSESSION,W32R_POWER_BEGIN,0,10,0,"","",NULL,0,&r,NULL,0)==5);
        PSTATUS(); assert(st.key==1 && !st.count);                       /* nothing running */
        assert(!rpc(99,W32R_ENDSESSION,W32R_POWER_BEGIN,0,10,0,"","",NULL,0,&r,NULL,0));   /* PBT_APMPOWERSTATUSCHANGE */
        PSTATUS(); assert(st.key==0);
        assert(rpc(99,W32R_ENDSESSION,W32R_POWER_BEGIN,0,9,0,"","",NULL,0,&r,NULL,0)==1115);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.key==hwnd&&r.type==W32R_POWERBROADCAST&&r.count==10&&!r.disposition);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&!r.key);   /* delivered once */
        assert(rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd+1,100,1,"","",NULL,0,&r,NULL,0)==87);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        PSTATUS(); assert(st.key==1 && st.count==1);
        /* PBT_APMQUERYSUSPEND with the UI flag, denied. */
        assert(!rpc(99,W32R_ENDSESSION,W32R_POWER_BEGIN,0,0,1,"","",NULL,0,&r,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==W32R_POWERBROADCAST&&r.count==0&&r.disposition==1);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,0,"","",NULL,0,&r,NULL,0));
        PSTATUS(); assert(st.key==2 && !st.count);
        /* A notification answered FALSE is still delivered, not denied. */
        assert(!rpc(99,W32R_ENDSESSION,W32R_POWER_BEGIN,0,4,0,"","",NULL,0,&r,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.count==4);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,0,"","",NULL,0,&r,NULL,0));
        PSTATUS(); assert(st.key==1 && !st.count);
        /* An end of session comes first; the broadcast follows it. */
        assert(!rpc(99,W32R_ENDSESSION,W32R_POWER_BEGIN,0,10,0,"","",NULL,0,&r,NULL,0));
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_BEGIN,9|W32R_SESSION_HOLD,&r,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==0x11);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&!r.key);  /* held: nothing else */
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_CANCEL,0,&st,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==0x16&&r.count==0);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.type==W32R_POWERBROADCAST&&r.count==10);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        PSTATUS(); assert(st.key==1 && st.count==1);
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_SESSION_RESET,0,&r,NULL,0));
        /* Malformed requests, then a finished broadcast is replaced. */
        assert(rpc(99,W32R_ENDSESSION,W32R_POWER_BEGIN,0,0x13,0,"","",NULL,0,&r,NULL,0)==87);
        assert(rpc(99,W32R_ENDSESSION,W32R_POWER_STATUS,0,1,0,"","",NULL,0,&r,NULL,0)==87);
        assert(!rpc(99,W32R_ENDSESSION,W32R_POWER_BEGIN,0,18,0,"","",NULL,0,&r,NULL,0));
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_POWER_RESET,0,&r,NULL,0));
        PSTATUS(); assert(st.key==1);
        assert(rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0)==6);
        /* G11B: dequeue deadline belongs to a window, not the broadcast.
         * A delivered query may wait for its handler beyond 20 seconds. */
        assert(!rpc(99,W32R_ENDSESSION,W32R_POWER_BEGIN,0,0,1,"","",NULL,0,&r,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.count==0);
        server.now+=20001;PSTATUS();assert(st.key==0);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_POWER_RESET,0,&r,NULL,0));
        assert(!rpc(99,W32R_ENDSESSION,W32R_POWER_BEGIN,0,2,0,"","",NULL,0,&r,NULL,0));
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_POLL,0,100,0,"","",NULL,0,&r,NULL,0)&&r.count==2);
        assert(!rpc(10,W32R_ENDSESSION,W32R_SESSION_REPLY,hwnd,100,1,"","",NULL,0,&r,NULL,0));
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_POWER_RESET,0,&r,NULL,0));
        assert(!rpc(99,W32R_ENDSESSION,W32R_POWER_BEGIN,0,0,0,"","",NULL,0,&r,NULL,0));
        server.now+=20000;PSTATUS();assert(st.key==1 && st.count==1);
        assert(!desktop_call(NULL,W32R_ENDSESSION,W32R_POWER_RESET,0,&r,NULL,0));
        #undef PSTATUS
        puts("PASS G11 WM_POWERBROADCAST: desktop authority, one at a time, per-window delivery on the USER thread, query denial, notifications not denied, end of session first, reset");
    }
    struct desk_restart d={.boot_checked=1};live=1;killed=0;
    assert(!desk_restart_begin(&d,&ops,10,1,server.now));
    tick(&d,&ops,server.now+9999);assert(!killed);tick(&d,&ops,server.now+21);assert(killed==1);
    tick(&d,&ops,server.now+20);assert(!d.stage);
    wr_exit(&server,10);
    value(key(0,"RunOnce"),"watchdog","once.exe");live=0;
    assert(!desk_restart_begin(&d,&ops,10,2,server.now));
    for(unsigned i=0;i<20&&d.stage!=5;i++)tick(&d,&ops,server.now+20);
    assert(d.stage==5 && launched==1);live=1;tick(&d,&ops,server.now+300001);assert(!live);
    for(unsigned i=0;i<20&&d.stage;i++)tick(&d,&ops,server.now+20);
    assert(!d.stage);
    assert(!rpc(10,W32R_SHUTDOWN,W32R_SHUTDOWN_REQUEST,0,10,2,"message","",NULL,0,&r,NULL,0));
    assert(!rpc(10,W32R_SHUTDOWN,W32R_SHUTDOWN_ABORT,0,0,0,"","",NULL,0,&r,NULL,0));
    assert(rpc(10,W32R_SHUTDOWN,W32R_SHUTDOWN_ABORT,0,0,0,"","",NULL,0,&r,NULL,0)==1116);
    assert(rpc(10,W32R_SHUTDOWN,W32R_SHUTDOWN_BEGIN,0,0,0,"","",NULL,0,&r,NULL,0)==5);
    assert(!wr_client_join(&server,12));assert(!wr_flush(&server));
    ws_destroy(shell);wr_destroy(&server);assert(wr_init(&server,&io,server.now));
    assert(!wr_reconcile(&server,alive_twelve,NULL));assert(server.live_count==1 && server.live[0]==12);
    struct ws_config config={programs,root};shell=ws_create(&fs,&config,&server);assert(shell);server.shell=shell;server.shell_call=ws_call;server.shell_exit=ws_exit;
    puts("PASS windowless client identity restored after actual service destroy/load/reconcile");
    assert(!wr_clients_reset(&server));assert(!server.live_count);
    assert(!wr_reconcile(&server,alive_twelve,NULL));assert(!server.live_count);
    puts("PASS fresh desktop clears prior-boot identities even when a native PID is reused");
    puts("PASS session query accept veto force timeout plain end RunOnce watchdog deferred request abort authority");
}

#include "restart_fixture.inc"
int main(int argc,char **argv)
{
    setbuf(stdout,NULL);assert(argc==3);snprintf(root,sizeof(root),"%s",argv[2]);assert(strlen(root)+10<sizeof(programs));strcpy(programs,root);strcat(programs,"/Programs");assert(!mkdir(programs,0700));
    wr_posix_io(&io,root);real_sync=io.sync;io.sync=sync_fault;io.log=log_line;assert(wr_init(&server,&io,0));
    ws_posix_fs(&fs);real_write=fs.write;fs.write=write_fault;real_rename=fs.rename;fs.rename=rename_fault;fs.log=log_line;
    struct ws_config config={programs,root};shell=ws_create(&fs,&config,&server);assert(shell);server.shell=shell;server.shell_call=ws_call;server.shell_exit=ws_exit;
    if(!strcmp(argv[1],"fixture"))restart_fixture_test();else if(!strcmp(argv[1],"dde"))dde();else if(!strcmp(argv[1],"wininit"))wininit();else if(!strcmp(argv[1],"runonce"))runonce();else session();
    ws_destroy(shell);wr_destroy(&server);assert(logs);return 0;
}
