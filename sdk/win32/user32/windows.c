/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
static UINT zserial;
void u32_restack(struct u32_window *w, HWND after) {
  struct u32_window *a = U32_ORDER_SENTINEL(after) ? 0 : u32_window(after),
                    *order[U32_WINDOWS];
  UINT n = 0;
  if (after == HWND_TOPMOST) w->exstyle |= WS_EX_TOPMOST;
  else if (after == HWND_BOTTOM || after == HWND_NOTOPMOST ||
           (a && !(a->exstyle & WS_EX_TOPMOST))) {
    /* SetWindowPos: a window made non-topmost takes its owners and its owned
     * windows with it; a topmost owner cannot own a non-topmost window. */
    w->exstyle &= ~WS_EX_TOPMOST;
    for (struct u32_window *o = u32_window(w->owner); o; o = u32_window(o->owner))
      o->exstyle &= ~WS_EX_TOPMOST;
    for (UINT i = 0; i < U32_WINDOWS; i++) {
      struct u32_window *q = &u32_windows[i];
      UINT depth = 0;
      for (struct u32_window *o = q->handle ? u32_window(q->owner) : 0;
           o && depth < U32_WINDOWS; o = u32_window(o->owner), depth++)
        if (o == w) { q->exstyle &= ~WS_EX_TOPMOST; break; }
    }
  } else if (a) w->exstyle |= a->exstyle & WS_EX_TOPMOST;
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *q = &u32_windows[i];
    if (!q->handle || q == w || q->parent != w->parent) continue;
    UINT at = n++;
    while (at && order[at-1]->z > q->z) { order[at] = order[at-1]; at--; }
    order[at] = q;
  }
  UINT at = after == HWND_BOTTOM ? 0 : n;
  if (a) for (at = 0; at < n && order[at] != a; at++);
  for (UINT i = n; i > at; i--) order[i] = order[i-1];
  order[at] = w; n++;
  for (UINT pass = 0; pass < n; pass++) for (UINT i = 0; i < n; i++) {
    struct u32_window *o = u32_window(order[i]->owner);
    if (o && (o->exstyle & WS_EX_TOPMOST)) order[i]->exstyle |= WS_EX_TOPMOST;
  }
  for (UINT pass = 0; pass < n; pass++) for (UINT i = 0; i + 1 < n; i++)
    if ((order[i]->exstyle & WS_EX_TOPMOST) && !(order[i+1]->exstyle & WS_EX_TOPMOST)) {
      struct u32_window *q = order[i]; order[i] = order[i+1]; order[i+1] = q;
    }
  for (UINT pass = 0; pass < n; pass++) for (UINT i = 0; i < n; i++)
    for (UINT j = i+1; j < n; j++) if (order[i]->owner == order[j]->handle) {
      struct u32_window *q = order[i];
      for (UINT k = i; k < j; k++) order[k] = order[k+1];
      order[j] = q; break;
    }
  for (UINT i = 0; i < n; i++) order[i]->z = ++zserial;
  u32_display_cache();
}
static BOOL atom_name(const char *name) { return (uintptr_t)name <= 65535; }
static int equal(const char *a, const char *b) {
  while (*a && *b) {
    unsigned x = (uint8_t)*a++, y = (uint8_t)*b++;
    if (x >= 'A' && x <= 'Z')
      x += 32;
    if (y >= 'A' && y <= 'Z')
      y += 32;
    if (x != y)
      return 0;
  }
  return *a == *b;
}
struct u32_class *u32_class(const char *n, HINSTANCE instance) {
  for (UINT i = 0; i < U32_CLASSES; i++) {
    struct u32_class *c = &u32_classes[i];
    if (c->atom && (!instance || !c->c.hInstance || c->c.hInstance == instance) &&
        (atom_name(n) ? c->atom == (uintptr_t)n : equal(c->name, n)))
      return c;
  }
  return 0;
}
ATOM W32API RegisterClassExA(const WNDCLASSEXA *c) {
  U32_GUARD;
  if (!c || c->cbSize != sizeof(*c) || !c->lpfnWndProc || !c->lpszClassName ||
      atom_name(c->lpszClassName) || w32_strlen(c->lpszClassName) > 255 ||
      c->cbClsExtra < 0 || c->cbWndExtra < 0 || c->cbClsExtra > 4096 ||
      c->cbWndExtra > 4096)
    return u32_fail(87);
  if (u32_class(c->lpszClassName, c->hInstance))
    return u32_fail(1410);
  struct u32_class *slot = 0;
  for (UINT i = 0; i < U32_CLASSES; i++)
    if (!u32_classes[i].atom) {
      slot = &u32_classes[i];
      break;
    }
  if (!slot)
    return u32_fail(8);
  uint8_t *extra = c->cbClsExtra ? u32_alloc(c->cbClsExtra) : 0;
  if (c->cbClsExtra && !extra)
    return 0;
  struct w32r_reply r;
  if (!u32_directory(W32R_ATOM_ADD, 0, 1, 0, 0, c->lpszClassName, 0, 0, &r, 0,
                     0)) {
    u32_free(extra);
    return 0;
  }
  slot->atom = (ATOM)r.key;
  slot->c = *c;
  w32_copy(slot->name, c->lpszClassName, w32_strlen(c->lpszClassName) + 1);
  slot->c.lpszClassName = slot->name;
  slot->extra = extra;
  return slot->atom;
}
ATOM W32API RegisterClassA(const WNDCLASSA *c) {
  U32_GUARD;
  if (!c)
    return u32_fail(87);
  WNDCLASSEXA x = {sizeof(x),       c->style,         c->lpfnWndProc,
                   c->cbClsExtra,   c->cbWndExtra,    c->hInstance,
                   c->hIcon,        c->hCursor,       c->hbrBackground,
                   c->lpszMenuName, c->lpszClassName, 0};
  return RegisterClassExA(&x);
}
/* f5-20B: the predefined control classes register lazily on lookup too,
 * so superclassing (GetClassInfo("EDIT")) works before any CreateWindow. */
