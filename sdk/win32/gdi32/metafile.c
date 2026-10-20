/* f5-20B: Windows metafiles (WMF) and enhanced metafiles (EMF): handles,
 * bits, copies, recording (CreateMetaFile), playback and the WMF<->EMF
 * conversions OLE uses. Formats: Microsoft [MS-WMF] and [MS-EMF]; APIs:
 * CreateMetaFileW, CloseMetaFile, SetMetaFileBitsEx, GetMetaFileBitsEx,
 * CopyMetaFileW, PlayMetaFile, DeleteMetaFile, SetEnhMetaFileBits,
 * GetEnhMetaFileBits, GetEnhMetaFileHeader, CopyEnhMetaFileW,
 * PlayEnhMetaFile, DeleteEnhMetaFile, GetWinMetaFileBits and
 * SetWinMetaFileBits (Microsoft Learn).
 * Records handled (recorded and played): state (colours, modes, mapping,
 * window/viewport origin and extent, text alignment and spacing, save and
 * restore), objects (pens, solid/hatched brushes, fonts; select/delete),
 * clipping rectangles, lines, rectangles, ellipses, rounded rectangles,
 * polygons, polylines, poly-polygons, pixels, pattern blits, text and
 * device-independent bitmaps (StretchDIB, DIBStretchBlt, DIBBitBlt), and
 * comments. Other records are skipped as unsupported (limited: arcs,
 * paths, palettes, regions, world transforms). Integer arithmetic only.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#ifndef W32_HOST_MODEL
__declspec(dllimport) void *W32API CreateFileA(const char *, DWORD, DWORD,
                                               void *, DWORD, DWORD, void *);
__declspec(dllimport) BOOL W32API WriteFile(void *, const void *, DWORD,
                                            DWORD *, void *);
__declspec(dllimport) BOOL W32API CloseHandle(void *);
#else
extern void *W32API CreateFileA(const char *, DWORD, DWORD, void *, DWORD,
                                DWORD, void *);
extern BOOL W32API WriteFile(void *, const void *, DWORD, DWORD *, void *);
extern BOOL W32API CloseHandle(void *);
#endif
#define WMF_LIMIT (16u << 20)
/* WMF record functions ([MS-WMF] 2.1.1.1). */
enum {
  M_EOF = 0x0000, M_SAVEDC = 0x001e, M_SETBKMODE = 0x0102,
  M_SETMAPMODE = 0x0103, M_SETROP2 = 0x0104, M_SETPOLYFILLMODE = 0x0106,
  M_SETSTRETCHBLTMODE = 0x0107, M_SETTEXTCHAREXTRA = 0x0108,
  M_RESTOREDC = 0x0127, M_SELECTOBJECT = 0x012d, M_SETTEXTALIGN = 0x012e,
  M_DELETEOBJECT = 0x01f0, M_SETBKCOLOR = 0x0201, M_SETTEXTCOLOR = 0x0209,
  M_SETWINDOWORG = 0x020b, M_SETWINDOWEXT = 0x020c, M_SETVIEWPORTORG = 0x020d,
  M_SETVIEWPORTEXT = 0x020e, M_OFFSETWINDOWORG = 0x020f,
  M_OFFSETVIEWPORTORG = 0x0211, M_LINETO = 0x0213, M_MOVETO = 0x0214,
  M_CREATEPENINDIRECT = 0x02fa, M_CREATEFONTINDIRECT = 0x02fb,
  M_CREATEBRUSHINDIRECT = 0x02fc, M_POLYGON = 0x0324, M_POLYLINE = 0x0325,
  M_EXCLUDECLIPRECT = 0x0415, M_INTERSECTCLIPRECT = 0x0416,
  M_ELLIPSE = 0x0418, M_RECTANGLE = 0x041b, M_SETPIXEL = 0x041f,
  M_TEXTOUT = 0x0521, M_POLYPOLYGON = 0x0538, M_ROUNDRECT = 0x061c,
  M_PATBLT = 0x061d, M_ESCAPE = 0x0626, M_DIBBITBLT = 0x0940,
  M_EXTTEXTOUT = 0x0a32, M_DIBSTRETCHBLT = 0x0b41, M_STRETCHDIB = 0x0f43
};
/* EMF record types ([MS-EMF] 2.1.1). */
enum {
  E_HEADER = 1, E_POLYLINE = 4, E_POLYGON = 3, E_POLYPOLYGON = 8,
  E_SETWINDOWEXTEX = 9, E_SETWINDOWORGEX = 10, E_SETVIEWPORTEXTEX = 11,
  E_SETVIEWPORTORGEX = 12, E_EOF = 14, E_SETPIXELV = 15, E_SETMAPMODE = 17,
  E_SETBKMODE = 18, E_SETPOLYFILLMODE = 19, E_SETROP2 = 20,
  E_SETSTRETCHBLTMODE = 21, E_SETTEXTALIGN = 22, E_SETTEXTCOLOR = 24,
  E_SETBKCOLOR = 25, E_MOVETOEX = 27, E_EXCLUDECLIPRECT = 29,
  E_INTERSECTCLIPRECT = 30, E_SAVEDC = 33, E_RESTOREDC = 34,
  E_SELECTOBJECT = 37, E_CREATEPEN = 38, E_CREATEBRUSHINDIRECT = 39,
  E_DELETEOBJECT = 40, E_ELLIPSE = 42, E_RECTANGLE = 43, E_ROUNDRECT = 44,
  E_LINETO = 54, E_GDICOMMENT = 70, E_BITBLT = 76, E_STRETCHBLT = 77,
  E_STRETCHDIBITS = 81, E_EXTCREATEFONTINDIRECTW = 82, E_EXTTEXTOUTA = 83,
  E_EXTTEXTOUTW = 84, E_POLYGON16 = 86, E_POLYLINE16 = 87,
  E_POLYPOLYGON16 = 91
};
#define EMF_SIGNATURE 0x464d4520u
#define WMFC 0x43464d57u /* embedded EMF comment identifier */

/* ---- growable byte buffers ------------------------------------------------ */
struct buffer {
  uint8_t *p;
  DWORD n, cap;
  int failed;
};
static int grow(struct buffer *b, DWORD more) {
  if (b->failed || b->n + more > WMF_LIMIT || b->n + more < b->n) {
    b->failed = 1;
    return 0;
  }
  if (b->n + more <= b->cap)
    return 1;
  DWORD cap = b->cap ? b->cap : 256;
  while (cap < b->n + more)
    cap *= 2;
  uint8_t *p = gdi_alloc(cap);
  if (!p) {
    b->failed = 1;
    return 0;
  }
  if (b->n)
    w32_copy(p, b->p, b->n);
  gdi_free(b->p, b->cap);
  b->p = p;
  b->cap = cap;
  return 1;
}
static void put16(struct buffer *b, unsigned v) {
  if (grow(b, 2)) {
    w32_put16(b->p + b->n, (uint16_t)v);
    b->n += 2;
  }
}
static void put32(struct buffer *b, DWORD v) {
  if (grow(b, 4)) {
    w32_put32(b->p + b->n, v);
    b->n += 4;
  }
}
static void put_bytes(struct buffer *b, const void *p, DWORD n) {
  if (n && grow(b, n)) {
    w32_copy(b->p + b->n, p, n);
    b->n += n;
  }
}
static void release(struct buffer *b) {
  gdi_free(b->p, b->cap);
  *b = (struct buffer){0, 0, 0, 0};
}
static int16_t clamp16(LONG v) {
  return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}

/* ---- handles ------------------------------------------------------------- */
static HGDIOBJ meta_new(unsigned type, const void *bits, DWORD size) {
  HGDIOBJ h;
  uint8_t *copy = gdi_alloc(size);
  if (!copy) {
    SetLastError(8);
    return 0;
  }
  w32_copy(copy, bits, size);
  struct object *o = gdi_new(type, &h);
  if (!o) {
    gdi_free(copy, size);
    SetLastError(8);
    return 0;
  }
  o->u.meta.bits = copy;
  o->u.meta.size = size;
  return h;
}
static struct object *meta_of(HGDIOBJ h, unsigned type) {
  struct object *o = gdi_object(h, type);
  if (!o)
    SetLastError(6);
  return o;
}
/* DeleteObject releases the bits of a metafile object. */
static BOOL meta_delete(HGDIOBJ h, unsigned type) {
  GDI_GUARD;
  return meta_of(h, type) && DeleteObject(h);
}
static DWORD meta_bits(HGDIOBJ h, unsigned type, DWORD cap, void *out) {
  GDI_GUARD;
  struct object *o = meta_of(h, type);
  if (!o)
    return 0;
  if (!out)
    return o->u.meta.size;
  if (cap < o->u.meta.size) {
    SetLastError(122);
    return 0;
  }
  w32_copy(out, o->u.meta.bits, o->u.meta.size);
  return o->u.meta.size;
}
static BOOL write_file(const char *name, const void *bits, DWORD size) {
  void *f = CreateFileA(name, 0x40000000u, 0, 0, 2 /* CREATE_ALWAYS */, 0, 0);
  if (!f || f == (void *)(intptr_t)-1)
    return 0;
  DWORD done = 0;
  BOOL ok = WriteFile(f, bits, size, &done, 0) && done == size;
  CloseHandle(f);
  return ok;
}
static BOOL narrow_name(const WCHAR *w, char *out) {
  unsigned i = 0;
  for (; w[i] && i < 259; i++) {
    int c = gdi_cp1252_ansi(w[i]);
    if (c < 0) {
      SetLastError(123);
      return 0;
    }
    out[i] = (char)c;
  }
  if (w[i]) {
    SetLastError(206);
    return 0;
  }
  out[i] = 0;
  return 1;
}

/* ---- Windows metafile bits ----------------------------------------------- */
/* METAHEADER: type 1 or 2, header size 9 words, version 0x100 or 0x300,
 * total size in words. Records: size in words, function, parameters. */
