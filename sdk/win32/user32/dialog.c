/* Dialog manager: DIALOG/DIALOGEX templates (bounds-checked), dialog base
 * units from the dialog font, DefDlgProc, IsDialogMessage keyboard rules,
 * modal loop and the dialog item helpers.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "unicode.h"
extern HANDLE W32API FindResourceA(HANDLE, const void *, const void *);
extern HANDLE W32API LoadResource(HANDLE, HANDLE);
extern DWORD W32API SizeofResource(HANDLE, HANDLE);
#define TEMPLATE_MAX (1u << 20)
struct dialog {
  DLGPROC proc;
  BOOL unicode; /* f5-20B: the dialog procedure takes Unicode messages */
  LRESULT msgresult;
  LONG user;
  HFONT font;
  BOOL owns_font, ended, modal, owns_menu;
  int base_x, base_y;
  intptr_t result;
  HWND focus;
  UINT defid;
  HANDLE menu;
};
static struct dialog *dialog(HWND h) {
  struct u32_window *w = u32_window(h);
  return w ? w->dialog : 0;
}
int u32_muldiv(int a, int b, int c) {
  if (!c)
    return -1;
  int64_t v = (int64_t)a * b, half = (c > 0 ? c : -c) / 2;
  return (int)u32_div64(v >= 0 ? v + half : v - half, c);
}

/* ---- resources ---------------------------------------------------------- */
const void *u32_resource(HINSTANCE instance, const char *name, unsigned type,
                         DWORD *size) {
  HANDLE r = FindResourceA(instance, name, (void *)(uintptr_t)type);
  if (!r)
    return 0;
  const void *p = LoadResource(instance, r);
  DWORD n = SizeofResource(instance, r);
  if (!p || !n)
    return 0;
  if (size)
    *size = n;
  return p;
}
int W32API LoadStringA(HINSTANCE instance, UINT id, char *out, int cap) {
  U32_GUARD;
  if (!out || cap < 0)
    return u32_fail(87);
  DWORD n = 0;
  const uint8_t *block = u32_resource(
      instance, (const char *)(uintptr_t)((id >> 4) + 1), 6, &n);
  if (cap)
    out[0] = 0;
  if (!block)
    return u32_fail(1814);
  DWORD at = 0;
  for (UINT i = 0; i < (id & 15); i++) {
    if (at + 2 > n)
      return 0;
    at += 2 + 2u * w32_u16(block + at);
  }
  if (at + 2 > n)
    return 0;
  UINT length = w32_u16(block + at);
  if (at + 2 + 2u * length > n)
    return 0;
  if (!cap)
    return 0;
  UINT k = length < (UINT)cap - 1 ? length : (UINT)cap - 1;
  for (UINT i = 0; i < k; i++) {
    int c = u32_ansi(w32_u16(block + at + 2 + 2 * i));
    out[i] = (char)(c >= 0 ? c : '?');
  }
  out[k] = 0;
  return (int)k;
}
int W32API LoadStringW(HINSTANCE instance, UINT id, WCHAR *out, int cap) {
  U32_GUARD;
  if (!out || cap < 0)
    return u32_fail(87);
  DWORD n = 0;
  const uint8_t *block = u32_resource(
      instance, (const char *)(uintptr_t)((id >> 4) + 1), 6, &n);
  if (!block)
    return u32_fail(1814);
  DWORD at = 0;
  for (UINT i = 0; i < (id & 15); i++) {
    if (at + 2 > n)
      return 0;
    at += 2 + 2u * w32_u16(block + at);
  }
  if (at + 2 > n)
    return 0;
  UINT length = w32_u16(block + at);
  if (at + 2 + 2u * length > n)
    return 0;
  if (!cap) {
    /* Read-only pointer to the resource string, as Win32 documents. */
    *(const void **)out = block + at + 2;
    return (int)length;
  }
  UINT k = length < (UINT)cap - 1 ? length : (UINT)cap - 1;
  for (UINT i = 0; i < k; i++)
    out[i] = (WCHAR)w32_u16(block + at + 2 + 2 * i);
  out[k] = 0;
  return (int)k;
}

/* ---- bitmaps: RT_BITMAP resources and .BMP files (packed DIBs) ----------- */
static HANDLE dib_to_bitmap(const uint8_t *dib, DWORD size) {
  if (!dib || size < 40 || w32_u32(dib) < 40 || w32_u32(dib) > size)
    return (HANDLE)(uintptr_t)u32_fail(13);
  DWORD header = w32_u32(dib), compression = w32_u32(dib + 16),
        used = w32_u32(dib + 32);
  LONG width = (LONG)w32_u32(dib + 4), height = (LONG)w32_u32(dib + 8);
  UINT bpp = w32_u16(dib + 14);
  if (width <= 0 || width > 4096 || !height || height > 4096 ||
      height < -4096 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 &&
                         bpp != 24 && bpp != 32) ||
      (compression != 0 && compression != 3))
    return (HANDLE)(uintptr_t)u32_fail(13);
  DWORD colors = bpp <= 8 ? (used && used <= (1u << bpp) ? used : 1u << bpp) : 0;
  DWORD table = compression == 3 ? 12 : colors * 4;
  DWORD rows = (DWORD)(height < 0 ? -height : height),
        pitch = (((DWORD)width * bpp + 31) / 32) * 4;
  if ((uint64_t)header + table + (uint64_t)pitch * rows > size)
    return (HANDLE)(uintptr_t)u32_fail(13);
  const struct u32_graphics *g = u32_graphics();
  if (!g || !g->dib_bitmap || !g->memory_dc)
    return (HANDLE)(uintptr_t)u32_fail(120);
  DWORD dc = g->memory_dc(0);
  /* A display-compatible bitmap, as LoadBitmap returns. */
  DWORD bitmap = g->dib_bitmap(dc, dib, 4, dib + header + table, dib, 0);
  g->delete_dc(dc);
  return (HANDLE)(uintptr_t)bitmap;
}
HANDLE W32API LoadBitmapA(HINSTANCE instance, const char *name) {
  U32_GUARD;
  if (!instance)
    return (HANDLE)(uintptr_t)u32_fail(1814);
  DWORD size = 0;
  const uint8_t *dib = u32_resource(instance, name, 2, &size);
  if (!dib)
    return (HANDLE)(uintptr_t)u32_fail(1814);
  return dib_to_bitmap(dib, size);
}
static HANDLE bitmap_file(const char *path) {
  HANDLE file = CreateFileA(path, 0x80000000u, 1, 0, 3, 0, 0);
  if (!file || file == (HANDLE)(intptr_t)-1)
    return (HANDLE)(uintptr_t)u32_fail(2);
  DWORD cap = 1u << 16, n = 0;
  uint8_t *data = u32_alloc(cap);
  for (BOOL more = data != 0; more;) {
    DWORD got = 0;
    if (n == cap) {
      uint8_t *bigger = cap < (8u << 20) ? u32_alloc(cap * 2) : 0;
      if (!bigger) {
        u32_free(data);
        data = 0;
        break;
      }
      w32_copy(bigger, data, n);
      u32_free(data);
      data = bigger;
      cap *= 2;
    }
    more = ReadFile(file, data + n, cap - n, &got, 0) && got;
    n += got;
  }
  CloseHandle(file);
  HANDLE h = 0;
  /* BITMAPFILEHEADER: 'BM', size, reserved, bits offset; then the DIB. */
  if (data && n > 54 && data[0] == 'B' && data[1] == 'M') {
    /* Realign the DIB that follows the 14-byte file header. */
    for (DWORD i = 14; i < n; i++)
      data[i - 14] = data[i];
    h = dib_to_bitmap(data, n - 14);
  }
  else
    u32_fail(13);
  u32_free(data);
  return h;
}
HANDLE W32API LoadImageA(HINSTANCE instance, const char *name, UINT type,
                         int cx, int cy, UINT flags) {
  U32_GUARD;
  (void)cx, (void)cy;
  /* Requested sizes other than the image's own are not resampled (W21). */
  if (type == 0)
    return flags & 0x10 ? bitmap_file(name) : LoadBitmapA(instance, name);
  if (flags & 0x10)
    return (HANDLE)(uintptr_t)u32_fail(120);
  if (type == 1)
    return LoadIconA(instance, name);
  if (type == 2)
    return LoadCursorA(instance, name);
  return (HANDLE)(uintptr_t)u32_fail(87);
}

