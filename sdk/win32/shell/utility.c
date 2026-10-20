/* Small filesystem shell helpers. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/shell.h>
#include <w32/user32.h>
extern UINT W32API PrivateExtractIconExA(const char *,int,HICON *,HICON *,UINT);
extern UINT W32API PrivateExtractIconsA(const char *,int,int,int,HICON *,UINT *,UINT,UINT);
extern void *W32API GlobalLock(HANDLE);
extern BOOL W32API GlobalUnlock(HANDLE);
extern DWORD W32API GlobalSize(HANDLE);
extern HANDLE W32API GlobalFree(HANDLE);
struct fileinfo {HICON icon;int index;DWORD attributes;char display[260],type[80];};
UINT W32API ExtractIconExA(const char *file,int index,HICON *large,HICON *small,UINT count){
  return PrivateExtractIconExA(file,index,large,small,count);
}
UINT W32API ExtractIconExW(const WCHAR *file,int index,HICON *large,HICON *small,UINT count){char a[260];return shell_ansi(file,a,260)?ExtractIconExA(a,index,large,small,count):0;}
HICON W32API ExtractIconA(HINSTANCE instance,const char *file,UINT index){
  (void)instance;if(index==~0u)return (HICON)(uintptr_t)PrivateExtractIconsA(file,0,32,32,0,0,0,0);
  HICON icon=0;return PrivateExtractIconsA(file,(int)index,32,32,&icon,0,1,0)==1?icon:0;
}
HICON W32API ExtractIconW(HINSTANCE instance,const WCHAR *file,UINT index){char a[260];return shell_ansi(file,a,260)?ExtractIconA(instance,a,index):0;}
uintptr_t W32API SHGetFileInfoA(const char *file,DWORD attr,struct fileinfo *out,UINT size,UINT flags){
  if(!file || !out || size<sizeof(*out) || (flags&8)){SetLastError(87);return 0;}
  w32_fill(out,0,sizeof(*out));
  if(!(flags&0x10)){attr=GetFileAttributesA(file);if(attr==~0u)return 0;}
  out->attributes=attr;
  const char *base=file;for(const char *s=file;*s;s++)if(*s=='\\' || *s=='/')base=s+1;
  if(flags&0x200)if(!shell_copy(out->display,260,base))return 0;
  if(flags&0x400)shell_copy(out->type,80,attr&16?"File folder":"File");
  if(flags&0x100)out->icon=ExtractIconA(0,file,0);
  /* The returned index is meaningful only with an image-list handle. No
   * process-shared shell image list exists yet: never invent its handle. */
  if(flags&0x4000){if(out->icon)DestroyIcon(out->icon);out->icon=0;SetLastError(120);return 0;}
  return 1;
}
void W32API DragAcceptFiles(HWND window,BOOL accept){
  LONG style=GetWindowLongA(window,-20);
  SetWindowLongA(window,-20,accept?style|0x10:style&~0x10);
}
UINT W32API DragQueryFileA(HANDLE drop,UINT index,char *out,UINT cap){
  DWORD n=GlobalSize(drop);const uint8_t *p=GlobalLock(drop);UINT result=0;
  if(!p || n<20)goto done;
  DWORD at=w32_u32(p),wide=w32_u32(p+16);if(at<20 || at>=n || (wide && at%2))goto done;
  UINT count=0;
  while(at<n){
    DWORD begin=at,chars=0,step=wide?2:1;
    while(at+step<=n && (wide?w32_u16(p+at):p[at])){chars++;at+=step;}
    if(at+step>n)goto done;
    if(!chars){if(index==~0u)result=count;goto done;}
    if(count++==index){
      if(wide){
        WCHAR *temp=shell_alloc((chars+1)*2);if(!temp)goto done;
        w32_copy(temp,p+begin,chars*2);temp[chars]=0;
        int needed=WideCharToMultiByte(1252,0,temp,-1,0,0,0,0);
        result=needed>0?(UINT)needed-1:0;
        if(out && cap){char a[260];if(result>=sizeof(a) || !shell_ansi(temp,a,sizeof(a))){result=0;}else{UINT take=result<cap?result:cap-1;w32_copy(out,a,take);out[take]=0;}}
        shell_free(temp);
      }else {result=chars;if(out && cap){UINT take=chars<cap?chars:cap-1;w32_copy(out,p+begin,take);out[take]=0;}}
      goto done;
    }
    at+=step;
  }
done:if(p)GlobalUnlock(drop);return result;
}
void W32API DragFinish(HANDLE drop){GlobalFree(drop);}

struct allocator {const void *const *vt;};
static ULONG W32API ma_ref(struct allocator *a){(void)a;return 1;}
static HRESULT W32API ma_query(struct allocator *a,const SHELL_GUID *id,void **out){if(!out)return (HRESULT)0x80070057u;*out=0;if(!shell_guid(id,0) && !shell_guid(id,2))return (HRESULT)0x80004002u;*out=a;return 0;}
static void *W32API ma_alloc(struct allocator *a,DWORD n){(void)a;return shell_alloc(n);}
static void *W32API ma_realloc(struct allocator *a,void *p,DWORD n){(void)a;return p?HeapReAlloc(GetProcessHeap(),0,p,n):shell_alloc(n);}
static void W32API ma_free(struct allocator *a,void *p){(void)a;shell_free(p);}
static DWORD W32API ma_size(struct allocator *a,void *p){(void)a;return HeapSize(GetProcessHeap(),0,p);}
static int W32API ma_did(struct allocator *a,void *p){(void)a;(void)p;return -1;}
static void W32API ma_minimize(struct allocator *a){(void)a;}
static const void *const ma_vt[]={ma_query,ma_ref,ma_ref,ma_alloc,ma_realloc,ma_free,ma_size,ma_did,ma_minimize};
static struct allocator allocator={ma_vt};
HRESULT W32API SHGetMalloc(void **out){if(!out)return (HRESULT)0x80070057u;*out=&allocator;return 0;}