static BOOL valid_wmf(const uint8_t *p, DWORD size) {
  if (!p || size < 18 || (w32_u16(p) != 1 && w32_u16(p) != 2) ||
      w32_u16(p + 2) != 9 ||
      (w32_u16(p + 4) != 0x300 && w32_u16(p + 4) != 0x100))
    return 0;
  DWORD words = w32_u32(p + 6);
  if (words < 9 || words > size / 2)
    return 0;
  DWORD at = 18, end = words * 2;
  while (at + 6 <= end) {
    DWORD n = w32_u32(p + at) * 2;
    if (n < 6 || at + n > end)
      return 0;
    if (!w32_u16(p + at + 4))
      return 1;
    at += n;
  }
  return 0;
}
HGDIOBJ W32API SetMetaFileBitsEx(DWORD size, const uint8_t *bits) {
  GDI_GUARD;
  if (!valid_wmf(bits, size)) {
    SetLastError(87);
    return 0;
  }
  return meta_new(GDI_METAFILE, bits, w32_u32(bits + 6) * 2);
}
DWORD W32API GetMetaFileBitsEx(HGDIOBJ h, DWORD cap, void *out) {
  return meta_bits(h, GDI_METAFILE, cap, out);
}
BOOL W32API DeleteMetaFile(HGDIOBJ h) { return meta_delete(h, GDI_METAFILE); }
HGDIOBJ W32API CopyMetaFileW(HGDIOBJ h, const WCHAR *file) {
  GDI_GUARD;
  struct object *o = meta_of(h, GDI_METAFILE);
  char name[260];
  if (!o || (file && !narrow_name(file, name)))
    return 0;
  if (file && !write_file(name, o->u.meta.bits, o->u.meta.size))
    return 0;
  return meta_new(GDI_METAFILE, o->u.meta.bits, o->u.meta.size);
}
HGDIOBJ W32API CopyMetaFileA(HGDIOBJ h, const char *file) {
  GDI_GUARD;
  struct object *o = meta_of(h, GDI_METAFILE);
  if (!o || (file && !write_file(file, o->u.meta.bits, o->u.meta.size)))
    return 0;
  return meta_new(GDI_METAFILE, o->u.meta.bits, o->u.meta.size);
}

/* ---- recording ------------------------------------------------------------ */
struct gdi_recorder {
  struct buffer data;
  DWORD largest;
  HGDIOBJ objects[64];
  unsigned count;
  char file[260];
  int to_file;
};
static void record_words(struct gdi_recorder *r, unsigned function,
                         const int16_t *p, unsigned count, const void *tail,
                         DWORD tail_bytes) {
  DWORD words = 3 + count + (tail_bytes + 1) / 2;
  put32(&r->data, words);
  put16(&r->data, function);
  for (unsigned i = 0; i < count; i++)
    put16(&r->data, (uint16_t)p[i]);
  put_bytes(&r->data, tail, tail_bytes);
  if (tail_bytes & 1)
    put_bytes(&r->data, "", 1);
  if (words > r->largest)
    r->largest = words;
}
int gdi_record(struct dc *d, unsigned function, const int16_t *p,
               unsigned count) {
  if (!d || !d->record)
    return 0;
  record_words(d->record, function, p, count, 0, 0);
  return 1;
}
static unsigned object_slot(struct gdi_recorder *r, HGDIOBJ h, int *fresh) {
  for (unsigned i = 0; i < r->count; i++)
    if (r->objects[i] == h) {
      *fresh = 0;
      return i;
    }
  *fresh = 1;
  if (r->count == 64)
    return 64;
  r->objects[r->count] = h;
  return r->count++;
}
int gdi_record_select(HDC h, struct dc *d, HGDIOBJ obj) {
  (void)h;
  struct gdi_recorder *r = d ? d->record : 0;
  struct object *o = gdi_object(obj, 0);
  if (!r || !o ||
      (o->type != GDI_PEN && o->type != GDI_BRUSH && o->type != GDI_FONT))
    return 0;
  int fresh;
  unsigned slot = object_slot(r, obj, &fresh);
  if (slot == 64)
    return 0;
  if (fresh) {
    if (o->type == GDI_PEN) {
      LOGPEN *p = &o->u.pen;
      int16_t v[5] = {(int16_t)p->lopnStyle, clamp16(p->lopnWidth.x), 0,
                      (int16_t)(p->lopnColor & 0xffff),
                      (int16_t)(p->lopnColor >> 16)};
      record_words(r, M_CREATEPENINDIRECT, v, 5, 0, 0);
    } else if (o->type == GDI_BRUSH) {
      const LOGBRUSH *b = &o->u.brush.log;
      /* Pattern brushes are recorded with their colour (limited). */
      unsigned style = b->lbStyle <= 2 ? b->lbStyle : 0;
      int16_t v[4] = {(int16_t)style, (int16_t)(b->lbColor & 0xffff),
                      (int16_t)(b->lbColor >> 16), (int16_t)b->lbHatch};
      record_words(r, M_CREATEBRUSHINDIRECT, v, 4, 0, 0);
    } else {
      LOGFONTA lf;
      w32_fill(&lf, 0, sizeof(lf));
      GetObjectA(obj, sizeof(lf), &lf);
      int16_t v[5] = {clamp16(lf.lfHeight), clamp16(lf.lfWidth),
                      clamp16(lf.lfEscapement), clamp16(lf.lfOrientation),
                      clamp16(lf.lfWeight)};
      uint8_t tail[40];
      w32_copy(tail, &lf.lfItalic, 8);
      w32_copy(tail + 8, lf.lfFaceName, 32);
      record_words(r, M_CREATEFONTINDIRECT, v, 5, tail, 40);
    }
  }
  int16_t s[1] = {(int16_t)slot};
  record_words(r, M_SELECTOBJECT, s, 1, 0, 0);
  return 1;
}
int gdi_record_text(struct dc *d, int x, int y, unsigned flags,
                    const RECT *rect, const void *text, unsigned n,
                    const int *dx, int wide) {
  struct gdi_recorder *r = d ? d->record : 0;
  if (!r)
    return 0;
  if (n > 4096)
    n = 4096;
  uint8_t *s = gdi_alloc(n + 1);
  if (!s && n)
    return 1;
  for (unsigned i = 0; i < n; i++) {
    if (!wide)
      s[i] = ((const uint8_t *)text)[i];
    else {
      int c = gdi_cp1252_ansi(((const WCHAR *)text)[i]);
      s[i] = (uint8_t)(c < 0 ? '?' : c);
    }
  }
  int16_t head[8] = {clamp16(y), clamp16(x), (int16_t)n, (int16_t)flags};
  unsigned count = 4;
  if (rect && (flags & (ETO_OPAQUE | ETO_CLIPPED))) {
    head[4] = clamp16(rect->left);
    head[5] = clamp16(rect->top);
    head[6] = clamp16(rect->right);
    head[7] = clamp16(rect->bottom);
    count = 8;
  }
  DWORD pad = (n + 1) & ~1u, bytes = pad + (dx ? n * 2 : 0);
  uint8_t *tail = gdi_alloc(bytes + 1);
  if (tail) {
    w32_copy(tail, s, n);
    for (unsigned i = 0; dx && i < n; i++)
      w32_put16(tail + pad + i * 2, (uint16_t)clamp16(dx[i]));
    record_words(r, M_EXTTEXTOUT, head, count, tail, bytes);
    gdi_free(tail, bytes + 1);
  }
  gdi_free(s, n + 1);
  return 1;
}
int gdi_record_poly(struct dc *d, unsigned function, const POINT *p,
                    const int *counts, unsigned n) {
  struct gdi_recorder *r = d ? d->record : 0;
  if (!r)
    return 0;
  unsigned points = 0, header = function == M_POLYPOLYGON ? 1 + n : 1;
  if (function == M_POLYPOLYGON)
    for (unsigned i = 0; i < n; i++)
      points += (unsigned)counts[i];
  else
    points = n;
  if (points > 16000 || header > 1000)
    return 1;
  int16_t *v = gdi_alloc((header + points * 2) * 2);
  if (!v)
    return 1;
  v[0] = (int16_t)n;
  if (function == M_POLYPOLYGON)
    for (unsigned i = 0; i < n; i++)
      v[1 + i] = (int16_t)counts[i];
  for (unsigned i = 0; i < points; i++) {
    v[header + i * 2] = clamp16(p[i].x);
    v[header + i * 2 + 1] = clamp16(p[i].y);
  }
  record_words(r, function, v, header + points * 2, 0, 0);
  gdi_free(v, (header + points * 2) * 2);
  return 1;
}
/* META_STRETCHDIB: rop, colour use, source rectangle, destination
 * rectangle and a packed DIB (header, colour table, bits). */
