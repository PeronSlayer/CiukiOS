/* Standard controls in the Windows classic look: BUTTON, STATIC, EDIT,
 * LISTBOX and COMBOBOX (drop-down list as a protocol v3 popup window).
 * comctl32 classes come from the reused Wine comctl32 (f5-14B). Text is
 * CP1252, indexed by byte; layout uses the selected font's metrics.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "unicode.h"
#define TEXT_MAX 30000u
#define ITEMS_MAX 8192u
#define DC32(dc) U32_PTR(dc)
#define LOWORD_SB(x) ((UINT)(x) & 0xffffu)
enum { BUTTON, EDIT, STATIC, LISTBOX, SCROLLBAR, COMBOBOX, COMBOLIST };
static const char *const class_names[] = {"BUTTON",  "EDIT",      "STATIC",
                                          "LISTBOX", "SCROLLBAR", "COMBOBOX",
                                          "ComboLBox"};
struct item {
  char *text;
  uintptr_t data;
  UINT height;
  BOOL selected;
};
struct control {
  UINT kind;
  HFONT font;
  /* BUTTON / STATIC */
  UINT check;
  BOOL pressed, tracking, focused;
  HANDLE image;
  /* text */
  char *text, *undo;
  UINT undo_start, undo_end;
  UINT limit, anchor_pos, caret_pos;
  int first_line, xoffset, margin_left, margin_right;
  BOOL modified, readonly, selecting, rect_set, vertical_bar, horizontal_bar;
  char password;
  RECT format;
  int tabs[32];
  UINT tab_count;
  UINT *lines;
  UINT line_count, line_capacity;
  /* LISTBOX / COMBOBOX */
  struct item *items;
  UINT count, capacity, item_height, column_width;
  int selection, anchor, caret, top, extent, horizontal;
  HWND list, edit, combo;
  BOOL dropped, extended_ui;
  int drop_height, drop_width, field_height;
};
static struct control *control(HWND h) {
  struct u32_window *w = u32_window(h);
  return w ? w->control : 0;
}
static char *dup_text(const char *s, UINT max) {
  if (!s)
    s = "";
  UINT n = w32_strlen(s);
  if (n > max)
    n = max;
  char *p = u32_alloc(n + 1);
  if (p) {
    w32_copy(p, s, n);
    p[n] = 0;
  }
  return p;
}
static void notify(HWND h, UINT code) {
  struct u32_window *w = u32_window(h);
  if (w && w->parent)
    SendMessageA(w->parent, WM_COMMAND, (w->id & 0xffffu) | (code << 16),
                 (LPARAM)h);
}
static BOOL enabled(HWND h) { return IsWindowEnabled(h); }
static void changed(HWND h) { InvalidateRect(h, 0, 1); }
static const struct u32_font_info *font_of(struct control *c) {
  return u32_font_info(c->font);
}
/* WM_CTLCOLOR*: the parent may set the DC colours and return a brush. */
static HBRUSH ctl_color(HWND h, HDC dc, UINT m) {
  struct u32_window *w = u32_window(h);
  HWND parent = w ? (w->parent ? w->parent : w->owner) : 0;
  HBRUSH b = parent ? (HBRUSH)SendMessageA(parent, m, (WPARAM)dc, (LPARAM)h)
                    : 0;
  if (!b)
    b = (HBRUSH)DefWindowProcA(h, m, (WPARAM)dc, (LPARAM)h);
  return b;
}
static DWORD dc_text(HDC dc) {
  const struct u32_graphics *g = u32_graphics();
  return g && g->get_text_color ? g->get_text_color(DC32(dc))
                                : GetSysColor(COLOR_WINDOWTEXT);
}
static void fill(HDC dc, RECT r, HBRUSH b) {
  if (b && u32_gdi && u32_gdi->fill)
    u32_gdi->fill(dc, &r, b);
}
static DWORD select_font(HDC dc, struct control *c) {
  const struct u32_graphics *g = u32_graphics();
  return g && c->font ? g->select(DC32(dc), U32_PTR(c->font)) : 0;
}
static void restore_font(HDC dc, DWORD old) {
  const struct u32_graphics *g = u32_graphics();
  if (g && old)
    g->select(DC32(dc), old);
}
static void text_at(HDC dc, int x, int y, const char *s, int n, DWORD color) {
  const struct u32_graphics *g = u32_graphics();
  if (!g || !g->text_out || n <= 0)
    return;
  DWORD old = g->text_color(DC32(dc), color);
  int mode = g->bk_mode(DC32(dc), 1);
  g->text_out(DC32(dc), x, y, s, n);
  g->bk_mode(DC32(dc), mode);
  g->text_color(DC32(dc), old);
}

/* ---- BUTTON ----------------------------------------------------------- */
static UINT button_type(struct u32_window *w) { return w->style & BS_TYPEMASK; }
static BOOL is_push(UINT type) {
  return type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON;
}
static BOOL is_radio(UINT type) {
  return type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
}
static void button_paint(HWND h, HDC dc) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  RECT r;
  GetClientRect(h, &r);
  UINT type = button_type(w);
  BOOL focus = GetFocus() == h, disabled = !enabled(h);
  if (type == BS_OWNERDRAW) {
    DRAWITEMSTRUCT d = {4,
                        w->id,
                        0,
                        1,
                        (c->pressed ? 1u : 0u) | (focus ? 16u : 0u) |
                            (disabled ? 4u : 0u),
                        h,
                        dc,
                        r,
                        0};
    SendMessageA(w->parent, WM_DRAWITEM, w->id, (LPARAM)&d);
    return;
  }
  HBRUSH brush = ctl_color(h, dc, WM_CTLCOLORBTN);
  DWORD ink = disabled ? GetSysColor(COLOR_GRAYTEXT) : dc_text(dc);
  c = control(h);
  if (!c)
    return;
  const struct u32_font_info *f = font_of(c);
  UINT align = w->style & BS_CENTER;
  if (type == BS_GROUPBOX) {
    fill(dc, r, brush);
    RECT edge = r;
    edge.top += f->height / 2;
    DrawEdge(dc, &edge, EDGE_ETCHED, BF_RECT);
    if (c->text[0]) {
      int width = u32_text_width(f, c->text, (int)w32_strlen(c->text));
      RECT title = {8, 0, 8 + width + 4, f->height};
      if (align == BS_RIGHT)
        OffsetRect(&title, r.right - 16 - (title.right - title.left), 0);
      else if (align == BS_CENTER)
        OffsetRect(&title, (r.right - 16 - (title.right - title.left)) / 2, 0);
      fill(dc, title, brush);
      InflateRect(&title, -2, 0);
      u32_mnemonic_text(dc, title, c->text, c->font, ink, 0x20u, disabled);
    }
    return;
  }
  if (is_push(type) || (w->style & BS_PUSHLIKE)) {
    BOOL down = c->pressed || ((w->style & BS_PUSHLIKE) && c->check);
    RECT b = r;
    if (type == BS_DEFPUSHBUTTON) {
      FrameRect(dc, &b, GetSysColorBrush(COLOR_WINDOWFRAME));
      InflateRect(&b, -1, -1);
    }
    DrawFrameControl(dc, &b, DFC_BUTTON,
                     DFCS_BUTTONPUSH | (down ? DFCS_PUSHED : 0) |
                         (w->style & BS_FLAT ? DFCS_FLAT : 0));
    RECT text = b;
    InflateRect(&text, -4, -3);
    if (down)
      OffsetRect(&text, 1, 1);
    if (c->image && (w->style & BS_ICON))
      DrawIconEx(dc, (text.left + text.right - 32) / 2,
                 (text.top + text.bottom - 32) / 2, (HICON)c->image, 32, 32, 0,
                 0, 3);
    else {
      UINT flags = (align == BS_LEFT ? 0u : align == BS_RIGHT ? 2u : 1u) |
                   (w->style & BS_MULTILINE ? 0x10u : 0x24u);
      u32_mnemonic_text(dc, text, c->text, c->font, ink, flags, disabled);
    }
    if (focus) {
      RECT ring = b;
      InflateRect(&ring, -3, -3);
      DrawFocusRect(dc, &ring);
    }
    return;
  }
  /* Check box, radio button and three-state: 13-pixel glyph, then text. */
  fill(dc, r, brush);
  BOOL left_text = !!(w->style & BS_LEFTTEXT) || align == BS_RIGHT;
  RECT box = {r.left, (r.bottom - r.top - 13) / 2, r.left + 13,
              (r.bottom - r.top - 13) / 2 + 13};
  if (left_text)
    OffsetRect(&box, r.right - 13, 0);
  UINT state = (is_radio(type) ? DFCS_BUTTONRADIO
                : type == BS_3STATE || type == BS_AUTO3STATE
                    ? DFCS_BUTTON3STATE
                    : DFCS_BUTTONCHECK) |
               (c->check ? DFCS_CHECKED : 0) | (c->pressed ? DFCS_PUSHED : 0) |
               (disabled ? DFCS_INACTIVE : 0);
  if (state & DFCS_BUTTON3STATE && c->check == BST_CHECKED)
    state &= ~DFCS_BUTTON3STATE;
  DrawFrameControl(dc, &box, DFC_BUTTON, state);
  RECT text = r;
  if (left_text)
    text.right -= 17;
  else
    text.left += 17;
  UINT flags = w->style & BS_MULTILINE ? 0x10u : 0x24u;
  u32_mnemonic_text(dc, text, c->text, c->font, ink, flags, disabled);
  if (focus && c->text[0]) {
    int width = u32_text_width(f, c->text, (int)w32_strlen(c->text));
    RECT ring = {text.left - 1, (r.bottom - f->height) / 2 - 1,
                 text.left + width + 2, (r.bottom + f->height) / 2 + 1};
    if (ring.right > r.right)
      ring.right = r.right;
    DrawFocusRect(dc, &ring);
  }
}
static void radio_group_clear(HWND h) {
  struct u32_window *w = u32_window(h);
  if (!w || !w->parent)
    return;
  HWND at = GetNextDlgGroupItem(w->parent, h, 0);
  for (UINT i = 0; at && at != h && i < U32_WINDOWS; i++) {
    struct u32_window *o = u32_window(at);
    if (o && o->control && ((struct control *)o->control)->kind == BUTTON &&
        button_type(o) == BS_AUTORADIOBUTTON)
      SendMessageA(at, BM_SETCHECK, BST_UNCHECKED, 0);
    at = GetNextDlgGroupItem(w->parent, at, 0);
  }
}
static void button_click(HWND h) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  if (!w || !c)
    return;
  UINT type = button_type(w);
  if (type == BS_AUTOCHECKBOX)
    c->check = c->check ? BST_UNCHECKED : BST_CHECKED;
  else if (type == BS_AUTO3STATE)
    c->check = (c->check + 1) % 3;
  else if (type == BS_AUTORADIOBUTTON) {
    radio_group_clear(h);
    if ((c = control(h)))
      c->check = BST_CHECKED;
  }
  changed(h);
  notify(h, BN_CLICKED);
}
static LRESULT button_message(HWND h, UINT m, WPARAM wp, LPARAM lp,
                              BOOL *handled) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  UINT type = button_type(w);
  *handled = 1;
  switch (m) {
  case WM_GETDLGCODE:
    return DLGC_BUTTON | (type == BS_DEFPUSHBUTTON   ? DLGC_DEFPUSHBUTTON
                          : type == BS_PUSHBUTTON    ? DLGC_UNDEFPUSHBUTTON
                          : is_radio(type)           ? DLGC_RADIOBUTTON
                          : type == BS_GROUPBOX      ? DLGC_STATIC
                                                     : 0);
  case BM_GETCHECK:
    return is_push(type) || type == BS_GROUPBOX ? 0 : c->check;
  case BM_SETCHECK:
    if (is_push(type) || type == BS_GROUPBOX || type == BS_OWNERDRAW)
      return 0;
    c->check = wp > BST_INDETERMINATE ? BST_CHECKED : (UINT)wp;
    if (c->check == BST_INDETERMINATE && type != BS_3STATE &&
        type != BS_AUTO3STATE)
      c->check = BST_CHECKED;
    if (is_radio(type) && c->check)
      w->style |= WS_TABSTOP;
    changed(h);
    return 0;
  case BM_GETSTATE:
    return c->check | (c->pressed ? BST_PUSHED : 0) |
           (GetFocus() == h ? BST_FOCUS : 0);
  case BM_SETSTATE:
    if (c->pressed != !!wp) {
      c->pressed = !!wp;
      changed(h);
    }
    return 0;
  case BM_SETSTYLE:
    w->style = (w->style & ~0xffffu) | (wp & 0xffffu);
    if (lp)
      changed(h);
    return 0;
  case BM_CLICK:
    if (enabled(h) && type != BS_GROUPBOX) {
      SendMessageA(h, WM_LBUTTONDOWN, 1, 0);
      SendMessageA(h, WM_LBUTTONUP, 0, 0);
    }
    return 0;
  case BM_SETIMAGE: {
    HANDLE old = c->image;
    c->image = (HANDLE)lp;
    changed(h);
    return (LRESULT)old;
  }
  case BM_GETIMAGE:
    return (LRESULT)c->image;
  case WM_LBUTTONDOWN:
  case WM_LBUTTONDBLCLK:
    if (type == BS_GROUPBOX)
      return 0;
    if (m == WM_LBUTTONDBLCLK && (w->style & BS_NOTIFY) &&
        (is_push(type) || is_radio(type) || type == BS_OWNERDRAW)) {
      notify(h, BN_DBLCLK);
      return 0;
    }
    SetCapture(h);
    if (GetFocus() != h)
      SetFocus(h);
    if ((c = control(h))) {
      c->tracking = 1;
      c->pressed = 1;
      changed(h);
    }
    return 0;
  case WM_MOUSEMOVE:
    if (c->tracking) {
      RECT r;
      GetClientRect(h, &r);
      BOOL inside = PtInRect(&r, (POINT){(int16_t)lp, (int16_t)(lp >> 16)});
      if (inside != c->pressed) {
        c->pressed = inside;
        changed(h);
      }
    }
    return 0;
  case WM_LBUTTONUP: {
    BOOL pressed = c->pressed, tracking = c->tracking;
    c->pressed = 0;
    c->tracking = 0;
    if (GetCapture() == h)
      ReleaseCapture();
    changed(h);
    if (pressed && (tracking || lp == 0))
      button_click(h);
    return 0;
  }
  case WM_CAPTURECHANGED:
    if (c->tracking) {
      c->tracking = 0;
      c->pressed = 0;
      changed(h);
    }
    return 0;
  case WM_KEYDOWN:
    if (wp == VK_SPACE && type != BS_GROUPBOX && !c->pressed) {
      c->pressed = 1;
      changed(h);
    }
    return 0;
  case WM_KEYUP:
    if (wp == VK_SPACE && c->pressed) {
      c->pressed = 0;
      changed(h);
      button_click(h);
    }
    return 0;
  case WM_CHAR:
    /* Radio and check boxes toggle on '+'/'-'/'=' as in classic USER. */
    if (type == BS_AUTOCHECKBOX && (wp == '+' || wp == '='))
      SendMessageA(h, BM_SETCHECK, BST_CHECKED, 0), notify(h, BN_CLICKED);
    else if (type == BS_AUTOCHECKBOX && wp == '-')
      SendMessageA(h, BM_SETCHECK, BST_UNCHECKED, 0), notify(h, BN_CLICKED);
    return 0;
  case WM_SETFOCUS:
    if (w->style & BS_NOTIFY)
      notify(h, 6);
    /* Keyboard focus checks an auto radio button (not a mouse press). */
    if (type == BS_AUTORADIOBUTTON && !c->check && GetKeyState(1) >= 0 &&
        u32_dialog_of(h) && !c->tracking && GetCapture() != h)
      button_click(h);
    changed(h);
    return 0;
  case WM_KILLFOCUS:
    if (w->style & BS_NOTIFY)
      notify(h, 7);
    if (c->pressed && !c->tracking) {
      c->pressed = 0;
    }
    changed(h);
    return 0;
  default:
    *handled = 0;
    return 0;
  }
}

