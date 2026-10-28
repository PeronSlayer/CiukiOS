/* DIB/DDB object and transfer facades. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include "internal.h"
int gdi_info(HDC h, const BITMAPINFO *info, unsigned usage, void *bits,
             struct w32_dib *d) {
  if (!info || usage > 1)
    return 0;
  const BITMAPINFOHEADER *b = &info->bmiHeader;
  if (b->biSize < 40 || b->biSize > 124 || b->biWidth <= 0 ||
      b->biWidth > 8192 || !b->biHeight || b->biHeight < -8192 ||
      b->biHeight > 8192 || b->biPlanes != 1 ||
      (b->biCompression != BI_RGB && b->biCompression != BI_BITFIELDS) ||
      (b->biCompression == BI_BITFIELDS && b->biBitCount != 16 &&
       b->biBitCount != 32))
    return 0;
  if (b->biBitCount != 1 && b->biBitCount != 4 && b->biBitCount != 8 &&
      b->biBitCount != 16 && b->biBitCount != 24 && b->biBitCount != 32)
    return 0;
  *d = (struct w32_dib){
      .bits = bits,
      .width = (DWORD)b->biWidth,
      .height = (DWORD)(b->biHeight < 0 ? -b->biHeight : b->biHeight),
      .bpp = b->biBitCount,
      .top_down = b->biHeight < 0};
  d->pitch = ((d->width * d->bpp + 31) / 32) * 4;
  d->size = d->pitch * d->height;
  const uint8_t *colors = (const uint8_t *)info + b->biSize;
  if (d->bpp > 8) {
    if (b->biCompression == BI_BITFIELDS) {
      for (unsigned i = 0; i < 3; i++)
        d->masks[i] = w32_u32((const uint8_t *)info + 40 + i * 4);
      if (b->biSize == 40)
        colors += 12;
    } else if (d->bpp == 16) {
      d->masks[0] = 0x7c00;
      d->masks[1] = 0x3e0;
      d->masks[2] = 0x1f;
    } else {
      d->masks[0] = 0xff0000;
      d->masks[1] = 0xff00;
      d->masks[2] = 0xff;
    }
  } else {
    d->color_count = b->biClrUsed ? b->biClrUsed : 1u << d->bpp;
    if (d->color_count > (1u << d->bpp))
      return 0;
    struct dc *dc = gdi_dc(h);
    struct object *palette = dc ? gdi_object(dc->s.palette, GDI_PALETTE) : 0;
    for (unsigned i = 0; i < d->color_count; i++) {
      if (usage == DIB_PAL_COLORS) {
        unsigned index = w32_u16(colors + i * 2);
        if (!palette || index >= palette->u.palette.count)
          return 0;
        PALETTEENTRY c = palette->u.palette.entries[index];
        d->colors[i] =
            ((DWORD)c.peRed << 16) | ((DWORD)c.peGreen << 8) | c.peBlue;
      } else {
        const uint8_t *c = colors + i * 4;
        d->colors[i] = ((DWORD)c[2] << 16) | ((DWORD)c[1] << 8) | c[0];
      }
    }
  }
  uint8_t dummy;
  struct w32_dib check = *d;
  if (!check.bits)
    check.bits = &dummy;
  return w32_dib_validate(&check);
}
#ifndef W32_HOST_MODEL
__declspec(dllimport) void *W32API MapViewOfFile(void *, DWORD, DWORD, DWORD,
                                                 DWORD);
#else
extern void *W32API MapViewOfFile(void *, DWORD, DWORD, DWORD, DWORD);
#endif
HBITMAP W32API CreateDIBSection(HDC h, const BITMAPINFO *info, unsigned usage,
                                void **out, void *section, DWORD offset) {
  GDI_GUARD;
  if (!out)
    return 0;
  *out = 0;
  struct w32_dib dib;
  if ((h && !gdi_dc(h)) || (offset & 3) || (!section && offset) ||
      !gdi_info(h, info, usage, 0, &dib))
    return 0;
  HBITMAP handle;
  struct object *o = gdi_new(GDI_BITMAP, &handle);
  if (!o)
    return 0;
  struct bitmap *b = &o->u.bitmap;
  b->dib = dib;
  b->dib_section = 1;
  b->offset = offset;
  if (section) {
    DWORD base = offset & ~65535u, delta = offset - base;
    if (dib.size > 0xffffffffu - delta) {
      DeleteObject(handle);
      return 0;
    }
    b->allocation = MapViewOfFile(section, 2, 0, base, delta + dib.size);
    if (b->allocation) {
      b->section = section;
      b->dib.bits = (uint8_t *)b->allocation + delta;
    }
  } else {
    b->alloc_size = dib.size;
    b->allocation = gdi_alloc(dib.size);
    b->dib.bits = b->allocation;
  }
  if (!b->allocation) {
    DeleteObject(handle);
    return 0;
  }
  *out = b->dib.bits;
  return handle;
}
HBITMAP W32API CreateBitmap(int width, int height, unsigned planes,
                            unsigned bpp, const void *bits) {
  GDI_GUARD;
  if (width < 0 || height < 0 || width > 8192 || height > 8192 || planes != 1)
    return 0;
  if (!width || !height)
    return CreateBitmap(1, 1, 1, 1, 0);
  if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32)
    return 0;
  HBITMAP h;
  struct object *o = gdi_new(GDI_BITMAP, &h);
  if (!o)
    return 0;
  struct bitmap *b = &o->u.bitmap;
  b->dib = (struct w32_dib){.width = (DWORD)width,
                            .height = (DWORD)height,
                            .pitch = ((DWORD)width * bpp + 15) / 16 * 2,
                            .bpp = bpp,
                            .top_down = 1};
  b->dib.size = b->dib.pitch * b->dib.height;
  b->alloc_size = b->dib.size;
  b->mono = bpp == 1;
  b->allocation = gdi_alloc(b->alloc_size);
  b->dib.bits = b->allocation;
  if (!b->allocation) {
    DeleteObject(h);
    return 0;
  }
  if (bits)
    w32_copy(b->allocation, bits, b->alloc_size);
  if (bpp <= 8) {
    DWORD system[256];
    gdi_system_palette(system);
    b->dib.color_count = 1u << bpp;
    for (unsigned i = 0; i < b->dib.color_count; i++)
      b->dib.colors[i] = bpp == 1 ? (i ? 0xffffff : 0) : system[i];
  } else {
    b->dib.masks[0] = bpp == 16 ? 0x7c00 : 0xff0000;
    b->dib.masks[1] = bpp == 16 ? 0x3e0 : 0xff00;
    b->dib.masks[2] = bpp == 16 ? 0x1f : 0xff;
  }
  return h;
}
HBITMAP W32API CreateBitmapIndirect(const BITMAP *b) {
  return b ? CreateBitmap(b->bmWidth, b->bmHeight, b->bmPlanes, b->bmBitsPixel,
                          b->bmBits)
           : 0;
}
HBITMAP W32API CreateCompatibleBitmap(HDC h, int w, int height) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct w32_dib *s = gdi_target(d);
  if (!s)
    return 0;
  /* fx-hlrun: the screen DC stands for the display (w32_gdi_display_dc). */
  if (d->display)
    return CreateBitmap(w, height, 1, gdi_display_bpp(), 0);
  HBITMAP bitmap = CreateBitmap(w, height, 1, s->bpp, 0);
  struct object *b = gdi_object(bitmap, GDI_BITMAP);
  if (b && !b->stock) {
    for (unsigned i = 0; i < 3; i++)
      b->u.bitmap.dib.masks[i] = s->masks[i];
    b->u.bitmap.dib.color_count = s->color_count;
    w32_copy(b->u.bitmap.dib.colors, s->colors, sizeof(s->colors));
  }
  return bitmap;
}
HBITMAP W32API CreateDIBitmap(HDC h, const BITMAPINFOHEADER *header, DWORD init,
                              const void *bits, const BITMAPINFO *info,
                              unsigned usage) {
  GDI_GUARD;
  if (!header || header->biSize < 40 || header->biWidth <= 0 ||
      header->biHeight == (-2147483647 - 1))
    return 0;
  /* fx-hlrun: without a DC the bitmap is monochrome (Wine 10.0
   * dlls/gdi32/dib.c CreateDIBitmap: CreateBitmap(width, height, 1, 1)). */
  int lines = header->biHeight < 0 ? -header->biHeight : header->biHeight;
  HBITMAP b = h ? CreateCompatibleBitmap(h, header->biWidth, lines)
                : CreateBitmap(header->biWidth, lines, 1, 1, 0);
  if (b && (init & CBM_INIT) &&
      (!info || !bits ||
       !SetDIBits(h, b, 0,
                  (unsigned)(header->biHeight < 0 ? -header->biHeight
                                                  : header->biHeight),
                  bits, info, usage))) {
    DeleteObject(b);
    return 0;
  }
  return b;
}
BOOL W32API GdiFlush(void) { return 1; }
static int scan_transfer(struct w32_dib *dst, unsigned start, unsigned lines,
                         const struct w32_dib *src, int get) {
  unsigned height = get ? src->height : dst->height;
  if (start >= height)
    return 0;
  if (lines > height - start)
    lines = height - start;
  unsigned width = dst->width < src->width ? dst->width : src->width;
  /* Scan lines are numbered in the caller's DIB orientation (bottom-up DIBs
   * count from the bottom row); the bitmap side is addressed in logical
   * top-down rows, so orientation never depends on how the bitmap object
   * itself is stored (Microsoft SetDIBits/GetDIBits uStartScan). */
  const struct w32_dib *dib = get ? dst : src;
  for (unsigned i = 0; i < lines; i++)
    for (unsigned x = 0; x < width; x++) {
      unsigned sy =
          get ? (dib->top_down ? start + i : src->height - 1 - start - i)
              : (dib->top_down ? i : lines - 1 - i);
      unsigned dy =
          get ? (dib->top_down ? i : lines - 1 - i)
              : (dib->top_down ? start + i : dst->height - 1 - start - i);
      w32_dib_put(dst, (int)x, (int)dy,
                  w32_dib_encode(dst, w32_dib_rgb(src, (int)x, (int)sy)));
    }
  return (int)lines;
}
/* fx-hlrun: SetDIBits/GetDIBits also work on a bitmap that is selected into
 * a memory DC. Microsoft's reference asks callers not to do it, but Windows
 * performs the transfer (it is a copy of the bitmap's bits, not a DC
 * operation) and Wine 10.0 dlls/win32u/dib.c NtGdiSetDIBitsInternal and
 * NtGdiGetDIBitsInternal have no such refusal; Half-Life's launcher draws
 * its owner-drawn buttons by reading back the bitmap of its memory DC. */