static struct u32_class *known_class(const char *n, HINSTANCE i) {
  struct u32_class *c = u32_class(n, i);
  if (!c) {
    u32_controls_register();
    c = u32_class(n, i);
  }
  return c;
}
BOOL W32API GetClassInfoExA(HINSTANCE i, const char *n, WNDCLASSEXA *out) {
  U32_GUARD;
  struct u32_class *c = known_class(n, i);
  if (!c || !out || out->cbSize != sizeof(*out))
    return u32_fail(c ? 87 : 1411);
  *out = c->c;
  out->lpfnWndProc = (WNDPROC)u32_proc_handle(c->c.lpfnWndProc, c->unicode, 0);
  return 1;
}
BOOL W32API GetClassInfoA(HINSTANCE i, const char *n, WNDCLASSA *out) {
  U32_GUARD;
  struct u32_class *c = known_class(n, i);
  if (!c || !out)
    return u32_fail(c ? 87 : 1411);
  *out = (WNDCLASSA){c->c.style,
                     (WNDPROC)u32_proc_handle(c->c.lpfnWndProc, c->unicode, 0),
                     c->c.cbClsExtra,
                     c->c.cbWndExtra, c->c.hInstance,     c->c.hIcon,
                     c->c.hCursor,    c->c.hbrBackground, c->c.lpszMenuName,
                     c->name};
  return 1;
}
BOOL W32API UnregisterClassA(const char *n, HINSTANCE i) {
  U32_GUARD;
  struct u32_class *c = u32_class(n, i);
  if (!c)
    return u32_fail(1411);
  if (c->refs)
    return u32_fail(1412);
  struct w32r_reply r;
  if (!u32_directory(W32R_ATOM_DELETE, (HWND)(uintptr_t)c->atom, 1, 0, 0, "", 0,
                     0, &r, 0, 0))
    return 0;
  u32_free(c->extra);
  u32_free(c->wmenu);
  u32_free(c->amenu);
  w32_fill(c, 0, sizeof(*c));
  return 1;
}
HWND W32API CreateWindowExA(DWORD ex, const char *klass, const char *title,
                            DWORD style, int x, int y, int width, int height,
                            HWND parent, HANDLE menu, HINSTANCE instance,
                            void *param) {
  U32_GUARD;
  /* f5-10C: predefined control atoms and the dialog class register lazily. */
  static const char *const predefined[] = {"BUTTON",  "EDIT",      "STATIC",
                                           "LISTBOX", "SCROLLBAR", "COMBOBOX"};
  if ((uintptr_t)klass >= 0x80 && (uintptr_t)klass <= 0x85)
    klass = predefined[(uintptr_t)klass - 0x80];
  if ((uintptr_t)klass == 32770)
    klass = "#32770";
  /* f5-20B: the desktop as parent or owner makes a top-level window. */
  if (parent == U32_DESKTOP) {
    parent = 0;
    style &= ~WS_CHILD;
  }
  struct u32_class *c = u32_class(klass, instance);
  if (!c) {
    u32_controls_register();
    c = u32_class(klass, instance);
  }
  if (!c) {
    u32_fail(1407);
    return 0;
  }
  if ((style & WS_CHILD) && !u32_window(parent)) {
    u32_fail(1400);
    return 0;
  }
  if (title && w32_strlen(title) > 255) {
    u32_fail(87);
    return 0;
  }
  if (x == (int)0x80000000)
    x = 32;
  if (y == (int)0x80000000)
    y = 32;
  if (width == (int)0x80000000)
    width = 640;
  if (height == (int)0x80000000)
    height = 480;
  if (width < 0 || height < 0 || width > 2048 || height > 2048 || x < -8192 ||
      x > 8192 || y < -8192 || y > 8192) {
    u32_fail(87);
    return 0;
  }
  struct u32_window *w = 0;
  for (UINT i = 0; i < U32_WINDOWS; i++)
    if (!u32_windows[i].handle) {
      w = &u32_windows[i];
      break;
    }
  if (!w) {
    u32_fail(8);
    return 0;
  }
  struct w32r_reply r;
  if (!u32_directory(W32R_WINDOW_ALLOC, 0, u32_tid(), c->atom, style, title, 0,
                     0, &r, 0, 0))
    return 0;
  w32_fill(w, 0, sizeof(*w));
  w->surface.fd = -1;
  w->handle = (HWND)(uintptr_t)r.key;
  w->tid = u32_tid();
  w->pid = u32_pid();
  w->style = style;
  w->modal_prompt = !!(ex & U32_EX_MODAL_PROMPT);   /* review 4, finding 6: per-window, before the CREATE packet */
  ex &= ~U32_EX_MODAL_PROMPT;
  w->exstyle = ex;
  w->proc = c->c.lpfnWndProc;
  w->unicode = c->unicode;
  w->cls = c;
  w->instance = instance;
  w->id = style & WS_CHILD ? U32_PTR(menu) : 0;
  if (!(style & WS_CHILD)) {
    w->menu = menu;
    if (!menu && c->c.lpszMenuName)
      w->menu = LoadMenuA(c->c.hInstance ? c->c.hInstance : instance,
                          c->c.lpszMenuName);
    if (w->menu && !IsMenu(w->menu))
      w->menu = 0;
  }
  w->redraw = 1;
  w->creating = 1;
  w->z = ++zserial;
  w->rect = (RECT){x, y, x + width, y + height};
  if (style & WS_CHILD)
    w->parent = parent;
  else
    w->owner = parent;
  if (title)
    w32_copy(w->title, title, w32_strlen(title) + 1);
  c->refs++;
  if (c->c.cbWndExtra) {
    w->extra = u32_alloc(c->c.cbWndExtra);
    if (!w->extra)
      goto fail;
  }
  if (!u32_sync_window(w))
    goto fail;
  HWND hwnd = w->handle;
  CREATESTRUCTA cs = {param, instance, menu,        parent, height, width,
                      y,     x,        (LONG)style, title,  klass,  ex};
  /* fx-hlrun: WH_CBT HCBT_CREATEWND, once the window exists and before any
   * message reaches it (WM_GETMINMAXINFO, WM_NCCREATE): MFC subclasses the
   * window here. A nonzero result frees the window without messages and
   * CreateWindowEx fails. The hook may move or resize the window through the
   * CREATESTRUCT, which the window then takes. */
  struct {
    CREATESTRUCTA *cs;
    HWND after;
  } cbt = {&cs, 0 /* HWND_TOP */};
  BOOL hooked = u32_hook_set(5);
  if (u32_hooks(5, 3 /* HCBT_CREATEWND */, (WPARAM)hwnd, (LPARAM)&cbt) || u32_window(hwnd) != w) {
    if (u32_window(hwnd) == w)
      goto fail;
    return 0;
  }
  if (cs.x != x || cs.y != y || cs.cx != width || cs.cy != height) {
    if (cs.cx < 0 || cs.cy < 0 || cs.cx > 2048 || cs.cy > 2048)
      goto fail_hooked;
    w->rect = (RECT){cs.x, cs.y, cs.x + cs.cx, cs.y + cs.cy};
    if (!u32_sync_window(w))
      goto fail_hooked;
  }
  if (!(style & WS_CHILD) && !u32_create_wire(w))
    goto fail_hooked;
  if (!u32_invoke(hwnd, WM_NCCREATE, 0, (LPARAM)&cs) || !IsWindow(hwnd))
    goto fail_live;
  if (u32_invoke(hwnd, WM_CREATE, 0, (LPARAM)&cs) == -1 || !IsWindow(hwnd))
    goto fail_live;
  w->creating = 0;
  if (style & WS_VISIBLE) {
    u32_invoke(hwnd, WM_SHOWWINDOW, 1, 0);
    InvalidateRect(hwnd, 0, 1);
  }
  return hwnd;
fail_live:
  if (IsWindow(hwnd))
    DestroyWindow(hwnd);
  return 0;
fail_hooked:
  /* fx-hlrun: a WH_CBT hook has seen the window (MFC subclassed it and
   * attached its CWnd): a failure from here on ends it with WM_NCDESTROY,
   * as Wine 10.0 win32u/window.c create_window does (failed:
   * destroy_window sends WM_NCDESTROY), before the handle is freed. Without
   * a hook no code knows the handle and the window goes without messages. */
  if (hooked) {
    u32_invoke(hwnd, WM_NCDESTROY, 0, 0);
    if (u32_window(hwnd) != w)
      return 0;
  }
fail:
  u32_directory(W32R_WINDOW_FREE, w->handle, 0, 0, 0, "", 0, 0, &r, 0, 0);
  u32_free(w->extra);
  c->refs--;
  w32_fill(w, 0, sizeof(*w));
  return 0;
}
BOOL W32API IsWindow(HWND h) {
  U32_GUARD;
  if (u32_window(h) || h == U32_DESKTOP)
    return 1;
  struct w32r_window v;
  return h && u32_directory_window(h, &v, 0);
}
BOOL W32API IsChild(HWND p, HWND h) {
  U32_GUARD;
  for (UINT i = 0; h && i < 4096; i++) {
    struct u32_window *w = u32_window(h);
    if (w) {
      if (!w->parent)
        return 0;
      h = w->parent;
    } else {
      struct w32r_window value;
      if (!u32_directory_window(h, &value, 0) || !value.parent)
        return 0;
      h = (HWND)(uintptr_t)value.parent;
    }
    if (h == p)
      return 1;
  }
  return 0;
}
BOOL W32API IsWindowVisible(HWND h) {
  U32_GUARD;
  if (h == U32_DESKTOP)
    return 1;
  struct u32_window *w = u32_window(h);
  for (UINT i = 0; w && i < U32_WINDOWS; i++) {
    if (!(w->style & WS_VISIBLE))
      return 0;
    if (!w->parent)
      return 1;
    w = u32_window(w->parent);
  }
  return 0;
}
BOOL W32API IsWindowEnabled(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  return w && !(w->style & WS_DISABLED);
}
BOOL W32API IsIconic(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  return w && !!(w->style & WS_MINIMIZE);
}
BOOL W32API IsZoomed(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  return w && !!(w->style & WS_MAXIMIZE);
}
BOOL W32API EnableWindow(HWND h, BOOL enabled) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  BOOL disabled = !!(w->style & WS_DISABLED);
  if (disabled == !!enabled) {
    if (enabled)
      w->style &= ~WS_DISABLED;
    else
      w->style |= WS_DISABLED;
    u32_invoke(h, WM_ENABLE, !!enabled, 0);
  }
  return disabled;
}
extern void u32_destroy_paint(struct u32_window *);
BOOL W32API DestroyWindow(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (w->tid != u32_tid())
    return u32_fail(5);
  /* fx-hlrun: WH_CBT HCBT_DESTROYWND; a nonzero result keeps the window. */
  if (!w->destroying && u32_hooks(5, 4 /* HCBT_DESTROYWND */, (WPARAM)h, 0))
    return 0;
  if (!u32_window(h))
    return 1;
  return u32_destroy(w, U32_NORMAL);
}
/* Owned windows (PHASE 0, before WM_DESTROY), child windows (PHASE 1,
 * after it) or owned windows again (PHASE 2, after every callback) of W.
 * Only phase 0 can still fail and keep W; phases 1 and 2 repeat until none
 * is left, since a callback may create another one (q3exec4 review: a
 * popup a child creates in its WM_DESTROY, owned by W, must not outlive
 * W, whose desktop window takes it along). */