/* ---- template parser ------------------------------------------------------ */
struct reader {
  const uint8_t *p;
  DWORD n, at;
  BOOL bad;
};
static UINT r16(struct reader *r) {
  if (r->bad || r->at + 2 > r->n) {
    r->bad = 1;
    return 0;
  }
  UINT v = w32_u16(r->p + r->at);
  r->at += 2;
  return v;
}
static DWORD r32(struct reader *r) {
  UINT lo = r16(r), hi = r16(r);
  return lo | ((DWORD)hi << 16);
}
static void align4(struct reader *r) {
  r->at = (r->at + 3) & ~3u;
  if (r->at > r->n)
    r->bad = 1;
}
static void r_string(struct reader *r, char *out, UINT cap) {
  UINT n = 0;
  for (;;) {
    UINT c = r16(r);
    if (r->bad || !c)
      break;
    if (n + 1 < cap) {
      int a = u32_ansi(c);
      out[n++] = (char)(a >= 0 ? a : '?');
    }
  }
  out[n] = 0;
}
static void r_name(struct reader *r, char *out, UINT cap, UINT *ordinal) {
  *ordinal = 0;
  out[0] = 0;
  if (r->bad || r->at + 2 > r->n) {
    r->bad = 1;
    return;
  }
  UINT first = w32_u16(r->p + r->at);
  if (first == 0) {
    r->at += 2;
    return;
  }
  if (first == 0xffff) {
    r->at += 2;
    *ordinal = r16(r);
    return;
  }
  r_string(r, out, cap);
}
static const char *const atoms[] = {"BUTTON",    "EDIT",    "STATIC",
                                    "LISTBOX",   "SCROLLBAR", "COMBOBOX"};
BOOL u32_template_parse(const void *data, DWORD size, struct u32_template *t) {
  struct reader r = {data, size, 0, 0};
  w32_fill(t, 0, sizeof(*t));
  if (!data || size < 18)
    return 0;
  t->extended = w32_u16(data) == 1 && w32_u16((const uint8_t *)data + 2) == 0xffff;
  UINT count;
  if (t->extended) {
    r.at = 4;
    t->help = r32(&r);
    t->ex = r32(&r);
    t->style = r32(&r);
  } else {
    t->style = r32(&r);
    t->ex = r32(&r);
  }
  count = r16(&r);
  t->x = (int16_t)r16(&r);
  t->y = (int16_t)r16(&r);
  t->cx = (int16_t)r16(&r);
  t->cy = (int16_t)r16(&r);
  r_name(&r, t->menu, sizeof(t->menu), &t->menu_ordinal);
  UINT class_ordinal;
  r_name(&r, t->klass, sizeof(t->klass), &class_ordinal);
  /* A class ordinal names a registered atom; only the dialog class itself
   * (atom 32770, "#32770") is meaningful for dialog templates. */
  if (class_ordinal)
    u32_strcpy(t->klass, sizeof(t->klass), U32_DIALOG_CLASS);
  r_string(&r, t->title, sizeof(t->title));
  /* Font data is present exactly when DS_SETFONT is set (DS_SHELLFONT
   * includes it). */
  if (t->style & DS_SETFONT) {
    t->has_font = 1;
    t->point = (int16_t)r16(&r);
    if (t->extended) {
      t->weight = r16(&r);
      UINT pair = r16(&r);
      t->italic = (pair & 0xff) != 0;
      t->charset = (uint8_t)(pair >> 8);
    }
    r_string(&r, t->face, sizeof(t->face));
  }
  if (r.bad || count > U32_TEMPLATE_ITEMS || t->cx < 0 || t->cy < 0)
    return 0;
  for (UINT i = 0; i < count; i++) {
    struct u32_template_item *it = &t->items[i];
    align4(&r);
    if (t->extended) {
      it->help = r32(&r);
      it->ex = r32(&r);
      it->style = r32(&r);
    } else {
      it->style = r32(&r);
      it->ex = r32(&r);
    }
    it->x = (int16_t)r16(&r);
    it->y = (int16_t)r16(&r);
    it->cx = (int16_t)r16(&r);
    it->cy = (int16_t)r16(&r);
    it->id = t->extended ? r32(&r) : r16(&r);
    UINT atom;
    r_name(&r, it->klass, sizeof(it->klass), &atom);
    if (atom) {
      if (atom < 0x80 || atom > 0x85) {
        r.bad = 1;
        break;
      }
      u32_strcpy(it->klass, sizeof(it->klass), atoms[atom - 0x80]);
    }
    if (!it->klass[0])
      r.bad = 1;
    r_name(&r, it->text, sizeof(it->text), &it->text_ordinal);
    UINT extra = r16(&r);
    if (r.bad)
      break;
    if (extra) {
      if (r.at + extra > r.n) {
        r.bad = 1;
        break;
      }
      /* lpCreateParams points at the size word, as Win32 passes it. */
      it->data = r.p + r.at - 2;
      it->data_bytes = extra;
      r.at += extra;
    }
  }
  if (r.bad)
    return 0;
  t->count = count;
  return 1;
}