/* ---- STATIC ----------------------------------------------------------- */
static void static_paint(HWND h, HDC dc) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  RECT r;
  GetClientRect(h, &r);
  UINT type = w->style & SS_TYPEMASK;
  if (type == SS_OWNERDRAW) {
    DRAWITEMSTRUCT d = {3, w->id, 0, 1, 0, h, dc, r, 0};
    SendMessageA(w->parent, WM_DRAWITEM, w->id, (LPARAM)&d);
    return;
  }
  if (type >= SS_BLACKRECT && type <= SS_WHITEFRAME) {
    int color = (type - SS_BLACKRECT) % 3 == 0   ? COLOR_3DDKSHADOW
                : (type - SS_BLACKRECT) % 3 == 1 ? COLOR_BTNSHADOW
                                                 : COLOR_BTNHIGHLIGHT;
    if (type >= SS_BLACKFRAME)
      FrameRect(dc, &r, GetSysColorBrush(color));
    else
      u32_color_rect(dc, r, GetSysColor(color));
    return;
  }
  if (type == SS_ETCHEDHORZ || type == SS_ETCHEDVERT || type == SS_ETCHEDFRAME) {
    DrawEdge(dc, &r, EDGE_ETCHED,
             type == SS_ETCHEDHORZ   ? BF_TOP | BF_BOTTOM
             : type == SS_ETCHEDVERT ? BF_LEFT | BF_RIGHT
                                     : BF_RECT);
    return;
  }
  HBRUSH brush = ctl_color(h, dc, WM_CTLCOLORSTATIC);
  c = control(h);
  if (!c)
    return;
  fill(dc, r, brush);
  if (type == SS_ICON) {
    if (c->image) {
      int x = w->style & SS_CENTERIMAGE ? (r.right - 32) / 2 : 0,
          y = w->style & SS_CENTERIMAGE ? (r.bottom - 32) / 2 : 0;
      DrawIconEx(dc, x, y, (HICON)c->image, 32, 32, 0, 0, 3);
    }
    return;
  }
  if (type == SS_BITMAP) {
    const struct u32_graphics *g = u32_graphics();
    /* BITMAP: type, width, height first (pointer-size tail varies). */
    uint8_t bm[64];
    if (c->image && g && g->object && g->bitblt && g->memory_dc &&
        g->object(U32_PTR(c->image), sizeof(bm), bm)) {
      int width = (int)w32_u32(bm + 4), height = (int)w32_u32(bm + 8);
      DWORD mem = g->memory_dc(DC32(dc));
      DWORD old = mem ? g->select(mem, U32_PTR(c->image)) : 0;
      int x = w->style & SS_CENTERIMAGE ? (r.right - width) / 2 : 0,
          y = w->style & SS_CENTERIMAGE ? (r.bottom - height) / 2 : 0;
      if (old)
        g->bitblt(DC32(dc), x, y, width, height, mem, 0, 0, 0xcc0020u);
      if (old)
        g->select(mem, old);
      if (mem)
        g->delete_dc(mem);
    }
    return;
  }
  UINT flags = type == SS_CENTER ? 1u : type == SS_RIGHT ? 2u : 0u;
  if (type == SS_SIMPLE || type == SS_LEFTNOWORDWRAP)
    flags |= 0x20u;
  else
    flags |= 0x10u;
  if (w->style & SS_NOPREFIX)
    flags |= 0x800u;
  if (w->style & SS_CENTERIMAGE)
    flags |= 0x24u;
  UINT ellipsis = w->style & SS_WORDELLIPSIS;
  if (ellipsis == SS_ENDELLIPSIS)
    flags |= 0x8000u | 0x20u;
  else if (ellipsis == SS_PATHELLIPSIS)
    flags |= 0x4000u | 0x20u;
  else if (ellipsis == SS_WORDELLIPSIS)
    flags |= 0x40000u | 0x20u;
  u32_mnemonic_text(dc, r, c->text, c->font, dc_text(dc), flags,
                    !enabled(h));
}
static LRESULT static_message(HWND h, UINT m, WPARAM wp, LPARAM lp,
                              BOOL *handled) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  *handled = 1;
  switch (m) {
  case WM_GETDLGCODE:
    return DLGC_STATIC;
  case STM_SETICON:
  case STM_SETIMAGE: {
    HANDLE old = c->image;
    c->image = m == STM_SETICON ? (HANDLE)wp : (HANDLE)lp;
    changed(h);
    return (LRESULT)old;
  }
  case STM_GETICON:
  case STM_GETIMAGE:
    return (LRESULT)c->image;
  case WM_LBUTTONDOWN:
  case WM_LBUTTONDBLCLK:
    if (w->style & SS_NOTIFY)
      notify(h, m == WM_LBUTTONDOWN ? STN_CLICKED : STN_DBLCLK);
    return 0;
  case WM_ENABLE:
    changed(h);
    return 0;
  default:
    *handled = 0;
    return 0;
  }
}

/* ---- EDIT ------------------------------------------------------------- */
static BOOL multiline(struct u32_window *w) {
  return !!(w->style & ES_MULTILINE);
}
static UINT text_length(struct control *c) { return w32_strlen(c->text); }
static UINT sel_low(struct control *c) {
  return c->anchor_pos < c->caret_pos ? c->anchor_pos : c->caret_pos;
}
static UINT sel_high(struct control *c) {
  return c->anchor_pos > c->caret_pos ? c->anchor_pos : c->caret_pos;
}
static RECT edit_format(HWND h, struct control *c) {
  if (c->rect_set)
    return c->format;
  RECT r;
  GetClientRect(h, &r);
  struct u32_window *w = u32_window(h);
  BOOL border = w && (w->style & WS_BORDER) && !(w->exstyle & WS_EX_CLIENTEDGE);
  r.left += c->margin_left + (border ? 1 : 1);
  r.right -= c->margin_right + (border ? 1 : 1);
  if (!multiline(w)) {
    const struct u32_font_info *f = font_of(c);
    int space = r.bottom - f->height;
    r.top = space > 0 ? space / 2 : 0;
    r.bottom = r.top + f->height;
  } else
    r.top += border ? 1 : 0;
  return r;
}
static int char_width(struct control *c, const struct u32_font_info *f,
                      uint8_t ch) {
  return c->password ? f->widths[(uint8_t)c->password] : f->widths[ch];
}
static int span_width(struct control *c, const struct u32_font_info *f,
                      UINT from, UINT to) {
  int x = 0;
  for (UINT i = from; i < to && c->text[i]; i++)
    x += c->text[i] == '\t' ? 0 : char_width(c, f, (uint8_t)c->text[i]);
  return x;
}
/* Line starts: hard breaks at CR LF; soft wrap when multiline without
 * ES_AUTOHSCROLL (word wrap at the formatting rectangle width). */
