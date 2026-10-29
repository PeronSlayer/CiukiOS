/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "classic.h"
static struct u32_dc dcs[64];
static DWORD next_frame;
HWND u32_dc_window(HDC dc) {
  for (UINT i = 0; i < 64; i++) if (dcs[i].dc == dc) return dcs[i].hwnd;
  return 0;
}
static void piece(struct u32_region *out, RECT r) {
  if (IsRectEmpty(&r))
    return;
  if (out->n < U32_RECTS)
    out->r[out->n++] = r;
  else {
    RECT box = r;
    for (UINT i = 0; i < out->n; i++)
      UnionRect(&box, &box, &out->r[i]);
    out->n = 1;
    out->r[0] = box;
  }
}
void u32_region_sub(struct u32_region *region, RECT cut) {
  struct u32_region result = {0};
  for (UINT i = 0; i < region->n; i++) {
    RECT r = region->r[i], x;
    if (!IntersectRect(&x, &r, &cut)) {
      piece(&result, r);
      continue;
    }
    piece(&result, (RECT){r.left, r.top, r.right, x.top});
    piece(&result, (RECT){r.left, x.bottom, r.right, r.bottom});
    piece(&result, (RECT){r.left, x.top, x.left, x.bottom});
    piece(&result, (RECT){x.right, x.top, r.right, x.bottom});
  }
  *region = result;
}
void u32_region_add(struct u32_region *r, RECT add) {
  if (IsRectEmpty(&add))
    return;
  struct u32_region pending = {.r = {add}, .n = 1};
  for (UINT i = 0; i < r->n && pending.n; i++)
    u32_region_sub(&pending, r->r[i]);
  for (UINT i = 0; i < pending.n; i++)
    piece(r, pending.r[i]);
  /* Merge only exact rectangular unions, never holes between damage. */
  for (UINT i = 0; i < r->n; i++)
    for (UINT j = i + 1; j < r->n;) {
      RECT a = r->r[i], b = r->r[j];
      BOOL merge = (a.left == b.left && a.right == b.right &&
                    (a.bottom == b.top || b.bottom == a.top)) ||
                   (a.top == b.top && a.bottom == b.bottom &&
                    (a.right == b.left || b.right == a.left));
      if (merge) {
        UnionRect(&r->r[i], &a, &b);
        r->r[j] = r->r[--r->n];
      } else
        j++;
    }
}
BOOL W32API InvalidateRect(HWND h, const RECT *rect, BOOL erase) {
  U32_GUARD;
  if (!h) {
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].handle)
        InvalidateRect(u32_windows[i].handle, rect, erase);
    return 1;
  }
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  RECT client, r;
  GetClientRect(h, &client);
  if (IntersectRect(&r, rect ? rect : &client, &client)) {
    u32_region_add(&w->update, r);
    /* Without WS_CLIPCHILDREN the parent paints over its children, which
     * are invalidated too (classic USER behaviour). */
    if (!(w->style & WS_CLIPCHILDREN))
      u32_invalidate_children(w, r, erase);
  }
  if (erase)
    w->erase = U32_ERASE_PENDING;
  u32_mark();
  return 1;
}
void u32_invalidate_children(struct u32_window *w, RECT r, BOOL erase) {
  POINT po = u32_origin(w);
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *c = &u32_windows[i];
    if (!c->handle || c->parent != w->handle || !(c->style & WS_VISIBLE))
      continue;
    RECT window, hit;
    GetWindowRect(c->handle, &window);
    OffsetRect(&window, -po.x, -po.y);
    if (!IntersectRect(&hit, &r, &window))
      continue;
    POINT co = u32_origin(c);
    OffsetRect(&hit, po.x - co.x, po.y - co.y);
    InvalidateRect(c->handle, &hit, erase);
    /* A child border may lie outside its client rectangle. */
    if (c->style & (WS_BORDER | WS_DLGFRAME | 0x00300000u /* scroll bars */) ||
        c->exstyle & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE))
      c->internal_paint = 1;
  }
}
BOOL W32API ValidateRect(HWND h, const RECT *rect) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (rect)
    u32_region_sub(&w->update, *rect);
  else
    w->update.n = 0;
  if (!w->update.n)
    w->erase = 0;
  u32_mark();
  return 1;
}
BOOL W32API InvalidateRgn(HWND h, HRGN region, BOOL erase) {
  U32_GUARD;
  if (!region)
    return InvalidateRect(h, 0, erase);
  if (!u32_gdi || !u32_gdi->region_rects)
    return u32_fail(120);
  RECT r[U32_RECTS];
  UINT n = u32_gdi->region_rects(region, r, U32_RECTS);
  if (n > U32_RECTS)
    return u32_fail(8);
  for (UINT i = 0; i < n; i++)
    InvalidateRect(h, &r[i], erase);
  return 1;
}
BOOL W32API ValidateRgn(HWND h, HRGN region) {
  U32_GUARD;
  if (!region)
    return ValidateRect(h, 0);
  if (!u32_gdi || !u32_gdi->region_rects)
    return u32_fail(120);
  RECT r[U32_RECTS];
  UINT n = u32_gdi->region_rects(region, r, U32_RECTS);
  if (n > U32_RECTS)
    return u32_fail(8);
  for (UINT i = 0; i < n; i++)
    ValidateRect(h, &r[i]);
  return 1;
}
BOOL W32API GetUpdateRect(HWND h, RECT *rect, BOOL erase) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  RECT box = {0};
  for (UINT i = 0; i < w->update.n; i++)
    UnionRect(&box, &box, &w->update.r[i]);
  if (rect)
    *rect = box;
  /* The answer is the region as found; an erase handler may validate it
   * (Wine 10.0 win32u/dce.c NtUserGetUpdateRect returns UPDATE_PAINT from
   * before send_erase). */
  BOOL pending = w->update.n != 0;
  if (erase)
    u32_erase_window(h);
  return pending;
}
int W32API GetUpdateRgn(HWND h, HRGN region, BOOL erase) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (!u32_gdi || !u32_gdi->region_set ||
      !u32_gdi->region_set(region, w->update.r, w->update.n))
    return 0;
  if (erase)
    GetUpdateRect(h, 0, 1);
  return w->update.n > 1 ? 3 : w->update.n ? 2 : 1;
}
static HDC acquire(HWND h, BOOL frame, const struct u32_region *paint) {
  u32_gdi_ensure();
  if (!h || h == U32_DESKTOP) {
    /* f5-20B: an application has no access to the composed screen; its
     * screen DC (GetDC(NULL), the desktop window) is a memory DC with the
     * display's device capabilities, for measuring and compatible bitmaps.
     * Drawing on it is discarded. */
    const struct u32_graphics *g = u32_graphics();
    struct u32_dc *slot = 0;
    for (UINT i = 0; i < 64 && !slot; i++)
      if (!dcs[i].dc)
        slot = &dcs[i];
    if (!g || !g->memory_dc || !slot) {
      u32_fail(slot ? 120 : 8);
      return 0;
    }
    /* fx-hlrun: GDI's display DC, so compatible bitmaps take the
     * display's format (a plain memory DC would make them monochrome). */
    HDC dc = (HDC)(uintptr_t)(g->display_dc ? g->display_dc() : g->memory_dc(0));
    if (!dc)
      return 0;
    *slot = (struct u32_dc){dc, h, 0, 0, {0}};
    return dc;
  }
  struct u32_window *w = u32_window(h), *root = u32_root(w);
  if (!w || !root || !root->surface.pixels) {
    u32_fail(1400);
    return 0;
  }
  if (!u32_gdi || !u32_gdi->acquire) {
    u32_fail(120);
    return 0;
  }
  struct u32_dc *slot = 0;
  for (UINT i = 0; i < 64; i++)
    if (!dcs[i].dc) {
      slot = &dcs[i];
      break;
    }
  if (!slot) {
    u32_fail(8);
    return 0;
  }
  POINT ro = u32_surface_origin(root), o = u32_origin(w);
  o.x -= ro.x;
  o.y -= ro.y;
  RECT client;
  GetClientRect(h, &client);
  struct u32_region include =
      paint ? *paint : (struct u32_region){.r = {client}, .n = 1};
  /* q3exec4: a hidden child (or the child of a hidden window) has an empty
   * visible region: what it draws is discarded, as on Windows. */
  if (!u32_paintable(w))
    include.n = 0;
  for (UINT i = 0; i < include.n; i++)
    OffsetRect(&include.r[i], o.x, o.y);
  for (struct u32_window *parent = u32_window(w->parent); parent;
       parent = u32_window(parent->parent)) {
    RECT bounds;
    GetClientRect(parent->handle, &bounds);
    POINT po = u32_origin(parent);
    OffsetRect(&bounds, po.x - ro.x, po.y - ro.y);
    for (UINT i = 0; i < include.n; i++) {
      RECT hit;
      IntersectRect(&hit, &include.r[i], &bounds);
      include.r[i] = hit;
    }
  }
  RECT bounds = {0, 0, root->surface.width, root->surface.height};
  for (UINT i = 0; i < include.n; i++) {
    RECT intersection;
    IntersectRect(&intersection, &include.r[i], &bounds);
    include.r[i] = intersection;
  }
  RECT exclude[U32_WINDOWS];
  UINT n = 0;
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *q = &u32_windows[i];
    if (!q->handle || !IsWindowVisible(q->handle))
      continue;
    if (((w->style & WS_CLIPCHILDREN) && q->parent == h) ||
        ((w->style & WS_CLIPSIBLINGS) && q->parent == w->parent &&
         q->z > w->z)) {
      RECT r;
      GetWindowRect(q->handle, &r);
      OffsetRect(&r, -ro.x, -ro.y);
      exclude[n++] = r;
    }
  }
  /* The desktop owns non-client rendering. Its window DC clips to client. */
  (void)frame;
  HDC dc =
      u32_gdi->acquire(&root->surface, o, include.r, include.n, exclude, n);
  if (!dc)
    return 0;
  if (u32_gdi->version >= 2 && u32_gdi->window_pixel_format && !u32_gdi->window_pixel_format(dc, &w->pixel_format)) {
    RECT ignored[U32_RECTS];
    u32_gdi->release(dc, ignored, U32_RECTS);
    return 0;
  }
  *slot = (struct u32_dc){dc, h, root->handle, !!paint, {0}};
  root->dcs++;
  return dc;
}
HDC W32API GetDC(HWND h) {
  U32_GUARD;
  return acquire(h, 0, 0);
}
HDC W32API GetWindowDC(HWND h) {
  U32_GUARD;
  return acquire(h, 1, 0);
}
HDC W32API GetDCEx(HWND h, HRGN region, DWORD flags) {
  U32_GUARD;
  if (!region)
    return acquire(h, !!(flags & 1), 0);
  if (!u32_gdi || !u32_gdi->region_rects) {
    u32_fail(120);
    return 0;
  }
  struct u32_region r = {0};
  r.n = u32_gdi->region_rects(region, r.r, U32_RECTS);
  if (r.n > U32_RECTS) {
    u32_fail(8);
    return 0;
  }
  if (flags & 0x80) {
    RECT c;
    GetClientRect(h, &c);
    struct u32_region all = {.r = {c}, .n = 1};
    for (UINT i = 0; i < r.n; i++)
      u32_region_sub(&all, r.r[i]);
    r = all;
  }
  return acquire(h, !!(flags & 1), &r);
}
int W32API ReleaseDC(HWND h, HDC dc) {
  U32_GUARD;
  for (UINT i = 0; i < 64; i++)
    if (dcs[i].dc == dc && !dcs[i].root &&
        (dcs[i].hwnd == h || !h || h == U32_DESKTOP)) {
      const struct u32_graphics *g = u32_graphics();
      if (g && g->delete_dc)
        g->delete_dc(U32_PTR(dc));
      w32_fill(&dcs[i], 0, sizeof(dcs[i]));
      return 1;
    }
  for (UINT i = 0; i < 64; i++)
    if (dcs[i].dc == dc && dcs[i].hwnd == h) {
      struct u32_window *root = u32_window(dcs[i].root);
      RECT touched[U32_RECTS];
      UINT n = u32_gdi && u32_gdi->release
                   ? u32_gdi->release(dc, touched, U32_RECTS)
                   : 0;
      if (n > U32_RECTS)
        n = 0;
      for (UINT j = 0; root && j < n; j++)
        u32_region_add(&root->damage, touched[j]);
      if (root && root->dcs)
        root->dcs--;
      w32_fill(&dcs[i], 0, sizeof(dcs[i]));
      if (root)
        u32_resize(root);
      u32_flush();
      return 1;
    }
  return 0;
}
/* fx-hlrun (Half-Life's OpenGL renderer imports it): the window a DC from
 * GetDC/GetWindowDC/GetDCEx/BeginPaint belongs to until ReleaseDC; the
 * screen DC of GetDC(NULL) belongs to the desktop window; any other DC
 * (memory, metafile, printer, released) has none (Windows 98 USER keeps the
 * window in its DCE entry; Wine 10.0 dlls/win32u/dce.c NtUserWindowFromDC). */