/* ---- base units ------------------------------------------------------------ */
LONG W32API GetDialogBaseUnits(void) {
  U32_GUARD;
  const struct u32_font_info *f = u32_font_info(0);
  return (LONG)((uint16_t)f->base_x | ((uint32_t)(uint16_t)f->base_y << 16));
}
static void base_units(HWND h, int *x, int *y) {
  struct dialog *d = dialog(h);
  if (d) {
    *x = d->base_x;
    *y = d->base_y;
    return;
  }
  const struct u32_font_info *f = u32_font_info(0);
  *x = f->base_x;
  *y = f->base_y;
}
BOOL W32API MapDialogRect(HWND h, RECT *r) {
  U32_GUARD;
  if (!r || !dialog(h))
    return u32_fail(1400);
  int x, y;
  base_units(h, &x, &y);
  r->left = u32_muldiv(r->left, x, 4);
  r->right = u32_muldiv(r->right, x, 4);
  r->top = u32_muldiv(r->top, y, 8);
  r->bottom = u32_muldiv(r->bottom, y, 8);
  return 1;
}

/* ---- tab order, groups and focus ----------------------------------------------
 * Controls keep template order as z order (the first control is on top). */
static UINT children(HWND parent, HWND *out, UINT cap) {
  UINT n = 0;
  for (UINT i = 0; i < U32_WINDOWS && n < cap; i++) {
    struct u32_window *w = &u32_windows[i];
    if (w->handle && w->parent == parent && (w->style & WS_CHILD))
      out[n++] = w->handle;
  }
  for (UINT i = 1; i < n; i++)
    for (UINT j = i; j > 0; j--) {
      struct u32_window *a = u32_window(out[j - 1]), *b = u32_window(out[j]);
      if (b->z <= a->z)
        break;
      HWND t = out[j - 1];
      out[j - 1] = out[j];
      out[j] = t;
    }
  return n;
}
/* Flattened tab order, descending into WS_EX_CONTROLPARENT children. */
static UINT flatten(HWND parent, HWND *out, UINT n, UINT cap, UINT depth) {
  HWND list[U32_WINDOWS];
  UINT count = children(parent, list, U32_WINDOWS);
  for (UINT i = 0; i < count && n < cap; i++) {
    struct u32_window *w = u32_window(list[i]);
    if ((w->exstyle & WS_EX_CONTROLPARENT) && depth < 8) {
      if ((w->style & WS_VISIBLE) && !(w->style & WS_DISABLED))
        n = flatten(list[i], out, n, cap, depth + 1);
    } else
      out[n++] = list[i];
  }
  return n;
}
/* The control's own state decides; hidden or disabled control-parent
 * containers are already skipped by flatten(). */
static BOOL usable(HWND h) {
  struct u32_window *w = u32_window(h);
  return w && (w->style & WS_VISIBLE) && !(w->style & WS_DISABLED);
}
HWND W32API GetNextDlgTabItem(HWND dlg, HWND ctl, BOOL previous) {
  U32_GUARD;
  if (!IsWindow(dlg))
    return (HWND)(uintptr_t)u32_fail(1400);
  HWND list[U32_WINDOWS];
  UINT n = flatten(dlg, list, 0, U32_WINDOWS, 0);
  if (!n)
    return 0;
  int start = -1;
  for (UINT i = 0; i < n; i++)
    if (list[i] == ctl)
      start = (int)i;
  for (UINT k = 1; k <= n; k++) {
    int i = start < 0 ? (previous ? (int)n - (int)k : (int)k - 1)
                      : (int)(((int)start + (previous ? -(int)k : (int)k) +
                               (int)n * 2) %
                              (int)n);
    struct u32_window *w = u32_window(list[i]);
    if (w && (w->style & WS_TABSTOP) && usable(list[i]))
      return list[i];
  }
  return ctl;
}
HWND W32API GetNextDlgGroupItem(HWND dlg, HWND ctl, BOOL previous) {
  U32_GUARD;
  if (!IsWindow(dlg))
    return (HWND)(uintptr_t)u32_fail(1400);
  HWND list[U32_WINDOWS];
  UINT n = flatten(dlg, list, 0, U32_WINDOWS, 0);
  int at = -1;
  for (UINT i = 0; i < n; i++)
    if (list[i] == ctl)
      at = (int)i;
  if (at < 0)
    return n ? list[0] : 0;
  int first = at, last = at;
  while (first > 0 && !(u32_window(list[first])->style & WS_GROUP))
    first--;
  while (last + 1 < (int)n && !(u32_window(list[last + 1])->style & WS_GROUP))
    last++;
  int size = last - first + 1;
  for (int k = 1; k < size; k++) {
    int i = first + ((at - first + (previous ? -k : k)) % size + size) % size;
    if (usable(list[i]))
      return list[i];
  }
  return ctl;
}
HWND u32_dialog_of(HWND h) {
  for (struct u32_window *w = u32_window(h); w; w = u32_window(w->parent)) {
    if (w->dialog && !(w->exstyle & WS_EX_CONTROLPARENT && w->style & WS_CHILD &&
                       !w->dialog))
      return w->handle;
    if (!(w->style & WS_CHILD))
      break;
  }
  for (struct u32_window *w = u32_window(h); w; w = u32_window(w->parent))
    if (w->dialog)
      return w->handle;
  return 0;
}
static UINT dlg_code(HWND h, MSG *m) {
  return (UINT)SendMessageA(h, WM_GETDLGCODE, m ? m->wParam : 0, (LPARAM)m);
}
static HWND find_default(HWND dlg, UINT id) {
  HWND list[U32_WINDOWS];
  UINT n = flatten(dlg, list, 0, U32_WINDOWS, 0);
  for (UINT i = 0; i < n; i++)
    if (u32_window(list[i])->id == id)
      return list[i];
  return 0;
}
static void set_button(HWND h, BOOL defaulted) {
  struct u32_window *w = u32_window(h);
  if (!w)
    return;
  UINT type = w->style & 15;
  if ((type == BS_DEFPUSHBUTTON) == !!defaulted)
    return;
  SendMessageA(h, BM_SETSTYLE,
               (w->style & 0xfff0u) |
                   (defaulted ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
               1);
}
/* Focus change with the classic default-button transfer. */
void u32_dialog_focus(HWND dlg, HWND target) {
  if (!target || !IsWindow(target))
    return;
  UINT code = dlg_code(target, 0);
  HWND list[U32_WINDOWS];
  UINT n = flatten(dlg, list, 0, U32_WINDOWS, 0);
  struct dialog *d = dialog(dlg);
  UINT defid = d ? d->defid : 0;
  if (code & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)) {
    for (UINT i = 0; i < n; i++)
      if (list[i] != target && (dlg_code(list[i], 0) & DLGC_DEFPUSHBUTTON))
        set_button(list[i], 0);
    set_button(target, 1);
  } else {
    for (UINT i = 0; i < n; i++) {
      UINT c = dlg_code(list[i], 0);
      if (c & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON))
        set_button(list[i], defid && u32_window(list[i])->id == defid);
    }
  }
  if (code & DLGC_HASSETSEL)
    SendMessageA(target, EM_SETSEL, 0, -1);
  SetFocus(target);
  if ((d = dialog(dlg)))
    d->focus = target;
}