static void layout(HWND h, struct control *c) {
  struct u32_window *w = u32_window(h);
  const struct u32_font_info *f = font_of(c);
  RECT fr = edit_format(h, c);
  int width = fr.right - fr.left;
  BOOL wrap = multiline(w) && !(w->style & ES_AUTOHSCROLL);
  UINT len = text_length(c);
  c->line_count = 0;
  UINT at = 0;
  for (;;) {
    if (c->line_count == c->line_capacity) {
      UINT cap = c->line_capacity ? c->line_capacity * 2 : 16;
      UINT *p = u32_alloc(cap * sizeof(UINT));
      if (!p)
        return;
      if (c->lines)
        w32_copy(p, c->lines, c->line_count * sizeof(UINT));
      u32_free(c->lines);
      c->lines = p;
      c->line_capacity = cap;
    }
    c->lines[c->line_count++] = at;
    if (!multiline(w))
      return;
    UINT end = at;
    while (end < len && c->text[end] != '\r' && c->text[end] != '\n')
      end++;
    UINT next = end;
    if (wrap && width > 0) {
      int x = 0;
      UINT space = 0;
      for (UINT i = at; i < end; i++) {
        x += char_width(c, f, (uint8_t)c->text[i]);
        if (c->text[i] == ' ')
          space = i + 1;
        if (x > width && i > at) {
          next = space > at ? space : i;
          break;
        }
      }
    }
    if (next < end) {
      at = next;
      continue;
    }
    if (end >= len)
      return;
    at = end + (c->text[end] == '\r' && c->text[end + 1] == '\n' ? 2 : 1);
  }
}
static UINT line_of(struct control *c, UINT pos) {
  UINT line = 0;
  for (UINT i = 1; i < c->line_count; i++)
    if (c->lines[i] <= pos)
      line = i;
  return line;
}
static UINT line_end(struct control *c, UINT line) {
  UINT end = line + 1 < c->line_count ? c->lines[line + 1] : text_length(c);
  while (end > c->lines[line] &&
         (c->text[end - 1] == '\n' || c->text[end - 1] == '\r'))
    end--;
  return end;
}
static int visible_lines(HWND h, struct control *c) {
  RECT fr = edit_format(h, c);
  int n = (fr.bottom - fr.top) / (font_of(c)->height ? font_of(c)->height : 13);
  return n > 0 ? n : 1;
}
static POINT pos_point(HWND h, struct control *c, UINT pos) {
  const struct u32_font_info *f = font_of(c);
  RECT fr = edit_format(h, c);
  UINT line = line_of(c, pos);
  int x = span_width(c, f, c->lines[line], pos);
  struct u32_window *w = u32_window(h);
  if (!multiline(w) && (w->style & (ES_CENTER | ES_RIGHT))) {
    int total = span_width(c, f, 0, text_length(c)), room = fr.right - fr.left;
    if (total < room)
      x += w->style & ES_RIGHT ? room - total : (room - total) / 2;
  }
  return (POINT){fr.left + x - c->xoffset,
                 fr.top + ((int)line - c->first_line) * f->height};
}
static UINT point_pos(HWND h, struct control *c, POINT p) {
  const struct u32_font_info *f = font_of(c);
  RECT fr = edit_format(h, c);
  int row = f->height ? (p.y - fr.top) / f->height : 0;
  if (p.y < fr.top)
    row = -1;
  int line = c->first_line + row;
  if (line < 0)
    line = 0;
  if (line >= (int)c->line_count)
    line = (int)c->line_count - 1;
  UINT at = c->lines[line], end = line_end(c, (UINT)line);
  int x = fr.left - c->xoffset;
  POINT origin = pos_point(h, c, at);
  x = origin.x;
  while (at < end) {
    int cw = char_width(c, f, (uint8_t)c->text[at]);
    if (p.x < x + cw / 2)
      break;
    x += cw;
    at++;
  }
  return at;
}
static void scroll_caret(HWND h, struct control *c) {
  struct u32_window *w = u32_window(h);
  const struct u32_font_info *f = font_of(c);
  RECT fr = edit_format(h, c);
  UINT line = line_of(c, c->caret_pos);
  if (multiline(w)) {
    int rows = visible_lines(h, c);
    if ((int)line < c->first_line)
      c->first_line = (int)line;
    else if ((int)line >= c->first_line + rows)
      c->first_line = (int)line - rows + 1;
  }
  if (!multiline(w) || (w->style & ES_AUTOHSCROLL)) {
    int x = span_width(c, f, c->lines[line], c->caret_pos),
        room = fr.right - fr.left;
    if (x - c->xoffset > room - 1)
      c->xoffset = x - room + room / 4;
    if (x < c->xoffset)
      c->xoffset = x - room / 4 > 0 ? x - room / 4 : 0;
    if (c->xoffset < 0)
      c->xoffset = 0;
  }
}
static void edit_scrollbars(HWND h, struct control *c) {
  struct u32_window *w = u32_window(h);
  if (!w || !multiline(w))
    return;
  /* Multi-line edit bars stay visible and are disabled when not needed. */
  if (c->vertical_bar) {
    SCROLLINFO s = {sizeof(s), SIF_ALL | SIF_DISABLENOSCROLL, 0,
                    (int)c->line_count - 1, (UINT)visible_lines(h, c),
                    c->first_line, 0};
    SetScrollInfo(h, SB_VERT, &s, 1);
  }
}
static void place_caret(HWND h) {
  struct control *c = control(h);
  if (!c || GetFocus() != h)
    return;
  POINT p = pos_point(h, c, c->caret_pos);
  SetCaretPos(p.x, p.y);
}
static void edit_refresh(HWND h) {
  struct control *c = control(h);
  if (!c)
    return;
  layout(h, c);
  scroll_caret(h, c);
  edit_scrollbars(h, c);
  changed(h);
  place_caret(h);
}
static void set_selection(HWND h, UINT anchor, UINT caret) {
  struct control *c = control(h);
  UINT len = text_length(c);
  c->anchor_pos = anchor > len ? len : anchor;
  c->caret_pos = caret > len ? len : caret;
  scroll_caret(h, c);
  changed(h);
  place_caret(h);
}
static BOOL edit_replace(HWND h, const char *insert, UINT n, BOOL undoable,
                         BOOL notify_change) {
  struct control *c = control(h);
  struct u32_window *w = u32_window(h);
  if (!c || !w)
    return 0;
  UINT len = text_length(c), a = sel_low(c), b = sel_high(c);
  UINT room = c->limit > len - (b - a) ? c->limit - (len - (b - a)) : 0;
  BOOL truncated = n > room;
  if (truncated)
    n = room;
  if (!n && a == b) {
    if (truncated)
      notify(h, EN_MAXTEXT);
    return 0;
  }
  if (!multiline(w) && !(w->style & ES_AUTOHSCROLL) && n) {
    const struct u32_font_info *f = font_of(c);
    RECT fr = edit_format(h, c);
    int width = span_width(c, f, 0, a) + span_width(c, f, b, len);
    for (UINT i = 0; i < n; i++)
      width += char_width(c, f, (uint8_t)insert[i]);
    if (width > fr.right - fr.left) {
      MessageBeep(0);
      return 0;
    }
  }
  char *p = u32_alloc(len - (b - a) + n + 1);
  if (!p)
    return 0;
  w32_copy(p, c->text, a);
  w32_copy(p + a, insert, n);
  w32_copy(p + a + n, c->text + b, len - b + 1);
  if (undoable) {
    u32_free(c->undo);
    c->undo = c->text;
    c->undo_start = a;
    c->undo_end = a + n;
  } else
    u32_free(c->text);
  c->text = p;
  c->anchor_pos = c->caret_pos = a + n;
  c->modified = 1;
  edit_refresh(h);
  if (notify_change) {
    notify(h, EN_UPDATE);
    notify(h, EN_CHANGE);
  }
  if (truncated)
    notify(h, EN_MAXTEXT);
  return 1;
}
static void edit_paint(HWND h, HDC dc) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  RECT client;
  GetClientRect(h, &client);
  BOOL static_look = c->readonly || !enabled(h);
  HBRUSH brush = ctl_color(h, dc, static_look ? WM_CTLCOLORSTATIC
                                              : WM_CTLCOLOREDIT);
  if (!(c = control(h)) || !(w = u32_window(h)))
    return;
  fill(dc, client, brush);
  DWORD ink = enabled(h) ? dc_text(dc) : GetSysColor(COLOR_GRAYTEXT);
  const struct u32_font_info *f = font_of(c);
  DWORD old = select_font(dc, c);
  RECT fr = edit_format(h, c);
  int rows = multiline(w) ? visible_lines(h, c) + 1 : 1;
  BOOL show_sel = GetFocus() == h || (w->style & ES_NOHIDESEL);
  UINT a = sel_low(c), b = sel_high(c);
  for (int row = 0; row < rows; row++) {
    int line = c->first_line + row;
    if (line >= (int)c->line_count)
      break;
    UINT start = c->lines[line], end = line_end(c, (UINT)line);
    POINT p = pos_point(h, c, start);
    char buffer[512];
    UINT at = start;
    int x = p.x;
    while (at < end) {
      UINT n = 0;
      BOOL selected = show_sel && at >= a && at < b;
      while (at + n < end && n < sizeof(buffer) &&
             (show_sel && at + n >= a && at + n < b) == selected) {
        buffer[n] = c->password ? c->password : c->text[at + n];
        n++;
      }
      int width = 0;
      for (UINT i = 0; i < n; i++)
        width += f->widths[(uint8_t)buffer[i]];
      if (x + width >= fr.left && x < fr.right) {
        if (selected)
          u32_color_rect(dc,
                         (RECT){x, p.y, x + width, p.y + f->height},
                         GetSysColor(COLOR_HIGHLIGHT));
        text_at(dc, x, p.y, buffer, (int)n,
                selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : ink);
      }
      x += width;
      at += n;
    }
  }
  restore_font(dc, old);
}
static UINT word_left(struct control *c, UINT pos) {
  while (pos && c->text[pos - 1] == ' ')
    pos--;
  while (pos && c->text[pos - 1] != ' ' && c->text[pos - 1] != '\n')
    pos--;
  return pos;
}
static UINT word_right(struct control *c, UINT pos) {
  UINT len = text_length(c);
  while (pos < len && c->text[pos] != ' ' && c->text[pos] != '\r')
    pos++;
  while (pos < len && c->text[pos] == ' ')
    pos++;
  return pos;
}
static void clipboard_copy(HWND h, struct control *c) {
  UINT a = sel_low(c), b = sel_high(c);
  if (a == b || c->password || !OpenClipboard(h))
    return;
  EmptyClipboard();
  HANDLE mem = GlobalAlloc(2, b - a + 1);
  char *p = mem ? GlobalLock(mem) : 0;
  if (p) {
    w32_copy(p, c->text + a, b - a);
    p[b - a] = 0;
    GlobalUnlock(mem);
    SetClipboardData(1, mem);
  }
  CloseClipboard();
}
static void clipboard_paste(HWND h) {
  if (!OpenClipboard(h))
    return;
  HANDLE mem = GetClipboardData(1);
  const char *p = mem ? GlobalLock(mem) : 0;
  if (p) {
    UINT n = 0;
    struct u32_window *w = u32_window(h);
    while (p[n] && n < TEXT_MAX && (multiline(w) || (p[n] != '\r' && p[n] != '\n')))
      n++;
    edit_replace(h, p, n, 1, 1);
    GlobalUnlock(mem);
  }
  CloseClipboard();
}
static LRESULT edit_message(HWND h, UINT m, WPARAM wp, LPARAM lp,
                            BOOL *handled) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  UINT len = text_length(c);
  *handled = 1;
  switch (m) {
  case WM_GETDLGCODE: {
    LRESULT code = DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTARROWS;
    if (multiline(w) && (w->style & ES_WANTRETURN))
      code |= DLGC_WANTALLKEYS;
    MSG *msg = (MSG *)lp;
    if (msg && msg->message == WM_KEYDOWN && msg->wParam == VK_TAB)
      code &= ~(LRESULT)DLGC_WANTALLKEYS;
    return code;
  }
  case WM_SETTEXT: {
    char *p = dup_text((const char *)lp, c->limit < TEXT_MAX ? c->limit : TEXT_MAX);
    if (!p)
      return 0;
    u32_free(c->text);
    u32_free(c->undo);
    c->text = p;
    c->undo = 0;
    c->anchor_pos = c->caret_pos = 0;
    c->first_line = c->xoffset = 0;
    c->modified = 0;
    edit_refresh(h);
    notify(h, EN_UPDATE);
    notify(h, EN_CHANGE);
    return 1;
  }
  case WM_GETTEXT: {
    if (!lp || !wp)
      return 0;
    UINT n = len < wp ? len : (UINT)wp - 1;
    w32_copy((void *)lp, c->text, n);
    ((char *)lp)[n] = 0;
    return n;
  }
  case WM_GETTEXTLENGTH:
    return len;
  case EM_GETSEL:
    if (wp)
      *(DWORD *)wp = sel_low(c);
    if (lp)
      *(DWORD *)lp = sel_high(c);
    return (LRESULT)((sel_low(c) & 0xffffu) | (sel_high(c) << 16));
  case EM_SETSEL: {
    if ((intptr_t)wp == -1) {
      set_selection(h, c->caret_pos, c->caret_pos);
      return 1;
    }
    UINT a = (UINT)wp, b = lp < 0 ? len : (UINT)lp;
    if (lp < 0 && (intptr_t)wp == 0)
      a = 0;
    set_selection(h, a, b);
    return 1;
  }
  case EM_REPLACESEL:
    return lp ? edit_replace(h, (const char *)lp, w32_strlen((const char *)lp),
                             !!wp, 1)
              : 0;
  case EM_LIMITTEXT:
    c->limit = wp && wp < TEXT_MAX ? (UINT)wp : TEXT_MAX;
    return 0;
  case EM_GETLIMITTEXT:
    return c->limit;
  case EM_GETMODIFY:
    return c->modified;
  case EM_SETMODIFY:
    c->modified = !!wp;
    return 0;
  case EM_SETREADONLY:
    c->readonly = !!wp;
    w->style = wp ? w->style | ES_READONLY : w->style & ~ES_READONLY;
    changed(h);
    return 1;
  case EM_SETPASSWORDCHAR:
    if (multiline(w))
      return 0;
    c->password = (char)wp;
    w->style = wp ? w->style | ES_PASSWORD : w->style & ~ES_PASSWORD;
    edit_refresh(h);
    return 0;
  case EM_GETPASSWORDCHAR:
    return (uint8_t)c->password;
  case EM_CANUNDO:
    return c->undo != 0;
  case EM_EMPTYUNDOBUFFER:
    u32_free(c->undo);
    c->undo = 0;
    return 0;
  case EM_UNDO:
  case 0x304: /* WM_UNDO */ {
    if (!c->undo)
      return 0;
    char *p = c->text;
    c->text = c->undo;
    c->undo = p;
    UINT ul = text_length(c);
    c->anchor_pos = c->undo_start <= ul ? c->undo_start : ul;
    c->caret_pos = c->undo_start <= ul ? c->undo_start : ul;
    c->modified = 1;
    edit_refresh(h);
    notify(h, EN_UPDATE);
    notify(h, EN_CHANGE);
    return 1;
  }
  case EM_GETLINECOUNT:
    return multiline(w) ? c->line_count : 1;
  case EM_GETFIRSTVISIBLELINE:
    return multiline(w) ? c->first_line : 0;
  case EM_LINEINDEX: {
    UINT line = (intptr_t)wp == -1 ? line_of(c, c->caret_pos) : (UINT)wp;
    return line < c->line_count ? (LRESULT)c->lines[line] : -1;
  }
  case EM_LINEFROMCHAR:
    return line_of(c, (intptr_t)wp == -1 ? c->caret_pos
                                         : ((UINT)wp > len ? len : (UINT)wp));
  case EM_LINELENGTH: {
    if ((intptr_t)wp == -1) {
      UINT a = sel_low(c), b = sel_high(c), la = line_of(c, a),
           lb = line_of(c, b);
      return (LRESULT)(a - c->lines[la] + line_end(c, lb) - b);
    }
    UINT line = line_of(c, (UINT)wp > len ? len : (UINT)wp);
    return (LRESULT)(line_end(c, line) - c->lines[line]);
  }
  case EM_GETLINE: {
    if (!lp || (UINT)wp >= c->line_count)
      return 0;
    UINT start = c->lines[wp], end = line_end(c, (UINT)wp),
         max = w32_u16((void *)lp);
    UINT n = end - start < max ? end - start : max;
    w32_copy((void *)lp, c->text + start, n);
    if (n < max)
      ((char *)lp)[n] = 0;
    return n;
  }
  case EM_LINESCROLL: {
    if (!multiline(w))
      return 0;
    int line = c->first_line + (int)lp;
    if (line < 0)
      line = 0;
    if (line >= (int)c->line_count)
      line = (int)c->line_count - 1;
    c->first_line = line;
    c->xoffset += (int)wp * font_of(c)->average;
    if (c->xoffset < 0)
      c->xoffset = 0;
    edit_scrollbars(h, c);
    changed(h);
    place_caret(h);
    return 1;
  }
  case EM_SCROLL: {
    if (!multiline(w))
      return 0;
    int rows = visible_lines(h, c), old = c->first_line;
    int delta = wp == SB_LINEUP     ? -1
                : wp == SB_LINEDOWN ? 1
                : wp == SB_PAGEUP   ? -rows
                : wp == SB_PAGEDOWN ? rows
                                    : 0;
    SendMessageA(h, EM_LINESCROLL, 0, delta);
    c = control(h);
    return c ? (LRESULT)(0x10000u | (uint16_t)(c->first_line - old)) : 0;
  }
  case EM_SCROLLCARET:
    scroll_caret(h, c);
    edit_scrollbars(h, c);
    changed(h);
    place_caret(h);
    return 1;
  case EM_GETRECT:
    if (lp)
      *(RECT *)lp = edit_format(h, c);
    return 0;
  case EM_SETRECT:
  case EM_SETRECTNP:
    if (lp) {
      c->format = *(RECT *)lp;
      c->rect_set = 1;
    } else
      c->rect_set = 0;
    layout(h, c);
    if (m == EM_SETRECT)
      changed(h);
    return 0;
  case EM_SETMARGINS:
    if (wp & 1)
      c->margin_left = (int16_t)lp;
    if (wp & 2)
      c->margin_right = (int16_t)(lp >> 16);
    edit_refresh(h);
    return 0;
  case EM_GETMARGINS:
    return (LRESULT)((uint16_t)c->margin_left |
                     ((uint32_t)(uint16_t)c->margin_right << 16));
  case EM_POSFROMCHAR: {
    if ((UINT)wp > len)
      return -1;
    POINT p = pos_point(h, c, (UINT)wp);
    return (LRESULT)((uint16_t)p.x | ((uint32_t)(uint16_t)p.y << 16));
  }
  case EM_CHARFROMPOS: {
    POINT p = {(int16_t)lp, (int16_t)(lp >> 16)};
    UINT pos = point_pos(h, c, p);
    return (LRESULT)((pos & 0xffffu) | (line_of(c, pos) << 16));
  }
  case EM_SETTABSTOPS:
    if (wp > 32 || (!lp && wp))
      return 0;
    c->tab_count = (UINT)wp;
    for (UINT i = 0; i < wp; i++)
      c->tabs[i] = ((const int *)lp)[i];
    return 1;
  case 0x300: /* WM_CUT */
    if (!c->readonly && sel_low(c) != sel_high(c)) {
      clipboard_copy(h, c);
      edit_replace(h, "", 0, 1, 1);
    }
    return 0;
  case 0x301: /* WM_COPY */
    clipboard_copy(h, c);
    return 0;
  case 0x302: /* WM_PASTE */
    if (!c->readonly)
      clipboard_paste(h);
    return 0;
  case 0x303: /* WM_CLEAR */
    if (!c->readonly)
      edit_replace(h, "", 0, 1, 1);
    return 0;
  case WM_SETFOCUS: {
    const struct u32_font_info *f = font_of(c);
    CreateCaret(h, 0, 1, f->height);
    place_caret(h);
    ShowCaret(h);
    changed(h);
    notify(h, EN_SETFOCUS);
    return 0;
  }
  case WM_KILLFOCUS:
    DestroyCaret();
    changed(h);
    notify(h, EN_KILLFOCUS);
    return 0;
  case WM_LBUTTONDOWN: {
    if (GetFocus() != h)
      SetFocus(h);
    if (!(c = control(h)))
      return 0;
    UINT pos = point_pos(h, c, (POINT){(int16_t)lp, (int16_t)(lp >> 16)});
    if (wp & 4)
      set_selection(h, c->anchor_pos, pos);
    else
      set_selection(h, pos, pos);
    if ((c = control(h)))
      c->selecting = 1;
    SetCapture(h);
    return 0;
  }
  case WM_LBUTTONDBLCLK: {
    UINT pos = point_pos(h, c, (POINT){(int16_t)lp, (int16_t)(lp >> 16)});
    UINT a = pos, b = pos;
    while (a && c->text[a - 1] != ' ' && c->text[a - 1] != '\n')
      a--;
    while (b < len && c->text[b] != ' ' && c->text[b] != '\r')
      b++;
    set_selection(h, a, b);
    return 0;
  }
  case WM_MOUSEMOVE:
    if (c->selecting) {
      UINT pos = point_pos(h, c, (POINT){(int16_t)lp, (int16_t)(lp >> 16)});
      if (pos != c->caret_pos)
        set_selection(h, c->anchor_pos, pos);
    }
    return 0;
  case WM_LBUTTONUP:
  case WM_CAPTURECHANGED:
    if (c->selecting) {
      c->selecting = 0;
      if (GetCapture() == h)
        ReleaseCapture();
    }
    return 0;
  case WM_KEYDOWN: {
    BOOL shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    struct u32_window *parent = u32_window(w->parent);
    if (parent && parent->control &&
        ((struct control *)parent->control)->kind == COMBOBOX &&
        (wp == VK_UP || wp == VK_DOWN || wp == 0x73 || wp == VK_RETURN ||
         wp == VK_ESCAPE))
      return SendMessageA(w->parent, WM_KEYDOWN, wp, lp);
    if (wp == VK_TAB && multiline(w) && u32_dialog_of(h)) {
      SendMessageA(u32_dialog_of(h), WM_NEXTDLGCTL, shift, 0);
      return 0;
    }
    UINT pos = c->caret_pos, line = line_of(c, pos);
    const struct u32_font_info *f = font_of(c);
    switch (wp) {
    case VK_LEFT:
      pos = ctrl ? word_left(c, pos) : pos ? pos - 1 : 0;
      if (pos && c->text[pos] == '\n' && c->text[pos - 1] == '\r')
        pos--;
      break;
    case VK_RIGHT:
      pos = ctrl ? word_right(c, pos) : pos < len ? pos + 1 : len;
      if (pos < len && c->text[pos - 1] == '\r' && c->text[pos] == '\n')
        pos++;
      break;
    case VK_HOME:
      pos = ctrl ? 0 : c->lines[line];
      break;
    case VK_END:
      pos = ctrl ? len : line_end(c, line);
      break;
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT: {
      if (!multiline(w))
        return 0;
      int rows = wp == VK_PRIOR || wp == VK_NEXT ? visible_lines(h, c) : 1;
      int target = (int)line + (wp == VK_UP || wp == VK_PRIOR ? -rows : rows);
      if (target < 0)
        target = 0;
      if (target >= (int)c->line_count)
        target = (int)c->line_count - 1;
      POINT p = pos_point(h, c, pos);
      p.y += (target - (int)line) * f->height;
      pos = point_pos(h, c, p);
      break;
    }
    case VK_DELETE:
      if (c->readonly)
        return 0;
      if (shift) {
        SendMessageA(h, 0x300, 0, 0);
        return 0;
      }
      if (c->anchor_pos == c->caret_pos && c->caret_pos < len) {
        c->caret_pos++;
        if (c->text[c->caret_pos - 1] == '\r' && c->text[c->caret_pos] == '\n')
          c->caret_pos++;
      }
      edit_replace(h, "", 0, 1, 1);
      return 0;
    case VK_INSERT:
      if (ctrl)
        SendMessageA(h, 0x301, 0, 0);
      else if (shift)
        SendMessageA(h, 0x302, 0, 0);
      return 0;
    default:
      return 0;
    }
    set_selection(h, shift ? c->anchor_pos : pos, pos);
    return 0;
  }
  case WM_CHAR: {
    BOOL ctrl = GetKeyState(VK_CONTROL) < 0;
    if (ctrl && (wp == 3 || wp == 24 || wp == 22 || wp == 26)) {
      SendMessageA(h, wp == 3 ? 0x301 : wp == 24 ? 0x300 : wp == 22 ? 0x302
                                                                    : EM_UNDO,
                   0, 0);
      return 0;
    }
    if (c->readonly) {
      MessageBeep(0);
      return 0;
    }
    char text[2] = {(char)wp, 0};
    UINT n = 1;
    if (wp == VK_BACK) {
      if (c->anchor_pos == c->caret_pos && c->caret_pos) {
        c->caret_pos--;
        if (c->caret_pos && c->text[c->caret_pos] == '\n' &&
            c->text[c->caret_pos - 1] == '\r')
          c->caret_pos--;
      }
      edit_replace(h, "", 0, 1, 1);
      return 0;
    }
    if (wp == '\r' || wp == '\n') {
      if (!multiline(w))
        return 0;
      text[0] = '\r';
      text[1] = '\n';
      n = 2;
    } else if (wp == '\t') {
      if (!multiline(w))
        return 0;
    } else if (wp < 32)
      return 0;
    if ((w->style & ES_NUMBER) && n == 1 && (wp < '0' || wp > '9')) {
      MessageBeep(0);
      return 0;
    }
    if ((w->style & ES_UPPERCASE) && wp >= 'a' && wp <= 'z')
      text[0] = (char)(wp - 32);
    if ((w->style & ES_LOWERCASE) && wp >= 'A' && wp <= 'Z')
      text[0] = (char)(wp + 32);
    edit_replace(h, text, n, 1, 1);
    return 0;
  }
  case WM_VSCROLL:
    if (LOWORD_SB(wp) == SB_THUMBTRACK || LOWORD_SB(wp) == SB_THUMBPOSITION)
      SendMessageA(h, EM_LINESCROLL, 0, (int)(wp >> 16) - c->first_line);
    else if (LOWORD_SB(wp) <= SB_PAGEDOWN)
      SendMessageA(h, EM_SCROLL, wp & 0xffff, 0);
    return 0;
  case WM_SIZE:
    edit_refresh(h);
    return 0;
  case WM_ENABLE:
    changed(h);
    return 0;
  default:
    *handled = 0;
    return 0;
  }
}