int gdi_record_dib(struct dc *d, int x, int y, int w, int h, int sx, int sy,
                   int sw, int sh, const void *bits, const BITMAPINFO *info,
                   unsigned usage, DWORD rop) {
  struct gdi_recorder *r = d ? d->record : 0;
  if (!r)
    return 0;
  const BITMAPINFOHEADER *bi = info ? &info->bmiHeader : 0;
  if (!bi || bi->biSize < 40 || !bits || bi->biWidth <= 0 ||
      bi->biWidth > 4096 || !bi->biHeight || bi->biHeight > 4096 ||
      bi->biHeight < -4096)
    return 1;
  DWORD colors = bi->biBitCount <= 8
                     ? (bi->biClrUsed ? bi->biClrUsed : 1u << bi->biBitCount)
                     : bi->biCompression == 3 ? 3 : 0;
  DWORD table = colors * (usage == 1 ? 2 : 4);
  DWORD rows = (DWORD)(bi->biHeight < 0 ? -bi->biHeight : bi->biHeight);
  DWORD pitch = (((DWORD)bi->biWidth * bi->biBitCount + 31) / 32) * 4;
  DWORD image = pitch * rows, packed = 40 + table + image;
  uint8_t *dib = gdi_alloc(packed);
  if (!dib)
    return 1;
  w32_copy(dib, bi, 40);
  w32_put32(dib, 40);
  w32_copy(dib + 40, (const uint8_t *)info + bi->biSize, table);
  w32_copy(dib + 40 + table, bits, image);
  int16_t v[11] = {(int16_t)(rop & 0xffff), (int16_t)(rop >> 16),
                   (int16_t)usage,          clamp16(sh),
                   clamp16(sw),             clamp16(sy),
                   clamp16(sx),             clamp16(h),
                   clamp16(w),              clamp16(y),
                   clamp16(x)};
  record_words(r, M_STRETCHDIB, v, 11, dib, packed);
  gdi_free(dib, packed);
  return 1;
}
/* A GDI bitmap (struct w32_dib) recorded as a packed DIB. */
int gdi_record_bitmap(struct dc *d, int x, int y, int w, int h,
                      const struct w32_dib *src, int sx, int sy, int sw, int sh,
                      DWORD rop) {
  if (!d || !d->record)
    return 0;
  if (!src || !src->bits)
    return 1;
  struct {
    BITMAPINFOHEADER h;
    DWORD table[256];
  } info;
  w32_fill(&info, 0, sizeof(info));
  info.h.biSize = 40;
  info.h.biWidth = (LONG)src->width;
  info.h.biHeight = src->top_down ? -(LONG)src->height : (LONG)src->height;
  info.h.biPlanes = 1;
  info.h.biBitCount = (uint16_t)src->bpp;
  if (src->bpp <= 8) {
    unsigned n = src->color_count ? src->color_count : 1u << src->bpp;
    info.h.biClrUsed = n;
    for (unsigned i = 0; i < n && i < 256; i++)
      info.table[i] = src->colors[i];
  } else if (src->bpp == 16 || src->bpp == 32) {
    if (src->masks[0]) {
      info.h.biCompression = 3; /* BI_BITFIELDS */
      for (unsigned i = 0; i < 3; i++)
        info.table[i] = src->masks[i];
    }
  }
  DWORD pitch = ((src->width * src->bpp + 31) / 32) * 4;
  if (pitch != src->pitch) { /* repack rows to the DIB pitch */
    uint8_t *copy = gdi_alloc(pitch * src->height);
    if (!copy)
      return 1;
    for (DWORD row = 0; row < src->height; row++)
      w32_copy(copy + row * pitch, src->bits + row * src->pitch,
               pitch < src->pitch ? pitch : src->pitch);
    gdi_record_dib(d, x, y, w, h, sx,
                   src->top_down ? sy : (int)src->height - sy - sh, sw, sh,
                   copy, (const BITMAPINFO *)&info, 0, rop);
    gdi_free(copy, pitch * src->height);
    return 1;
  }
  return gdi_record_dib(d, x, y, w, h, sx,
                        src->top_down ? sy : (int)src->height - sy - sh, sw,
                        sh, src->bits, (const BITMAPINFO *)&info, 0, rop);
}
int gdi_metafile_escape(HDC h, int escape, int bytes, const void *data) {
  struct dc *d = gdi_dc(h);
  if (!d || !d->record || bytes < 0 || bytes > 32000 || (bytes && !data))
    return 0;
  int16_t v[2] = {(int16_t)escape, (int16_t)bytes};
  record_words(d->record, M_ESCAPE, v, 2, data, (DWORD)bytes);
  return 1;
}
void gdi_record_free(struct dc *d) {
  if (d && d->record) {
    release(&d->record->data);
    gdi_free(d->record, sizeof(*d->record));
    d->record = 0;
  }
}
static HDC create_metafile(const char *file) {
  HDC h = CreateCompatibleDC(0);
  struct dc *d = gdi_dc(h);
  struct gdi_recorder *r = d ? gdi_alloc(sizeof(*r)) : 0;
  if (!r) {
    if (h)
      DeleteDC(h);
    SetLastError(8);
    return 0;
  }
  if (file) {
    unsigned i = 0;
    for (; file[i] && i < 259; i++)
      r->file[i] = file[i];
    r->file[i] = 0;
    r->to_file = 1;
  }
  d->record = r;
  return h;
}
HDC W32API CreateMetaFileA(const char *file) {
  GDI_GUARD;
  return create_metafile(file);
}
HDC W32API CreateMetaFileW(const WCHAR *file) {
  GDI_GUARD;
  char name[260];
  if (file && !narrow_name(file, name))
    return 0;
  return create_metafile(file ? name : 0);
}
HGDIOBJ W32API CloseMetaFile(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct gdi_recorder *r = d ? d->record : 0;
  if (!r) {
    SetLastError(6);
    return 0;
  }
  struct buffer out = {0, 0, 0, 0};
  record_words(r, M_EOF, 0, 0, 0, 0);
  DWORD words = 9 + r->data.n / 2;
  put16(&out, r->to_file ? 2 : 1);
  put16(&out, 9);
  put16(&out, 0x300);
  put32(&out, words);
  put16(&out, (uint16_t)r->count);
  put32(&out, r->largest);
  put16(&out, 0);
  put_bytes(&out, r->data.p, r->data.n);
  HGDIOBJ result = 0;
  if (!out.failed && !r->data.failed &&
      (!r->to_file || write_file(r->file, out.p, out.n))) {
    /* A disk metafile handle still holds the bits (type 1 in memory). */
    w32_put16(out.p, 1);
    result = meta_new(GDI_METAFILE, out.p, out.n);
  } else if (out.failed || r->data.failed)
    SetLastError(8);
  release(&out);
  gdi_record_free(d);
  DeleteDC(h);
  return result;
}

