/* f5-20B: the Uniscribe (USP10) entry points that Wine 10.0 moved into
 * GDI32 and that RICHED20 and COMCTL32 call (rule core-uniscribe): a
 * left-to-right, single-script subset. ScriptItemize returns one item;
 * ScriptShape maps each UTF-16 code unit to itself as its glyph (one
 * cluster per character, SCRIPT_ANALYSIS.fNoGlyphIndex set, so the
 * "glyphs" are characters, which ScriptTextOut draws with ExtTextOutW);
 * ScriptPlace takes the advances from the selected font. The
 * ScriptString* functions build on the same pieces (tabs, clipping,
 * password masking, selection highlight). Bidirectional reordering is
 * supported by ScriptLayout only (embedding levels in, visual order out);
 * complex shaping (Arabic, Indic, ligatures, combining marks) is not
 * performed (limited). Microsoft "Uniscribe Functions" reference.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#ifndef W32_HOST_MODEL
__declspec(dllimport) void *W32API LoadLibraryA(const char *);
__declspec(dllimport) void *W32API GetProcAddress(void *, const char *);
#else
extern void *W32API LoadLibraryA(const char *);
extern void *W32API GetProcAddress(void *, const char *);
#endif
#define S_OK 0
#define E_INVALIDARG ((int32_t)0x80070057)
#define E_OUTOFMEMORY ((int32_t)0x8007000e)
#define E_PENDING ((int32_t)0x8000000a)
#define USP_E_SCRIPT_NOT_IN_FONT ((int32_t)0x80040200)
typedef int32_t HRESULT;
typedef struct {
  uint16_t eScript : 10, fRTL : 1, fLayoutRTL : 1, fLinkBefore : 1,
      fLinkAfter : 1, fLogicalOrder : 1, fNoGlyphIndex : 1;
  uint16_t s; /* SCRIPT_STATE */
} SCRIPT_ANALYSIS;
typedef struct {
  int iCharPos;
  SCRIPT_ANALYSIS a;
} SCRIPT_ITEM;
typedef struct {
  uint16_t uJustification : 4, fClusterStart : 1, fDiacritic : 1,
      fZeroWidth : 1, fReserved : 1, fShapeReserved : 8;
} SCRIPT_VISATTR;
typedef struct {
  LONG du, dv;
} GOFFSET;
typedef struct {
  uint8_t fSoftBreak : 1, fWhiteSpace : 1, fCharStop : 1, fWordStop : 1,
      fInvalid : 1, fReserved : 3;
} SCRIPT_LOGATTR;
typedef struct {
  int cTabStops, iScale;
  int *pTabStops;
  int iTabOrigin;
} SCRIPT_TABDEF;
/* The script cache remembers the font it measured, so later calls without
 * a DC can select it into a memory DC. */
