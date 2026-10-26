/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/controls.h>
static BOOL connected;
static DWORD generation;
struct request {
  struct request *next;
  DWORD token, window, generation, tid;
  BOOL done;
  int error;
};
static struct request *requests;
void u32_desktop_cleanup(DWORD tid) {
  if (!tid) u32_startup_reset();
  struct request **at = &requests;
  while (*at) {
    if (!tid || (*at)->tid == tid) *at = (*at)->next;
    else at = &(*at)->next;
  }
  if (!tid && connected) {
    u32_desktop.transport.close(u32_desktop.channel);
    connected = 0;
    w32_fill(&u32_desktop, 0, sizeof(u32_desktop));
  }
}
static int tx(int fd, const struct ciuki_message *m, uint32_t flags) {
  return (int)u32_backend.gate(CIUKI_SYS_CHANNEL_SEND, fd, U32_PTR(m), flags);
}
static int rx(int fd, struct ciuki_message *m, uint32_t flags) {
  return (int)u32_backend.gate(CIUKI_SYS_CHANNEL_RECV, fd, U32_PTR(m), flags);
}
static int info(int fd, struct ciuki_surface_info *i) {
  i->size = sizeof(*i);
  return (int)u32_backend.gate(CIUKI_SYS_SURFACE_INFO, fd, U32_PTR(i), 0);
}
static int close_fd(int fd) {
  return (int)u32_backend.gate(CIUKI_SYS_CLOSE, fd, 0, 0);
}
BOOL u32_connect(void) {
  if (connected)
    return !u32_desktop.disconnected;
  struct ciuki_w32_service_args a = {.header = K32_HEADER(W32_SERVICE, a),
                                     .action = W32_SVC_CONNECT,
                                     .service = W32_SERVICE_DESKTOP};
  if (u32_backend.call(&a, sizeof(a)) || a.error || !a.result)
    return u32_fail(a.error ? a.error : 1062);
  struct du3_transport t = {tx, rx, info, close_fd};
  if (du3_connect(&u32_desktop, (int)a.endpoint, &t) < 0)
    return u32_fail(1062);
  connected = 1;
  return 1;
}
BOOL u32_wire(struct u32_window *w, UINT op, const void *data, UINT n, int fd) {
  /* q3exec3: every per-window message names a live wire id and generation.
   * Once DESK_CLOSE is sent the desktop no longer has the window, and the
   * DESK_CLOSED answer clears the id: nothing after the close may name it
   * (a packet with window 0 ended the program's whole connection). */
  if (!w || (!w->wire && op!=DESK_INPUT_SETTINGS) ||
      (w->wire && !w->generation) || (w->closing && op != DESK_CLOSE) ||
      !u32_connect())
    return 0;
  DWORD token =
      du3_request(&u32_desktop, op, w->wire, w->generation, data, n, fd);
  if (!token)
    return u32_fail(1062);
  if (op == DESK_DAMAGE || op == DESK_V3_SOUND)
    return 1;
  struct request request = {requests, token, w->wire, w->generation, u32_tid(), 0, 0};
  requests = &request;
  DWORD start = u32_ticks();
  while (!request.done && !u32_desktop.disconnected &&
         (DWORD)(u32_ticks() - start) < 2000) {
    u32_pump();
    if (!request.done) {
      UINT saved = u32_suspend();
      u32_backend.pause(1);
      u32_resume(saved);
    }
  }
  struct request **at = &requests;
  while (*at && *at != &request)
    at = &(*at)->next;
  if (*at)
    *at = request.next;
  if (!request.done)
    return u32_fail(u32_desktop.disconnected ? 1062 : 1460);
  if (request.error)
    return u32_fail(request.error == -1 || request.error == -13 ? 5
                    : request.error == -12                      ? 8
                    : request.error == -16                      ? 170
                    : request.error == -2                       ? 1400
                    : request.error == -5                      ? 31
                                                                : 87);
  return 1;
}
/* q3exec3: an owner the desktop no longer has (closing, or no wire id)
 * is not named; the window is created unowned there. */