static BOOL u32_destroy_dependents(struct u32_window *w, UINT mode, UINT phase) {
  HWND h = w->handle;
  for (UINT round = 0; round < U32_WINDOWS && w->handle == h; round++) {
    BOOL found = 0;
    for (UINT i = 0; i < U32_WINDOWS; i++) {
      struct u32_window *q = &u32_windows[i];
      if (!q->handle || q->destroying ||
          (phase == 1 ? q->parent != h : q->parent || q->owner != h))
        continue;
      HWND dependent = q->handle;
      BOOL remote = 0;
      found = 1;
      if (mode == U32_TEARDOWN || (mode == U32_VICTIM && q->tid == w->tid))
        u32_destroy(q, mode);
      else if (q->tid == u32_tid())
        u32_destroy(q, U32_NORMAL);
      else {
        remote = 1;
        q->destroy_request = 1;
        BOOL sent = SendMessageA(q->handle, U32_DESTROY, 0, 0) != 0;
        if (q->handle == dependent && !sent && mode == U32_NORMAL && phase == 0) {
          q->destroy_request = 0;
          w->destroying = 0;
          return 0;
        }
      }
      /* An unreachable owner thread, or (once W's own WM_DESTROY has run) a
       * failed destruction: release, never leak. */
      if (q->handle == dependent && !q->destroying && (remote || phase))
        u32_destroy(q, U32_TEARDOWN);
    }
    if (!found || phase == 0)
      break;
  }
  return 1;
}
/* MODE: U32_TEARDOWN releases without any callback or send (process exit);
 * U32_NORMAL is DestroyWindow on the owning thread; U32_VICTIM releases a
 * terminated thread's window without running its procedures, while live
 * dependents still receive WM_DESTROY on their own threads. */
BOOL u32_destroy(struct u32_window *w, UINT mode) {
  HWND h = w->handle;
  BOOL callbacks = mode == U32_NORMAL;
  if (w->destroying)
    return 1;
  w->destroying = 1;
  /* q3exec4: Windows order (WM_DESTROY on Microsoft Learn: "sent first to
   * the window being destroyed and then to the child windows (if any) as
   * they are destroyed. During the processing of the message, it can be
   * assumed that all child windows still exist"). Owned windows go first,
   * then the window's WM_DESTROY while its children exist (Installer VISE
   * reads the destination folder from its edit control there), then the
   * children, each with its own WM_DESTROY and WM_NCDESTROY, and last the
   * window's WM_NCDESTROY. A dependent owned by another thread of this
   * process is destroyed on that thread through a private sent request
   * (DestroyWindow is thread-bound). */
  if (!u32_destroy_dependents(w, mode, 0))
    return 0;
  if (callbacks && u32_capture == h)
    ReleaseCapture();
  else if (u32_capture == h) u32_capture = 0;
  if (callbacks && GetFocus() == h)
    SetFocus(0);
  if (callbacks && GetActiveWindow() == h)
    SetActiveWindow(0);
  if (u32_foreground == h)
    u32_foreground = 0;
  u32_input_focus();
  u32_clip_render_all(h,callbacks);
  if (callbacks) u32_invoke(h, WM_DESTROY, 0, 0);
  /* Children last: after WM_DESTROY nothing can cancel the destruction, so
   * an unreachable child thread's window is released (never leaked). */
  u32_destroy_dependents(w, mode, 1);
  for (UINT i = 0; i < U32_TIMERS; i++)
    if (u32_timers[i].tid && u32_timers[i].hwnd == h)
      w32_fill(&u32_timers[i], 0, sizeof(u32_timers[i]));
  if (callbacks) u32_invoke(h, WM_NCDESTROY, 0, 0);
  u32_destroy_dependents(w, mode, 2);
  u32_classic_destroy(h); /* USER-owned control/dialog/menu/caret records, also at thread exit */
  u32_window_region_free(w);
  w32_user_display_release((uintptr_t)h);
  u32_destroy_paint(w);
  while (w->props) {
    struct u32_prop *p = w->props;
    w->props = p->next;
    u32_free(p->name);
    u32_free(p);
  }
  /* At process detach the desktop connection is closed next, which drops
   * every window of the connection in one step. Pending frame updates of a
   * closing window are discarded (u32_flush skips destroying windows). */
  w->damage.n = 0;
  w->ping = 0;
  if (w->wire && !u32_exiting) {
    w->closing = 1;
    u32_wire(w, DESK_CLOSE, 0, 0, -1);
  }
  u32_backend.drop_surface(&w->surface);
  struct w32r_reply r;
  BOOL ok = u32_directory(W32R_WINDOW_FREE, h, 0, 0, 0, "", 0, 0, &r, 0, 0);
  w->cls->refs--;
  u32_free(w->extra);
  w32_fill(w, 0, sizeof(*w));
  u32_display_cache();
  u32_mark();
  return ok;
}
void u32_frame(DWORD style, DWORD ex, int *bx, int *by) {
  if (!(style & WS_CHILD)) {
    BOOL popup =
        (style & WS_POPUP) && !(style & WS_CAPTION) && !(ex & WS_EX_TOOLWINDOW);
    *bx = popup ? 0 : 3;
    *by = popup ? 0 : 24;
    return;
  }
  int border = style & (WS_BORDER | WS_DLGFRAME | WS_THICKFRAME) ? 1 : 0;
  if (ex & WS_EX_CLIENTEDGE)
    border += 2;
  else if (ex & WS_EX_STATICEDGE)
    border++;
  *bx = border;
  *by = border + ((style & WS_CAPTION) == WS_CAPTION ? 18 : 0);
}
BOOL W32API GetClientRect(HWND h, RECT *r) {
  U32_GUARD;
  if (h == U32_DESKTOP && r)
    return SetRect(r, 0, 0, GetSystemMetrics(0), GetSystemMetrics(1));
  struct u32_window *w = u32_window(h);
  if (!w || !r)
    return u32_fail(1400);
  int bx, by, top, right, bottom;
  u32_frame(w->style, w->exstyle, &bx, &by);
  u32_nonclient(w, &top, &right, &bottom);
  int width = w->rect.right - w->rect.left - 2 * bx - right,
      height = w->rect.bottom - w->rect.top - by - bx - top - bottom;
  return SetRect(r, 0, 0, width > 0 ? width : 0, height > 0 ? height : 0);
}
POINT u32_origin(struct u32_window *w) {
  POINT p = {0, 0};
  for (UINT i = 0; w && i < U32_WINDOWS; i++) {
    int bx, by, top, right, bottom;
    u32_frame(w->style, w->exstyle, &bx, &by);
    u32_nonclient(w, &top, &right, &bottom);
    p.x += w->rect.left + bx;
    p.y += w->rect.top + by + top;
    w = u32_window(w->parent);
  }
  return p;
}
BOOL W32API GetWindowRect(HWND h, RECT *r) {
  U32_GUARD;
  if (h == U32_DESKTOP && r)
    return SetRect(r, 0, 0, GetSystemMetrics(0), GetSystemMetrics(1));
  struct u32_window *w = u32_window(h);
  if (!w || !r)
    return u32_fail(1400);
  *r = w->rect;
  if (w->parent) {
    POINT p = u32_origin(u32_window(w->parent));
    OffsetRect(r, p.x, p.y);
  }
  return 1;
}
/* fx-is5boot (InstallShield 5 engine import). The desktop minimizes and
 * zooms a window without a separate restore rectangle, so rcNormalPosition
 * is the current rectangle: parent client coordinates for a child, work-area
 * coordinates for a top-level window. Minimized/maximized positions were
 * never set: (-1, -1). showCmd follows WS_MINIMIZE/WS_MAXIMIZE. */