HWND W32API WindowFromDC(HDC dc) {
  U32_GUARD;
  for (UINT i = 0; dc && i < 64; i++)
    if (dcs[i].dc == dc)
      return dcs[i].hwnd ? dcs[i].hwnd : U32_DESKTOP;
  return 0;
}
/* fx-hlrun (Half-Life's launcher and Sierra's MFC utilities import it): take
 * the window's pending update region out of the DC's clipping region, so a
 * program drawing outside WM_PAINT leaves to WM_PAINT what it will repaint
 * anyway; the result is the complexity of the new clipping region, ERROR (0)
 * for a bad window. Microsoft ExcludeUpdateRgn; Wine 10.0 win32u/dce.c
 * NtUserExcludeUpdateRgn (ExtSelectClipRgn RGN_DIFF with the update region in
 * the DC's client coordinates; a DC of the window has its origin there). */
extern HANDLE W32API GetModuleHandleA(const char *);
extern void *W32API GetProcAddress(HANDLE, const char *);
int W32API ExcludeUpdateRgn(HDC dc, HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || !dc) {
    u32_fail(1400);
    return 0;
  }
  HANDLE gdi = GetModuleHandleA("gdi32.dll");
  int(W32API * exclude)(HDC, int, int, int, int) = gdi ? GetProcAddress(gdi, "ExcludeClipRect") : 0;
  int(W32API * clip_box)(HDC, RECT *) = gdi ? GetProcAddress(gdi, "GetClipBox") : 0;
  if (!exclude || !clip_box) {
    u32_fail(120);
    return 0;
  }
  RECT box;
  int type = clip_box(dc, &box);
  for (UINT i = 0; type && i < w->update.n; i++)
    type = exclude(dc, w->update.r[i].left, w->update.r[i].top, w->update.r[i].right, w->update.r[i].bottom);
  return type;
}
HDC W32API BeginPaint(HWND h, PAINTSTRUCT *ps) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || !ps) {
    u32_fail(87);
    return 0;
  }
  struct u32_region snapshot = w->update;
  u32_caret_paint(h, 0);
  u32_paint_frame(h);
  HDC dc = acquire(h, 0, &snapshot);
  if (!dc) {
    u32_caret_paint(h, (HDC)(uintptr_t)1);
    return 0;
  }
  w32_fill(ps, 0, sizeof(*ps));
  ps->hdc = dc;
  for (UINT i = 0; i < snapshot.n; i++)
    UnionRect(&ps->rcPaint, &ps->rcPaint, &snapshot.r[i]);
  w->update.n = 0;
  /* fx-hlrun: what a child paints is no longer pending in the ancestors
   * that draw over their children (no WS_CLIPCHILDREN). Windows removes the
   * child's update region from theirs when the child begins painting (Wine
   * 10.0 server/window.c validate_parents, from get_update_region for
   * BeginPaint), so a dialog that refreshes a child in its WM_PAINT and then
   * paints its whole background leaves the child's pixels alone (Half-Life's
   * launcher dialogs). A child whose whole client area is repainted also
   * takes its frame out of the ancestors' regions. */
  POINT child_origin = u32_origin(w);
  RECT client_area, whole;
  GetClientRect(h, &client_area);
  GetWindowRect(h, &whole);
  BOOL full = snapshot.n && EqualRect(&ps->rcPaint, &client_area);
  for (struct u32_window *p = u32_window(w->parent); p && snapshot.n; p = u32_window(p->parent)) {
    if ((p->style & WS_CLIPCHILDREN) || !p->update.n)
      continue;
    POINT po = u32_origin(p);
    for (UINT i = 0; i < snapshot.n; i++) {
      RECT r = snapshot.r[i];
      OffsetRect(&r, child_origin.x - po.x, child_origin.y - po.y);
      u32_region_sub(&p->update, r);
    }
    if (full) {
      RECT frame = whole;
      OffsetRect(&frame, -po.x, -po.y);
      u32_region_sub(&p->update, frame);
    }
  }
  w->internal_paint = 0;
  /* fx-hlrun: a background still to erase gets WM_ERASEBKGND here; one
   * whose earlier erase (u32_erase_window) was not handled only reports
   * fErase (Wine 10.0 server PAINT_DELAYED_ERASE, win32u send_erase). */
  UINT erase = w->erase;
  w->erase = 0;
  if (erase == U32_ERASE_PENDING)
    ps->fErase = !SendMessageA(h, WM_ERASEBKGND, (WPARAM)dc, 0);
  else if (erase == U32_ERASE_DELAYED)
    ps->fErase = 1;
  u32_mark();
  return dc;
}
BOOL W32API EndPaint(HWND h, const PAINTSTRUCT *ps) {
  U32_GUARD;
  if (!ps)
    return u32_fail(87);
  BOOL ok = ReleaseDC(h, ps->hdc);
  u32_caret_paint(h, (HDC)(uintptr_t)1);
  return ok;
}
void u32_destroy_paint(struct u32_window *w) {
  for (UINT i = 0; i < 64; i++)
    if (dcs[i].dc && (dcs[i].hwnd == w->handle || dcs[i].root == w->handle))
      ReleaseDC(dcs[i].hwnd, dcs[i].dc);
}
void u32_damage(struct u32_window *w, RECT r) {
  struct u32_window *root = u32_root(w);
  if (!root)
    return;
  POINT a = u32_origin(w), b = u32_surface_origin(root);
  OffsetRect(&r, a.x - b.x, a.y - b.y);
  RECT bounds = {0, 0, root->surface.width, root->surface.height}, cut;
  if (IntersectRect(&cut, &r, &bounds))
    u32_region_add(&root->damage, cut);
}
void u32_flush(void) {
  DWORD now = u32_ticks();
  if ((int32_t)(now - next_frame) < 0)
    return;
  BOOL any = 0;
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    /* q3exec2: a window being destroyed sends nothing more. DestroyWindow
     * waits for the desktop's DESK_CLOSE answer while pumping, and a frame
     * update flushed during that wait reached the desktop after the window
     * was gone: the desktop then dropped the program's whole connection. */
    if (!w->handle || !w->wire || w->destroying || w->dcs || !w->redraw ||
        !IsWindowVisible(w->handle) || IsIconic(w->handle))
      continue;
    UINT sent = 0;
    for (; sent < w->damage.n; sent++) {
      RECT r = w->damage.r[sent];
      uint8_t data[16];
      desk_put32(data, r.left);
      desk_put32(data + 4, r.top);
      desk_put32(data + 8, r.right - r.left);
      desk_put32(data + 12, r.bottom - r.top);
      if (!u32_wire(w, DESK_DAMAGE, data, 16, -1))
        break;
      any = 1;
    }
    if (sent) {
      for (UINT j = sent; j < w->damage.n; j++)
        w->damage.r[j - sent] = w->damage.r[j];
      w->damage.n -= sent;
    }
  }
  if (any)
    next_frame = now + 17;
}
/* fx-hlrun: erasing before painting. Windows sends WM_ERASEBKGND outside
 * WM_PAINT when a window is shown, resized or uncovered (SetWindowPos
 * without SWP_DEFERERASE, RDW_ERASENOW, WM_SYNCPAINT): the DC is clipped to
 * the update region, the erase flag is cleared first and the update region
 * stays pending, so a WM_ERASEBKGND handler that calls BeginPaint itself
 * (MFC programs: Half-Life's launcher and CD menu draw their whole
 * background that way) still paints the region and validates it. An erase
 * the window did not handle (FALSE) is reported later as fErase. Wine 10.0
 * dlls/win32u/dce.c erase_now/send_erase and window.c set_window_pos;
 * server/window.c get_update_region (UPDATE_ERASE clears PAINT_ERASE only)
 * and get_child_update_flags (children only below parents that clip them,
 * unless RDW_ALLCHILDREN). */
