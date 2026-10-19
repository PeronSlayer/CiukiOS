/* Window scroll bars, the SCROLLBAR control and thumb tracking.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "classic.h"
#define DC32(dc) U32_PTR(dc)
typedef struct u32_scroll_state state_t;
static state_t *bar_state(struct u32_window *w, int bar) {
  return &w->scroll[bar];
}
HBRUSH u32_dither_brush(void) {
  static DWORD brush;
  const struct u32_graphics *g = u32_graphics();
  if (brush || !g || !g->create_bitmap || !g->pattern_brush)
    return (HBRUSH)(uintptr_t)brush;
  static const uint16_t bits[8] = {0x55, 0xaa, 0x55, 0xaa,
                                   0x55, 0xaa, 0x55, 0xaa};
  DWORD bitmap = g->create_bitmap(8, 8, 1, 1, bits);
  if (bitmap) {
    brush = g->pattern_brush(bitmap);
    g->delete_object(bitmap);
  }
  return (HBRUSH)(uintptr_t)brush;
}
static void redraw(struct u32_window *w, int bar) {
  if (bar == SB_CTL)
    InvalidateRect(w->handle, 0, 0);
  else
    u32_paint_frame(w->handle);
}
static BOOL bar_visible(struct u32_window *w, int bar) {
  return bar == SB_CTL ||
         (w->style & (bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL));
}
/* Showing or hiding a window bar changes the client area. */
static void bar_style(struct u32_window *w, DWORD mask, BOOL show) {
  DWORD style = show ? w->style | mask : w->style & ~mask;
  if (style == w->style)
    return;
  HWND h = w->handle;
  w->style = style;
  if (!(style & WS_CHILD))
    u32_resize(w);
  RECT r;
  GetClientRect(h, &r);
  SendMessageA(h, WM_SIZE, 0, (LPARAM)((r.right & 0xffff) | (r.bottom << 16)));
  if (IsWindow(h)) {
    InvalidateRect(h, 0, 1);
    struct u32_window *parent = u32_window(GetParent(h));
    if (parent)
      InvalidateRect(parent->handle, 0, 1);
  }
}
int W32API SetScrollInfo(HWND h, int bar, const SCROLLINFO *s, BOOL draw) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || !s || (s->cbSize != sizeof(*s) && s->cbSize != sizeof(*s) - 4) ||
      bar < 0 || bar > SB_CTL)
    return u32_fail(87);
  state_t *v = bar_state(w, bar);
  if (s->fMask & SIF_RANGE) {
    if (s->nMax < s->nMin)
      return u32_fail(87);
    v->minimum = s->nMin;
    v->maximum = s->nMax;
  }
  int64_t span = (int64_t)v->maximum - v->minimum + 1;
  if (s->fMask & SIF_PAGE)
    v->page = (int64_t)s->nPage > span ? (UINT)span : s->nPage;
  if (s->fMask & SIF_POS)
    v->position = s->nPos;
  int64_t top = (int64_t)v->maximum - (v->page ? v->page - 1 : 0);
  if (top < v->minimum)
    top = v->minimum;
  if (v->position < v->minimum)
    v->position = v->minimum;
  if (v->position > top)
    v->position = (int)top;
  if (bar != SB_CTL && (s->fMask & (SIF_RANGE | SIF_PAGE))) {
    BOOL needed = v->maximum > v->minimum &&
                  (!v->page || (int64_t)v->page <= span - 1);
    if (s->fMask & SIF_DISABLENOSCROLL) {
      v->disabled = needed ? 0 : ESB_DISABLE_BOTH;
      bar_style(w, bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL, 1);
    } else
      bar_style(w, bar == SB_VERT ? WS_VSCROLL : WS_HSCROLL, needed);
    w = u32_window(h);
    if (!w)
      return 0;
    v = bar_state(w, bar);
  }
  if (draw && bar_visible(w, bar))
    redraw(w, bar);
  return v->position;
}
BOOL W32API GetScrollInfo(HWND h, int bar, SCROLLINFO *s) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || !s || (s->cbSize != sizeof(*s) && s->cbSize != sizeof(*s) - 4) ||
      bar < 0 || bar > SB_CTL)
    return u32_fail(87);
  state_t *v = bar_state(w, bar);
  if (s->fMask & SIF_RANGE) {
    s->nMin = v->minimum;
    s->nMax = v->maximum;
  }
  if (s->fMask & SIF_PAGE)
    s->nPage = v->page;
  if (s->fMask & SIF_POS)
    s->nPos = v->position;
  if ((s->fMask & SIF_TRACKPOS) && s->cbSize == sizeof(*s))
    s->nTrackPos = v->track;
  return 1;
}
int W32API SetScrollPos(HWND h, int bar, int n, BOOL draw) {
  U32_GUARD;
  SCROLLINFO s = {.cbSize = sizeof(s), .fMask = SIF_POS};
  if (!GetScrollInfo(h, bar, &s))
    return 0;
  int old = s.nPos;
  s.nPos = n;
  SetScrollInfo(h, bar, &s, draw);
  return old;
}
int W32API GetScrollPos(HWND h, int bar) {
  U32_GUARD;
  SCROLLINFO s = {.cbSize = sizeof(s), .fMask = SIF_POS};
  return GetScrollInfo(h, bar, &s) ? s.nPos : 0;
}
BOOL W32API SetScrollRange(HWND h, int bar, int a, int z, BOOL draw) {
  U32_GUARD;
  SCROLLINFO s = {.cbSize = sizeof(s), .fMask = SIF_RANGE, .nMin = a,
                  .nMax = z};
  if (!IsWindow(h) || bar < 0 || bar > SB_CTL || z < a ||
      (int64_t)z - a > 0x7fffffff)
    return u32_fail(87);
  SetScrollInfo(h, bar, &s, draw);
  return 1;
}
BOOL W32API GetScrollRange(HWND h, int bar, int *a, int *z) {
  U32_GUARD;
  SCROLLINFO s = {.cbSize = sizeof(s), .fMask = SIF_RANGE};
  if (!a || !z || !GetScrollInfo(h, bar, &s))
    return u32_fail(87);
  *a = s.nMin;
  *z = s.nMax;
  return 1;
}
BOOL W32API ShowScrollBar(HWND h, int bar, BOOL show) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || bar < 0 || bar > SB_BOTH)
    return u32_fail(87);
  if (bar == SB_CTL)
    return ShowWindow(h, show ? 5 : 0), 1;
  DWORD mask = bar == SB_BOTH   ? WS_HSCROLL | WS_VSCROLL
               : bar == SB_VERT ? WS_VSCROLL
                                : WS_HSCROLL;
  bar_style(w, mask, show);
  return 1;
}
BOOL W32API EnableScrollBar(HWND h, UINT bar, UINT arrows) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w || bar > SB_BOTH || arrows > ESB_DISABLE_BOTH)
    return u32_fail(87);
  for (int b = 0; b < 3; b++)
    if ((int)bar == b || (bar == SB_BOTH && b < 2)) {
      bar_state(w, b)->disabled = arrows;
      if (bar_visible(w, b))
        redraw(w, b);
    }
  return 1;
}

