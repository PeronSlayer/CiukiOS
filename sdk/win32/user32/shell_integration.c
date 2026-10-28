/* Classic shell boundary: no application pointer crosses the desktop wire.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/ntdll.h>
#include <w32/controls.h>
#ifdef W32_HOST_MODEL
__attribute__((weak)) _Thread_local struct w32_teb *w32_model_teb;
#endif
static BOOL startup_used;
void u32_startup_reset(void) { startup_used=0; }
/* STARTUPINFO's wShowWindow (STARTF_USESHOWWINDOW) replaces the show
 * command of the program's main window once. SW_SHOWDEFAULT always asks for
 * it (Microsoft, STARTUPINFO: "In subsequent calls to ShowWindow, the
 * wShowWindow member is used if the nCmdShow parameter of ShowWindow is set
 * to SW_SHOWDEFAULT"). Any other first show takes it only for a window the
 * window manager can take for the main window: top-level, unowned, with a
 * caption, not system-modal, shown with SW_SHOW or SW_SHOWNORMAL (Raymond
 * Chen, "When does STARTF_USESHOWWINDOW override the parameter passed to
 * ShowWindow()?", The Old New Thing 2010-03-01, rules zero to two; ReactOS
 * win32ss/user/ntuser/winpos.c co_WinPosShowWindow). Until such a window
 * takes it, the override stays unused. fx-hlis5: every top-level window
 * took it before; Half-Life's AUTORUN.EXE starts the setup with SW_HIDE and
 * the IS5 bootstrap's error box (a system-modal message box) was hidden. */
int u32_startup_show(const struct u32_window *w, int command) {
  struct w32_teb *teb=NtCurrentTeb();
  struct w32_process_parameters *p=teb && teb->ProcessEnvironmentBlock ? teb->ProcessEnvironmentBlock->ProcessParameters : 0;
  BOOL supplied=p && (p->WindowFlags&1u);
  if (command==10) { startup_used=1; return supplied ? (int)p->ShowWindowFlags : 1; }
  if (!supplied || startup_used || !w || w->parent || (w->style&WS_CHILD) || (w->style&WS_CAPTION)!=WS_CAPTION ||
      w->owner || (w->dialog && (w->style&DS_SYSMODAL)) || (command!=1 && command!=5))
    return command;
  startup_used=1;
  return (int)p->ShowWindowFlags;
}
extern HANDLE W32API GlobalAlloc(DWORD,DWORD);
extern HANDLE W32API GlobalFree(HANDLE);
extern void *W32API GlobalLock(HANDLE);
extern BOOL W32API GlobalUnlock(HANDLE);

/* Copy counted input first. cbSize is the PE32 SDK layout, including under
 * the host model. Reject unterminated fields instead of reading past them. */
