/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Win32 bridge to the desktop store. w32srv retains only the Win32 open
 * thread lock, viewer chain and registered-format atom namespace. */
#include "internal.h"
struct clip_rpc {
  struct clip_rpc *next;
  struct desk_clip_wire reply;
  DWORD serial;
  BOOL done;
  void *data;
};
static struct clip_rpc *pending;
static BOOL watching;
static DWORD render_tid;
static DWORD render_depth;
BOOL u32_clip_rendering(void) { return render_depth && render_tid==u32_tid(); }
static HWND owner_window;
BOOL u32_clip_rpc(struct desk_clip_wire *w, const void *data,
                struct desk_clip_wire *reply, void **out) {
  if (!u32_connect()) return 0;
  struct u32_surface surface = {.fd = -1};
  if (w->bytes) {
    DWORD width, height;
    desk_clip_dims(w->bytes, &width, &height);
    if (!u32_backend.surface(width, height, &surface)) return 0;
    w32_copy(surface.pixels, data, w->bytes);
  }
  w->serial = ++u32_desktop.request;
  if (!w->serial) w->serial = ++u32_desktop.request;
  struct ciuki_message message;
  if (desk_clip_message(&message, 3, 1, w, surface.fd)) {
    if (surface.fd >= 0) u32_backend.drop_surface(&surface);
    return u32_fail(87);
  }
  struct clip_rpc request = {.next = pending, .serial = w->serial};
  pending = &request;
  DWORD start = u32_ticks();
  int sent;
  for (;;) {
    sent=u32_desktop.transport.send(u32_desktop.channel,&message,DONTWAIT);
    if (sent>=0 || (sent!=-11 && sent!=-4) || (DWORD)(u32_ticks()-start)>=3000) break;
    u32_pump(); UINT saved=u32_suspend(); u32_backend.pause(1); u32_resume(saved);
  }
  if (surface.fd >= 0) u32_backend.drop_surface(&surface);
  while (sent >= 0 && !request.done && !u32_desktop.disconnected &&
         (DWORD)(u32_ticks() - start) < 3000) {
    u32_pump();
    if (!request.done) {
      UINT saved = u32_suspend(); u32_backend.pause(1); u32_resume(saved);
    }
  }
  struct clip_rpc **at = &pending;
  while (*at && *at != &request) at = &(*at)->next;
  if (*at) *at = request.next;
  if (!request.done || request.reply.result) {
    HeapFree(GetProcessHeap(),0,request.data);
    return u32_fail(!request.done ? 1460 : request.reply.result == -16 ? 5 :
                    request.reply.result == -12 ? 8 : 87);
  }
  *reply = request.reply;
  if (out) *out = request.data; else HeapFree(GetProcessHeap(),0,request.data);
  return 1;
}
static UINT to_wire(UINT f) {
  if (f < 0xc000) return f;
  char name[W32R_NAME + 1]; struct w32r_reply atom;
  if (!u32_directory(W32R_ATOM_NAME, (HWND)(uintptr_t)f, 0, 0, 0, "", 0, 0,
                      &atom, name, W32R_NAME)) return 0;
  DWORD n = atom.data_bytes;
  if (n && !name[n-1]) n--;
  if (!n || n > DESK_CLIP_NAME_MAX) return 0;
  struct desk_clip_wire w = {.op=DESK_CLIP_REGISTER, .count=n}, r;
  w32_copy(w.name,name,n);
  return u32_clip_rpc(&w,0,&r,0) ? r.format : 0;
}
static UINT from_wire(UINT f) {
  if (f<0xc000) return f;
  struct desk_clip_wire w={.op=DESK_CLIP_NAME,.format=f},r;
  if (!u32_clip_rpc(&w,0,&r,0)) return 0;
  r.name[r.count]=0; struct w32r_reply atom;
  return u32_directory_utf8(W32R_ATOM_ADD,0,0,0,0,r.name,0,0,&atom,0,0) ? atom.key : 0;
}
static BOOL query(struct desk_clip_wire *r) {
  struct desk_clip_wire w={.op=DESK_CLIP_QUERY}; return u32_clip_rpc(&w,0,r,0);
}
BOOL u32_clip_call(UINT op, HWND hwnd, UINT f, DWORD flags, const void *data,
                   DWORD bytes, struct w32r_reply *result, void *out, DWORD cap) {
  w32_fill(result,0,sizeof(*result));
  struct desk_clip_wire w={0},r;
  if (!watching) { w.op=DESK_CLIP_WATCH; w.flags=1; if (!u32_clip_rpc(&w,0,&r,0)) return 0; watching=1; w=(struct desk_clip_wire){0}; }
  if (op==W32R_CLIP_VIEWER || op==W32R_CLIP_CHAIN)
    return u32_directory(op,hwnd,u32_tid(),f,flags,"",data,bytes,result,out,cap);
  if (op==W32R_CLIP_OPEN) {
    if (!u32_directory(op,hwnd,u32_tid(),0,0,"",0,0,result,0,0)) return 0;
    w.op=DESK_CLIP_OPEN; w.token=U32_PTR(hwnd);
    if (!u32_clip_rpc(&w,0,&r,0)) { struct w32r_reply ignored; u32_directory(W32R_CLIP_CLOSE,0,u32_tid(),0,0,"",0,0,&ignored,0,0); return 0; }
    return 1;
  }
  if (op==W32R_CLIP_OWNER || op==W32R_CLIP_QUERY) {
    if (!query(&r)) return 0;
    result->key=r.token; result->disposition=0;
    if (!f) result->disposition=r.sequence;
    else { UINT wire=to_wire(f); for (UINT i=0;i<r.count;i++) if (r.formats[i]==wire) result->disposition=1; }
    return 1;
  }
  w.op=op==W32R_CLIP_EMPTY ? DESK_CLIP_EMPTY : op==W32R_CLIP_CLOSE ? DESK_CLIP_CLOSE :
       op==W32R_CLIP_SET ? DESK_CLIP_PUT : DESK_CLIP_GET;
  if (op==W32R_CLIP_SET || op==W32R_CLIP_GET) {
    w.format=to_wire(f); if (!w.format) return u32_fail(87);
    if (op==W32R_CLIP_SET) { w.bytes=bytes; w.flags=u32_clip_rendering() ? DESK_CLIP_RENDERED : !data ? DESK_CLIP_DELAYED : 0; }
  }
  if (op==W32R_CLIP_SET && !owner_window && !u32_clip_rendering()) return u32_fail(1418);
  void *copy=0;
  BOOL ok=u32_clip_rpc(&w,data,&r,&copy);
  if (op==W32R_CLIP_CLOSE) {
    struct w32r_reply ignored;
    BOOL closed=u32_directory(W32R_CLIP_CLOSE,0,u32_tid(),0,0,"",0,0,&ignored,0,0);
    if (!closed) ok=0;
  }
  if (!ok) { HeapFree(GetProcessHeap(),0,copy); return 0; }
  if (op==W32R_CLIP_EMPTY) owner_window=(HWND)(uintptr_t)r.token;
  if (r.bytes>cap && copy) { HeapFree(GetProcessHeap(),0,copy); return u32_fail(122); }
  if (copy) { w32_copy(out,copy,r.bytes); result->data_bytes=r.bytes; }
  HeapFree(GetProcessHeap(),0,copy); return 1;
}
void *u32_clip_fetch(UINT f, DWORD *bytes) {
  struct desk_clip_wire w={.op=DESK_CLIP_GET,.format=to_wire(f)},r; void *data=0;
  if (!w.format || !u32_clip_rpc(&w,0,&r,&data)) return 0;
  *bytes=r.bytes; return data;
}
UINT u32_clip_enum(UINT previous) {
  struct desk_clip_wire r; if (!query(&r)) return 0;
  UINT wire=previous ? to_wire(previous) : 0;
  for (UINT i=0;i<r.count;i++) {
    if (!previous) return from_wire(r.formats[i]);
    if (r.formats[i]==wire) { if (i+1<r.count) return from_wire(r.formats[i+1]); break; }
  }
  SetLastError(0); return 0;
}
void u32_clip_event(const struct desk_packet *p) {
  if (p->clip.kind==DESK_CLIP_REPLY) {
    for (struct clip_rpc *r=pending;r;r=r->next) if (r->serial==p->clip.serial) {
      r->reply=p->clip; r->done=1;
      if (p->clipboard_fd>=0) {
        struct ciuki_surface_info info={.size=sizeof(info)};
        if ((int)u32_backend.gate(CIUKI_SYS_SURFACE_INFO,p->clipboard_fd,U32_PTR(&info),0)<0 ||
            !desk_surface_valid(&info) || p->clip.bytes>info.allocation_bytes) r->reply.result=-5;
        else {
          DWORD address=u32_backend.gate(CIUKI_SYS_SURFACE_MAP,p->clipboard_fd,1,0);
          if ((int32_t)address<0) r->reply.result=-12;
          else {
            r->data=HeapAlloc(GetProcessHeap(),8,p->clip.bytes);
            if (!r->data) r->reply.result=-12; else w32_copy(r->data,(void *)(uintptr_t)address,p->clip.bytes);
            u32_backend.gate(CIUKI_SYS_MUNMAP,address,info.allocation_bytes,0);
          }
        }
      }
      break;
    }
  } else if (p->clip.kind==DESK_CLIP_DISPLAY_EVENT) {
    u32_clip_display_event(p);
  } else if (p->clip.kind==DESK_CLIP_RENDER) {
    UINT format=from_wire(p->clip.format);
    struct u32_window *owner=u32_window((HWND)(uintptr_t)p->clip.token);
    if (owner) {
      DWORD previous=render_tid; render_tid=owner->tid; render_depth++;
      SendMessageA(owner->handle,WM_RENDERFORMAT,format,0);
      render_depth--; render_tid=previous;
    }
  } else if (p->clip.kind==DESK_CLIP_DESTROY) {
    if (owner_window && U32_PTR(owner_window)==p->clip.token) {
      SendMessageA(owner_window,WM_DESTROYCLIPBOARD,0,0);
      owner_window=0; u32_clipboard_owned_clear();
    }
    u32_clipboard_changed(p->clip.sequence);
  } else if (p->clip.kind==DESK_CLIP_CHANGED) {
    u32_clipboard_changed(p->clip.sequence);
    struct w32r_reply r;
    if (u32_directory(W32R_CLIP_OWNER,0,0,0,0,"",0,0,&r,0,0) && r.type && u32_window((HWND)(uintptr_t)r.type))
      PostMessageA((HWND)(uintptr_t)r.type,WM_DRAWCLIPBOARD,0,0);
  }
  if (p->clipboard_fd>=0) u32_backend.gate(CIUKI_SYS_CLOSE,p->clipboard_fd,0,0);
}
void u32_clip_render_all(HWND hwnd, BOOL callbacks) {
  if (hwnd && hwnd==owner_window) {
    if (callbacks && u32_clip_has_delayed()) SendMessageA(hwnd,WM_RENDERALLFORMATS,0,0);
    struct desk_clip_wire w={.op=DESK_CLIP_RELEASE,.token=U32_PTR(hwnd)},r;
    (void)u32_clip_rpc(&w,0,&r,0); owner_window=0; u32_clipboard_owned_clear();
  }
}

int W32API CountClipboardFormats(void) {
  U32_GUARD; struct desk_clip_wire r; return query(&r) ? (int)r.count : 0;
}
int W32API GetPriorityClipboardFormat(const UINT *formats, int count) {
  U32_GUARD;
  if (!formats || count<0) { u32_fail(87); return -1; }
  if (!CountClipboardFormats()) return 0;
  for (int i=0;i<count;i++) if (IsClipboardFormatAvailable(formats[i])) return formats[i];
  return -1;
}

void u32_clip_abandon(DWORD tid) {
  struct desk_clip_wire w={.op=DESK_CLIP_CLOSE},r;
  (void)u32_clip_rpc(&w,0,&r,0);
  struct w32r_reply ignored;
  u32_directory(W32R_CLIP_CLOSE,0,tid,0,0,"",0,0,&ignored,0,0);
}