/* ---- geometry and painting --------------------------------------------- */
struct geometry {
  BOOL vertical;
  int length, thickness, arrow, track, thumb, thumb_at;
  BOOL empty;
};
static struct geometry measure(const state_t *v, RECT r, BOOL vertical) {
  struct geometry g = {vertical, 0, 0, 0, 0, 0, 0, 0};
  g.length = vertical ? r.bottom - r.top : r.right - r.left;
  g.thickness = vertical ? r.right - r.left : r.bottom - r.top;
  g.arrow = g.thickness;
  if (2 * g.arrow > g.length)
    g.arrow = g.length / 2;
  g.track = g.length - 2 * g.arrow;
  int64_t range = (int64_t)v->maximum - v->minimum + 1;
  g.empty = v->maximum <= v->minimum || (v->page && (int64_t)v->page >= range) ||
            v->disabled == ESB_DISABLE_BOTH;
  if (g.empty || g.track < 8)
    return g;
  int64_t thumb =
      v->page ? u32_div64((int64_t)g.track * v->page, range)
              : (int64_t)g.thickness;
  int minimum = g.thickness / 2 > 8 ? g.thickness / 2 : 8;
  if (thumb < minimum)
    thumb = minimum;
  if (thumb > g.track)
    thumb = g.track;
  int64_t steps = range - (v->page ? v->page : 1);
  g.thumb = (int)thumb;
  g.thumb_at = g.arrow + (steps > 0 ? (int)u32_div64((int64_t)(g.track - g.thumb) *
                                                         (v->position - v->minimum),
                                                     steps)
                                    : 0);
  return g;
}
void u32_scroll_paint(struct u32_window *w, HDC dc, int bar, RECT r) {
  const state_t *v = bar_state(w, bar);
  BOOL vertical = bar == SB_VERT || (bar == SB_CTL && (w->style & SBS_VERT));
  struct geometry g = measure(v, r, vertical);
  UINT lt = vertical ? DFCS_SCROLLUP : DFCS_SCROLLLEFT,
       rb = vertical ? DFCS_SCROLLDOWN : DFCS_SCROLLRIGHT;
  BOOL disabled_lt = g.empty || (v->disabled & ESB_DISABLE_LTUP) ||
                     (w->style & WS_DISABLED),
       disabled_rb = g.empty || (v->disabled & ESB_DISABLE_RTDN) ||
                     (w->style & WS_DISABLED);
  RECT a = vertical ? (RECT){r.left, r.top, r.right, r.top + g.arrow}
                    : (RECT){r.left, r.top, r.left + g.arrow, r.bottom};
  RECT b = vertical ? (RECT){r.left, r.bottom - g.arrow, r.right, r.bottom}
                    : (RECT){r.right - g.arrow, r.top, r.right, r.bottom};
  RECT track = vertical ? (RECT){r.left, a.bottom, r.right, b.top}
                        : (RECT){a.right, r.top, b.left, r.bottom};
  const struct u32_graphics *gr = u32_graphics();
  HBRUSH dither = u32_dither_brush();
  if (gr && dither && !IsRectEmpty(&track)) {
    DWORD old_text = gr->text_color(DC32(dc), GetSysColor(COLOR_BTNFACE)),
          old_bk = gr->bk_color(DC32(dc), GetSysColor(COLOR_BTNHIGHLIGHT));
    FillRect(dc, &track, dither);
    gr->text_color(DC32(dc), old_text);
    gr->bk_color(DC32(dc), old_bk);
  } else
    u32_color_rect(dc, track, GetSysColor(COLOR_SCROLLBAR));
  DrawFrameControl(dc, &a, DFC_SCROLL, lt | (disabled_lt ? DFCS_INACTIVE : 0));
  DrawFrameControl(dc, &b, DFC_SCROLL, rb | (disabled_rb ? DFCS_INACTIVE : 0));
  if (!g.empty && g.thumb) {
    int at = g.thumb_at;
    RECT t = vertical
                 ? (RECT){r.left, r.top + at, r.right, r.top + at + g.thumb}
                 : (RECT){r.left + at, r.top, r.left + at + g.thumb, r.bottom};
    u32_color_rect(dc, t, GetSysColor(COLOR_BTNFACE));
    DrawEdge(dc, &t, EDGE_RAISED, BF_RECT);
  }
}

