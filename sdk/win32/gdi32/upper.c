/* f5-20B: GDI entry points the reused Wine 10.0 DLLs import (decision table
 * config/wine-upper.json, rule core-api): regions, pens, poly-polylines,
 * alpha blending, device-dependent bitmap bits, layout, halftone palette,
 * colour spaces, printing documents and escapes, DEVMODE conversion,
 * character sets and font measurement, Unicode font enumeration.
 * Windows 98 semantics; references are the Microsoft pages of each
 * function, cited in build/codex/f5-20b-report.md. Integer arithmetic only
 * (the GDI builtin is built without the FPU).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#ifndef W32_HOST_MODEL
__declspec(dllimport) void *W32API LoadLibraryA(const char *);
__declspec(dllimport) void *W32API GetProcAddress(void *, const char *);
#else
extern void *W32API LoadLibraryA(const char *);
extern void *W32API GetProcAddress(void *, const char *);
#endif
/* Process-heap memory a caller releases with HeapFree(GetProcessHeap()),
 * through KERNEL32 resolved once by name. */
static void *process_heap_alloc(DWORD n) {
  static void *(W32API *heap)(void);
  static void *(W32API *alloc)(void *, DWORD, DWORD);
  if (!heap || !alloc) {
    void *k = LoadLibraryA("kernel32.dll");
    heap = k ? (void *(W32API *)(void))GetProcAddress(k, "GetProcessHeap") : 0;
    alloc = k ? (void *(W32API *)(void *, DWORD, DWORD))GetProcAddress(k, "HeapAlloc")
              : 0;
  }
  return heap && alloc ? alloc(heap(), 8 /* HEAP_ZERO_MEMORY */, n) : 0;
}

/* ---- regions, pens, lines ------------------------------------------------- */
/* Rounded rectangle: the rectangle minus, in each corner, the part outside
 * the quarter ellipse of size w x h (the RoundRect outline). */
HRGN W32API CreateRoundRectRgn(int l, int t, int r, int b, int w, int h) {
  if (l > r) {
    int v = l;
    l = r;
    r = v;
  }
  if (t > b) {
    int v = t;
    t = b;
    b = v;
  }
  if (w < 0)
    w = -w;
  if (h < 0)
    h = -h;
  if (w > r - l)
    w = r - l;
  if (h > b - t)
    h = b - t;
  if (w < 2 || h < 2)
    return CreateRectRgn(l, t, r, b);
  HRGN all = CreateRectRgn(l, t, r, b);
  if (!all)
    return 0;
  /* Each corner: subtract the rows of the ellipse's bounding square that
   * lie outside it, using the elliptic region of the full w x h ellipse. */
  HRGN e = CreateEllipticRgn(0, 0, w, h);
  HRGN corners = CreateRectRgn(0, 0, 0, 0), piece = CreateRectRgn(0, 0, 0, 0);
  if (!e || !corners || !piece)
    goto fail;
  int hw = w / 2, hh = h / 2;
  const RECT quarters[4] = {{0, 0, hw, hh}, {w - hw, 0, w, hh},
                            {0, h - hh, hw, h}, {w - hw, h - hh, w, h}};
  const POINT at[4] = {{l, t}, {r - w, t}, {l, b - h}, {r - w, b - h}};
  for (int i = 0; i < 4; i++) {
    /* piece = quarter square minus ellipse, moved to its corner */
    HRGN square = CreateRectRgn(quarters[i].left, quarters[i].top,
                                quarters[i].right, quarters[i].bottom);
    if (!square)
      goto fail;
    CombineRgn(piece, square, e, 4 /* RGN_DIFF */);
    DeleteObject(square);
    OffsetRgn(piece, at[i].x, at[i].y);
    CombineRgn(corners, corners, piece, 2 /* RGN_OR */);
  }
  CombineRgn(all, all, corners, 4);
  DeleteObject(e);
  DeleteObject(corners);
  DeleteObject(piece);
  return all;
fail:
  if (e)
    DeleteObject(e);
  if (corners)
    DeleteObject(corners);
  if (piece)
    DeleteObject(piece);
  DeleteObject(all);
  return 0;
}
/* ExtCreatePen: cosmetic and geometric pens with a solid-colour brush. The
 * line style is kept (PS_ALTERNATE and PS_USERSTYLE draw as PS_DOT);
 * end caps and joins are not drawn by the scalar engine (limited). */
