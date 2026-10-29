/* Actual loader wrappers and ASCII IDN boundary. LGPL-2.1-or-later. */
#include <w32/kernel32.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../../../sdk/win32/kernel32/internet.c"
_Thread_local struct w32_teb *w32_model_teb;
static DWORD error,refs,pinned;
void W32API SetLastError(DWORD e) {error=e;}
ULONG W32API RtlNtStatusToDosError(NTSTATUS status) {return (ULONG)status;}
NTSTATUS W32API RtlEnterCriticalSection(RTL_CRITICAL_SECTION *p) {(void)p;return 0;}
NTSTATUS W32API RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *p) {(void)p;return 0;}
NTSTATUS W32API LdrAddRefDll(ULONG flags,void *base) {assert(base==(void *)0x10000);refs++;pinned=flags;return 0;}
HANDLE W32API k32_module_w(const WCHAR *name) {(void)name;return (void *)0x10000;}
BOOL W32API FreeLibrary(HANDLE module) {(void)module;return 1;}
void W32API ExitThread(DWORD code) {(void)code;assert(0);}
int W32API MultiByteToWideChar(DWORD cp,DWORD flags,const char *in,int n,WCHAR *out,int cap) {
  (void)cp;(void)flags;assert(n==-1);int i=0;
  do {assert(i<cap);out[i]=(uint8_t)in[i];} while(in[i++]);return i;
}
BOOL W32API GetComputerNameA(char *name,DWORD *n) {assert(*n>=8);memcpy(name,"CIUKIOS",8);*n=7;return 1;}
int main(void) {
  struct w32_peb_ldr ldr={0};RTL_CRITICAL_SECTION lock={0};
  struct w32_peb peb={.Ldr=&ldr,.FastPebLock=&lock};struct w32_teb teb={.ProcessEnvironmentBlock=&peb};
  w32_model_teb=&teb;
  struct w32_ldr_entry entry={.DllBase=(void *)0x10000,.SizeOfImage=4096};
  ldr.InLoadOrderModuleList.Flink=ldr.InLoadOrderModuleList.Blink=&entry.InLoadOrderLinks;
  entry.InLoadOrderLinks.Flink=entry.InLoadOrderLinks.Blink=&ldr.InLoadOrderModuleList;
  HANDLE module;
  assert(GetModuleHandleExW(4,(const WCHAR *)0x10123,&module) && module==(void *)0x10000 && refs==1);
  assert(GetModuleHandleExW(6,(const WCHAR *)0x10123,&module) && refs==1);
  assert(GetModuleHandleExA(1,"WININET.DLL",&module) && refs==2 && pinned==1);
  assert(!GetModuleHandleExW(3,0,&module) && error==87);
  assert(!GetModuleHandleExW(4,(const WCHAR *)0x20000,&module) && error==126);
  DWORD n=0;assert(!GetComputerNameExA(1,0,&n) && n==8 && error==234);
  char name[16];n=sizeof(name);assert(GetComputerNameExA(1,name,&n) && !strcmp(name,"CIUKIOS") && n==7);
  n=sizeof(name);assert(GetComputerNameExA(2,name,&n) && !n && !name[0]);
  WCHAR dns[]={'E','x','a','m','p','l','e','.','c','o','m',0},out[32];
  assert(IdnToAscii(2,dns,-1,0,0)==12);
  assert(IdnToAscii(2,dns,-1,out,32)==12 && out[0]=='e' && !out[11]);
  assert(!IdnToAscii(2,dns,-1,out,3) && error==122);
  WCHAR invalid[]={'-','a',0},unicode[]={0xe9,0},ace[]={'x','n','-','-','a',0};
  assert(!IdnToAscii(2,invalid,-1,out,32) && error==123);
  assert(!IdnToAscii(0,unicode,-1,out,32) && error==123);
  assert(!IdnToUnicode(0,ace,-1,out,32) && error==123);
  puts("PASS WinINet loader references/pinning, host/domain names and bounded IDN errors");
}
