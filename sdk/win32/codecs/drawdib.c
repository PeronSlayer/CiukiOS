/* SPDX-License-Identifier: LGPL-2.1-or-later
 * msvfw32 DrawDib (f5-12B): DrawDibOpen/Begin/Draw/End/Close and the
 * informational entry points. Compressed sources are decoded with the
 * installed ICM decompressors (ICLocate/ICDecompress, f5-14C); every image
 * reaches the device through gdi32 StretchDIBits (f5-10B) with
 * DIB_RGB_COLORS and SRCCOPY. No dithering, halftoning or palette
 * realization is performed (DrawDibRealize realizes nothing). */
#include "codec.h"
#ifndef W32_HOST_MODEL
#define IMPORT __declspec(dllimport)
#else
#define IMPORT
#endif
IMPORT void *W32API GetProcessHeap(void);
IMPORT void *W32API HeapAlloc(void *, uint32_t, uint32_t);
IMPORT int W32API HeapFree(void *, uint32_t, void *);
/* gdi32 is bound at first use (GetModuleHandle/LoadLibrary +
 * GetProcAddress) so that the codec images keep importing kernel32 only:
 * loading msvfw32/avifil32 for decoding never loads the GDI. */
typedef int(W32API *stretch_fn)(uint32_t, int, int, int, int, int, int, int,
                                int, const void *, const void *, uint32_t,
                                uint32_t);
typedef int(W32API *mode_fn)(uint32_t, int);
#ifdef W32_HOST_MODEL
int StretchDIBits(uint32_t, int, int, int, int, int, int, int, int,
                  const void *, const void *, uint32_t, uint32_t);
int SetStretchBltMode(uint32_t, int);
#else
IMPORT void *W32API GetModuleHandleA(const char *);
IMPORT void *W32API LoadLibraryA(const char *);
IMPORT void *W32API GetProcAddress(void *, const char *);
#endif
struct gdi_api {
  stretch_fn stretch;
  mode_fn mode;
};
static const struct gdi_api *gdi(void) {
#ifdef W32_HOST_MODEL
  static const struct gdi_api api = {StretchDIBits, SetStretchBltMode};
  return &api;
#else
  static struct gdi_api api;
  static volatile uint32_t state;
  if (!state) {
    void *m = GetModuleHandleA("gdi32.dll");
    if (!m)
      m = LoadLibraryA("gdi32.dll");
    api.stretch = m ? (stretch_fn)GetProcAddress(m, "StretchDIBits") : 0;
    api.mode = m ? (mode_fn)GetProcAddress(m, "SetStretchBltMode") : 0;
    state = api.stretch && api.mode ? 1 : 2;
  }
  return state == 1 ? &api : 0;
#endif
}
uintptr_t W32API ICLocate(uint32_t, uint32_t, struct cc_bitmap *,
                          struct cc_bitmap *, uint32_t);
intptr_t W32API ICSendMessage(uintptr_t, uint32_t, uintptr_t, uintptr_t);
int32_t W32API ICClose(uintptr_t);
int32_t ICDecompress(uintptr_t, uint32_t, struct cc_bitmap *, const void *,
                     struct cc_bitmap *, void *);

