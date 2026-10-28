/* Small loader/NLS endpoints needed by Wine WinINet. LGPL-2.1-or-later. */
#include <w32/kernel32.h>
extern NTSTATUS W32API LdrAddRefDll(ULONG,void *);
extern BOOL W32API FreeLibrary(HANDLE);
extern BOOL W32API GetComputerNameA(char *,DWORD *);
extern HANDLE W32API k32_module_w(const WCHAR *);
void W32API FreeLibraryAndExitThread(HANDLE module,DWORD code) {
  FreeLibrary(module);ExitThread(code);
}
BOOL W32API GetModuleHandleExW(DWORD flags,const WCHAR *name,HANDLE *out) {
  if(!out || flags&~7u || (flags&3u)==3u || ((flags&4) && !name)) {SetLastError(87);return 0;}
  *out=0;HANDLE h=0;
  struct w32_peb *peb=NtCurrentTeb()->ProcessEnvironmentBlock;
  if(!peb || !peb->Ldr || !peb->FastPebLock) {SetLastError(126);return 0;}
  RtlEnterCriticalSection(peb->FastPebLock);
  if(flags&4) {
    LIST_ENTRY *head=&peb->Ldr->InLoadOrderModuleList;
    for(LIST_ENTRY *e=head->Flink;e!=head;e=e->Flink) {
      struct w32_ldr_entry *m=(void *)e;
      uintptr_t address=(uintptr_t)name,base=(uintptr_t)m->DllBase;
      if(address>=base && address-base<m->SizeOfImage) {h=m->DllBase;break;}
    }
  } else h=k32_module_w(name);
  NTSTATUS status=h && !(flags&2) ? LdrAddRefDll(flags&1,h) : 0;
  RtlLeaveCriticalSection(peb->FastPebLock);
  if(status || !h) {SetLastError(status ? RtlNtStatusToDosError(status) : 126);return 0;}
  *out=h;return 1;
}
BOOL W32API GetModuleHandleExA(DWORD flags,const char *name,HANDLE *out) {
  WCHAR w[260];
  if(name && !(flags&4) && !MultiByteToWideChar(1252,0,name,-1,w,260)) return 0;
  return GetModuleHandleExW(flags,name && !(flags&4) ? w : (const WCHAR *)name,out);
}
BOOL W32API GetComputerNameExA(DWORD kind,char *out,DWORD *count) {
  if(!count || kind>7) {SetLastError(87);return 0;}
  char name[256];DWORD n=sizeof(name);
  if(kind==2 || kind==6) {name[0]=0;n=0;}
  else if(!GetComputerNameA(name,&n)) return 0;
  if(!out || *count<=n) {*count=n+1;SetLastError(234);return 0;}
  w32_copy(out,name,n+1);*count=n;return 1;
}
/* The Windows 98 personality has no Unicode Nameprep database. ASCII DNS
 * labels are handled exactly; names requiring IDNA mapping fail explicitly. */
static int idn(DWORD flags,const WCHAR *in,int n,WCHAR *out,int cap) {
  if(flags&~3u || !in || n==0 || n< -1 || cap<0 || (!out && cap)) {SetLastError(87);return 0;}
  if(n==-1) {n=0;while(n<256 && in[n]) n++;if(n==256) {SetLastError(123);return 0;}n++;}
  if(n>256) {SetLastError(123);return 0;}
  unsigned label=0;int last=0;
  for(int i=0;i<n;i++) {
    unsigned c=in[i];
    if(!c && i==n-1) break;
    if(c=='.') {if(!label || ((flags&2) && last=='-')) {SetLastError(123);return 0;}label=0;last='.';continue;}
    if(c<33 || c>=127 || c=='/' || c=='\\' || c==':' || ((flags&2) &&
        !((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='-')) ||
        ((flags&2) && !label && c=='-') || ++label>63) {SetLastError(123);return 0;}
    last=(int)c;
  }
  if(!label && last!='.') {SetLastError(123);return 0;}
  if((flags&2) && last=='-') {SetLastError(123);return 0;}
  if(!cap) return n;
  if(cap<n) {SetLastError(122);return 0;}
  for(int i=0;i<n;i++) out[i]=in[i]>='A' && in[i]<='Z' ? in[i]+32 : in[i];
  return n;
}
int W32API IdnToAscii(DWORD flags,const WCHAR *in,int n,WCHAR *out,int cap) {return idn(flags,in,n,out,cap);}
int W32API IdnToUnicode(DWORD flags,const WCHAR *in,int n,WCHAR *out,int cap) {
  /* Do not pass an ACE label through as if it had been decoded. */
  if(in && (n>=4 || n==-1)) for(int i=0;i<(n==-1 ? 252 : n-3) && in[i];i++)
    if((i==0 || in[i-1]=='.') && (in[i]=='x'||in[i]=='X') && (in[i+1]=='n'||in[i+1]=='N') && in[i+2]=='-' && in[i+3]=='-') {
      SetLastError(123);return 0;
    }
  return idn(flags,in,n,out,cap);
}
