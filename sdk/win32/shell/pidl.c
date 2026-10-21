/* Windows 9x installers use the PIDL APIs even when SHGetFolderPath exists.
 * Filesystem items use Wine 10.0 shell32's GUID/drive/FileStruct layout, so
 * the identifiers remain usable by the reused shell, not private tokens.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shell.h>
static const uint8_t computer[16]={0xe0,0x4f,0xd0,0x20,0xea,0x3a,0x69,0x10,0xa2,0xd8,8,0,0x2b,0x30,0x30,0x9d};
static void *reused(const char *name){
  extern HANDLE W32API GetModuleHandleA(const char *);
  HANDLE shell=GetModuleHandleA("shell32.dll");
  return shell?GetProcAddress(shell,name):0;
}
HRESULT W32API SHGetSpecialFolderLocation(HANDLE window,int id,void **out){
  if(!out)return (HRESULT)0x80070057u;*out=0;
  char path[260];
  if(!SHGetSpecialFolderPathA(window,path,id,0)){
    HRESULT(W32API *original)(HANDLE,int,void **)=reused("__ciuki_reused_SHGetSpecialFolderLocation");
    return original?original(window,id,out):(HRESULT)0x80070057u;
  }
  uint8_t data[1024]={0};DWORD at=0;
  if((id&255)!=0){
    /* Desktop -> My Computer -> drive -> relative filesystem components. */
    w32_put16(data,20);data[2]=0x1f;w32_copy(data+4,computer,16);at=20;
    w32_put16(data+at,25);data[at+2]=0x23;w32_copy(data+at+3,path,3);at+=25;
    const char *p=path+3;
    while(*p){
      DWORD n=0;while(p[n] && p[n]!='\\')n++;
      DWORD cb=14+n+2; /* cb, type, FileStruct, long name NUL, empty DOS name */
      if(cb>sizeof(data)-at-2)return (HRESULT)0x80070057u;
      w32_put16(data+at,(uint16_t)cb);data[at+2]=0x31;
      w32_put16(data+at+12,16);w32_copy(data+at+14,p,n);at+=cb;
      p+=n;if(*p)p++;
    }
  }
  uint8_t *pidl=shell_alloc(at+2);if(!pidl)return (HRESULT)0x8007000eu;
  w32_copy(pidl,data,at+2);*out=pidl;return 0;
}
/* 0 means a different namespace: leave those identifiers with Wine. */
static int filesystem_path(const uint8_t *p,char out[260]){
  if(!p)return -1;
  if(!w32_u16(p))return SHGetSpecialFolderPathA(0,out,0,0)?1:-1;
  if(w32_u16(p)!=20 || p[2]!=0x1f)return 0;
  for(unsigned i=0;i<16;i++)if(p[4+i]!=computer[i])return 0;
  p+=20;
  if(w32_u16(p)!=25 || p[2]!=0x23)return 0;
  if(p[3]<'A' || p[3]>'Z' || p[4]!=':' || p[5]!='\\' || p[6])return -1;
  w32_copy(out,p+3,4);DWORD at=3,total=45;p+=25;
  while(w32_u16(p)){
    DWORD cb=w32_u16(p);
    if(cb<16 || cb>1024 || total>4096-cb)return -1;
    if(p[2]!=0x31 && p[2]!=0x32)return 0;
    /* Extended Unicode items and DOS aliases belong to the reused parser;
     * don't substitute their lossy ANSI display names for the real path. */
    if(p[cb-1] || p[cb-2])return 0;
    DWORD n=0;while(n<cb-14 && p[14+n]){
      if(p[14+n]=='\\' || p[14+n]=='/' || p[14+n]==':')return -1;
      n++;
    }
    if(!n || n==cb-14 || (n==1 && p[14]=='.') || (n==2 && p[14]=='.' && p[15]=='.'))return -1;
    if(at>3){if(at==259)return -1;out[at++]='\\';}
    if(n>=260-at)return -1;w32_copy(out+at,p+14,n);at+=n;out[at]=0;
    p+=cb;total+=cb;
  }
  return 1;
}
BOOL W32API SHGetPathFromIDListA(const void *pidl,char *out){
  if(!out)return 0;out[0]=0;char path[260];int result=filesystem_path(pidl,path);
  if(result==1)return shell_copy(out,260,path);
  if(!result){
    BOOL(W32API *original)(const void *,char *)=reused("__ciuki_reused_SHGetPathFromIDListA");
    if(original)return original(pidl,out);
  }
  return 0;
}
BOOL W32API SHGetPathFromIDListW(const void *pidl,WCHAR *out){
  if(!out)return 0;out[0]=0;char path[260];int result=filesystem_path(pidl,path);
  if(result==1)return shell_wide(path,out,260);
  if(!result){
    BOOL(W32API *original)(const void *,WCHAR *)=reused("__ciuki_reused_SHGetPathFromIDListW");
    if(original)return original(pidl,out);
  }
  return 0;
}
