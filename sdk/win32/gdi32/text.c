/* Coverage composition and Western Win32 text geometry. Font policy is f5-10C.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
static const uint16_t cp1252[32] = {
    0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021,
    0x2c6,  0x2030, 0x160,  0x2039, 0x152,  0x8d,   0x17d,  0x8f,
    0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
static DWORD codepoint(const void *text, unsigned *at, unsigned length,
                       int wide) {
  if (!wide) {
    unsigned c = ((const uint8_t *)text)[(*at)++];
    return c >= 128 && c < 160 ? cp1252[c - 128] : c;
  }
  const WCHAR *s = text;
  unsigned c = s[(*at)++];
  if (c >= 0xd800 && c <= 0xdbff && *at < length && s[*at] >= 0xdc00 &&
      s[*at] <= 0xdfff)
    return 0x10000 + ((c - 0xd800) << 10) + (s[(*at)++] - 0xdc00);
  return c >= 0xd800 && c <= 0xdfff ? 0xfffd : c;
}
static int valid_provider(const struct w32_gdi_font_provider *p, unsigned em) {
  return p && p->metrics && p->raster && p->free_glyph && em >= 64 &&
         em <= 8192;
}
HFONT W32API w32_gdi_create_font(const struct w32_gdi_font_provider *p,
                                 void *face, unsigned em) {
  GDI_GUARD;
  if (!valid_provider(p, em))
    return 0;
  HFONT h;
  struct object *o = gdi_new(GDI_FONT, &h);
  if (!o)
    return 0;
  o->u.font = (struct font){*p, face, em};
  if (p->retain)
    p->retain(face);
  return h;
}
BOOL W32API w32_gdi_stock_font(const struct w32_gdi_font_provider *p,
                               void *face, unsigned em) {
  GDI_GUARD;
  if (!valid_provider(p, em))
    return 0;
  struct object *o = gdi_object(GetStockObject(SYSTEM_FONT), GDI_FONT);
  if (!o)
    return 0;
  if (p->retain)
    p->retain(face);
  if (o->u.font.provider.release)
    o->u.font.provider.release(o->u.font.face);
  o->u.font = (struct font){*p, face, em};
  return 1;
}
static struct font *font_of(struct dc *d) {
  struct object *o = d ? gdi_object(d->s.font, GDI_FONT) : 0;
  return o && valid_provider(&o->u.font.provider, o->u.font.em) ? &o->u.font
                                                                : 0;
}
static int ceil26(int n) { return n >= 0 ? (n + 63) / 64 : n / 64; }
static int round26(int n) { return n >= 0 ? (n + 32) / 64 : -((-n + 32) / 64); }
static int glyph(struct font *f, DWORD cp, int index, struct w32_gdi_glyph *g) {
  *g = (struct w32_gdi_glyph){0};
  if (!f->provider.raster(f->face, f->em, cp, index, g))
    return 0;
  if (g->width > 8192 || g->height > 8192 || g->pitch < g->width ||
      g->left < -8192 || g->left > 8192 || g->top < -8192 || g->top > 8192 ||
      (uint64_t)g->pitch * g->height > 65536 ||
      (g->width && g->height && !g->coverage) || g->advance < -GDI_LIMIT * 64 ||
      g->advance > GDI_LIMIT * 64) {
    f->provider.free_glyph(f->face, g);
    return 0;
  }
  return 1;
}
static int text_metrics(struct font *f, struct w32_gdi_font_metrics *m) {
  return f && f->provider.metrics(f->face, f->em, m) && m->ascent >= 0 &&
         m->descent >= 0 && (int64_t)m->ascent + m->descent <= GDI_LIMIT * 64 &&
         m->line_gap >= 0 && m->line_gap <= GDI_LIMIT * 64;
}
static int measure(struct dc *d, const void *text, unsigned count, int wide,
                   const int *dx, int indices, int *width) {
  struct font *f = font_of(d);
  if (!f || (!text && count) || count > 8192)
    return 0;
  unsigned at = 0;
  int64_t advance = 0;
  DWORD previous = 0;
  while (at < count) {
    unsigned before = at;
    DWORD cp = indices ? ((const WCHAR *)text)[at++]
                       : codepoint(text, &at, count, wide);
    struct w32_gdi_glyph g;
    if (!glyph(f, cp, indices, &g))
      return 0;
    int kern = 0;
    if (!dx && previous && !indices && f->provider.kerning &&
        !f->provider.kerning(f->face, f->em, previous, cp, &kern)) {
      f->provider.free_glyph(f->face, &g);
      return 0;
    }
    if (dx) {
      for (unsigned i = before; i < at; i++)
        advance += (int64_t)dx[i] * 64;
    } else
      advance += (int64_t)g.advance + (int64_t)d->s.extra * 64 + kern;
    f->provider.free_glyph(f->face, &g);
    previous = cp;
    if (advance < -GDI_LIMIT * 64 || advance > GDI_LIMIT * 64)
      return 0;
  }
  *width = (int)advance;
  return 1;
}
static int in_rect(const RECT *r, int x, int y) {
  return !r || (x >= r->left && x < r->right && y >= r->top && y < r->bottom);
}
static void text_fill(struct dc *d, RECT r, COLORREF color_ref,
                      const RECT *clip) {
  struct w32_dib *s = gdi_target(d);
  if (r.left < 0)
    r.left = 0;
  if (r.top < 0)
    r.top = 0;
  if (r.right > (LONG)s->width)
    r.right = (LONG)s->width;
  if (r.bottom > (LONG)s->height)
    r.bottom = (LONG)s->height;
  DWORD color = gdi_encode_color(d, color_ref);
  for (int y = r.top; y < r.bottom; y++)
    for (int x = r.left; x < r.right; x++)
      if (in_rect(clip, x, y) && gdi_visible(d, x, y))
        w32_dib_put(s, x, y, color);
  gdi_damage(d, r);
}
static void compose(struct dc *d, const struct w32_gdi_glyph *g, int x,
                    int baseline, const RECT *clip) {
  if (!g->width || !g->height)
    return;
  struct w32_dib *s = gdi_target(d);
  RECT r = gdi_rect(d, (RECT){x + g->left, baseline - g->top,
                              x + g->left + (int)g->width,
                              baseline - g->top + (int)g->height});
  int w = r.right - r.left, h = r.bottom - r.top;
  if (w <= 0 || h <= 0)
    return;
  RECT bounds = r;
  if (bounds.left < 0)
    bounds.left = 0;
  if (bounds.top < 0)
    bounds.top = 0;
  if (bounds.right > (LONG)s->width)
    bounds.right = (LONG)s->width;
  if (bounds.bottom > (LONG)s->height)
    bounds.bottom = (LONG)s->height;
  DWORD text = gdi_resolve_color(d, d->s.text);
  int smoothing=gdi_smoothing();
  for (int yy = bounds.top; yy < bounds.bottom; yy++)
    for (int xx = bounds.left; xx < bounds.right; xx++) {
      if (!in_rect(clip, xx, yy) || !gdi_visible(d, xx, yy))
        continue;
      unsigned gx = (unsigned)((int64_t)(xx - r.left) * g->width / w),
               gy = (unsigned)((int64_t)(yy - r.top) * g->height / h);
      if (d->s.viewport_ext.cx * d->s.window_ext.cx < 0)
        gx = g->width - 1 - gx;
      if (d->s.viewport_ext.cy * d->s.window_ext.cy < 0)
        gy = g->height - 1 - gy;
      unsigned a = g->coverage[gy * g->pitch + gx];
      if(!smoothing) a=a>=104 ? 255 : 0;
      if (!a)
        continue;
      if (a == 255) {
        w32_dib_put(s, xx, yy, gdi_encode_color(d, d->s.text));
        continue;
      }
      DWORD rgb = w32_dib_rgb(s, xx, yy), result = 0;
      for (unsigned k = 0; k < 3; k++)
        result |= ((((text >> (k * 8)) & 255) * a +
                    ((rgb >> (k * 8)) & 255) * (255 - a) + 127) /
                   255)
                  << (k * 8);
      w32_dib_put(s, xx, yy, w32_dib_encode(s, result));
    }
  gdi_damage(d, r);
}
int gdi_text(HDC h, int x, int y, unsigned flags, const RECT *rect,
             const void *text, unsigned count, const int *dx, int wide) {
  struct dc *d = gdi_dc(h);
  if (d && d->record && (text || !count)) /* f5-20B metafile DC */
    return gdi_record_text(d, x, y, flags, rect, text, count, dx, wide);
  struct font *f = font_of(d);
  struct w32_gdi_font_metrics m;
  if (!d || (!text && count) || count > 8192 ||
      ((flags & (ETO_CLIPPED | ETO_OPAQUE)) && !rect) ||
      (flags & ~(ETO_CLIPPED | ETO_OPAQUE | ETO_GLYPH_INDEX)))
    return 0;
  if (d->printer) {
    int printed=gdi_printer_text(d,x,y,flags,rect,text,count,dx,wide);
    if(printed>=0)return printed;
  }
  if ((flags & ETO_GLYPH_INDEX) && !wide)
    return 1;
  RECT area = {0}, *clip = 0;
  if (rect)
    area = gdi_rect(d, *rect);
  if (flags & ETO_CLIPPED)
    clip = &area;
  if (!count) {
    if (flags & ETO_OPAQUE)
      text_fill(d, area, d->s.bk, clip);
    return 1;
  }
  if (!text_metrics(f, &m))
    return 0;
  int advance;
  if (!measure(d, text, count, wide, dx, (flags & ETO_GLYPH_INDEX) != 0,
               &advance))
    return 0;
  if (d->s.align & TA_UPDATECP) {
    x = d->s.current.x;
    y = d->s.current.y;
  }
  if (x < -GDI_LIMIT || x > GDI_LIMIT || y < -GDI_LIMIT || y > GDI_LIMIT)
    return 0;
  int width = round26(advance), baseline = y;
  if ((d->s.align & 24) == 0)
    baseline += ceil26(m.ascent);
  else if ((d->s.align & 24) == TA_BOTTOM)
    baseline -= ceil26(m.descent);
  if ((d->s.align & 6) == TA_CENTER)
    x -= width / 2;
  else if ((d->s.align & 6) == TA_RIGHT)
    x -= width;
  if ((int64_t)x + width < -GDI_LIMIT || (int64_t)x + width > GDI_LIMIT ||
      baseline < -GDI_LIMIT || baseline > GDI_LIMIT)
    return 0;
  if (flags & ETO_OPAQUE)
    text_fill(d, area, d->s.bk, clip);
  if (d->s.bk_mode == OPAQUE && !(flags & ETO_OPAQUE))
    text_fill(d,
              gdi_rect(d, (RECT){x, baseline - ceil26(m.ascent), x + width,
                                 baseline + ceil26(m.descent)}),
              d->s.bk, clip);
  unsigned at = 0;
  int64_t pen = 0;
  DWORD previous = 0;
  while (at < count) {
    unsigned before = at;
    DWORD cp = flags & ETO_GLYPH_INDEX ? ((const WCHAR *)text)[at++]
                                       : codepoint(text, &at, count, wide);
    struct w32_gdi_glyph g;
    if (!glyph(f, cp, (flags & ETO_GLYPH_INDEX) != 0, &g))
      return 0;
    int kern = 0;
    if (!dx && previous && !(flags & ETO_GLYPH_INDEX) && f->provider.kerning &&
        !f->provider.kerning(f->face, f->em, previous, cp, &kern)) {
      f->provider.free_glyph(f->face, &g);
      return 0;
    }
    pen += kern;
    compose(d, &g, x + round26((int)pen), baseline, clip);
    if (dx)
      for (unsigned i = before; i < at; i++)
        pen += (int64_t)dx[i] * 64;
    else
      pen += (int64_t)g.advance + (int64_t)d->s.extra * 64;
    f->provider.free_glyph(f->face, &g);
    previous = cp;
  }
  if (d->s.align & TA_UPDATECP)
    d->s.current = (POINT){x + width, y};
  return 1;
}
BOOL W32API ExtTextOutA(HDC h, int x, int y, unsigned flags, const RECT *r,
                        const char *text, unsigned n, const int *dx) {
  GDI_GUARD;
  return gdi_text(h, x, y, flags, r, text, n, dx, 0);
}
BOOL W32API ExtTextOutW(HDC h, int x, int y, unsigned flags, const RECT *r,
                        const WCHAR *text, unsigned n, const int *dx) {
  GDI_GUARD;
  return gdi_text(h, x, y, flags, r, text, n, dx, 1);
}
BOOL W32API TextOutA(HDC h, int x, int y, const char *text, int n) {
  return n >= 0 ? ExtTextOutA(h, x, y, 0, 0, text, (unsigned)n, 0) : 0;
}
BOOL W32API TextOutW(HDC h, int x, int y, const WCHAR *text, int n) {
  return n >= 0 ? ExtTextOutW(h, x, y, 0, 0, text, (unsigned)n, 0) : 0;
}
static int extent(HDC h, const void *text, int count, SIZE *size, int wide) {
  struct dc *d = gdi_dc(h);
  struct w32_gdi_font_metrics m;
  int width;
  if (!size || count < 0 || !text_metrics(font_of(d), &m) ||
      !measure(d, text, (unsigned)count, wide, 0, 0, &width))
    return 0;
  *size = (SIZE){round26(width), ceil26(m.ascent + m.descent)};
  return 1;
}
BOOL W32API GetTextExtentPoint32A(HDC h, const char *text, int n, SIZE *s) {
  GDI_GUARD;
  return extent(h, text, n, s, 0);
}
BOOL W32API GetTextExtentPoint32W(HDC h, const WCHAR *text, int n, SIZE *s) {
  GDI_GUARD;
  return extent(h, text, n, s, 1);
}
static BOOL extent_ex(HDC h, const void *text, int n, int maximum, int *fit,
                      int *dx, SIZE *size, int wide) {
  struct dc *d = gdi_dc(h);
  struct w32_gdi_font_metrics m;
  int total;
  if (n < 0 || n > 8192 || maximum < 0 || !size ||
      !text_metrics(font_of(d), &m) ||
      !measure(d, text, (unsigned)n, wide, 0, 0, &total))
    return 0;
  struct font *f = font_of(d);
  unsigned at = 0;
  DWORD previous = 0;
  int64_t advance = 0;
  int fitting = 0;
  while (at < (unsigned)n) {
    unsigned first = at;
    DWORD cp = codepoint(text, &at, (unsigned)n, wide);
    struct w32_gdi_glyph g;
    if (!glyph(f, cp, 0, &g))
      return 0;
    int kern = 0;
    if (previous && f->provider.kerning &&
        !f->provider.kerning(f->face, f->em, previous, cp, &kern)) {
      f->provider.free_glyph(f->face, &g);
      return 0;
    }
    advance += (int64_t)g.advance + kern + (int64_t)d->s.extra * 64;
    f->provider.free_glyph(f->face, &g);
    previous = cp;
    int width = round26((int)advance);
    for (unsigned i = first; i < at; i++)
      if (dx)
        dx[i] = width;
    if (width <= maximum)
      fitting = (int)at;
  }
  if (fit)
    *fit = fitting;
  *size = (SIZE){round26(total), ceil26(m.ascent + m.descent)};
  return 1;
}
BOOL W32API GetTextExtentExPointA(HDC h, const char *s, int n, int maximum,
                                  int *fit, int *dx, SIZE *size) {
  GDI_GUARD;
  return extent_ex(h, s, n, maximum, fit, dx, size, 0);
}
BOOL W32API GetTextExtentExPointW(HDC h, const WCHAR *s, int n, int maximum,
                                  int *fit, int *dx, SIZE *size) {
  GDI_GUARD;
  return extent_ex(h, s, n, maximum, fit, dx, size, 1);
}
unsigned W32API SetTextAlign(HDC h, unsigned align) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || (align & ~31u) || (align & 6) == 4 || (align & 24) == 16)
    return CLR_INVALID;
  if (d->record) {
    int16_t v[1] = {(int16_t)align};
    gdi_record(d, 0x012eu, v, 1);
  }
  unsigned old = d->s.align;
  d->s.align = align;
  return old;
}
unsigned W32API GetTextAlign(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  return d ? d->s.align : CLR_INVALID;
}
int W32API SetTextCharacterExtra(HDC h, int extra) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || extra < -GDI_LIMIT || extra > GDI_LIMIT)
    return (-2147483647 - 1);
  if (d->record) {
    int16_t v[1] = {(int16_t)extra};
    gdi_record(d, 0x0108u, v, 1);
  }
  int old = d->s.extra;
  d->s.extra = extra;
  return old;
}
int W32API GetTextCharacterExtra(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  return d ? d->s.extra : (-2147483647 - 1);
}
struct layout_char {
  WCHAR value;
  uint16_t underline;
  int advance;
};
struct layout_line {
  unsigned start, end;
  int width;
};
static int char_advance(struct dc *d, WCHAR c, int *width) {
  struct font *f = font_of(d);
  struct w32_gdi_glyph g;
  if (!glyph(f, c, 0, &g))
    return 0;
  *width = round26(g.advance) + d->s.extra;
  f->provider.free_glyph(f->face, &g);
  return 1;
}
static int line_width(struct layout_char *c, unsigned first, unsigned end,
                      int tab, int expand) {
  int64_t width = 0;
  for (unsigned i = first; i < end; i++) {
    int step =
        c[i].value == '\t' && expand ? tab - (int)(width % tab) : c[i].advance;
    width += step;
    if (width > GDI_LIMIT || width < -GDI_LIMIT)
      return GDI_LIMIT;
  }
  return (int)width;
}
static unsigned ellipsis(struct layout_char *c, unsigned first, unsigned end,
                         int available, int dot, unsigned flags) {
  if (!(flags & (DT_END_ELLIPSIS | DT_WORD_ELLIPSIS | DT_PATH_ELLIPSIS)))
    return end;
  int width = line_width(c, first, end, 1, 0);
  if (width <= available)
    return end;
  unsigned tail = end;
  if (flags & DT_PATH_ELLIPSIS) {
    while (tail > first && c[tail - 1].value != '\\' &&
           c[tail - 1].value != '/')
      tail--;
    if (tail == first)
      tail = end;
    else
      tail--;
  }
  int tail_width = line_width(c, tail, end, 1, 0);
  unsigned keep = first;
  int used = 0;
  while (keep < tail &&
         used + c[keep].advance + 3 * dot + tail_width <= available)
    used += c[keep++].advance;
  if ((flags & DT_WORD_ELLIPSIS) && keep > first && keep < tail) {
    while (keep > first && c[keep - 1].value != ' ')
      keep--;
  }
  while (keep > first && c[keep - 1].value == ' ')
    keep--;
  if (tail < end && keep + 3 <= tail) {
    for (unsigned i = 0; i < end - tail; i++)
      c[keep + 3 + i] = c[tail + i];
    unsigned n = end - tail;
    for (unsigned i = 0; i < 3; i++)
      c[keep + i] = (struct layout_char){'.', 0, dot};
    return keep + 3 + n;
  }
  for (unsigned i = 0; i < 3; i++)
    c[keep + i] = (struct layout_char){'.', 0, dot};
  return keep + 3;
}
int W32API w32_gdi_draw_text(HDC h, const void *text, int length, RECT *rect,
                             unsigned flags, int wide, DRAWTEXTPARAMS *params) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct font *f = font_of(d);
  struct w32_gdi_font_metrics m;
  if (!d || !rect || !text || !text_metrics(f, &m) ||
      (params && params->cbSize != sizeof(*params)) || (flags & DT_RTLREADING))
    return 0;
  if (length == -1) {
    length = 0;
    while (length < 8192 && (wide ? ((const WCHAR *)text)[length]
                                  : ((const uint8_t *)text)[length]))
      length++;
  }
  if (length < 0 || length > 8192)
    return 0;
  unsigned capacity = (unsigned)length + 4;
  struct layout_char *chars = gdi_alloc(capacity * sizeof(*chars));
  struct layout_line *lines = gdi_alloc(capacity * sizeof(*lines));
  WCHAR *run = gdi_alloc(capacity * sizeof(WCHAR));
  int *dx = gdi_alloc(capacity * sizeof(int));
  int result = 0;
  if (!chars || !lines || !run || !dx)
    goto done;
  unsigned n = 0, at = 0;
  int prefix = 0, space, dot;
  if (!char_advance(d, ' ', &space) || !char_advance(d, '.', &dot))
    goto done;
  while (at < (unsigned)length) {
    DWORD cp = codepoint(text, &at, (unsigned)length, wide);
    if (cp > 65535)
      cp = 0xfffd;
    if (!(flags & DT_NOPREFIX) && cp == '&') {
      if (at < (unsigned)length && (wide ? ((const WCHAR *)text)[at]
                                         : ((const uint8_t *)text)[at]) == '&')
        at++;
      else {
        prefix = 1;
        continue;
      }
    }
    if (cp == '\r') {
      if (at < (unsigned)length && (wide ? ((const WCHAR *)text)[at]
                                         : ((const uint8_t *)text)[at]) == '\n')
        at++;
      cp = '\n';
    }
    if ((flags & DT_SINGLELINE) && cp == '\n')
      cp = ' ';
    int advance = 0;
    if (cp != '\n' && cp != '\t' && !char_advance(d, (WCHAR)cp, &advance))
      goto done;
    if (cp == '\t')
      advance = space;
    chars[n++] = (struct layout_char){(WCHAR)cp, (uint16_t)prefix, advance};
    prefix = 0;
  }
  int margin_l = params ? params->iLeftMargin : 0,
      margin_r = params ? params->iRightMargin : 0;
  int64_t avail64 = (int64_t)rect->right - rect->left - margin_l - margin_r;
  if (avail64 < 0 || avail64 > GDI_LIMIT || margin_l < 0 || margin_r < 0)
    goto done;
  int available = (int)avail64;
  int tab_chars = params && params->iTabLength > 0 ? params->iTabLength
                  : (flags & DT_TABSTOP)           ? (int)((flags >> 8) & 255)
                                                   : 8;
  if (tab_chars <= 0)
    tab_chars = 8;
  if (tab_chars > 256)
    tab_chars = 256;
  int tab = space > 0 ? space * tab_chars : tab_chars;
  unsigned line_count = 0, first = 0;
  do {
    unsigned end = first, last_space = first;
    int width = 0;
    while (end < n && chars[end].value != '\n') {
      int step = chars[end].value == '\t' && (flags & DT_EXPANDTABS)
                     ? tab - width % tab
                     : chars[end].advance;
      if ((flags & DT_WORDBREAK) && !(flags & DT_SINGLELINE) &&
          width + step > available && end > first && last_space > first) {
        end = last_space;
        break;
      }
      width += step;
      if (width > GDI_LIMIT || width < -GDI_LIMIT)
        goto done;
      if (chars[end].value == ' ' || chars[end].value == '\t')
        last_space = end + 1;
      end++;
    }
    unsigned next = end;
    if (next < n && chars[next].value == '\n')
      next++;
    else if (end < n)
      while (next < n &&
             (chars[next].value == ' ' || chars[next].value == '\t'))
        next++;
    unsigned trimmed = end;
    while (trimmed > first && chars[trimmed - 1].value == ' ' &&
           !(flags & DT_SINGLELINE))
      trimmed--;
    lines[line_count++] = (struct layout_line){
        first, trimmed,
        line_width(chars, first, trimmed, tab, (flags & DT_EXPANDTABS) != 0)};
    first = next;
  } while (first < n);
  int line_height = ceil26(m.ascent + m.descent +
                           ((flags & DT_EXTERNALLEADING) ? m.line_gap : 0));
  if (line_height <= 0)
    goto done;
  int64_t height64 = (int64_t)line_height * line_count;
  if (height64 > GDI_LIMIT)
    goto done;
  int height = (int)height64;
  if (flags & DT_CALCRECT) {
    int max = 0;
    for (unsigned i = 0; i < line_count; i++)
      if (lines[i].width > max)
        max = lines[i].width;
    if ((int64_t)rect->left + max + margin_l + margin_r > GDI_LIMIT ||
        (int64_t)rect->top + height > GDI_LIMIT)
      goto done;
    rect->right = rect->left + max + margin_l + margin_r;
    rect->bottom = rect->top + height;
    if (params)
      params->uiLengthDrawn = (DWORD)length;
    result = height;
    goto done;
  }
  int y = rect->top;
  if (flags & DT_SINGLELINE) {
    if (flags & DT_VCENTER)
      y += (rect->bottom - rect->top - height) / 2;
    else if (flags & DT_BOTTOM)
      y = rect->bottom - height;
  }
  unsigned old_align = d->s.align;
  d->s.align = 0;
  for (unsigned k = 0; k < line_count; k++) {
    struct layout_line line = lines[k];
    if (flags & (DT_END_ELLIPSIS | DT_WORD_ELLIPSIS | DT_PATH_ELLIPSIS)) {
      /* Work on a separate run: adding dots must not overwrite the
       * following line's characters. */
      struct layout_char *temp =
          gdi_alloc((line.end - line.start + 4) * sizeof(*temp));
      if (!temp) {
        d->s.align = old_align;
        goto done;
      }
      unsigned num = line.end - line.start;
      w32_copy(temp, chars + line.start, num * sizeof(*temp));
      num = ellipsis(temp, 0, num, available, dot, flags);
      int width = 0;
      for (unsigned i = 0; i < num; i++) {
        run[i] = temp[i].value;
        dx[i] = temp[i].advance;
        width += dx[i];
      }
      gdi_free(temp, (line.end - line.start + 4) * sizeof(*temp));
      line.width = width;
      line.start = 0;
      line.end = num;
    } else {
      int width = 0;
      for (unsigned i = line.start; i < line.end; i++) {
        unsigned index = i - line.start;
        run[index] = chars[i].value;
        dx[index] = run[index] == '\t' && (flags & DT_EXPANDTABS)
                        ? tab - width % tab
                        : chars[i].advance;
        if (run[index] == '\t')
          run[index] = ' ';
        width += dx[index];
      }
    }
    int x = rect->left + margin_l;
    if (flags & DT_CENTER)
      x += (available - line.width) / 2;
    else if (flags & DT_RIGHT)
      x += available - line.width;
    unsigned num = line.end - line.start;
    if (!(flags & DT_PREFIXONLY) &&
        !gdi_text(h, x, y, flags & DT_NOCLIP ? 0 : ETO_CLIPPED, rect, run, num,
                  dx, 1)) {
      d->s.align = old_align;
      goto done;
    }
    if (!(flags & (DT_NOPREFIX | DT_HIDEPREFIX)) &&
        !(flags & (DT_END_ELLIPSIS | DT_WORD_ELLIPSIS | DT_PATH_ELLIPSIS))) {
      int pen = x;
      RECT local = gdi_rect(d, *rect);
      for (unsigned i = lines[k].start; i < lines[k].end; i++) {
        unsigned index = i - lines[k].start;
        if (chars[i].underline)
          text_fill(
              d,
              gdi_rect(d, (RECT){pen, y + ceil26(m.ascent) + 1, pen + dx[index],
                                 y + ceil26(m.ascent) + 2}),
              d->s.text, flags & DT_NOCLIP ? 0 : &local);
        pen += dx[index];
      }
    }
    if ((flags & DT_MODIFYSTRING) && line_count == 1 &&
        (flags & (DT_END_ELLIPSIS | DT_WORD_ELLIPSIS | DT_PATH_ELLIPSIS))) {
      for (unsigned i = 0; i < num; i++) {
        if (wide)
          ((WCHAR *)text)[i] = run[i];
        else
          ((char *)text)[i] = (char)(run[i] < 256 ? run[i] : '?');
      }
      if (wide)
        ((WCHAR *)text)[num] = 0;
      else
        ((char *)text)[num] = 0;
    }
    y += line_height;
  }
  d->s.align = old_align;
  if (params)
    params->uiLengthDrawn = (DWORD)length;
  result = height;
done:
  gdi_free(dx, capacity * sizeof(int));
  gdi_free(run, capacity * sizeof(WCHAR));
  gdi_free(lines, capacity * sizeof(*lines));
  gdi_free(chars, capacity * sizeof(*chars));
  return result;
}
