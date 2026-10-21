/* Windows 98 W entry refusal precedes any pointer conversion, like A.
 * SPDX-License-Identifier: MIT */
#include <w32/kernel32.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static DWORD error,calls,conversions;
void W32API SetLastError(DWORD e){error=e;}
DWORD W32API GetLastError(void){return error;}
BOOL W32API MoveFileExA(const char *a,const char *b,DWORD flags){
    calls++;assert(!strcmp(a,"source") && !strcmp(b,"target") && !flags);return 1;
}
int W32API WideCharToMultiByte(unsigned cp,DWORD flags,const WCHAR *in,int n,char *out,int cap,const char *def,BOOL *used){
    (void)cp;(void)flags;(void)def;conversions++;assert(n>0 && cap>=n);*used=0;
    for(int i=0;i<n;i++){assert(in[i]<128);out[i]=(char)in[i];}return n;
}
extern BOOL W32API MoveFileExW(const WCHAR *,const WCHAR *,DWORD);
int main(void){
    assert(!MoveFileExW((const WCHAR *)(uintptr_t)1,(const WCHAR *)(uintptr_t)1,4) && error==120);
    assert(!MoveFileExW(NULL,NULL,5) && error==120 && !calls && !conversions);
    static const WCHAR from[]={'s','o','u','r','c','e',0},to[]={'t','a','r','g','e','t',0};
    assert(MoveFileExW(from,to,0) && calls==1 && conversions==2);
    puts("PASS MoveFileExW 9x refusal before path conversion; ordinary move preserved");return 0;
}