#define DDF_UPDATE 0x0002u
#define DDF_SAME_HDC 0x0004u
#define DDF_SAME_DRAW 0x0008u
#define DDF_DONTDRAW 0x0010u
#define DDF_NOTKEYFRAME 0x0400u
#define DDF_HURRYUP 0x0800u
#define DRAWDIBS 16u
struct drawdib {
  uint32_t used, begun;
  uint8_t source[40 + 1024]; /* BITMAPINFOHEADER + colour table */
  uint8_t image[40 + 1024];  /* format drawn (decoded or the source) */
  uintptr_t decoder;
  uint8_t *buffer;
  uint32_t buffer_bytes, have_frame;
  int32_t dst_w, dst_h, src_w, src_h;
  const void *last_bits;
};
static struct drawdib drawdibs[DRAWDIBS];
static struct drawdib *get(uintptr_t h) {
  if (!h || h > DRAWDIBS || !drawdibs[h - 1].used)
    return 0;
  return &drawdibs[h - 1];
}
static uint32_t colours(const struct cc_bitmap *b) {
  if (b->bits > 8)
    return 0;
  uint32_t n = b->colors ? b->colors : 1u << b->bits;
  return n > 256 ? 256 : n;
}
static uint32_t stride(const struct cc_bitmap *b) {
  return (((uint32_t)b->width * b->bits + 31) / 32) * 4;
}
static uint32_t rows(const struct cc_bitmap *b) {
  return (uint32_t)(b->height < 0 ? -(int64_t)b->height : b->height);
}
static int sane(const struct cc_bitmap *b) {
  return b && b->size >= 40 && b->width > 0 && b->width <= 4096 &&
         b->height != (-2147483647 - 1) && b->height && rows(b) <= 4096 &&
         b->planes == 1 &&
         (b->bits == 1 || b->bits == 4 || b->bits == 8 || b->bits == 16 ||
          b->bits == 24 || b->bits == 32);
}
uintptr_t W32API DrawDibOpen(void) {
  for (uint32_t i = 0; i < DRAWDIBS; i++)
    if (!drawdibs[i].used) {
      cc_zero(&drawdibs[i], sizeof(drawdibs[i]));
      drawdibs[i].used = 1;
      return i + 1;
    }
  return 0;
}
int W32API DrawDibEnd(uintptr_t h) {
  struct drawdib *d = get(h);
  if (!d)
    return 0;
  if (d->decoder) {
    ICSendMessage(d->decoder, 0x400e, 0, 0); /* ICM_DECOMPRESS_END */
    ICClose(d->decoder);
    d->decoder = 0;
  }
  if (d->buffer)
    HeapFree(GetProcessHeap(), 0, d->buffer);
  d->buffer = 0;
  d->buffer_bytes = d->have_frame = d->begun = 0;
  return 1;
}
int W32API DrawDibClose(uintptr_t h) {
  struct drawdib *d = get(h);
  if (!d)
    return 0;
  DrawDibEnd(h);
  d->used = 0;
  return 1;
}
int W32API DrawDibBegin(uintptr_t h, uintptr_t dc, int dx, int dy,
                        const struct cc_bitmap *b, int sx, int sy,
                        uint32_t flags) {
  struct drawdib *d = get(h);
  (void)dc;
  (void)flags;
  if (!d || !b || !sane(b))
    return 0;
  DrawDibEnd(h);
  uint32_t n = colours(b) * 4; /* the colour table follows biSize bytes */
  cc_zero(d->source, sizeof(d->source));
  cc_copy(d->source, b, 40);
  if (n)
    cc_copy(d->source + 40, (const uint8_t *)b + b->size, n);
  ((struct cc_bitmap *)d->source)->size = 40;
  struct cc_bitmap *src = (void *)d->source, *img = (void *)d->image;
  if (src->compression == 0 || src->compression == 3) {
    cc_copy(d->image, d->source, sizeof(d->image));
  } else {
    d->decoder = ICLocate(CC_FOUR('v', 'i', 'd', 'c'), 0, src, 0, 2);
    if (!d->decoder)
      return 0;
    cc_zero(d->image, sizeof(d->image));
    if (ICSendMessage(d->decoder, 0x400a, (uintptr_t)src, (uintptr_t)img) ||
        ICSendMessage(d->decoder, 0x400c, (uintptr_t)src, (uintptr_t)img)) {
      ICClose(d->decoder);
      d->decoder = 0;
      return 0;
    }
    if (img->bits <= 8) /* decoded indexed output keeps the source colours */
      cc_copy(d->image + 40, d->source + 40, 1024);
    img->colors = img->bits <= 8 ? colours(src) : 0;
    uint64_t need = (uint64_t)stride(img) * rows(img);
    if (need > 16u * 1024 * 1024) {
      DrawDibEnd(h);
      return 0;
    }
    d->buffer = HeapAlloc(GetProcessHeap(), 8, (uint32_t)need);
    if (!d->buffer) {
      DrawDibEnd(h);
      return 0;
    }
    d->buffer_bytes = (uint32_t)need;
  }
  d->src_w = sx > 0 ? sx : src->width;
  d->src_h = sy > 0 ? sy : (int32_t)rows(src);
  d->dst_w = dx > 0 ? dx : d->src_w;
  d->dst_h = dy > 0 ? dy : d->src_h;
  d->begun = 1;
  return 1;
}
int W32API DrawDibDraw(uintptr_t h, uintptr_t dc, int x, int y, int dx,
                       int dy, const struct cc_bitmap *b, const void *bits,
                       int sx, int sy, int sw, int sh, uint32_t flags) {
  struct drawdib *d = get(h);
  if (!d)
    return 0;
  if (!(flags & (DDF_SAME_DRAW | DDF_UPDATE)) || !d->begun) {
    if (!b)
      return 0;
    const struct cc_bitmap *cur = (const void *)d->source;
    if (!d->begun || cur->width != b->width || cur->height != b->height ||
        cur->bits != b->bits || cur->compression != b->compression) {
      if (!DrawDibBegin(h, dc, dx, dy, b, sw, sh, 0))
        return 0;
    } else if (colours(b) && b->size >= 40) {
      /* Same format: refresh the colour table, keep the decoder state
       * (delta frames depend on it). */
      uint32_t n = colours(b) * 4;
      cc_copy(d->source + 40, (const uint8_t *)b + b->size, n);
      if (((struct cc_bitmap *)d->image)->bits <= 8)
        cc_copy(d->image + 40, d->source + 40, n);
    }
  }
  struct cc_bitmap *img = (void *)d->image;
  const void *pixels = bits;
  if (flags & DDF_UPDATE) {
    pixels = d->decoder ? (d->have_frame ? d->buffer : 0) : d->last_bits;
    if (!pixels)
      return 0;
  } else if (d->decoder) {
    struct cc_bitmap in;
    if (!b || !bits)
      return 0;
    cc_copy(&in, b, 40);
    in.size = 40;
    uint32_t r = (uint32_t)ICDecompress(
        d->decoder, (flags & DDF_NOTKEYFRAME) ? 0x08000000u : 0, &in, bits,
        img, d->buffer);
    if (r)
      return 0;
    d->have_frame = 1;
    pixels = d->buffer;
  } else {
    if (!bits)
      return 0;
    d->last_bits = bits;
  }
  if (flags & (DDF_DONTDRAW | DDF_HURRYUP))
    return 1;
  int w = dx > 0 ? dx : d->dst_w, hh = dy > 0 ? dy : d->dst_h;
  int src_w = sw > 0 ? sw : img->width, src_h = sh > 0 ? sh : (int)rows(img);
  const struct gdi_api *g = gdi();
  if (!g)
    return 0;
  g->mode((uint32_t)dc, 3); /* COLORONCOLOR */
  return g->stretch((uint32_t)dc, x, y, w, hh, sx > 0 ? sx : 0,
                    sy > 0 ? sy : 0, src_w, src_h, pixels, d->image, 0,
                    0x00CC0020u) != 0;
}
void *W32API DrawDibGetBuffer(uintptr_t h, struct cc_bitmap *b, uint32_t size,
                              uint32_t flags) {
  struct drawdib *d = get(h);
  (void)flags;
  if (!d || !d->decoder || !b || size < 40)
    return 0;
  cc_copy(b, d->image, size < sizeof(d->image) ? size : sizeof(d->image));
  return d->buffer;
}
uintptr_t W32API DrawDibGetPalette(uintptr_t h) {
  (void)h;
  return 0; /* no logical palette is created */
}
int W32API DrawDibSetPalette(uintptr_t h, uintptr_t palette) {
  (void)palette;
  return get(h) != 0;
}
int W32API DrawDibChangePalette(uintptr_t h, int start, int count,
                                const uint8_t *entries) {
  struct drawdib *d = get(h);
  if (!d || start < 0 || count < 0 || start + count > 256 ||
      (count && !entries))
    return 0;
  for (int i = 0; i < count; i++) { /* PALETTEENTRY rgb -> RGBQUAD bgr */
    uint8_t *q = d->image + 40 + 4 * (start + i);
    q[0] = entries[4 * i + 2];
    q[1] = entries[4 * i + 1];
    q[2] = entries[4 * i];
    q[3] = 0;
  }
  return 1;
}
uint32_t W32API DrawDibRealize(uintptr_t h, uintptr_t dc, int background) {
  (void)dc;
  (void)background;
  (void)h;
  return 0;
}
int W32API DrawDibStart(uintptr_t h, uint32_t rate) {
  (void)rate;
  return get(h) != 0;
}
int W32API DrawDibStop(uintptr_t h) { return get(h) != 0; }
int W32API DrawDibTime(uintptr_t h, uint32_t *times) {
  if (!get(h) || !times)
    return 0;
  cc_zero(times, 9 * 4); /* DRAWDIBTIME: no timing is collected */
  return 1;
}
uint32_t W32API DrawDibProfileDisplay(const struct cc_bitmap *b) {
  (void)b;
  /* PD_CAN_DRAW_DIB | PD_CAN_STRETCHDIB | PD_STRETCHDIB_1_1/1_2/1_N_OK */
  return 0x1fu;
}
