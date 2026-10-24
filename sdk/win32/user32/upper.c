/* f5-20B: the remaining USER entry points the reused Wine 10.0 DLLs import
 * (window tree queries, deferred positioning, monitors and DPI, mouse
 * tracking, system timers, rectangles, clipboard formats, help).
 * Windows 98 semantics; the Windows 2000+/8.1 additions behave as on a
 * single 96-DPI monitor. References are the Microsoft pages of each
 * function, cited in build/codex/f5-20b-report.md.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "unicode.h"
extern HANDLE W32API GetModuleHandleA(const char *);
extern void *W32API GetProcAddress(HANDLE, const char *);
void *u32_kernel(const char *name) {
  HANDLE k = GetModuleHandleA("kernel32.dll");
  return k ? GetProcAddress(k, name) : 0;
}
static BOOL fail_null(DWORD e) { return u32_fail(e); }

/* ---- window tree ------------------------------------------------------ */
HWND W32API GetDesktopWindow(void) { return U32_DESKTOP; }
HWND W32API GetAncestor(HWND h, UINT flags) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || h == U32_DESKTOP) {
    if (h != U32_DESKTOP)
      u32_fail(1400);
    return 0;
  }
  switch (flags) {
  case 1: /* GA_PARENT: the parent, or the desktop for a top-level window */
    return w->parent ? w->parent : U32_DESKTOP;
  case 2: /* GA_ROOT */
    return u32_root(w)->handle;
  case 3: { /* GA_ROOTOWNER: the root, then its owners (GetParent) */
    struct u32_window *r = u32_root(w);
    for (UINT i = 0; r && r->owner && i < U32_WINDOWS; i++) {
      struct u32_window *o = u32_window(r->owner);
      if (!o)
        break;
      r = u32_root(o);
    }
    return r ? r->handle : 0;
  }
  default:
    u32_fail(87);
    return 0;
  }
}
static HWND child_at(HWND parent, POINT p, UINT skip) {
  struct u32_window *w = u32_window(parent);
  if (!w)
    return (HWND)(uintptr_t)fail_null(1400);
  RECT client;
  GetClientRect(parent, &client);
  if (!PtInRect(&client, p))
    return 0;
  HWND best = 0;
  UINT best_z = 0;
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *c = &u32_windows[i];
    if (!c->handle || c->parent != parent)
      continue;
    if (((skip & 1) && !(c->style & WS_VISIBLE)) ||
        ((skip & 2) && (c->style & WS_DISABLED)) ||
        ((skip & 4) && (c->exstyle & 0x20u)))
      continue;
    if (PtInRect(&c->rect, p) && (!best || c->z > best_z)) {
      best = c->handle;
      best_z = c->z;
    }
  }
  return best ? best : parent;
}
HWND W32API ChildWindowFromPointEx(HWND parent, POINT p, UINT flags) {
  U32_GUARD;
  return child_at(parent, p, flags);
}
HWND W32API ChildWindowFromPoint(HWND parent, POINT p) {
  U32_GUARD;
  return child_at(parent, p, 0);
}
BOOL W32API GetWindowInfo(HWND h, WINDOWINFO *out) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (!out || out->cbSize != sizeof(*out))
    return u32_fail(87);
  int bx, by;
  u32_frame(w->style, w->exstyle, &bx, &by);
  GetWindowRect(h, &out->rcWindow);
  GetClientRect(h, &out->rcClient);
  POINT o = u32_origin(w);
  OffsetRect(&out->rcClient, o.x, o.y);
  out->dwStyle = w->style;
  out->dwExStyle = w->exstyle;
  out->dwWindowStatus = h == u32_foreground ? 1u : 0u; /* WS_ACTIVECAPTION */
  out->cxWindowBorders = (UINT)bx;
  out->cyWindowBorders = (UINT)bx;
  out->atomWindowType = w->cls ? w->cls->atom : 0;
  out->wCreatorVersion = 0x0400; /* a Windows 4.x (95/98) application */
  return 1;
}