HPEN W32API ExtCreatePen(DWORD style, DWORD width, const LOGBRUSH *brush,
                         DWORD count, const DWORD *styles) {
  DWORD type = style & 0xf0000u, line = style & 0xfu;
  if (!brush || (type != 0 && type != 0x10000u) || line > 8 ||
      (line == 7 && (!count || count > 16 || !styles)) ||
      (line != 7 && (count || styles)) ||
      (type == 0 && width != 1) /* cosmetic pens are one pixel wide */) {
    SetLastError(87);
    return 0;
  }
  if (brush->lbStyle != BS_SOLID && brush->lbStyle != 2 /* BS_HATCHED */ &&
      brush->lbStyle != 1 /* BS_NULL */) {
    SetLastError(87);
    return 0;
  }
  int pen = brush->lbStyle == 1 ? 5 /* PS_NULL */
            : line == 7 || line == 8 ? 2 /* PS_DOT */
                                      : (int)line;
  return CreatePen(pen, type ? (int)width : 1, brush->lbColor);
}
BOOL W32API PolyPolyline(HDC h, const POINT *p, const DWORD *counts,
                         DWORD n) {
  GDI_GUARD;
  if (!gdi_dc(h) || !p || !counts || !n) {
    SetLastError(87);
    return 0;
  }
  for (DWORD i = 0; i < n; i++)
    if (counts[i] < 2) {
      SetLastError(87);
      return 0;
    }
  for (DWORD i = 0; i < n; i++) {
    if (!Polyline(h, p, (int)counts[i]))
      return 0;
    p += counts[i];
  }
  return 1;
}

/* ---- alpha blending ------------------------------------------------------- */
/* BLENDFUNCTION: BlendOp AC_SRC_OVER (0), BlendFlags 0, SourceConstantAlpha,
 * AlphaFormat 0 or AC_SRC_ALPHA (1, premultiplied 32-bpp source). Stretching
 * samples the nearest source pixel. */