/* ---- WMF playback ----------------------------------------------------- */
static int16_t s16(const uint8_t *p, unsigned i) {
  return (int16_t)w32_u16(p + i * 2);
}
static COLORREF color_at(const uint8_t *p, unsigned i) {
  return (COLORREF)w32_u16(p + i * 2) | ((COLORREF)w32_u16(p + i * 2 + 2) << 16);
}
struct objects {
  HGDIOBJ h[256];
  unsigned count;
};
static void object_add(struct objects *o, HGDIOBJ h) {
  for (unsigned i = 0; i < o->count; i++)
    if (!o->h[i]) {
      o->h[i] = h;
      return;
    }
  if (o->count < 256)
    o->h[o->count++] = h;
  else if (h)
    DeleteObject(h);
}
static void objects_free(HDC dc, struct objects *o, HGDIOBJ pen, HGDIOBJ brush,
                         HGDIOBJ font) {
  SelectObject(dc, pen);
  SelectObject(dc, brush);
  SelectObject(dc, font);
  for (unsigned i = 0; i < o->count; i++)
    if (o->h[i])
      DeleteObject(o->h[i]);
}
static HFONT font16(const uint8_t *p, DWORD n) {
  if (n < 18)
    return 0;
  LOGFONTA lf;
  w32_fill(&lf, 0, sizeof(lf));
  lf.lfHeight = s16(p, 0);
  lf.lfWidth = s16(p, 1);
  lf.lfEscapement = s16(p, 2);
  lf.lfOrientation = s16(p, 3);
  lf.lfWeight = s16(p, 4);
  w32_copy(&lf.lfItalic, p + 10, 8);
  DWORD face = n - 18 < 31 ? n - 18 : 31;
  w32_copy(lf.lfFaceName, p + 18, face);
  return CreateFontIndirectA(&lf);
}
static void play_dib(HDC dc, const uint8_t *dib, DWORD n, int x, int y, int w,
                     int h, int sx, int sy, int sw, int sh, DWORD rop,
                     unsigned usage) {
  if (n < 40 || w32_u32(dib) < 40 || w32_u32(dib) > n)
    return;
  const BITMAPINFOHEADER *bi = (const BITMAPINFOHEADER *)dib;
  DWORD colors = bi->biBitCount <= 8
                     ? (bi->biClrUsed ? bi->biClrUsed : 1u << bi->biBitCount)
                     : bi->biCompression == 3 ? 3 : 0;
  DWORD offset = bi->biSize + colors * (usage == 1 ? 2 : 4);
  DWORD rows = (DWORD)(bi->biHeight < 0 ? -bi->biHeight : bi->biHeight);
  DWORD pitch = (((DWORD)bi->biWidth * bi->biBitCount + 31) / 32) * 4;
  if (offset > n || (uint64_t)pitch * rows > n - offset)
    return;
  StretchDIBits(dc, x, y, w, h, sx, sy, sw, sh, dib + offset,
                (const BITMAPINFO *)dib, usage, rop);
}
/* One WMF record on dc (logical coordinates of dc). */
static void play_wmf_record(HDC dc, struct objects *o, unsigned f,
                            const uint8_t *p, DWORD n) {
  unsigned c = n / 2;
#define NEED(k)                                                                \
  if (c < (k))                                                                 \
    return;
  switch (f) {
  case M_SAVEDC:
    SaveDC(dc);
    break;
  case M_RESTOREDC:
    NEED(1) RestoreDC(dc, s16(p, 0));
    break;
  case M_SETBKMODE:
    NEED(1) SetBkMode(dc, s16(p, 0));
    break;
  case M_SETMAPMODE:
    NEED(1) SetMapMode(dc, s16(p, 0));
    break;
  case M_SETROP2:
    NEED(1) SetROP2(dc, s16(p, 0));
    break;
  case M_SETPOLYFILLMODE:
    NEED(1) SetPolyFillMode(dc, s16(p, 0));
    break;
  case M_SETSTRETCHBLTMODE:
    NEED(1) SetStretchBltMode(dc, s16(p, 0));
    break;
  case M_SETTEXTCHAREXTRA:
    NEED(1) SetTextCharacterExtra(dc, s16(p, 0));
    break;
  case M_SETTEXTALIGN:
    NEED(1) SetTextAlign(dc, (unsigned)(uint16_t)s16(p, 0));
    break;
  case M_SETBKCOLOR:
    NEED(2) SetBkColor(dc, color_at(p, 0));
    break;
  case M_SETTEXTCOLOR:
    NEED(2) SetTextColor(dc, color_at(p, 0));
    break;
  case M_SETWINDOWORG:
    NEED(2) SetWindowOrgEx(dc, s16(p, 1), s16(p, 0), 0);
    break;
  case M_SETWINDOWEXT:
    NEED(2) SetWindowExtEx(dc, s16(p, 1), s16(p, 0), 0);
    break;
  case M_SETVIEWPORTORG:
    NEED(2) SetViewportOrgEx(dc, s16(p, 1), s16(p, 0), 0);
    break;
  case M_SETVIEWPORTEXT:
    NEED(2) SetViewportExtEx(dc, s16(p, 1), s16(p, 0), 0);
    break;
  case M_OFFSETWINDOWORG:
    NEED(2) OffsetWindowOrgEx(dc, s16(p, 1), s16(p, 0), 0);
    break;
  case M_OFFSETVIEWPORTORG:
    NEED(2) OffsetViewportOrgEx(dc, s16(p, 1), s16(p, 0), 0);
    break;
  case M_MOVETO:
    NEED(2) MoveToEx(dc, s16(p, 1), s16(p, 0), 0);
    break;
  case M_LINETO:
    NEED(2) LineTo(dc, s16(p, 1), s16(p, 0));
    break;
  case M_RECTANGLE:
    NEED(4) Rectangle(dc, s16(p, 3), s16(p, 2), s16(p, 1), s16(p, 0));
    break;
  case M_ELLIPSE:
    NEED(4) Ellipse(dc, s16(p, 3), s16(p, 2), s16(p, 1), s16(p, 0));
    break;
  case M_ROUNDRECT:
    NEED(6) RoundRect(dc, s16(p, 5), s16(p, 4), s16(p, 3), s16(p, 2),
                      s16(p, 1), s16(p, 0));
    break;
  case M_INTERSECTCLIPRECT:
    NEED(4) IntersectClipRect(dc, s16(p, 3), s16(p, 2), s16(p, 1), s16(p, 0));
    break;
  case M_EXCLUDECLIPRECT:
    NEED(4) ExcludeClipRect(dc, s16(p, 3), s16(p, 2), s16(p, 1), s16(p, 0));
    break;
  case M_SETPIXEL:
    NEED(4) SetPixelV(dc, s16(p, 3), s16(p, 2), color_at(p, 0));
    break;
  case M_PATBLT:
    NEED(6) PatBlt(dc, s16(p, 5), s16(p, 4), s16(p, 3), s16(p, 2),
                   color_at(p, 0));
    break;
  case M_POLYGON:
  case M_POLYLINE: {
    NEED(1) int k = s16(p, 0);
    if (k < 1 || (unsigned)(1 + k * 2) > c)
      return;
    POINT *pts = gdi_alloc((DWORD)k * sizeof(POINT));
    if (!pts)
      return;
    for (int i = 0; i < k; i++)
      pts[i] = (POINT){s16(p, 1 + i * 2), s16(p, 2 + i * 2)};
    if (f == M_POLYGON)
      Polygon(dc, pts, k);
    else
      Polyline(dc, pts, k);
    gdi_free(pts, (DWORD)k * sizeof(POINT));
    break;
  }
  case M_POLYPOLYGON: {
    NEED(1) int polys = s16(p, 0);
    if (polys < 1 || (unsigned)(1 + polys) > c)
      return;
    int total = 0;
    for (int i = 0; i < polys; i++)
      total += s16(p, 1 + (unsigned)i) > 0 ? s16(p, 1 + (unsigned)i) : 0;
    if ((unsigned)(1 + polys + total * 2) > c)
      return;
    POINT *pts = gdi_alloc((DWORD)total * sizeof(POINT) + 4);
    int *counts = gdi_alloc((DWORD)polys * sizeof(int));
    if (pts && counts) {
      for (int i = 0; i < polys; i++)
        counts[i] = s16(p, 1 + (unsigned)i);
      for (int i = 0; i < total; i++)
        pts[i] = (POINT){s16(p, 1 + polys + i * 2), s16(p, 2 + polys + i * 2)};
      PolyPolygon(dc, pts, counts, polys);
    }
    gdi_free(pts, (DWORD)total * sizeof(POINT) + 4);
    gdi_free(counts, (DWORD)polys * sizeof(int));
    break;
  }
  case M_TEXTOUT: {
    NEED(1) int k = s16(p, 0);
    unsigned words = 1 + ((unsigned)k + 1) / 2;
    if (k < 0 || words + 2 > c)
      return;
    TextOutA(dc, s16(p, words + 1), s16(p, words), (const char *)p + 2, k);
    break;
  }
  case M_EXTTEXTOUT: {
    NEED(4) int y = s16(p, 0), x = s16(p, 1), k = s16(p, 2);
    unsigned flags = (uint16_t)s16(p, 3), at = 4;
    RECT r = {0, 0, 0, 0};
    if (flags & (ETO_OPAQUE | ETO_CLIPPED)) {
      NEED(8) r = (RECT){s16(p, 4), s16(p, 5), s16(p, 6), s16(p, 7)};
      at = 8;
    }
    if (k < 0 || at + ((unsigned)k + 1) / 2 > c)
      return;
    const int16_t *dx16 = 0;
    unsigned dx_at = at + ((unsigned)k + 1) / 2;
    if (k && dx_at + (unsigned)k <= c)
      dx16 = (const int16_t *)(p + dx_at * 2);
    int *dx = dx16 ? gdi_alloc((DWORD)k * sizeof(int)) : 0;
    for (int i = 0; dx && i < k; i++)
      dx[i] = (int16_t)w32_u16(dx16 + i);
    ExtTextOutA(dc, x, y, flags & (ETO_OPAQUE | ETO_CLIPPED),
                at == 8 ? &r : 0, (const char *)p + at * 2, (unsigned)k, dx);
    if (dx)
      gdi_free(dx, (DWORD)k * sizeof(int));
    break;
  }
  case M_CREATEPENINDIRECT: {
    NEED(5) object_add(o, CreatePen(s16(p, 0), s16(p, 1), color_at(p, 3)));
    break;
  }
  case M_CREATEBRUSHINDIRECT: {
    NEED(4) LOGBRUSH b = {(DWORD)(uint16_t)s16(p, 0), color_at(p, 1),
                          (uintptr_t)(uint16_t)s16(p, 3)};
    object_add(o, CreateBrushIndirect(&b));
    break;
  }
  case M_CREATEFONTINDIRECT:
    object_add(o, font16(p, n));
    break;
  case M_SELECTOBJECT: {
    NEED(1) unsigned i = (uint16_t)s16(p, 0);
    if (i < o->count && o->h[i])
      SelectObject(dc, o->h[i]);
    break;
  }
  case M_DELETEOBJECT: {
    NEED(1) unsigned i = (uint16_t)s16(p, 0);
    if (i < o->count && o->h[i]) {
      HGDIOBJ stock[3] = {GetStockObject(7), GetStockObject(0),
                          GetStockObject(13)};
      for (unsigned k = 0; k < 3; k++)
        if (GetCurrentObject(dc, k == 0 ? GDI_PEN : k == 1 ? GDI_BRUSH
                                                            : GDI_FONT) == o->h[i])
          SelectObject(dc, stock[k]);
      DeleteObject(o->h[i]);
      o->h[i] = 0;
    }
    break;
  }
  case M_STRETCHDIB: {
    NEED(11) play_dib(dc, p + 22, n - 22, s16(p, 10), s16(p, 9), s16(p, 8),
                      s16(p, 7), s16(p, 6), s16(p, 5), s16(p, 4), s16(p, 3),
                      color_at(p, 0), (unsigned)s16(p, 2));
    break;
  }
  case M_DIBSTRETCHBLT: {
    NEED(10) play_dib(dc, p + 20, n - 20, s16(p, 9), s16(p, 8), s16(p, 7),
                      s16(p, 6), s16(p, 5), s16(p, 4), s16(p, 3), s16(p, 2),
                      color_at(p, 0), 0);
    break;
  }
  case M_DIBBITBLT: {
    NEED(8) play_dib(dc, p + 16, n - 16, s16(p, 7), s16(p, 6), s16(p, 5),
                     s16(p, 4), s16(p, 3), s16(p, 2), s16(p, 5), s16(p, 4),
                     color_at(p, 0), 0);
    break;
  }
  default: /* comments and unsupported records */
    break;
  }
#undef NEED
}
static BOOL play_wmf(HDC dc, const uint8_t *bits, DWORD size) {
  if (!gdi_dc(dc) || !valid_wmf(bits, size))
    return 0;
  struct objects *o = gdi_alloc(sizeof(*o));
  if (!o)
    return 0;
  HGDIOBJ pen = GetCurrentObject(dc, GDI_PEN),
          brush = GetCurrentObject(dc, GDI_BRUSH),
          font = GetCurrentObject(dc, GDI_FONT);
  int level = SaveDC(dc);
  DWORD at = 18, end = w32_u32(bits + 6) * 2;
  while (at + 6 <= end) {
    DWORD n = w32_u32(bits + at) * 2;
    unsigned f = w32_u16(bits + at + 4);
    if (!f)
      break;
    play_wmf_record(dc, o, f, bits + at + 6, n - 6);
    at += n;
  }
  RestoreDC(dc, level);
  objects_free(dc, o, pen, brush, font);
  gdi_free(o, sizeof(*o));
  return 1;
}
BOOL W32API PlayMetaFile(HDC dc, HGDIOBJ h) {
  GDI_GUARD;
  struct object *o = meta_of(h, GDI_METAFILE);
  return o && play_wmf(dc, o->u.meta.bits, o->u.meta.size);
}

/* ---- enhanced metafile bits ----------------------------------------------- */
/* ENHMETAHEADER: type 1, size, bounds, frame (.01 mm), signature " EMF",
 * version, total bytes, records, handles, description, palette, reference
 * device size in pixels and millimetres. */
static BOOL valid_emf(const uint8_t *p, DWORD size) {
  if (!p || size < 88 || w32_u32(p) != E_HEADER || w32_u32(p + 4) < 88 ||
      w32_u32(p + 4) > size || w32_u32(p + 40) != EMF_SIGNATURE ||
      w32_u32(p + 48) > size || w32_u32(p + 48) < w32_u32(p + 4))
    return 0;
  DWORD at = 0, end = w32_u32(p + 48);
  while (at + 8 <= end) {
    DWORD n = w32_u32(p + at + 4);
    if (n < 8 || (n & 3) || at + n > end)
      return 0;
    if (w32_u32(p + at) == E_EOF)
      return 1;
    at += n;
  }
  return 0;
}
HGDIOBJ W32API SetEnhMetaFileBits(DWORD size, const uint8_t *bits) {
  GDI_GUARD;
  if (!valid_emf(bits, size)) {
    SetLastError(87);
    return 0;
  }
  return meta_new(GDI_ENHMETAFILE, bits, w32_u32(bits + 48));
}
DWORD W32API GetEnhMetaFileBits(HGDIOBJ h, DWORD cap, void *out) {
  return meta_bits(h, GDI_ENHMETAFILE, cap, out);
}
/* The header record (its own size), or that size with no buffer. */
DWORD W32API GetEnhMetaFileHeader(HGDIOBJ h, DWORD cap, void *out) {
  GDI_GUARD;
  struct object *o = meta_of(h, GDI_ENHMETAFILE);
  if (!o)
    return 0;
  DWORD n = w32_u32(o->u.meta.bits + 4);
  if (!out)
    return n;
  if (cap > n)
    cap = n;
  w32_copy(out, o->u.meta.bits, cap);
  return cap;
}
BOOL W32API DeleteEnhMetaFile(HGDIOBJ h) {
  return meta_delete(h, GDI_ENHMETAFILE);
}
HGDIOBJ W32API CopyEnhMetaFileW(HGDIOBJ h, const WCHAR *file) {
  GDI_GUARD;
  struct object *o = meta_of(h, GDI_ENHMETAFILE);
  char name[260];
  if (!o || (file && !narrow_name(file, name)))
    return 0;
  if (file && !write_file(name, o->u.meta.bits, o->u.meta.size))
    return 0;
  return meta_new(GDI_ENHMETAFILE, o->u.meta.bits, o->u.meta.size);
}
HGDIOBJ W32API CopyEnhMetaFileA(HGDIOBJ h, const char *file) {
  GDI_GUARD;
  struct object *o = meta_of(h, GDI_ENHMETAFILE);
  if (!o || (file && !write_file(file, o->u.meta.bits, o->u.meta.size)))
    return 0;
  return meta_new(GDI_ENHMETAFILE, o->u.meta.bits, o->u.meta.size);
}