struct u32_placement {
  UINT length, flags, show;
  POINT minimum, maximum;
  RECT normal;
};
BOOL W32API GetWindowPlacement(HWND h, struct u32_placement *p) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (!p)
    return u32_fail(998);
  p->length = sizeof(*p);
  p->flags = 0;
  p->show = w->style & WS_MINIMIZE ? 2 : w->style & WS_MAXIMIZE ? 3 : 1;
  p->minimum.x = p->minimum.y = p->maximum.x = p->maximum.y = -1;
  p->normal = w->rect;
  RECT area;
  if (!w->parent && SystemParametersInfoA(48, 0, &area, 0))
    OffsetRect(&p->normal, -area.left, -area.top);
  return 1;
}
/* fx-hlrun (Sierra's MFC utilities import it): the inverse of
 * GetWindowPlacement. rcNormalPosition (work-area coordinates for a
 * top-level window, parent client coordinates for a child) becomes the
 * window's rectangle, then showCmd is applied as by ShowWindow. USER keeps
 * no separate minimized/maximized positions (the desktop places icons and
 * zoomed windows), so ptMinPosition/ptMaxPosition are accepted and unused;
 * Windows 98 does not check the length field (Wine 10.0 dlls/win32u
 * window.c NtUserSetWindowPlacement validates only the pointer). */
BOOL W32API SetWindowPlacement(HWND h, const struct u32_placement *p) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (!p)
    return u32_fail(998);
  RECT r = p->normal;
  RECT area;
  if (!w->parent && SystemParametersInfoA(48, 0, &area, 0))
    OffsetRect(&r, area.left, area.top);
  if (r.right < r.left || r.bottom < r.top)
    return u32_fail(87);
  BOOL zoomed = !!(w->style & (WS_MINIMIZE | WS_MAXIMIZE));
  if (zoomed)
    ShowWindow(h, 9 /* SW_RESTORE */);
  if (!u32_window(h) || !MoveWindow(h, r.left, r.top, r.right - r.left, r.bottom - r.top, 1))
    return 0;
  ShowWindow(h, (int)p->show);
  return 1;
}
/* fx-hlrun: OpenIcon restores a minimized window ("Restores a minimized
 * (iconic) window to its previous size and position; it then activates the
 * window"); FALSE when the window is not minimized, as in Wine 10.0
 * dlls/user32/win.c OpenIcon. */