/* ---- dialog creation ------------------------------------------------------- */
static HFONT dialog_font(const struct u32_template *t) {
  const struct u32_graphics *g = u32_graphics();
  if (!g || !g->font_indirect)
    return 0;
  if (!t->has_font)
    return (t->style & DS_FIXEDSYS) && g->stock
               ? (HFONT)(uintptr_t)g->stock(16)
               : 0;
  if (t->point == 0x7fff)
    return 0;
  struct u32_logfont lf = {0};
  lf.height = -u32_muldiv(t->point, 96, 72);
  lf.weight = t->weight;
  lf.italic = (uint8_t)t->italic;
  lf.charset = t->extended ? t->charset : 1;
  u32_strcpy(lf.face, sizeof(lf.face), t->face);
  return (HFONT)(uintptr_t)g->font_indirect(&lf);
}
HWND u32_dialog_create(HINSTANCE instance, const struct u32_template *t,
                       HWND owner, DLGPROC proc, LPARAM param, BOOL modal) {
  return u32_dialog_create_ex(instance, t, owner, proc, param, modal, 0);
}
HWND u32_dialog_create_ex(HINSTANCE instance, const struct u32_template *t,
                          HWND owner, DLGPROC proc, LPARAM param, BOOL modal,
                          BOOL wide) {
  u32_controls_register();
  HFONT font = dialog_font(t);
  BOOL owns_font = font && t->has_font;
  const struct u32_font_info *f = u32_font_info(font);
  int bx = f->base_x, by = f->base_y;
  DWORD style = t->style, ex = t->ex;
  if (style & DS_MODALFRAME)
    ex |= WS_EX_DLGMODALFRAME;
  if (style & DS_CONTEXTHELP)
    ex |= WS_EX_CONTEXTHELP; /* G15: the "?" caption button */
  if (style & DS_CONTROL) {
    style = (style & ~(WS_CAPTION | 0x80000u | WS_POPUP)) | WS_CHILD;
    ex |= WS_EX_CONTROLPARENT;
  }
  HANDLE menu = 0;
  if (!(style & WS_CHILD) && (t->menu[0] || t->menu_ordinal))
    menu = LoadMenuA(instance, t->menu_ordinal
                                   ? (const char *)(uintptr_t)t->menu_ordinal
                                   : t->menu);
  RECT r = {u32_muldiv(t->x, bx, 4), u32_muldiv(t->y, by, 8), 0, 0};
  r.right = r.left + u32_muldiv(t->cx, bx, 4);
  r.bottom = r.top + u32_muldiv(t->cy, by, 8);
  AdjustWindowRectEx(&r, style, menu != 0, ex);
  int width = r.right - r.left, height = r.bottom - r.top;
  if (style & WS_CHILD) {
    /* Template coordinates are relative to the parent's client area. */
  } else if (style & DS_CENTER) {
    RECT work = {0, 0, GetSystemMetrics(0), GetSystemMetrics(1)};
    SystemParametersInfoA(48, 0, &work, 0);
    r.left = work.left + (work.right - work.left - width) / 2;
    r.top = work.top + (work.bottom - work.top - height) / 2;
  } else if (!(style & DS_ABSALIGN) && owner) {
    POINT o = {0, 0};
    ClientToScreen(owner, &o);
    r.left += o.x;
    r.top += o.y;
  }
  if (!(style & WS_CHILD)) {
    int sw = GetSystemMetrics(0), sh = GetSystemMetrics(1);
    if (r.left + width > sw)
      r.left = sw - width;
    if (r.top + height > sh)
      r.top = sh - height;
    if (r.left < 0)
      r.left = 0;
    if (r.top < 0)
      r.top = 0;
  }
  const char *klass = t->klass[0] ? t->klass : U32_DIALOG_CLASS;
  HWND h = CreateWindowExA(ex, klass, t->title, style & ~WS_VISIBLE, r.left,
                           r.top, width, height, owner, menu, instance, 0);
  if (!h) {
    if (owns_font)
      u32_graphics()->delete_object(U32_PTR(font));
    if (menu)
      DestroyMenu(menu);
    return 0;
  }
  struct u32_window *w = u32_window(h);
  struct dialog *d = u32_alloc(sizeof(*d));
  if (!d) {
    DestroyWindow(h);
    return 0;
  }
  w32_fill(d, 0, sizeof(*d));
  d->proc = proc;
  d->unicode = !!wide;
  /* f5-20B: a dialog created by a W function is a Unicode window whose
   * procedure is DefDlgProcW (IsWindowUnicode, GetWindowLongW). */
  if (wide && w->proc == u32_dialog_proc) {
    w->proc = DefDlgProcW;
    w->unicode = 1;
  }
  d->font = font;
  d->owns_font = owns_font;
  d->base_x = bx;
  d->base_y = by;
  d->modal = modal;
  d->menu = menu;
  d->owns_menu = menu != 0;
  w->dialog = d;
  if (font)
    SendMessageA(h, WM_SETFONT, (WPARAM)font, 0);
  static UINT order = 0x7fffffffu;
  for (UINT i = 0; i < t->count && IsWindow(h); i++) {
    const struct u32_template_item *it = &t->items[i];
    DWORD cs = it->style | WS_CHILD, cex = it->ex;
    BOOL edit_like = !u32_stricmp(it->klass, "EDIT") ||
                     !u32_stricmp(it->klass, "LISTBOX") ||
                     !u32_stricmp(it->klass, "COMBOBOX");
    if (edit_like && (cs & WS_BORDER)) {
      cs &= ~WS_BORDER;
      cex |= WS_EX_CLIENTEDGE;
    }
    int x = u32_muldiv(it->x, bx, 4), y = u32_muldiv(it->y, by, 8),
        cx = u32_muldiv(it->cx, bx, 4), cy = u32_muldiv(it->cy, by, 8);
    HWND c = CreateWindowExA(cex, it->klass, it->text, cs, x, y, cx, cy, h,
                             (HANDLE)(uintptr_t)it->id, instance,
                             (void *)(uintptr_t)it->data);
    if (!c) {
      if (t->style & DS_NOFAILCREATE)
        continue;
      DestroyWindow(h);
      return 0;
    }
    struct u32_window *cw = u32_window(c);
    if (cw)
      cw->z = order--;
    if (it->text_ordinal) {
      UINT type = cs & SS_TYPEMASK;
      if (!u32_stricmp(it->klass, "STATIC") && type == SS_ICON) {
        HICON icon = LoadIconA(instance, (const char *)(uintptr_t)it->text_ordinal);
        if (icon)
          SendMessageA(c, STM_SETICON, (WPARAM)icon, 0);
      }
      if (!u32_stricmp(it->klass, "STATIC") && type == SS_BITMAP) {
        HANDLE bitmap =
            LoadBitmapA(instance, (const char *)(uintptr_t)it->text_ordinal);
        if (bitmap)
          SendMessageA(c, STM_SETIMAGE, 0, (LPARAM)bitmap);
      }
    }
    if (font)
      SendMessageA(c, WM_SETFONT, (WPARAM)font, 0);
    if ((d = dialog(h)) && !d->defid && !u32_stricmp(it->klass, "BUTTON") &&
        (cs & 15) == BS_DEFPUSHBUTTON)
      d->defid = it->id;
  }
  if (!IsWindow(h))
    return 0;
  HWND first = GetNextDlgTabItem(h, 0, 0);
  /* TRUE asks the dialog manager to focus wParam's control; FALSE leaves
   * the focus the procedure chose. */
  BOOL set_focus = 0;
  if (SendMessageA(h, WM_INITDIALOG, (WPARAM)first, param) && IsWindow(h)) {
    HWND target = IsWindow(first) ? first : GetNextDlgTabItem(h, 0, 0);
    if ((d = dialog(h)))
      d->focus = target;
    set_focus = 1;
  }
  if (!IsWindow(h))
    return 0;
  if ((t->style & WS_VISIBLE) && !(GetWindowLongA(h, GWL_STYLE) & WS_VISIBLE)) {
    ShowWindow(h, 5);
    UpdateWindow(h);
  }
  if (set_focus && (d = dialog(h)) && d->focus && IsWindow(d->focus))
    u32_dialog_focus(h, d->focus);
  return h;
}
HWND u32_dialog_from(HINSTANCE instance, const void *data, DWORD size,
                     HWND owner, DLGPROC proc, LPARAM param, BOOL modal,
                     BOOL wide) {
  struct u32_template *t = u32_alloc(sizeof(*t));
  if (!t)
    return (HWND)(uintptr_t)u32_fail(8);
  HWND h = 0;
  if (!u32_template_parse(data, size, t))
    u32_fail(1812);
  else {
    if (modal)
      t->style |= WS_VISIBLE;
    h = u32_dialog_create_ex(instance, t, owner, proc, param, modal, wide);
  }
  u32_free(t);
  return h;
}
HWND W32API w32_create_dialog(HINSTANCE instance, const void *data,
                              DWORD size, HWND owner, DLGPROC proc,
                              LPARAM param) {
  U32_GUARD;
  return u32_dialog_from(instance, data, size, owner, proc, param, 0, 0);
}
HWND W32API CreateDialogIndirectParamA(HINSTANCE instance,
                                       const DLGTEMPLATE *t, HWND owner,
                                       DLGPROC proc, LPARAM param) {
  U32_GUARD;
  return u32_dialog_from(instance, t, TEMPLATE_MAX, owner, proc, param, 0, 0);
}
HWND W32API CreateDialogIndirectA(HINSTANCE i, const DLGTEMPLATE *t, HWND o,
                                  DLGPROC p) {
  return CreateDialogIndirectParamA(i, t, o, p, 0);
}
HWND W32API CreateDialogParamA(HINSTANCE instance, const char *name,
                               HWND owner, DLGPROC proc, LPARAM param) {
  U32_GUARD;
  DWORD size = 0;
  const void *data = u32_resource(instance, name, 5, &size);
  if (!data)
    return (HWND)(uintptr_t)u32_fail(1812);
  return u32_dialog_from(instance, data, size, owner, proc, param, 0, 0);
}
HWND W32API CreateDialogA(HINSTANCE i, const char *n, HWND o, DLGPROC p) {
  return CreateDialogParamA(i, n, o, p, 0);
}

