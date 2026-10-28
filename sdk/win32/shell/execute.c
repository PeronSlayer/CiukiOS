/* Shell dispatch preserves executable, argument tail and cwd separately.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shell.h>
#include <w32/shims.h>
#include <w32/user32.h>
extern LONG W32API RegOpenKeyExA(DWORD,const char *,DWORD,DWORD,DWORD *);
extern LONG W32API RegQueryValueExA(DWORD,const char *,DWORD *,DWORD *,void *,DWORD *);
extern LONG W32API RegCloseKey(DWORD);
static const char *base(const char *s){const char *p=s;for(;*s;s++)if(*s=='\\' || *s=='/')p=s+1;return p;}
static const char *extension(const char *s){const char *p=base(s),*ext="";for(;*p;p++)if(*p=='.')ext=p;return ext;}
static int read_default(const char *key,char *out,DWORD cap){
  DWORD h=0,n=cap,type=0;
  if(RegOpenKeyExA(0x80000000u,key,0,0x20019,&h))return 0;
  LONG e=RegQueryValueExA(h,"",0,&type,out,&n);RegCloseKey(h);
  if(e || type!=1 || !n || n>cap || out[n-1])return 0;
  for(DWORD i=0;i+1<n;i++)if(!out[i])return 0;
  return 1;
}
static DWORD refused(const char *file){
  if(!w32_stricmp(extension(file),".msi") || !w32_stricmp(base(file),"msiexec.exe") || !w32_stricmp(base(file),"msi.dll")){
    w32_report_refusal(W32_REFUSE_MSI,file,120);
    return 120;
  }
  for(const char *p=file;*p;p++)if(*p==':' && p!=file+1){w32_diag("refuse","shell-url",file);return 1155;}
  return 0;
}
static int append(char *out,DWORD cap,DWORD *at,const char *s){
  while(*s){if(*at+1>=cap)return 0;out[(*at)++]=*s++;}out[*at]=0;return 1;
}
static DWORD command_for(const char *file,const char *verb,const char *parameters,char *exe,char *command){
  const char *ext=extension(file);char templ[2048];
  if(!w32_stricmp(ext,".exe") || !w32_stricmp(ext,".com") || !w32_stricmp(ext,".bat")){
    if(w32_stricmp(verb,"open"))return 1155;
    if(!shell_copy(exe,260,file))return 206;
    DWORD at=0;command[0]=0;
    if(!append(command,32768,&at,"\"") || !append(command,32768,&at,file) || !append(command,32768,&at,"\""))return 206;
    if(parameters && *parameters && (!append(command,32768,&at," ") || !append(command,32768,&at,parameters)))return 206;
    return 0;
  }
  char cls[260],key[520];
  if(!*ext || !read_default(ext,cls,sizeof(cls)))return 1155;
  DWORD at=0;key[0]=0;
  if(!append(key,sizeof(key),&at,cls) || !append(key,sizeof(key),&at,"\\shell\\") || !append(key,sizeof(key),&at,verb) || !append(key,sizeof(key),&at,"\\command") || !read_default(key,templ,sizeof(templ)))return 1155;
  at=0;command[0]=0;
  for(unsigned i=0;templ[i];i++){
    const char *sub=0;char c[2]={templ[i],0};
    if(templ[i]=='%' && templ[i+1]){
      if(templ[i+1]=='1' || templ[i+1]=='l' || templ[i+1]=='L')sub=file;
      else if(templ[i+1]=='*')sub=parameters?parameters:"";
      else if(templ[i+1]=='%')sub="%";
      else return 1155;
      i++;
    }
    if(!append(command,32768,&at,sub?sub:c))return 206;
  }
  const char *p=command;while(*p==' ' || *p=='\t')p++;
  int quote=*p=='"';if(quote)p++;unsigned n=0;
  while(*p && (quote?*p!='"':*p!=' ' && *p!='\t')){if(n==259)return 206;exe[n++]=*p++;}
  exe[n]=0;if(!n || (quote && *p!='"'))return 1155;
  return 0;
}
/* fx-q3vise: folders. On Windows 98 a directory is a Folder object whose
 * "open" (also the default verb) and "explore" verbs show it in an Explorer
 * window: class CabinetWClass, or ExploreWClass for "explore", titled with
 * the folder's name (the default "Display the full path in title bar" is
 * off); Explorer activates the window it already has for that folder.
 * Microsoft Learn, ShellExecuteA: "To open a folder, use ... ShellExecute(
 * handle, NULL, <fully_qualified_path_to_folder>, ...)" and "explore";
 * class names: The Old New Thing, 2015-06-19 ("Why does the class name for
 * Explorer change depending on whether you open it with /e?"), programs
 * find Explorer's window with FindWindow on CabinetWClass. The Installer
 * VISE engine (Quake III Arena demo) opens every Start-menu folder it filled
 * and polls FindWindow("CabinetWClass", <path>) and (..., <name>) for five
 * seconds without pumping messages; before this the open failed with
 * ERROR_NO_ASSOCIATION and ten shortcuts cost fifty seconds of a
 * not-responding installer. CiukiOS shows folders in Files: the desktop
 * starts it on the folder (user32 CiukiShellOpenFolder). The shell keeps,
 * per folder opened by this process, a hidden top-level window of the
 * Explorer class with Explorer's title, so FindWindow and IsWindow answer
 * as with Explorer's window; a second open of the same folder reuses it and
 * starts nothing, as Explorer only activates its window. */