/* ---- tracking ------------------------------------------------------------ */
/* Bar rectangle in screen coordinates. */
static RECT bar_rect(struct u32_window *w, int bar) {
  RECT r;
  if (bar == SB_CTL) {
    GetClientRect(w->handle, &r);
    POINT o = u32_origin(w);
    OffsetRect(&r, o.x, o.y);
    return r;
  }
  GetClientRect(w->handle, &r);
  POINT o = u32_origin(w);
  OffsetRect(&r, o.x, o.y);
  int top, right, bottom;
  u32_nonclient(w, &top, &right, &bottom);
  if (bar == SB_VERT)
    return (RECT){r.right, r.top, r.right + right, r.bottom};
  return (RECT){r.left, r.bottom, r.right, r.bottom + bottom};
}
static void send(HWND h, int bar, UINT code, int pos) {
  struct u32_window *w = u32_window(h);
  if (!w)
    return;
  BOOL vertical = bar == SB_VERT || (bar == SB_CTL && (w->style & SBS_VERT));
  UINT m = vertical ? WM_VSCROLL : WM_HSCROLL;
  WPARAM wp = code | ((WPARAM)(uint16_t)pos << 16);
  if (bar == SB_CTL)
    SendMessageA(w->parent ? w->parent : w->owner, m, wp, (LPARAM)h);
  else
    SendMessageA(h, m, wp, 0);
}
void u32_scroll_track(HWND h, int bar, POINT screen) {
  struct u32_window *w = u32_window(h);
  if (!w)
    return;
  state_t *v = bar_state(w, bar);
  RECT r = bar_rect(w, bar);
  BOOL vertical = bar == SB_VERT || (bar == SB_CTL && (w->style & SBS_VERT));
  struct geometry g = measure(v, r, vertical);
  if (g.empty)
    return;
  int at = vertical ? screen.y - r.top : screen.x - r.left;
  UINT code;
  if (at < g.arrow)
    code = SB_LINEUP;
  else if (at >= g.length - g.arrow)
    code = SB_LINEDOWN;
  else if (g.thumb && at < g.thumb_at)
    code = SB_PAGEUP;
  else if (g.thumb && at >= g.thumb_at + g.thumb)
    code = SB_PAGEDOWN;
  else
    code = SB_THUMBTRACK;
  int grab = at - g.thumb_at;
  if (code != SB_THUMBTRACK)
    send(h, bar, code, 0);
  SetCapture(h);
  v->track = v->position;
  MSG msg;
  for (UINT guard = 0; guard < 100000; guard++) {
    if (!GetMessageA(&msg, 0, 0, 0)) {
      PostQuitMessage((int)msg.wParam);
      break;
    }
    if (msg.message == WM_LBUTTONUP || msg.message == WM_NCLBUTTONUP)
      break;
    if ((msg.message == WM_MOUSEMOVE || msg.message == WM_NCMOUSEMOVE) &&
        code == SB_THUMBTRACK) {
      w = u32_window(h);
      if (!w)
        break;
      v = bar_state(w, bar);
      int pos = (vertical ? msg.pt.y - r.top : msg.pt.x - r.left) - grab;
      int64_t steps = (int64_t)v->maximum - v->minimum + 1 -
                      (v->page ? v->page : 1);
      int span = g.track - g.thumb;
      int64_t value =
          span > 0 ? u32_div64((int64_t)(pos - g.arrow) * steps, span) : 0;
      if (value < 0)
        value = 0;
      if (value > steps)
        value = steps;
      v->track = v->minimum + (int)value;
      send(h, bar, SB_THUMBTRACK, v->track);
      continue;
    }
    if (msg.message >= WM_KEYDOWN && msg.message <= 0x108)
      continue;
    if (msg.message >= WM_MOUSEMOVE && msg.message <= WM_MOUSEWHEEL)
      continue;
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
    if (!IsWindow(h))
      return;
  }
  if (GetCapture() == h)
    ReleaseCapture();
  w = u32_window(h);
  if (!w)
    return;
  v = bar_state(w, bar);
  if (code == SB_THUMBTRACK)
    send(h, bar, SB_THUMBPOSITION, v->track);
  send(h, bar, SB_ENDSCROLL, 0);
}

