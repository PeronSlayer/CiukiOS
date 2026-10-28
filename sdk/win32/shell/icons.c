/* fx-exeicons: the icon the Windows 98 shell shows for a file, for
 * SHGetFileInfoA and ExtractIconA (docs/design/file-icons.md).
 * A program, library or icon file shows its first icon group; a shortcut
 * its IconLocation, else its target's icon; a PIF the icon of its "WINDOWS
 * VMM 4.0" section, else its program's; an Internet shortcut its IconFile;
 * a read-only or system folder its desktop.ini icon; a drive root its
 * AUTORUN.INF icon=. Otherwise the shell's default document, folder or
 * program icon (user32 CiukiShellIcon, the Tango icons the native shell
 * uses). The parsers are the bounded iconx core shared with libdeskui.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shell.h>
#include <w32/user32.h>
#include "../../../apps/libdeskui/iconx.h"
extern UINT W32API PrivateExtractIconsA(const char *,int,int,int,HICON *,UINT *,UINT,UINT);
extern HICON W32API CiukiShellIcon(UINT);
#define SHELL_LINK_BYTES (64u * 1024u)
enum { DEFAULT_DOCUMENT, DEFAULT_FOLDER, DEFAULT_PROGRAM, DEFAULT_FOLDER_OPEN };
struct fileinfo {HICON icon;int index;DWORD attributes;char display[260],type[80];};
struct location { char file[260]; int index; int own, fallback; };

static char lower(char c){return c>='A' && c<='Z' ? (char)(c+32) : c;}
static int same(const char *a,const char *b){while(*a && *b)if(lower(*a++)!=lower(*b++))return 0;return !*a && !*b;}
static const char *extension(const char *path)
{
  const char *base=path,*dot=0;
  for(const char *p=path;*p;p++){if(*p=='\\' || *p=='/'){base=p+1;dot=0;}else if(*p=='.')dot=p;}
  return dot && dot!=base ? dot+1 : "";
}
static int one_of(const char *ext,const char *const *list){for(;*list;list++)if(same(ext,*list))return 1;return 0;}
static const char *const own_types[]={"exe","dll","cpl","scr","ocx","icl","ico","cur","ani",0};

/* A small file read whole: a link, PIF, INI. */
static uint8_t *read_small(const char *path,DWORD *n)
{
  HANDLE f=CreateFileA(path,0x80000000u,1,0,3,0,0);
  if(f==K32_INVALID || !f)return 0;
  DWORD size=GetFileSize(f,0),got=0;uint8_t *data=0;
  if(size && size<=SHELL_LINK_BYTES && (data=shell_alloc(size)) && (!ReadFile(f,data,size,&got,0) || got!=size)){shell_free(data);data=0;}
  CloseHandle(f);
  if(data)*n=size;
  return data;
}
static void folder_of(const char *path,char *out)
{
  shell_copy(out,260,path);
  char *cut=0;for(char *p=out;*p;p++)if(*p=='\\' || *p=='/')cut=p;
  if(cut)cut[cut==out+2 && out[1]==':' ? 1 : 0]=0;
}
/* A location named relative to a folder ("icons\a.ico", "\a.ico") or by
 * %SystemRoot%; absolute ones are kept. 0 if it does not fit. */
static int absolute(const char *location,const char *folder,char *out)
{
  char text[260];unsigned n=0;
  while(*location==' ' || *location=='"')location++;
  for(const char *p=location;*p;p++){
    if(*p=='%'){
      static const char *const names[]={"SystemRoot","windir"};
      const char *end=p+1;while(*end && *end!='%')end++;
      int known=0;
      for(unsigned k=0;*end && k<2;k++){
        unsigned len=0;while(names[k][len])len++;
        if((unsigned)(end-p-1)==len){int ok=1;for(unsigned j=0;j<len;j++)if(lower(p[1+j])!=lower(names[k][j]))ok=0;if(ok)known=1;}
      }
      if(!known)return 0;
      const char *value="C:\\WINDOWS";for(;*value;value++){if(n+1>=sizeof(text))return 0;text[n++]=*value;}
      p=end;continue;
    }
    if(n+1>=sizeof(text))return 0;
    text[n++]=*p=='/' ? '\\' : *p;
  }
  while(n && (text[n-1]==' ' || text[n-1]=='"'))n--;
  text[n]=0;
  if(!n)return 0;
  int ok;
  if(text[1]==':' || (text[0]=='\\' && text[1]=='\\'))ok=shell_copy(out,260,text);
  else if(text[0]=='\\'){char drive[3]={folder[0] ? folder[0] : 'C',':',0};ok=shell_join(out,drive,text+1);}
  else ok=shell_join(out,folder,text);
  if(!ok || out[1]!=':' || out[2]!='\\')return ok;
  /* "X:\a\..\b" -> "X:\b": the canonical name SHGFI_ICONLOCATION reports. */
  char *w=out+3;const char *r=out+3;
  while(*r){
    const char *end=r;while(*end && *end!='\\')end++;
    unsigned len=(unsigned)(end-r);
    if(len==1 && r[0]=='.'){}
    else if(len==2 && r[0]=='.' && r[1]=='.'){
      if(w>out+3){w--;while(w>out+3 && w[-1]!='\\')w--;}
      else return 0;   /* above the drive's root */
    } else if(len){
      for(unsigned k=0;k<len;k++)*w++=r[k];
      if(*end)*w++='\\';
    }
    r=*end ? end+1 : end;
  }
  if(w>out+3 && w[-1]=='\\')w--;
  *w=0;
  return 1;
}

