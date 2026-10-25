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
    w->erase = 1;
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
  if (erase && w->erase && w->update.n) {
    HDC dc = GetDC(h);
    if (dc) {
      if (SendMessageA(h, WM_ERASEBKGND, (WPARAM)dc, 0))
        w->erase = 0;
      ReleaseDC(h, dc);
    }
  }
  return w->update.n != 0;
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
    HDC dc = (HDC)(uintptr_t)g->memory_dc(0);
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
  w->internal_paint = 0;
  BOOL erase = w->erase;
  w->erase = 0;
  if (erase)
    ps->fErase = !SendMessageA(h, WM_ERASEBKGND, (WPARAM)dc, 0);
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
BOOL W32API UpdateWindow(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  if (w->redraw && w->update.n && u32_paintable(w))
    SendMessageA(h, WM_PAINT, 0, 0);
  return 1;
}
/* q3exec4: Windows paints only visible windows; a child that is hidden, or
 * whose parent is, keeps no pixels in the parent's surface. Top-level
 * windows keep painting their own surface while hidden (it is shown only
 * with the window, whose first frame it is). */
BOOL u32_paintable(struct u32_window *w) {
  return !w->parent || IsWindowVisible(w->handle);
}
/* Hiding a visible child exposes the parent beneath it (ShowWindow SW_HIDE,
 * SetWindowPos SWP_HIDEWINDOW): the parent repaints that area. */
void u32_child_hidden(struct u32_window *w) {
  struct u32_window *parent = u32_window(w->parent);
  if (!parent)
    return;
  RECT r;
  GetWindowRect(w->handle, &r);
  POINT po = u32_origin(parent);
  OffsetRect(&r, -po.x, -po.y);
  InvalidateRect(parent->handle, &r, 1);
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
  if (flags & (256 | 512))
    UpdateWindow(h);
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
 values[3]=!w->parent&&!(w->style&WS_CAPTION)&&(w->style&WS_POPUP)&&IsWindowVisible(w->handle);
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