BOOL W32API GdiAlphaBlend(HDC dst, int x, int y, int w, int h, HDC src, int sx,
                          int sy, int sw, int sh, DWORD blend) {
  GDI_GUARD;
  struct dc *d = gdi_dc(dst), *s = gdi_dc(src);
  unsigned op = blend & 0xff, flags = (blend >> 8) & 0xff,
           constant = (blend >> 16) & 0xff, format = (blend >> 24) & 0xff;
  if (!d || !s || op || flags || format > 1 || w < 0 || h < 0 || sw < 0 ||
      sh < 0) {
    SetLastError(87);
    return 0;
  }
  if (!w || !h || !sw || !sh)
    return 1;
  struct w32_dib *target = gdi_target(d), *source = gdi_target(s);
  if (!target || !source)
    return 0;
  if (format && source->bpp != 32) {
    SetLastError(87);
    return 0;
  }
  POINT dp[2] = {{x, y}, {x + w, y + h}}, sp[2] = {{sx, sy}, {sx + sw, sy + sh}};
  if (!gdi_transform(d, dp, 2, 0) || !gdi_transform(s, sp, 2, 0))
    return 0;
  int dw = dp[1].x - dp[0].x, dh = dp[1].y - dp[0].y,
      swd = sp[1].x - sp[0].x, shd = sp[1].y - sp[0].y;
  if (dw <= 0 || dh <= 0 || swd <= 0 || shd <= 0)
    return 1;
  if (sp[0].x < 0 || sp[0].y < 0 || sp[1].x > (int)source->width ||
      sp[1].y > (int)source->height) {
    SetLastError(87);
    return 0;
  }
  RECT touched = {dp[0].x, dp[0].y, dp[1].x, dp[1].y};
  for (int j = 0; j < dh; j++)
    for (int i = 0; i < dw; i++) {
      int px = dp[0].x + i, py = dp[0].y + j;
      if (px < 0 || py < 0 || px >= (int)target->width ||
          py >= (int)target->height || !gdi_visible(d, px, py))
        continue;
      int qx = sp[0].x + (int)((int64_t)i * swd / dw),
          qy = sp[0].y + (int)((int64_t)j * shd / dh);
      DWORD c = w32_dib_rgb(source, qx, qy), under = w32_dib_rgb(target, px, py);
      unsigned a = format ? (w32_dib_raw(source, qx, qy) >> 24) & 0xff : 255;
      DWORD out = 0;
      for (int shift = 0; shift < 24; shift += 8) {
        unsigned sc = (c >> shift) & 0xff, dc = (under >> shift) & 0xff;
        unsigned v;
        if (format) {
          /* premultiplied: dst = src*K + dst*(1 - srcA*K) */
          unsigned scaled = sc * constant / 255, alpha = a * constant / 255;
          v = scaled + dc * (255 - alpha) / 255;
        } else
          v = (sc * constant + dc * (255 - constant)) / 255;
        out |= (DWORD)(v > 255 ? 255 : v) << shift;
      }
      w32_dib_put(target, px, py, gdi_encode_color(d, gdi_colorref(out)));
    }
  gdi_damage(d, touched);
  return 1;
}

/* ---- bitmaps, layout, palettes, colour spaces ----------------------------- */
/* Device-dependent bits: top-down rows, each a multiple of two bytes. */
LONG W32API GetBitmapBits(HBITMAP h, LONG cap, void *out) {
  GDI_GUARD;
  struct object *o = gdi_object(h, GDI_BITMAP);
  if (!o || cap < 0) {
    SetLastError(87);
    return 0;
  }
  const struct w32_dib *b = &o->u.bitmap.dib;
  DWORD row = ((b->width * b->bpp + 15) / 16) * 2, size = row * b->height;
  if (!out)
    return (LONG)size;
  DWORD rows = (DWORD)cap / row, copied = 0;
  if (rows > b->height)
    rows = b->height;
  for (DWORD y = 0; y < rows; y++) {
    DWORD from = b->top_down ? y : b->height - 1 - y;
    w32_copy((uint8_t *)out + y * row, b->bits + from * b->pitch,
             row < b->pitch ? row : b->pitch);
    copied += row;
  }
  return (LONG)copied;
}
/* Windows 98 has no right-to-left DC layout. */
DWORD W32API GetLayout(HDC h) {
  GDI_GUARD;
  return gdi_dc(h) ? 0 : GDI_ERROR;
}
/* The halftone palette: the 20 static colours, the 6x6x6 colour cube and
 * 20 more grey levels. */