void u32_erase_window(HWND h) {
  struct u32_window *w = u32_window(h);
  if (!w || w->erase != U32_ERASE_PENDING || !w->update.n || !u32_paintable(w) || IsIconic(h))
    return;
  w->erase = 0;
  struct u32_region region = w->update;
  HDC dc = acquire(h, 0, &region);
  if (!dc) {
    w->erase = U32_ERASE_PENDING;
    return;
  }
  BOOL handled = SendMessageA(h, WM_ERASEBKGND, (WPARAM)dc, 0) != 0;
  ReleaseDC(h, dc);
  if ((w = u32_window(h)) && !handled && !w->erase && w->update.n)
    w->erase = U32_ERASE_DELAYED;
}
static void erase_tree(HWND h, UINT flags, UINT depth) {
  struct u32_window *w = u32_window(h);
  if (!w || depth > 16 || !IsWindowVisible(h) || IsIconic(h))
    return;
  u32_erase_window(h);
  if (!(w = u32_window(h)) || (flags & 0x40 /* RDW_NOCHILDREN */) ||
      (!(flags & 0x80 /* RDW_ALLCHILDREN */) && !(w->style & WS_CLIPCHILDREN)))
    return;
  DWORD below = UINT32_MAX;
  for (UINT round = 0; round < U32_WINDOWS; round++) {
    struct u32_window *next = 0;
    for (UINT i = 0; i < U32_WINDOWS; i++) {
      struct u32_window *c = &u32_windows[i];
      if (c->handle && c->parent == h && (c->style & WS_VISIBLE) && c->z < below && (!next || c->z > next->z))
        next = c;
    }
    if (!next)
      break;
    below = next->z;
    erase_tree(next->handle, flags, depth + 1);
    if (!u32_window(h))
      return;
  }
}
void u32_erase_now(HWND h, UINT flags) { erase_tree(h, flags, 0); }
/* fx-hlrun: RDW_UPDATENOW. The window gets its WM_PAINT first, then its
 * descendants that still need one, top of the z-order first: all of them
 * for RDW_ALLCHILDREN (UpdateWindow), otherwise only below windows that clip
 * their children (the others wait for their parent's WM_PAINT); none for
 * RDW_NOCHILDREN. Wine 10.0 dlls/win32u/dce.c update_now and
 * server/window.c get_child_update_flags; dlls/user32/win.c UpdateWindow is
 * RedrawWindow(RDW_UPDATENOW | RDW_ALLCHILDREN). */
