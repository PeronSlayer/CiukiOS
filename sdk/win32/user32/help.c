/* SPDX-License-Identifier: LGPL-2.1-or-later
 * WinHelp and HHCTRL dispatch through the native desktop Help viewer. */
#include "unicode.h"
#include "../../../apps/desktop/help_request.h"
static HWND html_proxy;
static struct u32_window *caller(HWND h)
{
    struct u32_window *w=u32_window(h);
    if (!w) w=u32_window(GetActiveWindow());
    if (!w) for (UINT i=0;i<U32_WINDOWS;i++) if (u32_windows[i].handle && u32_windows[i].wire) { w=&u32_windows[i]; break; }
    return w ? u32_root(w) : 0;
}
static BOOL native_path(const char *file,char path[161])
{
    if (!file || !*file) return u32_fail(87);
    char source[260],full[260],utf8[778]; UINT n=0;
    while (file[n] && file[n]!='>' && !(file[n]==':' && file[n+1]==':')) {
        if (n+1>=sizeof(source)) return u32_fail(206);
        source[n]=file[n]; n++;
    }
    source[n]=0;
    typedef DWORD (W32API *full_fn)(const char *,DWORD,char *,char **);
    full_fn fullpath=(full_fn)u32_kernel("GetFullPathNameA");
    if (!fullpath) return u32_fail(120);
    DWORD length=fullpath(source,sizeof(full),full,0);
    if (!length || length>=sizeof(full)) return u32_fail(length ? 206 : GetLastError());
    typedef DWORD (W32API *attributes_fn)(const char *);
    attributes_fn attributes=(attributes_fn)u32_kernel("GetFileAttributesA");
    if (!attributes) return u32_fail(120);
    DWORD attr=attributes(full); if (attr==UINT32_MAX) return 0;
    if (attr&16) return u32_fail(5);
    u32_utf8(utf8,sizeof(utf8),full);
    if (!utf8[0] || utf8[1]!=':' || (utf8[2]!='\\' && utf8[2]!='/')) return u32_fail(123);
    unsigned drive=(unsigned char)utf8[0]; if (drive>='a' && drive<='z') drive-=32;
    if (drive<'A' || drive>'Z') return u32_fail(15);
    UINT at=0;
    if (drive!='C') { const char prefix[]="/mnt/"; for (unsigned i=0;i<5;i++) path[at++]=prefix[i]; path[at++]=(char)drive; }
    for (UINT i=2;utf8[i];i++) { if (at==160) return u32_fail(206); path[at++]=utf8[i]=='\\' ? '/' : utf8[i]; }
    path[at]=0; return 1;
}
static BOOL dispatch(HWND h,unsigned format,const char *file,UINT command,DWORD context,const char *query)
{
    char path[161]="",argument[161];
    BOOL quit=(format==1 && command==2) || (format==2 && command==18);
    if (!quit && !native_path(file,path)) return 0;
    if (!desk_help_encode(argument,format,command,context,path,query)) return u32_fail(206);
    struct u32_window *w=caller(h);
    if (!w) return quit ? 1 : u32_fail(1400);
    uint8_t payload[165]; UINT n=w32_strlen(argument); desk_put32(payload,n); w32_copy(payload+4,argument,n);
    return u32_wire(w,DESK_LAUNCH,payload,n+4,-1);
}
BOOL W32API WinHelpA(HWND h,const char *file,UINT command,uintptr_t data)
{
    U32_GUARD;
    const char *query=""; DWORD context=(DWORD)data;
    switch (command) {
    case 2: case 4: case 9: case 0xb: context=0; break;
    case 1: case 8: break;
    case 3: case 0x105: case 0x102:
        if (!data) return u32_fail(87);
        query=(const char *)data; context=0; break;
    case 0xc: case 0xd: {
        if (!data) return u32_fail(87);
        const DWORD *map=(const DWORD *)data; int control=GetDlgCtrlID(h); BOOL found=0;
        for (UINT i=0;i<256;i++,map+=2) {
            if (!map[0] && !map[1]) break;
            if (map[0]==(DWORD)control) { context=map[1]; found=1; break; }
        }
        if (!found || context==UINT32_MAX) return u32_fail(2);
        command=8; break;
    }
    default: return u32_fail(120);
    }
    return dispatch(h,1,file,command,context,query);
}
BOOL W32API WinHelpW(HWND h,const WCHAR *file,UINT command,uintptr_t data)
{
    U32_GUARD;
    char *name=u32_narrow(file),*query=0;
    if (file && !name) return 0;
    uintptr_t value=data;
    if (command==3 || command==0x105 || command==0x102) {
        query=u32_narrow((const WCHAR *)data); if (data && !query) { u32_text_free(name); return 0; }
        value=(uintptr_t)query;
    }
    BOOL ok=WinHelpA(h,name,command,value); u32_text_free(name); u32_text_free(query); return ok;
}
static LRESULT W32API proxy_proc(HWND h,UINT message,WPARAM wp,LPARAM lp)
{
    if (message==WM_CLOSE) { (void)dispatch(h,2,0,18,0,""); DestroyWindow(h); return 0; }
    if (message==WM_NCDESTROY && h==html_proxy) html_proxy=0;
    return DefWindowProcA(h,message,wp,lp);
}
/* The native viewer has no process-local USER handle. A hidden child proxy
 * supplies a real HWND for callers that close it. Window positioning and
 * embedded browser automation on that proxy are not implemented. */
HWND W32API CiukiHtmlHelpA(HWND h,const char *file,UINT command,uintptr_t data)
{
    U32_GUARD;
    const char *query=""; DWORD context=0;
    if (command==0) {
        query=(const char *)data;
        if (!query && file) for (const char *s=file;*s;s++) if (s[0]==':' && s[1]==':') { query=s+2; break; }
        if (!query) query="";
    } else if (command==2) query=data ? (const char *)data : "";
    else if (command==15) context=(DWORD)data;
    else if (command!=1 && command!=3 && command!=18) { u32_fail(120); return 0; }
    if (!dispatch(h,2,file,command,context,query)) return 0;
    if (command==18) { if (html_proxy) DestroyWindow(html_proxy); html_proxy=0; return 0; }
    if (html_proxy && IsWindow(html_proxy)) return html_proxy;
    struct u32_window *w=caller(h); if (!w) { u32_fail(1400); return 0; }
    WNDCLASSA klass={.lpfnWndProc=proxy_proc,.lpszClassName="CiukiHtmlHelpProxy"};
    if (!u32_class(klass.lpszClassName,0) && !RegisterClassA(&klass)) return 0;
    html_proxy=CreateWindowExA(0,klass.lpszClassName,"CiukiOS Help",WS_CHILD,0,0,1,1,w->handle,0,0,0);
    return html_proxy;
}
HWND W32API CiukiHtmlHelpW(HWND h,const WCHAR *file,UINT command,uintptr_t data)
{
    U32_GUARD;
    char *name=u32_narrow(file),*query=0; uintptr_t value=data;
    if (file && !name) return 0;
    if ((command==0 || command==2) && data) {
        query=u32_narrow((const WCHAR *)data); if (!query) { u32_text_free(name); return 0; } value=(uintptr_t)query;
    }
    HWND result=CiukiHtmlHelpA(h,name,command,value); u32_text_free(name); u32_text_free(query); return result;
}
