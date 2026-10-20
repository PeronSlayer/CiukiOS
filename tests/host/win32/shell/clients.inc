#include <sys/stat.h>
/* Included by the existing production USER/kernel-queue harness. */
#include "../../../../apps/w32srv/shell.h"
extern uintptr_t W32API DdeCreateStringHandleA(DWORD,const char *,int);
extern UINT W32API DdeInitializeA(DWORD *,void *(W32API *)(UINT,UINT,void *,uintptr_t,uintptr_t,void *,uintptr_t,uintptr_t),DWORD,DWORD);
extern uintptr_t W32API DdeConnect(DWORD,uintptr_t,uintptr_t,void *);
extern uintptr_t W32API DdeClientTransaction(void *,DWORD,uintptr_t,uintptr_t,UINT,UINT,DWORD,DWORD *);
extern BOOL W32API DdeDisconnect(uintptr_t),DdeUninitialize(DWORD),DdeFreeStringHandle(DWORD,uintptr_t),DdeFreeDataHandle(uintptr_t);
extern DWORD W32API DdeGetData(uintptr_t,void *,DWORD,DWORD);
extern void *W32API DdeAccessData(uintptr_t,DWORD *);
extern BOOL W32API DdeUnaccessData(uintptr_t);
extern LPARAM W32API PackDDElParam(UINT,uintptr_t,uintptr_t);
extern BOOL W32API UnpackDDElParam(UINT,LPARAM,uintptr_t *,uintptr_t *),FreeDDElParam(UINT,LPARAM);
static void *W32API shell_callback(UINT a,UINT b,void *c,uintptr_t d,uintptr_t e,void *f,uintptr_t g,uintptr_t h)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return 0;}
static HWND dde_server;
static unsigned dde_ack,dde_terminated;
static int dde_initiating,session_answer=1,session_ended=-1;
static char dde_text[4096];
static LRESULT W32API shell_client(HWND h,UINT m,WPARAM wp,LPARAM lp)
{
    if(m==0x11)return session_answer;
    if(m==0x16){session_ended=(int)wp;return 0;}
    if(m==0x3e4){
        if(dde_initiating){dde_server=(HWND)wp;return 0;}
        uintptr_t lo,hi;CHECK(UnpackDDElParam(m,lp,&lo,&hi));dde_ack=(unsigned)lo;
        CHECK(FreeDDElParam(m,lp));return 0;
    }
    if(m==0x3e5){
        uintptr_t lo,hi;CHECK(UnpackDDElParam(m,lp,&lo,&hi));uint8_t *p=GlobalLock((HANDLE)lo);
        CHECK(p && w32_u16(p)==0x3000 && w32_u16(p+2)==1);
        CHECK(strlen((char *)p+4)<sizeof(dde_text));strcpy(dde_text,(char *)p+4);
        GlobalUnlock((HANDLE)lo);GlobalFree((HANDLE)lo);FreeDDElParam(m,lp);return 0;
    }
    if(m==0x3e1){dde_terminated++;return 0;}
    return DefWindowProcA(h,m,wp,lp);
}
static void shell_dispatch(void)
{MSG m;for(unsigned i=0;i<100 && PeekMessageA(&m,0,0,0,1);i++)DispatchMessageA(&m);}
static ATOM shell_atom(const char *name)
{struct w32r_reply r;CHECK(u32_directory(W32R_ATOM_ADD,0,0,0,0,name,0,0,&r,0,0));return (ATOM)r.key;}
static void session_action(unsigned action,unsigned flags,struct w32r_reply *r)
{
    struct {struct w32r_request r;char empty[2];} a={.r={.version=1,.op=W32R_ENDSESSION,.index=action,.flags=flags,.path_bytes=1,.name_bytes=1}};
    size_t used=0;CHECK(!wr_dispatch(&server,99,&a,sizeof(a.r)+2,r,sizeof(*r),&used)&&used==sizeof(*r)&&!r->error);
}
static void shell_clients(const char *directory)
{
    char cwd[1024],path[2048],programs[2048],exe[2200];CHECK(getcwd(cwd,sizeof(cwd)));
    snprintf(path,sizeof(path),"%s/%s",cwd,directory);snprintf(programs,sizeof(programs),"%s/Programs",path);CHECK(!mkdir(programs,0700));
    snprintf(exe,sizeof(exe),"%s/tool.exe",path);FILE *f=fopen(exe,"wb");CHECK(f&&fputs("MZ",f)>=0&&!fclose(f));
    struct ws_fs fs;ws_posix_fs(&fs);struct ws_config config={programs,path};
    struct ws_shell *svc=ws_create(&fs,&config,&server);CHECK(svc);server.shell=svc;server.shell_call=ws_call;server.shell_exit=ws_exit;
    WNDCLASSA cls={.lpfnWndProc=shell_client,.lpszClassName="DDE client"};CHECK(RegisterClassA(&cls));
    HWND client=CreateWindowExA(0,cls.lpszClassName,"",WS_POPUP,0,0,32,32,0,0,0,0);CHECK(client);
    ATOM app=shell_atom("PROGMAN"),topic=shell_atom("PROGMAN");dde_initiating=1;
    SendMessageA((HWND)(uintptr_t)0xffff,0x3e0,(WPARAM)client,(LPARAM)((DWORD)app|((DWORD)topic<<16)));
    dde_initiating=0;CHECK(dde_server);
    DWORD id=0;CHECK(!DdeInitializeA(&id,shell_callback,0x10,0));
    uintptr_t service=DdeCreateStringHandleA(id,"Shell",1004),subject=DdeCreateStringHandleA(id,"AppProperties",1004);
    uintptr_t conv=DdeConnect(id,service,subject,0);CHECK(conv);
    char add[4096];snprintf(add,sizeof(add),"[AddItem(\"%s -arg\",Tool)][DeleteItem(Tool)]",exe);
    const char *commands[]={"[CreateGroup(Test)][ShowGroup(Test,1)]",add,"[Reload(Test)][ExitProgman(0)]","[ReplaceItem(missing)]","[ShowGroup(Test)]","[CreateGroup(\"bad)]","[DeleteGroup(Test)]"};
    const unsigned answers[]={0x8000,0x8000,0x8000,0,0,0,0x8000};
    for(unsigned i=0;i<sizeof(commands)/sizeof(*commands);i++){
        HANDLE block=GlobalAlloc(2,(DWORD)strlen(commands[i])+1);CHECK(block);strcpy(GlobalLock(block),commands[i]);GlobalUnlock(block);
        dde_ack=0xffff;CHECK(PostMessageA(dde_server,0x3e8,(WPARAM)client,(LPARAM)block));shell_dispatch();CHECK(dde_ack==answers[i]);GlobalFree(block);
        DWORD status=0;
        if(i==6){char again[]="[CreateGroup(Test)]";CHECK(DdeClientTransaction(again,sizeof(again),conv,0,0,0x4050,1000,&status));}
        uintptr_t result=DdeClientTransaction((void *)commands[i],(DWORD)strlen(commands[i])+1,conv,0,0,0x4050,1000,&status);
        CHECK((!!result)==(!!answers[i]) && status==answers[i]);
        printf("TRANSCRIPT raw+DDEML command=%s ack=%04x\n",commands[i],answers[i]);
    }
    DWORD status;char create[]="[CreateGroup(Request)]";CHECK(DdeClientTransaction(create,sizeof(create),conv,0,0,0x4050,1000,&status));
    uintptr_t item=DdeCreateStringHandleA(id,"Groups",1004);
    uintptr_t data=DdeClientTransaction(0,0,conv,item,1,0x20b0,1000,&status);CHECK(data && status==0x8000);
    char got[4096];DWORD n=DdeGetData(data,got,sizeof(got),0);CHECK(n && !strcmp(got,"Request\r\n"));DWORD size;
    CHECK(DdeAccessData(data,&size) && size==n && DdeUnaccessData(data));CHECK(DdeFreeDataHandle(data));
    ATOM raw_item=shell_atom("Groups");dde_text[0]=0;CHECK(PostMessageA(dde_server,0x3e6,(WPARAM)client,PackDDElParam(0x3e6,1,raw_item)));shell_dispatch();CHECK(!strcmp(got,dde_text));
    char remove[]="[DeleteGroup(Request)]";CHECK(DdeClientTransaction(remove,sizeof(remove),conv,0,0,0x4050,1000,&status));
    CHECK(DdeDisconnect(conv)&&DdeFreeStringHandle(id,item)&&DdeFreeStringHandle(id,service)&&DdeFreeStringHandle(id,subject)&&DdeUninitialize(id));
    CHECK(PostMessageA(dde_server,0x3e1,(WPARAM)client,0));shell_dispatch();CHECK(dde_terminated==1);
    server.compat_writer_pid=99;wr_client_join(&server,u32_pid());
    for(unsigned force=0;force<3;force++){
        struct w32r_reply status;session_answer=force==2;session_ended=-1;
        session_action(W32R_SESSION_BEGIN,force==1?4:0,&status);
        ui_kernel_pause(21);u32_session_pump();ui_kernel_pause(21);u32_session_pump();
        session_action(W32R_SESSION_STATUS,0,&status);
        CHECK(status.key==(force==0?2u:1u) && session_ended==(force!=0));
        session_action(W32R_SESSION_RESET,0,&status);
    }
    puts("PASS USER owning-thread WM_QUERYENDSESSION WM_ENDSESSION accept veto force");
    CHECK(DestroyWindow(client)&&UnregisterClassA(cls.lpszClassName,0));
    struct w32r_reply r;u32_directory(W32R_ATOM_DELETE,(HWND)(uintptr_t)raw_item,0,0,0,"",0,0,&r,0,0);
    u32_directory(W32R_ATOM_DELETE,(HWND)(uintptr_t)app,0,0,0,"",0,0,&r,0,0);u32_directory(W32R_ATOM_DELETE,(HWND)(uintptr_t)topic,0,0,0,"",0,0,&r,0,0);
    server.shell=NULL;server.shell_call=NULL;server.shell_exit=NULL;ws_destroy(svc);unlink(exe);rmdir(programs);
    puts("PASS PROGMAN raw-message and DDEML clients production USER queues RPC effects");
}