#define FOLDER_WINDOWS 16
static struct {HWND window;BOOL explore;char path[260];} folder_windows[FOLDER_WINDOWS];
extern BOOL W32API CiukiShellOpenFolder(HWND,const char *);
static LRESULT W32API folder_procedure(HWND h,UINT message,WPARAM wp,LPARAM lp){return DefWindowProcA(h,message,wp,lp);}
/* The Files path of a Win32 drive path, as Files maps drives (C: is the
 * root, another drive X: is /mnt/x): apps/files/model.c fm_posix_path. */
static int folder_posix(const char *full,char *out,DWORD cap){
  DWORD at=0;
  char drive=full[0]>='a' && full[0]<='z'?(char)(full[0]-32):full[0];
  if(drive<'A' || drive>'Z' || full[1]!=':' || full[2]!='\\')return 0;
  if(drive!='C'){if(cap<8)return 0;w32_copy(out,"/mnt/",5);out[5]=(char)(drive+32);at=6;}
  for(const char *p=full+2;*p;p++){
    char c=*p=='\\'?'/':*p;
    if(c=='/' && at && out[at-1]=='/')continue;
    if(at+1>=cap)return 0;
    out[at++]=c;
  }
  while(at>1 && out[at-1]=='/')at--;
  out[at]=0;return at>1;
}
static DWORD open_folder(const char *file,BOOL explore){
  char full[260],posix[260],title[260];
  DWORD n=GetFullPathNameA(file,sizeof(full),full,0);
  if(!n || n>=sizeof(full))return n?206:GetLastError();
  while(n>3 && full[n-1]=='\\')full[--n]=0;
  /* A drive root (My Computer's drive window) and UNC folders have no Files
   * launch here: they keep the earlier refusal. */
  if(n<=3 || !folder_posix(full,posix,sizeof(posix)))return 1155;
  if(!shell_copy(title,sizeof(title),base(full)))return 206;
  for(unsigned i=0;i<FOLDER_WINDOWS;i++)if(folder_windows[i].window && folder_windows[i].explore==explore &&
      !w32_stricmp(folder_windows[i].path,full)){
    if(IsWindow(folder_windows[i].window))return 0;
    folder_windows[i].window=0;
  }
  const char *klass=explore?"ExploreWClass":"CabinetWClass";
  WNDCLASSA cls={.lpfnWndProc=folder_procedure,.lpszClassName=klass};
  RegisterClassA(&cls);   /* already registered by an earlier folder: the window creation decides */
  HWND window=CreateWindowExA(0,klass,title,0x80000000u /* WS_POPUP, hidden */,0,0,1,1,0,0,0,0);
  if(!window)return GetLastError()?GetLastError():8;
  if(!CiukiShellOpenFolder(window,posix)){DWORD e=GetLastError();DestroyWindow(window);return e?e:1155;}
  for(unsigned i=0;i<FOLDER_WINDOWS;i++)if(!folder_windows[i].window){
    folder_windows[i].window=window;folder_windows[i].explore=explore;shell_copy(folder_windows[i].path,260,full);break;
  }
  return 0;
}
BOOL W32API ShellExecuteExA(SHELL_EXECUTE *a){
  if(!a || a->size!=sizeof(*a) || !a->file){SetLastError(87);return 0;}
  a->process=0;a->instance=0;
  const char *verb=a->verb && *a->verb?a->verb:"open";
  DWORD e=refused(a->file);
  if(!e && w32_stricmp(verb,"open") && w32_stricmp(verb,"explore") && w32_stricmp(verb,"print"))e=1155;
  if(!e && w32_stricmp(verb,"print")){
    DWORD attributes=GetFileAttributesA(a->file);
    if(attributes!=0xffffffffu && (attributes&0x10)){
      e=open_folder(a->file,!w32_stricmp(verb,"explore"));
      if(!e){a->instance=(HANDLE)(uintptr_t)33;return 1;}
      SetLastError(e);a->instance=(HANDLE)(uintptr_t)(e==2 || e==3?e:e==5?5:31);return 0;
    }
  }
  char exe[260];char *command=0;
  if(!e){command=shell_alloc(32768);if(!command)e=8;}
  if(!e)e=command_for(a->file,verb,a->parameters,exe,command);
  if(!e){
    struct shell_startup s={.size=sizeof(s),.flags=1,.show=(uint16_t)a->show};struct shell_process p={0};
    if(!CreateProcessA(exe,command,0,0,0,0,0,a->directory,&s,&p))e=GetLastError();
    else {CloseHandle(p.thread);if(a->mask&0x40)a->process=p.process;else CloseHandle(p.process);a->instance=(HANDLE)(uintptr_t)33;}
  }
  shell_free(command);
  if(e){SetLastError(e);a->instance=(HANDLE)(uintptr_t)(e==2 || e==3?e:e==5?5:31);return 0;}
  return 1;
}
HANDLE W32API ShellExecuteA(HANDLE owner,const char *verb,const char *file,const char *parameters,const char *directory,int show){
  SHELL_EXECUTE a={.size=sizeof(a),.window=owner,.verb=verb,.file=file,.parameters=parameters,.directory=directory,.show=show};
  ShellExecuteExA(&a);return a.instance;
}
BOOL W32API ShellExecuteExW(SHELL_EXECUTE *w){
  if(!w || w->size!=sizeof(*w)){SetLastError(87);return 0;}
  char file[260],verb[64],directory[260];char *params=shell_alloc(32768);
  if(!params){SetLastError(8);return 0;}
  if(!shell_ansi((const WCHAR *)w->file,file,260) || (w->verb && !shell_ansi((const WCHAR *)w->verb,verb,64)) ||
     (w->directory && !shell_ansi((const WCHAR *)w->directory,directory,260)) ||
     (w->parameters && !shell_ansi((const WCHAR *)w->parameters,params,32768))){shell_free(params);SetLastError(1113);return 0;}
  SHELL_EXECUTE a=*w;a.file=file;a.verb=w->verb?verb:0;a.directory=w->directory?directory:0;a.parameters=w->parameters?params:0;
  BOOL ok=ShellExecuteExA(&a);w->process=a.process;w->instance=a.instance;shell_free(params);return ok;
}
HANDLE W32API ShellExecuteW(HANDLE owner,const WCHAR *verb,const WCHAR *file,const WCHAR *parameters,const WCHAR *directory,int show){
  SHELL_EXECUTE a={.size=sizeof(a),.window=owner,.verb=(const char *)verb,.file=(const char *)file,.parameters=(const char *)parameters,.directory=(const char *)directory,.show=show};
  ShellExecuteExW(&a);return a.instance;
}
HANDLE W32API FindExecutableA(const char *file,const char *directory,char *out){
  (void)directory;char *command=shell_alloc(32768);char exe[260];if(!command)return (HANDLE)(uintptr_t)8;
  DWORD e=refused(file);if(!e)e=command_for(file,"open",0,exe,command);
  shell_free(command);if(e || !shell_copy(out,260,exe))return (HANDLE)(uintptr_t)31;
  return (HANDLE)(uintptr_t)33;
}
DWORD W32API SHAppBarMessage(DWORD message,void *data){(void)message;(void)data;return 0;}
extern BOOL W32API CiukiShellNotify(DWORD,void *,BOOL);
BOOL W32API Shell_NotifyIconA(DWORD message,void *data){return CiukiShellNotify(message,data,0);}
BOOL W32API Shell_NotifyIconW(DWORD message,void *data){return CiukiShellNotify(message,data,1);}
void W32API SHChangeNotify(LONG event,ULONG flags,const void *a,const void *b){(void)event;(void)flags;(void)a;(void)b;}