/* ---- EMF mapping ------------------------------------------------------- */
/* The EMF's own logical space (its mapping records) maps to its reference
 * device pixels; those map onto the caller's rectangle, which is in the
 * target DC's logical units: out = rect + (ref - frame) * rect / frame. */
struct emf_map {
  int mode;
  POINT worg, vorg;
  SIZE wext, vext;
  int64_t fx, fy, fw, fh; /* frame in reference pixels */
  int64_t rx, ry, rw, rh; /* target rectangle */
  int64_t dpmx, dpmy;     /* reference pixels per 100 m (x100 mm) */
  int64_t sx, sy;
};
/* Division rounding half away from zero. */
static int64_t div_round(int64_t a, int64_t b) {
  if (!b)
    return 0;
  int64_t q = a / b, r = a % b;
  if (2 * (r < 0 ? -r : r) >= (b < 0 ? -b : b))
    q += (a < 0) == (b < 0) ? 1 : -1;
  return q;
}
static void emf_ref(const struct emf_map *m, LONG x, LONG y, int64_t *ox,
                    int64_t *oy) {
  int64_t lx = (int64_t)x - m->worg.x, ly = (int64_t)y - m->worg.y;
  switch (m->mode) {
  case 2: /* MM_LOMETRIC: 0.1 mm, y up */
  case 3: /* MM_HIMETRIC: 0.01 mm */
  case 4: /* MM_LOENGLISH: 0.01 in */
  case 5: /* MM_HIENGLISH: 0.001 in */
  case 6: { /* MM_TWIPS: 1/1440 in */
    static const int64_t per_100m[] = {0, 0, 1000000, 10000000, 3937008,
                                       39370079, 5669291};
    *ox = div_round(lx * m->dpmx, per_100m[m->mode]) + m->vorg.x;
    *oy = -div_round(ly * m->dpmy, per_100m[m->mode]) + m->vorg.y;
    return;
  }
  case 7: /* MM_ISOTROPIC */
  case 8: /* MM_ANISOTROPIC */
    *ox = (m->wext.cx ? div_round(lx * m->vext.cx, m->wext.cx) : lx) + m->vorg.x;
    *oy = (m->wext.cy ? div_round(ly * m->vext.cy, m->wext.cy) : ly) + m->vorg.y;
    return;
  default:
    *ox = lx + m->vorg.x;
    *oy = ly + m->vorg.y;
  }
}
static POINT emf_point(const struct emf_map *m, LONG x, LONG y) {
  int64_t rx, ry;
  emf_ref(m, x, y, &rx, &ry);
  int64_t ox = m->fw ? m->rx + div_round((rx - m->fx) * m->rw, m->fw) : m->rx;
  int64_t oy = m->fh ? m->ry + div_round((ry - m->fy) * m->rh, m->fh) : m->ry;
  return (POINT){(LONG)ox, (LONG)oy};
}
/* Lengths (pen widths, font heights) scale like the y axis. */
static LONG emf_length(const struct emf_map *m, LONG v, int vertical) {
  POINT a = emf_point(m, 0, 0), b = vertical ? emf_point(m, 0, v)
                                              : emf_point(m, v, 0);
  LONG d = vertical ? b.y - a.y : b.x - a.x;
  return d < 0 ? -d : d;
}
static RECT emf_rect(const struct emf_map *m, const uint8_t *p) {
  POINT a = emf_point(m, (LONG)w32_u32(p), (LONG)w32_u32(p + 4)),
        b = emf_point(m, (LONG)w32_u32(p + 8), (LONG)w32_u32(p + 12));
  return (RECT){a.x, a.y, b.x, b.y};
}
static void emf_reset(struct emf_map *m) {
  m->mode = 1;
  m->worg = m->vorg = (POINT){0, 0};
  m->wext = m->vext = (SIZE){1, 1};
}

