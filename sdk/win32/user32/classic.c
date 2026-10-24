/* Windows classic look drawn from GDI primitives: edges, frame controls,
 * text helpers, system brushes, font layout data, the caret, personality-
 * owned non-client bands (menu bar, borders, scroll bars) and their hit
 * testing. No theme bitmaps. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "classic.h"
extern HANDLE W32API LoadLibraryA(const char *);
extern HANDLE W32API GetModuleHandleA(const char *);
extern void *W32API GetProcAddress(HANDLE, const char *);
#define DC32(dc) U32_PTR(dc)

const struct u32_graphics *u32_graphics(void) {
  static struct u32_graphics g;
  static BOOL ready;
  if (ready)
    return &g;
  HANDLE h = GetModuleHandleA("gdi32.dll");
  if (!h)
    h = LoadLibraryA("gdi32.dll");
  if (!h)
    return 0;
  struct u32_graphics t;
#define B(member, name) *(void **)(&t.member) = GetProcAddress(h, name)
  B(brush, "CreateSolidBrush");
  B(delete_object, "DeleteObject");
  B(select, "SelectObject");
  B(patblt, "PatBlt");
  B(text_color, "SetTextColor");
  B(bk_color, "SetBkColor");
  B(bk_mode, "SetBkMode");
  B(draw_text, "w32_gdi_draw_text");
  B(text_out, "TextOutA");
  B(font_indirect, "CreateFontIndirectA");
  B(metrics, "GetTextMetricsA");
  B(extent, "GetTextExtentPoint32A");
  B(char_widths, "GetCharWidth32A");
  B(memory_dc, "CreateCompatibleDC");
  B(delete_dc, "DeleteDC");
  B(stock, "GetStockObject");
  B(object, "GetObjectA");
  B(bitblt, "BitBlt");
  B(pen, "CreatePen");
  B(move_to, "MoveToEx");
  B(line_to, "LineTo");
  B(create_bitmap, "CreateBitmap");
  B(pattern_brush, "CreatePatternBrush");
  B(get_text_color, "GetTextColor");
  B(get_bk_color, "GetBkColor");
  B(dib_bitmap, "CreateDIBitmap");
#undef B
  if (!t.brush || !t.delete_object || !t.select || !t.patblt ||
      !t.text_color || !t.bk_mode || !t.bk_color || !t.draw_text)
    return 0;
  g = t;
  ready = 1;
  return &g;
}

/* Signed 64-bit division truncating toward zero by shift and subtract:
 * USER links no compiler runtime for the i686 64-bit helpers. */
int64_t u32_div64(int64_t n, int64_t d) {
  if (!d)
    return 0;
  BOOL negative = (n < 0) != (d < 0);
  uint64_t a = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n,
           b = d < 0 ? (uint64_t)0 - (uint64_t)d : (uint64_t)d, q = 0, r = 0;
  for (int bit = 63; bit >= 0; bit--) {
    r = (r << 1) | ((a >> bit) & 1);
    if (r >= b) {
      r -= b;
      q |= (uint64_t)1 << bit;
    }
  }
  return negative ? -(int64_t)q : (int64_t)q;
}
int u32_stricmp(const char *a, const char *b) {
  for (;; a++, b++) {
    int x = (uint8_t)*a, y = (uint8_t)*b;
    if (x >= 'a' && x <= 'z')
      x -= 32;
    if (y >= 'a' && y <= 'z')
      y -= 32;
    if (x != y || !x)
      return x - y;
  }
}
void u32_strcpy(char *out, UINT cap, const char *s) {
  UINT n = 0;
  while (s && s[n] && n + 1 < cap) {
    out[n] = s[n];
    n++;
  }
  if (cap)
    out[n] = 0;
}

/* ---- font layout data ------------------------------------------------- */
static struct u32_font_info fonts[16];
static UINT font_clock[16], font_tick;
static void fallback(struct u32_font_info *f, HFONT font) {
  BOOL system = !font;
  f->font = font;
  f->height = system ? 16 : 13;
  f->ascent = system ? 13 : 11;
  f->descent = f->height - f->ascent;
  f->average = system ? 7 : 5;
  f->external = 0;
  for (UINT i = 0; i < 256; i++)
    f->widths[i] = i < 32 ? 0 : system ? 8 : 6;
  f->base_x = system ? 8 : 6;
  f->base_y = f->height;
  f->measured = 0;
}
const struct u32_font_info *u32_font_info(HFONT font) {
  for (UINT i = 0; i < 16; i++)
    if (font_clock[i] && fonts[i].font == font) {
      font_clock[i] = ++font_tick;
      return &fonts[i];
    }
  UINT slot = 0;
  for (UINT i = 1; i < 16; i++)
    if (font_clock[i] < font_clock[slot])
      slot = i;
  struct u32_font_info *f = &fonts[slot];
  fallback(f, font);
  font_clock[slot] = ++font_tick;
  const struct u32_graphics *g = u32_graphics();
  if (!g || !g->memory_dc || !g->metrics || !g->char_widths || !g->stock)
    return f;
  DWORD dc = g->memory_dc(0);
  if (!dc)
    return f;
  DWORD old = g->select(dc, font ? U32_PTR(font) : g->stock(13));
  struct u32_textmetric tm;
  int widths[256];
  if (old && g->metrics(dc, &tm) && g->char_widths(dc, 0, 255, widths)) {
    f->height = tm.height;
    f->ascent = tm.ascent;
    f->descent = tm.descent;
    f->average = tm.average;
    f->external = tm.external;
    for (UINT i = 0; i < 256; i++)
      f->widths[i] = widths[i];
    /* Windows dialog base units: the 52-letter alphabet extent. */
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    U32SIZE size = {0, 0};
    int total = 0;
    if (g->extent && g->extent(dc, alphabet, 52, &size))
      total = size.cx;
    else
      for (UINT i = 0; i < 52; i++)
        total += widths[(uint8_t)alphabet[i]];
    f->base_x = (total / 26 + 1) / 2;
    f->base_y = tm.height;
    f->measured = 1;
  }
  if (old)
    g->select(dc, old);
  g->delete_dc(dc);
  return f;
}
int u32_text_width(const struct u32_font_info *f, const char *s, int n) {
  int width = 0;
  for (int i = 0; s && i < n && s[i]; i++)
    width += f->widths[(uint8_t)s[i]];
  return width;
}

