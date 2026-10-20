/* Authored Win32 facade exercise; the kernel reads controller.h, not stdout.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#include "controller.h"
extern void W32API Sleep(DWORD);
extern void W32API ExitProcess(DWORD);
extern HANDLE W32API CreateThread(SECURITY_ATTRIBUTES *,DWORD,DWORD(W32API *)(void *),void *,DWORD,DWORD *);
extern DWORD W32API TlsAlloc(void);
extern BOOL W32API TlsSetValue(DWORD,void *);
extern void *W32API TlsGetValue(DWORD);
extern BOOL W32API TlsFree(DWORD);
static volatile struct w32_kernel32_result *r;
static void check(unsigned group,int ok,unsigned line)
{
    r->checked[group]++;
    if (!ok) { r->errors[group]++; if (!r->first_error) r->first_error=line; }
}
#define CHECK(g,c) check(g,!!(c),__LINE__)
static DWORD tls_slot;
static DWORD W32API thread(void *arg)
{
    CHECK(W32_K32_THREADS,TlsGetValue(tls_slot)==0);
    CHECK(W32_K32_THREADS,TlsSetValue(tls_slot,arg));
    CHECK(W32_K32_THREADS,TlsGetValue(tls_slot)==arg);
    return 37;
}
extern BOOL W32API GetExitCodeThread(HANDLE,DWORD *);
void W32API entry(void)
{
    r=VirtualAlloc((void *)W32_KERNEL32_RESULT_VA,4096,0x3000,4);
    if ((uintptr_t)r!=W32_KERNEL32_RESULT_VA) ExitProcess(1);
    r->magic=W32_KERNEL32_MAGIC;r->version=1;
    static const char path[]="C:\\WINDOWS\\F521K32.TMP";
    static const char ini[]="C:\\WINDOWS\\F521K32.INI";
    HANDLE f=CreateFileA(path,0xc0000000,0,0,2,0x80,0);
    CHECK(W32_K32_FILES,f!=K32_INVALID);
    DWORD n=0;char got[4]={0};
    if (f!=K32_INVALID) {
        CHECK(W32_K32_FILES,WriteFile(f,"abc",3,&n,0)&&n==3);
        CHECK(W32_K32_FILES,FlushFileBuffers(f));
        CHECK(W32_K32_FILES,SetFilePointer(f,0,0,0)==0);
        CHECK(W32_K32_FILES,ReadFile(f,got,3,&n,0)&&n==3&&got[0]=='a'&&got[1]=='b'&&got[2]=='c');
        CHECK(W32_K32_TEARDOWN,CloseHandle(f));
    }
    void *p=VirtualAlloc(0,65536,0x3000,4);
    CHECK(W32_K32_MEMORY,p!=0);
    if (p) {
        CHECK(W32_K32_MEMORY,*(volatile DWORD *)p==0);
        *(volatile DWORD *)p=0x521;
        DWORD old=0;struct ciuki_w32_memory_info q={0};
        CHECK(W32_K32_MEMORY,VirtualProtect(p,4096,2,&old)&&old==4);
        CHECK(W32_K32_MEMORY,VirtualQuery(p,&q,sizeof(q))==sizeof(q)&&q.protect==2&&*(volatile DWORD *)p==0x521);
        CHECK(W32_K32_TEARDOWN,VirtualFree(p,0,0x8000));
    }
    tls_slot=TlsAlloc();
    CHECK(W32_K32_THREADS,tls_slot!=0xffffffff);
    if (tls_slot!=0xffffffff) {
        CHECK(W32_K32_THREADS,TlsSetValue(tls_slot,(void *)17));
        HANDLE t=CreateThread(0,0,thread,(void *)29,0,0);
        CHECK(W32_K32_THREADS,t!=0);
        if (t) {
            DWORD code=0;
            CHECK(W32_K32_THREADS,WaitForSingleObject(t,10000)==0);
            CHECK(W32_K32_THREADS,GetExitCodeThread(t,&code)&&code==37);
            CHECK(W32_K32_TEARDOWN,CloseHandle(t));
        }
        CHECK(W32_K32_THREADS,TlsGetValue(tls_slot)==(void *)17);
        CHECK(W32_K32_TEARDOWN,TlsFree(tls_slot));
    }
    CHECK(W32_K32_PROFILE,WritePrivateProfileStringA("probe","value","ciuki",ini));
    char value[16]={0};
    CHECK(W32_K32_PROFILE,GetPrivateProfileStringA("probe","value","missing",value,sizeof(value),ini)==5&&
        value[0]=='c'&&value[1]=='i'&&value[2]=='u'&&value[3]=='k'&&value[4]=='i'&&value[5]==0);
    CHECK(W32_K32_PROFILE,WritePrivateProfileStringA("probe","value",0,ini));
    CHECK(W32_K32_PROFILE,GetPrivateProfileStringA("probe","value","gone",value,sizeof(value),ini)==4&&value[0]=='g');
    CHECK(W32_K32_TEARDOWN,DeleteFileA(path));
    CHECK(W32_K32_TEARDOWN,DeleteFileA(ini));
    r->done=1;
    for (DWORD i=0;!r->release&&i<30000;i++) Sleep(1);
    DWORD errors=0;for (unsigned i=0;i<W32_K32_CASES;i++) errors+=r->errors[i];
    ExitProcess(!r->release||errors?1:0);
}
