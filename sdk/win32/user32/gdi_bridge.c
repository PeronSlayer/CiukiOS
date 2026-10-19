/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Adapter to f5-10B's published w32_gdi_surface_dc boundary. No engine copy. */
#include "internal.h"
extern HANDLE W32API GetModuleHandleA(const char *);
extern HANDLE W32API LoadLibraryA(const char *);
extern void *W32API GetProcAddress(HANDLE, const char *);
/* Identical ABI to f5-10B w32/gdi32.h; isolate its DWORD GDI handles from
 * USER's pointer-sized host model typedefs. Actual PE32 handles are 32-bit. */
struct gdi_dib {
  uint8_t *bits;
  DWORD width, height, pitch, size, bpp, top_down, masks[3], colors[256],
      color_count;
};
#ifndef W32_HOST_MODEL
_Static_assert(sizeof(struct gdi_dib) == 1068, "f5-10B DIB ABI");
#endif
struct bitmap_info {
  DWORD size;
  LONG width, height;
  uint16_t planes, bpp;
  DWORD compression, bytes;
  LONG xppm, yppm;
  DWORD colors, important;
};
struct bitmap {
  LONG type, width, height, pitch;
  uint16_t planes, bpp;
  void *bits;
};
struct region_header {
  DWORD size, type, count, bytes;
  RECT bounds;
};
static struct {
  DWORD(W32API *surface_dc)(const struct gdi_dib *, POINT,
                            void (*)(void *, const RECT *), void *);
  BOOL(W32API *visible)(DWORD, DWORD);
  DWORD(W32API *rect_region)(int, int, int, int);
  int(W32API *combine)(DWORD, DWORD, DWORD, int);
  DWORD(W32API *region_data)(DWORD, DWORD, void *);
  BOOL(W32API *delete_object)(DWORD);
  BOOL(W32API *delete_dc)(DWORD);
  DWORD(W32API *select)(DWORD, DWORD);
  BOOL(W32API *patblt)(DWORD, int, int, int, int, DWORD);
  DWORD(W32API *brush)(DWORD);
  DWORD(W32API *dib_section)(DWORD, const void *, UINT, void **, HANDLE, DWORD);
  DWORD(W32API *bitmap)(int, int, UINT, UINT, const void *);
  int(W32API *object)(DWORD, int, void *);
  int(W32API *get_dibits)(DWORD, DWORD, UINT, UINT, void *, void *, UINT);
  BOOL(W32API *icon)(DWORD, int, int, int, int, DWORD, DWORD);
} g;
struct binding {
  DWORD dc;
  struct u32_region touched;
};
static struct binding bindings[64];
static void damage(void *opaque, const RECT *r) {
  struct binding *b = opaque;
  u32_region_add(&b->touched, *r);
}
static DWORD region(const RECT *r, UINT n) {
  DWORD all = g.rect_region(0, 0, 0, 0);
  if (!all)
    return 0;
  for (UINT i = 0; i < n; i++) {
    DWORD one = g.rect_region(r[i].left, r[i].top, r[i].right, r[i].bottom);
    if (!one || !g.combine(all, all, one, 2)) {
      if (one)
        g.delete_object(one);
      g.delete_object(all);
      return 0;
    }
    g.delete_object(one);
  }
  return all;
}
static HDC acquire(const struct u32_surface *s, POINT origin, const RECT *r,
                   UINT n, const RECT *exclude, UINT excludes) {
  struct binding *b = 0;
  for (UINT i = 0; i < 64; i++)
    if (!bindings[i].dc) {
      b = &bindings[i];
      break;
    }
  if (!b)
    return 0;
  struct gdi_dib dib = {.bits = s->pixels,
                        .width = s->width,
                        .height = s->height,
                        .pitch = s->pitch,
                        .size = s->pitch * s->height,
                        .bpp = 32,
                        .top_down = 1,
                        .masks = {0xff0000, 0xff00, 0xff}};
  DWORD clip = region(r, n);
  if (!clip)
    return 0;
  for (UINT i = 0; i < excludes; i++) {
    DWORD one = g.rect_region(exclude[i].left, exclude[i].top, exclude[i].right,
                              exclude[i].bottom);
    if (!one || !g.combine(clip, clip, one, 4)) {
      if (one)
        g.delete_object(one);
      g.delete_object(clip);
      return 0;
    }
    g.delete_object(one);
  }
  b->touched.n = 0;
  b->dc = g.surface_dc(&dib, origin, damage, b);
  if (!b->dc || !g.visible(b->dc, clip)) {
    if (b->dc)
      g.delete_dc(b->dc);
    b->dc = 0;
  }
  g.delete_object(clip);
  return (HDC)(uintptr_t)b->dc;
}
static UINT release(HDC dc, RECT *out, UINT cap) {
  for (UINT i = 0; i < 64; i++)
    if (bindings[i].dc == U32_PTR(dc)) {
      struct binding *b = &bindings[i];
      UINT count = b->touched.n;
      if (count > cap)
        return count;
      for (UINT j = 0; j < count; j++)
        out[j] = b->touched.r[j];
      g.delete_dc(b->dc);
      b->dc = 0;
      b->touched.n = 0;
      return count;
    }
  return 0;
}
static BOOL fill(HDC dc, const RECT *r, HBRUSH brush) {
  DWORD temporary = 0, h = U32_PTR(brush);
  if (h && h <= 29) {
    temporary = g.brush(GetSysColor(h - 1));
    h = temporary;
  }
  if (!h)
    return 0;
  DWORD old = g.select(U32_PTR(dc), h);
  BOOL ok = g.patblt(U32_PTR(dc), r->left, r->top, r->right - r->left,
                     r->bottom - r->top, 0x00f00021);
  if (old)
    g.select(U32_PTR(dc), old);
  if (temporary)
    g.delete_object(temporary);
  return ok;
}
static HANDLE create_bitmap(const struct u32_surface *s) {
  struct bitmap_info info = {.size = 40,
                             .width = s->width,
                             .height = -(LONG)s->height,
                             .planes = 1,
                             .bpp = 32};
  void *bits = 0;
  DWORD h = g.dib_section(0, &info, 0, &bits, 0, 0);
  if (h && bits)
    for (UINT y = 0; y < s->height; y++)
      w32_copy((uint8_t *)bits + y * s->width * 4,
               (uint8_t *)s->pixels + y * s->pitch, s->width * 4);
  return (HANDLE)(uintptr_t)h;
}
static BOOL bitmap_view(HANDLE h, struct u32_surface *s, BOOL release) {
  if (release) {
    if (!s)
      return g.delete_object(U32_PTR(h));
    if (s->fd == -2)
      u32_free(s->pixels);
    w32_fill(s, 0, sizeof(*s));
    return 1;
  }
  struct bitmap b = {0};
  if (!s || g.object(U32_PTR(h), sizeof(b), &b) != sizeof(b) || b.width <= 0 ||
      b.height <= 0 || b.width > 2048 || b.height > 2048)
    return 0;
  DWORD bytes = (DWORD)b.width * b.height * 4;
  void *pixels = u32_alloc(bytes);
  if (!pixels)
    return 0;
  struct bitmap_info info = {.size = 40,
                             .width = b.width,
                             .height = -b.height,
                             .planes = 1,
                             .bpp = 32};
  if (g.get_dibits(0, U32_PTR(h), 0, b.height, pixels, &info, 0) != b.height) {
    u32_free(pixels);
    return 0;
  }
  *s = (struct u32_surface){pixels, b.width, b.height, b.width * 4, 1, -2};
  return 1;
}
static BOOL icon(HDC dc, int x, int y, int width, int height,
                 const DWORD *pixels, const uint8_t *and, UINT size,
                 UINT flags) {
  if (size != 32 || flags & ~0xb)
    return 0;
  struct u32_surface s = {(void *)pixels, 32, 32, 128, 1, -1};
  DWORD color = U32_PTR(create_bitmap(&s));
  uint8_t mask[128];
  w32_fill(mask, 0, 128);
  for (UINT p = 0; p < 1024; p++)
    if (and[p])
      mask[p / 8] |= 128 >> (p & 7);
  DWORD mono = g.bitmap(32, 32, 1, 1, mask);
  BOOL ok =
      color && mono && g.icon(U32_PTR(dc), x, y, width, height, color, mono);
  if (mono)
    g.delete_object(mono);
  if (color)
    g.delete_object(color);
  return ok;
}
static UINT rects(HRGN handle, RECT *out, UINT cap) {
  DWORD size = g.region_data(U32_PTR(handle), 0, 0);
  if (size < sizeof(struct region_header))
    return UINT32_MAX;
  uint8_t *data = u32_alloc(size);
  if (!data)
    return UINT32_MAX;
  if (g.region_data(U32_PTR(handle), size, data) != size) {
    u32_free(data);
    return UINT32_MAX;
  }
  struct region_header *header = (void *)data;
  UINT count = header->count;
  if (header->size != sizeof(*header) ||
      sizeof(*header) + (uint64_t)count * sizeof(RECT) > size) {
    u32_free(data);
    return UINT32_MAX;
  }
  if (count <= cap)
    w32_copy(out, data + sizeof(*header), count * sizeof(RECT));
  u32_free(data);
  return count;
}
static BOOL set_region(HRGN handle, const RECT *r, UINT n) {
  DWORD source = region(r, n);
  if (!source)
    return 0;
  BOOL ok = g.combine(U32_PTR(handle), source, 0, 5) != 0;
  g.delete_object(source);
  return ok;
}
static const struct u32_gdi adapter = {1,          acquire,     release,
                                       fill,       icon,        rects,
                                       set_region, bitmap_view, create_bitmap};
