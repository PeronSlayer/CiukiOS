/* Microsoft memory-status and Windows 9x Unicode refusal edges.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <w32/kernel32.h>
static DWORD last,physical,available,reserved,error,records;
static struct w32_teb teb;
_Thread_local struct w32_teb *w32_model_teb=&teb;
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"memory edges %u: %s\n",__LINE__,#c);exit(1); } } while (0)
DWORD W32API GetLastError(void){return last;}
void W32API SetLastError(DWORD n){last=n;}
int k32_status(int32_t s){return s==0;}
void *w32_copy(void *a,const void *b,ULONG n){return memmove(a,b,n);}
void *w32_fill(void *a,uint8_t b,ULONG n){return memset(a,b,n);}
ULONG w32_strlen(const char *s){return s?(ULONG)strlen(s):0;}
void w32_diag(const char *e,const char *m,const char *n){(void)e;(void)m;(void)n;records++;}
int32_t w32_ciuki_call(void *record,uint32_t n) {
  CHECK(n==sizeof(struct ciuki_w32_system_args));struct ciuki_w32_system_args *a=record;
  CHECK(a->action==W32_S_MEMORY);a->low=physical;a->high=available;a->extra=reserved;a->error=error;return 0;
}
extern void W32API GlobalMemoryStatus(MEMORYSTATUS *);
extern BOOL W32API GetVersionExW(OSVERSIONINFOW *);
extern DWORD W32API GetModuleFileNameW(HANDLE,WCHAR *,DWORD);
extern HANDLE W32API CreateFileW(const WCHAR *,DWORD,DWORD,SECURITY_ATTRIBUTES *,DWORD,DWORD,HANDLE);
int main(void) {
  MEMORYSTATUSEX x,saved;memset(&x,0xa5,sizeof(x));saved=x;
  CHECK(!GlobalMemoryStatusEx(&x)&&GetLastError()==87&&!memcmp(&x,&saved,sizeof(x)));
  x.length=sizeof(x);CHECK(GlobalMemoryStatusEx(&x)&&!x.load&&!x.total_phys&&!x.avail_phys&&!x.avail_pagefile);
  physical=1;available=2;CHECK(GlobalMemoryStatusEx(&x)&&!x.load&&x.avail_phys==4096);
  physical=1048576;available=524288;reserved=0;
  CHECK(GlobalMemoryStatusEx(&x)&&x.total_phys==UINT64_C(0x100000000)&&x.load==50&&x.avail_phys==UINT64_C(0x80000000));
  CHECK(x.total_pagefile==x.total_phys&&x.avail_pagefile==x.avail_phys&&!x.avail_extended_virtual);
  MEMORYSTATUS m;GlobalMemoryStatus(&m);CHECK(m.length==32&&m.total_phys==0x7fffffff&&m.avail_phys==0x7fffffff);
  CHECK(m.total_virtual==0x7ffe0000u&&m.avail_virtual==m.total_virtual);
  reserved=UINT32_MAX;CHECK(GlobalMemoryStatusEx(&x)&&!x.avail_virtual&&x.avail_phys==UINT64_C(0x80000000));
  available=0;CHECK(GlobalMemoryStatusEx(&x)&&x.load==100);
  error=8;saved=x;CHECK(!GlobalMemoryStatusEx(&x)&&GetLastError()==8&&!memcmp(&x,&saved,sizeof(x)));
  WCHAR w[4]={0xaaaa,0xbbbb,0xcccc,0};
  CHECK(!MultiByteToWideChar(1252,2,"a",1,w,4)&&GetLastError()==1004&&w[0]==0xaaaa);
  CHECK(!MultiByteToWideChar(1252,4,"a",1,w,4)&&GetLastError()==1004&&w[0]==0xaaaa);
  CHECK(records==1);
  CHECK(CreateFileW(w,0,0,0,0,0,0)==K32_INVALID&&GetLastError()==120);
  CHECK(!GetModuleFileNameW(0,w,4)&&w[0]==0xaaaa&&GetLastError()==120);
  OSVERSIONINFOW v;memset(&v,0xa5,sizeof(v));OSVERSIONINFOW before=v;
  CHECK(!GetVersionExW(&v)&&GetLastError()==120&&!memcmp(&v,&before,sizeof(v)));
  puts("PASS memory status edge matrix Unicode refusals conversion records");return 0;
}