BOOL W32API OpenIcon(HWND h) {
  U32_GUARD;
  if (!u32_window(h))
    return u32_fail(1400);
  if (!IsIconic(h))
    return 0;
  ShowWindow(h, 1 /* SW_SHOWNORMAL */);
  return 1;
}
BOOL W32API ClientToScreen(HWND h, POINT *p) {
  U32_GUARD;
  if (h == U32_DESKTOP && p)
    return 1;
  struct u32_window *w = u32_window(h);
  if (!w || !p)
    return u32_fail(1400);
  POINT o = u32_origin(w);
  p->x += o.x;
  p->y += o.y;
  return 1;
}
BOOL W32API ScreenToClient(HWND h, POINT *p) {
  U32_GUARD;
  if (h == U32_DESKTOP && p)
    return 1;
  struct u32_window *w = u32_window(h);
  if (!w || !p)
    return u32_fail(1400);
  POINT o = u32_origin(w);
  p->x -= o.x;
  p->y -= o.y;
  return 1;
}
int W32API MapWindowPoints(HWND from, HWND to, POINT *p, UINT n) {
  U32_GUARD;
  POINT a = {0}, b = {0};
  if ((from && !ClientToScreen(from, &a)) || (to && !ClientToScreen(to, &b)) ||
      (!p && n))
    return 0;
  int x = a.x - b.x, y = a.y - b.y;
  for (UINT i = 0; i < n; i++)
    p[i].x += x, p[i].y += y;
  return (uint16_t)x | ((uint32_t)(uint16_t)y << 16);
}
BOOL W32API AdjustWindowRectEx(RECT *r, DWORD style, BOOL menu, DWORD ex) {
  U32_GUARD;
  if (!r)
    return u32_fail(87);
  int bx, by;
  u32_frame(style, ex, &bx, &by);
  r->left -= bx;
  r->right += bx;
  r->top -= by + (menu ? 18 : 0);
  r->bottom += bx;
  return 1;
}
BOOL W32API AdjustWindowRect(RECT *r, DWORD style, BOOL menu) {
  U32_GUARD;
  return AdjustWindowRectEx(r, style, menu, 0);
}
BOOL W32API SetWindowPos(HWND h, HWND after, int x, int y, int cx, int cy,
                         UINT flags) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (!(flags & SWP_NOMOVE) && (x < -8192 || x > 8192 || y < -8192 || y > 8192))
    return u32_fail(87);
  struct u32_window *sibling = U32_ORDER_SENTINEL(after) ? 0 : u32_window(after);
  if (!(flags & SWP_NOZORDER) && !U32_ORDER_SENTINEL(after) &&
      (!sibling || sibling->parent != w->parent || sibling == w))
    return u32_fail(1400);
  RECT before = w->rect;
  DWORD old_style = w->style, old_exstyle = w->exstyle;
  UINT old_z = w->z;
  if (!(flags & SWP_NOMOVE)) {
    w->rect.right += x - w->rect.left;
    w->rect.bottom += y - w->rect.top;
    w->rect.left = x;
    w->rect.top = y;
  }
  if (!(flags & SWP_NOSIZE)) {
    if (cx < 0 || cy < 0 || cx > 2048 || cy > 2048) {
      w->rect = before;
      return u32_fail(87);
    }
    w->rect.right = w->rect.left + cx;
    w->rect.bottom = w->rect.top + cy;
  }
  if (!(flags & SWP_NOZORDER)) {
    if (after == HWND_TOPMOST)
      w->exstyle |= WS_EX_TOPMOST;
    else if (after == HWND_NOTOPMOST)
      w->exstyle &= ~WS_EX_TOPMOST;
  }
  if (flags & SWP_SHOWWINDOW)
    w->style |= WS_VISIBLE;
  if (flags & SWP_HIDEWINDOW) {
    w->style &= ~WS_VISIBLE;
    /* q3exec4 review: the area it occupied before this call is exposed. */
    if (old_style & WS_VISIBLE)
      u32_child_hidden(w, before);
  }
  if (!w->parent && !u32_place(w)) {
    w->rect = before;
    w->style = old_style;
    w->exstyle = old_exstyle;
    w->z = old_z;
    return 0;
  }
  if (!(flags & SWP_NOZORDER)) {
    if (!w->parent && !u32_order_wire(w, after, 0, DESK_V3_ORDER_Z)) {
      w->rect = before; w->style = old_style; w->exstyle = old_exstyle;
      u32_place(w);
      return 0;
    }
    u32_restack(w, after);
  }
  u32_sync_window(w);
  /* q3exec4 review: SWP_SHOWWINDOW paints a window that was hidden, even
   * without a move or a size change. */
  if ((flags & SWP_SHOWWINDOW) && !(old_style & WS_VISIBLE)) {
    if (w->parent)
      u32_child_shown(w);
    else
      InvalidateRect(h, 0, 1);
  }
  if (before.left != w->rect.left || before.top != w->rect.top)
    u32_invoke(h, WM_MOVE, 0,
               (uint16_t)w->rect.left |
                   ((uint32_t)(uint16_t)w->rect.top << 16));
  if (before.right - before.left != cx || before.bottom - before.top != cy) {
    if (!(flags & SWP_NOSIZE)) {
      RECT client;
      GetClientRect(h, &client);
      u32_invoke(h, WM_SIZE, IsIconic(h) ? SIZE_MINIMIZED : IsZoomed(h) ? SIZE_MAXIMIZED : SIZE_RESTORED,
                 (uint16_t)client.right |
                     ((uint32_t)(uint16_t)client.bottom << 16));
      InvalidateRect(h, 0, 1);
    }
  }
  /* fx-hlrun: without SWP_DEFERERASE the parent is erased now when a
   * window is hidden, moved or resized, and a window that becomes visible
   * is erased now (Wine 10.0 dlls/win32u/window.c set_window_pos; Windows
   * 9x USER erases synchronously in SetWindowPos). */
  if (!(flags & 0x2000 /* SWP_DEFERERASE */) && u32_window(h)) {
    HWND parent = w->parent ? w->parent : h;
    BOOL geometry = !EqualRect(&before, &w->rect);
    if ((flags & SWP_HIDEWINDOW) || (!(flags & SWP_SHOWWINDOW) && geometry))
      u32_erase_now(parent, 0);
    if ((flags & SWP_SHOWWINDOW) && !(old_style & WS_VISIBLE) && u32_window(h))
      u32_erase_now(h, 0);
  }
  if (!(w = u32_window(h)))
    return 1;
  if (!(flags & SWP_NOACTIVATE) && (w->style & WS_VISIBLE))
    SetActiveWindow(h);
  return 1;
}
BOOL W32API MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL paint) {
  U32_GUARD;
  BOOL r = SetWindowPos(h, 0, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE);
  if (r && paint)
    UpdateWindow(h);
  return r;
}
BOOL W32API ShowWindow(HWND h, int cmd) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (!w->parent) cmd=u32_startup_show(w,cmd);
  BOOL old = !!(w->style & WS_VISIBLE);
  DWORD prev = w->style;
  /* fx-hlrun: WH_CBT HCBT_MINMAX (lParam: the show command) before a
   * minimize or maximize, or a restore of a minimized or maximized window;
   * a nonzero result keeps the current state (visibility still changes). */
  BOOL state = cmd == 2 || cmd == 3 || cmd == 6 || cmd == 7 ||
               ((cmd == 1 || cmd == 4 || cmd == 9 || cmd == 10) && (w->style & (WS_MINIMIZE | WS_MAXIMIZE)));
  BOOL refused = state && u32_hooks(5, 1 /* HCBT_MINMAX */, (WPARAM)h, (LPARAM)cmd);
  if (refused && !u32_window(h))
    return old;
  /* fx-hlrun: a command that changes nothing does nothing (no placement,
   * activation or repaint) and returns the previous visibility: hiding a
   * hidden window, SW_SHOW of a visible one, SW_SHOWNORMAL, SW_SHOWNOACTIVATE,
   * SW_RESTORE or SW_SHOWDEFAULT of a visible window that is neither
   * minimized nor maximized, minimizing a minimized or maximizing a
   * maximized visible window (after WH_CBT HCBT_MINMAX), and an unknown
   * command. Wine 10.0 dlls/win32u/window.c show_window ("goto done");
   * Microsoft ShowWindow: the return value is the previous visibility.
   * Half-Life's launcher calls ShowWindow(dialog, SW_SHOWNORMAL) on its
   * visible menu several times a second. */
  if (!old ? !cmd || cmd < 0 || cmd > 11
           : cmd == 5 /* SW_SHOW */ || cmd < 0 || cmd > 11 ||
                 ((cmd == 1 || cmd == 4 || cmd == 9 || cmd == 10) &&
                  !(w->style & (WS_MINIMIZE | WS_MAXIMIZE))) ||
                 ((cmd == 2 || cmd == 6 || cmd == 7 || cmd == 11) && (w->style & WS_MINIMIZE)) ||
                 (cmd == 3 && (w->style & WS_MAXIMIZE))) {
    /* CreateWindowEx here shows a WS_VISIBLE top-level window without the
     * ShowWindow(SW_SHOW) that activates it on Windows, so an activating
     * command still activates such a window when it is not active. */
    struct u32_thread *t = u32_thread();
    if (old && !w->parent && (cmd == 1 || cmd == 3 || cmd == 5 || cmd == 9 || cmd == 10) && t &&
        t->active != h)
      SetActiveWindow(h);
    return old;
  }
  if (!cmd)
    w->style &= ~WS_VISIBLE;
  else
    w->style |= WS_VISIBLE;
  if (refused) {
    /* WH_CBT kept the minimized/maximized state */
  } else if (cmd == 2 || cmd == 6 || cmd == 7)
    w->style |= WS_MINIMIZE;
  else if (cmd) {
    w->style &= ~WS_MINIMIZE;
    if (cmd == 3)
      w->style |= WS_MAXIMIZE;
    else
      w->style &= ~WS_MAXIMIZE;
  }
  if (!w->parent)
    u32_place(w);
  u32_sync_window(w);
  if (old != !!(w->style & WS_VISIBLE))
    u32_invoke(h, WM_SHOWWINDOW, !!cmd, 0);
  if (old && !cmd)
    u32_child_hidden(w, w->rect);
  else if (!old && cmd && w->parent)
    u32_child_shown(w);
  /* A connected top-level window learns its maximised size from the desktop
   * (DESK_CONFIGURE), which sends WM_SIZE with SIZE_MAXIMIZED itself. */
  BOOL desktop_zoom = w->wire && !w->parent;
  if ((prev ^ w->style) & (desktop_zoom ? WS_MINIMIZE : (WS_MINIMIZE | WS_MAXIMIZE))) {
    RECT r;
    GetClientRect(h, &r);
    u32_invoke(h, WM_SIZE, IsIconic(h) ? SIZE_MINIMIZED : IsZoomed(h) ? SIZE_MAXIMIZED : SIZE_RESTORED,
               IsIconic(h) ? 0
                           : ((uint16_t)r.right |
                              ((uint32_t)(uint16_t)r.bottom << 16)));
  }
  if (cmd && cmd != 4 && cmd != 7 && cmd != 8)
    SetActiveWindow(h);
  /* fx-hlrun: SW_SHOWNA of a visible window only raises it (Wine
   * show_window: SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
   * a window that appears or changes state is repainted. */
  if (cmd && (!old || ((prev ^ w->style) & (WS_MINIMIZE | WS_MAXIMIZE))))
    InvalidateRect(h, 0, 1);
  /* fx-hlrun: a window that becomes visible is erased now, and hiding a
   * child erases what it uncovered in its parent now (ShowWindow is
   * SetWindowPos with SWP_SHOWWINDOW or SWP_HIDEWINDOW: Wine 10.0
   * dlls/win32u/window.c show_window and set_window_pos). */
  if (!old && cmd && u32_window(h))
    u32_erase_now(h, 0);
  else if (old && !cmd && (w = u32_window(h)) && w->parent)
    u32_erase_now(w->parent, 0);
  return old;
}
/* f7 (Quake III imports it): CloseWindow minimizes, never destroys.
 * Windows: "Minimizes (but does not destroy) the specified window"; a child
 * window is not minimized (FALSE), as in Wine's CloseWindow. */