struct cache {
  HFONT font;
  LONG height, ascent;
};
static HDC with_font(HDC hdc, void **psc, HDC *owned) {
  *owned = 0;
  struct cache *c = psc ? *psc : 0;
  if (hdc) {
    if (psc && !c) {
      c = gdi_alloc(sizeof(*c));
      if (!c)
        return 0;
      *psc = c;
    }
    if (c) {
      TEXTMETRICA tm;
      c->font = (HFONT)GetCurrentObject(hdc, 6 /* OBJ_FONT */);
      if (GetTextMetricsA(hdc, &tm)) {
        c->height = tm.tmHeight;
        c->ascent = tm.tmAscent;
      }
    }
    return hdc;
  }
  if (!c || !gdi_object(c->font, GDI_FONT))
    return 0;
  HDC dc = CreateCompatibleDC(0);
  if (dc)
    SelectObject(dc, c->font);
  *owned = dc;
  return dc;
}
static void done(HDC owned) {
  if (owned)
    DeleteDC(owned);
}
HRESULT W32API ScriptFreeCache(void **psc) {
  if (!psc)
    return E_INVALIDARG;
  gdi_free(*psc, sizeof(struct cache));
  *psc = 0;
  return S_OK;
}
HRESULT W32API ScriptItemize(const WCHAR *s, int n, int max, const void *control,
                             const void *state, SCRIPT_ITEM *items, int *count) {
  (void)control;
  if (!s || n <= 0 || !items || max < 2)
    return E_INVALIDARG;
  w32_fill(items, 0, 2 * sizeof(*items));
  items[0].iCharPos = 0;
  if (state)
    items[0].a.s = *(const uint16_t *)state;
  items[1].iCharPos = n;
  if (count)
    *count = 1;
  return S_OK;
}
HRESULT W32API ScriptShape(HDC hdc, void **psc, const WCHAR *s, int n, int max,
                           SCRIPT_ANALYSIS *sa, uint16_t *glyphs,
                           uint16_t *clusters, SCRIPT_VISATTR *va, int *count) {
  if (!psc || !s || n <= 0 || !sa || !glyphs || !clusters || !va || !count)
    return E_INVALIDARG;
  if (max < n)
    return E_OUTOFMEMORY;
  HDC owned;
  HDC dc = with_font(hdc, psc, &owned);
  if (!dc)
    return E_PENDING;
  done(owned);
  sa->fNoGlyphIndex = 1;
  for (int i = 0; i < n; i++) {
    glyphs[i] = s[i];
    clusters[i] = (uint16_t)i;
    w32_fill(&va[i], 0, sizeof(va[i]));
    va[i].fClusterStart = 1;
    va[i].fZeroWidth = s[i] < 0x20 || s[i] == 0x200b || s[i] == 0xfeff;
    va[i].uJustification = s[i] == ' ' ? 2 /* SCRIPT_JUSTIFY_BLANK */
                                       : 1 /* SCRIPT_JUSTIFY_CHARACTER */;
  }
  *count = n;
  return S_OK;
}
HRESULT W32API ScriptPlace(HDC hdc, void **psc, const uint16_t *glyphs, int n,
                           const SCRIPT_VISATTR *va, SCRIPT_ANALYSIS *sa,
                           int *advance, GOFFSET *offsets, ABC *total) {
  (void)sa;
  if (!psc || !glyphs || n <= 0 || !advance)
    return E_INVALIDARG;
  HDC owned;
  HDC dc = with_font(hdc, psc, &owned);
  if (!dc)
    return E_PENDING;
  ABC sum = {0, 0, 0};
  for (int i = 0; i < n; i++) {
    ABC abc = {0, 0, 0};
    if (va && va[i].fZeroWidth)
      abc = (ABC){0, 0, 0};
    else if (!GetCharABCWidthsW(dc, glyphs[i], glyphs[i], &abc)) {
      done(owned);
      return E_PENDING;
    }
    advance[i] = abc.abcA + (int)abc.abcB + abc.abcC;
    if (offsets)
      offsets[i] = (GOFFSET){0, 0};
    if (!i)
      sum.abcA = abc.abcA;
    sum.abcB += (unsigned)advance[i];
    if (i == n - 1)
      sum.abcC = abc.abcC;
  }
  done(owned);
  if (total) {
    sum.abcB -= (unsigned)(sum.abcA + sum.abcC);
    *total = sum;
  }
  return S_OK;
}
HRESULT W32API ScriptTextOut(HDC hdc, void **psc, int x, int y, unsigned options,
                             const RECT *rect, const SCRIPT_ANALYSIS *sa,
                             const WCHAR *reserved, int reserved2,
                             const uint16_t *glyphs, int n, const int *advance,
                             const int *justify, const GOFFSET *offsets) {
  (void)reserved, (void)reserved2, (void)offsets;
  if (!hdc || !psc || !sa || !glyphs || n <= 0 || !advance)
    return E_INVALIDARG;
  return ExtTextOutW(hdc, x, y, options & (ETO_OPAQUE | ETO_CLIPPED), rect,
                     (const WCHAR *)glyphs, (unsigned)n,
                     justify ? justify : advance)
             ? S_OK
             : E_INVALIDARG;
}
/* Left-to-right: positions are sums of advances. */
HRESULT W32API ScriptCPtoX(int cp, BOOL trailing, int chars, int n,
                           const uint16_t *clusters, const SCRIPT_VISATTR *va,
                           const int *advance, const SCRIPT_ANALYSIS *sa,
                           int *x) {
  (void)clusters, (void)va, (void)sa;
  if (!advance || !x || n < 0)
    return E_INVALIDARG;
  int pos = 0;
  if (cp < 0) {
    *x = 0;
    return S_OK;
  }
  if (cp >= chars) {
    for (int i = 0; i < n; i++)
      pos += advance[i];
    *x = pos;
    return S_OK;
  }
  for (int i = 0; i < cp && i < n; i++)
    pos += advance[i];
  if (trailing && cp < n)
    pos += advance[cp];
  *x = pos;
  return S_OK;
}
HRESULT W32API ScriptXtoCP(int x, int chars, int n, const uint16_t *clusters,
                           const SCRIPT_VISATTR *va, const int *advance,
                           const SCRIPT_ANALYSIS *sa, int *cp, int *trailing) {
  (void)clusters, (void)va, (void)sa;
  if (!advance || !cp || !trailing || n < 0)
    return E_INVALIDARG;
  if (x < 0) {
    *cp = -1;
    *trailing = 1;
    return S_OK;
  }
  int pos = 0;
  for (int i = 0; i < n && i < chars; i++) {
    if (x < pos + advance[i]) {
      *cp = i;
      *trailing = (x - pos) * 2 >= advance[i];
      return S_OK;
    }
    pos += advance[i];
  }
  *cp = chars;
  *trailing = 0;
  return S_OK;
}
static BOOL white(WCHAR c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xa0 ||
         (c >= 0x2000 && c <= 0x200a) || c == 0x3000;
}
static BOOL wordish(WCHAR c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') || (c >= 0xc0 && c != 0xd7 && c != 0xf7);
}
/* Character stops everywhere; a soft break after white space; word stops
 * at the start of each word. */