/* ---- EMF playback ------------------------------------------------------- */
static void play_emf_record(HDC dc, struct emf_map *m, struct objects *o,
                            const uint8_t *r, DWORD n, struct emf_map *saved,
                            unsigned *depth) {
  DWORD type = w32_u32(r);
  const uint8_t *p = r + 8;
  DWORD c = n - 8;
#define NEED(k)                                                                \
  if (c < (k))                                                                 \
    return;
#define I32(i) ((LONG)w32_u32(p + (i) * 4))
  switch (type) {
  case E_SETMAPMODE:
    NEED(4) m->mode = I32(0) >= 1 && I32(0) <= 8 ? I32(0) : 1;
    if (m->mode < 7)
      m->wext = m->vext = (SIZE){1, 1};
    break;
  case E_SETWINDOWEXTEX:
    NEED(8) if (m->mode >= 7 && I32(0) && I32(1)) m->wext = (SIZE){I32(0), I32(1)};
    break;
  case E_SETWINDOWORGEX:
    NEED(8) m->worg = (POINT){I32(0), I32(1)};
    break;
  case E_SETVIEWPORTEXTEX:
    NEED(8) if (m->mode >= 7 && I32(0) && I32(1)) m->vext = (SIZE){I32(0), I32(1)};
    break;
  case E_SETVIEWPORTORGEX:
    NEED(8) m->vorg = (POINT){I32(0), I32(1)};
    break;
  case E_SETBKMODE:
    NEED(4) SetBkMode(dc, I32(0));
    break;
  case E_SETPOLYFILLMODE:
    NEED(4) SetPolyFillMode(dc, I32(0));
    break;
  case E_SETROP2:
    NEED(4) SetROP2(dc, I32(0));
    break;
  case E_SETSTRETCHBLTMODE:
    NEED(4) SetStretchBltMode(dc, I32(0));
    break;
  case E_SETTEXTALIGN:
    NEED(4) SetTextAlign(dc, (unsigned)I32(0));
    break;
  case E_SETTEXTCOLOR:
    NEED(4) SetTextColor(dc, (COLORREF)I32(0));
    break;
  case E_SETBKCOLOR:
    NEED(4) SetBkColor(dc, (COLORREF)I32(0));
    break;
  case E_SAVEDC:
    SaveDC(dc);
    if (*depth < 16)
      saved[(*depth)++] = *m;
    break;
  case E_RESTOREDC:
    NEED(4) RestoreDC(dc, I32(0));
    if (I32(0) < 0 && (unsigned)-I32(0) <= *depth) {
      *depth -= (unsigned)-I32(0);
      *m = saved[*depth];
    }
    break;
  case E_MOVETOEX: {
    NEED(8) POINT q = emf_point(m, I32(0), I32(1));
    MoveToEx(dc, q.x, q.y, 0);
    break;
  }
  case E_LINETO: {
    NEED(8) POINT q = emf_point(m, I32(0), I32(1));
    LineTo(dc, q.x, q.y);
    break;
  }
  case E_SETPIXELV: {
    NEED(12) POINT q = emf_point(m, I32(0), I32(1));
    SetPixelV(dc, q.x, q.y, (COLORREF)I32(2));
    break;
  }
  case E_RECTANGLE:
  case E_ELLIPSE:
  case E_INTERSECTCLIPRECT:
  case E_EXCLUDECLIPRECT: {
    NEED(16) RECT q = emf_rect(m, p);
    if (type == E_RECTANGLE)
      Rectangle(dc, q.left, q.top, q.right, q.bottom);
    else if (type == E_ELLIPSE)
      Ellipse(dc, q.left, q.top, q.right, q.bottom);
    else if (type == E_INTERSECTCLIPRECT)
      IntersectClipRect(dc, q.left, q.top, q.right, q.bottom);
    else
      ExcludeClipRect(dc, q.left, q.top, q.right, q.bottom);
    break;
  }
  case E_ROUNDRECT: {
    NEED(24) RECT q = emf_rect(m, p);
    RoundRect(dc, q.left, q.top, q.right, q.bottom,
              emf_length(m, I32(4), 0), emf_length(m, I32(5), 1));
    break;
  }
  case E_POLYGON:
  case E_POLYLINE:
  case E_POLYGON16:
  case E_POLYLINE16: {
    BOOL small = type == E_POLYGON16 || type == E_POLYLINE16;
    NEED(20) DWORD k = (DWORD)I32(4), each = small ? 4 : 8;
    if (!k || k > 65536 || 20 + (uint64_t)k * each > c)
      return;
    POINT *pts = gdi_alloc(k * sizeof(POINT));
    if (!pts)
      return;
    for (DWORD i = 0; i < k; i++) {
      const uint8_t *q = p + 20 + i * each;
      pts[i] = small ? emf_point(m, (int16_t)w32_u16(q), (int16_t)w32_u16(q + 2))
                     : emf_point(m, (LONG)w32_u32(q), (LONG)w32_u32(q + 4));
    }
    if (type == E_POLYGON || type == E_POLYGON16)
      Polygon(dc, pts, (int)k);
    else
      Polyline(dc, pts, (int)k);
    gdi_free(pts, k * sizeof(POINT));
    break;
  }
  case E_POLYPOLYGON:
  case E_POLYPOLYGON16: {
    BOOL small = type == E_POLYPOLYGON16;
    NEED(24) DWORD polys = (DWORD)I32(4), total = (DWORD)I32(5),
                   each = small ? 4 : 8;
    if (!polys || polys > 4096 || total > 65536 ||
        24 + (uint64_t)polys * 4 + (uint64_t)total * each > c)
      return;
    POINT *pts = gdi_alloc(total * sizeof(POINT) + 4);
    int *counts = gdi_alloc(polys * sizeof(int));
    DWORD sum = 0;
    for (DWORD i = 0; counts && i < polys; i++) {
      counts[i] = (int)w32_u32(p + 24 + i * 4);
      sum += (DWORD)counts[i];
    }
    if (pts && counts && sum == total) {
      const uint8_t *q0 = p + 24 + polys * 4;
      for (DWORD i = 0; i < total; i++) {
        const uint8_t *q = q0 + i * each;
        pts[i] = small ? emf_point(m, (int16_t)w32_u16(q), (int16_t)w32_u16(q + 2))
                       : emf_point(m, (LONG)w32_u32(q), (LONG)w32_u32(q + 4));
      }
      PolyPolygon(dc, pts, counts, (int)polys);
    }
    gdi_free(pts, total * sizeof(POINT) + 4);
    gdi_free(counts, polys * sizeof(int));
    break;
  }
  case E_CREATEPEN: { /* ihPen, LOGPEN (style, width.x, width.y, colour) */
    NEED(20) DWORD i = (DWORD)I32(0);
    HGDIOBJ pen = CreatePen(I32(1), I32(2) ? emf_length(m, I32(2), 0) : 0,
                            (COLORREF)I32(4));
    if (i < 256) {
      while (o->count <= i)
        o->h[o->count++] = 0;
      if (o->h[i])
        DeleteObject(o->h[i]);
      o->h[i] = pen;
    } else if (pen)
      DeleteObject(pen);
    break;
  }
  case E_CREATEBRUSHINDIRECT: { /* ihBrush, LOGBRUSH32 */
    NEED(16) DWORD i = (DWORD)I32(0);
    LOGBRUSH b = {(DWORD)I32(1), (COLORREF)I32(2), (uintptr_t)(DWORD)I32(3)};
    if (b.lbStyle > 2)
      b.lbStyle = 0;
    HGDIOBJ brush = CreateBrushIndirect(&b);
    if (i < 256) {
      while (o->count <= i)
        o->h[o->count++] = 0;
      if (o->h[i])
        DeleteObject(o->h[i]);
      o->h[i] = brush;
    } else if (brush)
      DeleteObject(brush);
    break;
  }
  case E_EXTCREATEFONTINDIRECTW: { /* ihFont, LOGFONTW (92 bytes) */
    NEED(96) DWORD i = (DWORD)I32(0);
    LOGFONTW lf;
    w32_copy(&lf, p + 4, sizeof(lf));
    lf.lfHeight = lf.lfHeight < 0 ? -emf_length(m, -lf.lfHeight, 1)
                                  : emf_length(m, lf.lfHeight, 1);
    lf.lfWidth = emf_length(m, lf.lfWidth, 0);
    HGDIOBJ font = CreateFontIndirectW(&lf);
    if (i < 256) {
      while (o->count <= i)
        o->h[o->count++] = 0;
      if (o->h[i])
        DeleteObject(o->h[i]);
      o->h[i] = font;
    } else if (font)
      DeleteObject(font);
    break;
  }
  case E_SELECTOBJECT: {
    NEED(4) DWORD i = (DWORD)I32(0);
    if (i & 0x80000000u) /* stock object */
      SelectObject(dc, GetStockObject((int)(i & 0x7fffffffu)));
    else if (i < o->count && o->h[i])
      SelectObject(dc, o->h[i]);
    break;
  }
  case E_DELETEOBJECT: {
    NEED(4) DWORD i = (DWORD)I32(0);
    if (i < o->count && o->h[i]) {
      for (unsigned k = 0; k < 3; k++)
        if (GetCurrentObject(dc, k == 0 ? GDI_PEN : k == 1 ? GDI_BRUSH
                                                            : GDI_FONT) == o->h[i])
          SelectObject(dc, GetStockObject(k == 0 ? 7 : k == 1 ? 0 : 13));
      DeleteObject(o->h[i]);
      o->h[i] = 0;
    }
    break;
  }
  case E_EXTTEXTOUTA:
  case E_EXTTEXTOUTW: {
    /* rclBounds, iGraphicsMode, exScale, eyScale (floats, unused), then
     * EMRTEXT: reference point, count, string offset, options, rectangle,
     * dx offset. Offsets are from the record start. */
    NEED(68) POINT q = emf_point(m, (LONG)w32_u32(p + 28), (LONG)w32_u32(p + 32));
    DWORD count = w32_u32(p + 36), off = w32_u32(p + 40),
          options = w32_u32(p + 44), dx_off = w32_u32(p + 64);
    BOOL wide = type == E_EXTTEXTOUTW;
    if (count > 8192 || off > n || (uint64_t)count * (wide ? 2 : 1) > n - off)
      return;
    RECT box = emf_rect(m, p + 48);
    int *dx = 0;
    if (dx_off && dx_off <= n && (uint64_t)count * 4 <= n - dx_off && count) {
      dx = gdi_alloc(count * sizeof(int));
      for (DWORD i = 0; dx && i < count; i++)
        dx[i] = (int)emf_length(m, (LONG)w32_u32(r + dx_off + i * 4), 0);
    }
    unsigned flags = options & (ETO_OPAQUE | ETO_CLIPPED);
    if (wide)
      ExtTextOutW(dc, q.x, q.y, flags, flags ? &box : 0,
                  (const WCHAR *)(r + off), count, dx);
    else
      ExtTextOutA(dc, q.x, q.y, flags, flags ? &box : 0,
                  (const char *)(r + off), count, dx);
    if (dx)
      gdi_free(dx, count * sizeof(int));
    break;
  }
  case E_STRETCHDIBITS: { /* bounds, xDest yDest xSrc ySrc cxSrc cySrc
                            offBmi cbBmi offBits cbBits iUsage rop cxDest
                            cyDest */
    NEED(72) DWORD obmi = w32_u32(p + 40), cbmi = w32_u32(p + 44),
                   obits = w32_u32(p + 48), cbits = w32_u32(p + 52);
    if (obmi > n || cbmi > n - obmi || obits > n || cbits > n - obits ||
        cbmi < 40)
      return;
    POINT a = emf_point(m, (LONG)w32_u32(p + 16), (LONG)w32_u32(p + 20)),
          b = emf_point(m, (LONG)w32_u32(p + 16) + (LONG)w32_u32(p + 64),
                        (LONG)w32_u32(p + 20) + (LONG)w32_u32(p + 68));
    StretchDIBits(dc, a.x, a.y, b.x - a.x, b.y - a.y, (int)w32_u32(p + 24),
                  (int)w32_u32(p + 28), (int)w32_u32(p + 32),
                  (int)w32_u32(p + 36), r + obits,
                  (const BITMAPINFO *)(r + obmi), w32_u32(p + 56),
                  w32_u32(p + 60));
    break;
  }
  case E_BITBLT:
  case E_STRETCHBLT: { /* bounds, xDest yDest cxDest cyDest rop xSrc ySrc
                         xform(24) bkColor usage offBmi cbBmi offBits
                         cbBits [cxSrc cySrc] */
    NEED(92) DWORD rop = w32_u32(p + 32), obmi = w32_u32(p + 76),
                   cbmi = w32_u32(p + 80), obits = w32_u32(p + 84),
                   cbits = w32_u32(p + 88);
    LONG x = (LONG)w32_u32(p + 16), y = (LONG)w32_u32(p + 20),
         w = (LONG)w32_u32(p + 24), h = (LONG)w32_u32(p + 28);
    POINT a = emf_point(m, x, y), b = emf_point(m, x + w, y + h);
    if (!cbmi) {
      PatBlt(dc, a.x, a.y, b.x - a.x, b.y - a.y, rop);
      break;
    }
    if (obmi > n || cbmi > n - obmi || obits > n || cbits > n - obits ||
        cbmi < 40)
      return;
    const BITMAPINFOHEADER *bi = (const BITMAPINFOHEADER *)(r + obmi);
    int sw = type == E_STRETCHBLT && c >= 100 ? (int)w32_u32(p + 92) : (int)w,
        sh = type == E_STRETCHBLT && c >= 100 ? (int)w32_u32(p + 96) : (int)h;
    (void)bi;
    StretchDIBits(dc, a.x, a.y, b.x - a.x, b.y - a.y, (int)w32_u32(p + 36),
                  (int)w32_u32(p + 40), sw, sh, r + obits,
                  (const BITMAPINFO *)(r + obmi), w32_u32(p + 72), rop);
    break;
  }
  default: /* header, comments, unsupported records */
    break;
  }
#undef NEED
#undef I32
}
/* Reference pixels of the frame (.01 mm) from the header. */
static void emf_frame(const uint8_t *h, struct emf_map *m) {
  int64_t dx = (LONG)w32_u32(h + 72), dy = (LONG)w32_u32(h + 76),
          mx = (LONG)w32_u32(h + 80), my = (LONG)w32_u32(h + 84);
  if (dx <= 0 || dy <= 0 || mx <= 0 || my <= 0)
    dx = dy = 96, mx = my = 25; /* 96 dpi */
  m->dpmx = dx * 100000 / mx; /* pixels per 100 m */
  m->dpmy = dy * 100000 / my;
  LONG l = (LONG)w32_u32(h + 24), t = (LONG)w32_u32(h + 28),
       r = (LONG)w32_u32(h + 32), b = (LONG)w32_u32(h + 36);
  m->fx = div_round((int64_t)l * dx, mx * 100);
  m->fy = div_round((int64_t)t * dy, my * 100);
  m->fw = div_round((int64_t)(r - l) * dx, mx * 100);
  m->fh = div_round((int64_t)(b - t) * dy, my * 100);
  if (!m->fw || !m->fh) { /* an empty frame: use the bounds (pixels) */
    m->fx = (LONG)w32_u32(h + 8);
    m->fy = (LONG)w32_u32(h + 12);
    m->fw = (LONG)w32_u32(h + 16) - m->fx + 1;
    m->fh = (LONG)w32_u32(h + 20) - m->fy + 1;
  }
}
static BOOL play_emf(HDC dc, const uint8_t *bits, DWORD size, const RECT *rect) {
  if (!gdi_dc(dc) || !rect || !valid_emf(bits, size))
    return 0;
  struct emf_map m;
  w32_fill(&m, 0, sizeof(m));
  emf_reset(&m);
  emf_frame(bits, &m);
  m.rx = rect->left;
  m.ry = rect->top;
  m.rw = rect->right - rect->left;
  m.rh = rect->bottom - rect->top;
  struct objects *o = gdi_alloc(sizeof(*o));
  struct emf_map *saved = gdi_alloc(16 * sizeof(*saved));
  if (!o || !saved) {
    gdi_free(o, sizeof(*o));
    gdi_free(saved, 16 * sizeof(*saved));
    return 0;
  }
  unsigned depth = 0;
  HGDIOBJ pen = GetCurrentObject(dc, GDI_PEN),
          brush = GetCurrentObject(dc, GDI_BRUSH),
          font = GetCurrentObject(dc, GDI_FONT);
  int level = SaveDC(dc);
  DWORD at = 0, end = w32_u32(bits + 48);
  while (at + 8 <= end) {
    DWORD n = w32_u32(bits + at + 4);
    if (w32_u32(bits + at) == E_EOF)
      break;
    play_emf_record(dc, &m, o, bits + at, n, saved, &depth);
    at += n;
  }
  RestoreDC(dc, level);
  objects_free(dc, o, pen, brush, font);
  gdi_free(o, sizeof(*o));
  gdi_free(saved, 16 * sizeof(*saved));
  return 1;
}
BOOL W32API PlayEnhMetaFile(HDC dc, HGDIOBJ h, const RECT *rect) {
  GDI_GUARD;
  struct object *o = meta_of(h, GDI_ENHMETAFILE);
  if (o && !rect)
    SetLastError(87);
  return o && play_emf(dc, o->u.meta.bits, o->u.meta.size, rect);
}