/* ---- window regions --------------------------------------------------- */
/* The region is kept for GetWindowRgn and owned by USER (deleted on
 * replacement and destruction). Limited: the desktop composes top-level
 * surfaces as rectangles, so painting and hit testing ignore the shape. */
int W32API SetWindowRgn(HWND h, HANDLE region, BOOL redraw) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || w->pid != u32_pid())
    return u32_fail(1400);
  const struct u32_graphics *g = u32_graphics();
  if (w->rgn && w->rgn != region && g)
    g->delete_object(U32_PTR(w->rgn));
  w->rgn = region;
  if (redraw && (w->style & WS_VISIBLE))
    InvalidateRect(h, 0, 1);
  return 1;
}
int W32API GetWindowRgn(HWND h, HANDLE out) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || !w->rgn || !out || !u32_gdi || !u32_gdi->region_rects ||
      !u32_gdi->region_set)
    return 0; /* ERROR: no region set */
  RECT r[U32_RECTS];
  UINT n = u32_gdi->region_rects(w->rgn, r, U32_RECTS);
  if (n > U32_RECTS || !u32_gdi->region_set(out, r, n))
    return 0;
  return n == 0 ? 1 : n == 1 ? 2 : 3; /* NULLREGION, SIMPLEREGION, COMPLEX */
}
void u32_window_region_free(struct u32_window *w) {
  const struct u32_graphics *g = u32_graphics();
  if (w->rgn && g)
    g->delete_object(U32_PTR(w->rgn));
  w->rgn = 0;
}

/* ---- deferred window positioning ---------------------------------------- */
struct deferred {
  HWND hwnd, after;
  int x, y, cx, cy;
  UINT flags;
};
static struct {
  BOOL used;
  DWORD tid;
  UINT count, capacity;
  struct deferred *items;
} defers[16];
static UINT defer_slot(HANDLE h) {
  UINT i = (UINT)((uintptr_t)h - 0x50000001u);
  return ((uintptr_t)h >> 28) == 5u && i < 16 && defers[i].used ? i : 16;
}
HANDLE W32API BeginDeferWindowPos(int n) {
  U32_GUARD;
  if (n < 0)
    return (HANDLE)(uintptr_t)u32_fail(87);
  for (UINT i = 0; i < 16; i++)
    if (!defers[i].used) {
      UINT cap = n > 8 ? (UINT)n : 8;
      if (cap > U32_WINDOWS)
        cap = U32_WINDOWS;
      struct deferred *items = u32_alloc(cap * sizeof(*items));
      if (!items)
        return 0;
      defers[i].used = 1;
      defers[i].tid = u32_tid();
      defers[i].count = 0;
      defers[i].capacity = cap;
      defers[i].items = items;
      return (HANDLE)(uintptr_t)(0x50000001u + i);
    }
  return (HANDLE)(uintptr_t)u32_fail(8);
}
static void defer_free(UINT i) {
  u32_free(defers[i].items);
  w32_fill(&defers[i], 0, sizeof(defers[i]));
}
HANDLE W32API DeferWindowPos(HANDLE info, HWND h, HWND after, int x, int y,
                             int cx, int cy, UINT flags) {
  U32_GUARD;
  UINT i = defer_slot(info);
  if (i == 16)
    return (HANDLE)(uintptr_t)u32_fail(6);
  /* Windows frees the whole structure when one entry is invalid. */
  if (!u32_window(h)) {
    defer_free(i);
    return (HANDLE)(uintptr_t)u32_fail(1400);
  }
  for (UINT k = 0; k < defers[i].count; k++)
    if (defers[i].items[k].hwnd == h) {
      defers[i].items[k] = (struct deferred){h, after, x, y, cx, cy, flags};
      return info;
    }
  if (defers[i].count == defers[i].capacity) {
    UINT cap = defers[i].capacity * 2;
    struct deferred *items = cap <= U32_WINDOWS * 2 ? u32_alloc(cap * sizeof(*items)) : 0;
    if (!items) {
      defer_free(i);
      return (HANDLE)(uintptr_t)u32_fail(8);
    }
    w32_copy(items, defers[i].items, defers[i].count * sizeof(*items));
    u32_free(defers[i].items);
    defers[i].items = items;
    defers[i].capacity = cap;
  }
  defers[i].items[defers[i].count++] =
      (struct deferred){h, after, x, y, cx, cy, flags};
  return info;
}
BOOL W32API EndDeferWindowPos(HANDLE info) {
  U32_GUARD;
  UINT i = defer_slot(info);
  if (i == 16)
    return u32_fail(6);
  BOOL ok = 1;
  for (UINT k = 0; k < defers[i].count; k++) {
    struct deferred d = defers[i].items[k];
    if (IsWindow(d.hwnd))
      ok &= SetWindowPos(d.hwnd, d.after, d.x, d.y, d.cx, d.cy, d.flags);
  }
  defer_free(i);
  return ok;
}