static int notify_text(char *out,UINT cap,const uint8_t *p,UINT chars,BOOL wide) {
  WCHAR text[256]; UINT n=0;
  while (n<chars && (wide?w32_u16(p+n*2):p[n])) n++;
  if (n==chars) return -1;
  if (wide) {
    for (UINT k=0;k<n;k++) text[k]=w32_u16(p+k*2);
  } else for (UINT k=0;k<n;k++) text[k]=(WCHAR)u32_scalar(p[k]);
  text[n]=0;
  /* Convert one scalar at a time so truncation never splits UTF-8. */
  UINT at=0;
  for (UINT k=0;k<n;k++) {
    uint32_t c=text[k];
    if (c>=0xd800 && c<=0xdbff) {
      if (++k==n || text[k]<0xdc00 || text[k]>0xdfff) return -1;
      c=0x10000+((c-0xd800)<<10)+text[k]-0xdc00;
    } else if (c>=0xdc00 && c<=0xdfff) return -1;
    if (c<32 || c==127 || (c>=0x80 && c<0xa0)) c=' ';
    unsigned count=c<128?1:c<2048?2:c<65536?3:4;
    if (at+count>=cap) break;
    if (count==1) out[at++]=(char)c;
    else for (unsigned j=0;j<count;j++) {
      unsigned shift=(count-1-j)*6;
      out[at++]=(char)(j?0x80|((c>>shift)&63):(count==2?0xc0:count==3?0xe0:0xf0)|(c>>shift));
    }
  }
  out[at]=0; return (int)at;
}
BOOL W32API CiukiShellNotify(DWORD action,void *data,BOOL wide) {
  U32_GUARD;
  if (!data || action>2) return u32_fail(87);
  const uint8_t *p=data; DWORD size=w32_u32(p),v1=wide?152:88,extended=wide?936:488;
  if (size!=v1 && size!=extended && size!=extended+16 && size!=extended+20) return u32_fail(87);
  struct u32_window *w=u32_window((HWND)(uintptr_t)w32_u32(p+4));
  if (!w || w->parent || !w->wire) return u32_fail(1400);
  DWORD id=w32_u32(p+8), flags=action==2?0:w32_u32(p+12);
  if (flags&~23u || ((flags&16u) && size<extended)) return u32_fail(87);
  uint8_t b[240]={0},balloon[240]={0};
  int tip=0,title=0,text=0;
  if (flags&4u) tip=notify_text((char *)b+20,128,p+24,size==v1?64:128,wide);
  if (flags&16u) {
    UINT info=wide?288:160,head=wide?804:420;
    title=notify_text((char *)balloon+16,64,p+head,64,wide);
    if (title>=0) text=notify_text((char *)balloon+16+title,160,p+info,256,wide);
    DWORD timeout=w32_u32(p+(wide?800:416));
    desk_put32(balloon,id); desk_put32(balloon+4,(DWORD)title); desk_put32(balloon+8,(DWORD)text);
    DWORD kind=w32_u32(p+(wide?932:484))&3u;
    desk_put32(balloon+12,(timeout<10000?10000:timeout>30000?30000:timeout)|(kind<<16));
  }
  if (tip<0 || title<0 || text<0) return u32_fail(87);
  struct u32_surface surface={.fd=-1};
  if ((flags&2u) && !u32_tray_pixels((HICON)(uintptr_t)w32_u32(p+20),&surface)) return 0;
  desk_put32(b,action); desk_put32(b+4,id); desk_put32(b+8,flags&7u);
  desk_put32(b+12,flags&1u?w32_u32(p+16):0); desk_put32(b+16,(DWORD)tip);
  BOOL ok=u32_wire(w,DESK_V3_TRAY_ICON,b,20+(UINT)tip,surface.fd);
  if (surface.fd>=0) u32_backend.drop_surface(&surface);
  if (ok && (flags&16u)) ok=u32_wire(w,DESK_V3_TRAY_BALLOON,balloon,16+(UINT)title+(UINT)text,-1);
  return ok;
}
/* fx-q3vise: ShellExecute(Ex) of a folder (sdk/win32/shell/execute.c).
 * Windows 98 shows the folder in an Explorer window; CiukiOS shows it in
 * Files. The shell passes its folder window (a top-level window of this
 * thread, which carries the Win32 identity on the desktop wire) and the
 * folder as an absolute POSIX path; the desktop starts Files there
 * (DESK_V3_LAUNCH "files", which it accepts from a Win32 window for an
 * absolute folder path only: apps/desktop/client.c). The ANSI path is sent
 * as UTF-8 and is never truncated. */
BOOL W32API CiukiShellOpenFolder(HWND h,const char *path) {
  U32_GUARD;
  struct u32_window *w=u32_window(h);
  if (!w || w->parent || !w->wire || w->tid!=u32_tid()) return u32_fail(1400);
  if (!path || path[0]!='/' || !path[1]) return u32_fail(87);
  uint8_t b[13+DESK_ARG_MAX+4];
  /* u32_utf8 stops before a scalar that would not fit: a result this close
   * to the capacity may have lost the end of the path. */
  UINT n=u32_utf8((char *)b+13,DESK_ARG_MAX+4,path);
  if (n>DESK_ARG_MAX) return u32_fail(206);
  desk_put32(b,5); w32_copy(b+4,"files",5); desk_put32(b+9,n);
  return u32_wire(w,DESK_V3_LAUNCH,b,13+n,-1);
}
BOOL W32API FlashWindow(HWND h,BOOL invert) {
  U32_GUARD;
  struct u32_window *w=u32_window(h);
  if (!w || w->parent) return u32_fail(1400);
  BOOL previous=(u32_foreground==h)!=!!w->flash;
  uint8_t b[4]; desk_put32(b,!!invert);
  if (!u32_wire(w,DESK_V3_FLASH,b,4,-1)) return 0;
  w->flash=invert?!w->flash:0;
  return previous;
}

/* Own the read-only surface snapshot attached to this DROP_FILES event.
 * HDROP uses wide drive paths regardless of the target procedure's mode. */