HPALETTE W32API CreateHalftonePalette(HDC h) {
  GDI_GUARD;
  (void)h;
  static const uint8_t statics[20][3] = {
      {0, 0, 0},       {128, 0, 0},     {0, 128, 0},     {128, 128, 0},
      {0, 0, 128},     {128, 0, 128},   {0, 128, 128},   {192, 192, 192},
      {192, 220, 192}, {166, 202, 240}, {255, 251, 240}, {160, 160, 164},
      {128, 128, 128}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
      {0, 0, 255},     {255, 0, 255},   {0, 255, 255},   {255, 255, 255}};
  struct {
    uint16_t version, count;
    PALETTEENTRY entries[256];
  } p = {0x300, 256, {{0, 0, 0, 0}}};
  unsigned n = 0;
  for (unsigned i = 0; i < 10; i++)
    p.entries[n++] = (PALETTEENTRY){statics[i][0], statics[i][1], statics[i][2], 0};
  for (unsigned r = 0; r < 6; r++)
    for (unsigned g = 0; g < 6; g++)
      for (unsigned b = 0; b < 6; b++)
        p.entries[n++] = (PALETTEENTRY){(uint8_t)(r * 51), (uint8_t)(g * 51),
                                        (uint8_t)(b * 51), 0};
  for (unsigned i = 0; i < 20; i++) {
    uint8_t v = (uint8_t)(8 + i * 12);
    p.entries[n++] = (PALETTEENTRY){v, v, v, 0};
  }
  for (unsigned i = 10; i < 20; i++)
    p.entries[n++] = (PALETTEENTRY){statics[i][0], statics[i][1], statics[i][2], 0};
  return CreatePalette((const LOGPALETTE *)&p);
}
/* ICM colour spaces: the LOGCOLORSPACE record is validated and kept; no
 * colour matching is performed (limited: no ICM engine). */
static struct {
  DWORD used;
  uint8_t record[600];
} spaces[16];
void *W32API CreateColorSpaceW(const void *logical) {
  GDI_GUARD;
  const uint8_t *p = logical;
  /* LOGCOLORSPACEW: signature 'PSOC', version 0x400, size 588. */
  if (!p || w32_u32(p) != 0x50534f43u || w32_u32(p + 4) != 0x400 ||
      w32_u32(p + 8) != 588) {
    SetLastError(87);
    return 0;
  }
  for (unsigned i = 0; i < 16; i++)
    if (!spaces[i].used) {
      spaces[i].used = 1;
      w32_copy(spaces[i].record, p, 588);
      return (void *)(uintptr_t)(0x00c0de00u | (i + 1));
    }
  SetLastError(8);
  return 0;
}
BOOL W32API DeleteColorSpace(void *h) {
  GDI_GUARD;
  uintptr_t v = (uintptr_t)h;
  unsigned i = (unsigned)(v & 0xff) - 1;
  if ((v & ~(uintptr_t)0xff) != 0x00c0de00u || i >= 16 || !spaces[i].used)
    return 0;
  spaces[i].used = 0;
  return 1;
}

/* ---- printing ----------------------------------------------------------- */
/* No printer driver or printer DC exists on CiukiOS; StartDoc on any DC is
 * the non-printer case and fails with SP_ERROR (limited). */
int W32API StartDocW(HDC h, const void *info) {
  GDI_GUARD;
  if (!gdi_dc(h) || !info) {
    SetLastError(6);
    return -1;
  }
  SetLastError(87);
  return -1;
}
int W32API StartDocA(HDC h, const void *info) { return StartDocW(h, info); }
/* Escapes: QUERYESCSUPPORT reports the supported ones; MFCOMMENT on a
 * metafile DC is recorded (metafile.c); every other escape is not
 * implemented by the display driver (0). */
int gdi_metafile_escape(HDC, int, int, const void *);
int W32API ExtEscape(HDC h, int escape, int in_bytes, const char *in,
                     int out_bytes, char *out) {
  GDI_GUARD;
  (void)out_bytes, (void)out;
  struct dc *d = gdi_dc(h);
  if (!d)
    return -1;
  if (escape == 8 /* QUERYESCSUPPORT */) {
    if (in_bytes < 4 || !in)
      return 0;
    DWORD which = w32_u32(in);
    return which == 8 || (which == 15 && d->record) ? 1 : 0;
  }
  if (escape == 15 /* MFCOMMENT */)
    return d->record ? gdi_metafile_escape(h, escape, in_bytes, in) : 0;
  return 0;
}
/* DEVMODEA (156 bytes) to DEVMODEW (220): the device and form names widen
 * (32 characters each); the driver-private bytes follow dmSize. The result
 * is allocated from the process heap and freed by the caller (HeapFree). */