struct u32_window *u32_wire_owner(struct u32_window *w) {
  struct u32_window *owner = u32_window(w->owner);
  if (owner)
    owner = u32_root(owner);
  return owner && owner->wire && owner->generation && !owner->closing ? owner : 0;
}
UINT u32_style(struct u32_window *w) {
  return (w->exstyle & WS_EX_TOOLWINDOW ? DESK_V3_TOOL
          : (w->style & WS_POPUP) && !(w->style & WS_CAPTION)
              ? DESK_V3_POPUP
              : DESK_V3_OVERLAPPED) |
         (w->exstyle & WS_EX_TOPMOST ? DESK_V3_TOPMOST : 0) |
         (w->exstyle & WS_EX_CONTEXTHELP ? DESK_V3_CONTEXTHELP : 0) |
         /* a modal dialog's prompt, only with an owner the desktop knows (the protocol requires one) */
         (u32_dialog_modal(w) && u32_wire_owner(w) ? DESK_V3_MODAL : 0);
}
/* q3exec4: CREATE and PLACE name the desktop frame's origin (apps/desktop
 * desk_frame: the frame at x,y, its content at x + DESK_FRAME_BORDER,
 * y + DESK_FRAME_TITLE), while DESK_CONFIGURE answers with the content
 * origin (desk_content). Sending the content origin moved a captioned
 * window by its frame (3, 24) on every placement round trip: the Quake III
 * installer's pages, which reopen where the last one was, crept down the
 * screen until the Finished page's buttons were under the taskbar. */
static POINT u32_frame_origin(struct u32_window *w) {
  POINT p = u32_surface_origin(w);
  int bx, by;
  u32_frame(w->style, w->exstyle, &bx, &by);
  p.x -= bx;
  p.y -= by;
  return p;
}
BOOL u32_create_wire(struct u32_window *w) {
  if (!u32_connect())
    return 0;
  uint32_t used = 0;
  for (UINT i = 0; i < U32_WINDOWS; i++)
    if (u32_windows[i].handle && u32_windows[i].wire)
      used |= 1u << (u32_windows[i].wire - 1);
  for (UINT i = 1; i <= 16; i++)
    if (!(used & (1u << (i - 1)))) {
      w->wire = i;
      break;
    }
  if (!w->wire)
    return u32_fail(8);
  w->generation = ++generation;
  if (!generation)
    return u32_fail(8);
  /* The surface holds the client area and the personality-owned bands. */
  UINT width, height;
  u32_surface_size(w, &width, &height);
  if (!u32_backend.surface(width, height, &w->surface)) {
    w->wire = 0;
    return 0;
  }
  uint8_t data[100];
  w32_fill(data, 0, sizeof(data));
  desk_put32(data, u32_style(w));
  struct u32_window *owner = u32_wire_owner(w);
  if (owner) {
    desk_put32(data + 4, owner->wire);
    desk_put32(data + 8, owner->generation);
  }
  POINT p = u32_frame_origin(w);
  desk_put32(data + 12, p.x);
  desk_put32(data + 16, p.y);
  desk_put32(data + 20, width);
  desk_put32(data + 24, height);
  desk_put32(data + 28, 1);
  UINT n = u32_utf8((char *)data + 36, 64, w->title);
  desk_put32(data + 32, n);
  if (!u32_wire(w, DESK_CREATE_WINDOW, data, 36 + n, w->surface.fd)) {
    u32_backend.drop_surface(&w->surface);
    w->wire = 0;
    return 0;
  }
  uint8_t identity[4];desk_put32(identity,U32_PTR(w->handle));
  if(!u32_wire(w,DESK_V3_WIN32_HWND,identity,4,-1))return 0;
  if (w->exstyle&0x10u) {
    desk_put32(identity,1);
    if (!u32_wire(w,DESK_ACCEPT_FILES,identity,4,-1)) return 0;
  }
  if (!(w->style & WS_VISIBLE) || IsIconic(w->handle))
    return u32_place(w);
  return 1;
}
BOOL u32_resize(struct u32_window *w) {
  if (!w || w->destroying || w->creating || w->dcs || !w->serial)
    return 1;
  UINT width, height;
  u32_surface_size(w, &width, &height);
  if (w->surface.width == width && w->surface.height == height)
    return 1;
  struct u32_surface replacement = {.fd = -1};
  if (!u32_backend.surface(width, height, &replacement))
    return 0;
  uint8_t data[12];
  desk_put32(data, width);
  desk_put32(data + 4, height);
  desk_put32(data + 8, w->serial);
  if (!u32_wire(w, DESK_V3_RESIZE, data, 12, replacement.fd)) {
    u32_backend.drop_surface(&replacement);
    return 0;
  }
  u32_backend.drop_surface(&w->surface);
  w->surface = replacement;
  w->damage.n = 0;
  InvalidateRect(w->handle, 0, 1);
  return 1;
}
/* WM_GETMINMAXINFO: the window procedure may set the smallest and largest
 * track size (outer sizes, zero keeps the default); the desktop holds the
 * window to them as client-area sizes (DESK_V3_LIMITS) and maximises within
 * the largest. Sent only when they changed. */