HRESULT W32API ScriptBreak(const WCHAR *s, int n, const SCRIPT_ANALYSIS *sa,
                           SCRIPT_LOGATTR *out) {
  (void)sa;
  if (!s || n <= 0 || !out)
    return E_INVALIDARG;
  for (int i = 0; i < n; i++) {
    w32_fill(&out[i], 0, sizeof(out[i]));
    out[i].fCharStop = 1;
    out[i].fWhiteSpace = white(s[i]);
    out[i].fSoftBreak = i > 0 && white(s[i - 1]) && !white(s[i]);
    out[i].fWordStop = wordish(s[i]) && (i == 0 || !wordish(s[i - 1]));
  }
  return S_OK;
}
/* Unicode bidirectional algorithm rule L2: from the highest level down to
 * the lowest odd level, reverse every maximal sequence at that level or
 * higher. */
HRESULT W32API ScriptLayout(int runs, const uint8_t *levels, int *v2l,
                            int *l2v) {
  if (runs <= 0 || !levels || (!v2l && !l2v))
    return E_INVALIDARG;
  int *order = gdi_alloc((DWORD)runs * sizeof(int));
  if (!order)
    return E_OUTOFMEMORY;
  unsigned high = 0, low = 255;
  for (int i = 0; i < runs; i++) {
    order[i] = i;
    if (levels[i] > high)
      high = levels[i];
    if ((levels[i] & 1) && levels[i] < low)
      low = levels[i];
  }
  for (unsigned level = high; level >= low && level; level--)
    for (int i = 0; i < runs;) {
      if (levels[order[i]] < level) {
        i++;
        continue;
      }
      int j = i;
      while (j < runs && levels[order[j]] >= level)
        j++;
      for (int a = i, b = j - 1; a < b; a++, b--) {
        int t = order[a];
        order[a] = order[b];
        order[b] = t;
      }
      i = j;
    }
  for (int i = 0; i < runs; i++) {
    if (v2l)
      v2l[i] = order[i];
    if (l2v)
      l2v[order[i]] = i;
  }
  gdi_free(order, (DWORD)runs * sizeof(int));
  return S_OK;
}

/* ---- ScriptString ------------------------------------------------------- */
#define SSA_PASSWORD 0x1u
#define SSA_TAB 0x2u
#define SSA_CLIP 0x4u
#define SSA_BREAK 0x40u
#define SSA_GLYPHS 0x80u
struct ssa {
  HDC dc;
  DWORD flags;
  int chars, out_chars;
  WCHAR *text;
  int *advance;
  SCRIPT_LOGATTR *attrs;
  SIZE size;
};
static void ssa_free(struct ssa *a) {
  if (!a)
    return;
  gdi_free(a->text, (DWORD)a->chars * sizeof(WCHAR));
  gdi_free(a->advance, (DWORD)a->chars * sizeof(int));
  gdi_free(a->attrs, (DWORD)a->chars * sizeof(SCRIPT_LOGATTR));
  gdi_free(a, sizeof(*a));
}
static HRESULT analyse(HDC, HDC, const void *, int, int, DWORD, int,
                       const int *, const SCRIPT_TABDEF *, void **);
