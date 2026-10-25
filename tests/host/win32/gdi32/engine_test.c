/* Independent Boolean/region oracles and public API/lifetime tests.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../../sdk/win32/gdi32/native_font.h"
#include "support.h"
#include <ciuki/font.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static unsigned damage_count;
static RECT last_damage;
static void damage(void *opaque, const RECT *r) {
  (void)opaque;
  damage_count++;
  last_damage = *r;
}
static HDC dc_for(struct w32_dib *s) {
  HDC h = w32_gdi_surface_dc(s, (POINT){0, 0}, damage, 0);
  assert(h);
  return h;
}
static void lifetime(void) {
  uint32_t pixels[32 * 24] = {0};
  struct w32_dib surface = test_dib(pixels, 32, 24, 32, 0, 1);
  HDC dc = dc_for(&surface), memory = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, 12, 8);
  assert(bitmap);
  HBITMAP old = SelectObject(memory, bitmap);
  assert(old);
  BITMAP default_bitmap;
  assert(GetObjectA(old, sizeof(default_bitmap), &default_bitmap) ==
         sizeof(default_bitmap));
  assert(default_bitmap.bmWidthBytes == 2);
  assert(SaveDC(memory));
  assert(SelectObject(memory, old) == bitmap);
  assert(SelectObject(memory, bitmap) == old);
  HDC other = CreateCompatibleDC(dc);
  assert(other && !SelectObject(other, bitmap));
  assert(DeleteDC(other));
  assert(RestoreDC(memory, -1));
  HPEN pen = CreatePen(PS_DASH, 1, RGB(255, 0, 0));
  assert(pen);
  HPEN oldpen = SelectObject(dc, pen);
  assert(oldpen);
  assert(!DeleteObject(pen));
  assert(SaveDC(dc) == 1);
  assert(SelectObject(dc, oldpen) == pen);
  assert(!DeleteObject(pen));
  assert(SaveDC(dc) == 2);
  assert(RestoreDC(dc, 1));
  assert(GetCurrentObject(dc, 1) == pen);
  assert(!RestoreDC(dc, 0));
  assert(DeleteDC(dc));
  assert(DeleteObject(pen));
  assert(!DeleteObject(pen));
  assert(!GetObjectType(pen));
  assert(!DeleteObject(bitmap));
  assert(SelectObject(memory, old) == bitmap);
  assert(DeleteObject(bitmap));
  assert(DeleteDC(memory));
  DWORD before = w32_gdi_live_objects();
  test_fail_after = 0;
  assert(!CreateCompatibleDC(0));
  test_fail_after = -1;
  assert(w32_gdi_live_objects() == before);
  for (unsigned i = 0; i < 200; i++) {
    HDC h = CreateCompatibleDC(0);
    assert(h);
    HBRUSH b = CreateSolidBrush(i);
    assert(b);
    assert(SelectObject(h, b));
    assert(SaveDC(h));
  }
  test_reset();
  assert(!GetObjectType(bitmap));
  puts("PASS lifetime");
}
static int rand32(void) {
  static unsigned n = 0x12345678;
  n ^= n << 13;
  n ^= n >> 17;
  n ^= n << 5;
  return (int)(n & 0x7fffffff);
}
static void regions(void) {
  for (unsigned trial = 0; trial < 40; trial++) {
    HRGN a = CreateRectRgn(0, 0, 0, 0), b = CreateRectRgn(0, 0, 0, 0),
         out = CreateRectRgn(0, 0, 0, 0);
    unsigned char aa[32][32] = {0}, bb[32][32] = {0};
    for (unsigned k = 0; k < 10; k++) {
      int x = rand32() % 22, y = rand32() % 22, w = rand32() % 9 + 1,
          h = rand32() % 9 + 1;
      HRGN r = CreateRectRgn(x, y, x + w, y + h);
      assert(CombineRgn(k & 1 ? a : b, k & 1 ? a : b, r, RGN_OR));
      assert(DeleteObject(r));
      for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
          if (k & 1)
            aa[yy][xx] = 1;
          else
            bb[yy][xx] = 1;
    }
    for (int mode = 1; mode <= 5; mode++) {
      assert(CombineRgn(out, a, b, mode));
      for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) {
          int expected = mode == 1   ? aa[y][x] && bb[y][x]
                         : mode == 2 ? aa[y][x] || bb[y][x]
                         : mode == 3 ? aa[y][x] != bb[y][x]
                         : mode == 4 ? aa[y][x] && !bb[y][x]
                                     : aa[y][x];
          assert(PtInRegion(out, x, y) == expected);
        }
      DWORD size = GetRegionData(out, 0, 0);
      RGNDATA *data = malloc(size);
      assert(GetRegionData(out, size, data) == size);
      HRGN copy = ExtCreateRegion(0, size, data);
      assert(copy && EqualRgn(copy, out));
      assert(OffsetRgn(copy, 3, 4));
      assert(OffsetRgn(copy, -3, -4));
      assert(EqualRgn(copy, out));
      assert(DeleteObject(copy));
      free(data);
    }
    assert(DeleteObject(a));
    assert(DeleteObject(b));
    assert(DeleteObject(out));
  }
  POINT p[] = {{2, 2}, {20, 2}, {20, 20}, {2, 20},
               {2, 2}, {20, 2}, {20, 20}, {2, 20}};
  int counts[] = {4, 4};
  HRGN alternate = CreatePolyPolygonRgn(p, counts, 2, ALTERNATE),
       winding = CreatePolyPolygonRgn(p, counts, 2, WINDING);
  RECT box;
  assert(GetRgnBox(alternate, &box) == NULLREGION);
  assert(PtInRegion(winding, 5, 5));
  assert(!PtInRegion(winding, 20, 5));
  HRGN ellipse = CreateEllipticRgn(0, 0, 20, 16);
  assert(PtInRegion(ellipse, 10, 8));
  assert(!PtInRegion(ellipse, 0, 0));
  test_reset();
  puts("PASS regions");
}
static void state(void) {
  uint32_t pixels[64 * 48] = {0};
  struct w32_dib s = test_dib(pixels, 64, 48, 32, 0, 1);
  HDC dc = dc_for(&s);
  POINT point;
  assert(w32_gdi_set_desktop(&s));
  assert(GetDeviceCaps(dc, 8) == 64);
  assert(GetDeviceCaps(dc, 10) == 48);
  assert(GetDeviceCaps(dc, 88) == 96);
  assert(GetDeviceCaps(dc, 14) == 1);
  HDC display = CreateDCA("DISPLAY", 0, 0, 0);
  assert(display);
  assert(!CreateDCA("printer", 0, 0, 0));
  assert(DeleteDC(display));
  assert(GetDCOrgEx(dc, &point) && point.x == 0 && point.y == 0);
  assert(SetMapMode(dc, MM_ANISOTROPIC) == MM_TEXT);
  assert(SetWindowExtEx(dc, 10, 10, 0));
  assert(SetViewportExtEx(dc, 20, -30, 0));
  assert(SetWindowOrgEx(dc, 1, 2, 0));
  assert(SetViewportOrgEx(dc, 3, 44, 0));
  POINT p[2] = {{1, 2}, {11, 12}};
  assert(LPtoDP(dc, p, 2));
  assert(p[0].x == 3 && p[0].y == 44 && p[1].x == 23 && p[1].y == 14);
  assert(DPtoLP(dc, p, 2));
  assert(p[1].x == 11 && p[1].y == 12);
  assert(SetMapMode(dc, MM_ISOTROPIC));
  assert(SetWindowExtEx(dc, 10, 20, 0));
  assert(SetViewportExtEx(dc, 40, 40, 0));
  SIZE size;
  assert(GetViewportExtEx(dc, &size));
  assert(abs(size.cx) * 20 == abs(size.cy) * 10);
  for (int mode = MM_LOMETRIC; mode <= MM_TWIPS; mode++) {
    assert(SetMapMode(dc, mode));
    p[0] = (POINT){100, 100};
    assert(LPtoDP(dc, p, 1));
    assert(p[0].y < 44);
  }
  SetMapMode(dc, MM_TEXT);
  SetWindowOrgEx(dc, 0, 0, 0);
  SetViewportOrgEx(dc, 0, 0, 0);
  HRGN clip = CreateRectRgn(2, 3, 20, 22),
       visible = CreateRectRgn(5, 6, 15, 18);
  assert(SelectClipRgn(dc, clip));
  assert(w32_gdi_set_visible_region(dc, visible));
  assert(SaveDC(dc));
  assert(ExcludeClipRect(dc, 8, 8, 12, 12));
  assert(!PtVisible(dc, 10, 10));
  assert(RestoreDC(dc, -1));
  assert(PtVisible(dc, 10, 10));
  assert(OffsetClipRgn(dc, 1, 2));
  RECT box;
  assert(GetClipBox(dc, &box));
  assert(box.left == 5 && box.top == 6 && box.right == 15 && box.bottom == 18);
  assert(SetPixel(dc, 1, 1, RGB(3, 4, 5)) == CLR_INVALID);
  assert(SetPixel(dc, 10, 10, RGB(3, 4, 5)) == RGB(3, 4, 5));
  assert(GetPixel(dc, 10, 10) == RGB(3, 4, 5));
  assert(SelectClipRgn(dc, 0));
  assert(w32_gdi_window_shape(dc, clip));
  assert(!PtVisible(dc, 1, 1));
  assert(w32_gdi_window_shape(dc, 0));
  test_reset();
  puts("PASS state");
}
struct info256 {
  BITMAPINFOHEADER h;
  RGBQUAD colors[256];
};
static void bitmaps(void) {
  uint32_t pixels[40 * 30] = {0};
  struct w32_dib surface = test_dib(pixels, 40, 30, 32, 0, 1);
  HDC dc = dc_for(&surface);
  struct info256 info = {.h = {40, 7, -5, 1, 8, BI_RGB, 0, 0, 0, 256, 0}};
  for (unsigned i = 0; i < 256; i++)
    info.colors[i] =
        (RGBQUAD){(uint8_t)i, (uint8_t)(255 - i), (uint8_t)(i ^ 85), 0};
  void *bits = 0;
  HBITMAP bitmap =
      CreateDIBSection(dc, (BITMAPINFO *)&info, DIB_RGB_COLORS, &bits, 0, 0);
  assert(bitmap && bits);
  memset(bits, 0xcc, 40);
  ((uint8_t *)bits)[0] = 3;
  assert(GdiFlush());
  HDC memory = CreateCompatibleDC(dc);
  HBITMAP old = SelectObject(memory, bitmap);
  assert(old);
  assert(BitBlt(dc, 2, 3, 7, 5, memory, 0, 0, SRCCOPY));
  assert(GetPixel(dc, 2, 3) == RGB(3 ^ 85, 252, 3));
  DIBSECTION object;
  assert(GetObjectA(bitmap, sizeof(object), &object) == sizeof(object));
  assert(object.dsBm.bmBits == bits);
  assert(SetDIBColorTable(memory, 3, 1, (RGBQUAD[]){{7, 8, 9, 0}}) == 1);
  assert(BitBlt(dc, 2, 3, 1, 1, memory, 0, 0, SRCCOPY));
  assert(GetPixel(dc, 2, 3) == RGB(9, 8, 7));
  assert(SelectObject(memory, old) == bitmap);
  uint8_t output[40];
  memset(output, 0xee, sizeof(output));
  assert(GetDIBits(dc, bitmap, 0, 5, output, (BITMAPINFO *)&info,
                   DIB_RGB_COLORS) == 5);
  assert(output[0] == 3 && output[7] == 0xee);
  uint8_t band[8] = {4, 4, 4, 4, 4, 4, 4, 0};
  assert(SetDIBits(dc, bitmap, 2, 1, band, (BITMAPINFO *)&info,
                   DIB_RGB_COLORS) == 1);
  assert(((uint8_t *)bits)[16] == 4);
  {
    /* Mixed orientations: scan lines follow the caller's DIB (a bottom-up
     * DIB counts from the bottom row), whatever the bitmap's own storage. */
    BITMAPINFOHEADER top = {40, 2, -2, 1, 32, BI_RGB, 0, 0, 0, 0, 0},
                     bottom = {40, 2, 2, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
    void *pixels = 0;
    HBITMAP section =
        CreateDIBSection(dc, (BITMAPINFO *)&top, DIB_RGB_COLORS, &pixels, 0, 0);
    assert(section && pixels);
    const uint32_t upright[4] = {0x111111, 0x222222, 0x333333, 0x444444},
                   stored[4] = {0x333333, 0x444444, 0x111111, 0x222222};
    assert(SetDIBits(dc, section, 0, 2, stored, (BITMAPINFO *)&bottom,
                     DIB_RGB_COLORS) == 2);
    assert(!memcmp(pixels, upright, sizeof(upright)));
    uint32_t back[4] = {0};
    assert(GetDIBits(dc, section, 0, 2, back, (BITMAPINFO *)&bottom,
                     DIB_RGB_COLORS) == 2);
    assert(!memcmp(back, stored, sizeof(stored)));
    assert(DeleteObject(section));
  }
  assert(CreateBitmap(0, 0, 1, 32, 0));
  assert(CreateBitmap(4, 3, 1, 1, 0));
  uint32_t masks[3] = {0xf800, 0x7e0, 0x1f};
  struct {
    BITMAPINFOHEADER h;
    DWORD masks[3];
  } rgb565 = {{40, 5, 4, 1, 16, BI_BITFIELDS, 0, 0, 0, 0, 0}, {0}};
  memcpy(rgb565.masks, masks, sizeof(masks));
  void *rgbbits;
  assert(CreateDIBSection(dc, (BITMAPINFO *)&rgb565, 0, &rgbbits, 0, 0));
  rgb565.masks[1] = 0xf800;
  assert(!CreateDIBSection(dc, (BITMAPINFO *)&rgb565, 0, &rgbbits, 0, 0));
  unsigned char shared[1024] = {0};
  info.h.biWidth = 7;
  info.h.biHeight = -5;
  void *view;
  HBITMAP section =
      CreateDIBSection(dc, (BITMAPINFO *)&info, 0, &view, shared, 4);
  assert(section && view == shared + 4);
  ((uint8_t *)view)[0] = 99;
  assert(shared[4] == 99);
  assert(DeleteObject(section));
  test_reset();
  puts("PASS bitmaps");
}
static unsigned palette_changed, query_palette;
static int palette_metrics(void *face, unsigned em,
                           struct w32_gdi_font_metrics *out) {
  (void)face;
  (void)em;
  *out = (struct w32_gdi_font_metrics){64, 0, 0, 64};
  return 1;
}
static int palette_glyph(void *face, unsigned em, DWORD cp, int index,
                         struct w32_gdi_glyph *out) {
  (void)face;
  (void)em;
  (void)cp;
  (void)index;
  static uint8_t coverage = 255;
  *out = (struct w32_gdi_glyph){1, 1, 1, 0, 1, 64, &coverage};
  return 1;
}
static void palette_glyph_free(void *face, struct w32_gdi_glyph *g) {
  (void)face;
  (void)g;
}
static void palette_hook(void *opaque, HDC dc, unsigned message) {
  (void)opaque;
  (void)dc;
  if (message == 0x311)
    palette_changed++;
  if (message == 0x30f)
    query_palette++;
}
static void palettes(void) {
  uint8_t pixels[64] = {0};
  struct w32_dib s = test_dib(pixels, 8, 8, 8, 0, 1);
  HDC dc = dc_for(&s);
  assert(w32_gdi_set_desktop(&s));
  struct {
    uint16_t version, count;
    PALETTEENTRY entries[4];
  } log = {0x300,
           4,
           {{1, 2, 3, PC_RESERVED},
            {1, 2, 3, PC_NOCOLLAPSE},
            {255, 0, 0, 0},
            {4, 5, 6, 0}}};
  HPALETTE p = CreatePalette((LOGPALETTE *)&log);
  assert(p);
  w32_gdi_palette_hook(palette_hook, 0);
  HPALETTE old = SelectPalette(dc, p, 0);
  assert(old);
  assert(RealizePalette(dc) > 0);
  assert(RealizePalette(dc) == 0);
  assert(palette_changed && query_palette);
  PALETTEENTRY system[256];
  assert(GetSystemPaletteEntries(dc, 0, 256, system) == 256);
  assert(system[10].peRed == 1 && system[11].peRed == 1);
  assert(system[0].peRed == 0 && system[255].peRed == 255);
  assert(GetNearestPaletteIndex(p, RGB(255, 0, 0)) == 2);
  assert(SetPixel(dc, 0, 0, PALETTEINDEX(0)) == RGB(1, 2, 3) &&
         pixels[0] == 10);
  assert(SetPixel(dc, 1, 0, PALETTEINDEX(1)) == RGB(1, 2, 3) &&
         pixels[1] == 11);
  assert(SetPixel(dc, 2, 0, PALETTERGB(2, 3, 4)) == RGB(1, 2, 3) &&
         pixels[2] == 10);
  assert(SelectObject(dc, CreateSolidBrush(PALETTEINDEX(1))));
  assert(PatBlt(dc, 3, 0, 1, 1, PATCOPY) && pixels[3] == 11);
  assert(SelectObject(dc, CreatePen(PS_SOLID, 1, PALETTEINDEX(1))));
  assert(MoveToEx(dc, 4, 0, 0) && LineTo(dc, 5, 0) && pixels[4] == 11);
  SetBkColor(dc, PALETTEINDEX(1));
  assert(ExtTextOutA(dc, 0, 0, ETO_OPAQUE, &(RECT){5, 0, 6, 1}, 0, 0, 0) &&
         pixels[5] == 11);
  const struct w32_gdi_font_provider provider = {.metrics = palette_metrics,
                                                 .raster = palette_glyph,
                                                 .free_glyph =
                                                     palette_glyph_free};
  HFONT font = w32_gdi_create_font(&provider, 0, 12 * 64);
  assert(font && SelectObject(dc, font));
  SetTextColor(dc, PALETTEINDEX(1));
  SetBkMode(dc, TRANSPARENT);
  assert(TextOutA(dc, 6, 0, "X", 1) && pixels[6] == 11);
  assert(AnimatePalette(
      p, 0, 4,
      (PALETTEENTRY[]){
          {7, 8, 9, 0}, {10, 11, 12, 0}, {13, 14, 15, 0}, {16, 17, 18, 0}}));
  PALETTEENTRY actual[4];
  assert(GetPaletteEntries(p, 0, 4, actual) == 4);
  assert(actual[0].peRed == 7 && actual[1].peRed == 1);
  assert(GetSystemPaletteEntries(dc, 10, 1, system) == 1 &&
         system[0].peRed == 7);
  assert(GetPixel(dc, 0, 0) == RGB(7, 8, 9) &&
         GetPixel(dc, 1, 0) == RGB(1, 2, 3));
  DWORD rgb_pixels[64] = {0};
  struct w32_dib rgb = test_dib(rgb_pixels, 8, 8, 32, 0, 1);
  HDC truecolor = dc_for(&rgb);
  assert(w32_gdi_set_desktop(&rgb));
  assert(SelectPalette(truecolor, p, 0));
  assert(SetPixel(truecolor, 0, 0, PALETTERGB(123, 45, 67)) ==
         RGB(123, 45, 67));
  assert(SetPixel(truecolor, 1, 0, PALETTEINDEX(1)) == RGB(1, 2, 3));
  assert(DeleteDC(truecolor));
  assert(SelectPalette(dc, old, 0) == p);
  assert(DeleteObject(p));
  w32_gdi_palette_hook(0, 0);
  test_reset();
  puts("PASS palettes");
}
static void drawing(void) {
  uint32_t pixels[64 * 48] = {0};
  struct w32_dib s = test_dib(pixels, 64, 48, 32, 0, 1);
  HDC dc = dc_for(&s);
  HBRUSH b = CreateSolidBrush(RGB(80, 120, 160));
  assert(SelectObject(dc, b));
  assert(Rectangle(dc, 2, 2, 20, 20));
  assert(GetPixel(dc, 10, 10) == RGB(80, 120, 160));
  assert(GetPixel(dc, 2, 2) == 0);
  assert(GetPixel(dc, 20, 20) == 0);
  assert(Ellipse(dc, 22, 2, 42, 20));
  assert(RoundRect(dc, 2, 22, 25, 44, 8, 10));
  POINT polygon[] = {{40, 22}, {60, 30}, {42, 44}};
  assert(Polygon(dc, polygon, 3));
  assert(MoveToEx(dc, 1, 1, 0));
  assert(LineTo(dc, 30, 30));
  POINT q;
  assert(GetCurrentPositionEx(dc, &q) && q.x == 30 && q.y == 30);
  assert(PolyBezier(dc, (POINT[]){{1, 5}, {10, 40}, {30, 3}, {50, 40}}, 4));
  assert(Arc(dc, 5, 5, 40, 40, 40, 22, 5, 22));
  assert(SelectObject(dc, GetStockObject(NULL_PEN)));
  assert(SelectObject(dc, CreateSolidBrush(RGB(255, 0, 0))));
  assert(Rectangle(dc, 1, 1, 10, 10));
  assert(ExtFloodFill(dc, 5, 5, RGB(255, 0, 0), 1));
  assert(GetPixel(dc, 5, 5) == RGB(255, 0, 0));
  assert(damage_count && last_damage.right <= 64 && last_damage.bottom <= 48);
  memset(pixels, 0, sizeof(pixels));
  assert(SelectObject(dc, CreatePen(PS_INSIDEFRAME, 256, RGB(255, 0, 0))));
  assert(SelectObject(dc, GetStockObject(NULL_BRUSH)));
  assert(Rectangle(dc, 10, 10, 14, 14));
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 64; x++)
      assert(pixels[y * 64 + x] ==
             (x >= 10 && x < 14 && y >= 10 && y < 14 ? 0xff0000u : 0));
  assert(SelectObject(dc, GetStockObject(BLACK_PEN)));
  assert(Rectangle(dc, 2, 2, 3, 3));
  assert(pixels[2 * 64 + 2] == 0);
  test_reset();
  puts("PASS drawing");
}
static void rops(void) {
  for (unsigned r = 0; r < 256; r++)
    for (unsigned trial = 0; trial < 100; trial++) {
      DWORD p = (DWORD)rand32(), s = (DWORD)rand32(), d = (DWORD)rand32(),
            expected = 0;
      for (unsigned bit = 0; bit < 32; bit++) {
        unsigned index = (((p >> bit) & 1) << 2) | (((s >> bit) & 1) << 1) |
                         ((d >> bit) & 1);
        expected |= ((r >> index) & 1u) << bit;
      }
      assert(w32_gdi_rop3(r, p, s, d) == expected);
    }
  uint8_t pixels[512], before[512];
  memset(pixels, 0xa5, sizeof(pixels));
  struct w32_dib d = test_dib(pixels, 12, 7, 32, 5, 1);
  memcpy(before, pixels, sizeof(pixels));
  test_fail_after = 0;
  assert(!w32_dib_blt(&d, 1, 1, 10, 5, &d, 0, 0, 10, 5, 0xcc, 3, 0, (POINT){0},
                      0, -1, 0, 0));
  test_fail_after = -1;
  assert(!memcmp(pixels, before, sizeof(pixels)));
  assert(w32_dib_blt(&d, 1, 1, 10, 5, &d, 0, 0, 10, 5, 0xcc, 3, 0, (POINT){0},
                     0, -1, 0, 0));
  for (int y = 1; y < 6; y++)
    for (int x = 1; x < 11; x++)
      assert(w32_dib_raw(&d, x, y) == 0xa5a5a5a5u);
  struct w32_dib bad = d;
  bad.pitch = 2;
  assert(!w32_dib_blt(&bad, 0, 0, 1, 1, &d, 0, 0, 1, 1, 0xcc, 3, 0, (POINT){0},
                      0, -1, 0, 0));
  test_reset();
  puts("PASS rops");
}
static void *thread_test(void *unused) {
  (void)unused;
  for (unsigned i = 0; i < 200; i++) {
    HDC dc = CreateCompatibleDC(0);
    assert(dc);
    HBRUSH b = CreateSolidBrush(i);
    assert(b);
    HBRUSH old = SelectObject(dc, b);
    assert(old);
    assert(PatBlt(dc, 0, 0, 1, 1, PATCOPY));
    assert(DeleteDC(dc));
    assert(DeleteObject(b));
  }
  return 0;
}
static void concurrency(void) {
  pthread_t a, b;
  assert(!pthread_create(&a, 0, thread_test, 0));
  assert(!pthread_create(&b, 0, thread_test, 0));
  pthread_join(a, 0);
  pthread_join(b, 0);
  test_reset();
  puts("PASS concurrency");
}
static int mock_metrics(void *face, unsigned em,
                        struct w32_gdi_font_metrics *out) {
  (void)face;
  (void)em;
  *out = (struct w32_gdi_font_metrics){6 * 64, 2 * 64, 0, 8 * 64};
  return 1;
}
static int mock_raster(void *face, unsigned em, DWORD cp, int index,
                       struct w32_gdi_glyph *out) {
  (void)face;
  (void)em;
  (void)cp;
  (void)index;
  *out = (struct w32_gdi_glyph){.advance = 8 * 64};
  return 1;
}
static void mock_free(void *face, struct w32_gdi_glyph *g) {
  (void)face;
  assert(!g->coverage);
}
static void mock_retain(void *face) { (*(int *)face)++; }
static void mock_release(void *face) { (*(int *)face)--; }
static int mock_object(void *face, int bytes, void *out, int wide) {
  (void)face;
  if (!out)
    return sizeof(DWORD);
  if (bytes < (int)sizeof(DWORD))
    return 0;
  *(DWORD *)out = 0xab000000u + (unsigned)wide;
  return sizeof(DWORD);
}
static void font_boundary(void) {
  const struct w32_gdi_font_provider provider = {.metrics = mock_metrics,
                                                 .raster = mock_raster,
                                                 .free_glyph = mock_free,
                                                 .retain = mock_retain,
                                                 .release = mock_release,
                                                 .get_object = mock_object};
  int refs = 0;
  HFONT font = w32_gdi_create_font(&provider, &refs, 12 * 64);
  assert(font && refs == 1);
  DWORD metadata;
  assert(GetObjectA(font, sizeof(metadata), &metadata) == sizeof(metadata) &&
         metadata == 0xab000000u);
  assert(GetObjectW(font, sizeof(metadata), &metadata) == sizeof(metadata) &&
         metadata == 0xab000001u);
  HDC dc = CreateCompatibleDC(0);
  HFONT stock = SelectObject(dc, font);
  assert(stock);
  assert(SaveDC(dc));
  assert(SelectObject(dc, stock) == font);
  assert(!DeleteObject(font));
  assert(RestoreDC(dc, -1));
  SIZE size;
  assert(GetTextExtentPoint32A(dc, "ABC", 3, &size) && size.cx == 24 &&
         size.cy == 8);
  int fit = -1, dx[3];
  assert(GetTextExtentExPointA(dc, "ABC", 3, 17, &fit, dx, &size) && fit == 2 &&
         dx[2] == 24);
  assert(DeleteDC(dc));
  assert(DeleteObject(font) && refs == 0);
  assert(w32_gdi_stock_font(&provider, &refs, 12 * 64) && refs == 1);
  test_reset();
  assert(refs == 0);
  puts("PASS font boundary");
}
static int left_clip(void *opaque, int x, int y) {
  (void)opaque;
  return x >= 3 && y >= 2;
}
static void child_dc(void) {
  DWORD pixels[32 * 24] = {0};
  struct w32_dib s = test_dib(pixels, 32, 24, 32, 0, 1);
  s.masks[0] = s.masks[1] = s.masks[2] = 0;
  HDC dc = w32_gdi_surface_dc(&s, (POINT){7, 9}, damage, 0);
  assert(dc);
  HRGN visible = CreateRectRgn(7, 9, 20, 20), clip = CreateRectRgn(1, 1, 3, 3),
       out = CreateRectRgn(0, 0, 0, 0);
  assert(w32_gdi_set_visible_region(dc, visible));
  assert(SelectClipRgn(dc, clip));
  assert(GetClipRgn(dc, out) == 1 && EqualRgn(clip, out));
  assert(FillRect(dc, &(RECT){0, 0, 5, 5}, GetStockObject(WHITE_BRUSH)));
  assert(pixels[10 * 32 + 8] == 0xffffff && pixels[9 * 32 + 7] == 0);
  POINT point = {1, 1};
  assert(LPtoDP(dc, &point, 1) && point.x == 1 && point.y == 1);
  assert(GetDCOrgEx(dc, &point) && point.x == 7 && point.y == 9);
  assert(w32_gdi_set_screen_origin(dc, (POINT){107, 209}));
  assert(GetDCOrgEx(dc, &point) && point.x == 107 && point.y == 209);
  assert(SetViewportOrgEx(dc, 1, 2, 0));
  assert(SetPixel(dc, 0, 0, RGB(1, 2, 3)) == RGB(1, 2, 3));
  assert(pixels[11 * 32 + 8] == 0x010203);
  assert(GetClipBox(dc, &(RECT){0}));
  assert(IntersectClipRect(dc, 0, 0, 1, 1));
  assert(SetPixel(dc, 1, 1, RGB(4, 5, 6)) == CLR_INVALID);
  test_reset();
  puts("PASS child DC");
}
static void transfers(void) {
  uint8_t src_pixels[256], dst_pixels[512];
  struct w32_dib src = test_dib(src_pixels, 7, 5, 32, 5, 1),
                 dst = test_dib(dst_pixels, 9, 7, 32, 5, 1);
  for (int y = 0; y < 5; y++)
    for (int x = 0; x < 7; x++)
      w32_dib_put(&src, x, y, (DWORD)(x + y * 7 + 1));
  for (int direction = 0; direction < 4; direction++) {
    memset(dst_pixels, 0, sizeof(dst_pixels));
    int negative_x = direction & 1, negative_y = direction & 2;
    assert(w32_dib_blt(&dst, negative_x ? 9 : 0, negative_y ? 7 : 0,
                       negative_x ? -9 : 9, negative_y ? -7 : 7, &src, 0, 0, 7,
                       5, 0xcc, COLORONCOLOR, 0, (POINT){0}, 0, -1, left_clip,
                       0));
    for (int y = 0; y < 7; y++)
      for (int x = 0; x < 9; x++) {
        DWORD expected =
            x < 3 || y < 2
                ? 0
                : (DWORD)(((negative_x ? 8 - x : x) * 7 / 9) +
                          ((negative_y ? 6 - y : y) * 5 / 7) * 7 + 1);
        assert(w32_dib_raw(&dst, x, y) == expected);
      }
  }
  memset(dst_pixels, 0, sizeof(dst_pixels));
  assert(w32_dib_blt(&dst, 0, 0, 7, 5, &src, 7, 5, -7, -5, 0xcc, 3, 0,
                     (POINT){0}, 0, -1, 0, 0));
  for (int y = 0; y < 5; y++)
    for (int x = 0; x < 7; x++)
      assert(w32_dib_raw(&dst, x, y) == (DWORD)(6 - x + (4 - y) * 7 + 1));
  HDC dc = dc_for(&dst), source = dc_for(&src);
  uint8_t mask_bits[10] = {0xaa, 0, 0x55, 0, 0xaa, 0, 0x55, 0, 0xaa, 0};
  HBITMAP mask = CreateBitmap(7, 5, 1, 1, mask_bits);
  assert(mask);
  memset(dst_pixels, 0, sizeof(dst_pixels));
  assert(MaskBlt(dc, 0, 0, 7, 5, source, 0, 0, mask, 0, 0, 0xaacc0020u));
  for (int y = 0; y < 5; y++)
    for (int x = 0; x < 7; x++)
      assert(
          w32_dib_raw(&dst, x, y) ==
          (((mask_bits[y * 2] >> (7 - x)) & 1) ? (DWORD)(x + y * 7 + 1) : 0));
  memset(dst_pixels, 0, sizeof(dst_pixels));
  assert(TransparentBlt(dc, 0, 0, 7, 5, source, 0, 0, 7, 5, RGB(0, 0, 3)));
  assert(w32_dib_raw(&dst, 2, 0) == 0 && w32_dib_raw(&dst, 3, 0) == 4);
  HDC mono = CreateCompatibleDC(dc);
  HBITMAP mono_bitmap = CreateBitmap(7, 5, 1, 1, 0);
  assert(SelectObject(mono, mono_bitmap));
  SetBkColor(source, RGB(0, 0, 3));
  assert(BitBlt(mono, 0, 0, 7, 5, source, 0, 0, SRCCOPY));
  SetTextColor(dc, RGB(80, 90, 100));
  SetBkColor(dc, RGB(200, 210, 220));
  assert(BitBlt(dc, 0, 0, 7, 5, mono, 0, 0, SRCCOPY));
  assert(GetPixel(dc, 2, 0) == RGB(200, 210, 220) &&
         GetPixel(dc, 3, 0) == RGB(80, 90, 100));
  SetBrushOrgEx(dc, -2, 3, 0);
  HBRUSH pattern = CreatePatternBrush(mask);
  assert(pattern);
  assert(SelectObject(dc, pattern));
  SetBkMode(dc, OPAQUE);
  assert(PatBlt(dc, 0, 0, 7, 5, PATCOPY));
  for (int y = 0; y < 5; y++)
    for (int x = 0; x < 7; x++) {
      int xx = (x + 2) % 8, yy = (y - 3) % 8;
      if (yy < 0)
        yy += 8;
      xx %= 7;
      yy %= 5;
      COLORREF expected = ((mask_bits[yy * 2] >> (7 - xx)) & 1)
                              ? RGB(200, 210, 220)
                              : RGB(80, 90, 100);
      assert(GetPixel(dc, x, y) == expected);
    }
  uint8_t input[1] = {2}, output[1] = {0};
  struct w32_dib indexed = test_dib(input, 1, 1, 8, 0, 1);
  indexed.colors[2] = indexed.colors[0];
  struct w32_dib same = indexed;
  same.bits = output;
  assert(w32_dib_blt(&same, 0, 0, 1, 1, &indexed, 0, 0, 1, 1, 0xcc, 3, 0,
                     (POINT){0}, 0, -1, 0, 0) &&
         output[0] == 2);
  test_reset();
  puts("PASS transfers");
}
static void faults(void) {
  for (long fail = 0; fail < 12; fail++) {
    HRGN a = CreateRectRgn(1, 1, 20, 20), b = CreateEllipticRgn(0, 0, 24, 24),
         out = CreateRectRgn(7, 8, 9, 10);
    long before = test_allocations;
    test_fail_after = fail;
    int result = CombineRgn(out, a, b, RGN_XOR);
    test_fail_after = -1;
    if (!result) {
      RECT box;
      assert(GetRgnBox(out, &box) == SIMPLEREGION && box.left == 7 &&
             box.top == 8);
      assert(test_allocations == before);
    }
    test_reset();
  }
  HDC dc = CreateCompatibleDC(0);
  HRGN clip = CreateRectRgn(0, 0, 1, 1);
  assert(SelectClipRgn(dc, clip));
  long before = test_allocations;
  test_fail_after = 1;
  assert(!SaveDC(dc));
  test_fail_after = -1;
  assert(test_allocations == before);
  assert(!RestoreDC(dc, -1));
  RGNDATA *data = malloc(GetRegionData(clip, 0, 0));
  DWORD bytes = GetRegionData(clip, 0, 0);
  assert(GetRegionData(clip, bytes, data));
  DWORD rotate[6] = {0, 0x3f800000, 0xbf800000, 0, 0x41200000, 0x41a00000};
  HRGN transformed = ExtCreateRegion(rotate, bytes, data);
  assert(transformed);
  RECT box;
  assert(GetRgnBox(transformed, &box) == SIMPLEREGION);
  assert(box.left == 9 && box.top == 20 && box.right == 10 && box.bottom == 21);
  rotate[0] = 0x7fc00000;
  assert(!ExtCreateRegion(rotate, bytes, data));
  data->rdh.nCount = 0xffffffff;
  assert(!ExtCreateRegion(0, bytes, data));
  free(data);
  test_reset();
  puts("PASS faults");
}
static void text(const char *font_path) {
  uint32_t pixels[160 * 100] = {0};
  struct w32_dib s = test_dib(pixels, 160, 100, 32, 0, 1);
  HDC dc = dc_for(&s);
  HFONT font = w32_gdi_native_font_open(font_path, 12 * 64);
  assert(font);
  assert(SelectObject(dc, font));
  SIZE a, b;
  assert(GetTextExtentPoint32A(dc, "A\x80\xe9", 3, &a));
  assert(GetTextExtentPoint32W(dc, (WCHAR[]){'A', 0x20ac, 0xe9}, 3, &b));
  assert(a.cx == b.cx && a.cy == b.cy);
  assert(SetTextAlign(dc, TA_RIGHT | TA_BASELINE) == 0);
  assert(TextOutA(dc, 100, 30, "Ciuki", 5));
  assert(GetTextAlign(dc) == (TA_RIGHT | TA_BASELINE));
  assert(SetTextAlign(dc, TA_UPDATECP | TA_BASELINE) != (unsigned)-1);
  assert(MoveToEx(dc, 20, 50, 0));
  int dx[] = {8, 9, 10};
  assert(ExtTextOutA(dc, 999, 999, ETO_CLIPPED | ETO_OPAQUE,
                     &(RECT){10, 20, 90, 70}, "ABC", 3, dx));
  POINT p;
  assert(GetCurrentPositionEx(dc, &p) && p.x == 47 && p.y == 50);
  RECT calc = {0, 0, 50, 100};
  assert(w32_gdi_draw_text(dc, "&Classic\tCiuki\nOne woof", -1, &calc,
                           DT_WORDBREAK | DT_EXPANDTABS | DT_CALCRECT, 0,
                           0) > 0);
  assert(calc.bottom > 12 && calc.right > 0);
  RECT layout = {1, 1, 100, 90};
  assert(w32_gdi_draw_text(dc, "&Classic computing, one woof at a time.", -1,
                           &layout, DT_WORDBREAK, 0, 0));
  char ell[64] = "C:\\WINDOWS\\SYSTEM\\ciuki.exe";
  assert(w32_gdi_draw_text(dc, ell, -1, &(RECT){0, 70, 90, 95},
                           DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_MODIFYSTRING,
                           0, 0));
  assert(strstr(ell, "..."));
  assert(!TextOutA(dc, 1, 1, "bad", -1));
  assert(!DeleteObject(font));
  test_reset();
  puts("PASS text");
}
/* f5-10C2 baseline: glyph rows against the outline box read independently
 * from the font file (head.unitsPerEm, loca/glyf yMin/yMax), with the
 * Win32 convention (GLYPHMETRICS.gmptGlyphOrigin.y above the baseline). */
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
         p[3];
}
static const uint8_t *sfnt_table(const uint8_t *d, size_t n, const char *tag) {
  unsigned count = be16(d + 4);
  for (unsigned i = 0; i < count && 12 + 16 * (i + 1) <= n; i++)
    if (!memcmp(d + 12 + 16 * i, tag, 4)) {
      uint32_t at = be32(d + 12 + 16 * i + 8);
      assert(at < n);
      return d + at;
    }
  abort();
}
static void outline_box(const uint8_t *d, size_t n, unsigned glyph,
                        int *ymin, int *ymax, unsigned *upem) {
  const uint8_t *head = sfnt_table(d, n, "head"), *loca = sfnt_table(d, n, "loca"),
                *glyf = sfnt_table(d, n, "glyf");
  *upem = be16(head + 18);
  uint32_t at = be16(head + 50) ? be32(loca + 4 * glyph)
                                : 2u * be16(loca + 2 * glyph);
  const uint8_t *g = glyf + at;
  assert(g + 10 <= d + n);
  *ymin = (int16_t)be16(g + 4);
  *ymax = (int16_t)be16(g + 8);
}
static void ink_rows(const uint32_t *pixels, int w, int h, int *top,
                     int *bottom) {
  *top = h, *bottom = -1;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (pixels[y * w + x] & 0xffffff) {
        if (y < *top)
          *top = y;
        *bottom = y;
      }
}
static void baseline(const char *font_path) {
  FILE *file = fopen(font_path, "rb");
  assert(file);
  static uint8_t data[1 << 20];
  size_t n = fread(data, 1, sizeof(data), file);
  fclose(file);
  assert(n > 64 && n < sizeof(data));
  struct ciuki_font_face *face;
  assert(!ciuki_font_open(font_path, &face));
  static uint32_t pixels[64 * 80];
  struct w32_dib s = test_dib(pixels, 64, 80, 32, 0, 1);
  HDC dc = dc_for(&s);
  HFONT font = w32_gdi_native_font_open(font_path, 12 * 64);
  assert(font && SelectObject(dc, font));
  SetTextColor(dc, RGB(255, 255, 255));
  SetBkMode(dc, TRANSPARENT);
  struct ciuki_font_size_metrics m;
  assert(!ciuki_font_get_size_metrics(face, 12 * 64, &m));
  int ascent = (m.ascent + 63) / 64;
  const char probes[] = {'H', 'x', 'g', 'p'};
  int pinned[4][2];
  for (unsigned i = 0; i < 4; i++) {
    unsigned glyph, upem;
    int ymin, ymax, top, bottom, top2, bottom2;
    assert(!ciuki_font_lookup(face, (unsigned char)probes[i], &glyph));
    outline_box(data, n, glyph, &ymin, &ymax, &upem);
    /* TA_BASELINE at y=40: rows above the baseline are 39, 38, ... */
    memset(pixels, 0, sizeof(pixels));
    SetTextAlign(dc, TA_BASELINE);
    assert(TextOutA(dc, 4, 40, &probes[i], 1));
    ink_rows(pixels, 64, 80, &top, &bottom);
    int want_top = 40 - (ymax * 12 + (int)upem / 2) / (int)upem;
    int want_bottom = 40 - (ymin * 12 + (int)upem / 2) / (int)upem - 1;
    printf("GLYPH %c top=%d bottom=%d outline_top=%d outline_bottom=%d\n",
           probes[i], top, bottom, want_top, want_bottom);
    /* Grid fitting moves edges by at most one pixel (FreeType glyph
     * conventions); a flat bottom on the baseline stays exactly on it. */
    assert(top >= want_top - 1 && top <= want_top + 1);
    assert(bottom >= want_bottom - 1 && bottom <= want_bottom + 1);
    if (!ymin)
      assert(bottom == 39);
    if (ymin < 0)
      assert(bottom >= 40);
    /* TA_TOP at y=4 puts the baseline at 4 + ascent. */
    memset(pixels, 0, sizeof(pixels));
    SetTextAlign(dc, TA_TOP);
    assert(TextOutA(dc, 4, 4, &probes[i], 1));
    ink_rows(pixels, 64, 80, &top2, &bottom2);
    assert(top2 - top == 4 + ascent - 40 && bottom2 - bottom == 4 + ascent - 40);
    pinned[i][0] = top, pinned[i][1] = bottom;
  }
  /* Pinned rows: Liberation Sans Regular 2.1.5, 12 px em, FreeType 2.13.3
   * v35, baseline y=40. */
  static const int expected[4][2] = {{31, 39}, {33, 39}, {33, 41}, {33, 41}};
  assert(!memcmp(pinned, expected, sizeof(expected)));
  assert(!DeleteObject(font));
  ciuki_font_close(face);
  test_reset();
  puts("PASS baseline");
}
static DWORD authored(unsigned x, unsigned y) {
  return (((x * 37 + y * 19) & 255) << 16) | (((x * 13 + y * 43) & 255) << 8) |
         ((x * 61 + y * 7) & 255);
}
static void frames(const char *folder, const char *font_path) {
  uint8_t source_bytes[4096], dest_bytes[4096], pattern_bytes[256];
  struct w32_dib src = test_dib(source_bytes, 17, 11, 32, 5, 1),
                 pattern = test_dib(pattern_bytes, 8, 8, 32, 0, 1);
  for (int y = 0; y < 11; y++)
    for (int x = 0; x < 17; x++)
      w32_dib_put(&src, x, y, authored((unsigned)x, (unsigned)y));
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++)
      w32_dib_put(&pattern, x, y, authored((unsigned)x + 2, (unsigned)y + 5));
  char name[100];
  for (unsigned depth = 16; depth <= 32; depth += 16)
    for (unsigned r = 0; r < 256; r++) {
      memset(dest_bytes, 0xa5, sizeof(dest_bytes));
      struct w32_dib dst = test_dib(dest_bytes, 17, 11, depth, 7, 1);
      for (int y = 0; y < 11; y++)
        for (int x = 0; x < 17; x++)
          w32_dib_put(
              &dst, x, y,
              w32_dib_encode(&dst, authored((unsigned)x + 7, (unsigned)y + 9)));
      assert(w32_dib_blt(&dst, 0, 0, 17, 11, &src, 0, 0, 17, 11, r, 3, &pattern,
                         (POINT){-3, 2}, 0, -1, 0, 0));
      snprintf(name, sizeof(name), depth == 16 ? "rop16-%03u" : "rop-%03u", r);
      test_save(folder, name, &dst);
    }
  unsigned depths[] = {1, 4, 8, 16, 24, 32, 16};
  for (unsigned si = 0; si < 7; si++)
    for (unsigned di = 0; di < 2; di++)
      for (int top = 0; top < 2; top++) {
        memset(source_bytes, 0xc3, sizeof(source_bytes));
        memset(dest_bytes, 0xa5, sizeof(dest_bytes));
        src = test_dib(source_bytes, 17, 11, depths[si], 5, top);
        if (si == 6) {
          src.masks[0] = 0x7c00;
          src.masks[1] = 0x3e0;
        }
        struct w32_dib dst = test_dib(dest_bytes, 17, 11, di ? 32 : 16, 7, 1);
        for (int y = 0; y < 11; y++)
          for (int x = 0; x < 17; x++) {
            DWORD v =
                src.bpp <= 8
                    ? ((unsigned)x * 7 + (unsigned)y * 11) % src.color_count
                    : w32_dib_encode(&src, authored((unsigned)x, (unsigned)y));
            w32_dib_put(&src, x, y, v);
          }
        assert(w32_dib_blt(&dst, 0, 0, 17, 11, &src, 0, 0, 17, 11, 0xcc, 3, 0,
                           (POINT){0}, 0, -1, 0, 0));
        snprintf(name, sizeof(name),
                 si == 6 ? "format555-%u-%u-%d" : "format-%u-%u-%d", src.bpp,
                 dst.bpp, top);
        test_save(folder, name, &dst);
      }
  src = test_dib(source_bytes, 17, 11, 32, 5, 1);
  for (int y = 0; y < 11; y++)
    for (int x = 0; x < 17; x++)
      w32_dib_put(&src, x, y, authored((unsigned)x, (unsigned)y));
  for (unsigned mode = 1; mode <= 4; mode++)
    for (unsigned size = 0; size < 3; size++) {
      int widths[] = {9, 25, 34}, heights[] = {7, 17, 22};
      memset(dest_bytes, 0xa5, sizeof(dest_bytes));
      struct w32_dib dst = test_dib(dest_bytes, (unsigned)widths[size],
                                    (unsigned)heights[size], 32, 7, 1);
      assert(w32_dib_blt(&dst, 0, 0, widths[size], heights[size], &src, 0, 0,
                         17, 11, 0xcc, mode, 0, (POINT){0}, 0, -1, 0, 0));
      snprintf(name, sizeof(name), "stretch-%u-%u", mode, size);
      test_save(folder, name, &dst);
    }
  for (int mode = 1; mode <= 5; mode++) {
    memset(dest_bytes, 0xa5, sizeof(dest_bytes));
    struct w32_dib dst = test_dib(dest_bytes, 32, 24, 32, 7, 1);
    HDC dc = dc_for(&dst);
    HRGN a = CreateEllipticRgn(1, 1, 22, 21), b = CreateRectRgn(10, 3, 30, 17),
         r = CreateRectRgn(0, 0, 0, 0);
    assert(CombineRgn(r, a, b, mode));
    assert(FillRgn(dc, r, GetStockObject(WHITE_BRUSH)));
    snprintf(name, sizeof(name), "region-%d", mode);
    test_save(folder, name, &dst);
    test_reset();
  }
  uint8_t *text_pixels = calloc(1, 160 * 80 * 4);
  assert(text_pixels);
  struct w32_dib dst = test_dib(text_pixels, 160, 80, 32, 0, 1);
  HDC dc = dc_for(&dst);
  HFONT font = w32_gdi_native_font_open(font_path, 12 * 64);
  assert(font);
  SelectObject(dc, font);
  SetTextColor(dc, RGB(230, 190, 50));
  SetBkColor(dc, RGB(20, 40, 80));
  unsigned aligns[] = {0,
                       TA_RIGHT,
                       TA_CENTER,
                       TA_BASELINE,
                       TA_BOTTOM,
                       TA_CENTER | TA_BASELINE,
                       TA_RIGHT | TA_BOTTOM,
                       TA_RIGHT | TA_BASELINE,
                       TA_CENTER | TA_BOTTOM,
                       TA_UPDATECP | TA_BASELINE};
  for (unsigned i = 0; i < sizeof(aligns) / sizeof(aligns[0]); i++) {
    memset(text_pixels, 0, 160 * 80 * 4);
    SetTextAlign(dc, aligns[i]);
    MoveToEx(dc, 80, 40, 0);
    assert(TextOutA(dc, 80, 40, "Ciuki \x80\xe9", 8));
    snprintf(name, sizeof(name), "text-align-%u", aligns[i]);
    test_save(folder, name, &dst);
  }
  test_reset();
  free(text_pixels);
  uint8_t indexes[17 * 11];
  unsigned entries[4] = {10, 11, 249, 12};
  for (unsigned i = 0; i < sizeof(indexes); i++)
    indexes[i] = (uint8_t)entries[i % 4];
  struct w32_dib indexed = test_dib(indexes, 17, 11, 8, 0, 1),
                 rgb = test_dib(dest_bytes, 17, 11, 32, 7, 1);
  HDC indexed_dc = dc_for(&indexed), rgb_dc = dc_for(&rgb);
  w32_gdi_set_desktop(&indexed);
  struct {
    uint16_t version, count;
    PALETTEENTRY colors[4];
  } pal = {0x300,
           4,
           {{1, 2, 3, PC_RESERVED},
            {1, 2, 3, PC_NOCOLLAPSE},
            {255, 0, 0, 0},
            {4, 5, 6, 0}}};
  HPALETTE hp = CreatePalette((LOGPALETTE *)&pal);
  assert(hp && SelectPalette(indexed_dc, hp, 0));
  assert(RealizePalette(indexed_dc) > 0);
  memset(dest_bytes, 0xa5, sizeof(dest_bytes));
  assert(BitBlt(rgb_dc, 0, 0, 17, 11, indexed_dc, 0, 0, SRCCOPY));
  test_save(folder, "palette-realize", &rgb);
  assert(AnimatePalette(hp, 0, 1, (PALETTEENTRY[]){{200, 100, 50, 0}}));
  memset(dest_bytes, 0xa5, sizeof(dest_bytes));
  assert(BitBlt(rgb_dc, 0, 0, 17, 11, indexed_dc, 0, 0, SRCCOPY));
  test_save(folder, "palette-animate", &rgb);
  test_reset();
  puts("PASS frames");
}
static uint64_t nanoseconds(void) {
  struct timespec t;
  assert(!clock_gettime(CLOCK_MONOTONIC, &t));
  return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}
