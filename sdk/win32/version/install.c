/* File installation through the ordinary Win32 file API. Builtin providers
 * are immutable; an installer copy is kept beside its installer instead.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#ifdef W32_HOST_MODEL
extern int w32_builtin_version(const char *,uint16_t *) __attribute__((weak));
#else
extern int w32_builtin_version(const char *,uint16_t *);
#endif
extern BOOL W32API CopyFileA(const char *,const char *,BOOL);
extern DWORD W32API GetModuleFileNameA(HANDLE,char *,DWORD);
extern DWORD W32API GetFileVersionInfoSizeA(const char *,DWORD *);
extern BOOL W32API GetFileVersionInfoA(const char *,DWORD,DWORD,void *);
extern BOOL W32API VerQueryValueA(const void *,const char *,void **,DWORD *);
static int join(char out[260],const char *dir,const char *name){
  if(!dir || !name || !w32_copy_string(out,260,dir))return 0;
  DWORD n=w32_strlen(out),m=w32_strlen(name);
  if(n && out[n-1]!='\\' && out[n-1]!='/'){if(n>=259)return 0;out[n++]='\\';}
  if(m>=260-n)return 0;w32_copy(out+n,name,m+1);return 1;
}
static int fixed(const char *name,DWORD *ms,DWORD *ls){
  DWORD ignored,n=GetFileVersionInfoSizeA(name,&ignored);if(!n || n>131080)return 0;
  void *p=HeapAlloc(GetProcessHeap(),0,n);if(!p)return 0;
  void *value=0;DWORD bytes=0;
  int ok=GetFileVersionInfoA(name,0,n,p) && VerQueryValueA(p,"\\",&value,&bytes) && bytes>=52;
  if(ok){*ms=w32_u32((uint8_t *)value+8);*ls=w32_u32((uint8_t *)value+12);}
  HeapFree(GetProcessHeap(),0,p);return ok;
}
DWORD W32API VerInstallFileA(DWORD flags,const char *src,const char *dst,const char *srcdir,const char *dstdir,const char *current,char *temporary,DWORD *cap){
  (void)current;
  if(!src || !dst || !cap || !temporary || flags&~3u)return 0x200u;
  if(!*cap){*cap=1;return 0x40000u;}temporary[0]=0;*cap=1;
  char from[260],to[260];if(!join(from,srcdir,src) || !join(to,dstdir,dst))return 0x200u;
  if(GetFileAttributesA(from)==~0u)return 0x10000u;
  uint16_t version[4];int protected=0;
#ifdef W32_HOST_MODEL
  if(w32_builtin_version)
#endif
  protected=w32_builtin_version(dst,version);
  if(protected){
    char exe[260];DWORD n=GetModuleFileNameA(0,exe,260);
    if(!n || n>=260)return 0x800u;
    char *last=0;for(char *p=exe;*p;p++)if(*p=='\\' || *p=='/')last=p;
    if(!last)return 0x800u;*last=0;
    if(!join(to,exe,dst))return 0x800u;
    w32_diag("installer","builtin-copy","application-directory; dll-native shim required");
  }else if(!(flags&1)){
    DWORD sm,sl,dm,dl;
    if(fixed(from,&sm,&sl) && fixed(to,&dm,&dl) && (sm<dm || (sm==dm && sl<dl)))return 2|4;
  }
  if(!w32_stricmp(from,to))return 0;
  if(CopyFileA(from,to,0))return 0;
  DWORD e=GetLastError();return e==112?0x100u:e==19?0x40u:e==32?0x480u:e==2 || e==3?0x10000u:0x800u;
}
static int narrow(const WCHAR *w,char a[260]){
  BOOL used=0;return w && WideCharToMultiByte(1252,0,w,-1,a,260,0,&used)>0 && !used;
}
DWORD W32API VerInstallFileW(DWORD flags,const WCHAR *src,const WCHAR *dst,const WCHAR *srcdir,const WCHAR *dstdir,const WCHAR *current,WCHAR *temporary,DWORD *cap){
  char s[260],d[260],sd[260],dd[260],c[260],tmp[260];
  if(!temporary || !cap || !narrow(src,s) || !narrow(dst,d) || !narrow(srcdir,sd) || !narrow(dstdir,dd) || (current && !narrow(current,c)))return 0x200u;
  DWORD n=*cap;DWORD result=VerInstallFileA(flags,s,d,sd,dd,current?c:0,tmp,&n);
  if(*cap && !(result&0x40000u))temporary[0]=0;*cap=n;return result;
}