static void resolve(const char *path,DWORD attributes,struct location *out,unsigned depth)
{
  out->file[0]=0;out->index=0;out->own=0;
  if(attributes!=0xffffffffu && (attributes&16)){
    out->fallback=DEFAULT_FOLDER;
    char ini[300],icon[260],folder[260];int32_t index=0;DWORD n=0;uint8_t *data;
    shell_copy(folder,260,path);
    unsigned len=0;while(folder[len])len++;
    while(len>3 && (folder[len-1]=='\\' || folder[len-1]=='/'))folder[--len]=0;
    if(len<=3 && folder[1]==':'){          /* a drive root: AUTORUN.INF */
      if(shell_join(ini,folder,"autorun.inf") && (data=read_small(ini,&n))){
        int found=iconx_autorun_inf(data,n,icon,sizeof(icon),&index);shell_free(data);
        if(found && absolute(icon,folder,out->file)){out->index=index;out->own=1;return;}
      }
    }
    if(attributes&(1|4)){                  /* read-only or system: desktop.ini */
      if(shell_join(ini,folder,"desktop.ini") && (data=read_small(ini,&n))){
        int found=iconx_desktop_ini(data,n,icon,sizeof(icon),&index);shell_free(data);
        if(found && absolute(icon,folder,out->file)){out->index=index;out->own=1;}
      }
    }
    return;
  }
  const char *ext=extension(path);
  char folder[260];folder_of(path,folder);
  out->fallback=one_of(ext,own_types) && !same(ext,"ico") && !same(ext,"cur") && !same(ext,"ani") ? DEFAULT_PROGRAM : DEFAULT_DOCUMENT;
  if(one_of(ext,own_types)){shell_copy(out->file,260,path);out->own=1;return;}
  if(same(ext,"lnk")){
    DWORD n=0;uint8_t *data=read_small(path,&n);
    struct iconx_link *link=data ? shell_alloc(sizeof(*link)) : 0;
    int parsed=link && !iconx_link_parse(data,n,link);
    if(data)shell_free(data);
    if(parsed && link->icon[0] && absolute(link->icon,folder,out->file)){out->index=link->icon_index;out->own=1;}
    else if(parsed && link->target[0] && depth<4){
      char target[260];
      if(absolute(link->target,folder,target))resolve(target,GetFileAttributesA(target),out,depth+1);
    }
    if(link)shell_free(link);
    return;
  }
  if(same(ext,"pif")){
    out->fallback=DEFAULT_PROGRAM;
    DWORD n=0;uint8_t *data=read_small(path,&n);char icon[260],program[260];int32_t index=0;
    int named=data ? iconx_pif_parse(data,n,icon,sizeof(icon),&index,program,sizeof(program)) : -1;
    if(data)shell_free(data);
    if(named>0 && absolute(icon,folder,out->file)){out->index=index;out->own=1;}
    else if(named>=0 && program[0] && same(extension(program),"exe") && absolute(program,folder,out->file))out->own=1;
    return;
  }
  if(same(ext,"url")){
    DWORD n=0;uint8_t *data=read_small(path,&n);char icon[260],value[32];int32_t index=0;
    if(data && iconx_ini_get(data,n,"InternetShortcut","IconFile",icon,sizeof(icon)) && icon[0] && absolute(icon,folder,out->file)){
      if(iconx_ini_get(data,n,"InternetShortcut","IconIndex",value,sizeof(value))){
        char probe[40]="x,";shell_copy(probe+2,38,value);iconx_location_split(probe,&index);
      }
      out->index=index;out->own=1;
    }
    if(data)shell_free(data);
  }
}

/* ExtractIcon: NULL without icons, (HICON)1 for a file that is neither a
 * program, a library nor an icon file (Microsoft, ExtractIconA). */
HICON W32API ExtractIconA(HINSTANCE instance,const char *file,UINT index){
  (void)instance;
  if(index==~0u)return (HICON)(uintptr_t)PrivateExtractIconsA(file,0,32,32,0,0,0,0);
  HICON icon=0;UINT n=PrivateExtractIconsA(file,(int)index,32,32,&icon,0,1,0);
  if(n==1)return icon;
  if(n==0xffffffffu || !file)return 0;
  DWORD size=0;HANDLE f=CreateFileA(file,0x80000000u,1,0,3,0,0);
  if(f==K32_INVALID || !f)return 0;
  uint8_t head[64];DWORD got=0;
  if(ReadFile(f,head,sizeof(head),&got,0))size=got;
  CloseHandle(f);
  struct iconx_memory m={head,size};
  struct iconx *x=shell_alloc(sizeof(*x));
  if(!x)return 0;
  iconx_init(x,iconx_memory_read,&m,size);
  int kind=iconx_kind(x);
  shell_free(x);
  return kind==ICONX_UNKNOWN ? (HICON)(uintptr_t)1 : 0;
}
HICON W32API ExtractIconW(HINSTANCE instance,const WCHAR *file,UINT index){char a[260];return shell_ansi(file,a,260)?ExtractIconA(instance,a,index):0;}