static void update_now(HWND h, UINT flags, UINT depth) {
  struct u32_window *w = u32_window(h);
  if (!w || depth > 16)
    return;
  if (w->redraw && w->update.n && u32_paintable(w))
    SendMessageA(h, WM_PAINT, 0, 0);
  w = u32_window(h);
  if (!w || (flags & 0x40 /* RDW_NOCHILDREN */) || IsIconic(h) ||
      (!(flags & 0x80 /* RDW_ALLCHILDREN */) && !(w->style & WS_CLIPCHILDREN)))
    return;
  /* Each child once, highest z first; a callback may create or destroy
   * windows, so the next one is looked up again every time. */
  DWORD below = UINT32_MAX;
  for (UINT round = 0; round < U32_WINDOWS; round++) {
    struct u32_window *next = 0;
    for (UINT i = 0; i < U32_WINDOWS; i++) {
      struct u32_window *c = &u32_windows[i];
      if (c->handle && c->parent == h && (c->style & WS_VISIBLE) && c->z < below && (!next || c->z > next->z))
        next = c;
    }
    if (!next)
      break;
    below = next->z;
    update_now(next->handle, flags, depth + 1);
    if (!u32_window(h))
      return;
  }
}
BOOL W32API UpdateWindow(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  update_now(h, 0x80 /* RDW_ALLCHILDREN */, 0);
  return 1;
}
/* q3exec4: Windows paints only visible windows; a child that is hidden, or
 * whose parent is, keeps no pixels in the parent's surface. Top-level
 * windows keep painting their own surface while hidden (it is shown only
 * with the window, whose first frame it is). */