/* ---- primitives -------------------------------------------------------- */
void u32_color_rect(HDC dc, RECT r, DWORD color) {
  const struct u32_graphics *g = u32_graphics();
  if (!g || !dc || r.right <= r.left || r.bottom <= r.top)
    return;
  DWORD b = g->brush(color);
  if (!b)
    return;
  DWORD old = g->select(DC32(dc), b);
  g->patblt(DC32(dc), r.left, r.top, r.right - r.left, r.bottom - r.top,
            0xf00021u);
  g->select(DC32(dc), old);
  g->delete_object(b);
}
static void sys_rect(HDC dc, RECT r, int index) {
  u32_color_rect(dc, r, GetSysColor(index));
}
void u32_label(HDC dc, RECT r, const char *s, HFONT font, DWORD color,
               UINT flags) {
  const struct u32_graphics *g = u32_graphics();
  if (!g || !s || !dc)
    return;
  DWORD old = font ? g->select(DC32(dc), U32_PTR(font)) : 0;
  DWORD c = g->text_color(DC32(dc), color);
  int mode = g->bk_mode(DC32(dc), 1);
  g->draw_text(DC32(dc), s, -1, &r, flags, 0, 0);
  g->bk_mode(DC32(dc), mode);
  g->text_color(DC32(dc), c);
  if (font && old)
    g->select(DC32(dc), old);
}
int u32_label_height(HDC dc, RECT r, const char *s, HFONT font, UINT flags) {
  const struct u32_graphics *g = u32_graphics();
  if (!g || !s || !dc)
    return 0;
  DWORD old = font ? g->select(DC32(dc), U32_PTR(font)) : 0;
  int h = g->draw_text(DC32(dc), s, -1, &r, flags | 0x400u, 0, 0);
  if (font && old)
    g->select(DC32(dc), old);
  return h;
}
void u32_mnemonic_text(HDC dc, RECT r, const char *s, HFONT font, DWORD color,
                       UINT flags, BOOL disabled) {
  if (disabled) {
    RECT shadow = r;
    OffsetRect(&shadow, 1, 1);
    u32_label(dc, shadow, s, font, GetSysColor(COLOR_BTNHIGHLIGHT), flags);
    u32_label(dc, r, s, font, GetSysColor(COLOR_BTNSHADOW), flags);
  } else
    u32_label(dc, r, s, font, color, flags);
}
static HBRUSH sys_brushes[29];
static DWORD sys_brush_colors[29];
HBRUSH W32API GetSysColorBrush(int n) {
  U32_GUARD;
  const struct u32_graphics *g = u32_graphics();
  if (n < 0 || n >= 29 || !g)
    return 0;
  DWORD color = GetSysColor(n);
  if (!sys_brushes[n] || sys_brush_colors[n] != color) {
    DWORD b = g->brush(color);
    if (!b)
      return sys_brushes[n];
    /* Superseded system brushes stay valid for callers that kept them. */
    sys_brushes[n] = (HBRUSH)(uintptr_t)b;
    sys_brush_colors[n] = color;
  }
  return sys_brushes[n];
}
HBRUSH u32_sys_brush(int n) { return GetSysColorBrush(n); }