/* SHGFI_EXETYPE: MZ (DOS, HIWORD 0), NE/PE with the expected Windows
 * version in the HIWORD, PE console programs with HIWORD 0. */
static uintptr_t exe_type(const char *file)
{
  HANDLE f=CreateFileA(file,0x80000000u,1,0,3,0,0);
  if(f==K32_INVALID || !f)return 0;
  DWORD size=GetFileSize(f,0),take=size<4096 ? size : 4096,got=0;
  uint8_t *data=take ? shell_alloc(take) : 0;
  uintptr_t result=0;
  if(data && ReadFile(f,data,take,&got,0) && got==take && take>=64 && data[0]=='M' && data[1]=='Z'){
    DWORD at=w32_u32(data+60);
    result=0x5a4d;
    if(at<=take-96 && data[at]=='P' && data[at+1]=='E' && !data[at+2] && !data[at+3]){
      DWORD optional=at+24,subsystem=w32_u16(data+optional+68),major=w32_u16(data+optional+48),minor=w32_u16(data+optional+50);
      result=0x4550|(subsystem==3 ? 0 : (uintptr_t)((major<<8|minor)&0xffff)<<16);
    } else if(at<=take-64 && data[at]=='N' && data[at+1]=='E')
      result=0x454e|(uintptr_t)w32_u16(data+at+0x3e)<<16;
  }
  if(data)shell_free(data);
  CloseHandle(f);
  return result;
}

uintptr_t W32API SHGetFileInfoA(const char *file,DWORD attr,struct fileinfo *out,UINT size,UINT flags){
  if(flags&0x2000)return flags==0x2000 && file ? exe_type(file) : 0;   /* SHGFI_EXETYPE alone */
  if(!file || !out || size<sizeof(*out) || (flags&8)){SetLastError(87);return 0;}
  w32_fill(out,0,sizeof(*out));
  if(!(flags&0x10)){attr=GetFileAttributesA(file);if(attr==~0u)return 0;}
  out->attributes=attr;
  const char *base=file;for(const char *s=file;*s;s++)if(*s=='\\' || *s=='/')base=s+1;
  if(flags&0x200)if(!shell_copy(out->display,260,base))return 0;
  if(flags&0x400){
    const char *ext=extension(file);
    const char *type=attr&16 ? "File Folder" : same(ext,"exe") || same(ext,"com") ? "Application" :
      same(ext,"lnk") ? "Shortcut" : same(ext,"pif") ? "Shortcut to MS-DOS Program" : same(ext,"ico") ? "Icon" :
      same(ext,"dll") ? "Application Extension" : 0;
    if(type)shell_copy(out->type,80,type);
    else if(*ext){char t[80];unsigned k=0;for(;ext[k] && k<10;k++)t[k]=ext[k]>='a' && ext[k]<='z' ? (char)(ext[k]-32) : ext[k];
      shell_copy(t+k,80-k," File");shell_copy(out->type,80,t);}
    else shell_copy(out->type,80,"File");
  }
  if(flags&(0x100|0x1000)){
    struct location *l=shell_alloc(sizeof(*l));
    if(!l)return 0;
    if(flags&0x10){                         /* SHGFI_USEFILEATTRIBUTES: by type only */
      l->own=0;l->fallback=attr&16 ? DEFAULT_FOLDER : one_of(extension(file),own_types) ? DEFAULT_PROGRAM : DEFAULT_DOCUMENT;
    } else resolve(file,attr,l,0);
    if((flags&2) && l->fallback==DEFAULT_FOLDER && !l->own)l->fallback=DEFAULT_FOLDER_OPEN;   /* SHGFI_OPENICON */
    if(flags&0x1000){                       /* SHGFI_ICONLOCATION: shell32's indices for the defaults */
      static const int shell_index[]={0,3,2,4};
      if(l->own){shell_copy(out->display,260,l->file);out->index=l->index;}
      else {shell_copy(out->display,260,"C:\\WINDOWS\\SYSTEM\\SHELL32.DLL");out->index=shell_index[l->fallback];}
    }
    if(flags&0x100){
      HICON icon=0;
      if(l->own && PrivateExtractIconsA(l->file,l->index,32,32,&icon,0,1,0)!=1)icon=0;
      out->icon=icon ? icon : CiukiShellIcon((UINT)l->fallback);
    }
    shell_free(l);
  }
  /* The returned index is meaningful only with an image-list handle. No
   * process-shared shell image list exists yet: never invent its handle. */
  if(flags&0x4000){if(out->icon)DestroyIcon(out->icon);out->icon=0;SetLastError(120);return 0;}
  return 1;
}