BOOL u32_paintable(struct u32_window *w) {
  return !w->parent || IsWindowVisible(w->handle);
}
/* Invalidate, in WINDOW's client coordinates, every visible child of
 * WINDOW but SKIP that meets R, and their children, whatever their parents'
 * WS_CLIPCHILDREN says (a clipped parent does not repaint under them). */
static void expose_children(struct u32_window *window, RECT r, struct u32_window *skip) {
  POINT po = u32_origin(window);
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *c = &u32_windows[i];
    if (!c->handle || c == skip || c->parent != window->handle || !(c->style & WS_VISIBLE))
      continue;
    RECT box, hit;
    GetWindowRect(c->handle, &box);
    OffsetRect(&box, -po.x, -po.y);
    if (!IntersectRect(&hit, &r, &box))
      continue;
    POINT co = u32_origin(c);
    OffsetRect(&hit, po.x - co.x, po.y - co.y);
    InvalidateRect(c->handle, &hit, 1);
    if (c->style & (WS_BORDER | WS_DLGFRAME | 0x00300000u) ||
        c->exstyle & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE))
      c->internal_paint = 1;
    expose_children(c, hit, skip);
  }
}
/* q3exec4 review: a DC acquired before its window (or an ancestor) was
 * hidden draws nothing more, as Windows updates busy DCs' visible regions;
 * a WM_PAINT handler that hides its window keeps its DC. */