BOOL W32API DrawEdge(HDC dc, RECT *r, UINT edge, UINT flags) {
  U32_GUARD;
  if (!dc || !r)
    return u32_fail(87);
  RECT b = *r;
  /* Outer pass then inner pass; classic colours per edge kind. */
  for (UINT pass = 0; pass < 2; pass++) {
    UINT raised = pass ? BDR_RAISEDINNER : BDR_RAISEDOUTER,
         sunken = pass ? BDR_SUNKENINNER : BDR_SUNKENOUTER;
    if (!(edge & (raised | sunken)))
      continue;
    int tl, br;
    if (edge & raised) {
      tl = pass ? COLOR_BTNHIGHLIGHT : COLOR_3DLIGHT;
      br = pass ? COLOR_BTNSHADOW : COLOR_3DDKSHADOW;
    } else {
      tl = pass ? COLOR_3DDKSHADOW : COLOR_BTNSHADOW;
      br = pass ? COLOR_3DLIGHT : COLOR_BTNHIGHLIGHT;
    }
    if (flags & BF_MONO)
      tl = br = COLOR_WINDOWFRAME;
    else if (flags & BF_FLAT)
      tl = br = pass ? COLOR_BTNFACE : COLOR_BTNSHADOW;
    if (flags & BF_LEFT)
      sys_rect(dc, (RECT){b.left, b.top, b.left + 1, b.bottom}, tl);
    if (flags & BF_TOP)
      sys_rect(dc, (RECT){b.left, b.top, b.right, b.top + 1}, tl);
    if (flags & BF_RIGHT)
      sys_rect(dc, (RECT){b.right - 1, b.top, b.right, b.bottom}, br);
    if (flags & BF_BOTTOM)
      sys_rect(dc, (RECT){b.left, b.bottom - 1, b.right, b.bottom}, br);
    if (flags & BF_LEFT)
      b.left++;
    if (flags & BF_TOP)
      b.top++;
    if (flags & BF_RIGHT)
      b.right--;
    if (flags & BF_BOTTOM)
      b.bottom--;
  }
  if (flags & BF_MIDDLE)
    sys_rect(dc, b, flags & BF_MONO ? COLOR_WINDOW : COLOR_BTNFACE);
  if (flags & BF_ADJUST)
    *r = b;
  return !!u32_graphics();
}
BOOL W32API DrawFocusRect(HDC dc, const RECT *r) {
  U32_GUARD;
  const struct u32_graphics *g = u32_graphics();
  if (!g || !r || !dc)
    return 0;
  for (int x = r->left; x < r->right; x += 2) {
    g->patblt(DC32(dc), x, r->top, 1, 1, 0x550009u);
    if (r->bottom - 1 > r->top)
      g->patblt(DC32(dc), x + ((r->bottom - r->top + 1) & 1), r->bottom - 1, 1,
                1, 0x550009u);
  }
  for (int y = r->top + 2; y < r->bottom - 1; y += 2) {
    g->patblt(DC32(dc), r->left, y, 1, 1, 0x550009u);
    if (r->right - 1 > r->left)
      g->patblt(DC32(dc), r->right - 1, y + ((r->right - r->left + 1) & 1) - 1,
                1, 1, 0x550009u);
  }
  return 1;
}
static void arrow(HDC dc, RECT b, UINT dir, int color) {
  int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
  int size = (b.right - b.left < b.bottom - b.top ? b.right - b.left
                                                  : b.bottom - b.top) /
             4;
  if (size < 2)
    size = 2;
  if (size > 4)
    size = 4;
  for (int i = 0; i < size; i++) {
    RECT a;
    if (dir == DFCS_SCROLLUP)
      a = (RECT){cx - i, cy - size / 2 + i, cx + i + 1, cy - size / 2 + i + 1};
    else if (dir == DFCS_SCROLLDOWN || dir == DFCS_SCROLLCOMBOBOX)
      a = (RECT){cx - (size - 1 - i), cy - size / 2 + i, cx + size - i,
                 cy - size / 2 + i + 1};
    else if (dir == DFCS_SCROLLLEFT)
      a = (RECT){cx - size / 2 + i, cy - i, cx - size / 2 + i + 1, cy + i + 1};
    else
      a = (RECT){cx - size / 2 + i, cy - (size - 1 - i),
                 cx - size / 2 + i + 1, cy + size - i};
    sys_rect(dc, a, color);
  }
}
static void radio(HDC dc, RECT b, UINT state) {
  int ox = b.left + (b.right - b.left - 12) / 2,
      oy = b.top + (b.bottom - b.top - 12) / 2;
  int fill = state & (DFCS_PUSHED | DFCS_INACTIVE) ? COLOR_BTNFACE : COLOR_WINDOW;
  for (int y = 0; y < 12; y++)
    for (int x = 0; x < 12; x++) {
      int dx = 2 * x - 11, dy = 2 * y - 11, d2 = dx * dx + dy * dy;
      int color = -1;
      BOOL upper = x + y < 11;
      if (d2 <= 144 && d2 > 96)
        color = upper ? COLOR_BTNSHADOW : COLOR_BTNHIGHLIGHT;
      else if (d2 <= 96 && d2 > 56)
        color = upper ? COLOR_3DDKSHADOW : COLOR_3DLIGHT;
      else if (d2 <= 56)
        color = (state & DFCS_CHECKED) && d2 <= 12
                    ? (state & DFCS_INACTIVE ? COLOR_BTNSHADOW
                                             : COLOR_WINDOWTEXT)
                    : fill;
      if (color >= 0)
        sys_rect(dc, (RECT){ox + x, oy + y, ox + x + 1, oy + y + 1}, color);
    }
}
static void check_mark(HDC dc, RECT b, int color) {
  /* Seven-column classic check, three pixels tall per column. */
  int x = (b.left + b.right) / 2 - 3, y = (b.top + b.bottom) / 2 - 1;
  static const int rise[7] = {2, 3, 4, 3, 2, 1, 0};
  for (int i = 0; i < 7; i++)
    sys_rect(dc, (RECT){x + i, y + rise[i] - 2, x + i + 1, y + rise[i] + 1},
             color);
}
BOOL W32API DrawFrameControl(HDC dc, RECT *r, UINT kind, UINT state) {
  U32_GUARD;
  if (!dc || !r)
    return 0;
  RECT b = *r;
  int ink = state & DFCS_INACTIVE ? COLOR_BTNSHADOW : COLOR_BTNTEXT;
  if (kind == DFC_BUTTON) {
    UINT type = state & 0xff;
    if (type == DFCS_BUTTONPUSH) {
      sys_rect(dc, b, COLOR_BTNFACE);
      if (state & DFCS_PUSHED) {
        FrameRect(dc, &b, GetSysColorBrush(COLOR_WINDOWFRAME));
        InflateRect(&b, -1, -1);
        FrameRect(dc, &b, GetSysColorBrush(COLOR_BTNSHADOW));
        InflateRect(&b, -1, -1);
      } else if (state & DFCS_FLAT)
        DrawEdge(dc, &b, BDR_SUNKENOUTER, BF_RECT | BF_ADJUST | BF_FLAT);
      else
        DrawEdge(dc, &b, EDGE_RAISED, BF_RECT | BF_ADJUST);
    } else if (type == DFCS_BUTTONRADIO) {
      radio(dc, b, state);
    } else {
      DrawEdge(dc, &b, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
      BOOL gray = (state & (DFCS_PUSHED | DFCS_INACTIVE)) ||
                  ((type == DFCS_BUTTON3STATE) && (state & DFCS_CHECKED));
      sys_rect(dc, b, gray ? COLOR_BTNFACE : COLOR_WINDOW);
      if (state & DFCS_CHECKED)
        check_mark(dc, b,
                   type == DFCS_BUTTON3STATE || (state & DFCS_INACTIVE)
                       ? COLOR_BTNSHADOW
                       : COLOR_WINDOWTEXT);
    }
  } else if (kind == DFC_SCROLL || kind == DFC_CAPTION) {
    sys_rect(dc, b, COLOR_BTNFACE);
    RECT inner = b;
    if ((state & DFCS_PUSHED) && kind == DFC_SCROLL)
      DrawEdge(dc, &inner, BDR_SUNKENOUTER, BF_RECT | BF_ADJUST | BF_FLAT);
    else
      DrawEdge(dc, &inner, state & DFCS_PUSHED ? EDGE_SUNKEN : EDGE_RAISED,
               BF_RECT | BF_ADJUST);
    if (state & DFCS_PUSHED)
      OffsetRect(&inner, 1, 1);
    if (kind == DFC_SCROLL) {
      if (state & DFCS_INACTIVE) {
        RECT shade = inner;
        OffsetRect(&shade, 1, 1);
        arrow(dc, shade, state & 0xff, COLOR_BTNHIGHLIGHT);
      }
      arrow(dc, inner, state & 0xff, ink);
    } else {
      int cx = (inner.left + inner.right) / 2, cy = (inner.top + inner.bottom) / 2;
      UINT glyph = state & 0xff;
      if (glyph == DFCS_CAPTIONCLOSE)
        for (int i = -3; i <= 3; i++) {
          sys_rect(dc, (RECT){cx + i, cy + i, cx + i + 2, cy + i + 1}, ink);
          sys_rect(dc, (RECT){cx + i, cy - i, cx + i + 2, cy - i + 1}, ink);
        }
      else if (glyph == DFCS_CAPTIONMIN)
        sys_rect(dc, (RECT){cx - 3, cy + 2, cx + 3, cy + 4}, ink);
      else {
        RECT box = {cx - 4, cy - 4, cx + 5, cy + 4};
        FrameRect(dc, &box, GetSysColorBrush(ink));
        sys_rect(dc, (RECT){box.left, box.top + 1, box.right, box.top + 2},
                 ink);
      }
    }
  } else if (kind == DFC_MENU) {
    UINT glyph = state & 0xff;
    if (glyph == DFCS_MENUARROW)
      arrow(dc, b, DFCS_SCROLLRIGHT, COLOR_MENUTEXT);
    else if (glyph == DFCS_MENUCHECK)
      check_mark(dc, b, COLOR_MENUTEXT);
    else {
      int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
      sys_rect(dc, (RECT){cx - 2, cy - 1, cx + 2, cy + 2}, COLOR_MENUTEXT);
      sys_rect(dc, (RECT){cx - 1, cy - 2, cx + 1, cy + 3}, COLOR_MENUTEXT);
    }
  } else
    return u32_fail(87);
  if (state & DFCS_ADJUSTRECT)
    *r = b;
  return !!u32_graphics();
}
BOOL W32API DrawStateA(HDC dc, HBRUSH brush, DRAWSTATEPROC proc, LPARAM data,
                       WPARAM n, int x, int y, int cx, int cy, UINT flags) {
  U32_GUARD;
  (void)brush;
  UINT type = flags & 15;
  RECT r = {x, y, x + cx, y + cy};
  if (type == DST_TEXT || type == DST_PREFIXTEXT) {
    const char *s = (const char *)data;
    if (!s)
      return 0;
    if (!cx || !cy) {
      const struct u32_font_info *f = u32_font_info(0);
      int len = n ? (int)n : (int)w32_strlen(s);
      if (!cx)
        r.right = x + u32_text_width(f, s, len);
      if (!cy)
        r.bottom = y + f->height;
    }
    UINT text = 0x20u | (type == DST_TEXT ? 0x800u : 0u);
    u32_mnemonic_text(dc, r, s, 0, GetSysColor(COLOR_BTNTEXT), text,
                      !!(flags & DSS_DISABLED));
    return 1;
  }
  if (type == DST_ICON)
    return DrawIconEx(dc, x, y, (HICON)data, cx, cy, 0, 0, 3);
  if (type == DST_COMPLEX && proc)
    return proc(dc, data, n, cx, cy);
  return u32_fail(120);
}

/* ---- DrawText family (layout by the f5-10B engine) --------------------- */
int W32API DrawTextExA(HDC dc, char *s, int n, RECT *r, UINT flags,
                       void *params) {
  U32_GUARD;
  const struct u32_graphics *g = u32_graphics();
  if (!g || !dc || !r || !s)
    return u32_fail(87);
  return g->draw_text(DC32(dc), s, n, r, flags, 0, params);
}
int W32API DrawTextA(HDC dc, const char *s, int n, RECT *r, UINT flags) {
  return DrawTextExA(dc, (char *)s, n, r, flags, 0);
}
int W32API DrawTextExW(HDC dc, WCHAR *s, int n, RECT *r, UINT flags,
                       void *params) {
  U32_GUARD;
  const struct u32_graphics *g = u32_graphics();
  if (!g || !dc || !r || !s)
    return u32_fail(87);
  return g->draw_text(DC32(dc), s, n, r, flags, 1, params);
}
int W32API DrawTextW(HDC dc, const WCHAR *s, int n, RECT *r, UINT flags) {
  return DrawTextExW(dc, (WCHAR *)s, n, r, flags, 0);
}
static LONG tabbed(HDC dc, int x, int y, const char *s, int n, int count,
                   const int *tabs, int origin, BOOL draw) {
  const struct u32_graphics *g = u32_graphics();
  if (!g || !g->extent || !dc || (!s && n))
    return 0;
  struct u32_textmetric tm;
  int height = g->metrics && g->metrics(DC32(dc), &tm) ? tm.height : 13;
  int average = g->metrics && g->metrics(DC32(dc), &tm) ? tm.average : 6;
  if (n < 0)
    n = (int)w32_strlen(s);
  int pen = x, at = 0;
  while (at <= n) {
    int end = at;
    while (end < n && s[end] != '\t')
      end++;
    U32SIZE size = {0, 0};
    if (end > at)
      g->extent(DC32(dc), s + at, end - at, &size);
    if (draw && end > at && g->text_out)
      g->text_out(DC32(dc), pen, y, s + at, end - at);
    pen += size.cx;
    if (end >= n)
      break;
    /* Next stop: explicit array, a repeated interval, or 8 averages. */
    int next = 0, rel = pen - origin;
    if (count <= 0 || !tabs) {
      int step = 8 * average;
      next = origin + (rel / step + 1) * step;
    } else if (count == 1) {
      int step = tabs[0] > 0 ? tabs[0] : 8 * average;
      next = origin + (rel / step + 1) * step;
    } else {
      next = pen;
      for (int i = 0; i < count; i++)
        if (tabs[i] > rel) {
          next = origin + tabs[i];
          break;
        }
    }
    pen = next;
    at = end + 1;
  }
  return (LONG)(((uint32_t)(pen - x) & 0xffffu) | ((uint32_t)height << 16));
}
DWORD W32API GetTabbedTextExtentA(HDC dc, const char *s, int n, int count,
                                  const int *tabs) {
  U32_GUARD;
  return (DWORD)tabbed(dc, 0, 0, s, n, count, tabs, 0, 0);
}
LONG W32API TabbedTextOutA(HDC dc, int x, int y, const char *s, int n,
                           int count, const int *tabs, int origin) {
  U32_GUARD;
  return tabbed(dc, x, y, s, n, count, tabs, origin, 1);
}
typedef BOOL(W32API *GRAYSTRINGPROC)(HDC, LPARAM, int);
BOOL W32API GrayStringA(HDC dc, HBRUSH brush, GRAYSTRINGPROC proc, LPARAM data,
                        int n, int x, int y, int cx, int cy) {
  U32_GUARD;
  (void)brush, (void)cx, (void)cy;
  const struct u32_graphics *g = u32_graphics();
  if (!g || !dc)
    return 0;
  /* A solid gray text colour exists (COLOR_GRAYTEXT), so the text is drawn
   * directly in that colour, as Windows does for such displays. */
  DWORD old = g->text_color(DC32(dc), GetSysColor(COLOR_GRAYTEXT));
  BOOL ok;
  if (proc)
    ok = proc(dc, data, n);
  else {
    const char *s = (const char *)data;
    ok = s && g->text_out &&
         g->text_out(DC32(dc), x, y, s, n ? n : (int)w32_strlen(s));
  }
  g->text_color(DC32(dc), old);
  return ok;
}

/* ---- caret: inverted directly on the surface, hidden around painting --- */
struct caret {
  DWORD tid;
  HWND hwnd;
  HANDLE bitmap;
  POINT pos;
  int width, height, hidden;
  BOOL drawn, paint_hidden;
  UINT blink;
};
static struct caret carets[16];
static struct caret *caret_for(DWORD tid, BOOL create) {
  for (UINT i = 0; i < 16; i++)
    if (carets[i].tid == tid)
      return &carets[i];
  if (!create)
    return 0;
  for (UINT i = 0; i < 16; i++)
    if (!carets[i].tid) {
      carets[i] = (struct caret){.tid = tid, .blink = u32_input_value(DESK_INPUT_BLINK)};
      return &carets[i];
    }
  return 0;
}
static void caret_xor(struct caret *c) {
  const struct u32_graphics *g = u32_graphics();
  HDC dc = g && IsWindow(c->hwnd) ? GetDC(c->hwnd) : 0;
  if (dc) {
    g->patblt(DC32(dc), c->pos.x, c->pos.y, c->width, c->height, 0x550009u);
    ReleaseDC(c->hwnd, dc);
  }
  c->drawn = !c->drawn;
}
static void W32API caret_timer(HWND h, UINT m, uintptr_t id, DWORD t) {
  (void)m, (void)id, (void)t;
  U32_GUARD;
  struct caret *c = caret_for(u32_tid(), 0);
  if (c && c->hwnd == h && !c->hidden && !c->paint_hidden)
    caret_xor(c);
}
#define CARET_TIMER ((uintptr_t)0xfffe)
BOOL W32API DestroyCaret(void) {
  U32_GUARD;
  struct caret *c = caret_for(u32_tid(), 0);
  if (!c || !c->hwnd)
    return 0;
  if (c->drawn)
    caret_xor(c);
  if (IsWindow(c->hwnd))
    KillTimer(c->hwnd, CARET_TIMER);
  UINT blink = c->blink;
  *c = (struct caret){.tid = c->tid, .blink = blink};
  return 1;
}
BOOL W32API CreateCaret(HWND h, HANDLE bitmap, int w, int height) {
  U32_GUARD;
  struct u32_window *win = u32_window(h);
  if (!win || win->tid != u32_tid() || w < 0 || height < 0)
    return u32_fail(1400);
  DestroyCaret();
  struct caret *c = caret_for(u32_tid(), 1);
  if (!c)
    return u32_fail(8);
  c->hwnd = h;
  c->bitmap = bitmap;
  c->width = w ? w : GetSystemMetrics(6) * 2;
  c->height = height ? height : u32_font_info(0)->height;
  c->hidden = 1;
  c->drawn = 0;
  c->pos = (POINT){0, 0};
  if (c->blink && c->blink != 0xffffffffu)
    SetTimer(h, CARET_TIMER, c->blink, caret_timer);
  return 1;
}
BOOL W32API ShowCaret(HWND h) {
  U32_GUARD;
  struct caret *c = caret_for(u32_tid(), 0);
  if (!c || !c->hwnd || (h && c->hwnd != h))
    return 0;
  if (c->hidden > 0)
    c->hidden--;
  if (!c->hidden && !c->drawn && !c->paint_hidden)
    caret_xor(c);
  return 1;
}
BOOL W32API HideCaret(HWND h) {
  U32_GUARD;
  struct caret *c = caret_for(u32_tid(), 0);
  if (!c || !c->hwnd || (h && c->hwnd != h))
    return 0;
  if (c->drawn)
    caret_xor(c);
  c->hidden++;
  return 1;
}
BOOL W32API SetCaretPos(int x, int y) {
  U32_GUARD;
  struct caret *c = caret_for(u32_tid(), 0);
  if (!c || !c->hwnd)
    return 0;
  if (c->pos.x == x && c->pos.y == y)
    return 1;
  BOOL was = c->drawn;
  if (was)
    caret_xor(c);
  c->pos = (POINT){x, y};
  if (!c->hidden && !c->paint_hidden)
    caret_xor(c);
  return 1;
}
BOOL W32API GetCaretPos(POINT *p) {
  U32_GUARD;
  struct caret *c = caret_for(u32_tid(), 0);
  if (!c || !c->hwnd || !p)
    return 0;
  *p = c->pos;
  return 1;
}
void u32_caret_preferences(void)
{
  UINT blink=u32_input_value(DESK_INPUT_BLINK);
  for(UINT i=0;i<sizeof(carets)/sizeof(carets[0]);i++)if(carets[i].tid) {
    struct caret *c=&carets[i];c->blink=blink;
    if(!blink && c->hwnd && !c->hidden && !c->paint_hidden && !c->drawn)caret_xor(c);
    if(c->hwnd) {KillTimer(c->hwnd,CARET_TIMER);if(blink)SetTimer(c->hwnd,CARET_TIMER,blink,caret_timer);}
  }
}
BOOL W32API SetCaretBlinkTime(UINT n) {
  U32_GUARD;
  struct caret *c = caret_for(u32_tid(), 1);
  if (!c)
    return 0;
  if(!u32_input_setting(DESK_INPUT_BLINK,n==0xffffffffu ? 0 : n,0))return 0;
  c->blink = n;
  if (c->hwnd) {
    KillTimer(c->hwnd, CARET_TIMER);
    if (n && n != 0xffffffffu)
      SetTimer(c->hwnd, CARET_TIMER, n, caret_timer);
  }
  return 1;
}
UINT W32API GetCaretBlinkTime(void) {
  U32_GUARD;
  struct caret *c = caret_for(u32_tid(), 0);
  (void)c;u32_input_refresh();UINT n=u32_input_value(DESK_INPUT_BLINK);return n ? n : 0xffffffffu;
}
/* BeginPaint/EndPaint bracket: the caret never ends up inside painted pixels. */
void u32_caret_paint(HWND h, HDC dc) {
  (void)dc;
  struct caret *c = caret_for(u32_tid(), 0);
  if (!c || c->hwnd != h)
    return;
  if (dc == 0) {
    if (c->drawn)
      caret_xor(c);
    c->paint_hidden = 1;
  } else {
    c->paint_hidden = 0;
    if (!c->hidden && !c->drawn)
      caret_xor(c);
  }
}
void u32_caret_destroyed(HWND h) {
  for (UINT i = 0; i < 16; i++)
    if (carets[i].hwnd == h) {
      UINT blink = carets[i].blink;
      DWORD tid = carets[i].tid;
      carets[i] = (struct caret){.tid = tid, .blink = blink};
    }
}

/* ---- personality-owned non-client bands --------------------------------- */
void u32_nonclient(struct u32_window *w, int *top, int *right, int *bottom) {
  *top = *right = *bottom = 0;
  if (!w)
    return;
  if (!(w->style & WS_CHILD))
    *top = u32_menu_height(w);
  /* A combo box's scroll styles belong to its list, not to the field. */
  if (!u32_stricmp(w->cls->name, "COMBOBOX"))
    return;
  if (w->style & WS_VSCROLL)
    *right = GetSystemMetrics(2);
  if (w->style & WS_HSCROLL)
    *bottom = GetSystemMetrics(3);
}
POINT u32_surface_origin(struct u32_window *root) {
  POINT p = u32_origin(root);
  int top, right, bottom;
  u32_nonclient(root, &top, &right, &bottom);
  p.y -= top;
  return p;
}
void u32_surface_size(struct u32_window *w, UINT *width, UINT *height) {
  RECT r;
  GetClientRect(w->handle, &r);
  int top, right, bottom;
  u32_nonclient(w, &top, &right, &bottom);
  int x = r.right + right, y = r.bottom + top + bottom;
  *width = x > 0 ? (UINT)x : 1;
  *height = y > 0 ? (UINT)y : 1;
}
/* Window rectangle of w in its root surface's coordinates. */
static RECT surface_rect(struct u32_window *w, struct u32_window **root_out) {
  struct u32_window *root = u32_root(w);
  *root_out = root;
  RECT r = {0, 0, 0, 0};
  if (!root)
    return r;
  POINT so = u32_surface_origin(root);
  if (w == root) {
    UINT width, height;
    u32_surface_size(root, &width, &height);
    return (RECT){0, 0, (LONG)width, (LONG)height};
  }
  GetWindowRect(w->handle, &r);
  OffsetRect(&r, -so.x, -so.y);
  return r;
}
HDC u32_band_dc(struct u32_window *w, RECT band, struct u32_window **root_out) {
  u32_gdi_ensure();
  struct u32_window *root;
  RECT whole = surface_rect(w, &root);
  *root_out = root;
  if (!root || !root->surface.pixels || !u32_gdi || !u32_gdi->acquire)
    return 0;
  OffsetRect(&band, whole.left, whole.top);
  RECT clip;
  if (!IntersectRect(&clip, &band, &whole))
    return 0;
  /* Child bands are clipped by every ancestor's client area. */
  POINT so = u32_surface_origin(root);
  for (struct u32_window *p = u32_window(w->parent); p;
       p = u32_window(p->parent)) {
    RECT client;
    GetClientRect(p->handle, &client);
    POINT o = u32_origin(p);
    OffsetRect(&client, o.x - so.x, o.y - so.y);
    if (!IntersectRect(&clip, &clip, &client))
      return 0;
  }
  RECT bounds = {0, 0, (LONG)root->surface.width, (LONG)root->surface.height};
  if (!IntersectRect(&clip, &clip, &bounds))
    return 0;
  HDC dc = u32_gdi->acquire(&root->surface, (POINT){whole.left, whole.top},
                            &clip, 1, 0, 0);
  if (dc)
    root->dcs++;
  return dc;
}
void u32_band_release(struct u32_window *root, HDC dc) {
  if (!dc || !root)
    return;
  RECT touched[U32_RECTS];
  UINT n = u32_gdi && u32_gdi->release ? u32_gdi->release(dc, touched, U32_RECTS)
                                       : 0;
  if (n > U32_RECTS)
    n = 0;
  for (UINT j = 0; j < n; j++)
    u32_region_add(&root->damage, touched[j]);
  if (root->dcs)
    root->dcs--;
  u32_flush();
}
static void frame_metrics(struct u32_window *w, int *bx, int *by) {
  u32_frame(w->style, w->exstyle, bx, by);
}
/* Draws the classic non-client of w: child borders and scroll bars, and the
 * menu bar and scroll bars of top-level surfaces. */
void u32_paint_frame(HWND h) {
  struct u32_window *w = u32_window(h);
  if (!w)
    return;
  int top, right, bottom;
  u32_nonclient(w, &top, &right, &bottom);
  BOOL child = !!(w->style & WS_CHILD);
  BOOL border = child && (w->style & (WS_BORDER | WS_DLGFRAME | WS_THICKFRAME) ||
                          (w->exstyle & (WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
                                         WS_EX_DLGMODALFRAME)));
  if (!border && !top && !right && !bottom)
    return;
  if (top)
    u32_menu_paint(w);
  if (!border && !right && !bottom)
    return;
  struct u32_window *root;
  RECT whole = surface_rect(w, &root);
  RECT local = {0, 0, whole.right - whole.left, whole.bottom - whole.top};
  HDC dc = u32_band_dc(w, local, &root);
  if (!dc)
    return;
  RECT r = local;
  int bx = 0, by = 0;
  if (child) {
    frame_metrics(w, &bx, &by);
    if (w->exstyle & WS_EX_DLGMODALFRAME)
      DrawEdge(dc, &r, EDGE_RAISED, BF_RECT | BF_ADJUST);
    if (w->style & (WS_BORDER | WS_DLGFRAME | WS_THICKFRAME)) {
      FrameRect(dc, &r, GetSysColorBrush(COLOR_WINDOWFRAME));
      InflateRect(&r, -1, -1);
    }
    if (w->exstyle & WS_EX_CLIENTEDGE)
      DrawEdge(dc, &r, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
    else if (w->exstyle & WS_EX_STATICEDGE)
      DrawEdge(dc, &r, BDR_SUNKENOUTER, BF_RECT | BF_ADJUST);
  } else {
    r.top += top;
  }
  if (right) {
    RECT bar = {r.right - right, r.top, r.right, r.bottom - bottom};
    u32_scroll_paint(w, dc, SB_VERT, bar);
  }
  if (bottom) {
    RECT bar = {r.left, r.bottom - bottom, r.right - right, r.bottom};
    u32_scroll_paint(w, dc, SB_HORZ, bar);
  }
  if (right && bottom)
    sys_rect(dc, (RECT){r.right - right, r.bottom - bottom, r.right, r.bottom},
             COLOR_BTNFACE);
  u32_band_release(root, dc);
}
UINT u32_hit_test(HWND h, POINT screen) {
  struct u32_window *w = u32_window(h);
  if (!w)
    return HTNOWHERE;
  RECT window;
  GetWindowRect(h, &window);
  if (!(w->style & WS_CHILD)) {
    struct u32_window *root = w;
    POINT so = u32_surface_origin(root);
    UINT width, height;
    u32_surface_size(root, &width, &height);
    window = (RECT){so.x, so.y, so.x + (LONG)width, so.y + (LONG)height};
  }
  if (!PtInRect(&window, screen))
    return HTNOWHERE;
  POINT o = u32_origin(w);
  RECT client;
  GetClientRect(h, &client);
  OffsetRect(&client, o.x, o.y);
  if (PtInRect(&client, screen))
    return HTCLIENT;
  int top, right, bottom;
  u32_nonclient(w, &top, &right, &bottom);
  if (top && screen.y < client.top && screen.y >= client.top - top)
    return HTMENU;
  if (right && screen.x >= client.right && screen.x < client.right + right &&
      screen.y >= client.top && screen.y < client.bottom)
    return HTVSCROLL;
  if (bottom && screen.y >= client.bottom && screen.y < client.bottom + bottom &&
      screen.x >= client.left && screen.x < client.right)
    return HTHSCROLL;
  return HTBORDER;
}

/* ---- DefWindowProc classic defaults ------------------------------------- */
static POINT point_of(LPARAM lp) {
  return (POINT){(int16_t)lp, (int16_t)(lp >> 16)};
}
LRESULT u32_nc_mouse(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  POINT p = point_of(lp);
  if (wp == HTMENU && (m == WM_NCLBUTTONDOWN || m == WM_NCLBUTTONDBLCLK))
    return SendMessageA(h, WM_SYSCOMMAND, SC_MOUSEMENU | HTMENU, lp);
  if ((wp == HTVSCROLL || wp == HTHSCROLL) &&
      (m == WM_NCLBUTTONDOWN || m == WM_NCLBUTTONDBLCLK))
    u32_scroll_track(h, wp == HTVSCROLL ? SB_VERT : SB_HORZ, p);
  if (m == WM_NCRBUTTONUP)
    SendMessageA(h, WM_CONTEXTMENU, (WPARAM)h, lp);
  return 0;
}
/* ---- G15: WM_HELP and What's This? ----
 * Windows 98 (RK-keys; the WM_HELP, WM_SYSCOMMAND/SC_CONTEXTHELP and
 * DefWindowProc documentation): F1 sends WM_HELP with a HELPINFO
 * (HELPINFO_WINDOW) to the window with the keyboard focus, and DefWindowProc
 * passes it to the parent of a child window; Shift+F1 is SC_CONTEXTHELP, which
 * turns the pointer into the help arrow (IDC_HELP) until the next click, and
 * that click sends WM_HELP for the window under the pointer (no click reaches
 * it). Esc ends the mode. There is no WinHelp engine: a WM_HELP nobody
 * handles does nothing. */
#if defined(__i386__)
_Static_assert(sizeof(HELPINFO) == 28, "PE32 HELPINFO");
#endif
#define IDC_HELP_ID 32651u
static HWND help_mode;
static HCURSOR help_cursor_before;
BOOL u32_help_mode(void) { return help_mode != 0; }
LRESULT u32_help_send(HWND target, HWND item, int control, POINT screen) {
  struct u32_window *w = u32_window(item);
  HELPINFO hi;
  hi.cbSize = sizeof(hi);
  hi.iContextType = HELPINFO_WINDOW;
  hi.iCtrlId = control;
  hi.hItemHandle = item;
  hi.dwContextId = w ? w->help_id : 0;
  hi.MousePos = screen;
  return SendMessageA(target, WM_HELP, 0, (LPARAM)&hi);
}
static void help_begin(HWND h) {
  if (help_mode)
    return;
  help_mode = h;
  help_cursor_before = SetCursor((HCURSOR)(uintptr_t)IDC_HELP_ID);
}
void u32_help_end(void) {
  if (!help_mode)
    return;
  help_mode = 0;
  SetCursor(help_cursor_before);
  help_cursor_before = 0;
}
void u32_help_click(HWND h, POINT screen) {
  struct u32_window *w = u32_window(h);
  u32_help_end();
  if (w)
    u32_help_send(h, h, (int)w->id, screen);
}
LRESULT u32_classic_default(HWND h, UINT m, WPARAM wp, LPARAM lp,
                            BOOL *handled) {
  *handled = 1;
  switch (m) {
  case WM_KEYDOWN:
    if (wp == VK_F1 && GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0) {
      if (GetKeyState(VK_SHIFT) < 0)
        SendMessageA(h, WM_SYSCOMMAND, SC_CONTEXTHELP, 0);
      else {
        struct u32_window *w = u32_window(h);
        POINT p;
        GetCursorPos(&p);
        u32_help_send(h, h, w ? (int)w->id : 0, p);
      }
      return 0;
    }
    *handled = 0;
    return 0;
  case WM_HELP: {
    struct u32_window *w = u32_window(h);
    if (w && (w->style & WS_CHILD) && w->parent)
      return SendMessageA(w->parent, m, wp, lp);
    return 0;
  }
  case WM_NCHITTEST:
    return u32_hit_test(h, point_of(lp));
  case WM_NCLBUTTONDOWN:
  case WM_NCLBUTTONDBLCLK:
  case WM_NCRBUTTONUP:
    return u32_nc_mouse(h, m, wp, lp);
  case WM_RBUTTONUP: {
    POINT p = point_of(lp);
    ClientToScreen(h, &p);
    SendMessageA(h, WM_CONTEXTMENU, (WPARAM)h,
                 (LPARAM)((uint16_t)p.x | ((uint32_t)(uint16_t)p.y << 16)));
    return 0;
  }
  case WM_CONTEXTMENU: {
    struct u32_window *w = u32_window(h);
    if (w && (w->style & WS_CHILD) && w->parent)
      return SendMessageA(w->parent, m, wp, lp);
    return 0;
  }
  case WM_SYSKEYDOWN:
  case WM_SYSKEYUP:
  case WM_SYSCHAR:
    return u32_menu_key(h, m, wp, lp);
  case WM_SYSCOMMAND: {
    UINT command = (UINT)wp & 0xfff0u;
    if (command == SC_KEYMENU || command == SC_MOUSEMENU)
      return u32_menu_command(h, wp, lp);
    if (command == SC_CONTEXTHELP) {
      help_begin(h);
      return 0;
    }
    /* Interactive move/size belongs to the desktop; protocol v3 has no
     * request for it, so the command is accepted without effect. */
    if (command == SC_MOVE || command == SC_SIZE)
      return 0;
    *handled = 0;
    return 0;
  }
  case WM_CTLCOLORMSGBOX:
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLORDLG:
  case WM_CTLCOLORSCROLLBAR:
  case WM_CTLCOLORSTATIC: {
    const struct u32_graphics *g = u32_graphics();
    BOOL window = m == WM_CTLCOLOREDIT || m == WM_CTLCOLORLISTBOX;
    int back = window                       ? COLOR_WINDOW
               : m == WM_CTLCOLORSCROLLBAR ? COLOR_SCROLLBAR
                                            : COLOR_BTNFACE;
    if (g && wp) {
      g->text_color(DC32(wp), GetSysColor(window ? COLOR_WINDOWTEXT
                                                 : COLOR_BTNTEXT));
      g->bk_color(DC32(wp), GetSysColor(back));
    }
    return (LRESULT)GetSysColorBrush(back);
  }
  default:
    *handled = 0;
    return 0;
  }
}
void u32_classic_destroy(HWND h) {
  u32_control_cleanup(h);
  u32_dialog_destroyed(h);
  u32_menu_destroy_window(h);
  u32_caret_destroyed(h);
}