static BOOL u32_limits(struct u32_window *w) {
  if (w->parent || !w->wire || !w->proc)
    return 1;
  MINMAXINFO mmi;
  w32_fill(&mmi, 0, sizeof(mmi));
  u32_invoke(w->handle, WM_GETMINMAXINFO, 0, (LPARAM)&mmi);
  int bx, by;
  u32_frame(w->style, w->exstyle, &bx, &by);
  uint32_t limits[4] = {0, 0, 0, 0};
  const POINT *track[2] = {&mmi.ptMinTrackSize, &mmi.ptMaxTrackSize};
  for (UINT k = 0; k < 2; k++) {
    if (track[k]->x > 0) {
      int c = track[k]->x - 2 * bx;
      limits[k * 2] = (uint32_t)(c < 1 ? 1 : c > 2048 ? 2048 : c);
    }
    if (track[k]->y > 0) {
      int c = track[k]->y - by - bx;
      limits[k * 2 + 1] = (uint32_t)(c < 1 ? 1 : c > 2048 ? 2048 : c);
    }
  }
  if (limits[2] && limits[0] > limits[2])
    limits[0] = limits[2];
  if (limits[3] && limits[1] > limits[3])
    limits[1] = limits[3];
  BOOL same = 1;
  for (UINT k = 0; k < 4; k++)
    same &= limits[k] == w->limits[k];
  if (same)
    return 1;
  uint8_t b[16];
  for (UINT k = 0; k < 4; k++)
    desk_put32(b + k * 4, limits[k]);
  if (!u32_wire(w, DESK_V3_LIMITS, b, sizeof(b), -1))
    return 0;
  w32_copy(w->limits, limits, sizeof(limits));
  return 1;
}
BOOL u32_place(struct u32_window *w) {
  if (!w->wire)
    return 1;
  if (!u32_limits(w))
    return 0;
  UINT width, height;
  u32_surface_size(w, &width, &height);
  POINT p = u32_frame_origin(w);
  uint8_t b[32];
  desk_put32(b, p.x);
  desk_put32(b + 4, p.y);
  desk_put32(b + 8, width);
  desk_put32(b + 12, height);
  desk_put32(b + 16, 1);
  desk_put32(b + 20, !(w->style & WS_VISIBLE) ? 2
                     : IsIconic(w->handle)    ? 1
                     : IsZoomed(w->handle)    ? 3
                                              : 0);
  desk_put32(b + 24, u32_style(w));
  return u32_wire(w, DESK_V3_PLACE, b, 28, -1);
}
BOOL u32_order_wire(struct u32_window *w, HWND after, HWND owner, UINT flags) {
  if (!w->wire) return 1;
  /* q3exec3: a window the desktop no longer has is never named: ordering
   * after it is dropped, and it is no owner there. */
  struct u32_window *named = U32_ORDER_SENTINEL(after) ? 0 : u32_window(after);
  if ((flags & DESK_V3_ORDER_Z) && named && (!named->wire || named->closing))
    flags &= ~DESK_V3_ORDER_Z;
  if (!flags) return 1;
  uint8_t b[20] = {0};
  desk_put32(b, flags);
  if (flags & DESK_V3_ORDER_Z) {
    struct u32_window *a = named;
    DWORD target = after == HWND_BOTTOM ? DESK_V3_ORDER_BOTTOM
                   : after == HWND_TOPMOST ? DESK_V3_ORDER_TOPMOST
                   : after == HWND_NOTOPMOST ? DESK_V3_ORDER_NOTOPMOST
                   : a ? a->wire : DESK_V3_ORDER_TOP;
    desk_put32(b + 4, target);
    desk_put32(b + 8, a ? a->generation : 0);
  }
  if (flags & DESK_V3_ORDER_OWNER) {
    struct u32_window *o = u32_window(owner);
    if (o && (!o->wire || !o->generation || o->closing))
      o = 0;
    desk_put32(b + 12, o ? o->wire : 0);
    desk_put32(b + 16, o ? o->generation : 0);
  }
  return u32_wire(w, DESK_V3_WINDOW_ORDER, b, sizeof(b), -1);
}
void u32_event(const struct desk_packet *p) {
  if(p->opcode==DESK_INPUT_STATE) {
    BOOL changed=0;
    if(p->index<=DESK_INPUT_VALUES)for(UINT k=1;k<=DESK_INPUT_VALUES;k++)
      if((!p->index || k==p->index) && u32_input_value(k)!=p->input_settings[k-1])changed=1;
    u32_input_snapshot(p->input_settings);
    if(changed && (p->flags&2)) {
      static const UINT actions[]={0,0,23,11,0,33,93,105,87,113,32,91};
      for(UINT i=0;i<U32_WINDOWS;i++)if(u32_windows[i].handle && !u32_windows[i].parent)
        SendNotifyMessageA(u32_windows[i].handle,0x1a,actions[p->index],0);
    }
    return;
  }
  if (p->opcode==DESK_V3_CLIPBOARD_EVENT) { u32_clip_event(p); return; }
  if (p->opcode == DESK_V3_RESULT) {
    for (struct request *r = requests; r; r = r->next)
      if (r->token == p->request && r->window == p->window_id &&
          r->generation == p->generation) {
        r->done = 1;
        r->error = p->result;
        break;
      }
    return;
  }
  struct u32_window *w = 0;
  for (UINT i = 0; i < U32_WINDOWS; i++)
    if (u32_windows[i].handle && u32_windows[i].wire == p->window_id &&
        u32_windows[i].generation == p->generation) {
      w = &u32_windows[i];
      break;
    }
  if (!w) {
    u32_event_release(p);
    return;
  }
  /* q3exec3: DestroyWindow flushes the window's queue; while it runs, only
   * the desktop's DESK_CLOSED answer still concerns the window (a PING
   * taken here was answered after DESK_CLOSED had cleared the wire id).
   * A PING is still taken until DESK_CLOSE is sent: the destruction may
   * fail (a dependent on another thread cannot be reached) and the window
   * then lives on, and the desktop pings again only after an answer. */
  if (w->destroying && p->opcode != DESK_CLOSED &&
      (p->opcode != DESK_PING || w->closing)) {
    u32_event_release(p);
    return;
  }
  if (w->tid != u32_tid() || w->creating) {
    if (w->tid == u32_tid()) u32_self_deferred = 1;   /* this thread's own event: not ready yet (creating) */
    struct u32_thread *t = u32_thread_for(w->tid);
    if (!t || t->event_count >= 256) {
      u32_event_release(p);
      u32_fail(1816);
      return;
    }
    struct u32_pending_event *e = u32_alloc(sizeof(*e));
    if (!e) {
      u32_event_release(p);
      return;
    }
    e->packet = *p;
    if (t->events_tail)
      t->events_tail->next = e;
    else
      t->events = e;
    t->events_tail = e;
    t->event_count++;
    struct ciuki_w32_message_args a = {.action = W32_MSG_MARK,
                                       .tid = t->id,
                                       .bits = QS_POSTMESSAGE,
                                       .message = {.wparam = QS_POSTMESSAGE}};
    u32_message(&a);
    return;
  }
  HWND h = w->handle;
  switch (p->opcode) {
  case DESK_V3_TRAY_EVENT:
    u32_pointer=(POINT){p->x,p->y};
    u32_queue(w->tid,h,p->serial,p->index,p->flags,QS_POSTMESSAGE,u32_ticks(),u32_pointer);
    break;
  case DESK_DROP_FILES:
    u32_drop_files(w,p->x,p->y,p->attachment_fd);
    break;
  case DESK_PING:
    w->ping = p->serial;
    break;
  case DESK_CLOSE_REQUEST: {
    /* T23 c42f556e: the desktop's title button, Alt+F4 and the task
     * button's Close are Windows' close button: WM_SYSCOMMAND SC_CLOSE,
     * which DefWindowProc turns into WM_CLOSE. A program that grays Close in
     * its system menu, has CS_NOCLOSE or ignores SC_CLOSE (the Quake III
     * installer's background window does both) is not closed, as on Windows
     * 98; the old direct WM_CLOSE destroyed that window and the installer
     * went on copying files with no window. A grayed or CS_NOCLOSE close
     * declines at once; End Task stays the way to end such a program. */
    UINT state = w->system_menu ? GetMenuState(w->system_menu, SC_CLOSE, MF_BYCOMMAND) : (UINT)-1;
    if ((w->cls && (w->cls->c.style & 0x200u)) || (state != (UINT)-1 && (state & (MF_GRAYED | MF_DISABLED)))) {
      static const uint8_t declined[4];
      u32_wire(w, DESK_CLOSE_ACK, declined, sizeof(declined), -1);
      break;
    }
    w->desktop_close = 1;
    u32_queue(w->tid, h, WM_SYSCOMMAND, SC_CLOSE, 0, QS_POSTMESSAGE, u32_ticks(),
              u32_pointer);
    break;
  }
  case DESK_V3_HELP:
    /* G15: the "?" caption button: DefWindowProc turns SC_CONTEXTHELP into What's This? */
    u32_queue(w->tid, h, WM_SYSCOMMAND, 0xf180u /* SC_CONTEXTHELP */, 0, QS_POSTMESSAGE, u32_ticks(),
              u32_pointer);
    break;
  case DESK_V3_ACTIVATE:
    /* Owner thread only (foreign events were queued above): a deactivation
     * clears the cached lease before any WndProc observes WM_ACTIVATEAPP. */
    u32_display_event(w, p);
    u32_activate(w, p->focused, p->flags);
    break;
  case DESK_V3_LEASE_EVENT:
    u32_display_event(w, p);
    break;
  case DESK_V3_POINTER_STATE:
    if (!u32_pointer_changing && !(p->flags & DESK_V3_CAPTURE) && u32_capture &&
        u32_root(u32_window(u32_capture)) == w) {
      HWND old = u32_capture;
      u32_capture = 0;
      SendMessageA(old, WM_CAPTURECHANGED, 0, 0);
    }
    break;
  case DESK_CONFIGURE: {
    BOOL minimized = !!(w->style & WS_MINIMIZE),
         shown = !!(w->style & WS_VISIBLE);
    RECT old = w->rect;
    int bx, by;
    u32_frame(w->style, w->exstyle, &bx, &by);
    w->rect = (RECT){p->x - bx, p->y - by, p->x + (LONG)p->width + bx,
                     p->y + (LONG)p->height + bx};
    BOOL zoomed = !!(w->style & WS_MAXIMIZE);
    /* 3: maximised by the desktop (title button, double click). Minimising or
     * hiding keeps the maximised state, only the normal state clears it. */
    if (p->flags == 1)
      w->style |= WS_MINIMIZE;
    else
      w->style &= ~WS_MINIMIZE;
    if (p->flags == 3)
      w->style |= WS_MAXIMIZE;
    else if (p->flags == 0)
      w->style &= ~WS_MAXIMIZE;
    if (p->flags == 2)
      w->style &= ~WS_VISIBLE;
    else
      w->style |= WS_VISIBLE;
    w->serial = p->serial;
    u32_resize(w);
    u32_sync_window(w);
    if(u32_input_exclusive==h)u32_input_focus();
    if (old.left != w->rect.left || old.top != w->rect.top)
      SendMessageA(h, WM_MOVE, 0,
                   (uint16_t)w->rect.left |
                       ((uint32_t)(uint16_t)w->rect.top << 16));
    if (minimized != IsIconic(h) || zoomed != IsZoomed(h) ||
        old.right - old.left != w->rect.right - w->rect.left ||
        old.bottom - old.top != w->rect.bottom - w->rect.top)
      SendMessageA(h, WM_SIZE, IsIconic(h) ? SIZE_MINIMIZED : IsZoomed(h) ? SIZE_MAXIMIZED : SIZE_RESTORED,
                   IsIconic(h) ? 0 : ((uint16_t)p->width | (p->height << 16)));
    if (shown != IsWindowVisible(h))
      SendMessageA(h, WM_SHOWWINDOW, IsWindowVisible(h), 0);
    if (!IsIconic(h))
      InvalidateRect(h, 0, 1);
    break;
  }
  case DESK_V3_RELATIVE_EVENT:
    u32_input_notify(w->handle, &p->event, 1);
    break;
  case DESK_KEY:
  case DESK_MOTION:
  case DESK_BUTTON:
    u32_input(w, &p->event);
    break;
  case DESK_CLOSED:
    w->wire = 0;
    w->ping = 0;
    w->lease_wanted = 0;
    u32_display_event(w, p);
    break;
  default:
    break;
  }
}
void u32_event_release(const struct desk_packet *p)
{
  if (p->opcode==DESK_DROP_FILES && p->attachment_fd>=0)
    u32_backend.gate(CIUKI_SYS_CLOSE,(DWORD)p->attachment_fd,0,0);
}
BOOL u32_self_deferred;
/* This thread's deferred events, oldest first, until the list is empty or an
 * event deferred itself again (its window is still being created): that
 * bound keeps a creating window's events from spinning the pump. */