static void hide_dcs(struct u32_window *w) {
  if (!u32_gdi || u32_gdi->version < 3 || !u32_gdi->visible)
    return;
  for (UINT i = 0; i < 64; i++)
    if (dcs[i].dc && dcs[i].hwnd &&
        ((w->parent && dcs[i].hwnd == w->handle) || IsChild(w->handle, dcs[i].hwnd)))
      u32_gdi->visible(dcs[i].dc, 0, 0);
}
/* Hiding a visible child exposes what lies beneath the rectangle it
 * occupied (OLD, in its parent's client coordinates; SetWindowPos may move
 * it in the same call): the parent and the siblings there repaint. Hiding
 * any window also empties the DCs it and its children still hold. */
void u32_child_hidden(struct u32_window *w, RECT old) {
  struct u32_window *parent = u32_window(w->parent);
  if (parent) {
    InvalidateRect(parent->handle, &old, 1);
    expose_children(parent, old, w);
  }
  hide_dcs(w);
}
/* A child that becomes visible is painted whole, its children too (Windows
 * invalidates a window when it is shown). */
void u32_child_shown(struct u32_window *w) {
  RECT client;
  GetClientRect(w->handle, &client);
  InvalidateRect(w->handle, 0, 1);
  if (w->style & (WS_BORDER | WS_DLGFRAME | 0x00300000u) ||
      w->exstyle & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE))
    w->internal_paint = 1;
  expose_children(w, client, 0);
}
BOOL W32API RedrawWindow(HWND h, const RECT *r, HRGN region, UINT flags) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (flags & 1) {
    if (region)
      InvalidateRgn(h, region, flags & 4);
    else
      InvalidateRect(h, r, flags & 4);
  }
  if (flags & 8) {
    if (region)
      ValidateRgn(h, region);
    else
      ValidateRect(h, r);
  }
  if (flags & 2)
    w->internal_paint = 1;
  if (flags & 16)
    w->internal_paint = 0;
  if (flags & 32)
    w->erase = 0;
  if (flags & 128)
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].parent == h)
        RedrawWindow(u32_windows[i].handle, r, region, flags);
  /* fx-hlrun: RDW_UPDATENOW paints (BeginPaint erases first); RDW_ERASENOW
   * alone erases now and leaves WM_PAINT to the queue (Microsoft
   * RedrawWindow; Wine 10.0 dlls/win32u/dce.c NtUserRedrawWindow). */
  if (flags & 256)
    update_now(h, flags, 0); /* fx-hlrun: RDW_ALLCHILDREN/RDW_NOCHILDREN apply */
  else if (flags & 512)
    u32_erase_now(h, flags);
  u32_mark();
  return 1;
}
BOOL W32API LockWindowUpdate(HWND h) {
  U32_GUARD;
  (void)h;
  return 1;
}
int W32API ScrollWindowEx(HWND h, int dx, int dy, const RECT *scroll,
                          const RECT *clip, HRGN region, RECT *update,
                          UINT flags) {
  U32_GUARD;
  struct u32_window *w = u32_window(h), *root = u32_root(w);
  if (!w || !root || !root->surface.pixels)
    return u32_fail(1400);
  RECT client, area;
  GetClientRect(h, &client);
  if (!IntersectRect(&area, scroll ? scroll : &client, clip ? clip : &client)) {
    if (update)
      SetRectEmpty(update);
    return 1;
  }
  IntersectRect(&area, &area, &client);
  POINT o = u32_origin(w), ro = u32_surface_origin(root);
  o.x -= ro.x;
  o.y -= ro.y;
  RECT backing = {-o.x, -o.y, (LONG)root->surface.width - o.x,
                  (LONG)root->surface.height - o.y};
  IntersectRect(&area, &area, &backing);
  for (struct u32_window *parent = u32_window(w->parent); parent;
       parent = u32_window(parent->parent)) {
    RECT bounds;
    GetClientRect(parent->handle, &bounds);
    POINT origin = u32_origin(parent);
    OffsetRect(&bounds, origin.x - ro.x - o.x, origin.y - ro.y - o.y);
    IntersectRect(&area, &area, &bounds);
  }
  if (IsRectEmpty(&area)) {
    if (update)
      SetRectEmpty(update);
    return 1;
  }
  RECT dest = area;
  if (dx >= area.right - area.left || dx <= -(area.right - area.left) ||
      dy >= area.bottom - area.top || dy <= -(area.bottom - area.top))
    SetRectEmpty(&dest);
  else {
    OffsetRect(&dest, dx, dy);
    IntersectRect(&dest, &dest, &area);
  }
  uint8_t *pixels = root->surface.pixels;
  int start = dy > 0 ? dest.bottom - 1 : dest.top,
      end = dy > 0 ? dest.top - 1 : dest.bottom, step = dy > 0 ? -1 : 1;
  for (int y = start; y != end; y += step) {
    if (dx > 0) {
      for (int x = dest.right - 1; x >= dest.left; x--) {
        DWORD color = w32_u32(pixels + (y - dy + o.y) * root->surface.pitch +
                              (x - dx + o.x) * 4);
        w32_put32(pixels + (y + o.y) * root->surface.pitch + (x + o.x) * 4,
                  color);
      }
    } else
      for (int x = dest.left; x < dest.right; x++) {
        DWORD color = w32_u32(pixels + (y - dy + o.y) * root->surface.pitch +
                              (x - dx + o.x) * 4);
        w32_put32(pixels + (y + o.y) * root->surface.pitch + (x + o.x) * 4,
                  color);
      }
  }
  struct u32_region exposed = {.r = {area}, .n = 1};
  u32_region_sub(&exposed, dest);
  RECT box = {0};
  for (UINT i = 0; i < exposed.n; i++) {
    UnionRect(&box, &box, &exposed.r[i]);
    if (flags & 2)
      InvalidateRect(h, &exposed.r[i], flags & 4);
  }
  if (update)
    *update = box;
  if (region && u32_gdi && u32_gdi->region_set)
    u32_gdi->region_set(region, exposed.r, exposed.n);
  u32_damage(w, dest);
  if (flags & 1)
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].handle && u32_windows[i].parent == h) {
        RECT r = u32_windows[i].rect;
        MoveWindow(u32_windows[i].handle, r.left + dx, r.top + dy,
                   r.right - r.left, r.bottom - r.top, 1);
      }
  return exposed.n > 1 ? 3 : exposed.n ? 2 : 1;
}
/* fx-hlrun (MFC's CDC::ScrollDC imports it): scroll the bits of a rectangle
 * of a DC (logical units) by (dx, dy). The source is lprcScroll, or the DC's
 * visible clip box; only what lies inside the clip rectangle (lprcClip, or
 * the clip box) and inside the visible area is written. The part of the
 * clipped area the scroll did not refill is reported as the update rectangle
 * and region (Microsoft ScrollDC; Wine 10.0 dlls/win32u/dce.c
 * NtUserScrollDC: source limited to the visible region, update = clipped
 * area minus the scrolled destination). The copy is an SRCCOPY BitBlt within
 * the DC; GDI snapshots overlapping source bits. */