/* ---- processes and help ------------------------------------------------- */
/* Limited: no input-idle event exists, so a valid process handle reports
 * idle at once (Windows does so for processes without a message queue). */
DWORD W32API WaitForInputIdle(HANDLE process, DWORD ms) {
  U32_GUARD;
  (void)ms;
  DWORD(W32API * wait)(HANDLE, DWORD) =
      (DWORD(W32API *)(HANDLE, DWORD))u32_kernel("WaitForSingleObject");
  if (!process || !wait || wait(process, 0) == 0xffffffffu) {
    u32_fail(6);
    return 0xffffffffu;
  }
  return 0;
}
/* Limited: CiukiOS has no WinHelp viewer (WINHLP32.EXE). HELP_QUIT has
 * nothing to close and succeeds; other commands fail as when the viewer
 * cannot be started. */
BOOL W32API WinHelpA(HWND h, const char *file, UINT command, uintptr_t data) {
  (void)h, (void)file, (void)data;
  if (command == 2)
    return 1;
  return u32_fail(2);
}
BOOL W32API WinHelpW(HWND h, const WCHAR *file, UINT command, uintptr_t data) {
  (void)h, (void)file, (void)data;
  if (command == 2)
    return 1;
  return u32_fail(2);
}

/* ---- monitors and DPI ---------------------------------------------------- */
#define MONITOR ((HANDLE)(uintptr_t)0x00010001u)
static RECT screen(void) {
  return (RECT){0, 0, GetSystemMetrics(0), GetSystemMetrics(1)};
}
HANDLE W32API MonitorFromRect(const RECT *r, DWORD flags) {
  U32_GUARD;
  if (!r)
    return (HANDLE)(uintptr_t)u32_fail(87);
  RECT s = screen(), hit;
  if (flags == 0 && !IntersectRect(&hit, &s, r)) /* MONITOR_DEFAULTTONULL */
    return 0;
  return MONITOR;
}
HANDLE W32API MonitorFromPoint(POINT p, DWORD flags) {
  U32_GUARD;
  RECT s = screen();
  if (flags == 0 && !PtInRect(&s, p))
    return 0;
  return MONITOR;
}
HANDLE W32API MonitorFromWindow(HWND h, DWORD flags) {
  U32_GUARD;
  RECT r;
  if (h == U32_DESKTOP || !h)
    return flags ? MONITOR : 0;
  if (!GetWindowRect(h, &r))
    return flags ? MONITOR : 0;
  return MonitorFromRect(&r, flags);
}
static BOOL monitor_info(HANDLE m, void *out, BOOL wide) {
  U32_GUARD;
  MONITORINFO *i = out;
  if (m != MONITOR)
    return u32_fail(87);
  DWORD size = i ? i->cbSize : 0;
  if (!i || (size != sizeof(MONITORINFO) &&
             size != (wide ? sizeof(MONITORINFOEXW) : sizeof(MONITORINFOEXA))))
    return u32_fail(87);
  i->rcMonitor = screen();
  if (!SystemParametersInfoA(48, 0, &i->rcWork, 0))
    i->rcWork = i->rcMonitor;
  i->dwFlags = 1; /* MONITORINFOF_PRIMARY */
  if (size != sizeof(MONITORINFO)) {
    static const char device[] = "\\\\.\\DISPLAY1";
    if (wide)
      u32_to_wide(((MONITORINFOEXW *)out)->szDevice, 32, device, -1);
    else
      u32_strcpy(((MONITORINFOEXA *)out)->szDevice, 32, device);
  }
  return 1;
}
BOOL W32API GetMonitorInfoA(HANDLE m, void *out) { return monitor_info(m, out, 0); }
BOOL W32API GetMonitorInfoW(HANDLE m, void *out) { return monitor_info(m, out, 1); }
UINT W32API GetDpiForWindow(HWND h) {
  U32_GUARD;
  if (!IsWindow(h)) {
    u32_fail(1400);
    return 0;
  }
  return 96;
}
BOOL W32API IsProcessDPIAware(void) { return 0; }
/* Windows 8.1 per-monitor DPI internals (rule w21-dpi): typed W21 failures,
 * callers fall back to 96 DPI. */