void *W32API GdiConvertToDevmodeW(const uint8_t *a) {
  if (!a)
    return 0;
  DWORD size = w32_u16(a + 36), extra = w32_u16(a + 38);
  if (size < 68 || size > 156)
    return 0;
  DWORD wide = size + 32 + (size >= 102 ? 32 : 0);
  uint8_t *w = process_heap_alloc(wide + extra);
  if (!w)
    return 0;
  for (unsigned i = 0; i < 32; i++)
    w32_put16(w + i * 2, (uint16_t)gdi_cp1252_unicode(a[i]));
  w32_copy(w + 64, a + 32, (size < 70 ? size : 70) - 32);
  if (size >= 102) {
    for (unsigned i = 0; i < 32; i++)
      w32_put16(w + 102 + i * 2, (uint16_t)gdi_cp1252_unicode(a[70 + i]));
    w32_copy(w + 166, a + 102, size - 102);
  }
  w32_put16(w + 100, (uint16_t)wide); /* dmSize */
  w32_copy(w + wide, a + size, extra);
  return w;
}

/* ---- character sets ------------------------------------------------------- */
static const struct {
  uint16_t charset, page;
  uint32_t csb;
} charsets[] = {{0, 1252, 1u << 0},    {238, 1250, 1u << 1},  {204, 1251, 1u << 2},
                {161, 1253, 1u << 3},  {162, 1254, 1u << 4},  {177, 1255, 1u << 5},
                {178, 1256, 1u << 6},  {186, 1257, 1u << 7},  {163, 1258, 1u << 8},
                {222, 874, 1u << 16},  {128, 932, 1u << 17},  {134, 936, 1u << 18},
                {129, 949, 1u << 19},  {136, 950, 1u << 20},  {130, 1361, 1u << 21},
                {2, 42, 1u << 31}};
typedef struct {
  unsigned ciCharset, ciACP;
  FONTSIGNATURE fs;
} CHARSETINFO;
/* TCI_SRCCHARSET (1): the value of src is a charset; TCI_SRCCODEPAGE (2): a
 * code page; TCI_SRCFONTSIG (3): src points to fsCsb[0]. */
BOOL W32API TranslateCharsetInfo(DWORD *src, CHARSETINFO *out, DWORD flags) {
  if (!out)
    return 0;
  for (unsigned i = 0; i < sizeof(charsets) / sizeof(*charsets); i++) {
    BOOL match = flags == 1   ? (uintptr_t)src == charsets[i].charset
                 : flags == 2 ? (uintptr_t)src == charsets[i].page
                 : flags == 3 ? src && (*src & charsets[i].csb) &&
                                    !(*src & (charsets[i].csb - 1))
                              : 0;
    if (match) {
      w32_fill(out, 0, sizeof(*out));
      out->ciCharset = charsets[i].charset;
      out->ciACP = charsets[i].page;
      out->fs.fsCsb[0] = charsets[i].csb;
      return 1;
    }
  }
  return 0;
}
/* The ANSI code page of the selected font's character set. */
unsigned W32API GdiGetCodePage(HDC h) {
  unsigned charset = (unsigned)GetTextCharset(h);
  for (unsigned i = 0; i < sizeof(charsets) / sizeof(*charsets); i++)
    if (charsets[i].charset == charset)
      return charsets[i].page;
  return charset == OEM_CHARSET ? 437u : 1252u;
}
/* The dialog base unit of the selected font: (extent of the 52 letters /
 * 26 + 1) / 2, as GetDialogBaseUnits computes it, and the cell height. */
LONG W32API GdiGetCharDimensions(HDC h, TEXTMETRICW *tm, LONG *height) {
  static const WCHAR letters[52] = {
      'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
      'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
      'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
      'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z'};
  TEXTMETRICW m;
  SIZE s;
  if (!GetTextMetricsW(h, &m) || !GetTextExtentPoint32W(h, letters, 52, &s))
    return 0;
  if (tm)
    *tm = m;
  if (height)
    *height = m.tmHeight;
  return (s.cx / 26 + 1) / 2;
}
/* CHARWIDTHINFO: the smallest left and right side bearings of the font's
 * code-page characters (negative overhangs), as EDIT's font margins use. */