/* ---- modal loop ----------------------------------------------------------- */
static HWND top_owner(HWND h) {
  struct u32_window *w = u32_window(h);
  while (w && (w->style & WS_CHILD) && w->parent)
    w = u32_window(w->parent);
  return w ? w->handle : 0;
}
intptr_t u32_dialog_run(HWND h, HWND owner) {
  struct dialog *d = dialog(h);
  if (!d)
    return -1;
  d->modal = 1;
  BOOL disabled = 0;
  if (owner && IsWindowEnabled(owner)) {
    EnableWindow(owner, 0);
    disabled = 1;
  }
  MSG msg;
  intptr_t result = -1;
  BOOL quit = 0;
  WPARAM quit_code = 0;
  for (;;) {
    d = dialog(h);
    if (!d || !IsWindow(h))
      break;
    if (d->ended) {
      result = d->result;
      break;
    }
    if (!PeekMessageA(&msg, 0, 0, 0, 0)) {
      struct u32_window *w = u32_window(h);
      if (w && !(w->style & DS_NOIDLEMSG) && owner)
        SendMessageA(owner, WM_ENTERIDLE, MSGF_DIALOGBOX, (LPARAM)h);
      if (!(d = dialog(h)) || d->ended)
        continue;
    }
    BOOL got = GetMessageA(&msg, 0, 0, 0);
    if ((int)got == -1)
      break;
    if (!got) {
      quit = 1;
      quit_code = msg.wParam;
      break;
    }
    if (!IsDialogMessageA(h, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }
  if (disabled && IsWindow(owner))
    EnableWindow(owner, 1);
  if (owner && IsWindow(owner) && IsWindow(h) && GetActiveWindow() == h)
    SetActiveWindow(owner);
  if (IsWindow(h))
    DestroyWindow(h);
  if (quit)
    PostQuitMessage((int)quit_code);
  return result;
}
intptr_t u32_dialog_box(HINSTANCE instance, const void *data, DWORD size,
                        HWND parent, DLGPROC proc, LPARAM param, BOOL wide) {
  HWND owner = top_owner(parent);
  HWND h = u32_dialog_from(instance, data, size, owner, proc, param, 1, wide);
  if (!h)
    return -1;
  return u32_dialog_run(h, owner);
}
intptr_t W32API DialogBoxIndirectParamA(HINSTANCE instance,
                                        const DLGTEMPLATE *t, HWND parent,
                                        DLGPROC proc, LPARAM param) {
  U32_GUARD;
  return u32_dialog_box(instance, t, TEMPLATE_MAX, parent, proc, param, 0);
}
intptr_t W32API DialogBoxIndirectA(HINSTANCE i, const DLGTEMPLATE *t, HWND o,
                                   DLGPROC p) {
  return DialogBoxIndirectParamA(i, t, o, p, 0);
}
intptr_t W32API DialogBoxParamA(HINSTANCE instance, const char *name,
                                HWND parent, DLGPROC proc, LPARAM param) {
  U32_GUARD;
  DWORD size = 0;
  const void *data = u32_resource(instance, name, 5, &size);
  if (!data) {
    u32_fail(1812);
    return -1;
  }
  return u32_dialog_box(instance, data, size, parent, proc, param, 0);
}
intptr_t W32API DialogBoxA(HINSTANCE i, const char *n, HWND o, DLGPROC p) {
  return DialogBoxParamA(i, n, o, p, 0);
}
BOOL W32API EndDialog(HWND h, intptr_t result) {
  U32_GUARD;
  struct dialog *d = dialog(h);
  if (!d)
    return u32_fail(1400);
  d->result = result;
  d->ended = 1;
  if (d->modal) {
    struct u32_window *w = u32_window(h);
    HWND owner = w ? w->owner : 0;
    if (owner && IsWindow(owner))
      EnableWindow(owner, 1);
    ShowWindow(h, 0);
    PostMessageA(h, 0, 0, 0);
  }
  return 1;
}

/* ---- DefDlgProc --------------------------------------------------------- */
static BOOL returns_directly(UINT m) {
  return (m >= WM_CTLCOLORMSGBOX && m <= WM_CTLCOLORSTATIC) ||
         m == WM_COMPAREITEM || m == WM_VKEYTOITEM || m == WM_CHARTOITEM ||
         m == WM_QUERYDRAGICON || m == WM_INITDIALOG;
}
static LRESULT dialog_default(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  struct dialog *d = dialog(h);
  switch (m) {
  case WM_ERASEBKGND: {
    RECT r;
    GetClientRect(h, &r);
    HBRUSH b = (HBRUSH)SendMessageA(h, WM_CTLCOLORDLG, wp, (LPARAM)h);
    if (!b)
      b = GetSysColorBrush(COLOR_BTNFACE);
    FillRect((HDC)wp, &r, b);
    return 1;
  }
  case WM_SETFONT:
    if (d)
      d->font = (HFONT)wp;
    return 0;
  case WM_GETFONT:
    return d ? (LRESULT)d->font : 0;
  case DM_GETDEFID:
    return d && d->defid ? (LRESULT)(d->defid | (DC_HASDEFID << 16)) : 0;
  case DM_SETDEFID: {
    if (!d)
      return 0;
    UINT old = d->defid;
    d->defid = (UINT)wp;
    HWND focus = GetFocus();
    if (!(focus && IsChild(h, focus) &&
          (dlg_code(focus, 0) & (DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON)))) {
      HWND a = old ? find_default(h, old) : 0, b = find_default(h, d->defid);
      if (a)
        set_button(a, 0);
      if (b)
        set_button(b, 1);
    }
    return 1;
  }
  case WM_NEXTDLGCTL: {
    HWND target = (lp & 0xffff)
                      ? (HWND)wp
                      : GetNextDlgTabItem(h, GetFocus(), wp != 0);
    if (target)
      u32_dialog_focus(h, target);
    return 0;
  }
  case WM_CLOSE: {
    HWND cancel = GetDlgItem(h, IDCANCEL);
    if (!cancel || IsWindowEnabled(cancel))
      PostMessageA(h, WM_COMMAND, IDCANCEL | (BN_CLICKED << 16),
                   (LPARAM)cancel);
    return 0;
  }
  case WM_SETFOCUS:
    /* Restoring the saved focus keeps the edit selection. */
    if (d && d->focus && IsWindow(d->focus) && IsChild(h, d->focus))
      SetFocus(d->focus);
    else {
      HWND first = GetNextDlgTabItem(h, 0, 0);
      if (first)
        u32_dialog_focus(h, first);
    }
    return 0;
  case WM_ACTIVATE:
    if (d && !(wp & 0xffff)) {
      HWND focus = GetFocus();
      if (focus && IsChild(h, focus))
        d->focus = focus;
    } else if (d && d->focus && IsWindow(d->focus) && GetFocus() != d->focus)
      SetFocus(d->focus);
    return 0;
  case WM_SHOWWINDOW:
    if (wp && d && !d->focus) {
      HWND focus = GetFocus();
      if (focus && IsChild(h, focus))
        d->focus = focus;
    }
    return DefWindowProcA(h, m, wp, lp);
  case WM_GETDLGCODE:
    return 0;
  default:
    return DefWindowProcA(h, m, wp, lp);
  }
}
LRESULT W32API DefDlgProcA(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  struct dialog *d = dialog(h);
  if (!d)
    return DefWindowProcA(h, m, wp, lp);
  if (d->proc) {
    d->msgresult = 0;
    intptr_t r = u32_call_proc((WNDPROC)d->proc, d->unicode, 0, h, m, wp, lp);
    d = dialog(h);
    if (r || !d) {
      if (returns_directly(m))
        return r;
      return d ? d->msgresult : r;
    }
  }
  if (m == WM_NCDESTROY) {
    u32_dialog_destroyed(h);
    return DefWindowProcA(h, m, wp, lp);
  }
  return dialog_default(h, m, wp, lp);
}
static LRESULT dialog_default_ansi(void *unused, HWND h, UINT m, WPARAM wp,
                                   LPARAM lp) {
  (void)unused;
  return dialog_default(h, m, wp, lp);
}
LRESULT W32API DefDlgProcW(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  U32_GUARD;
  struct dialog *d = dialog(h);
  if (!d)
    return DefWindowProcW(h, m, wp, lp);
  if (d->proc) {
    d->msgresult = 0;
    intptr_t r = u32_call_proc((WNDPROC)d->proc, d->unicode, 1, h, m, wp, lp);
    d = dialog(h);
    if (r || !d) {
      if (returns_directly(m))
        return r;
      return d ? d->msgresult : r;
    }
  }
  if (m == WM_NCDESTROY) {
    u32_dialog_destroyed(h);
    return DefWindowProcW(h, m, wp, lp);
  }
  return u32_convert(0, dialog_default_ansi, 0, h, m, wp, lp);
}
LRESULT W32API u32_dialog_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  return DefDlgProcA(h, m, wp, lp);
}
void u32_dialog_destroyed(HWND h) {
  struct u32_window *w = u32_window(h);
  struct dialog *d = w ? w->dialog : 0;
  if (!d)
    return;
  w->dialog = 0;
  const struct u32_graphics *g = u32_graphics();
  if (d->owns_font && d->font && g)
    g->delete_object(U32_PTR(d->font));
  u32_free(d);
}
int u32_dialog_long(HWND h, int index, LONG *value, BOOL set) {
  return u32_dialog_long_ex(h, index, value, set, 0);
}
int u32_dialog_long_ex(HWND h, int index, LONG *value, BOOL set, BOOL wide) {
  struct dialog *d = dialog(h);
  if (!d || (index != DWL_MSGRESULT && index != DWL_DLGPROC &&
             index != DWL_USER))
    return 0;
  LONG old = index == DWL_MSGRESULT ? (LONG)d->msgresult
             : index == DWL_DLGPROC
                 ? (LONG)u32_proc_handle((WNDPROC)d->proc, d->unicode, wide)
                 : d->user;
  if (set) {
    if (index == DWL_MSGRESULT)
      d->msgresult = *value;
    else if (index == DWL_DLGPROC) {
      BOOL proc_wide;
      d->proc = (DLGPROC)u32_proc_decode((uintptr_t)(DWORD)*value, wide,
                                         &proc_wide);
      d->unicode = proc_wide;
    } else
      d->user = *value;
  }
  *value = old;
  return 1;
}

/* ---- IsDialogMessage ---------------------------------------------------- */
UINT u32_mnemonic(const char *s) {
  for (; s && *s; s++)
    if (*s == '&') {
      if (s[1] == '&') {
        s++;
        continue;
      }
      UINT c = (uint8_t)s[1];
      return c >= 'a' && c <= 'z' ? c - 32 : c;
    }
  return 0;
}
static BOOL mnemonic(HWND dlg, UINT ch) {
  if (ch >= 'a' && ch <= 'z')
    ch -= 32;
  HWND list[U32_WINDOWS];
  UINT n = flatten(dlg, list, 0, U32_WINDOWS, 0);
  HWND focus = GetFocus();
  int start = -1;
  for (UINT i = 0; i < n; i++)
    if (list[i] == focus)
      start = (int)i;
  for (UINT k = 1; k <= n; k++) {
    HWND c = list[(UINT)(start + (int)k + (int)n) % n];
    struct u32_window *w = u32_window(c);
    if (!w || !usable(c))
      continue;
    char text[256];
    GetWindowTextA(c, text, sizeof(text));
    UINT code = dlg_code(c, 0);
    if ((code & DLGC_STATIC) && (w->style & SS_NOPREFIX) && !(code & DLGC_BUTTON))
      continue;
    if (u32_mnemonic(text) != ch)
      continue;
    if ((code & DLGC_STATIC) && !(code & DLGC_BUTTON)) {
      /* The control after the label in tab order, skipping statics. */
      int at = -1;
      for (UINT j = 0; j < n; j++)
        if (list[j] == c)
          at = (int)j;
      for (UINT j = 1; j < n; j++) {
        HWND candidate = list[(UINT)(at + (int)j) % n];
        UINT cc = dlg_code(candidate, 0);
        if (usable(candidate) && !(cc & DLGC_STATIC)) {
          u32_dialog_focus(dlg, candidate);
          return 1;
        }
      }
      return 1;
    }
    if (code & DLGC_BUTTON) {
      UINT type = w->style & 15;
      if (type == BS_GROUPBOX)
        continue;
      u32_dialog_focus(dlg, c);
      SendMessageA(c, BM_CLICK, 0, 0);
      return 1;
    }
    u32_dialog_focus(dlg, c);
    return 1;
  }
  return 0;
}
static void press(HWND dlg, UINT id) {
  HWND ctl = GetDlgItem(dlg, (int)id);
  if (ctl && !IsWindowEnabled(ctl)) {
    MessageBeep(0);
    return;
  }
  SendMessageA(dlg, WM_COMMAND, (id & 0xffff) | (BN_CLICKED << 16),
               (LPARAM)ctl);
}
BOOL W32API IsDialogMessageA(HWND dlg, MSG *msg) {
  U32_GUARD;
  if (!msg || !IsWindow(dlg))
    return 0;
  if (msg->hwnd != dlg && !IsChild(dlg, msg->hwnd))
    return 0;
  UINT m = msg->message;
  if (m == WM_KEYDOWN || m == WM_CHAR || m == WM_SYSCHAR) {
    HWND target = msg->hwnd;
    UINT code = target != dlg ? dlg_code(target, msg) : 0;
    if (code & DLGC_WANTMESSAGE) {
      TranslateMessage(msg);
      DispatchMessageA(msg);
      return 1;
    }
    if (m == WM_KEYDOWN) {
      BOOL shift = GetKeyState(VK_SHIFT) < 0;
      switch (msg->wParam) {
      case VK_TAB:
        if (!(code & DLGC_WANTTAB)) {
          SendMessageA(dlg, WM_NEXTDLGCTL, shift, 0);
          return 1;
        }
        break;
      case VK_LEFT:
      case VK_UP:
      case VK_RIGHT:
      case VK_DOWN:
        if (!(code & DLGC_WANTARROWS)) {
          HWND next = GetNextDlgGroupItem(
              dlg, GetFocus(), msg->wParam == VK_LEFT || msg->wParam == VK_UP);
          /* An auto radio button checks itself when it gains the focus. */
          if (next && next != GetFocus())
            u32_dialog_focus(dlg, next);
          return 1;
        }
        break;
      case VK_RETURN: {
        HWND focus = GetFocus();
        UINT fc = focus && IsChild(dlg, focus) ? dlg_code(focus, msg) : 0;
        if (fc & DLGC_WANTALLKEYS)
          break;
        if (fc & DLGC_DEFPUSHBUTTON && IsWindowEnabled(focus)) {
          press(dlg, (UINT)GetDlgCtrlID(focus));
          return 1;
        }
        LRESULT def = SendMessageA(dlg, DM_GETDEFID, 0, 0);
        press(dlg, (def >> 16) == DC_HASDEFID ? (UINT)(def & 0xffff) : IDOK);
        return 1;
      }
      case VK_ESCAPE:
        if (!(code & DLGC_WANTALLKEYS)) {
          press(dlg, IDCANCEL);
          return 1;
        }
        break;
      default:
        break;
      }
    } else if (m == WM_CHAR) {
      if (!(code & (DLGC_WANTCHARS | DLGC_WANTALLKEYS)) &&
          msg->wParam > ' ' && mnemonic(dlg, (UINT)msg->wParam))
        return 1;
    } else if (m == WM_SYSCHAR) {
      if (mnemonic(dlg, (UINT)msg->wParam))
        return 1;
    }
  }
  TranslateMessage(msg);
  DispatchMessageA(msg);
  return 1;
}

/* ---- dialog item helpers ----------------------------------------------- */
HWND W32API GetDlgItem(HWND dlg, int id) {
  U32_GUARD;
  if (!IsWindow(dlg)) {
    u32_fail(1400);
    return 0;
  }
  HWND list[U32_WINDOWS];
  UINT n = children(dlg, list, U32_WINDOWS);
  for (UINT i = 0; i < n; i++)
    if ((int)u32_window(list[i])->id == id ||
        (uint16_t)u32_window(list[i])->id == (uint16_t)id)
      return list[i];
  u32_fail(1421);
  return 0;
}
int W32API GetDlgCtrlID(HWND h) {
  U32_GUARD;
  struct u32_window *w = u32_window(h);
  if (!w)
    return u32_fail(1400);
  return w->style & WS_CHILD ? (int)w->id : 0;
}
LRESULT W32API SendDlgItemMessageA(HWND dlg, int id, UINT m, WPARAM wp,
                                   LPARAM lp) {
  U32_GUARD;
  HWND c = GetDlgItem(dlg, id);
  return c ? SendMessageA(c, m, wp, lp) : 0;
}
UINT W32API GetDlgItemTextA(HWND dlg, int id, char *out, int n) {
  U32_GUARD;
  if (out && n > 0)
    out[0] = 0;
  HWND c = GetDlgItem(dlg, id);
  return c ? (UINT)SendMessageA(c, WM_GETTEXT, (WPARAM)n, (LPARAM)out) : 0;
}
BOOL W32API SetDlgItemTextA(HWND dlg, int id, const char *s) {
  U32_GUARD;
  HWND c = GetDlgItem(dlg, id);
  return c ? (BOOL)SendMessageA(c, WM_SETTEXT, 0, (LPARAM)(s ? s : "")) : 0;
}
UINT W32API GetDlgItemInt(HWND dlg, int id, BOOL *ok, BOOL is_signed) {
  U32_GUARD;
  char text[64];
  if (ok)
    *ok = 0;
  if (!GetDlgItem(dlg, id))
    return 0;
  GetDlgItemTextA(dlg, id, text, sizeof(text));
  const char *s = text;
  while (*s == ' ' || *s == '\t')
    s++;
  BOOL negative = 0;
  if (is_signed && *s == '-') {
    negative = 1;
    s++;
  }
  if (*s < '0' || *s > '9')
    return 0;
  uint64_t v = 0;
  for (; *s >= '0' && *s <= '9'; s++) {
    v = v * 10 + (uint64_t)(*s - '0');
    if (v > (is_signed ? (negative ? 0x80000000ull : 0x7fffffffull)
                       : 0xffffffffull))
      return 0;
  }
  while (*s == ' ' || *s == '\t')
    s++;
  if (*s)
    return 0;
  if (ok)
    *ok = 1;
  return negative ? (UINT)(-(int64_t)v) : (UINT)v;
}
BOOL W32API SetDlgItemInt(HWND dlg, int id, UINT value, BOOL is_signed) {
  U32_GUARD;
  char text[16];
  char *p = text + sizeof(text) - 1;
  *p = 0;
  BOOL negative = is_signed && (int)value < 0;
  uint32_t v = negative ? (uint32_t)(-(int64_t)(int)value) : value;
  do {
    *--p = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  if (negative)
    *--p = '-';
  return SetDlgItemTextA(dlg, id, p);
}
BOOL W32API CheckDlgButton(HWND dlg, int id, UINT check) {
  U32_GUARD;
  HWND c = GetDlgItem(dlg, id);
  if (!c)
    return 0;
  SendMessageA(c, BM_SETCHECK, check, 0);
  return 1;
}
UINT W32API IsDlgButtonChecked(HWND dlg, int id) {
  U32_GUARD;
  HWND c = GetDlgItem(dlg, id);
  return c ? (UINT)SendMessageA(c, BM_GETCHECK, 0, 0) : 0;
}
BOOL W32API CheckRadioButton(HWND dlg, int first, int last, int check) {
  U32_GUARD;
  if (!IsWindow(dlg))
    return u32_fail(1400);
  HWND list[U32_WINDOWS];
  UINT n = children(dlg, list, U32_WINDOWS);
  for (UINT i = 0; i < n; i++) {
    int id = (int)u32_window(list[i])->id;
    if (id >= first && id <= last)
      SendMessageA(list[i], BM_SETCHECK, id == check ? BST_CHECKED : 0, 0);
  }
  return 1;
}