static BOOL dpi_w21(const char *name) {
  static volatile ULONG once;
  if (!__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED))
    w32_diag("stub", "user32.dll", name);
  return u32_fail(120);
}
BOOL W32API GetDpiForMonitorInternal(HANDLE m, UINT type, UINT *x, UINT *y) {
  (void)m, (void)type, (void)x, (void)y;
  return dpi_w21("GetDpiForMonitorInternal");
}
BOOL W32API GetProcessDpiAwarenessInternal(HANDLE process, int *awareness) {
  (void)process, (void)awareness;
  return dpi_w21("GetProcessDpiAwarenessInternal");
}
BOOL W32API SetProcessDpiAwarenessInternal(int awareness) {
  (void)awareness;
  return dpi_w21("SetProcessDpiAwarenessInternal");
}
/* No WinEvent hooks exist (SetWinEventHook is not provided), so there is
 * nobody to notify. */
void W32API NotifyWinEvent(DWORD event, HWND h, LONG object, LONG child) {
  (void)event, (void)h, (void)object, (void)child;
}

/* ---- mouse ---------------------------------------------------------------- */
BOOL W32API DragDetect(HWND h, POINT start) {
  U32_GUARD;
  if (!IsWindow(h))
    return 0;
  int dx = GetSystemMetrics(68), dy = GetSystemMetrics(69); /* SM_CXDRAG */
  if (dx <= 0)
    dx = 4;
  if (dy <= 0)
    dy = 4;
  RECT r = {start.x - dx, start.y - dy, start.x + dx + 1, start.y + dy + 1};
  SetCapture(h);
  BOOL result = 0;
  for (UINT guard = 0; guard < 100000; guard++) {
    MSG m;
    if (GetCapture() != h)
      break;
    if (PeekMessageA(&m, 0, 0x100, 0x109, 1)) { /* keyboard */
      if (m.message == WM_KEYDOWN && m.wParam == 0x1b)
        break;
      continue;
    }
    if (PeekMessageA(&m, 0, 0x200, 0x20d, 1)) { /* mouse */
      if (m.message == WM_LBUTTONUP || m.message == WM_RBUTTONUP)
        break;
      if (m.message == WM_MOUSEMOVE && !PtInRect(&r, m.pt)) {
        result = 1;
        break;
      }
      continue;
    }
    if (MsgWaitForMultipleObjectsEx(0, 0, 0xffffffffu, 0x7u, 4) ==
        0xffffffffu)
      break;
  }
  if (GetCapture() == h)
    ReleaseCapture();
  return result;
}
/* TrackMouseEvent: one tracked window per thread; a TIMERPROC timer on the
 * window (never a WM_TIMER for its procedure) checks the pointer, posting
 * WM_MOUSEHOVER after the hover time inside the hover rectangle and
 * WM_MOUSELEAVE when the pointer leaves the window. */