BOOL W32API CloseWindow(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (w->style & WS_CHILD)
    return 0;
  ShowWindow(h, 6 /* SW_MINIMIZE */);
  return 1;
}
HWND W32API GetParent(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (w)
    return w->parent ? w->parent : w->owner;
  struct w32r_window v;
  return u32_directory_window(h, &v, 0) ? (HWND)(uintptr_t)v.parent : 0;
}
HWND W32API SetParent(HWND h, HWND parent) {
  U32_GUARD;
  if (parent == U32_DESKTOP)
    parent = 0;
  struct u32_window *w = u32_window(h);
  if (!w || (parent && !u32_window(parent)) || h == parent ||
      IsChild(h, parent)) {
    u32_fail(87);
    return 0;
  }
  HWND old = w->parent;
  BOOL was_top = !w->parent;
  if (was_top && parent) {
    u32_destroy_paint(w);
    /* q3exec3: as in u32_destroy, nothing names the old wire id after its
     * DESK_CLOSE (no queued frame update, no PING answer). */
    w->damage.n = 0;
    w->ping = 0;
    w->closing = 1;
    u32_wire(w, DESK_CLOSE, 0, 0, -1);
    u32_backend.drop_surface(&w->surface);
    w->wire = 0;
    w->closing = 0;
  }
  w->parent = parent;
  if (!parent && !was_top && !u32_create_wire(w)) {
    w->parent = old;
    return 0;
  }
  u32_sync_window(w);
  InvalidateRect(h, 0, 1);
  return old;
}
LONG W32API GetWindowLongA(HWND h, int index) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w) {
    u32_fail(1400);
    return 0;
  }
  switch (index) {
  case GWL_WNDPROC:
    return (LONG)u32_proc_handle(w->proc, w->unicode, 0);
  case GWL_HINSTANCE:
    return (LONG)(intptr_t)w->instance;
  case GWL_HWNDPARENT:
    return (LONG)(intptr_t)GetParent(h);
  case GWL_ID:
    return w->id;
  case GWL_STYLE:
    return w->style;
  case GWL_EXSTYLE:
    return w->exstyle;
  case GWL_USERDATA:
    return w->userdata;
  default: {
    LONG value = 0;
    if (u32_dialog_long(h, index, &value, 0))
      return value;
    if (index >= 0 && index <= w->cls->c.cbWndExtra - 4)
      return w32_u32(w->extra + index);
    u32_fail(1413);
    return 0;
  }
  }
}
LONG W32API SetWindowLongA(HWND h, int index, LONG value) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w) {
    u32_fail(1400);
    return 0;
  }
  LONG old = GetWindowLongA(h, index);
  switch (index) {
  case GWL_WNDPROC: {
    BOOL wide;
    w->proc = u32_proc_decode((uintptr_t)(DWORD)value, 0, &wide);
    w->unicode = wide;
    break;
  }
  case GWL_HINSTANCE:
    w->instance = (HINSTANCE)(intptr_t)value;
    break;
  case GWL_HWNDPARENT:
    if (w->parent)
      SetParent(h, (HWND)(intptr_t)value);
    else {
      HWND owner = (HWND)(uintptr_t)(DWORD)value;
      struct u32_window *o = u32_window(owner);
      if (owner && (!o || o->parent || o->pid != w->pid)) return u32_fail(87);
      for (struct u32_window *q = o; q; q = u32_window(q->owner))
        if (q == w) return u32_fail(87);
      if (!u32_order_wire(w, 0, owner, DESK_V3_ORDER_OWNER)) return 0;
      w->owner = owner;
      u32_restack(w, HWND_TOP);
    }
    break;
  case GWL_ID:
    w->id = value;
    break;
  case GWL_STYLE:
    w->style = value;
    u32_sync_window(w);
    if (!w->parent)
      u32_place(w);
    break;
  case GWL_EXSTYLE:
    if (!w->parent && ((w->exstyle^(DWORD)value)&0x10u)) {
      uint8_t b[4]; desk_put32(b,!!((DWORD)value&0x10u));
      if (!u32_wire(w,DESK_ACCEPT_FILES,b,4,-1)) return 0;
    }
    w->exstyle = value;
    if (!w->parent)
      u32_place(w);
    break;
  case GWL_USERDATA:
    w->userdata = value;
    break;
  default: {
    LONG v = value;
    if (u32_dialog_long(h, index, &v, 1))
      break;
    if (index < 0 || index > w->cls->c.cbWndExtra - 4) {
      u32_fail(1413);
      return 0;
    }
    w32_put32(w->extra + index, value);
  }
  }
  return old;
}
uintptr_t W32API GetClassLongA(HWND h, int index) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w) {
    u32_fail(1400);
    return 0;
  }
  struct u32_class *c = w->cls;
  switch (index) {
  case -32:
    return c->atom;
  case -26:
    return c->c.style;
  case -24:
    return u32_proc_handle(c->c.lpfnWndProc, c->unicode, 0);
  case -20:
    return c->c.cbClsExtra;
  case -18:
    return c->c.cbWndExtra;
  case -16:
    return (uintptr_t)c->c.hInstance;
  case -14:
    return (uintptr_t)c->c.hIcon;
  case -12:
    return (uintptr_t)c->c.hCursor;
  case -10:
    return (uintptr_t)c->c.hbrBackground;
  case -34:
    return (uintptr_t)c->c.hIconSm;
  default:
    if (index >= 0 && index <= c->c.cbClsExtra - 4)
      return w32_u32(c->extra + index);
    u32_fail(1413);
    return 0;
  }
}
uintptr_t W32API SetClassLongA(HWND h, int index, intptr_t value) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  uintptr_t old = GetClassLongA(h, index);
  struct u32_class *c = w->cls;
  switch (index) {
  case -26:
    c->c.style = value;
    break;
  case -24: {
    BOOL wide;
    c->c.lpfnWndProc = u32_proc_decode((uintptr_t)value, 0, &wide);
    c->unicode = wide;
    break;
  }
  case -14:
    c->c.hIcon = (HICON)value;
    break;
  case -12:
    c->c.hCursor = (HCURSOR)value;
    break;
  case -10:
    c->c.hbrBackground = (HBRUSH)value;
    break;
  case -34:
    c->c.hIconSm = (HICON)value;
    break;
  default:
    if (index < 0 || index > c->c.cbClsExtra - 4)
      return u32_fail(1413);
    w32_put32(c->extra + index, value);
  }
  if (index == -14 || index == -34) /* fx-exeicons: the class's windows show the new icon */
    for (UINT k = 0; k < U32_WINDOWS; k++)
      if (u32_windows[k].handle && u32_windows[k].cls == c && !u32_windows[k].parent)
        (void)u32_window_icon(&u32_windows[k]);
  return old;
}
uint16_t W32API GetWindowWord(HWND h, int n) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || n < 0 || n > w->cls->c.cbWndExtra - 2)
    return (uint16_t)GetWindowLongA(h, n);
  return w32_u16(w->extra + n);
}
uint16_t W32API SetWindowWord(HWND h, int n, uint16_t v) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || n < 0 || n > w->cls->c.cbWndExtra - 2)
    return (uint16_t)SetWindowLongA(h, n, v);
  uint16_t old = w32_u16(w->extra + n);
  w32_put16(w->extra + n, v);
  return old;
}
/* The stored title; GetWindowText sends WM_GETTEXT to the process's own
 * windows (controls keep their text), DefWindowProc reads this. */