/* ---- LISTBOX ---------------------------------------------------------- */
static BOOL multi_select(struct u32_window *w) {
  return !!(w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL));
}
static BOOL owner_drawn(struct u32_window *w) {
  return !!(w->style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE));
}
static BOOL has_strings(struct u32_window *w) {
  return !owner_drawn(w) || (w->style & LBS_HASSTRINGS);
}
static void clear_items(struct control *c) {
  for (UINT i = 0; i < c->count; i++)
    u32_free(c->items[i].text);
  u32_free(c->items);
  c->items = 0;
  c->count = c->capacity = 0;
  c->selection = c->anchor = c->caret = -1;
  c->top = 0;
}
static int list_rows(HWND h, struct control *c) {
  RECT r;
  GetClientRect(h, &r);
  int n = c->item_height ? r.bottom / (int)c->item_height : 1;
  return n > 0 ? n : 1;
}
static void list_scrollbar(HWND h, struct control *c) {
  struct u32_window *w = u32_window(h);
  if (!w || !c->vertical_bar || (w->style & LBS_MULTICOLUMN))
    return;
  SCROLLINFO s = {sizeof(s),
                  SIF_ALL | (w->style & LBS_DISABLENOSCROLL ? SIF_DISABLENOSCROLL
                                                            : 0),
                  0,
                  c->count ? (int)c->count - 1 : 0,
                  (UINT)list_rows(h, c),
                  c->top,
                  0};
  SetScrollInfo(h, SB_VERT, &s, 1);
}
static void list_show(HWND h, struct control *c, int index) {
  if (index < 0)
    return;
  int rows = list_rows(h, c);
  if (index < c->top)
    c->top = index;
  else if (index >= c->top + rows)
    c->top = index - rows + 1;
  if (c->top < 0)
    c->top = 0;
  list_scrollbar(h, c);
}
static void measure_item(HWND h, struct control *c, UINT at) {
  struct u32_window *w = u32_window(h);
  if (!(w->style & LBS_OWNERDRAWVARIABLE))
    return;
  MEASUREITEMSTRUCT m = {c->kind == COMBOLIST ? 3u : 2u, w->id, at, 0,
                         c->item_height, c->items[at].data};
  SendMessageA(w->parent ? w->parent : w->owner, WM_MEASUREITEM, w->id,
               (LPARAM)&m);
  if ((c = control(h)) && at < c->count && m.itemHeight && m.itemHeight < 256)
    c->items[at].height = m.itemHeight;
}
static int list_compare(HWND h, struct control *c, const struct item *a,
                        LPARAM value, BOOL strings) {
  if (strings)
    return u32_stricmp(a->text, (const char *)value);
  struct u32_window *w = u32_window(h);
  struct {
    UINT type, id;
    HWND item;
    UINT id1;
    uintptr_t data1;
    UINT id2;
    uintptr_t data2;
    DWORD locale;
  } compare = {c->kind == COMBOLIST ? 3u : 2u, w->id, h, 0, a->data, (UINT)-1,
               (uintptr_t)value, 0};
  return (int)SendMessageA(w->parent, WM_COMPAREITEM, w->id, (LPARAM)&compare);
}
static int list_add(HWND h, struct control *c, int at, LPARAM value,
                    BOOL sort) {
  struct u32_window *w = u32_window(h);
  BOOL strings = has_strings(w);
  if (c->count >= ITEMS_MAX)
    return LB_ERRSPACE;
  if (strings && !value)
    value = (LPARAM) "";
  char *text = strings ? dup_text((const char *)value, TEXT_MAX) : 0;
  if (strings && !text)
    return LB_ERRSPACE;
  if (at < 0 || at > (int)c->count)
    at = (int)c->count;
  if (sort) {
    at = 0;
    while (at < (int)c->count &&
           list_compare(h, c, &c->items[at], value, strings) <= 0)
      at++;
    c = control(h);
    if (!c) {
      u32_free(text);
      return LB_ERR;
    }
  }
  if (c->count == c->capacity) {
    UINT cap = c->capacity ? c->capacity * 2 : 16;
    struct item *p = u32_alloc(cap * sizeof(*p));
    if (!p) {
      u32_free(text);
      return LB_ERRSPACE;
    }
    if (c->items)
      w32_copy(p, c->items, c->count * sizeof(*p));
    u32_free(c->items);
    c->items = p;
    c->capacity = cap;
  }
  for (UINT i = c->count; i > (UINT)at; i--)
    c->items[i] = c->items[i - 1];
  c->items[at] = (struct item){text, strings ? 0 : (uintptr_t)value,
                               c->item_height, 0};
  if (owner_drawn(w) && strings)
    c->items[at].data = 0;
  c->count++;
  if (c->selection >= at)
    c->selection++;
  if (c->caret >= at)
    c->caret++;
  measure_item(h, c, (UINT)at);
  return at;
}
/* f5-20B: LB_DIR, LB_ADDFILE and CB_DIR (DlgDirList) over the KERNEL32
 * file search. Directories are listed as "[name]", drives as "[-x-]".
 * Without DDL_EXCLUSIVE, files with no read-only, hidden or system
 * attribute are listed plus those whose attributes were requested; with it,
 * only files having a requested attribute. Returns the index of the last
 * item added. */