#define TME_HOVER 1u
#define TME_LEAVE 2u
#define TME_NONCLIENT 0x10u
#define TME_QUERY 0x40000000u
#define TME_CANCEL 0x80000000u
#define TRACK_TIMER 0x7ff0u
static struct {
  DWORD tid, flags, hover, started;
  HWND hwnd;
  POINT anchor;
} tracks[16];
static UINT track_slot(DWORD tid, BOOL create) {
  for (UINT i = 0; i < 16; i++)
    if (tracks[i].tid == tid)
      return i;
  if (create)
    for (UINT i = 0; i < 16; i++)
      if (!tracks[i].tid) {
        w32_fill(&tracks[i], 0, sizeof(tracks[i]));
        tracks[i].tid = tid;
        return i;
      }
  return 16;
}
static BOOL pointer_in(HWND h, POINT p) {
  RECT r;
  if (!GetWindowRect(h, &r) || !PtInRect(&r, p))
    return 0;
  HWND under = WindowFromPoint(p);
  return under == h || IsChild(h, under) || !under;
}
static void W32API track_tick(HWND h, UINT m, uintptr_t id, DWORD now) {
  (void)m, (void)id;
  U32_GUARD;
  UINT i = track_slot(u32_tid(), 0);
  if (i == 16 || tracks[i].hwnd != h) {
    KillTimer(h, TRACK_TIMER);
    return;
  }
  POINT p = u32_pointer;
  if (!IsWindow(h) || !pointer_in(h, p)) {
    if (tracks[i].flags & TME_LEAVE)
      PostMessageA(h, tracks[i].flags & TME_NONCLIENT ? 0x2a2u : 0x2a3u, 0, 0);
    tracks[i].flags = 0;
    tracks[i].hwnd = 0;
    KillTimer(h, TRACK_TIMER);
    return;
  }
  if (tracks[i].flags & TME_HOVER) {
    int hx = 4; /* SPI_GETMOUSEHOVERWIDTH default */
    if (p.x < tracks[i].anchor.x - hx / 2 || p.x > tracks[i].anchor.x + hx / 2 ||
        p.y < tracks[i].anchor.y - hx / 2 || p.y > tracks[i].anchor.y + hx / 2) {
      tracks[i].anchor = p;
      tracks[i].started = now;
    } else if ((DWORD)(now - tracks[i].started) >= tracks[i].hover) {
      POINT c = p;
      ScreenToClient(h, &c);
      PostMessageA(h, tracks[i].flags & TME_NONCLIENT ? 0x2a0u : 0x2a1u, 0,
                   (LPARAM)((uint16_t)c.x | ((uint32_t)(uint16_t)c.y << 16)));
      tracks[i].flags &= ~TME_HOVER;
    }
  }
  if (!(tracks[i].flags & (TME_HOVER | TME_LEAVE))) {
    tracks[i].hwnd = 0;
    KillTimer(h, TRACK_TIMER);
  }
}
BOOL W32API TrackMouseEvent(TRACKMOUSEEVENT *e) {
  U32_GUARD;
  if (!e || e->cbSize != sizeof(*e))
    return u32_fail(87);
  UINT i = track_slot(u32_tid(), !(e->dwFlags & (TME_QUERY | TME_CANCEL)));
  if (e->dwFlags & TME_QUERY) {
    if (i == 16 || !tracks[i].hwnd) {
      e->dwFlags = 0;
      e->hwndTrack = 0;
      e->dwHoverTime = 0;
    } else {
      e->dwFlags = tracks[i].flags;
      e->hwndTrack = tracks[i].hwnd;
      e->dwHoverTime = tracks[i].hover;
    }
    return 1;
  }
  if (!IsWindow(e->hwndTrack))
    return u32_fail(1400);
  if (i == 16)
    return u32_fail(8);
  if (e->dwFlags & TME_CANCEL) {
    if (tracks[i].hwnd == e->hwndTrack) {
      tracks[i].flags &= ~(e->dwFlags & (TME_HOVER | TME_LEAVE));
      if (!(tracks[i].flags & (TME_HOVER | TME_LEAVE))) {
        KillTimer(tracks[i].hwnd, TRACK_TIMER);
        tracks[i].hwnd = 0;
      }
    }
    return 1;
  }
  POINT p = u32_pointer;
  if (!pointer_in(e->hwndTrack, p)) {
    /* Not over the window: WM_MOUSELEAVE at once, as Windows does. */
    if (e->dwFlags & TME_LEAVE)
      PostMessageA(e->hwndTrack, e->dwFlags & TME_NONCLIENT ? 0x2a2u : 0x2a3u,
                   0, 0);
    return 1;
  }
  if (tracks[i].hwnd && tracks[i].hwnd != e->hwndTrack)
    KillTimer(tracks[i].hwnd, TRACK_TIMER);
  if (tracks[i].hwnd != e->hwndTrack)
    tracks[i].flags = 0;
  tracks[i].hwnd = e->hwndTrack;
  tracks[i].flags |= e->dwFlags & (TME_HOVER | TME_LEAVE | TME_NONCLIENT);
  tracks[i].hover = e->dwHoverTime == 0xffffffffu || !e->dwHoverTime
                        ? 400u /* HOVER_DEFAULT: SPI_GETMOUSEHOVERTIME */
                        : e->dwHoverTime;
  tracks[i].anchor = p;
  tracks[i].started = u32_ticks();
  return SetTimer(e->hwndTrack, TRACK_TIMER, 50, track_tick) != 0;
}