typedef struct {
  int lsb, rsb, unk;
} CHARWIDTHINFO;
BOOL W32API GetCharWidthInfo(HDC h, CHARWIDTHINFO *out) {
  ABC abc[224];
  if (!out || !GetCharABCWidthsW(h, 32, 255, abc))
    return 0;
  int lsb = 0, rsb = 0;
  for (unsigned i = 0; i < 224; i++) {
    if (abc[i].abcA < lsb)
      lsb = abc[i].abcA;
    if (abc[i].abcC < rsb)
      rsb = abc[i].abcC;
  }
  *out = (CHARWIDTHINFO){lsb, rsb, 0};
  return 1;
}
/* GCP_RESULTSW for the left-to-right subset: logical order, glyphs are the
 * characters (no ligatures), advances from the font, caret positions,
 * GCP_MAXEXTENT fitting. Returns the extent (width | height << 16). */
typedef struct {
  DWORD lStructSize;
  WCHAR *lpOutString;
  unsigned *lpOrder;
  int *lpDx, *lpCaretPos;
  char *lpClass;
  WCHAR *lpGlyphs;
  unsigned nGlyphs;
  int nMaxFit;
} GCP_RESULTSW;
DWORD W32API GetCharacterPlacementW(HDC h, const WCHAR *s, int count,
                                    int maximum, GCP_RESULTSW *r, DWORD flags) {
  if (!s || count < 0)
    return 0;
  TEXTMETRICW tm;
  if (!GetTextMetricsW(h, &tm))
    return 0;
  if (r && r->lStructSize < sizeof(*r) - sizeof(int))
    return 0;
  unsigned limit = r ? (unsigned)count < r->nGlyphs || !r->nGlyphs
                           ? (unsigned)count
                           : r->nGlyphs
                     : (unsigned)count;
  int x = 0, fit = 0;
  for (unsigned i = 0; i < limit; i++) {
    int width = 0;
    if (!GetCharWidth32W(h, s[i], s[i], &width))
      return 0;
    if ((flags & 0x00100000u /* GCP_MAXEXTENT */) && x + width > maximum)
      break;
    if (r) {
      if (r->lpOutString)
        r->lpOutString[i] = s[i];
      if (r->lpOrder)
        r->lpOrder[i] = i;
      if (r->lpDx)
        r->lpDx[i] = width;
      if (r->lpCaretPos)
        r->lpCaretPos[i] = x;
      if (r->lpClass)
        r->lpClass[i] = 0;
      if (r->lpGlyphs)
        r->lpGlyphs[i] = s[i];
    }
    x += width;
    fit++;
  }
  if (r) {
    r->nGlyphs = (unsigned)fit;
    r->nMaxFit = fit;
  }
  return ((DWORD)x & 0xffffu) | ((DWORD)tm.tmHeight << 16);
}

/* ---- Unicode font enumeration ------------------------------------------- */
typedef struct {
  TEXTMETRICW tm;
  DWORD ntmFlags;
  unsigned ntmSizeEM, ntmCellHeight, ntmAvgWidth;
  FONTSIGNATURE ntmFontSig;
} NEWTEXTMETRICEXW;
typedef struct {
  LOGFONTW elfLogFont;
  WCHAR elfFullName[64], elfStyle[32], elfScript[32];
} ENUMLOGFONTEXW;
typedef int(W32API *FONTENUMPROCW)(const LOGFONTW *, const TEXTMETRICW *,
                                   DWORD, intptr_t);