#define DDL_DIRECTORY 0x10u
#define DDL_DRIVES 0x4000u
#define DDL_EXCLUSIVE 0x8000u
static int dir_fill(HWND h, UINT add, UINT attrib, const char *spec) {
  HANDLE(W32API * first)(const char *, void *) =
      (HANDLE(W32API *)(const char *, void *))u32_kernel("FindFirstFileA");
  BOOL(W32API * next)(HANDLE, void *) =
      (BOOL(W32API *)(HANDLE, void *))u32_kernel("FindNextFileA");
  BOOL(W32API * close)(HANDLE) = (BOOL(W32API *)(HANDLE))u32_kernel("FindClose");
  DWORD(W32API * drives)(void) = (DWORD(W32API *)(void))u32_kernel("GetLogicalDrives");
  int last = LB_ERR;
  if (!spec || !*spec)
    spec = "*.*";
  if (first && next && close) {
    /* WIN32_FIND_DATAA: attributes at 0, file name at 44 (260 bytes). */
    uint8_t data[320];
    HANDLE f = first(spec, data);
    if (f && f != (HANDLE)(intptr_t)-1) {
      do {
        DWORD fa = w32_u32(data);
        char *name = (char *)data + 44, text[264];
        name[259] = 0;
        if (fa & 0x10u) {
          if (!(attrib & DDL_DIRECTORY) || !u32_stricmp(name, "."))
            continue;
          text[0] = '[';
          u32_strcpy(text + 1, 261, name);
          UINT n = w32_strlen(text);
          text[n] = ']';
          text[n + 1] = 0;
        } else {
          UINT special = fa & 0x27u; /* read-only, hidden, system, archive */
          BOOL take = attrib & DDL_EXCLUSIVE
                          ? (special & attrib) != 0 ||
                                (!(attrib & 0x27u) && !(fa & 0x7u))
                          : !((fa & 0x7u) & ~attrib);
          if (!take)
            continue;
          u32_strcpy(text, sizeof(text), name);
        }
        int at = (int)SendMessageA(h, add, 0, (LPARAM)text);
        if (at < 0) {
          close(f);
          return at;
        }
        last = at;
      } while (next(f, data));
      close(f);
    }
  }
  if ((attrib & DDL_DRIVES) && drives) {
    DWORD mask = drives();
    for (UINT d = 0; d < 26; d++)
      if (mask & (1u << d)) {
        char text[6] = {'[', '-', (char)('a' + d), '-', ']', 0};
        int at = (int)SendMessageA(h, add, 0, (LPARAM)text);
        if (at < 0)
          return at;
        last = at;
      }
  }
  return last;
}
static int list_find(struct control *c, int after, const char *s,
                     BOOL exact) {
  if (!s || !c->count)
    return LB_ERR;
  UINT n = w32_strlen(s);
  for (UINT k = 0; k < c->count; k++) {
    UINT i = (UINT)(after + 1 + (int)k) % c->count;
    const char *t = c->items[i].text ? c->items[i].text : "";
    UINT j = 0;
    while (j < n && t[j]) {
      /* f5-20B: case-insensitive over code page 1252 (lstrcmpi), not only
       * ASCII. */
      UINT a = u32_lower(u32_scalar((uint8_t)t[j])),
           b = u32_lower(u32_scalar((uint8_t)s[j]));
      if (a != b)
        break;
      j++;
    }
    if (j == n && (!exact || !t[j]))
      return (int)i;
  }
  return LB_ERR;
}
static int item_top(struct control *c, int index) {
  int y = 0;
  for (int i = c->top; i < index && i < (int)c->count; i++)
    y += (int)c->items[i].height;
  for (int i = index; i < c->top; i++)
    y -= (int)c->items[i].height;
  return y;
}
static int item_at(HWND h, struct control *c, POINT p) {
  RECT r;
  GetClientRect(h, &r);
  int y = c->kind == COMBOLIST ? 1 : 0;
  if (p.y < y)
    return c->top > 0 ? c->top - 1 : 0;
  for (int i = c->top; i < (int)c->count; i++) {
    y += (int)c->items[i].height;
    if (p.y < y)
      return i;
  }
  return c->count ? (int)c->count - 1 : -1;
}
static void list_select(HWND h, struct control *c, int index, UINT keys,
                        BOOL from_mouse) {
  struct u32_window *w = u32_window(h);
  if (index < 0 || index >= (int)c->count || (w->style & LBS_NOSEL))
    return;
  BOOL shift = !!(keys & 4), ctrl = !!(keys & 8);
  if (w->style & LBS_EXTENDEDSEL) {
    if (shift && c->anchor >= 0) {
      int a = c->anchor < index ? c->anchor : index,
          b = c->anchor > index ? c->anchor : index;
      if (!ctrl)
        for (UINT i = 0; i < c->count; i++)
          c->items[i].selected = 0;
      for (int i = a; i <= b; i++)
        c->items[i].selected = 1;
    } else if (ctrl && from_mouse) {
      c->items[index].selected = !c->items[index].selected;
      c->anchor = index;
    } else if (!ctrl) {
      for (UINT i = 0; i < c->count; i++)
        c->items[i].selected = 0;
      c->items[index].selected = 1;
      c->anchor = index;
    }
  } else if (w->style & LBS_MULTIPLESEL) {
    if (from_mouse)
      c->items[index].selected = !c->items[index].selected;
  } else
    c->selection = index;
  c->caret = index;
  list_show(h, c, index);
  changed(h);
}
static void list_paint(HWND h, HDC dc) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  RECT client;
  GetClientRect(h, &client);
  HBRUSH brush = ctl_color(h, dc, WM_CTLCOLORLISTBOX);
  if (!(c = control(h)) || !(w = u32_window(h)))
    return;
  fill(dc, client, brush);
  if (c->kind == COMBOLIST) {
    FrameRect(dc, &client, GetSysColorBrush(COLOR_WINDOWFRAME));
    InflateRect(&client, -1, -1);
  }
  DWORD ink = enabled(h) ? dc_text(dc) : GetSysColor(COLOR_GRAYTEXT);
  BOOL focus = GetFocus() == h || (c->kind == COMBOLIST);
  int y = client.top;
  for (int i = c->top; i < (int)c->count && y < client.bottom; i++) {
    struct item *it = &c->items[i];
    RECT row = {client.left, y, client.right, y + (int)it->height};
    BOOL selected = multi_select(w) ? it->selected : i == c->selection;
    if (owner_drawn(w)) {
      DRAWITEMSTRUCT d = {c->kind == COMBOLIST ? 3u : 2u,
                          w->id,
                          (UINT)i,
                          1,
                          (selected ? 1u : 0u) |
                              (focus && i == c->caret ? 16u : 0u),
                          h,
                          dc,
                          row,
                          it->data};
      SendMessageA(w->parent ? w->parent : w->owner, WM_DRAWITEM, w->id,
                   (LPARAM)&d);
      if (!(c = control(h)) || !(w = u32_window(h)))
        return;
    } else {
      if (selected)
        u32_color_rect(dc, row, GetSysColor(COLOR_HIGHLIGHT));
      RECT text = row;
      text.left += 2 - c->horizontal;
      UINT flags = 0x820u | (w->style & LBS_USETABSTOPS ? 0x40u : 0u);
      u32_label(dc, text, it->text ? it->text : "", c->font,
                selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : ink, flags);
      if (focus && i == c->caret && c->kind != COMBOLIST)
        DrawFocusRect(dc, &row);
    }
    y += (int)it->height;
  }
}
static LRESULT list_message(HWND h, UINT m, WPARAM wp, LPARAM lp,
                            BOOL *handled) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  BOOL valid = wp < c->count;
  *handled = 1;
  switch (m) {
  case LB_ADDSTRING:
  case LB_INSERTSTRING: {
    int at = list_add(h, c, m == LB_ADDSTRING ? -1 : (int)wp, lp,
                      m == LB_ADDSTRING && (w->style & LBS_SORT));
    if ((c = control(h))) {
      list_scrollbar(h, c);
      changed(h);
    }
    return at;
  }
  case LB_DELETESTRING: {
    if (!valid)
      return LB_ERR;
    struct item it = c->items[wp];
    if (owner_drawn(w)) {
      struct {
        UINT type, id, item;
        HWND hwnd;
        uintptr_t data;
      } del = {2, w->id, (UINT)wp, h, it.data};
      SendMessageA(w->parent, WM_DELETEITEM, w->id, (LPARAM)&del);
      if (!(c = control(h)) || wp >= c->count)
        return LB_ERR;
    }
    u32_free(c->items[wp].text);
    for (UINT i = (UINT)wp; i + 1 < c->count; i++)
      c->items[i] = c->items[i + 1];
    c->count--;
    if (c->selection == (int)wp)
      c->selection = -1;
    else if (c->selection > (int)wp)
      c->selection--;
    if (c->caret >= (int)c->count)
      c->caret = (int)c->count - 1;
    if (c->top >= (int)c->count && c->top)
      c->top = (int)c->count ? (int)c->count - 1 : 0;
    list_scrollbar(h, c);
    changed(h);
    return c->count;
  }
  case LB_RESETCONTENT:
    clear_items(c);
    list_scrollbar(h, c);
    changed(h);
    return 0;
  case LB_GETCOUNT:
    return c->count;
  case LB_GETCURSEL:
    return multi_select(w) ? c->caret : c->selection;
  case LB_SETCURSEL:
    if (multi_select(w))
      return LB_ERR;
    if ((intptr_t)wp == -1 || !valid) {
      c->selection = -1;
      changed(h);
      return LB_ERR;
    }
    c->selection = c->caret = (int)wp;
    list_show(h, c, (int)wp);
    changed(h);
    return (LRESULT)wp;
  case LB_GETTEXTLEN:
    return valid ? (has_strings(w) ? (LRESULT)w32_strlen(c->items[wp].text)
                                   : (LRESULT)sizeof(DWORD))
                 : LB_ERR;
  case LB_GETTEXT:
    if (!valid || !lp)
      return LB_ERR;
    if (has_strings(w)) {
      UINT n = w32_strlen(c->items[wp].text);
      w32_copy((void *)lp, c->items[wp].text, n + 1);
      return n;
    }
    w32_put32((void *)lp, (DWORD)c->items[wp].data);
    return sizeof(DWORD);
  case LB_GETITEMDATA:
    return valid ? (LRESULT)c->items[wp].data : LB_ERR;
  case LB_SETITEMDATA:
    if (!valid)
      return LB_ERR;
    c->items[wp].data = (uintptr_t)lp;
    return 1;
  case LB_GETSEL:
    return valid ? (multi_select(w) ? c->items[wp].selected
                                    : (int)wp == c->selection)
                 : LB_ERR;
  case LB_SETSEL:
    if (!multi_select(w))
      return LB_ERR;
    if (lp == -1)
      for (UINT i = 0; i < c->count; i++)
        c->items[i].selected = !!wp;
    else if (lp >= 0 && (UINT)lp < c->count)
      c->items[lp].selected = !!wp;
    else
      return LB_ERR;
    changed(h);
    return 0;
  case LB_GETSELCOUNT: {
    if (!multi_select(w))
      return LB_ERR;
    UINT n = 0;
    for (UINT i = 0; i < c->count; i++)
      n += !!c->items[i].selected;
    return n;
  }
  case LB_GETSELITEMS: {
    if (!multi_select(w) || (!lp && wp))
      return LB_ERR;
    UINT n = 0;
    for (UINT i = 0; i < c->count && n < wp; i++)
      if (c->items[i].selected)
        ((int *)lp)[n++] = (int)i;
    return n;
  }
  case LB_SELITEMRANGE:
  case LB_SELITEMRANGEEX: {
    if (!multi_select(w))
      return LB_ERR;
    int a = m == LB_SELITEMRANGE ? (int)(lp & 0xffff) : (int)wp,
        b = m == LB_SELITEMRANGE ? (int)((lp >> 16) & 0xffff) : (int)lp;
    BOOL on = m == LB_SELITEMRANGE ? !!wp : a <= b;
    if (a > b) {
      int t = a;
      a = b;
      b = t;
    }
    if (a < 0 || a >= (int)c->count)
      return LB_ERR;
    if (b >= (int)c->count)
      b = (int)c->count - 1;
    for (int i = a; i <= b; i++)
      c->items[i].selected = on;
    changed(h);
    return 0;
  }
  case LB_SETANCHORINDEX:
    if (!valid)
      return LB_ERR;
    c->anchor = (int)wp;
    return 0;
  case LB_GETANCHORINDEX:
    return c->anchor;
  case LB_SETCARETINDEX:
    if (!valid)
      return LB_ERR;
    c->caret = (int)wp;
    list_show(h, c, (int)wp);
    changed(h);
    return 0;
  case LB_GETCARETINDEX:
    return c->caret >= 0 ? c->caret : 0;
  case LB_SETTOPINDEX:
    if (!valid && wp)
      return LB_ERR;
    c->top = (int)wp;
    list_scrollbar(h, c);
    changed(h);
    return 0;
  case LB_GETTOPINDEX:
    return c->top;
  case LB_SETHORIZONTALEXTENT:
    c->extent = (int)wp;
    if (w->style & WS_HSCROLL) {
      RECT r;
      GetClientRect(h, &r);
      SCROLLINFO s = {sizeof(s), SIF_RANGE | SIF_PAGE, 0, c->extent ? c->extent - 1 : 0,
                      (UINT)r.right, 0, 0};
      SetScrollInfo(h, SB_HORZ, &s, 1);
    }
    return 0;
  case LB_GETHORIZONTALEXTENT:
    return c->extent;
  case LB_SETCOLUMNWIDTH:
    c->column_width = (UINT)wp;
    changed(h);
    return 0;
  case LB_SETTABSTOPS:
    if (wp > 32 || (!lp && wp))
      return 0;
    c->tab_count = (UINT)wp;
    for (UINT i = 0; i < wp; i++)
      c->tabs[i] = ((const int *)lp)[i];
    return 1;
  case LB_SETITEMHEIGHT:
    if ((lp & 0xffff) == 0 || (lp & 0xffff) > 255)
      return LB_ERR;
    if (w->style & LBS_OWNERDRAWVARIABLE) {
      if (!valid)
        return LB_ERR;
      c->items[wp].height = (UINT)(lp & 0xffff);
    } else {
      c->item_height = (UINT)(lp & 0xffff);
      for (UINT i = 0; i < c->count; i++)
        c->items[i].height = c->item_height;
    }
    list_scrollbar(h, c);
    changed(h);
    return 0;
  case LB_GETITEMHEIGHT:
    return w->style & LBS_OWNERDRAWVARIABLE
               ? (valid ? (LRESULT)c->items[wp].height : LB_ERR)
               : (LRESULT)c->item_height;
  case LB_GETITEMRECT: {
    if (!lp || (!valid && !(wp == 0 && c->count == 0)))
      return LB_ERR;
    RECT r;
    GetClientRect(h, &r);
    int y = item_top(c, (int)wp);
    *(RECT *)lp = (RECT){0, y, r.right,
                         y + (int)(valid ? c->items[wp].height : c->item_height)};
    return 1;
  }
  case LB_ITEMFROMPOINT: {
    POINT p = {(int16_t)lp, (int16_t)(lp >> 16)};
    int i = item_at(h, c, p);
    RECT r;
    GetClientRect(h, &r);
    return (LRESULT)((uint16_t)(i < 0 ? 0 : i) | (PtInRect(&r, p) ? 0 : 0x10000));
  }
  case LB_FINDSTRING:
  case LB_FINDSTRINGEXACT:
  case LB_SELECTSTRING: {
    if (!has_strings(w)) {
      for (UINT k = 0; k < c->count; k++) {
        UINT i = (UINT)((int)wp + 1 + (int)k) % c->count;
        if (c->items[i].data == (uintptr_t)lp)
          return i;
      }
      return LB_ERR;
    }
    int i = list_find(c, (int)wp, (const char *)lp, m == LB_FINDSTRINGEXACT);
    if (i >= 0 && m == LB_SELECTSTRING && !multi_select(w)) {
      c->selection = c->caret = i;
      list_show(h, c, i);
      changed(h);
    }
    return i;
  }
  case LB_DIR:
    return dir_fill(h, LB_ADDSTRING, (UINT)wp, (const char *)lp);
  case LB_ADDFILE: /* add one file name as is */
    return lp ? (int)SendMessageA(h, LB_ADDSTRING, 0, lp) : LB_ERR;
  case WM_GETDLGCODE:
    return DLGC_WANTARROWS | DLGC_WANTCHARS;
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
    changed(h);
    if (w->style & LBS_NOTIFY)
      notify(h, m == WM_SETFOCUS ? LBN_SETFOCUS : LBN_KILLFOCUS);
    return 0;
  case WM_LBUTTONDOWN:
  case WM_MOUSEMOVE: {
    if (m == WM_MOUSEMOVE && GetCapture() != h)
      return 0;
    if (m == WM_LBUTTONDOWN && c->kind != COMBOLIST) {
      if (GetFocus() != h)
        SetFocus(h);
      if (!(c = control(h)))
        return 0;
      SetCapture(h);
    }
    int i = item_at(h, c, (POINT){(int16_t)lp, (int16_t)(lp >> 16)});
    int old = multi_select(w) ? c->caret : c->selection;
    if (m == WM_LBUTTONDOWN || i != old)
      list_select(h, c, i, m == WM_MOUSEMOVE ? (UINT)wp & ~8u : (UINT)wp,
                  m == WM_LBUTTONDOWN);
    c = control(h);
    if (c && (w->style & LBS_NOTIFY) && i != old && c->kind != COMBOLIST)
      notify(h, LBN_SELCHANGE);
    return 0;
  }
  case WM_LBUTTONUP:
    if (GetCapture() == h)
      ReleaseCapture();
    if (c->kind == COMBOLIST && c->combo)
      SendMessageA(c->combo, WM_USER + 0x7f00, 1, c->selection);
    return 0;
  case WM_LBUTTONDBLCLK:
    if (w->style & LBS_NOTIFY)
      notify(h, LBN_DBLCLK);
    return 0;
  case WM_KEYDOWN: {
    int i = multi_select(w) ? c->caret : c->selection, rows = list_rows(h, c);
    if (w->style & LBS_WANTKEYBOARDINPUT) {
      LRESULT r = SendMessageA(w->parent, WM_VKEYTOITEM,
                               (wp & 0xffff) | ((WPARAM)(i < 0 ? 0 : i) << 16),
                               (LPARAM)h);
      if (r == -2 || !(c = control(h)))
        return 0;
      if (r >= 0) {
        list_select(h, c, (int)r, 0, 0);
        return 0;
      }
    }
    if (i < 0)
      i = -1;
    switch (wp) {
    case VK_UP:
      i = i > 0 ? i - 1 : 0;
      break;
    case VK_DOWN:
      i = i + 1 < (int)c->count ? i + 1 : (int)c->count - 1;
      break;
    case VK_PRIOR:
      i = i - rows > 0 ? i - rows + 1 : 0;
      break;
    case VK_NEXT:
      i = i + rows < (int)c->count ? i + rows - 1 : (int)c->count - 1;
      break;
    case VK_HOME:
      i = 0;
      break;
    case VK_END:
      i = (int)c->count - 1;
      break;
    case VK_SPACE:
      if (multi_select(w) && c->caret >= 0) {
        c->items[c->caret].selected = !c->items[c->caret].selected;
        c->anchor = c->caret;
        changed(h);
        if (w->style & LBS_NOTIFY)
          notify(h, LBN_SELCHANGE);
      }
      return 0;
    default:
      return 0;
    }
    if (i < 0 || !c->count)
      return 0;
    UINT keys = (GetKeyState(VK_SHIFT) < 0 ? 4u : 0u) |
                (GetKeyState(VK_CONTROL) < 0 ? 8u : 0u);
    if (w->style & LBS_MULTIPLESEL) {
      c->caret = i;
      list_show(h, c, i);
      changed(h);
    } else
      list_select(h, c, i, keys, 0);
    if ((c = control(h)) && (w->style & LBS_NOTIFY))
      notify(h, LBN_SELCHANGE);
    return 0;
  }
  case WM_CHAR: {
    if (wp < 32 || !has_strings(w))
      return 0;
    char s[2] = {(char)wp, 0};
    int start = multi_select(w) ? c->caret : c->selection;
    int i = list_find(c, start, s, 0);
    if (i >= 0) {
      list_select(h, c, i, 0, 0);
      if ((c = control(h)) && (w->style & LBS_NOTIFY))
        notify(h, LBN_SELCHANGE);
    }
    return 0;
  }
  case WM_VSCROLL: {
    int rows = list_rows(h, c), top = c->top;
    switch (wp & 0xffff) {
    case SB_LINEUP:
      top--;
      break;
    case SB_LINEDOWN:
      top++;
      break;
    case SB_PAGEUP:
      top -= rows;
      break;
    case SB_PAGEDOWN:
      top += rows;
      break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
      top = (int)(wp >> 16);
      break;
    case SB_TOP:
      top = 0;
      break;
    case SB_BOTTOM:
      top = (int)c->count;
      break;
    default:
      return 0;
    }
    if (top > (int)c->count - rows)
      top = (int)c->count - rows;
    if (top < 0)
      top = 0;
    c->top = top;
    list_scrollbar(h, c);
    changed(h);
    return 0;
  }
  case WM_HSCROLL: {
    RECT r;
    GetClientRect(h, &r);
    int x = c->horizontal;
    switch (wp & 0xffff) {
    case SB_LINEUP:
      x -= 8;
      break;
    case SB_LINEDOWN:
      x += 8;
      break;
    case SB_PAGEUP:
      x -= r.right;
      break;
    case SB_PAGEDOWN:
      x += r.right;
      break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION:
      x = (int)(wp >> 16);
      break;
    default:
      return 0;
    }
    if (x > c->extent - r.right)
      x = c->extent - r.right;
    if (x < 0)
      x = 0;
    c->horizontal = x;
    SetScrollPos(h, SB_HORZ, x, 1);
    changed(h);
    return 0;
  }
  case WM_SIZE:
    list_scrollbar(h, c);
    return 0;
  default:
    *handled = 0;
    return 0;
  }
}

