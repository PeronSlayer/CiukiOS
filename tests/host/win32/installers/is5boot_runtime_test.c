/* Execute the production helper on a bounded, in-memory Win32 service model.
 * Every image/stream here is authored by this test. No commercial fixture.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define entry is5_pe_entry
#include "../../../../sdk/win32/is5boot/main.c"
#undef entry
struct file { char path[IS5_PATH];uint8_t data[16384];unsigned size,exists,opened; };
static struct file files[32];
static DWORD last_error;
static unsigned started,waits,refuse_start,env_frees,fail_write,live_engine,snapshot_refused;
static struct file *lookup(const char *path,int create) {
    for(unsigned i=0;i<32;i++)if(equal(files[i].path,path))return files+i;
    if(create)for(unsigned i=0;i<32;i++)if(!files[i].path[0]){copy(files[i].path,path);return files+i;}
    return 0;
}
DWORD W32API GetLastError(void){return last_error;}
HANDLE W32API GetProcessHeap(void){return (HANDLE)1;}
void *W32API HeapAlloc(HANDLE h,DWORD flags,DWORD n){(void)h;(void)flags;return malloc(n);}
BOOL W32API HeapFree(HANDLE h,DWORD flags,void *p){(void)h;(void)flags;free(p);return 1;}
HANDLE W32API CreateFileA(const char *path,DWORD access,DWORD share,SECURITY_ATTRIBUTES *s,DWORD disposition,DWORD flags,HANDLE template) {
    (void)access;(void)share;(void)s;(void)flags;(void)template;
    struct file *f=lookup(path,disposition!=3);
    if(!f || (disposition==3 && !f->exists)){last_error=2;return (HANDLE)(uintptr_t)-1;}
    if(disposition==1 && f->exists){last_error=80;return (HANDLE)(uintptr_t)-1;}
    if(f->opened){last_error=32;return (HANDLE)(uintptr_t)-1;}
    if(!f->exists || disposition==2)f->size=0;
    f->exists=f->opened=1;return f;
}
BOOL W32API CloseHandle(HANDLE h){if((uintptr_t)h>1024)((struct file *)h)->opened=0;return 1;}
DWORD W32API GetFileSize(HANDLE h,DWORD *high){*high=0;return ((struct file *)h)->size;}
BOOL W32API ReadFile(HANDLE h,void *p,DWORD n,DWORD *done,OVERLAPPED *o){(void)o;struct file *f=h;if(n>f->size)n=f->size;memcpy(p,f->data,n);*done=n;return 1;}
BOOL W32API WriteFile(HANDLE h,const void *p,DWORD n,DWORD *done,OVERLAPPED *o){
    (void)o;struct file *f=h;if(fail_write){fail_write=0;*done=0;return 0;}
    assert(n<=sizeof(f->data));memcpy(f->data,p,n);f->size=n;*done=n;return 1;
}
BOOL W32API FlushFileBuffers(HANDLE h){(void)h;return 1;}
BOOL W32API DeleteFileA(const char *path){
    struct file *f=lookup(path,0);if(!f || !f->exists){last_error=2;return 0;}
    assert(!live_engine);if(f->opened){last_error=32;return 0;}f->exists=0;return 1;
}
DWORD W32API GetTickCount(void){return 1234;}
void W32API Sleep(DWORD ms){(void)ms;}
DWORD W32API GetTempPathA(DWORD n,char *p){assert(n>16);copy(p,"C:\\TEMP\\");return len(p);}
DWORD W32API GetWindowsDirectoryA(char *p,DWORD n){assert(n>16);copy(p,"C:\\WINDOWS");return len(p);}
DWORD W32API GetPrivateProfileStringA(const char *section,const char *key,const char *fallback,char *out,DWORD cap,const char *path) {
    const char *value=fallback;char saved[IS5_PATH];
    if(equal(section,"Startup") && equal(key,"AppName"))value="Authored Setup";
    if(equal(section,"Startup") && equal(key,"CmdLine"))value="-s -f1\"C:\\a b.iss\"";
    if(equal(section,"file0")) {
        struct file *f=lookup(path,0);assert(f && f->exists && f->size<sizeof(f->data));f->data[f->size]=0;
        char needle[32];assert(len(key)+2<sizeof(needle));copy(needle,key);copy(needle+len(needle),"=");
        const char *p=strstr((char *)f->data,needle);
        if(p){p+=len(needle);unsigned n=0;while(p[n] && p[n]!='\r' && p[n]!='\n'){assert(n+1<sizeof(saved));saved[n]=p[n];n++;}saved[n]=0;value=saved;}
    }
    unsigned n=len(value);if(n>=cap)n=cap-1;memcpy(out,value,n);out[n]=0;return n;
}
DWORD W32API GetPrivateProfileSectionNamesA(char *p,DWORD n,const char *path){(void)path;assert(n>8);memcpy(p,"file0\0\0",7);return 6;}
BOOL W32API WritePrivateProfileStringA(const char *section,const char *key,const char *value,const char *path){(void)section;(void)key;(void)value;(void)path;return 1;}
char *W32API GetEnvironmentStringsA(void){char *p=malloc(32);assert(p);memcpy(p,"A=one\0B=two words\0\0",19);return p;}
BOOL W32API FreeEnvironmentStringsA(char *p){env_frees++;free(p);return 1;}
static void assert_text(const char *path,const char *expected) {
    struct file *f=lookup(path,0);assert(f && f->exists && f->size==strlen(expected));assert(!memcmp(f->data,expected,f->size));
}
BOOL W32API CreateProcessA(const char *image,char *command,SECURITY_ATTRIBUTES *p,SECURITY_ATTRIBUTES *t,BOOL inherit,DWORD flags,void *env,const char *cwd,struct shell_startup *startup,struct shell_process *child){
    (void)p;(void)t;assert(!*command && !inherit && !flags && !env && equal(cwd,"D:\\"));assert(startup->size==sizeof(*startup));
    assert(equal(image,"C:\\TEMP\\_INS1235._MP"));started++;
    assert_text("C:\\TEMP\\_INS0432.INI","  -fD:\\SETUP.INS  -z1 -cx -xC:\\TEMP\\ -x1D:\\ -q10010 -s -f1\"C:\\a b.iss\" -SMS -zuser");
    assert_text("C:\\WINDOWS\\_isenv31.ini","A=one B=two words");
    struct file *errors=lookup("C:\\WINDOWS\\_iserr31.ini",0);assert(errors && errors->exists);
    assert(!memcmp(errors->data,"[SharedErrors]\r\n7068=",21));
    if(refuse_start)return 0;
    child->process=(HANDLE)10;child->thread=(HANDLE)11;live_engine=1;return 1;
}
DWORD W32API WaitForSingleObject(HANDLE h,DWORD ms){assert((h==(HANDLE)10 || h==(HANDLE)12) && ms==0xffffffffu);waits++;live_engine=0;return 0;}
BOOL W32API GetExitCodeProcess(HANDLE h,DWORD *code){assert(h==(HANDLE)10);*code=77;return 1;}
HANDLE W32API CreateToolhelp32Snapshot(DWORD flags,DWORD pid){assert(flags==2 && !pid);return snapshot_refused?(HANDLE)(uintptr_t)-1:(HANDLE)20;}
BOOL W32API Process32First(HANDLE h,struct process_entry *p){assert(h==(HANDLE)20);p->pid=99;copy(p->exe,"_INS1235._MP");return 1;}
BOOL W32API Process32Next(HANDLE h,struct process_entry *p){(void)h;(void)p;last_error=18;return 0;}
HANDLE W32API OpenProcess(DWORD access,BOOL inherit,DWORD pid){assert(access==0x100000 && !inherit && pid==99);return (HANDLE)12;}
char *W32API GetCommandLineA(void){return "D:\\SETUP.EXE";}
int W32API MessageBoxA(HANDLE h,const char *text,const char *title,DWORD flags){(void)h;(void)text;(void)title;(void)flags;abort();}
void W32API ExitProcess(DWORD code){(void)code;abort();}
static void put(uint8_t *p,unsigned v,unsigned n){for(unsigned i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
static unsigned bitpos;
static void bits(uint8_t *p,unsigned v,unsigned n){for(unsigned i=0;i<n;i++,bitpos++)p[bitpos/8]|=((v>>i)&1)<<(bitpos%8);}
static void initialize(void) {
    memset(files,0,sizeof(files));started=waits=refuse_start=env_frees=fail_write=live_engine=snapshot_refused=0;
    struct file *stub=lookup("D:\\SETUP.EXE",1);stub->exists=1;stub->size=192;
    stub->data[0]='M';stub->data[1]='Z';put(stub->data+60,128,4);stub->data[128]='N';stub->data[129]='E';stub->data[182]=2;
    struct file *a=lookup("D:\\_INST32I.EX_",1);a->exists=1;uint8_t *b=a->data;
    const uint8_t sig[]={42,171,121,216,0,1};memcpy(b,sig,6);put(b+78,3,4);
    const char *names[]={"INSTALL.EXE","zdata.dll","WUTL95i.DLL"};
    const char *runtime[]={"_INS5176._MP","ZDataI51.dll","_WUTL951.DLL"};
    unsigned at=82,data=512;
    for(unsigned i=0;i<3;i++){
        unsigned h=at;put(b+h,data,4);put(b+h+8,5,4);at+=20;
        unsigned n=len(names[i]);put(b+at,n,2);memcpy(b+at+2,names[i],n);at+=2+n;
        n=len(runtime[i]);put(b+at,n,2);memcpy(b+at+2,runtime[i],n);at+=2+n;
        uint8_t *p=b+data;p[1]=6;bitpos=16;
        for(unsigned j=0;j<5;j++){bits(p,0,1);bits(p,(const uint8_t []){'M','Z','a','b','c'}[j],8);}
        bits(p,1,1);bits(p,0,7);bits(p,255,8);n=(bitpos+7)/8;put(b+h+4,n,4);data+=n;
    }
    a->size=data;
    struct file *collision=lookup("C:\\TEMP\\_INS1234._MP",1);collision->exists=1;collision->size=4;memcpy(collision->data,"keep",4);
}
int main(void) {
    struct state s;unsigned failure;
    initialize();memset(&s,0,sizeof(s));failure=1;
    assert(run(&s,"D:\\SETUP.EXE","-SMS -zuser",&failure)==77 && failure==4);
    assert(started==1 && waits==1 && env_frees==1);
    assert(!lookup(s.engine,0)->exists && !lookup(s.dll[0],0)->exists && !lookup(s.dll[1],0)->exists);
    assert_text("C:\\TEMP\\_INS1234._MP","keep");
    initialize();memset(&s,0,sizeof(s));refuse_start=1;
    assert(run(&s,"D:\\SETUP.EXE","-SMS -zuser",&failure)==1 && failure==2 && started==1);
    assert(!lookup(s.engine,0)->exists && !lookup(s.dll[0],0)->exists);
    initialize();memset(&s,0,sizeof(s));fail_write=1;
    assert(run(&s,"D:\\SETUP.EXE","",&failure)==1 && !started && !lookup(s.engine,0)->exists);
    initialize();memset(&s,0,sizeof(s));copy(s.temp,"C:\\TEMP\\");copy(s.windows,"C:\\WINDOWS");
    assert(is5_join(s.dll[0],s.temp,"ZDataI51.dll") && is5_join(s.dll[1],s.temp,"_WUTL951.DLL"));
    struct file *ini=lookup("C:\\WINDOWS\\_delis43.ini",1);ini->exists=1;
    assert(is5_cleanup_text((char *)ini->data,sizeof(ini->data),"C:\\TEMP\\_INS1235._MP",s.dll[0],s.dll[1]));ini->size=len((char *)ini->data);
    lookup("C:\\TEMP\\_INS1235._MP",1)->exists=1;lookup(s.dll[0],1)->exists=1;lookup(s.dll[1],1)->exists=1;
    snapshot_refused=1;assert(cleanup(&s)==1 && lookup(s.dll[0],0)->exists);
    snapshot_refused=0;live_engine=1;assert(cleanup(&s)==0 && waits==1 && !lookup(s.dll[0],0)->exists);
    struct file *isdel=lookup("D:\\_ISDEL.EXE",1),*stub=lookup("D:\\SETUP.EXE",0);
    isdel->exists=1;isdel->size=stub->size;memcpy(isdel->data,stub->data,stub->size);
    memset(&s,0,sizeof(s));assert(run(&s,"D:\\_ISDEL.EXE","",&failure)==0 && !started);
    assert(is5_cleanup_text((char *)ini->data,sizeof(ini->data),"C:\\Documents\\keep.txt",s.dll[0],s.dll[1]));ini->size=len((char *)ini->data);
    assert(cleanup(&s)==1);
    puts("PASS production IS5 helper: collisions, handoffs, empty command/CD cwd, waited exit, rollback and confined cleanup");
    return 0;
}