void u32_drop_files(struct u32_window *w,int x,int y,int fd) {
  if (fd<0) return;
  struct ciuki_surface_info info={0};
  if (!(w->exstyle&0x10u) ||
      (int32_t)u32_backend.gate(CIUKI_SYS_SURFACE_INFO,(DWORD)fd,U32_PTR(&info),0)<0 ||
      info.size!=sizeof(info) || info.width!=1024 || !info.height || info.height>1027 ||
      info.stride!=4096 || info.format!=CIUKI_SURFACE_XRGB8888 || info.allocation_bytes!=info.height*4096u) {
    u32_backend.gate(CIUKI_SYS_CLOSE,(DWORD)fd,0,0); return;
  }
  DWORD address=u32_backend.gate(CIUKI_SYS_SURFACE_MAP,(DWORD)fd,1,0);
  if (!address || address>=(DWORD)-4095) { u32_backend.gate(CIUKI_SYS_CLOSE,(DWORD)fd,0,0); return; }
  const uint8_t *snapshot=(const uint8_t *)(uintptr_t)address;
  DWORD size=w32_u32(snapshot);
  uint8_t *raw=size>=5 && size<=4096u*1026u && size<=info.allocation_bytes-4 ? u32_alloc(size+1) : 0;
  if (raw) w32_copy(raw,snapshot+4,size);
  u32_backend.gate(CIUKI_SYS_MUNMAP,address,info.allocation_bytes,0);
  u32_backend.gate(CIUKI_SYS_CLOSE,(DWORD)fd,0,0);
  if (!raw) return;
  BOOL valid=1;
  raw[size]=0;
  if (!valid || raw[0]!='X' || raw[1]!='\n' || raw[size-1]!='\n') { u32_free(raw); return; }
  /* Each UTF-8 input byte needs at most one UTF-16 unit plus drive prefix. */
  HANDLE drop=GlobalAlloc(0x42,20+(size+4096u*3u+1)*2);
  uint8_t *out=drop?GlobalLock(drop):0;
  if (!out) { if(drop)GlobalFree(drop); u32_free(raw); return; }
  w32_fill(out,0,20); w32_put32(out,20); w32_put32(out+4,(DWORD)x); w32_put32(out+8,(DWORD)y); w32_put32(out+16,1);
  DWORD at=20,start=2,count=0;
  for (DWORD k=2;k<size && valid;k++) {
    if (!raw[k] || raw[k]=='\r') { valid=0; break; }
    if (raw[k]!='\n') continue;
    if (k-start<2 || k-start>1024 || (raw[start]!='D' && raw[start]!='F') || raw[start+1]!='/' || ++count>4096) { valid=0; break; }
    const uint8_t *name=raw+start+1; UINT len=k-start-1;
    if (!desk_utf8_valid(name,len)) { valid=0; break; }
    UINT component=1;
    for (UINT j=1;len>1 && j<=len;j++) {
      if (j<len && name[j]!='/') {
        if (name[j]=='\\' || name[j]==':') { valid=0; break; }
        continue;
      }
      UINT n=j-component;
      if (!n || (name[component]=='.' && (n==1 || (n==2 && name[component+1]=='.')))) { valid=0; break; }
      component=j+1;
    }
    if (!valid) break;
    char drive='C'; UINT skip=1;
    if (len>=6 && name[1]=='m' && name[2]=='n' && name[3]=='t' && name[4]=='/' && name[5]>='d' && name[5]<='z' && (len==6 || name[6]=='/')) { drive=(char)(name[5]-'a'+'A'); skip=len==6?6:7; }
    w32_put16(out+at,(uint16_t)drive); at+=2; w32_put16(out+at,':'); at+=2; w32_put16(out+at,'\\'); at+=2;
    for (UINT j=skip;j<len;) {
      uint32_t c=name[j++]; unsigned extra=c<128?0:c<224?1:c<240?2:3;
      if (extra) { c&=(1u<<(6-extra))-1; while(extra--)c=(c<<6)|(name[j++]&63); }
      if (c=='/') c='\\';
      if (c>65535) { w32_put16(out+at,(uint16_t)(0xd800+((c-0x10000)>>10))); at+=2; c=0xdc00+((c-0x10000)&1023); }
      w32_put16(out+at,(uint16_t)c); at+=2;
    }
    w32_put16(out+at,0); at+=2; start=k+1;
  }
  w32_put16(out+at,0); GlobalUnlock(drop); u32_free(raw);
  if (!valid || !count || start!=size || !u32_queue(w->tid,w->handle,0x233,(WPARAM)drop,0,QS_POSTMESSAGE,u32_ticks(),u32_pointer)) GlobalFree(drop);
}
