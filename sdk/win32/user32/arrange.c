/* G4 (WM-12): CascadeWindows, TileWindows and ArrangeIconicWindows.
 * References: Microsoft Learn, CascadeWindows, TileWindows and
 * ArrangeIconicWindows (winuser.h). Top-level windows with WS_EX_TOPMOST or
 * WS_EX_TOOLWINDOW are never arranged; a maximised window is restored first;
 * without MDITILE_ZORDER the order of lpKids decides, the stacking itself is
 * never changed. The geometry is the one of the desktop's taskbar menu
 * (apps/desktop/client.c): cascaded windows get three quarters of the area
 * and a caption height of offset; up to three tiles form one strip, more fill
 * a grid. Only windows of this process can be moved.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"

#define ARRANGE_MAX 64
#define ICON_CELL_W 160
#define ICON_CELL_H 24

struct arrange_set {
  HWND list[ARRANGE_MAX];
  UINT count;
  HWND parent;
  UINT how;
};
static BOOL W32API collect_one(HWND h, LPARAM l) {
  struct arrange_set *s = (struct arrange_set *)l;
  struct u32_window *w = u32_window(h);
  if (!w || s->count >= ARRANGE_MAX)
    return 1;
  if (s->parent == U32_DESKTOP ? (w->parent || w->owner) : w->parent != s->parent)
    return 1;
  s->list[s->count++] = h;
  return 1;
}
/* The windows an arrangement moves, in the order it places them. */
static UINT arrange_windows(HWND parent, UINT how, UINT count, const HWND *kids,
                            HWND *out, BOOL top_first) {
  struct arrange_set *s = u32_alloc(sizeof(*s));
  if (!s)
    return 0;
  s->count = 0;
  s->parent = parent ? parent : U32_DESKTOP;
  s->how = how;
  if (kids) {
    for (UINT i = 0; i < count && s->count < ARRANGE_MAX; i++)
      s->list[s->count++] = kids[i];
  } else if (!EnumChildWindows(s->parent, collect_one, (LPARAM)s)) {
    /* The enumeration ends early only on error; a partial list is still valid. */
  }
  UINT n = 0;
  for (UINT i = 0; i < s->count; i++) {
    struct u32_window *w = u32_window(s->list[i]);
    if (!w || !(w->style & WS_VISIBLE) || (w->style & WS_MINIMIZE))
      continue;
    if ((how & MDITILE_SKIPDISABLED) && (w->style & WS_DISABLED))
      continue;
    if (!w->parent && (w->exstyle & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)))
      continue;
    if (s->parent != U32_DESKTOP ? w->parent != s->parent : !!w->parent)
      continue;
    out[n++] = s->list[i];
  }
  /* The stacking order decides when asked for, and when no list was given. */
  if ((how & MDITILE_ZORDER) || !kids)
    for (UINT a = 1; a < n; a++) {
      HWND key = out[a];
      UINT b = a, z = u32_window(key)->z;
      while (b && (top_first ? u32_window(out[b - 1])->z < z
                             : u32_window(out[b - 1])->z > z)) {
        out[b] = out[b - 1];
        b--;
      }
      out[b] = key;
    }
  u32_free(s);
  return n;
}
static BOOL arrange_area(HWND parent, const RECT *given, RECT *area) {
  if (given) {
    *area = *given;
  } else if (!parent || parent == U32_DESKTOP) {
    if (!SystemParametersInfoA(48 /* SPI_GETWORKAREA */, 0, area, 0))
      return 0;
  } else if (!GetClientRect(parent, area)) {
    return 0;
  }
  return area->right > area->left && area->bottom > area->top;
}
static BOOL place(HWND h, int x, int y, int cx, int cy) {
  struct u32_window *w = u32_window(h);
  if (!w)
    return 0;
  if (w->style & WS_MAXIMIZE)
    ShowWindow(h, 9 /* SW_RESTORE */);
  w = u32_window(h);
  if (!w) return 0;
  return SetWindowPos(h, 0, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE |
                      ((w->style & WS_THICKFRAME) ? 0 : SWP_NOSIZE));
}
uint16_t W32API CascadeWindows(HWND parent, UINT how, const RECT *rect, UINT count,
                           const HWND *kids) {
  U32_GUARD;
  if (parent && parent != U32_DESKTOP && !IsWindow(parent))
    return (uint16_t)u32_fail(1400);
  RECT area;
  if (!arrange_area(parent, rect, &area))
    return (uint16_t)u32_fail(87);
  HWND list[ARRANGE_MAX];
  UINT n = arrange_windows(parent, how, count, kids, list, 0);
  int rw = area.right - area.left, rh = area.bottom - area.top;
  int cw = rw * 3 / 4, ch = rh * 3 / 4;
  int step = GetSystemMetrics(4 /* SM_CYCAPTION */) +
             GetSystemMetrics(33 /* SM_CYSIZEFRAME */);
  int fit_x = (rw - cw) / step + 1, fit_y = (rh - ch) / step + 1;
  int wrap = fit_x < fit_y ? fit_x : fit_y;
  if (wrap < 1)
    wrap = 1;
  uint16_t done = 0;
  for (UINT i = 0; i < n; i++) {
    int k = (int)i % wrap;
    if (place(list[i], area.left + k * step, area.top + k * step, cw, ch))
      done++;
  }
  return done;
}
uint16_t W32API TileWindows(HWND parent, UINT how, const RECT *rect, UINT count,
                        const HWND *kids) {
  U32_GUARD;
  if (parent && parent != U32_DESKTOP && !IsWindow(parent))
    return (uint16_t)u32_fail(1400);
  RECT area;
  if (!arrange_area(parent, rect, &area))
    return (uint16_t)u32_fail(87);
  HWND list[ARRANGE_MAX];
  UINT n = arrange_windows(parent, how, count, kids, list, 1);
  if (!n)
    return 0;
  int horizontal = !!(how & MDITILE_HORIZONTAL), cols, rows;
  if (n <= 3) {
    cols = horizontal ? 1 : (int)n;
  } else {
    int a = 1;
    while ((UINT)(a * a) < n)
      a++;
    int b = ((int)n + a - 1) / a;
    cols = horizontal ? b : a;
  }
  rows = ((int)n + cols - 1) / cols;
  int rw = area.right - area.left, rh = area.bottom - area.top;
  uint16_t done = 0;
  UINT index = 0;
  for (int r = 0; r < rows; r++) {
    int per = r < rows - 1 ? cols : (int)n - (rows - 1) * cols;
    int y0 = area.top + rh * r / rows, y1 = area.top + rh * (r + 1) / rows;
    for (int c = 0; c < per; c++, index++) {
      int x0 = area.left + rw * c / per, x1 = area.left + rw * (c + 1) / per;
      if (place(list[index], x0, y0, x1 - x0, y1 - y0))
        done++;
    }
  }
  return done;
}
/* Minimised child windows go along the bottom edge of the parent, left to
 * right and then up. Minimised top-level windows live on the taskbar, which
 * arranges them; the call then only reports the row height. */
UINT W32API ArrangeIconicWindows(HWND parent) {
  U32_GUARD;
  if (!parent || parent == U32_DESKTOP)
    return ICON_CELL_H;
  if (!IsWindow(parent))
    return (UINT)u32_fail(1400);
  RECT client;
  if (!GetClientRect(parent, &client))
    return 0;
  struct arrange_set *s = u32_alloc(sizeof(*s));
  if (!s)
    return 0;
  s->count = 0;
  s->parent = parent;
  s->how = 0;
  EnumChildWindows(parent, collect_one, (LPARAM)s);
  int per_row = (client.right - client.left) / ICON_CELL_W;
  if (per_row < 1)
    per_row = 1;
  int slot = 0;
  for (UINT i = 0; i < s->count; i++) {
    struct u32_window *w = u32_window(s->list[i]);
    if (!w || !(w->style & WS_VISIBLE) || !(w->style & WS_MINIMIZE))
      continue;
    int col = slot % per_row, row = slot / per_row;
    SetWindowPos(s->list[i], 0, client.left + col * ICON_CELL_W,
                 client.bottom - (row + 1) * ICON_CELL_H, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    slot++;
  }
  u32_free(s);
  return ICON_CELL_H;
}
