/* Drive paths only; no host paths or automatic Startup execution.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shell.h>
void *shell_alloc(DWORD n) { return HeapAlloc(GetProcessHeap(),8,n); }
void shell_free(void *p) { if(p)HeapFree(GetProcessHeap(),0,p); }
int shell_copy(char *out,DWORD cap,const char *s) {
  if(!out || !s || !cap)return 0;
  DWORD n=0;while(n<cap && s[n])n++;
  if(n==cap){out[0]=0;return 0;}w32_copy(out,s,n+1);return 1;
}
int shell_join(char *out,const char *dir,const char *name) {
  if(!shell_copy(out,260,dir))return 0;
  DWORD n=w32_strlen(out),m=w32_strlen(name);
  if(n && out[n-1]!='\\' && out[n-1]!='/'){if(n==259)return 0;out[n++]='\\';}
  if(m>=260-n)return 0;w32_copy(out+n,name,m+1);return 1;
}
int shell_ansi(const WCHAR *s,char *out,DWORD cap) {
  if(!s || !out || !cap)return 0;
  DWORD n=0;while(n<32768 && s[n])n++;
  if(n==32768)return 0;
  BOOL used=0;
  return WideCharToMultiByte(1252,0,s,(int)n+1,out,(int)cap,0,&used)>0 && !used;
}
int shell_wide(const char *s,WCHAR *out,DWORD cap) {
  return s && out && cap && MultiByteToWideChar(1252,0,s,-1,out,(int)cap)>0;
}
int shell_utf8_encode(const WCHAR *s,char *out,DWORD cap) {
  DWORD at=0;
  for(DWORD i=0;s[i];i++) {
    uint32_t c=s[i];
    if(c>=0xd800 && c<=0xdbff){uint32_t low=s[++i];if(low<0xdc00 || low>0xdfff)return 0;c=0x10000+((c-0xd800)<<10)+low-0xdc00;}
    else if(c>=0xdc00 && c<=0xdfff)return 0;
    unsigned n=c<128?1:c<2048?2:c<65536?3:4;
    if(at>=cap || n>=cap-at)return 0;
    if(n==1)out[at++]=(char)c;
    else for(unsigned j=0;j<n;j++){unsigned shift=(n-1-j)*6;out[at++]=(char)(j?0x80|((c>>shift)&63):(n==2?0xc0:n==3?0xe0:0xf0)|(c>>shift));}
  }
  if(at>=cap)return 0;out[at]=0;return 1;
}
int shell_utf8_decode(const char *s,WCHAR *out,DWORD cap) {
  const uint8_t *p=(const uint8_t *)s;DWORD at=0;
  while(*p){
    uint32_t c=*p++;unsigned n=c<128?0:c>=0xc2 && c<=0xdf?1:c>=0xe0 && c<=0xef?2:c>=0xf0 && c<=0xf4?3:4;
    if(n==4)return 0;
    c&=n?((1u<<(6-n))-1):127;
    for(unsigned j=0;j<n;j++){if((*p&0xc0)!=0x80)return 0;c=(c<<6)|(*p++&63);}
    if((n==1 && c<128) || (n==2 && c<2048) || (n==3 && c<65536) || c>0x10ffff || (c>=0xd800 && c<=0xdfff))return 0;
    if(at+1>=cap)return 0;
    if(c>65535){if(at+2>=cap)return 0;out[at++]=(WCHAR)(0xd800+((c-0x10000)>>10));c=0xdc00+((c-0x10000)&1023);}
    out[at++]=(WCHAR)c;
  }
  if(at>=cap)return 0;out[at]=0;return 1;
}
int shell_mkdirs(const char *path) {
  char p[260];if(!shell_copy(p,sizeof(p),path))return 0;
  if(!p[0] || p[1]!=':' || (p[2]!='\\' && p[2]!='/')){SetLastError(123);return 0;}
  for(unsigned i=3;;i++) {
    if(p[i]!='\\' && p[i]!='/' && p[i])continue;
    char c=p[i];p[i]=0;
    if(!CreateDirectoryA(p,0)) {
      DWORD e=GetLastError(),attr=GetFileAttributesA(p);
      if((e!=183 && e!=80) || attr==~0u || !(attr&16)){SetLastError(e);return 0;}
    }
    p[i]=c;if(!c)break;
  }
  return 1;
}
static const struct {unsigned id;const char *path;} folders[]={
 {0,"C:\\WINDOWS\\Desktop"},{2,"C:\\WINDOWS\\Start Menu\\Programs"},
 {6,"C:\\WINDOWS\\Favorites"},{21,"C:\\WINDOWS\\Templates"},
 {5,"C:\\Documents"},{7,"C:\\WINDOWS\\Start Menu\\Programs\\StartUp"},
 {8,"C:\\WINDOWS\\Recent"},{9,"C:\\WINDOWS\\SendTo"},
 {11,"C:\\WINDOWS\\Start Menu"},{16,"C:\\WINDOWS\\Desktop"},
 {20,"C:\\WINDOWS\\FONTS"},{22,"C:\\WINDOWS\\Start Menu"},
 {23,"C:\\WINDOWS\\Start Menu\\Programs"},{24,"C:\\WINDOWS\\Start Menu\\Programs\\StartUp"},
 {25,"C:\\WINDOWS\\Desktop"},{26,"C:\\WINDOWS\\Application Data"},
 {28,"C:\\WINDOWS\\Application Data"},
 {35,"C:\\WINDOWS\\Application Data"},{36,"C:\\WINDOWS"},
 {37,"C:\\WINDOWS\\SYSTEM"},{38,"C:\\Program Files"},
 {43,"C:\\Program Files\\Common Files"},{46,"C:\\Documents"}
};
extern LONG W32API RegOpenKeyExA(DWORD,const char *,DWORD,DWORD,DWORD *);
extern LONG W32API RegQueryValueExA(DWORD,const char *,void *,DWORD *,void *,DWORD *);
extern LONG W32API RegCloseKey(DWORD);
static int personal_folder(unsigned id,char out[260]) {
  static const struct {unsigned id;const char *name;} names[]={
    {0,"Desktop"},{16,"Desktop"},{2,"Programs"},{5,"Personal"},{6,"Favorites"},
    {7,"Startup"},{8,"Recent"},{9,"SendTo"},{11,"Start Menu"},{21,"Templates"},{26,"AppData"},{28,"AppData"}};
  for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++)if(names[i].id==id){
    DWORD h,type=0,n=260;
    LONG e=RegOpenKeyExA(0x80000001u,"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",0,1,&h);
    if(e){SetLastError((DWORD)e);return -1;}
    e=RegQueryValueExA(h,names[i].name,0,&type,out,&n);RegCloseKey(h);
    if(e || type!=1 || n<4 || n>260 || out[n-1] || out[1]!=':' || out[2]!='\\'){
      SetLastError(e ? (DWORD)e : 13);return -1;
    }
    return 1;
  }
  return 0;
}
BOOL W32API SHGetSpecialFolderPathA(HANDLE w,char *out,int id,BOOL create) {
  (void)w;if(!out){SetLastError(87);return 0;}out[0]=0;
  for(unsigned i=0;i<sizeof(folders)/sizeof(*folders);i++)if(folders[i].id==((unsigned)id&255)) {
    char path[260];
    int personal=personal_folder((unsigned)id&255,path);
    if(personal<0)return 0;
    if(!personal && !shell_copy(path,260,folders[i].path))return 0;
    if((create || (id&0x8000)) && !shell_mkdirs(path))return 0;
    return shell_copy(out,260,path);
  }
  SetLastError(87);return 0;
}
HRESULT W32API SHGetFolderPathA(HANDLE w,int id,HANDLE token,DWORD flags,char *out) {
  if(token || flags>1)return (HRESULT)0x80070057u;
  return SHGetSpecialFolderPathA(w,out,id,0)?0:(HRESULT)(0x80070000u|GetLastError());
}
BOOL W32API SHGetSpecialFolderPathW(HANDLE w,WCHAR *out,int id,BOOL create) {
  char a[260];return SHGetSpecialFolderPathA(w,a,id,create) && shell_wide(a,out,260);
}
HRESULT W32API SHGetFolderPathW(HANDLE w,int id,HANDLE token,DWORD flags,WCHAR *out) {
  char a[260];HRESULT r=SHGetFolderPathA(w,id,token,flags,a);
  return r?r:shell_wide(a,out,260)?0:(HRESULT)0x80070057u;
}