/* ---- COMBOBOX --------------------------------------------------------- */
static UINT combo_type(struct u32_window *w) { return w->style & 3; }
static UINT combo_to_list(UINT m) {
  switch (m) {
  case CB_ADDSTRING:
    return LB_ADDSTRING;
  case CB_INSERTSTRING:
    return LB_INSERTSTRING;
  case CB_DELETESTRING:
    return LB_DELETESTRING;
  case CB_RESETCONTENT:
    return LB_RESETCONTENT;
  case CB_GETCOUNT:
    return LB_GETCOUNT;
  case CB_GETCURSEL:
    return LB_GETCURSEL;
  case CB_GETLBTEXT:
    return LB_GETTEXT;
  case CB_GETLBTEXTLEN:
    return LB_GETTEXTLEN;
  case CB_FINDSTRING:
    return LB_FINDSTRING;
  case CB_FINDSTRINGEXACT:
    return LB_FINDSTRINGEXACT;
  case CB_GETITEMDATA:
    return LB_GETITEMDATA;
  case CB_SETITEMDATA:
    return LB_SETITEMDATA;
  case CB_GETTOPINDEX:
    return LB_GETTOPINDEX;
  case CB_SETTOPINDEX:
    return LB_SETTOPINDEX;
  default:
    return 0;
  }
}
static RECT combo_field(HWND h) {
  RECT r;
  GetClientRect(h, &r);
  struct control *c = control(h);
  struct u32_window *w = u32_window(h);
  if (c && w && combo_type(w) == CBS_SIMPLE)
    r.bottom = c->field_height;
  return r;
}
static RECT combo_button(HWND h) {
  RECT r = combo_field(h);
  InflateRect(&r, -2, -2);
  r.left = r.right - GetSystemMetrics(2);
  return r;
}
static void combo_sync_edit(HWND h) {
  struct control *c = control(h);
  if (!c || !c->edit)
    return;
  const char *text = c->selection >= 0 && (UINT)c->selection < c->count &&
                             c->items[c->selection].text
                         ? c->items[c->selection].text
                         : "";
  SendMessageA(c->edit, WM_SETTEXT, 0, (LPARAM)text);
  if ((c = control(h)) && c->edit)
    SendMessageA(c->edit, EM_SETSEL, 0, -1);
}
static void combo_close(HWND h, BOOL accept);
static void combo_drop(HWND h) {
  struct control *c = control(h);
  struct u32_window *w = u32_window(h);
  if (!c || !w || c->dropped || combo_type(w) == CBS_SIMPLE)
    return;
  notify(h, CBN_DROPDOWN);
  if (!(c = control(h)) || !(w = u32_window(h)))
    return;
  RECT r;
  GetWindowRect(h, &r);
  int rows = (int)c->count ? (int)c->count : 1;
  int height = c->drop_height - (r.bottom - r.top);
  int needed = rows * (int)c->item_height + 2;
  if (height < (int)c->item_height + 2)
    height = (int)c->item_height + 2;
  if (needed < height)
    height = needed;
  int width = c->drop_width > r.right - r.left ? c->drop_width
                                                : r.right - r.left;
  int y = r.bottom;
  if (y + height > (int)GetSystemMetrics(1) && r.top - height >= 0)
    y = r.top - height;
  HWND list = CreateWindowExA(WS_EX_TOPMOST, "ComboLBox", "",
                              WS_POPUP |
                                  (w->style & (CBS_OWNERDRAWFIXED |
                                               CBS_OWNERDRAWVARIABLE |
                                               CBS_HASSTRINGS))
                                      << 0,
                              r.left, y, width, height, h, 0, w->instance, 0);
  if (!list || !(c = control(h)))
    return;
  struct control *lc = control(list);
  struct u32_window *lw = u32_window(list);
  if (!lc || !lw) {
    DestroyWindow(list);
    return;
  }
  lw->id = w->id;
  lw->owner = h;
  lc->combo = h;
  lc->font = c->font;
  lc->item_height = c->item_height;
  for (UINT i = 0; i < c->count; i++) {
    int at = list_add(list, lc, -1,
                      c->items[i].text ? (LPARAM)c->items[i].text
                                       : (LPARAM)c->items[i].data,
                      0);
    if (at >= 0)
      lc->items[at].data = c->items[i].data, lc->items[at].height = c->items[i].height;
  }
  lc->selection = lc->caret = c->selection;
  list_show(list, lc, c->selection);
  c->list = list;
  c->dropped = 1;
  ShowWindow(list, 8);
  SetCapture(h);
  changed(h);
}
static void combo_close(HWND h, BOOL accept) {
  struct control *c = control(h);
  if (!c || !c->dropped)
    return;
  HWND list = c->list;
  int chosen = list && control(list) ? control(list)->selection : -1;
  c->list = 0;
  c->dropped = 0;
  if (GetCapture() == h)
    ReleaseCapture();
  if (list)
    DestroyWindow(list);
  if (!(c = control(h)))
    return;
  notify(h, accept ? CBN_SELENDOK : CBN_SELENDCANCEL);
  if (!(c = control(h)))
    return;
  if (accept && chosen != c->selection && chosen >= 0) {
    c->selection = chosen;
    combo_sync_edit(h);
    changed(h);
    notify(h, CBN_SELCHANGE);
  }
  notify(h, CBN_CLOSEUP);
  changed(h);
}
static void combo_paint(HWND h, HDC dc) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  RECT r = combo_field(h);
  HBRUSH brush = ctl_color(h, dc, combo_type(w) == CBS_DROPDOWNLIST
                                      ? WM_CTLCOLORLISTBOX
                                      : WM_CTLCOLOREDIT);
  if (!(c = control(h)) || !(w = u32_window(h)))
    return;
  RECT outer = r;
  DrawEdge(dc, &outer, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
  fill(dc, outer, brush);
  if (combo_type(w) != CBS_SIMPLE) {
    RECT b = combo_button(h);
    DrawFrameControl(dc, &b, DFC_SCROLL,
                     DFCS_SCROLLCOMBOBOX | (c->dropped ? DFCS_PUSHED : 0) |
                         (enabled(h) ? 0 : DFCS_INACTIVE));
  }
  if (combo_type(w) == CBS_DROPDOWNLIST) {
    RECT text = outer;
    text.right = combo_button(h).left - 1;
    InflateRect(&text, -1, -1);
    BOOL focus = GetFocus() == h && !c->dropped;
    if (focus)
      u32_color_rect(dc, text, GetSysColor(COLOR_HIGHLIGHT));
    if (c->selection >= 0 && (UINT)c->selection < c->count) {
      if (w->style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE)) {
        DRAWITEMSTRUCT d = {3,     w->id, (UINT)c->selection, 1,
                            (focus ? 1u : 0u) | 0x1000u,
                            h,     dc,    text,
                            c->items[c->selection].data};
        SendMessageA(w->parent, WM_DRAWITEM, w->id, (LPARAM)&d);
      } else {
        RECT t = text;
        t.left += 2;
        u32_label(dc, t, c->items[c->selection].text, c->font,
                  focus ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                        : enabled(h) ? dc_text(dc) : GetSysColor(COLOR_GRAYTEXT),
                  0x824u);
      }
    }
    if (focus)
      DrawFocusRect(dc, &text);
  }
  if (combo_type(w) == CBS_SIMPLE) {
    /* CBS_SIMPLE: the list stays visible below the edit field. */
    RECT list;
    GetClientRect(h, &list);
    list.top = c->field_height;
    if (list.bottom > list.top) {
      DrawEdge(dc, &list, EDGE_SUNKEN, BF_RECT | BF_ADJUST);
      HBRUSH lb = ctl_color(h, dc, WM_CTLCOLORLISTBOX);
      if (!(c = control(h)))
        return;
      fill(dc, list, lb);
      int y = list.top;
      for (int i = c->top; i < (int)c->count && y < list.bottom; i++) {
        RECT row = {list.left, y, list.right, y + (int)c->items[i].height};
        BOOL selected = i == c->selection;
        if (selected)
          u32_color_rect(dc, row, GetSysColor(COLOR_HIGHLIGHT));
        RECT text = row;
        text.left += 2;
        u32_label(dc, text, c->items[i].text ? c->items[i].text : "", c->font,
                  GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT),
                  0x824u);
        y += (int)c->items[i].height;
      }
    }
  }
}
static LRESULT combo_message(HWND h, UINT m, WPARAM wp, LPARAM lp,
                             BOOL *handled) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  *handled = 1;
  UINT list = combo_to_list(m);
  if (list) {
    BOOL ignored;
    if (list == LB_ADDSTRING && (w->style & CBS_SORT)) {
      int at = list_add(h, c, -1, lp, 1);
      changed(h);
      return at;
    }
    LRESULT r = list_message(h, list, wp, lp, &ignored);
    if (list == LB_RESETCONTENT || list == LB_DELETESTRING)
      combo_sync_edit(h);
    if (c->list && (list == LB_ADDSTRING || list == LB_INSERTSTRING ||
                    list == LB_DELETESTRING || list == LB_RESETCONTENT))
      combo_close(h, 0);
    return r;
  }
  switch (m) {
  case CB_SETCURSEL:
    if ((intptr_t)wp < -1 || (int)wp >= (int)c->count) {
      c->selection = -1;
      combo_sync_edit(h);
      changed(h);
      return CB_ERR;
    }
    c->selection = (int)wp;
    combo_sync_edit(h);
    changed(h);
    return c->selection;
  case CB_SELECTSTRING: {
    int i = list_find(c, (int)wp, (const char *)lp, 0);
    if (i >= 0) {
      c->selection = i;
      combo_sync_edit(h);
      changed(h);
    }
    return i;
  }
  case CB_SHOWDROPDOWN:
    if (wp)
      combo_drop(h);
    else
      combo_close(h, 0);
    return 1;
  case CB_GETDROPPEDSTATE:
    return c->dropped;
  case CB_GETDROPPEDCONTROLRECT:
    if (!lp)
      return CB_ERR;
    GetWindowRect(h, (RECT *)lp);
    ((RECT *)lp)->bottom = ((RECT *)lp)->top + c->drop_height;
    return 1;
  case CB_SETEXTENDEDUI:
    c->extended_ui = !!wp;
    return 0;
  case CB_GETEXTENDEDUI:
    return c->extended_ui;
  case CB_SETDROPPEDWIDTH:
    c->drop_width = (int)wp;
    return c->drop_width;
  case CB_GETDROPPEDWIDTH: {
    RECT r;
    GetWindowRect(h, &r);
    return c->drop_width > r.right - r.left ? c->drop_width : r.right - r.left;
  }
  case CB_SETITEMHEIGHT:
    if ((intptr_t)wp == -1) {
      c->field_height = (int)lp + 6;
      return 0;
    }
    c->item_height = (UINT)lp;
    for (UINT i = 0; i < c->count; i++)
      c->items[i].height = c->item_height;
    return 0;
  case CB_GETITEMHEIGHT:
    return (intptr_t)wp == -1 ? c->field_height - 6 : (LRESULT)c->item_height;
  case CB_LIMITTEXT:
    return c->edit ? SendMessageA(c->edit, EM_LIMITTEXT, wp, 0) : 1;
  case CB_GETEDITSEL:
    return c->edit ? SendMessageA(c->edit, EM_GETSEL, wp, lp) : CB_ERR;
  case CB_SETEDITSEL:
    return c->edit ? (SendMessageA(c->edit, EM_SETSEL, (int16_t)lp,
                                   (int16_t)(lp >> 16)),
                      1)
                   : CB_ERR;
  case CB_GETHORIZONTALEXTENT:
    return c->extent;
  case CB_SETHORIZONTALEXTENT:
    c->extent = (int)wp;
    return 0;
  case CB_DIR:
    return dir_fill(h, CB_ADDSTRING, (UINT)wp, (const char *)lp);
  case WM_SETTEXT:
    if (c->edit)
      return SendMessageA(c->edit, WM_SETTEXT, wp, lp);
    {
      int i = list_find(c, -1, (const char *)lp, 1);
      if (i < 0)
        return CB_ERR;
      c->selection = i;
      changed(h);
      return 1;
    }
  case WM_GETTEXT:
    if (c->edit)
      return SendMessageA(c->edit, WM_GETTEXT, wp, lp);
    if (!wp || !lp)
      return 0;
    {
      const char *s = c->selection >= 0 && c->items[c->selection].text
                          ? c->items[c->selection].text
                          : "";
      UINT n = w32_strlen(s);
      if (n >= wp)
        n = (UINT)wp - 1;
      w32_copy((void *)lp, s, n);
      ((char *)lp)[n] = 0;
      return n;
    }
  case WM_GETTEXTLENGTH:
    if (c->edit)
      return SendMessageA(c->edit, WM_GETTEXTLENGTH, 0, 0);
    return c->selection >= 0 && c->items[c->selection].text
               ? (LRESULT)w32_strlen(c->items[c->selection].text)
               : 0;
  case WM_GETDLGCODE:
    return DLGC_WANTARROWS | DLGC_WANTCHARS |
           (c->dropped ? DLGC_WANTALLKEYS : 0);
  case WM_SETFOCUS:
    if (c->edit)
      SetFocus(c->edit);
    changed(h);
    notify(h, CBN_SETFOCUS);
    return 0;
  case WM_KILLFOCUS:
    if (c->dropped && (HWND)wp != c->edit && (HWND)wp != c->list)
      combo_close(h, 0);
    changed(h);
    notify(h, CBN_KILLFOCUS);
    return 0;
  case WM_LBUTTONDOWN: {
    POINT p = {(int16_t)lp, (int16_t)(lp >> 16)};
    if (c->dropped) {
      RECT r;
      GetWindowRect(c->list, &r);
      POINT s = p;
      ClientToScreen(h, &s);
      if (PtInRect(&r, s)) {
        ScreenToClient(c->list, &s);
        SendMessageA(c->list, WM_LBUTTONDOWN, 1, (LPARAM)((uint16_t)s.x | ((uint32_t)(uint16_t)s.y << 16)));
        if ((c = control(h)) && c->list)
          SendMessageA(c->list, WM_LBUTTONUP, 0, (LPARAM)((uint16_t)s.x | ((uint32_t)(uint16_t)s.y << 16)));
        return 0;
      }
      combo_close(h, 0);
      return 0;
    }
    if (GetFocus() != h && GetFocus() != c->edit)
      SetFocus(h);
    if (!(c = control(h)))
      return 0;
    if (combo_type(w) == CBS_SIMPLE && p.y >= c->field_height + 2 &&
        c->item_height) {
      int i = c->top + (p.y - c->field_height - 2) / (int)c->item_height;
      if (i < (int)c->count && i != c->selection) {
        c->selection = i;
        combo_sync_edit(h);
        changed(h);
        notify(h, CBN_SELCHANGE);
      }
      return 0;
    }
    RECT b = combo_button(h);
    if (combo_type(w) == CBS_DROPDOWNLIST || PtInRect(&b, p))
      combo_drop(h);
    return 0;
  }
  case WM_MOUSEMOVE:
    if (c->dropped && c->list) {
      POINT s = {(int16_t)lp, (int16_t)(lp >> 16)};
      ClientToScreen(h, &s);
      RECT r;
      GetWindowRect(c->list, &r);
      if (PtInRect(&r, s)) {
        struct control *lc = control(c->list);
        ScreenToClient(c->list, &s);
        int i = lc ? item_at(c->list, lc, s) : -1;
        if (lc && i >= 0 && i != lc->selection)
          list_select(c->list, lc, i, 0, 0);
      }
    }
    return 0;
  case WM_USER + 0x7f00: /* popup list chose an item */
    combo_close(h, wp != 0);
    return 0;
  case WM_KEYDOWN: {
    BOOL alt = GetKeyState(VK_MENU) < 0;
    if (wp == 0x73 || (alt && (wp == VK_DOWN || wp == VK_UP)) ||
        (c->extended_ui && wp == VK_DOWN && !c->dropped)) {
      if (c->dropped)
        combo_close(h, 1);
      else
        combo_drop(h);
      return 0;
    }
    if (c->dropped) {
      struct control *lc = control(c->list);
      if (wp == VK_RETURN) {
        combo_close(h, 1);
        return 0;
      }
      if (wp == VK_ESCAPE) {
        combo_close(h, 0);
        return 0;
      }
      if (lc && (wp == VK_UP || wp == VK_DOWN || wp == VK_HOME ||
                 wp == VK_END || wp == VK_PRIOR || wp == VK_NEXT))
        SendMessageA(c->list, WM_KEYDOWN, wp, lp);
      return 0;
    }
    int i = c->selection;
    if (wp == VK_UP)
      i = i > 0 ? i - 1 : 0;
    else if (wp == VK_DOWN)
      i = i + 1 < (int)c->count ? i + 1 : (int)c->count - 1;
    else if (wp == VK_HOME)
      i = 0;
    else if (wp == VK_END)
      i = (int)c->count - 1;
    else
      return 0;
    if (i >= 0 && i != c->selection) {
      c->selection = i;
      combo_sync_edit(h);
      changed(h);
      notify(h, CBN_SELCHANGE);
    }
    return 0;
  }
  case WM_CHAR: {
    if (wp < 32 || c->edit)
      return 0;
    char s[2] = {(char)wp, 0};
    int i = list_find(c, c->selection, s, 0);
    if (i >= 0 && i != c->selection) {
      c->selection = i;
      changed(h);
      notify(h, CBN_SELCHANGE);
    }
    return 0;
  }
  case WM_COMMAND:
    if ((HWND)lp == c->edit && c->edit) {
      UINT code = (UINT)(wp >> 16);
      if (code == EN_CHANGE)
        notify(h, CBN_EDITCHANGE);
      else if (code == EN_UPDATE)
        notify(h, 6);
      else if (code == EN_SETFOCUS)
        notify(h, CBN_SETFOCUS);
      else if (code == EN_KILLFOCUS) {
        if (c->dropped && GetFocus() != h)
          combo_close(h, 0);
        notify(h, CBN_KILLFOCUS);
      }
    }
    return 0;
  case WM_CTLCOLOREDIT:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLORSTATIC:
    return w->parent ? SendMessageA(w->parent, m, wp, (LPARAM)h)
                     : DefWindowProcA(h, m, wp, lp);
  case WM_ENABLE:
    if (c->edit)
      EnableWindow(c->edit, (BOOL)wp);
    changed(h);
    return 0;
  default:
    *handled = 0;
    return 0;
  }
}
static void combo_create(HWND h) {
  struct u32_window *w = u32_window(h);
  struct control *c = control(h);
  const struct u32_font_info *f = font_of(c);
  RECT r;
  GetWindowRect(h, &r);
  c->drop_height = r.bottom - r.top;
  c->field_height = f->height + 8;
  if (combo_type(w) != CBS_SIMPLE) {
    struct u32_window *self = u32_window(h);
    self->rect.bottom = self->rect.top + c->field_height;
  }
  if (combo_type(w) != CBS_DROPDOWNLIST) {
    RECT field = combo_field(h);
    InflateRect(&field, -3, -3);
    if (combo_type(w) != CBS_SIMPLE)
      field.right = combo_button(h).left - 1;
    HWND edit = CreateWindowExA(
        0, "EDIT", "",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL |
            (w->style & CBS_AUTOHSCROLL ? ES_AUTOHSCROLL : 0) |
            (w->style & WS_DISABLED ? WS_DISABLED : 0),
        field.left, field.top, field.right - field.left,
        field.bottom - field.top, h, (HANDLE)(uintptr_t)1001, w->instance, 0);
    if ((c = control(h)))
      c->edit = edit;
  }
}