static void benchmark(void) {
  uint8_t *indexed = malloc(640 * 480), *pixels = calloc(1, 640 * 480 * 4),
          *source = calloc(1, 640 * 480 * 4);
  assert(indexed && pixels && source);
  memset(indexed, 42, 640 * 480);
  struct w32_dib dst = test_dib(pixels, 640, 480, 32, 0, 1),
                 src = test_dib(source, 640, 480, 32, 0, 1);
  HDC dc = dc_for(&dst), sdc = dc_for(&src);
  struct info256 info = {.h = {40, 640, -480, 1, 8, 0, 0, 0, 0, 256, 0}};
  for (unsigned i = 0; i < 256; i++)
    info.colors[i] =
        (RGBQUAD){(uint8_t)i, (uint8_t)(i ^ 85), (uint8_t)(255 - i), 0};
  uint64_t start = nanoseconds();
  for (unsigned i = 0; i < 12; i++)
    assert(StretchDIBits(dc, 0, 0, 640, 480, 0, 0, 640, 480, indexed,
                         (BITMAPINFO *)&info, 0, SRCCOPY) == 480);
  printf("COST StretchDIBits-8-to-32 ns/frame=%llu\n",
         (unsigned long long)((nanoseconds() - start) / 12));
  start = nanoseconds();
  for (unsigned i = 0; i < 12; i++)
    assert(BitBlt(dc, 0, 0, 640, 480, sdc, 0, 0, SRCCOPY));
  printf("COST BitBlt-SRCCOPY-32 ns/frame=%llu\n",
         (unsigned long long)((nanoseconds() - start) / 12));
  test_reset();
  free(source);
  free(pixels);
  free(indexed);
}
/* f7 (Quake III Arena): display device capabilities of display DCs, driver
 * pixel formats and device gamma ramps. USER's value-only queries are the
 * weak seams of the host model; they answer only while a case enables them. */