HRESULT W32API ScriptStringAnalyse(HDC hdc, const void *string, int n,
                                   int glyphs, int charset, DWORD flags,
                                   int width, const void *control,
                                   const void *state, const int *dx,
                                   const SCRIPT_TABDEF *tabs,
                                   const uint8_t *classes, void **out) {
  (void)glyphs, (void)control, (void)state, (void)classes;
  if (!string || n <= 0 || !out || ((flags & SSA_GLYPHS) && !hdc))
    return E_INVALIDARG;
  /* Without a DC (SSA_BREAK analysis) the default font measures. */
  HDC owned = hdc ? 0 : CreateCompatibleDC(0);
  if (!hdc && !owned)
    return E_OUTOFMEMORY;
  HRESULT r = analyse(hdc ? hdc : owned, hdc, string, n, charset, flags, width,
                      dx, tabs, out);
  if (owned)
    DeleteDC(owned);
  return r;
}
static HRESULT analyse(HDC measure, HDC hdc, const void *string, int n,
                       int charset, DWORD flags, int width, const int *dx,
                       const SCRIPT_TABDEF *tabs, void **out) {
  *out = 0;
  struct ssa *a = gdi_alloc(sizeof(*a));
  if (!a)
    return E_OUTOFMEMORY;
  a->dc = hdc; /* NULL when analysed without a DC: no ScriptStringOut */
  a->flags = flags;
  a->chars = n;
  a->text = gdi_alloc((DWORD)n * sizeof(WCHAR));
  a->advance = gdi_alloc((DWORD)n * sizeof(int));
  a->attrs = gdi_alloc((DWORD)n * sizeof(SCRIPT_LOGATTR));
  if (!a->text || !a->advance || !a->attrs) {
    ssa_free(a);
    return E_OUTOFMEMORY;
  }
  for (int i = 0; i < n; i++)
    a->text[i] = charset == -1 ? ((const WCHAR *)string)[i]
                               : (WCHAR)gdi_cp1252_unicode(
                                     ((const uint8_t *)string)[i]);
  if (flags & SSA_BREAK)
    ScriptBreak(a->text, n, 0, a->attrs);
  TEXTMETRICW tm;
  if (!GetTextMetricsW(measure, &tm)) {
    ssa_free(a);
    return E_INVALIDARG;
  }
  int x = 0, fit = n;
  for (int i = 0; i < n; i++) {
    WCHAR c = flags & SSA_PASSWORD ? '*' : a->text[i];
    int advance = 0;
    if (dx)
      advance = dx[i];
    else if (c == '\t' && (flags & SSA_TAB)) {
      /* Tab stops: the table (scaled by iScale/4 dialog units) or eight
       * average characters. */
      int origin = tabs ? tabs->iTabOrigin : 0, step = tm.tmAveCharWidth * 8;
      int next = 0;
      if (tabs && tabs->cTabStops > 0 && tabs->pTabStops) {
        int scale = tabs->iScale ? tabs->iScale : 4;
        next = -1;
        for (int k = 0; k < tabs->cTabStops; k++) {
          int stop = tabs->pTabStops[k] * scale / 4;
          if (stop > x - origin) {
            next = origin + stop;
            break;
          }
        }
        if (next < 0) {
          int last = tabs->pTabStops[tabs->cTabStops - 1] * scale / 4;
          step = tabs->cTabStops == 1 && last > 0 ? last : step;
        }
      }
      if (next <= 0)
        next = origin + ((x - origin) / (step ? step : 1) + 1) * step;
      advance = next - x;
    } else if (!GetCharWidth32W(measure, c, c, &advance))
      advance = tm.tmAveCharWidth;
    if ((flags & SSA_CLIP) && width > 0 && x + advance > width) {
      fit = i;
      break;
    }
    a->advance[i] = advance;
    x += advance;
  }
  a->out_chars = fit;
  a->size = (SIZE){x, tm.tmHeight};
  *out = a;
  return S_OK;
}
HRESULT W32API ScriptStringFree(void **p) {
  if (!p || !*p)
    return E_INVALIDARG;
  ssa_free(*p);
  *p = 0;
  return S_OK;
}
const SIZE *W32API ScriptString_pSize(void *p) {
  return p ? &((struct ssa *)p)->size : 0;
}
const int *W32API ScriptString_pcOutChars(void *p) {
  return p ? &((struct ssa *)p)->out_chars : 0;
}
HRESULT W32API ScriptStringGetLogicalWidths(void *p, int *out) {
  struct ssa *a = p;
  if (!a || !out)
    return E_INVALIDARG;
  for (int i = 0; i < a->chars; i++)
    out[i] = i < a->out_chars ? a->advance[i] : 0;
  return S_OK;
}
HRESULT W32API ScriptStringCPtoX(void *p, int cp, BOOL trailing, int *x) {
  struct ssa *a = p;
  if (!a || !x || cp < 0 || cp >= a->chars)
    return E_INVALIDARG;
  return ScriptCPtoX(cp, trailing, a->out_chars, a->out_chars, 0, 0,
                     a->advance, 0, x);
}
HRESULT W32API ScriptStringXtoCP(void *p, int x, int *cp, int *trailing) {
  struct ssa *a = p;
  if (!a || !cp || !trailing)
    return E_INVALIDARG;
  return ScriptXtoCP(x, a->out_chars, a->out_chars, 0, 0, a->advance, 0, cp,
                     trailing);
}
/* Selection: the selected characters are drawn on the highlight colours
 * (COLOR_HIGHLIGHT, COLOR_HIGHLIGHTTEXT from USER32 when loaded; the
 * Windows 98 defaults otherwise). */