struct enum_w {
  FONTENUMPROCW proc;
  intptr_t lparam;
};
static void widen(WCHAR *w, const uint8_t *a, unsigned n) {
  unsigned i = 0;
  for (; i + 1 < n && a[i]; i++)
    w[i] = (WCHAR)gdi_cp1252_unicode(a[i]);
  w[i] = 0;
}
static int W32API enum_thunk(const LOGFONTA *lf, const TEXTMETRICA *tm,
                             DWORD type, intptr_t context) {
  const struct enum_w *e = (const struct enum_w *)context;
  const ENUMLOGFONTEXA *a = (const ENUMLOGFONTEXA *)lf;
  const NEWTEXTMETRICEXA *n = (const NEWTEXTMETRICEXA *)tm;
  ENUMLOGFONTEXW elf;
  NEWTEXTMETRICEXW ntm;
  w32_fill(&elf, 0, sizeof(elf));
  w32_fill(&ntm, 0, sizeof(ntm));
  w32_copy(&elf.elfLogFont, &a->elfLogFont, 28);
  widen(elf.elfLogFont.lfFaceName, (const uint8_t *)a->elfLogFont.lfFaceName,
        LF_FACESIZE);
  widen(elf.elfFullName, a->elfFullName, 64);
  widen(elf.elfStyle, a->elfStyle, 32);
  widen(elf.elfScript, a->elfScript, 32);
  w32_copy(&ntm.tm, &n->ntmTm, 44);
  ntm.tm.tmFirstChar = (WCHAR)n->ntmTm.tmFirstChar;
  ntm.tm.tmLastChar = n->ntmTm.tmCharSet == ANSI_CHARSET
                          ? 0xfffc
                          : (WCHAR)n->ntmTm.tmLastChar;
  ntm.tm.tmDefaultChar = (WCHAR)n->ntmTm.tmDefaultChar;
  ntm.tm.tmBreakChar = (WCHAR)n->ntmTm.tmBreakChar;
  ntm.tm.tmItalic = n->ntmTm.tmItalic;
  ntm.tm.tmUnderlined = n->ntmTm.tmUnderlined;
  ntm.tm.tmStruckOut = n->ntmTm.tmStruckOut;
  ntm.tm.tmPitchAndFamily = n->ntmTm.tmPitchAndFamily;
  ntm.tm.tmCharSet = n->ntmTm.tmCharSet;
  ntm.ntmFlags = n->ntmTm.ntmFlags;
  ntm.ntmSizeEM = n->ntmTm.ntmSizeEM;
  ntm.ntmCellHeight = n->ntmTm.ntmCellHeight;
  ntm.ntmAvgWidth = n->ntmTm.ntmAvgWidth;
  ntm.ntmFontSig = n->ntmFontSig;
  return e->proc(&elf.elfLogFont, &ntm.tm, type, e->lparam);
}
int W32API EnumFontFamiliesExW(HDC h, LOGFONTW *lf, FONTENUMPROCW proc,
                               intptr_t lparam, DWORD flags) {
  if (!lf || !proc)
    return 0;
  LOGFONTA a;
  w32_copy(&a, lf, 28);
  unsigned i = 0;
  for (; i + 1 < LF_FACESIZE && lf->lfFaceName[i]; i++) {
    int c = gdi_cp1252_ansi(lf->lfFaceName[i]);
    a.lfFaceName[i] = (char)(c < 0 ? '?' : c);
  }
  a.lfFaceName[i] = 0;
  struct enum_w e = {proc, lparam};
  return EnumFontFamiliesExA(h, &a, enum_thunk, (intptr_t)&e, flags);
}
int W32API EnumFontFamiliesW(HDC h, const WCHAR *family, FONTENUMPROCW proc,
                             intptr_t lparam) {
  LOGFONTW lf;
  w32_fill(&lf, 0, sizeof(lf));
  lf.lfCharSet = DEFAULT_CHARSET;
  for (unsigned i = 0; family && i + 1 < LF_FACESIZE && family[i]; i++)
    lf.lfFaceName[i] = family[i];
  return EnumFontFamiliesExW(h, &lf, proc, lparam, 0);
}