#include <w32/pixelformat.h>
static DWORD q3_display[3], q3_mode[3];
static int q3_display_on, q3_mode_on;
BOOL W32API w32_user_display_caps(DWORD *out) {
  if (!q3_display_on)
    return 0;
  memcpy(out, q3_display, sizeof(q3_display));
  return 1;
}
BOOL W32API w32_user_display_mode(DWORD *out) {
  if (!q3_mode_on)
    return 0;
  if (out)
    memcpy(out, q3_mode, sizeof(q3_mode));
  return 1;
}
static void quake3(void) {
  /* Screen DC of GetDC(NULL) is a memory DC: before the fix it reported its
   * 1x1 monochrome default bitmap (Quake III then asked "Low Desktop Color
   * Depth" and used 1x1 as the desktop size). */
  HDC screen = CreateCompatibleDC(0);
  assert(screen);
  assert(GetDeviceCaps(screen, 12) == 1 && GetDeviceCaps(screen, 8) == 1);
  q3_display_on = 1;
  q3_display[0] = 1024, q3_display[1] = 768, q3_display[2] = 32;
  /* (a failed query is never cached) */
  assert(GetDeviceCaps(screen, 12) == 32);   /* BITSPIXEL */
  assert(GetDeviceCaps(screen, 8) == 1024);  /* HORZRES */
  assert(GetDeviceCaps(screen, 10) == 768);  /* VERTRES */
  assert(GetDeviceCaps(screen, 24) == -1);   /* NUMCOLORS, direct colour */
  assert(GetDeviceCaps(screen, 2) == 1);     /* TECHNOLOGY: DT_RASDISPLAY */
  uint32_t pixels[16 * 8] = {0};
  struct w32_dib surface = test_dib(pixels, 16, 8, 32, 0, 1);
  HDC window = dc_for(&surface);
  assert(GetDeviceCaps(window, 8) == 1024 && GetDeviceCaps(window, 12) == 32);
  /* Driver formats (ten since f7-08: XRGB/RGB565 pairs with D16/S0 double
   * and single buffered, then D24/S0, D16/S8, D24/S8): never
   * PFD_GENERIC_FORMAT, depth of at least 15 bits (Quake III GLW_ChoosePFD
   * keeps them and takes format 9 for its 32/24/8 request). */
  PIXELFORMATDESCRIPTOR pfd;
  assert(DescribePixelFormat(window, 1, 40, &pfd) == 10);
  for (int n = 1; n <= 10; n++) {
    assert(DescribePixelFormat(window, n, 40, &pfd) == 10);
    assert(!(pfd.dwFlags & PFD_GENERIC_FORMAT) && pfd.iPixelType == PFD_TYPE_RGBA);
    assert(pfd.cDepthBits >= 15 && (pfd.dwFlags & PFD_SUPPORT_OPENGL));
    assert(!!(pfd.dwFlags & PFD_DOUBLEBUFFER) == (n != 3 && n != 4));
    assert(pfd.cColorBits == (n & 1 ? 32 : 16) && pfd.cStencilBits == (n >= 7 ? 8 : 0));
  }
  PIXELFORMATDESCRIPTOR want = {.nSize = 40, .nVersion = 1, .iPixelType = PFD_TYPE_RGBA,
                                .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                .cColorBits = 32, .cDepthBits = 24, .cStencilBits = 8};
  assert(ChoosePixelFormat(window, &want) == 9);
  /* Gamma ramps: no presenter, or no full-screen mode, means no device ramp
   * (FALSE, the program keeps its software gamma). */
  uint16_t ramp[3][256], set[3][256], copy[768];
  assert(!GetDeviceGammaRamp(screen, ramp) && !SetDeviceGammaRamp(screen, ramp));
  assert(w32_gdi_gamma_presenter(1) == 1);
  assert(!GetDeviceGammaRamp(screen, ramp));
  q3_mode_on = 1;
  q3_mode[0] = 640, q3_mode[1] = 480, q3_mode[2] = 32;
  assert(!GetDeviceGammaRamp(screen, 0) && !GetDeviceGammaRamp(0, ramp));
  assert(w32_gdi_gamma(copy) == 0);
  assert(GetDeviceGammaRamp(screen, ramp));
  for (unsigned c = 0; c < 3; c++)
    for (unsigned i = 0; i < 256; i++) {
      assert(ramp[c][i] == i * 257u);
      set[c][i] = (uint16_t)(i * 200u + c);
    }
  assert(SetDeviceGammaRamp(window, set));
  assert(w32_gdi_gamma(copy) == 1 && !memcmp(copy, set, sizeof(set)));
  assert(GetDeviceGammaRamp(screen, ramp) && !memcmp(ramp, set, sizeof(set)));
  assert(SetDeviceGammaRamp(screen, set) && w32_gdi_gamma(0) == 2);
  assert(w32_gdi_gamma_presenter(0) == 0);
  assert(!GetDeviceGammaRamp(screen, ramp) && !SetDeviceGammaRamp(screen, set));
  q3_mode_on = 0;
  DeleteDC(window);
  DeleteDC(screen);
  puts("PASS quake3 display caps, driver pixel formats, device gamma ramps");
}
int main(int argc, char **argv) {
  assert(argc >= 2);
  if (!strcmp(argv[1], "lifetime"))
    lifetime();
  else if (!strcmp(argv[1], "regions"))
    regions();
  else if (!strcmp(argv[1], "state"))
    state();
  else if (!strcmp(argv[1], "bitmaps"))
    bitmaps();
  else if (!strcmp(argv[1], "palettes"))
    palettes();
  else if (!strcmp(argv[1], "drawing"))
    drawing();
  else if (!strcmp(argv[1], "rops"))
    rops();
  else if (!strcmp(argv[1], "concurrency"))
    concurrency();
  else if (!strcmp(argv[1], "transfers"))
    transfers();
  else if (!strcmp(argv[1], "child"))
    child_dc();
  else if (!strcmp(argv[1], "font-boundary"))
    font_boundary();
  else if (!strcmp(argv[1], "faults"))
    faults();
  else if (!strcmp(argv[1], "quake3"))
    quake3();
  else if (!strcmp(argv[1], "text")) {
    assert(argc == 3);
    text(argv[2]);
  } else if (!strcmp(argv[1], "baseline")) {
    assert(argc == 3);
    baseline(argv[2]);
  } else if (!strcmp(argv[1], "frames")) {
    assert(argc == 4);
    frames(argv[2], argv[3]);
  } else if (!strcmp(argv[1], "benchmark"))
    benchmark();
  else
    abort();
  return 0;
}
