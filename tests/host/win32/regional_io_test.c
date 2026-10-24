/* G24: production NLS settings reader over an explicit file boundary.
 * No host-model bypass, and leak detection stays enabled.
 * SPDX-License-Identifier: MIT */
#define W32_HOST_MODEL
#include <w32/kernel32.h>
#include "../../../sdk/libciuki/regional_model.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static DWORD error=77;
static const char *file;
static unsigned position,closes;
static int read_error;
DWORD W32API GetLastError(void) { return error; }
void W32API SetLastError(DWORD value) { error=value; }
HANDLE W32API CreateFileA(const char *path,DWORD access,DWORD share,SECURITY_ATTRIBUTES *security,DWORD disposition,DWORD flags,HANDLE template)
{
    assert(!strcmp(path,"C:\\system\\settings\\desktop.cfg") && access==0x80000000u && share==7);
    assert(!security && disposition==3 && !flags && !template);
    position=0; error=2; return file?(HANDLE)(uintptr_t)1:K32_INVALID;
}
BOOL W32API CloseHandle(HANDLE h) { assert(h==(HANDLE)(uintptr_t)1); closes++; error=6; return 1; }
BOOL W32API ReadFile(HANDLE h,void *out,DWORD cap,DWORD *done,OVERLAPPED *ov)
{
    assert(h==(HANDLE)(uintptr_t)1 && !ov && cap>=7);
    if(read_error && position>10) { error=5; return 0; }
    unsigned n=(unsigned)strlen(file)-position; if(n>7) n=7;
    memcpy(out,file+position,n); position+=n; *done=n; return 1;
}
#undef W32_HOST_MODEL /* compile the production reader, with modeled file calls */
#include "../../../sdk/win32/kernel32/regional_io.h"
int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    struct regional r;
    k32_regional(&r); assert(!r.italian && error==77 && !closes);
    file=" unrelated = preserved\n regional_locale = it_IT\r\nregional_digits=3\nregional_thousand=\\s\nregional_date=yyyy-MM-dd";
    k32_regional(&r); assert(r.italian && !strcmp(r.value[REG_DIGITS],"3") && !strcmp(r.value[REG_THOUSAND]," ") && !strcmp(r.value[REG_DATE],"yyyy-MM-dd"));
    assert(closes==1 && error==77);
    read_error=1; k32_regional(&r); assert(!r.italian && closes==2 && error==77); read_error=0;
    file="regional_locale=it_IT\nregional_digits=bogus\nregional_decimal=.\nregional_thousand=.\nregional_time=%X\n";
    k32_regional(&r); assert(r.italian && !strcmp(r.value[REG_DIGITS],"2") && !r.value[REG_THOUSAND][0] && reg_consistent(&r));
    static char large[33000]; memset(large,'x',sizeof(large)-1);
    memcpy(large,"regional_locale=it_IT\n",22); file=large;
    k32_regional(&r); assert(!r.italian && closes==4 && error==77);
    puts("PASS G24 NLS settings reader: system drive, fragmented IO, CRLF, invalid/oversize/error fallback, handles, LastError");
    return 0;
}