static void deferred_drain(struct u32_thread *t) {
  while (t && t->events) {
    struct u32_pending_event *e = t->events;
    t->events = e->next;
    if (!t->events)
      t->events_tail = 0;
    t->event_count--;
    struct desk_packet packet = e->packet;
    u32_free(e);
    u32_self_deferred = 0;
    u32_event(&packet);
    if (u32_self_deferred)
      break;
  }
}
void u32_pump(void) {
  u32_session_pump();
  u32_palette_deliver();
  if (!connected)
    return;
  u32_settings();   /* once a second, before any packet is taken (see u32_input) */
  /* Events another thread of the program drained for this thread's windows
   * (u32_event defers them) are older than anything still in the channel:
   * they go first, or a key down deferred by the second thread of Quake III
   * came after the key up and the character its owner drained itself (the
   * WM_CHAR then stayed pending with the previous key's time: every other
   * keystroke lost in the console, QEMU 2026-10-26). */
  struct u32_thread *t = u32_thread();
  deferred_drain(t);
  struct desk_packet p;
  for (UINT i = 0; i < 256; i++) {
    int r = du3_next(&u32_desktop, &p);
    if (r != 1)
      break;
    u32_event(&p);
    /* A window procedure run by that event releases the USER lock; another
     * thread may have deferred a key down to this thread meanwhile. It is
     * older than the next packet: take it first (Codex review P2). */
    deferred_drain(t);
  }
  /* q3exec2: every window of the program is gone with the connection and
   * no new one can be shown; record it once on the serial log. */
  static BOOL lost;
  if (u32_desktop.disconnected && !lost) {
    lost = 1;
    w32_diag("warning", "user32.dll", "desktop-connection-lost");
  }
  struct ciuki_w32_message_args wake = {.action = W32_MSG_MARK,
                                        .bits =
                                            t && t->events ? QS_POSTMESSAGE : 0,
                                        .message = {.wparam = QS_POSTMESSAGE}};
  u32_message(&wake);
  u32_flush();
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    if (w->handle && w->tid == u32_tid() && w->ping) {
      /* q3exec3: the answer names a live window, as u32_wire requires:
       * until its DESK_CLOSE (a destruction may still fail), never after. */
      if (!w->wire || !w->generation || w->closing)
        w->ping = 0;
      else if (!du3_ui_progress(&u32_desktop, w->wire, w->generation, w->ping))
        w->ping = 0;
    }
  }
}