/* ---- EMF -> WMF (GetWinMetaFileBits) --------------------------------------- */
/* The WMF carries the EMF in MFCOMMENT escapes ([MS-WMF]
 * META_ESCAPE_ENHANCED_METAFILE: 'WMFC', type 1, version 0x10000, checksum,
 * flags, record count, this record's and the remaining sizes, total size),
 * which SetWinMetaFileBits uses to restore it exactly, followed by the
 * records converted for WMF players, in reference pixels. */
#define CHUNK 8192u
static void wmf_record(struct buffer *b, DWORD *largest, unsigned f,
                       const int16_t *v, unsigned count, const void *tail,
                       DWORD tail_bytes) {
  DWORD words = 3 + count + (tail_bytes + 1) / 2;
  put32(b, words);
  put16(b, f);
  for (unsigned i = 0; i < count; i++)
    put16(b, (uint16_t)v[i]);
  put_bytes(b, tail, tail_bytes);
  if (tail_bytes & 1)
    put_bytes(b, "", 1);
  if (words > *largest)
    *largest = words;
}
struct convert {
  struct buffer *b;
  DWORD *largest;
  struct emf_map m;
};
static void p16(const struct convert *k, LONG x, LONG y, int16_t *ox,
                int16_t *oy) {
  int64_t rx, ry;
  emf_ref(&k->m, x, y, &rx, &ry);
  *ox = clamp16((LONG)rx);
  *oy = clamp16((LONG)ry);
}
static void convert_emf_record(struct convert *k, const uint8_t *r, DWORD n) {
  DWORD type = w32_u32(r);
  const uint8_t *p = r + 8;
  DWORD c = n - 8;
  int16_t v[16];
#define I32(i) ((LONG)w32_u32(p + (i) * 4))
  switch (type) {
  case E_SETMAPMODE:
    if (c >= 4)
      k->m.mode = I32(0) >= 1 && I32(0) <= 8 ? I32(0) : 1;
    break;
  case E_SETWINDOWEXTEX:
    if (c >= 8 && k->m.mode >= 7 && I32(0) && I32(1))
      k->m.wext = (SIZE){I32(0), I32(1)};
    break;
  case E_SETWINDOWORGEX:
    if (c >= 8)
      k->m.worg = (POINT){I32(0), I32(1)};
    break;
  case E_SETVIEWPORTEXTEX:
    if (c >= 8 && k->m.mode >= 7 && I32(0) && I32(1))
      k->m.vext = (SIZE){I32(0), I32(1)};
    break;
  case E_SETVIEWPORTORGEX:
    if (c >= 8)
      k->m.vorg = (POINT){I32(0), I32(1)};
    break;
  case E_SETBKMODE:
  case E_SETPOLYFILLMODE:
  case E_SETROP2:
  case E_SETSTRETCHBLTMODE:
  case E_SETTEXTALIGN:
    if (c >= 4) {
      v[0] = (int16_t)I32(0);
      wmf_record(k->b, k->largest,
                 type == E_SETBKMODE          ? M_SETBKMODE
                 : type == E_SETPOLYFILLMODE  ? M_SETPOLYFILLMODE
                 : type == E_SETROP2          ? M_SETROP2
                 : type == E_SETSTRETCHBLTMODE ? M_SETSTRETCHBLTMODE
                                               : M_SETTEXTALIGN,
                 v, 1, 0, 0);
    }
    break;
  case E_SETTEXTCOLOR:
  case E_SETBKCOLOR:
    if (c >= 4) {
      v[0] = (int16_t)(I32(0) & 0xffff);
      v[1] = (int16_t)((DWORD)I32(0) >> 16);
      wmf_record(k->b, k->largest,
                 type == E_SETTEXTCOLOR ? M_SETTEXTCOLOR : M_SETBKCOLOR, v, 2,
                 0, 0);
    }
    break;
  case E_SAVEDC:
    wmf_record(k->b, k->largest, M_SAVEDC, 0, 0, 0, 0);
    break;
  case E_RESTOREDC:
    if (c >= 4) {
      v[0] = (int16_t)I32(0);
      wmf_record(k->b, k->largest, M_RESTOREDC, v, 1, 0, 0);
    }
    break;
  case E_MOVETOEX:
  case E_LINETO:
    if (c >= 8) {
      p16(k, I32(0), I32(1), &v[1], &v[0]);
      wmf_record(k->b, k->largest, type == E_MOVETOEX ? M_MOVETO : M_LINETO,
                 v, 2, 0, 0);
    }
    break;
  case E_RECTANGLE:
  case E_ELLIPSE:
  case E_ROUNDRECT:
    if (c >= 16) {
      unsigned base = type == E_ROUNDRECT ? 2 : 0;
      p16(k, I32(0), I32(1), &v[base + 3], &v[base + 2]);
      p16(k, I32(2), I32(3), &v[base + 1], &v[base + 0]);
      if (type == E_ROUNDRECT && c >= 24) {
        int16_t x0, y0, x1, y1;
        p16(k, 0, 0, &x0, &y0);
        p16(k, I32(4), I32(5), &x1, &y1);
        v[1] = (int16_t)(x1 - x0 < 0 ? x0 - x1 : x1 - x0);
        v[0] = (int16_t)(y1 - y0 < 0 ? y0 - y1 : y1 - y0);
      }
      wmf_record(k->b, k->largest,
                 type == E_RECTANGLE ? M_RECTANGLE
                 : type == E_ELLIPSE ? M_ELLIPSE
                                     : M_ROUNDRECT,
                 v, type == E_ROUNDRECT ? 6 : 4, 0, 0);
    }
    break;
  case E_POLYGON:
  case E_POLYLINE:
  case E_POLYGON16:
  case E_POLYLINE16: {
    BOOL small = type == E_POLYGON16 || type == E_POLYLINE16;
    DWORD each = small ? 4 : 8;
    if (c < 20)
      break;
    DWORD count = (DWORD)I32(4);
    if (!count || count > 16000 || 20 + (uint64_t)count * each > c)
      break;
    int16_t *w = gdi_alloc((1 + count * 2) * 2);
    if (!w)
      break;
    w[0] = (int16_t)count;
    for (DWORD i = 0; i < count; i++) {
      const uint8_t *q = p + 20 + i * each;
      LONG x = small ? (int16_t)w32_u16(q) : (LONG)w32_u32(q),
           y = small ? (int16_t)w32_u16(q + 2) : (LONG)w32_u32(q + 4);
      p16(k, x, y, &w[1 + i * 2], &w[2 + i * 2]);
    }
    wmf_record(k->b, k->largest,
               type == E_POLYGON || type == E_POLYGON16 ? M_POLYGON
                                                        : M_POLYLINE,
               w, 1 + count * 2, 0, 0);
    gdi_free(w, (1 + count * 2) * 2);
    break;
  }
  case E_SETPIXELV:
    if (c >= 12) {
      v[0] = (int16_t)(I32(2) & 0xffff);
      v[1] = (int16_t)((DWORD)I32(2) >> 16);
      p16(k, I32(0), I32(1), &v[3], &v[2]);
      wmf_record(k->b, k->largest, M_SETPIXEL, v, 4, 0, 0);
    }
    break;
  default: /* objects, text and bitmaps stay in the embedded EMF */
    break;
  }
#undef I32
}
static DWORD checksum_emf(const uint8_t *p, DWORD n) {
  DWORD sum = 0;
  for (DWORD i = 0; i + 4 <= n; i += 4)
    sum += w32_u32(p + i);
  return sum;
}
DWORD W32API GetWinMetaFileBits(HGDIOBJ h, DWORD cap, uint8_t *out, int mode,
                                HDC ref) {
  GDI_GUARD;
  (void)ref;
  struct object *o = meta_of(h, GDI_ENHMETAFILE);
  if (!o)
    return 0;
  if (mode < 1 || mode > 8) {
    SetLastError(87);
    return 0;
  }
  const uint8_t *emf = o->u.meta.bits;
  DWORD size = o->u.meta.size;
  struct buffer b = {0, 0, 0, 0};
  DWORD largest = 0;
  put16(&b, 1);
  put16(&b, 9);
  put16(&b, 0x300);
  put32(&b, 0); /* total words, patched below */
  put16(&b, 0);
  put32(&b, 0); /* largest record, patched below */
  put16(&b, 0);
  /* Embedded EMF chunks. */
  DWORD chunks = (size + CHUNK - 1) / CHUNK, sum = checksum_emf(emf, size);
  for (DWORD i = 0, at = 0; i < chunks; i++, at += CHUNK) {
    DWORD part = size - at < CHUNK ? size - at : CHUNK;
    uint8_t head[34];
    w32_put32(head, WMFC);
    w32_put32(head + 4, 1);
    w32_put32(head + 8, 0x10000);
    w32_put16(head + 12, (uint16_t)(0u - (sum & 0xffff) - (sum >> 16)));
    w32_put32(head + 14, 0);
    w32_put32(head + 18, chunks);
    w32_put32(head + 22, part);
    w32_put32(head + 26, size - at - part);
    w32_put32(head + 30, size);
    int16_t v[2] = {15, (int16_t)(34 + part)};
    uint8_t *data = gdi_alloc(34 + part);
    if (!data) {
      b.failed = 1;
      break;
    }
    w32_copy(data, head, 34);
    w32_copy(data + 34, emf + at, part);
    wmf_record(&b, &largest, M_ESCAPE, v, 2, data, 34 + part);
    gdi_free(data, 34 + part);
  }
  /* Converted records in reference pixels. */
  struct convert k = {&b, &largest, {0}};
  emf_reset(&k.m);
  emf_frame(emf, &k.m);
  int16_t v[2] = {(int16_t)mode};
  wmf_record(&b, &largest, M_SETMAPMODE, v, 1, 0, 0);
  if (mode >= 7) {
    v[0] = clamp16((LONG)k.m.fy);
    v[1] = clamp16((LONG)k.m.fx);
    wmf_record(&b, &largest, M_SETWINDOWORG, v, 2, 0, 0);
    v[0] = clamp16((LONG)k.m.fh);
    v[1] = clamp16((LONG)k.m.fw);
    wmf_record(&b, &largest, M_SETWINDOWEXT, v, 2, 0, 0);
  }
  for (DWORD at = 0; at + 8 <= w32_u32(emf + 48);) {
    DWORD n = w32_u32(emf + at + 4);
    if (w32_u32(emf + at) == E_EOF)
      break;
    convert_emf_record(&k, emf + at, n);
    at += n;
  }
  wmf_record(&b, &largest, M_EOF, 0, 0, 0, 0);
  DWORD total = 0;
  if (!b.failed) {
    w32_put32(b.p + 6, b.n / 2);
    w32_put32(b.p + 12, largest);
    total = b.n;
    if (out) {
      if (cap < b.n) {
        SetLastError(122);
        total = 0;
      } else
        w32_copy(out, b.p, b.n);
    }
  } else
    SetLastError(8);
  release(&b);
  return total;
}

