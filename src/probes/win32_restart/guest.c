/* Public Win32 API adapter. No service operation bypasses USER/ADVAPI.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/user32.h>
#include "exercise.h"
extern void W32API ExitProcess(DWORD);
extern HANDLE W32API GlobalAlloc(UINT,DWORD),GlobalFree(HANDLE);
extern void *W32API GlobalLock(HANDLE);
extern BOOL W32API GlobalUnlock(HANDLE);
extern DWORD W32API GlobalSize(HANDLE);
extern char *W32API GetCommandLineA(void);
extern ATOM W32API GlobalAddAtomA(const char *);
extern ATOM W32API GlobalDeleteAtom(ATOM);
extern LPARAM W32API PackDDElParam(UINT,uintptr_t,uintptr_t);
extern BOOL W32API UnpackDDElParam(UINT,LPARAM,uintptr_t *,uintptr_t *),FreeDDElParam(UINT,LPARAM);
extern UINT W32API DdeInitializeA(DWORD *,void *(W32API *)(UINT,UINT,void *,uintptr_t,uintptr_t,void *,uintptr_t,uintptr_t),DWORD,DWORD);
extern uintptr_t W32API DdeCreateStringHandleA(DWORD,const char *,int),DdeConnect(DWORD,uintptr_t,uintptr_t,void *);
extern uintptr_t W32API DdeClientTransaction(void *,DWORD,uintptr_t,uintptr_t,UINT,UINT,DWORD,DWORD *);
extern BOOL W32API DdeDisconnect(uintptr_t),DdeUninitialize(DWORD),DdeFreeStringHandle(DWORD,uintptr_t),DdeFreeDataHandle(uintptr_t);
extern DWORD W32API DdeGetData(uintptr_t,void *,DWORD,DWORD);
extern LONG W32API RegCreateKeyExA(uint32_t,const char *,DWORD,char *,DWORD,DWORD,void *,uint32_t *,DWORD *);
extern LONG W32API RegOpenKeyExA(uint32_t,const char *,DWORD,DWORD,uint32_t *);
extern LONG W32API RegSetValueExA(uint32_t,const char *,DWORD,DWORD,const uint8_t *,DWORD);
extern LONG W32API RegQueryValueExA(uint32_t,const char *,DWORD *,DWORD *,uint8_t *,DWORD *);
extern LONG W32API RegCloseKey(uint32_t);
static struct w32_restart_result *R;
static HWND windows[2],server;
static unsigned initiating,ack,terminated,ends,veto,session_active,query_base;
static char raw_groups[4096];
static unsigned len(const char *s){unsigned n=0;while(s[n])n++;return n;}
static void cp(void *d,const void *s,unsigned n){uint8_t *a=d;const uint8_t *b=s;while(n--)*a++=*b++;}
static void pump(void){MSG m;for(unsigned i=0;i<100&&PeekMessageA(&m,0,0,0,1);i++)DispatchMessageA(&m);}
static LRESULT W32API procedure(HWND h,UINT m,WPARAM w,LPARAM l)
{
    if(m==0x11&&session_active){R->queries++;R->logoff+=!!((uint32_t)l&0x80000000u);return !(veto&&R->queries-query_base==2);}
    if(m==0x16&&session_active){if(w)R->accepted++;else R->cancelled++;ends++;return 0;}
    if(m==0x3e4){if(initiating)server=(HWND)w;else{uintptr_t lo,hi;if(UnpackDDElParam(m,l,&lo,&hi))ack=(unsigned)lo;FreeDDElParam(m,l);}return 0;}
    if(m==0x3e5){uintptr_t lo,hi;if(UnpackDDElParam(m,l,&lo,&hi)){
        uint8_t *p=GlobalLock((HANDLE)lo);DWORD n=GlobalSize((HANDLE)lo);
        if(p&&n>=5&&n<=sizeof(raw_groups)+4&&p[2]==1&&p[3]==0&&p[n-1]==0){cp(raw_groups,p+4,n-4);}
        if(p)GlobalUnlock((HANDLE)lo);GlobalFree((HANDLE)lo);FreeDDElParam(m,l);}return 0;}
    if(m==0x3e1){terminated++;return 0;}
    return DefWindowProcA(h,m,w,l);
}
static int read_bytes(const char *path,void *out,unsigned cap)
{
    HANDLE f=CreateFileA(path,0x80000000,7,0,3,0,0);if(f==(HANDLE)(uintptr_t)0xffffffff)return -1;
    DWORD n=0,more=0;uint8_t extra;BOOL ok=ReadFile(f,out,cap,&n,0)&&ReadFile(f,&extra,1,&more,0)&&!more;BOOL closed=CloseHandle(f);return ok&&closed?(int)n:-1;
}
static int write_bytes(const char *path,const void *data,unsigned count)
{
    HANDLE f=CreateFileA(path,0x40000000,7,0,2,0,0);if(f==(HANDLE)(uintptr_t)0xffffffff)return 0;
    DWORD n=0;BOOL ok=WriteFile(f,data,count,&n,0)&&n==count&&FlushFileBuffers(f);BOOL closed=CloseHandle(f);return ok&&closed;
}
static int exists(const char *p){if(GetFileAttributesA(p)!=0xffffffff)return 1;DWORD e=GetLastError();return e==2||e==3?0:-1;}
static int remove_file(const char *p){return DeleteFileA(p)||GetLastError()==2;}
static int wait_counter(unsigned *p,unsigned target)
{DWORD start=GetTickCount();while(*p<target&&GetTickCount()-start<10000){pump();Sleep(5);}return *p>=target;}
static void *W32API callback(UINT a,UINT b,void *c,uintptr_t d,uintptr_t e,void *f,uintptr_t g,uintptr_t h)
{(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;return 0;}
static DWORD dde_id;
static uintptr_t dde_app,dde_topic,dde_conv;
static int dde(unsigned raw,const char *command,char *groups,unsigned cap)
{
    if(raw){
        if(!server){
        ATOM app=GlobalAddAtomA("PROGMAN"),topic=GlobalAddAtomA("PROGMAN");
        initiating=1;SendMessageA((HWND)(uintptr_t)0xffff,0x3e0,(WPARAM)windows[0],(LPARAM)((DWORD)app|((DWORD)topic<<16)));initiating=0;
        GlobalDeleteAtom(app);GlobalDeleteAtom(topic);if(!server)return 0;
        }
        int ok=0;
        if(command){
            HANDLE block=GlobalAlloc(2,len(command)+1);if(!block)return 0;
            void *p=GlobalLock(block);if(!p){GlobalFree(block);return 0;}cp(p,command,len(command)+1);GlobalUnlock(block);
            ack=0;ok=PostMessageA(server,0x3e8,(WPARAM)windows[0],(LPARAM)block)&&wait_counter(&ack,0x8000)&&ack==0x8000;GlobalFree(block);
        }else{
            ATOM item=GlobalAddAtomA("Groups");raw_groups[0]=0;
            if(PostMessageA(server,0x3e6,(WPARAM)windows[0],PackDDElParam(0x3e6,1,item))){
                DWORD start=GetTickCount();while(!raw_groups[0]&&GetTickCount()-start<10000){pump();Sleep(5);}
                unsigned n=len(raw_groups)+1;if(raw_groups[0]&&n<=cap){cp(groups,raw_groups,n);ok=1;}
            }
            GlobalDeleteAtom(item);
        }
        if(command&&command[1]=='D'&&command[7]=='G'){
            terminated=0;PostMessageA(server,0x3e1,(WPARAM)windows[0],0);ok=wait_counter(&terminated,1)&&ok;server=0;
        }
        return ok;
    }
    if(!dde_id){
        if(DdeInitializeA(&dde_id,callback,0x10,0))return 0;
        dde_app=DdeCreateStringHandleA(dde_id,"PROGMAN",1004);dde_topic=DdeCreateStringHandleA(dde_id,"PROGMAN",1004);
        dde_conv=DdeConnect(dde_id,dde_app,dde_topic,0);
    }
    DWORD id=dde_id;uintptr_t conv=dde_conv;int ok=0;DWORD status=0;
    if(conv){
        if(command)ok=DdeClientTransaction((void *)command,len(command)+1,conv,0,0,0x4050,10000,&status)!=0&&status==0x8000;
        else{
            uintptr_t item=DdeCreateStringHandleA(id,"Groups",1004),data=DdeClientTransaction(0,0,conv,item,1,0x20b0,10000,&status);
            if(data){DWORD n=DdeGetData(data,groups,cap,0);ok=n>0&&n<=cap&&groups[n-1]==0&&status==0x8000;DdeFreeDataHandle(data);}DdeFreeStringHandle(id,item);
        }
    }
    if(command&&command[1]=='D'&&command[7]=='G'){
        DdeDisconnect(conv);DdeFreeStringHandle(id,dde_app);DdeFreeStringHandle(id,dde_topic);DdeUninitialize(id);dde_id=0;dde_conv=0;
    }
    return ok;
}
static unsigned registry(unsigned queue,const char *name,const char *command)
{
    const char *path=queue==0?"Software\\Microsoft\\Windows\\CurrentVersion\\RunServicesOnce":"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
    uint32_t root=queue==2?0x80000001u:0x80000002u,key=0;DWORD disposition;
    LONG e=command?RegCreateKeyExA(root,path,0,0,0,0x2001f,0,&key,&disposition):RegOpenKeyExA(root,path,0,0x20019,&key);
    if(e)return (unsigned)e;
    DWORD n=0;e=command?RegSetValueExA(key,name,0,1,(const uint8_t *)command,len(command)+1):RegQueryValueExA(key,name,0,0,0,&n);
    LONG closed=RegCloseKey(key);return (unsigned)(e?e:closed);
}
static int session(unsigned flags,unsigned refuse,struct w32_restart_result *r)
{
    (void)r;veto=refuse;ends=0;query_base=R->queries;session_active=1;
    int ok=ExitWindowsEx(flags,0)&&wait_counter(&ends,2);
    session_active=0;return ok;
}
static int park(struct w32_restart_result *r)
{
    DWORD begin=GetTickCount();while(!((volatile struct w32_restart_result *)r)->release&&GetTickCount()-begin<30000)Sleep(5);
    if(!r->release)return 0;
    r->release=0;
    if(!r->helper)return 1;
    /* A new process alone is insufficient: prove the recovered endpoint
     * accepts an authenticated registry request before completing this child. */
    begin=GetTickCount();while(registry(0,"F507D-R",0)!=2&&GetTickCount()-begin<10000)Sleep(20);
    return registry(0,"F507D-R",0)==2;
}
void W32API entry(void)
{
    R=VirtualAlloc((void *)(uintptr_t)W32_RESTART_RESULT_VA,4096,0x3000,4);if(!R){ExitProcess(2);return;}
    const struct w32_restart_env env={read_bytes,write_bytes,remove_file,exists,dde,registry,session,park};
    const char *cmd=GetCommandLineA();unsigned which=5;
    for(unsigned i=0;cmd&&cmd[i];i++)if(cmd[i]=='-'&&cmd[i+1]=='-'){
        const char tags[]="SMURT";for(unsigned j=0;j<5;j++)if(cmd[i+2]==tags[j]&&(!cmd[i+3]||cmd[i+3]==' '))which=j;
    }
    if(which<5){
        int error=w32_restart_helper(R,&env,which);
        if(error){R->cases[which<3?RESTART_RUNONCE:RESTART_RECOVERY].errors=1;R->interrupt=0;park(R);}
        ExitProcess((DWORD)error);return;
    }
    WNDCLASSA cls={.lpfnWndProc=procedure,.lpszClassName="F507D restart"};
    if(!RegisterClassA(&cls)){ExitProcess(3);return;}
    for(unsigned i=0;i<2;i++){windows[i]=CreateWindowExA(0,cls.lpszClassName,"Restart fixture",WS_POPUP,0,0,32,32,0,0,0,0);if(!windows[i]){ExitProcess(4);return;}}
    int result=w32_restart_exercise(R,&env);
    for(unsigned i=0;i<2;i++)DestroyWindow(windows[i]);UnregisterClassA(cls.lpszClassName,0);
    if(!result){DWORD begin=GetTickCount();while(!((volatile struct w32_restart_result *)R)->release&&GetTickCount()-begin<30000)Sleep(5);if(!R->release)result=-1;}
    unsigned errors=result<0;for(unsigned i=0;i<RESTART_CASES;i++)errors+=R->cases[i].errors;
    ExitProcess(errors?1:0);
}