/* ---- the SCROLLBAR control ------------------------------------------------ */
LRESULT u32_scrollbar_message(HWND h, UINT m, WPARAM wp, LPARAM lp,
                              BOOL *handled) {
  struct u32_window *w = u32_window(h);
  *handled = 1;
  switch (m) {
  case SBM_SETSCROLLINFO:
    return SetScrollInfo(h, SB_CTL, (const SCROLLINFO *)lp, !!wp);
  case SBM_GETSCROLLINFO:
    return GetScrollInfo(h, SB_CTL, (SCROLLINFO *)lp);
  case SBM_SETPOS:
    return SetScrollPos(h, SB_CTL, (int)wp, !!lp);
  case SBM_GETPOS:
    return GetScrollPos(h, SB_CTL);
  case SBM_SETRANGE:
  case SBM_SETRANGEREDRAW: {
    int old = GetScrollPos(h, SB_CTL);
    SetScrollRange(h, SB_CTL, (int)wp, (int)lp, m == SBM_SETRANGEREDRAW);
    return old;
  }
  case SBM_GETRANGE:
    return GetScrollRange(h, SB_CTL, (int *)wp, (int *)lp);
  case SBM_ENABLE_ARROWS:
    return EnableScrollBar(h, SB_CTL, (UINT)wp);
  case WM_GETDLGCODE:
    return DLGC_WANTARROWS;
  case WM_LBUTTONDOWN: {
    POINT p = {(int16_t)lp, (int16_t)(lp >> 16)};
    ClientToScreen(h, &p);
    u32_scroll_track(h, SB_CTL, p);
    return 0;
  }
  case WM_KEYDOWN: {
    UINT code = wp == VK_LEFT || wp == VK_UP       ? SB_LINEUP
                : wp == VK_RIGHT || wp == VK_DOWN  ? SB_LINEDOWN
                : wp == VK_PRIOR                   ? SB_PAGEUP
                : wp == VK_NEXT                    ? SB_PAGEDOWN
                : wp == VK_HOME                    ? SB_TOP
                : wp == VK_END                     ? SB_BOTTOM
                                                   : 0xffff;
    if (code != 0xffff && w) {
      send(h, SB_CTL, code, 0);
      send(h, SB_CTL, SB_ENDSCROLL, 0);
    }
    return 0;
  }
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    w = u32_window(h);
    if (dc && w) {
      RECT r;
      GetClientRect(h, &r);
      if (w->style & SBS_SIZEBOX)
        u32_color_rect(dc, r, GetSysColor(COLOR_BTNFACE));
      else
        u32_scroll_paint(w, dc, SB_CTL, r);
    }
    if (dc)
      EndPaint(h, &ps);
    return 0;
  }
  default:
    *handled = 0;
    return 0;
  }
}