BOOL W32API ScrollDC(HDC dc, int dx, int dy, const RECT *scroll,
                     const RECT *clip, HRGN region, RECT *update) {
  U32_GUARD;
  const struct u32_graphics *g = u32_graphics();
  if (!dc || !g || !g->clip_box || !g->bitblt)
    return u32_fail(6);
  RECT visible;
  if (!g->clip_box(U32_PTR(dc), &visible))
    return u32_fail(6);
  RECT source = scroll ? *scroll : visible, bound = clip ? *clip : visible,
       area, moved, dest;
  if (source.left > source.right) { LONG t = source.left; source.left = source.right; source.right = t; }
  if (source.top > source.bottom) { LONG t = source.top; source.top = source.bottom; source.bottom = t; }
  if (!IntersectRect(&bound, &bound, &visible))
    SetRectEmpty(&bound);
  if (!IntersectRect(&area, &source, &bound))
    SetRectEmpty(&area);
  if (!IntersectRect(&moved, &source, &visible))
    SetRectEmpty(&moved);
  OffsetRect(&moved, dx, dy);
  if (!IntersectRect(&dest, &moved, &bound))
    SetRectEmpty(&dest);
  if (!IsRectEmpty(&dest) &&
      !g->bitblt(U32_PTR(dc), dest.left, dest.top, dest.right - dest.left, dest.bottom - dest.top, U32_PTR(dc),
                 dest.left - dx, dest.top - dy, 0x00cc0020u /* SRCCOPY */))
    return 0;
  struct u32_region exposed = {.r = {area}, .n = IsRectEmpty(&area) ? 0 : 1};
  if (!IsRectEmpty(&dest))
    u32_region_sub(&exposed, dest);
  RECT box = {0};
  for (UINT i = 0; i < exposed.n; i++)
    UnionRect(&box, &box, &exposed.r[i]);
  if (update)
    *update = box;
  if (region && (!u32_gdi || !u32_gdi->region_set || !u32_gdi->region_set(region, exposed.r, exposed.n)))
    return 0;
  return 1;
}
BOOL W32API ScrollWindow(HWND h, int x, int y, const RECT *r,
                         const RECT *clip) {
  U32_GUARD;
  return ScrollWindowEx(h, x, y, r, clip, 0, 0, 6) != 0;
}