static COLORREF system_color(int index, COLORREF fallback) {
  static DWORD(W32API * get)(int);
  static int looked;
  if (!looked) {
    looked = 1;
    void *user = LoadLibraryA("user32.dll");
    get = user ? (DWORD(W32API *)(int))GetProcAddress(user, "GetSysColor") : 0;
  }
  return get ? get(index) : fallback;
}
HRESULT W32API ScriptStringOut(void *p, int x, int y, unsigned options,
                               const RECT *rect, int min_sel, int max_sel,
                               BOOL disabled) {
  struct ssa *a = p;
  (void)disabled;
  if (!a || a->out_chars < 0 || !a->dc)
    return E_INVALIDARG;
  int n = a->out_chars;
  WCHAR *text = gdi_alloc((DWORD)(n + 1) * sizeof(WCHAR));
  if (!text)
    return E_OUTOFMEMORY;
  for (int i = 0; i < n; i++)
    text[i] = a->flags & SSA_PASSWORD ? '*'
              : a->text[i] == '\t' && (a->flags & SSA_TAB) ? ' '
                                                           : a->text[i];
  unsigned flags = options & (ETO_OPAQUE | ETO_CLIPPED);
  BOOL ok = ExtTextOutW(a->dc, x, y, flags, rect, text, (unsigned)n, a->advance);
  if (ok && min_sel < max_sel && min_sel < n && max_sel > 0) {
    int from = min_sel < 0 ? 0 : min_sel, to = max_sel > n ? n : max_sel, sx = x;
    for (int i = 0; i < from; i++)
      sx += a->advance[i];
    int sw = 0;
    for (int i = from; i < to; i++)
      sw += a->advance[i];
    RECT box = {sx, y, sx + sw, y + a->size.cy};
    if (rect && box.top < rect->top)
      box.top = rect->top;
    COLORREF old_text = SetTextColor(a->dc, system_color(14, RGB(255, 255, 255)));
    COLORREF old_back = SetBkColor(a->dc, system_color(13, RGB(0, 0, 128)));
    ok = ExtTextOutW(a->dc, sx, y, ETO_OPAQUE | (flags & ETO_CLIPPED), &box,
                     text + from, (unsigned)(to - from), a->advance + from);
    SetTextColor(a->dc, old_text);
    SetBkColor(a->dc, old_back);
  }
  gdi_free(text, (DWORD)(n + 1) * sizeof(WCHAR));
  return ok ? S_OK : E_INVALIDARG;
}