static int title_length(HWND h) {
  struct u32_window *w = u32_window(h);
  if (w)
    return w32_strlen(w->title);
  char t[256];
  struct w32r_window v;
  return u32_directory_window(h, &v, t) ? w32_strlen(t) : 0;
}
static int title_copy(HWND h, char *out, int n);
int W32API GetWindowTextLengthA(HWND h) {
  U32_GUARD;
  if (u32_window(h))
    return (int)SendMessageA(h, WM_GETTEXTLENGTH, 0, 0);
  return title_length(h);
}
int W32API GetWindowTextA(HWND h, char *out, int n) {
  U32_GUARD;
  if (!out || n <= 0)
    return 0;
  if (u32_window(h)) {
    out[0] = 0;
    return (int)SendMessageA(h, WM_GETTEXT, (WPARAM)n, (LPARAM)out);
  }
  return title_copy(h, out, n);
}
static int title_copy(HWND h, char *out, int n) {
  if (!out || n <= 0)
    return 0;
  struct u32_window *w = u32_window(h);
  char title[256];
  struct w32r_window v;
  const char *s = w ? w->title : title;
  if (!w && !u32_directory_window(h, &v, title))
    return 0;
  UINT count = w32_strlen(s);
  if (count >= (UINT)n)
    count = n - 1;
  w32_copy(out, s, count);
  out[count] = 0;
  return count;
}
BOOL W32API SetWindowTextA(HWND h, const char *text) {
  U32_GUARD;
  return !!SendMessageA(h, WM_SETTEXT, 0, (LPARAM)(text ? text : ""));
}
int W32API GetClassNameA(HWND h, char *out, int n) {
  U32_GUARD;
  if (!out || n <= 0)
    return 0;
  if (h == U32_DESKTOP) {
    static const char desktop[] = "#32769";
    int k = 0;
    for (; desktop[k] && k + 1 < n; k++)
      out[k] = desktop[k];
    out[k] = 0;
    return k;
  }
  struct u32_window *w = u32_window(h);
  if (w) {
    UINT len = w32_strlen(w->cls->name);
    if (len >= (UINT)n)
      len = n - 1;
    w32_copy(out, w->cls->name, len);
    out[len] = 0;
    return len;
  }
  struct w32r_reply reply;
  char name[1021];
  if (!u32_directory(W32R_WINDOW_CLASS, h, 0, 0, 0, "", 0, 0, &reply, name,
                     sizeof(name)))
    return 0;
  UINT at = 0;
  const uint8_t *p = (void *)name;
  while (*p && at + 1 < (UINT)n) {
    unsigned c = *p++;
    if (c >= 0xe0) {
      c &= 15;
      c = (c << 6) | (*p++ & 63);
      c = (c << 6) | (*p++ & 63);
    } else if (c >= 0xc0) {
      c &= 31;
      c = (c << 6) | (*p++ & 63);
    }
    out[at++] = u32_ansi(c);
  }
  out[at] = 0;
  return at;
}
DWORD W32API GetWindowThreadProcessId(HWND h, DWORD *pid) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  struct w32r_window v;
  if (w) {
    if (pid)
      *pid = w->pid;
    return w->tid;
  }
  if (!u32_directory_window(h, &v, 0))
    return 0;
  if (pid)
    *pid = v.pid;
  return v.tid;
}
static UINT list(HWND p, HWND out[U32_WINDOWS], BOOL descendants) {
  UINT n = 0;
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    if (w->handle && (descendants ? IsChild(p, w->handle) : w->parent == p))
      out[n++] = w->handle;
  }
  for (UINT i = 0; i < n; i++)
    for (UINT j = i + 1; j < n; j++) {
      struct u32_window *a = u32_window(out[i]), *b = u32_window(out[j]);
      BOOL atop = !!(a->exstyle & WS_EX_TOPMOST),
           btop = !!(b->exstyle & WS_EX_TOPMOST);
      if (btop > atop || (btop == atop && b->z > a->z)) {
        HWND h = out[i];
        out[i] = out[j];
        out[j] = h;
      }
    }
  return n;
}
static UINT directory_list(HWND *hs, BOOL all) {
  UINT n = 0;
  for (; n < 4096; n++) {
    struct w32r_reply r;
    uint8_t data[1045];
    if (!u32_directory(W32R_WINDOW_ENUM, 0, n, 0, all ? 1 : 0, "", 0, 0, &r,
                       data, sizeof(data))) {
      if (GetLastError() != 259)
        return UINT32_MAX;
      break;
    }
    hs[n] = (HWND)(uintptr_t)r.key;
  }
  return n;
}
BOOL W32API EnumWindows(WNDENUMPROC proc, LPARAM l) {
  U32_GUARD;
  if (!proc)
    return u32_fail(87);
  HWND *hs = u32_alloc(4096 * sizeof(*hs));
  if (!hs)
    return 0;
  UINT n = directory_list(hs, 0);
  BOOL ok = n != UINT32_MAX;
  if (ok)
    for (UINT i = 0; i < n; i++)
      if (IsWindow(hs[i])) {
        UINT saved = u32_suspend();
        BOOL more = proc(hs[i], l);
        u32_resume(saved);
        if (!more) {
          ok = 0;
          break;
        }
      }
  u32_free(hs);
  return ok;
}
BOOL W32API EnumChildWindows(HWND parent, WNDENUMPROC proc, LPARAM l) {
  U32_GUARD;
  if (!proc)
    return u32_fail(87);
  if (parent == U32_DESKTOP)
    return EnumWindows(proc, l);
  HWND *hs = u32_alloc(4096 * sizeof(*hs));
  if (!hs)
    return 0;
  UINT n = directory_list(hs, 1);
  BOOL ok = n != UINT32_MAX;
  if (ok)
    for (UINT i = 0; i < n; i++)
      if (IsChild(parent, hs[i])) {
        UINT saved = u32_suspend();
        BOOL more = proc(hs[i], l);
        u32_resume(saved);
        if (!more) {
          ok = 0;
          break;
        }
      }
  u32_free(hs);
  return ok;
}
HWND W32API FindWindowExA(HWND parent, HWND after, const char *klass,
                          const char *title) {
  U32_GUARD;
  HWND *hs = u32_alloc(4096 * sizeof(*hs));
  if (!hs)
    return 0;
  UINT n = directory_list(hs, !!parent);
  BOOL skip = !!after;
  HWND result = 0;
  if (n != UINT32_MAX)
    for (UINT i = 0; i < n; i++) {
      struct w32r_window value;
      char text[256];
      if (!u32_directory_window(hs[i], &value, text) ||
          value.parent != U32_PTR(parent))
        continue;
      if (skip) {
        if (hs[i] == after)
          skip = 0;
        continue;
      }
      if (title && !equal(text, title))
        continue;
      if (klass) {
        if (atom_name(klass)) {
          if (value.class_atom != (uintptr_t)klass)
            continue;
        } else {
          char name[256];
          if (!GetClassNameA(hs[i], name, sizeof(name)) || !equal(name, klass))
            continue;
        }
      }
      result = hs[i];
      break;
    }
  u32_free(hs);
  return result;
}
HWND W32API FindWindowA(const char *c, const char *t) {
  U32_GUARD;
  return FindWindowExA(0, 0, c, t);
}
HWND W32API GetTopWindow(HWND p) {
  U32_GUARD;
  if (p == U32_DESKTOP)
    p = 0;
  HWND hs[U32_WINDOWS];
  return list(p, hs, 0) ? hs[0] : 0;
}
HWND W32API GetWindow(HWND h, UINT cmd) {
  U32_GUARD;
  if (h == U32_DESKTOP)
    return cmd == 5 ? GetTopWindow(0) : 0; /* GW_CHILD: the top window */
  struct u32_window *w = u32_window(h);
  if (!w)
    return 0;
  if (cmd == 4)
    return w->owner;
  if (cmd == 5)
    return GetTopWindow(h);
  HWND hs[U32_WINDOWS];
  UINT n = list(w->parent, hs, 0);
  if (!n)
    return 0;
  if (cmd == 0)
    return hs[0];
  if (cmd == 1)
    return hs[n - 1];
  for (UINT i = 0; i < n; i++)
    if (hs[i] == h) {
      if (cmd == 2 && i + 1 < n)
        return hs[i + 1];
      if (cmd == 3 && i)
        return hs[i - 1];
    }
  return 0;
}
HWND W32API GetLastActivePopup(HWND h) {
  U32_GUARD;
  for (UINT i = 0; i < U32_WINDOWS; i++)
    if (u32_windows[i].owner == h && u32_windows[i].handle == u32_foreground)
      return u32_foreground;
  return h;
}
BOOL W32API BringWindowToTop(HWND h) {
  U32_GUARD;
  return SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}