/* ---- rectangles ----------------------------------------------------------- */
BOOL W32API InvertRect(HDC dc, const RECT *r) {
  U32_GUARD;
  const struct u32_graphics *g = u32_graphics();
  if (!g || !dc || !r)
    return u32_fail(87);
  return g->patblt(U32_PTR(dc), r->left, r->top, r->right - r->left,
                   r->bottom - r->top, 0x550009u); /* DSTINVERT */
}
BOOL W32API SubtractRect(RECT *out, const RECT *a, const RECT *b) {
  if (!out || !a || !b)
    return 0;
  RECT r = *a, hit;
  if (IsRectEmpty(a)) {
    SetRectEmpty(out);
    return 0;
  }
  /* Only a rectangle that spans the whole width or height of the first one
   * can be subtracted; the result must stay a rectangle. */
  if (IntersectRect(&hit, a, b)) {
    if (hit.left == a->left && hit.right == a->right) {
      if (hit.top == a->top)
        r.top = hit.bottom;
      else if (hit.bottom == a->bottom)
        r.bottom = hit.top;
    } else if (hit.top == a->top && hit.bottom == a->bottom) {
      if (hit.left == a->left)
        r.left = hit.right;
      else if (hit.right == a->right)
        r.right = hit.left;
    }
  }
  *out = r;
  if (IsRectEmpty(&r)) {
    SetRectEmpty(out);
    return 0;
  }
  return 1;
}

/* ---- system timers ---------------------------------------------------- */
uintptr_t u32_set_timer(HWND, uintptr_t, UINT, TIMERPROC, BOOL);
BOOL u32_kill_timer(HWND, uintptr_t, BOOL);
uintptr_t W32API SetSystemTimer(HWND h, uintptr_t id, UINT ms, TIMERPROC proc) {
  U32_GUARD;
  return u32_set_timer(h, id, ms, proc, 1);
}
BOOL W32API KillSystemTimer(HWND h, uintptr_t id) {
  U32_GUARD;
  return u32_kill_timer(h, id, 1);
}

/* ---- clipboard formats --------------------------------------------------- */
/* Registered clipboard formats share the global atom table with registered
 * window messages (0xC000-0xFFFF), as on Windows. */