/* ---- WMF -> EMF (SetWinMetaFileBits) --------------------------------------- */
/* An embedded EMF is restored as it was. Otherwise the WMF's records become
 * EMF records (16-bit to 32-bit) after a mapping prologue: for a
 * METAFILEPICT in MM_ANISOTROPIC/MM_ISOTROPIC the picture's extent (.01 mm)
 * is the frame; otherwise the WMF window extent in reference pixels. */
typedef struct {
  LONG mm, xExt, yExt;
  uint32_t hMF;
} METAFILEPICT;
static void emf_record(struct buffer *b, DWORD *records, DWORD type,
                       const DWORD *v, unsigned count) {
  put32(b, type);
  put32(b, 8 + count * 4);
  for (unsigned i = 0; i < count; i++)
    put32(b, v[i]);
  (*records)++;
}
static HGDIOBJ embedded_emf(const uint8_t *wmf) {
  DWORD at = 18, end = w32_u32(wmf + 6) * 2, total = 0, got = 0;
  uint8_t *emf = 0;
  while (at + 6 <= end) {
    DWORD n = w32_u32(wmf + at) * 2;
    unsigned f = w32_u16(wmf + at + 4);
    if (!f)
      break;
    const uint8_t *p = wmf + at + 6;
    if (f == M_ESCAPE && n >= 6 + 4 + 34 && w32_u16(p) == 15 &&
        w32_u32(p + 4) == WMFC && w32_u32(p + 8) == 1) {
      DWORD part = w32_u32(p + 26), size = w32_u32(p + 34);
      if (!emf) {
        if (size < 88 || size > WMF_LIMIT)
          return 0;
        emf = gdi_alloc(size);
        if (!emf)
          return 0;
        total = size;
      }
      if (size != total || part > total - got || 4 + 34 + part > n - 6) {
        gdi_free(emf, total);
        return 0;
      }
      w32_copy(emf + got, p + 4 + 34, part);
      got += part;
    }
    at += n;
  }
  HGDIOBJ h = 0;
  if (emf && got == total && valid_emf(emf, total))
    h = meta_new(GDI_ENHMETAFILE, emf, total);
  gdi_free(emf, total);
  return h;
}
HGDIOBJ W32API SetWinMetaFileBits(DWORD size, const uint8_t *wmf, HDC ref,
                                  const METAFILEPICT *pict) {
  GDI_GUARD;
  (void)ref;
  if (!valid_wmf(wmf, size)) {
    SetLastError(87);
    return 0;
  }
  HGDIOBJ restored = embedded_emf(wmf);
  if (restored)
    return restored;
  /* Reference device: 96 dpi. */
  LONG fw = 0, fh = 0; /* frame in .01 mm */
  DWORD at = 18, end = w32_u32(wmf + 6) * 2;
  LONG wx = 0, wy = 0;
  while (at + 6 <= end) {
    DWORD n = w32_u32(wmf + at) * 2;
    unsigned f = w32_u16(wmf + at + 4);
    if (!f)
      break;
    if (f == M_SETWINDOWEXT && n >= 10) {
      wy = (int16_t)w32_u16(wmf + at + 6);
      wx = (int16_t)w32_u16(wmf + at + 8);
    }
    at += n;
  }
  if (pict && (pict->mm == 7 || pict->mm == 8) && pict->xExt > 0 &&
      pict->yExt > 0) {
    fw = pict->xExt;
    fh = pict->yExt;
  } else {
    fw = (wx < 0 ? -wx : wx) * 2540 / 96;
    fh = (wy < 0 ? -wy : wy) * 2540 / 96;
  }
  if (fw <= 0)
    fw = 2540;
  if (fh <= 0)
    fh = 2540;
  LONG pw = fw * 96 / 2540, ph = fh * 96 / 2540;
  struct buffer b = {0, 0, 0, 0};
  DWORD records = 0;
  /* Header (88 bytes): type, size, bounds, frame, signature, version,
   * bytes, records, handles/reserved, description, palette, device px/mm. */
  DWORD header[22] = {E_HEADER, 88, 0, 0, (DWORD)(pw - 1), (DWORD)(ph - 1),
                      0, 0, (DWORD)fw, (DWORD)fh, EMF_SIGNATURE, 0x10000,
                      0, 0, 256, 0, 0, 0, 1024, 768, 270, 203};
  for (unsigned i = 0; i < 22; i++)
    put32(&b, header[i]);
  records = 1;
  /* The picture fills the frame: MM_ANISOTROPIC with a viewport of the
   * frame in reference pixels; the WMF supplies the window. */
  DWORD v[8];
  v[0] = 8;
  emf_record(&b, &records, E_SETMAPMODE, v, 1);
  v[0] = (DWORD)pw;
  v[1] = (DWORD)ph;
  emf_record(&b, &records, E_SETVIEWPORTEXTEX, v, 2);
  at = 18;
  while (at + 6 <= end) {
    DWORD n = w32_u32(wmf + at) * 2, words = (n - 6) / 2;
    unsigned f = w32_u16(wmf + at + 4);
    const uint8_t *p = wmf + at + 6;
    if (!f)
      break;
#define S(i) ((DWORD)(LONG)s16(p, i))
    switch (f) {
    case M_SETWINDOWORG:
    case M_SETWINDOWEXT:
      if (words >= 2) {
        v[0] = S(1);
        v[1] = S(0);
        emf_record(&b, &records,
                   f == M_SETWINDOWORG ? E_SETWINDOWORGEX : E_SETWINDOWEXTEX,
                   v, 2);
      }
      break;
    case M_SETBKMODE:
    case M_SETPOLYFILLMODE:
    case M_SETROP2:
    case M_SETSTRETCHBLTMODE:
    case M_SETTEXTALIGN:
      if (words >= 1) {
        v[0] = (DWORD)(uint16_t)s16(p, 0);
        emf_record(&b, &records,
                   f == M_SETBKMODE          ? E_SETBKMODE
                   : f == M_SETPOLYFILLMODE  ? E_SETPOLYFILLMODE
                   : f == M_SETROP2          ? E_SETROP2
                   : f == M_SETSTRETCHBLTMODE ? E_SETSTRETCHBLTMODE
                                              : E_SETTEXTALIGN,
                   v, 1);
      }
      break;
    case M_SETTEXTCOLOR:
    case M_SETBKCOLOR:
      if (words >= 2) {
        v[0] = color_at(p, 0);
        emf_record(&b, &records,
                   f == M_SETTEXTCOLOR ? E_SETTEXTCOLOR : E_SETBKCOLOR, v, 1);
      }
      break;
    case M_SAVEDC:
      emf_record(&b, &records, E_SAVEDC, v, 0);
      break;
    case M_RESTOREDC:
      if (words >= 1) {
        v[0] = S(0);
        emf_record(&b, &records, E_RESTOREDC, v, 1);
      }
      break;
    case M_MOVETO:
    case M_LINETO:
      if (words >= 2) {
        v[0] = S(1);
        v[1] = S(0);
        emf_record(&b, &records, f == M_MOVETO ? E_MOVETOEX : E_LINETO, v, 2);
      }
      break;
    case M_RECTANGLE:
    case M_ELLIPSE:
      if (words >= 4) {
        v[0] = S(3);
        v[1] = S(2);
        v[2] = S(1);
        v[3] = S(0);
        emf_record(&b, &records, f == M_RECTANGLE ? E_RECTANGLE : E_ELLIPSE,
                   v, 4);
      }
      break;
    case M_ROUNDRECT:
      if (words >= 6) {
        v[0] = S(5);
        v[1] = S(4);
        v[2] = S(3);
        v[3] = S(2);
        v[4] = S(1);
        v[5] = S(0);
        emf_record(&b, &records, E_ROUNDRECT, v, 6);
      }
      break;
    case M_POLYGON:
    case M_POLYLINE:
      if (words >= 1 && s16(p, 0) > 0 &&
          words >= 1 + (unsigned)s16(p, 0) * 2) {
        DWORD k = (DWORD)s16(p, 0);
        put32(&b, f == M_POLYGON ? E_POLYGON16 : E_POLYLINE16);
        put32(&b, 28 + k * 4);
        for (unsigned i = 0; i < 4; i++)
          put32(&b, 0); /* bounds */
        put32(&b, k);
        for (DWORD i = 0; i < k; i++) {
          put16(&b, (uint16_t)s16(p, 1 + i * 2));
          put16(&b, (uint16_t)s16(p, 2 + i * 2));
        }
        records++;
      }
      break;
    default: /* objects, text, bitmaps: limited conversion */
      break;
    }
#undef S
    at += n;
  }
  v[0] = 0;
  v[1] = 0x10;
  v[2] = 20;
  emf_record(&b, &records, E_EOF, v, 3);
  HGDIOBJ h = 0;
  if (!b.failed) {
    w32_put32(b.p + 48, b.n);
    w32_put32(b.p + 52, records);
    h = meta_new(GDI_ENHMETAFILE, b.p, b.n);
  } else
    SetLastError(8);
  release(&b);
  return h;
}