BOOL W32API ShowOwnedPopups(HWND h, BOOL show) {
  U32_GUARD;
  for (UINT i = 0; i < U32_WINDOWS; i++)
    if (u32_windows[i].handle && u32_windows[i].owner == h)
      ShowWindow(u32_windows[i].handle, show ? 8 : 0);
  return 1;
}
static struct u32_prop **property(struct u32_window *w, const char *n) {
  struct u32_prop **p = &w->props;
  while (*p && (atom_name(n) ? (*p)->atom != (uintptr_t)n
                             : !(*p)->name || !equal((*p)->name, n)))
    p = &(*p)->next;
  return p;
}
BOOL W32API SetPropA(HWND h, const char *name, HANDLE value) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || !name)
    return u32_fail(1400);
  struct u32_prop **at = property(w, name);
  if (!*at) {
    struct u32_prop *p = u32_alloc(sizeof(*p));
    if (!p)
      return 0;
    if (atom_name(name))
      p->atom = (ATOM)(uintptr_t)name;
    else {
      DWORD n = w32_strlen(name);
      if (n > 255) {
        u32_free(p);
        return u32_fail(87);
      }
      p->name = u32_alloc(n + 1);
      if (!p->name) {
        u32_free(p);
        return 0;
      }
      w32_copy(p->name, name, n + 1);
    }
    *at = p;
  }
  (*at)->value = value;
  return 1;
}
HANDLE W32API GetPropA(HWND h, const char *name) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || !name)
    return 0;
  struct u32_prop *p = *property(w, name);
  return p ? p->value : 0;
}
HANDLE W32API RemovePropA(HWND h, const char *name) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || !name)
    return 0;
  struct u32_prop **at = property(w, name), *p = *at;
  if (!p)
    return 0;
  HANDLE value = p->value;
  *at = p->next;
  u32_free(p->name);
  u32_free(p);
  return value;
}
LRESULT W32API DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  /* fx-hlrun: WH_CBT HCBT_SYSCOMMAND; a nonzero result skips the command. */
  if (msg == WM_SYSCOMMAND && u32_hooks(5, 8 /* HCBT_SYSCOMMAND */, wp, lp))
    return 0;
  BOOL classic = 0;
  LRESULT result = u32_classic_default(h, msg, wp, lp, &classic);
  if (classic)
    return result;
  switch (msg) {
  case WM_DESTROYCLIPBOARD:
    u32_clipboard_owned_clear();
    return 0;
  case 0x11:  /* WM_QUERYENDSESSION */
  case 0x218: /* G11 WM_POWERBROADCAST: TRUE, a query is granted */
  case WM_NCCREATE:
    return 1;
  case WM_CLOSE:
    return DestroyWindow(h), 0;
  case WM_PAINT: {
    PAINTSTRUCT ps;
    if (BeginPaint(h, &ps))
      EndPaint(h, &ps);
    return 0;
  }
  case 0x0088: /* WM_SYNCPAINT: erase what is pending now (Wine 10.0 defwnd.c) */
    RedrawWindow(h, 0, 0, 0x200 /* RDW_ERASENOW */ | 4 /* RDW_ERASE */ | 0x80 /* RDW_ALLCHILDREN */);
    return 0;
  case WM_ERASEBKGND:
    if (w && w->cls->c.hbrBackground && u32_gdi && u32_gdi->fill) {
      RECT r;
      GetClientRect(h, &r);
      return u32_gdi->fill((HDC)wp, &r, w->cls->c.hbrBackground);
    }
    return 0;
  case WM_SETREDRAW:
    if (w) {
      w->redraw = !!wp;
      if (wp)
        InvalidateRect(h, 0, 1);
      u32_mark();
    }
    return 0;
  case 0x0080: { /* WM_SETICON: ICON_SMALL 0, ICON_BIG 1; the previous icon */
    if (!w || wp > 1)
      return 0;
    HICON old = wp ? w->icon_big : w->icon_small;
    if (wp)
      w->icon_big = (HICON)lp;
    else
      w->icon_small = (HICON)lp;
    (void)u32_window_icon(w);
    return (LRESULT)old;
  }
  case 0x007f: /* WM_GETICON: ICON_SMALL2 2 is the small icon or the one made from the big */
    if (!w)
      return 0;
    return (LRESULT)(wp == 1 ? w->icon_big : wp == 0 ? w->icon_small : wp == 2 ? (w->icon_small ? w->icon_small : w->icon_big) : 0);
  case WM_GETTEXT:
    return title_copy(h, (char *)lp, (int)wp);
  case WM_GETTEXTLENGTH:
    return title_length(h);
  case WM_SETTEXT: {
    if (!w || !lp || w32_strlen((char *)lp) > 255)
      return u32_fail(87);
    char old[256];
    w32_copy(old, w->title, 256);
    w32_copy(w->title, (char *)lp, w32_strlen((char *)lp) + 1);
    if (!u32_sync_window(w)) {
      w32_copy(w->title, old, 256);
      return 0;
    }
    if (w->wire) {
      uint8_t b[68];
      UINT n = u32_utf8((char *)b + 4, 64, w->title);
      desk_put32(b, n);
      return u32_wire(w, DESK_SET_TITLE, b, n + 4, -1);
    }
    return 1;
  }
  case WM_SETCURSOR:
    if (u32_help_mode()) {
      SetCursor((HCURSOR)(uintptr_t)32651u);   /* G15: What's This? keeps the help arrow over every window */
      return 1;
    }
    if (w && w->cls->c.hCursor)
      SetCursor(w->cls->c.hCursor);
    return 1;
  case WM_SYSCOMMAND:
    switch (wp & 0xfff0) {
    case SC_SCREENSAVE:
      u32_saver_command(SC_SCREENSAVE,-1);
      break;
    case SC_MONITORPOWER:
      u32_saver_command(SC_MONITORPOWER,(int)lp);
      break;
    case 0xf060:
      SendMessageA(h, WM_CLOSE, 0, 0);
      break;
    case 0xf020:
      ShowWindow(h, 6);
      break;
    case 0xf120:
      ShowWindow(h, 9);
      break;
    case 0xf030:
      ShowWindow(h, 3);
      break;
    }
    return 0;
  default:
    return 0;
  }
}

/* G15: the help topic number a window reports in HELPINFO.dwContextId
 * (SetWindowContextHelpId / GetWindowContextHelpId, user32 exports). */
BOOL W32API SetWindowContextHelpId(HWND h, DWORD id) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  w->help_id = id;
  return 1;
}
DWORD W32API GetWindowContextHelpId(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w) {
    u32_fail(1400);
    return 0;
  }
  return w->help_id;
}