UINT W32API RegisterClipboardFormatA(const char *name) {
  U32_GUARD;
  if (!name || !*name) return u32_fail(87);
  WCHAR wide[DESK_CLIP_NAME_MAX+1];
  if (!MultiByteToWideChar(1252,0,name,-1,wide,DESK_CLIP_NAME_MAX+1)) return u32_fail(87);
  return RegisterClipboardFormatW(wide);
}
UINT W32API RegisterClipboardFormatW(const WCHAR *name) {
  U32_GUARD;
  if (!name || !*name) return u32_fail(87);
  char text[DESK_CLIP_NAME_MAX+1];
  unsigned n=0;
  for (unsigned i=0;name[i];i++) {
    unsigned c=name[i];
    if (c>=0xd800 && c<=0xdbff) {
      unsigned low=name[++i]; if (low<0xdc00 || low>0xdfff) return u32_fail(87);
      c=0x10000+((c-0xd800)<<10)+(low-0xdc00);
    } else if (c>=0xdc00 && c<=0xdfff) return u32_fail(87);
    unsigned count=c<128 ? 1 : c<2048 ? 2 : c<65536 ? 3 : 4;
    if (n+count>DESK_CLIP_NAME_MAX) return u32_fail(87);
    if (count==1) text[n++]=c;
    else {
      text[n++]=(count==2 ? 0xc0 : count==3 ? 0xe0 : 0xf0)|(c>>(6*(count-1)));
      for (unsigned k=count-1;k;k--) text[n++]=0x80|((c>>(6*(k-1)))&63);
    }
  }
  text[n]=0;
  if (!desk_utf8_valid((const uint8_t *)text,n)) return u32_fail(87);
  struct w32r_reply r;
  return u32_directory_utf8(W32R_ATOM_ADD,0,0,0,0,text,0,0,&r,0,0) ? r.key : 0;
}
static int format_name(UINT format, char *out, int n) {
  if (!out || n <= 0)
    return u32_fail(87);
  if (format < 0xc000u || format > 0xffffu)
    return u32_fail(87);
  struct w32r_reply r;
  char name[W32R_NAME + 1];
  if (!u32_directory(W32R_ATOM_NAME, (HWND)(uintptr_t)format, 0, 0, 0, "", 0,
                     0, &r, name, W32R_NAME))
    return 0;
  name[r.data_bytes < W32R_NAME ? r.data_bytes : W32R_NAME] = 0;
  /* The service keeps UTF-8; decode to the code page. */
  int at = 0;
  for (const uint8_t *p = (const uint8_t *)name; *p && at + 1 < n;) {
    unsigned c = *p++;
    if (c>=0xf0 && p[0] && p[1] && p[2]) {
      c=((c&7)<<18)|((unsigned)(p[0]&63)<<12)|((unsigned)(p[1]&63)<<6)|(p[2]&63); p+=3;
    } else if (c >= 0xe0 && p[0] && p[1]) {
      c = ((c & 15) << 12) | ((unsigned)(p[0] & 63) << 6) | (p[1] & 63);
      p += 2;
    } else if (c >= 0xc0 && p[0]) {
      c = ((c & 31) << 6) | (p[0] & 63);
      p++;
    }
    out[at++] = (char)u32_ansi(c);
  }
  out[at] = 0;
  return at;
}
int W32API GetClipboardFormatNameA(UINT format, char *out, int n) {
  U32_GUARD;
  return format_name(format, out, n);
}
int W32API GetClipboardFormatNameW(UINT format, WCHAR *out, int n) {
  U32_GUARD;
  if (!out || n<=0 || format<0xc000 || format>0xffff) return u32_fail(87);
  char text[W32R_NAME+1]; struct w32r_reply r;
  if (!u32_directory(W32R_ATOM_NAME,(HWND)(uintptr_t)format,0,0,0,"",0,0,&r,text,W32R_NAME)) return 0;
  unsigned bytes=r.data_bytes<W32R_NAME ? r.data_bytes : W32R_NAME;
  text[bytes]=0;
  if (!desk_utf8_valid((const uint8_t *)text,w32_strlen(text))) return u32_fail(87);
  unsigned used=0;
  for (const uint8_t *p=(const uint8_t *)text;*p;) {
    unsigned c=*p++;
    if (c>=128) {
      unsigned left=c<0xe0 ? 1 : c<0xf0 ? 2 : 3; c&=(1u<<(6-left))-1;
      while (left--) c=(c<<6)|(*p++&63);
    }
    if (used+(c>=65536 ? 2u : 1u)>=(unsigned)n) break;
    if (c>=65536) { out[used++]=0xd800+((c-65536)>>10); c=0xdc00+((c-65536)&1023); }
    out[used++]=c;
  }
  out[used]=0; return used;
}