void u32_gdi_ensure(void) {
  if (u32_gdi)
    return;
  HANDLE module = GetModuleHandleA("gdi32.dll");
  if (!module)
    module = LoadLibraryA("gdi32.dll");
  if (!module)
    return;
#define RESOLVE(field, name)                                                   \
  do {                                                                         \
    *(void **)(&g.field) = GetProcAddress(module, name);                       \
    if (!g.field)                                                              \
      return;                                                                  \
  } while (0)
  RESOLVE(surface_dc, "w32_gdi_surface_dc");
  RESOLVE(visible, "w32_gdi_set_visible_region");
  RESOLVE(rect_region, "CreateRectRgn");
  RESOLVE(combine, "CombineRgn");
  RESOLVE(region_data, "GetRegionData");
  RESOLVE(delete_object, "DeleteObject");
  RESOLVE(delete_dc, "DeleteDC");
  RESOLVE(select, "SelectObject");
  RESOLVE(patblt, "PatBlt");
  RESOLVE(brush, "CreateSolidBrush");
  RESOLVE(dib_section, "CreateDIBSection");
  RESOLVE(bitmap, "CreateBitmap");
  RESOLVE(object, "GetObjectA");
  RESOLVE(get_dibits, "GetDIBits");
  RESOLVE(icon, "w32_gdi_draw_icon");
#undef RESOLVE
  u32_set_gdi(&adapter);
}
