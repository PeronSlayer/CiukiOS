/* Wine COM requests flag 8 before calling the CiukiOS shell class factory.
 * Execute the production facade; this check does not emulate COM. MIT */
#include <w32/kernel32.h>
#include <assert.h>
#include <stdio.h>
static DWORD error,calls;
void W32API SetLastError(DWORD value){error=value;}
DWORD W32API GetLastError(void){return error;}
void W32API RtlInitUnicodeString(UNICODE_STRING *s,const WCHAR *p){
    unsigned n=0;while(p[n])n++;s->Buffer=(WCHAR *)p;s->Length=(uint16_t)(n*2);s->MaximumLength=(uint16_t)((n+1)*2);
}
NTSTATUS W32API LdrLoadDll(const WCHAR *path,ULONG *flags,UNICODE_STRING *name,void **out){
    static const WCHAR expected[]={'s','h','e','l','l','3','2','.','d','l','l',0};
    assert(!path && *flags==8 && name->Length==22);
    for(unsigned i=0;i<12;i++)assert(name->Buffer[i]==expected[i]);
    calls++;*out=(void *)(uintptr_t)0x70000000;return 0;
}
ULONG W32API RtlNtStatusToDosError(NTSTATUS status){(void)status;return 87;}
extern HANDLE W32API k32_load_w(const WCHAR *,HANDLE,DWORD);
int main(void){
    const WCHAR name[]={'s','h','e','l','l','3','2','.','d','l','l',0};
    assert(k32_load_w(name,0,8)==(HANDLE)(uintptr_t)0x70000000 && calls==1);
    assert(!k32_load_w(name,(HANDLE)1,8) && error==87 && calls==1);
    assert(!k32_load_w(name,0,0x100) && error==87 && calls==1);
    puts("PASS COM shell activation LoadLibraryEx altered-search flag and reserved-argument refusal");return 0;
}