int W32API FillRect(HDC dc, const RECT *rect, HBRUSH brush) {
  U32_GUARD;
  u32_gdi_ensure();
  return rect && u32_gdi && u32_gdi->fill ? u32_gdi->fill(dc, rect, brush)
                                          : u32_fail(120);
}
int W32API FrameRect(HDC dc, const RECT *rect, HBRUSH brush) {
  U32_GUARD;
  if (!rect)
    return u32_fail(87);
  RECT sides[4] = {
      {rect->left, rect->top, rect->right, rect->top + 1},
      {rect->left, rect->bottom - 1, rect->right, rect->bottom},
      {rect->left, rect->top + 1, rect->left + 1, rect->bottom - 1},
      {rect->right - 1, rect->top + 1, rect->right, rect->bottom - 1}};
  for (UINT i = 0; i < 4; i++)
    if (!IsRectEmpty(&sides[i]) && !FillRect(dc, &sides[i], brush))
      return 0;
  return 1;
}

/* Value-only GL target query. No GDI lock is held by the caller. */
BOOL W32API w32_user_gl_target(DWORD dc,DWORD *values) {
 U32_GUARD;
 HWND hwnd=u32_dc_window((HDC)(uintptr_t)dc);
 if(!hwnd)return 0;
 struct u32_window *w=u32_window(hwnd);
 if(!w||!values||w->destroying)return -1;
 RECT r;GetClientRect(w->handle,&r);values[0]=r.right;values[1]=r.bottom;values[2]=U32_PTR(w->handle);
 values[3]=!w->lease_failed&&!w->parent&&!(w->style&WS_CAPTION)&&(w->style&WS_POPUP)&&IsWindowVisible(w->handle);
 /* A window target must never fall through to a generic GDI swap when its
  * owner cannot service the synchronous presentation transport. */
 return w->tid==u32_tid()?1:-1;
}
/* GL has finished its private frame before publishing damage. The desktop
 * clips the copied surface through the existing v3 compositor path. */
BOOL W32API w32_user_gl_damage(DWORD dc) {
 U32_GUARD;struct u32_window *w=u32_root(u32_window(u32_dc_window((HDC)(uintptr_t)dc)));
 if(!w||!w->wire||w->destroying)return 0;
 uint8_t b[16]={0};desk_put32(b+8,w->surface.width);desk_put32(b+12,w->surface.height);
 return u32_wire(w,DESK_V3_PRESENT,b,sizeof(b),-1);
}