int W32API SetDIBits(HDC h, HBITMAP bitmap, unsigned start, unsigned lines,
                     const void *bits, const BITMAPINFO *info, unsigned usage) {
  GDI_GUARD;
  struct object *o = gdi_object(bitmap, GDI_BITMAP);
  struct w32_dib src;
  if (!o || !bits || !gdi_info(h, info, usage, (void *)bits, &src))
    return 0;
  if (start >= o->u.bitmap.dib.height)
    return 0;
  if (lines > o->u.bitmap.dib.height - start)
    lines = o->u.bitmap.dib.height - start;
  if (lines > src.height)
    lines = src.height;
  src.height = lines;
  if (!lines)
    return 0;
  return scan_transfer(&o->u.bitmap.dib, start, lines, &src, 0);
}
int W32API GetDIBits(HDC h, HBITMAP bitmap, unsigned start, unsigned lines,
                     void *bits, BITMAPINFO *info, unsigned usage) {
  GDI_GUARD;
  struct object *o = gdi_object(bitmap, GDI_BITMAP);
  if (!o || !info || usage > 1 || info->bmiHeader.biSize < 40)
    return 0;
  struct w32_dib *src = &o->u.bitmap.dib;
  if (!info->bmiHeader.biBitCount) {
    info->bmiHeader = (BITMAPINFOHEADER){40,
                                         (LONG)src->width,
                                         (LONG)src->height,
                                         1,
                                         (uint16_t)src->bpp,
                                         BI_RGB,
                                         ((src->width * src->bpp + 31) / 32) *
                                             4 * src->height,
                                         0,
                                         0,
                                         src->color_count,
                                         0};
    if (!bits)
      return (int)src->height;
  }
  if (info->bmiHeader.biBitCount <= 8) {
    unsigned count = info->bmiHeader.biClrUsed
                         ? info->bmiHeader.biClrUsed
                         : 1u << info->bmiHeader.biBitCount;
    if (count > 256)
      return 0;
    DWORD system[256];
    gdi_system_palette(system);
    for (unsigned i = 0; i < count; i++) {
      DWORD c = i < src->color_count ? src->colors[i] : system[i];
      if (usage == DIB_RGB_COLORS)
        ((RGBQUAD *)((uint8_t *)info + info->bmiHeader.biSize))[i] =
            (RGBQUAD){(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), 0};
      else
        w32_put16((uint8_t *)info + info->bmiHeader.biSize + i * 2,
                  (uint16_t)i);
    }
  }
  struct w32_dib dst;
  if (!gdi_info(h, info, usage, bits, &dst))
    return 0;
  info->bmiHeader.biSizeImage = dst.size;
  if (!bits)
    return (int)src->height;
  if (start >= src->height)
    return 0;
  if (lines > src->height - start)
    lines = src->height - start;
  if (lines > dst.height)
    lines = dst.height;
  dst.height = lines;
  if (!lines)
    return 0;
  return scan_transfer(&dst, start, lines, src, 1);
}
struct blt_clip {
  struct dc *dc;
  HBRUSH brush;
  int pattern_used;
};
static int blt_visible(void *opaque, int x, int y) {
  struct blt_clip *clip = opaque;
  DWORD c;
  return gdi_visible(clip->dc, x, y) &&
         (!clip->pattern_used ||
          gdi_brush_pixel(clip->dc, clip->brush, x, y, &c));
}
BOOL gdi_blt(struct dc *d, int x, int y, int w, int height,
             const struct w32_dib *src, int sx, int sy, int sw, int sh,
             DWORD rop, int64_t key) {
  POINT p[2] = {{x, y}, {0, 0}};
  if ((int64_t)x + w < -GDI_LIMIT || (int64_t)x + w > GDI_LIMIT ||
      (int64_t)y + height < -GDI_LIMIT || (int64_t)y + height > GDI_LIMIT)
    return 0;
  p[1] = (POINT){x + w, y + height};
  if (!gdi_transform(d, p, 2, 0))
    return 0;
  int dw = p[1].x - p[0].x, dh = p[1].y - p[0].y;
  unsigned r = (rop >> 16) & 255;
  DWORD pixels[64];
  struct w32_dib pattern;
  int uses_pattern = ((r ^ (r >> 4)) & 15) != 0;
  if (uses_pattern)
    gdi_brush_pattern(d, &pattern, pixels);
  struct blt_clip clip = {d, d->s.brush, uses_pattern};
  BOOL good = w32_dib_blt(
      gdi_target(d), p[0].x, p[0].y, dw, dh, src, sx, sy, sw, sh, r,
      (unsigned)d->s.stretch, uses_pattern ? &pattern : 0,
      (POINT){d->s.brush_org.x + d->origin.x, d->s.brush_org.y + d->origin.y},
      0, key, blt_visible, &clip);
  if (good)
    gdi_damage(d, (RECT){p[0].x < p[1].x ? p[0].x : p[1].x,
                         p[0].y < p[1].y ? p[0].y : p[1].y,
                         p[0].x > p[1].x ? p[0].x : p[1].x,
                         p[0].y > p[1].y ? p[0].y : p[1].y});
  return good;
}
int W32API StretchDIBits(HDC h, int x, int y, int w, int height, int sx, int sy,
                         int sw, int sh, const void *bits,
                         const BITMAPINFO *info, unsigned usage, DWORD rop) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct w32_dib src;
  if (d && d->record) /* f5-20B metafile DC */
    return gdi_record_dib(d, x, y, w, height, sx, sy, sw, sh, bits, info,
                          usage, rop)
               ? (sh < 0 ? -sh : sh)
               : 0;
  if (!d || !bits || !gdi_info(h, info, usage, (void *)bits, &src) ||
      sy < -GDI_LIMIT || sy > GDI_LIMIT || sh < -GDI_LIMIT || sh > GDI_LIMIT)
    return 0;
  if (!src.top_down)
    sy = (int)src.height - sy - sh;
  return gdi_blt(d, x, y, w, height, &src, sx, sy, sw, sh, rop, -1)
             ? (sh < 0 ? -sh : sh)
             : 0;
}
int W32API SetDIBitsToDevice(HDC h, int x, int y, DWORD w, DWORD height, int sx,
                             int sy, unsigned start, unsigned lines,
                             const void *bits, const BITMAPINFO *info,
                             unsigned usage) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct w32_dib src;
  if (!d || w > GDI_LIMIT || height > GDI_LIMIT || !bits || x < -GDI_LIMIT ||
      x > GDI_LIMIT || y < -GDI_LIMIT || y > GDI_LIMIT || sy < -GDI_LIMIT ||
      sy > GDI_LIMIT || !gdi_info(h, info, usage, (void *)bits, &src) ||
      start >= src.height)
    return 0;
  if (lines > src.height - start)
    lines = src.height - start;
  if (!lines)
    return 0;
  /* Input consists only of the requested scan band, beginning at start. */
  DWORD full_height = src.height;
  src.height = lines;
  src.size = src.pitch * lines;
  int top = src.top_down ? (int)start : (int)(full_height - start - lines);
  int requested_top = src.top_down ? sy : (int)full_height - sy - (int)height;
  int first = requested_top > top ? requested_top : top,
      last = requested_top + (int)height;
  if (last > top + (int)lines)
    last = top + (int)lines;
  if (last <= first)
    return 0;
  return gdi_blt(d, x, y + first - requested_top, (int)w, last - first, &src,
                 sx, first - top, (int)w, last - first, SRCCOPY, -1)
             ? last - first
             : 0;
}
BOOL W32API StretchBlt(HDC dst, int x, int y, int w, int height, HDC source,
                       int sx, int sy, int sw, int sh, DWORD rop) {
  GDI_GUARD;
  struct dc *d = gdi_dc(dst), *s = gdi_dc(source);
  unsigned r = (rop >> 16) & 255;
  if (!d)
    return 0;
  if (d->record) { /* f5-20B metafile DC */
    if (!((r ^ (r >> 2)) & 0x33)) {
      int16_t v[6] = {(int16_t)(rop & 0xffff), (int16_t)(rop >> 16),
                      (int16_t)height,         (int16_t)w,
                      (int16_t)y,              (int16_t)x};
      return gdi_record(d, 0x061du, v, 6);
    }
    return s && gdi_target(s) &&
           gdi_record_bitmap(d, x, y, w, height, gdi_target(s), sx, sy, sw, sh,
                             rop);
  }
  if (!((r ^ (r >> 2)) & 0x33))
    return gdi_blt(d, x, y, w, height, 0, 0, 0, 0, 0, rop, -1);
  if (!s || (int64_t)sx + sw < -GDI_LIMIT || (int64_t)sx + sw > GDI_LIMIT ||
      (int64_t)sy + sh < -GDI_LIMIT || (int64_t)sy + sh > GDI_LIMIT)
    return 0;
  POINT p[2] = {{sx, sy}, {sx + sw, sy + sh}};
  if (!gdi_transform(s, p, 2, 0))
    return 0;
  struct w32_dib src = *gdi_target(s);
  struct object *b = gdi_object(s->s.bitmap, GDI_BITMAP);
  if (s->memory && b && b->u.bitmap.mono) {
    src.colors[0] = gdi_resolve_color(d, d->s.text);
    src.colors[1] = gdi_resolve_color(d, d->s.bk);
  }
  struct object *destination =
      d->memory ? gdi_object(d->s.bitmap, GDI_BITMAP) : 0;
  void *mono_bits = 0;
  if (destination && destination->u.bitmap.mono && src.bpp > 1) {
    struct w32_dib mono = {.width = src.width,
                           .height = src.height,
                           .bpp = 1,
                           .top_down = 1,
                           .pitch = (src.width + 7) / 8,
                           .colors = {0, 0xffffff},
                           .color_count = 2};
    mono.size = mono.pitch * mono.height;
    mono_bits = gdi_alloc(mono.size);
    if (!mono_bits)
      return 0;
    mono.bits = mono_bits;
    DWORD background = gdi_resolve_color(s, s->s.bk);
    for (unsigned yy = 0; yy < src.height; yy++)
      for (unsigned xx = 0; xx < src.width; xx++)
        w32_dib_put(&mono, (int)xx, (int)yy,
                    w32_dib_rgb(&src, (int)xx, (int)yy) == background);
    src = mono;
  }
  BOOL result = gdi_blt(d, x, y, w, height, &src, p[0].x, p[0].y,
                        p[1].x - p[0].x, p[1].y - p[0].y, rop, -1);
  if (mono_bits)
    gdi_free(mono_bits, src.size);
  return result;
}
BOOL W32API BitBlt(HDC d, int x, int y, int w, int height, HDC s, int sx,
                   int sy, DWORD rop) {
  return StretchBlt(d, x, y, w, height, s, sx, sy, w, height, rop);
}
BOOL W32API PatBlt(HDC h, int x, int y, int w, int height, DWORD rop) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  unsigned r = (rop >> 16) & 255;
  if (!d || ((r ^ (r >> 2)) & 0x33))
    return 0;
  if (d->record) { /* f5-20B metafile DC */
    int16_t v[6] = {(int16_t)(rop & 0xffff), (int16_t)(rop >> 16),
                    (int16_t)height,         (int16_t)w,
                    (int16_t)y,              (int16_t)x};
    return gdi_record(d, 0x061du, v, 6);
  }
  return gdi_blt(d, x, y, w, height, 0, 0, 0, 0, 0, rop, -1);
}
BOOL W32API TransparentBlt(HDC h, int x, int y, int w, int height, HDC source,
                           int sx, int sy, int sw, int sh, COLORREF color) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h), *s = gdi_dc(source);
  if (!d || !s || w < 0 || height < 0 || sw < 0 || sh < 0 ||
      (int64_t)sx + sw < -GDI_LIMIT || (int64_t)sx + sw > GDI_LIMIT ||
      (int64_t)sy + sh < -GDI_LIMIT || (int64_t)sy + sh > GDI_LIMIT)
    return 0;
  POINT p[2] = {{sx, sy}, {sx + sw, sy + sh}};
  if (!gdi_transform(s, p, 2, 0))
    return 0;
  return gdi_blt(d, x, y, w, height, gdi_target(s), p[0].x, p[0].y,
                 p[1].x - p[0].x, p[1].y - p[0].y, SRCCOPY,
                 gdi_color(GetNearestColor(source, color)));
}
struct mask_clip {
  struct dc *dc;
  const struct w32_dib *mask;
  int x, y, mx, my, value;
};
static int mask_visible(void *opaque, int x, int y) {
  struct mask_clip *m = opaque;
  int xx = x - m->x + m->mx, yy = y - m->y + m->my;
  return gdi_visible(m->dc, x, y) && xx >= 0 && yy >= 0 &&
         (DWORD)xx < m->mask->width && (DWORD)yy < m->mask->height &&
         (int)w32_dib_raw(m->mask, xx, yy) == m->value;
}
BOOL W32API MaskBlt(HDC dst, int x, int y, int w, int height, HDC source,
                    int sx, int sy, HBITMAP mask, int mx, int my, DWORD rop) {
  GDI_GUARD;
  struct dc *d = gdi_dc(dst), *s = gdi_dc(source);
  struct object *m = gdi_object(mask, GDI_BITMAP);
  if (!d || !s || !m || m->u.bitmap.dib.bpp != 1 || w < 0 || height < 0 ||
      w > GDI_LIMIT || height > GDI_LIMIT || x < -GDI_LIMIT || x > GDI_LIMIT ||
      y < -GDI_LIMIT || y > GDI_LIMIT || sx < -GDI_LIMIT || sx > GDI_LIMIT ||
      sy < -GDI_LIMIT || sy > GDI_LIMIT || mx < -GDI_LIMIT || mx > GDI_LIMIT ||
      my < -GDI_LIMIT || my > GDI_LIMIT || d->s.map != MM_TEXT ||
      s->s.map != MM_TEXT)
    return 0;
  struct w32_dib src = *gdi_target(s);
  void *copy = gdi_alloc(src.size);
  if (!copy)
    return 0;
  w32_copy(copy, src.bits, src.size);
  src.bits = copy;
  DWORD pixels[64];
  struct w32_dib pattern;
  gdi_brush_pattern(d, &pattern, pixels);
  struct mask_clip clip = {
      d,
      &m->u.bitmap.dib,
      x + d->s.viewport_org.x - d->s.window_org.x + d->origin.x,
      y + d->s.viewport_org.y - d->s.window_org.y + d->origin.y,
      mx,
      my,
      0};
  int good = 1;
  for (int value = 0; value < 2; value++) {
    clip.value = value;
    unsigned r = value ? (rop >> 16) & 255 : rop >> 24;
    if (!w32_dib_blt(gdi_target(d), clip.x, clip.y, w, height, &src,
                     sx + s->s.viewport_org.x - s->s.window_org.x + s->origin.x,
                     sy + s->s.viewport_org.y - s->s.window_org.y + s->origin.y,
                     w, height, r, COLORONCOLOR, &pattern,
                     (POINT){d->s.brush_org.x + d->origin.x,
                             d->s.brush_org.y + d->origin.y},
                     0, -1, mask_visible, &clip))
      good = 0;
  }
  gdi_free(copy, src.size);
  if (good)
    gdi_damage(d, (RECT){clip.x, clip.y, clip.x + w, clip.y + height});
  return good;
}