/* ---- class procedure and registration ------------------------------------ */
static void paint(HWND h) {
  struct control *c = control(h);
  if (!c)
    return;
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  if (!dc)
    return;
  c = control(h);
  if (c) {
    DWORD old = select_font(dc, c);
    switch (c->kind) {
    case BUTTON:
      button_paint(h, dc);
      break;
    case STATIC:
      static_paint(h, dc);
      break;
    case EDIT:
      edit_paint(h, dc);
      break;
    case LISTBOX:
    case COMBOLIST:
      list_paint(h, dc);
      break;
    case COMBOBOX:
      combo_paint(h, dc);
      break;
    case SCROLLBAR: {
      RECT r;
      GetClientRect(h, &r);
      struct u32_window *w = u32_window(h);
      if (w && (w->style & SBS_SIZEBOX))
        u32_color_rect(dc, r, GetSysColor(COLOR_BTNFACE));
      else if (w)
        u32_scroll_paint(w, dc, SB_CTL, r);
      break;
    }
    default:
      break;
    }
    restore_font(dc, old);
  }
  EndPaint(h, &ps);
}
void u32_control_cleanup(HWND h) {
  struct u32_window *w = u32_window(h);
  struct control *c = w ? w->control : 0;
  if (!c)
    return;
  w->control = 0;
  if (c->list && IsWindow(c->list))
    DestroyWindow(c->list);
  clear_items(c);
  u32_free(c->text);
  u32_free(c->undo);
  u32_free(c->lines);
  u32_free(c);
}
static LRESULT create(HWND h) {
  struct u32_window *w = u32_window(h);
  struct control *c = u32_alloc(sizeof(*c));
  if (!c)
    return 0;
  w32_fill(c, 0, sizeof(*c));
  for (UINT i = 0; i < sizeof(class_names) / sizeof(*class_names); i++)
    if (!u32_stricmp(w->cls->name, class_names[i]))
      c->kind = i;
  c->text = dup_text(w->title, TEXT_MAX);
  if (!c->text) {
    u32_free(c);
    return 0;
  }
  c->limit = TEXT_MAX;
  c->selection = c->anchor = c->caret = -1;
  c->readonly = !!(w->style & ES_READONLY);
  c->vertical_bar = !!(w->style & WS_VSCROLL);
  c->horizontal_bar = !!(w->style & WS_HSCROLL);
  c->password = c->kind == EDIT && (w->style & ES_PASSWORD) ? '*' : 0;
  c->item_height = (UINT)u32_font_info(0)->height;
  w->control = c;
  return 1;
}
static void font_changed(HWND h, struct control *c) {
  const struct u32_font_info *f = font_of(c);
  struct u32_window *w = u32_window(h);
  if ((c->kind == LISTBOX || c->kind == COMBOBOX || c->kind == COMBOLIST) &&
      !(w->style & (LBS_OWNERDRAWVARIABLE))) {
    c->item_height = (UINT)f->height;
    for (UINT i = 0; i < c->count; i++)
      c->items[i].height = c->item_height;
  }
  if (c->kind == COMBOBOX) {
    c->field_height = f->height + 8;
    if (combo_type(w) != CBS_SIMPLE)
      w->rect.bottom = w->rect.top + c->field_height;
    if (c->edit)
      SendMessageA(c->edit, WM_SETFONT, (WPARAM)c->font, 0);
  }
  if (c->kind == EDIT)
    layout(h, c);
}
LRESULT W32API u32_control_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return 0;
  struct control *c = w->control;
  if (m == WM_NCCREATE)
    return create(h);
  if (m == WM_NCDESTROY) {
    u32_control_cleanup(h);
    return 0;
  }
  if (!c)
    return DefWindowProcA(h, m, wp, lp);
  if (m == WM_CREATE) {
    if (c->kind == COMBOBOX)
      combo_create(h);
    if (c->kind == EDIT)
      layout(h, c);
    if (c->kind == LISTBOX && !(w->style & LBS_NOINTEGRALHEIGHT) &&
        !(w->style & LBS_OWNERDRAWVARIABLE) && c->item_height) {
      int bx, by;
      u32_frame(w->style, w->exstyle, &bx, &by);
      RECT r;
      GetClientRect(h, &r);
      int extra = r.bottom % (int)c->item_height;
      if (extra && r.bottom > (int)c->item_height)
        w->rect.bottom -= extra;
    }
    return 0;
  }
  if (m == WM_PAINT) {
    paint(h);
    return 0;
  }
  if (m == WM_ERASEBKGND)
    return 1;
  if (m == WM_SETFONT) {
    c->font = (HFONT)wp;
    font_changed(h, c);
    if (lp)
      changed(h);
    return 0;
  }
  if (m == WM_GETFONT)
    return (LRESULT)c->font;
  BOOL handled = 0;
  LRESULT r = 0;
  switch (c->kind) {
  case EDIT:
    r = edit_message(h, m, wp, lp, &handled);
    break;
  case BUTTON:
    r = button_message(h, m, wp, lp, &handled);
    break;
  case STATIC:
    r = static_message(h, m, wp, lp, &handled);
    break;
  case LISTBOX:
  case COMBOLIST:
    r = list_message(h, m, wp, lp, &handled);
    break;
  case COMBOBOX:
    r = combo_message(h, m, wp, lp, &handled);
    break;
  case SCROLLBAR:
    r = u32_scrollbar_message(h, m, wp, lp, &handled);
    break;
  default:
    break;
  }
  if (handled)
    return r;
  if (!(c = control(h)))
    return DefWindowProcA(h, m, wp, lp);
  switch (m) {
  case WM_SETTEXT: {
    char *p = dup_text((const char *)lp, TEXT_MAX);
    if (!p)
      return 0;
    u32_free(c->text);
    c->text = p;
    DefWindowProcA(h, m, wp, lp && w32_strlen((const char *)lp) <= 255
                                 ? lp
                                 : (LPARAM) "");
    changed(h);
    return 1;
  }
  case WM_GETTEXTLENGTH:
    return w32_strlen(c->text);
  case WM_GETTEXT: {
    if (!wp || !lp)
      return 0;
    UINT n = w32_strlen(c->text);
    if (n >= wp)
      n = (UINT)wp - 1;
    w32_copy((void *)lp, c->text, n);
    ((char *)lp)[n] = 0;
    return n;
  }
  case WM_SETFOCUS:
  case WM_KILLFOCUS:
  case WM_ENABLE:
    changed(h);
    return 0;
  default:
    return DefWindowProcA(h, m, wp, lp);
  }
}
void u32_controls_register(void) {
  static const char *const extra[] = {U32_DIALOG_CLASS, U32_MENU_CLASS};
  for (UINT i = 0; i < sizeof(class_names) / sizeof(*class_names); i++)
    if (!u32_class(class_names[i], 0)) {
      WNDCLASSA c = {.style = 8,
                     .lpfnWndProc = u32_control_proc,
                     .lpszClassName = class_names[i]};
      RegisterClassA(&c);
    }
  for (UINT i = 0; i < 2; i++)
    if (!u32_class(extra[i], 0)) {
      WNDCLASSA c = {.style = 8,
                     .lpfnWndProc = i ? u32_menu_window_proc : u32_dialog_proc,
                     .cbWndExtra = i ? 0 : U32_DIALOG_EXTRA,
                     .lpszClassName = extra[i]};
      RegisterClassA(&c);
    }
}
