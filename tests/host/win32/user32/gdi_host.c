/* Host services and named exports for the production gdi32 and ddraw objects
 * linked into the USER fixture; every drawing operation is production code.
 * gdi32 handles are DWORDs, USER's are pointer-sized in this host model, so
 * the fixture reaches gdi32 only through these DWORD helpers.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/gdi32.h>
#include <w32/pixelformat.h>
#include <w32/services.h>
#include <linux/futex.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
/* Below 4 GiB: DirectDraw's native adapter passes 32-bit record addresses. */
static void *allocate(void *opaque, uint32_t bytes) {
  (void)opaque;
  uint32_t size = (bytes + 16 + 4095) & ~4095u;
  uint32_t *p = mmap(0, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if (p == MAP_FAILED)
    return 0;
  p[0] = size;
  return p + 4;
}
static void release(void *opaque, void *p, uint32_t bytes) {
  (void)opaque;
  (void)bytes;
  if (p)
    munmap((uint32_t *)p - 4, ((uint32_t *)p - 4)[0]);
}
static uint32_t thread_id(void *opaque) {
  (void)opaque;
  return (uint32_t)syscall(SYS_gettid);
}
static void wait_word(void *opaque, volatile uint32_t *at, uint32_t value) {
  (void)opaque;
  syscall(SYS_futex, at, FUTEX_WAIT_PRIVATE, value, 0, 0, 0);
}
static void wake_word(void *opaque, volatile uint32_t *at) {
  (void)opaque;
  syscall(SYS_futex, at, FUTEX_WAKE_PRIVATE, 0x7fffffff, 0, 0, 0);
}
static const struct w32_services services = {.alloc = allocate,
                                             .free = release,
                                             .thread_id = thread_id,
                                             .wait_word = wait_word,
                                             .wake_word = wake_word};
const struct w32_services *w32_get_services(void) { return &services; }
void *W32API MapViewOfFile(void *h, DWORD access, DWORD high, DWORD offset,
                           DWORD bytes) {
  (void)access;
  (void)high;
  (void)bytes;
  return (uint8_t *)h + offset;
}
BOOL W32API UnmapViewOfFile(const void *p) {
  (void)p;
  return 1;
}
int w32_stricmp(const char *a, const char *b) {
  while (*a && *b) {
    unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
    x -= x >= 'a' && x <= 'z' ? 32 : 0;
    y -= y >= 'a' && y <= 'z' ? 32 : 0;
    if (x != y)
      return x < y ? -1 : 1;
  }
  return (unsigned char)*a - (unsigned char)*b;
}
/* f5-10C font-store imports. user32_test.c supplies CreateFileA (always
 * INVALID_HANDLE_VALUE) and ReadFile/CloseHandle, so the font files are never
 * opened here; the remaining file imports of gdi32/font.c refuse. The font
 * store itself is exercised by tests/host/win32/dialogs. */
DWORD W32API GetFileSize(void *h, DWORD *high) {
  (void)h;
  (void)high;
  return 0xffffffffu;
}
DWORD W32API SetFilePointer(void *h, LONG at, LONG *high, DWORD how) {
  (void)h;
  (void)at;
  (void)high;
  (void)how;
  return 0xffffffffu;
}
BOOL W32API WriteFile(void *h, const void *data, DWORD bytes, DWORD *put,
                      void *o) {
  (void)h;
  (void)data;
  (void)bytes;
  (void)put;
  (void)o;
  return 0;
}
BOOL W32API CopyFileA(const char *from, const char *to, BOOL fail_exists) {
  (void)from;
  (void)to;
  (void)fail_exists;
  return 0;
}
BOOL W32API DeleteFileA(const char *name) {
  (void)name;
  return 0;
}
#define EXPORT(name) {#name, (void *)name}
static const struct {
  const char *name;
  void *proc;
} exports[] = {EXPORT(w32_gdi_surface_dc), EXPORT(w32_gdi_set_visible_region),
               EXPORT(CreateRectRgn),      EXPORT(CombineRgn),
               EXPORT(GetRegionData),      EXPORT(DeleteObject),
               EXPORT(DeleteDC),           EXPORT(SelectObject),
               EXPORT(PatBlt),             EXPORT(CreateSolidBrush),
               EXPORT(CreateDIBSection),   EXPORT(CreateBitmap),
               EXPORT(GetObjectA),         EXPORT(GetDIBits),
               EXPORT(w32_gdi_draw_icon),  EXPORT(SetDIBits),
               EXPORT(CreateCompatibleDC), EXPORT(StretchBlt),
               EXPORT(w32_gdi_palette_hook), EXPORT(w32_gdi_gl_window_format)};
void *host_gdi_proc(const char *name) {
  for (unsigned i = 0; i < sizeof(exports) / sizeof(*exports); i++)
    if (!strcmp(exports[i].name, name))
      return exports[i].proc;
  return 0;
}
/* A W x H 32-bpp top-down DIB section selected into a fresh memory DC. */
DWORD host_canvas(int w, int h, DWORD fill, DWORD *dc, uint32_t **bits) {
  BITMAPINFO info = {{40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
  void *pixels = 0;
  DWORD bitmap = CreateDIBSection(0, &info, 0, &pixels, 0, 0);
  if (!bitmap || !pixels)
    return 0;
  for (int i = 0; i < w * h; i++)
    ((uint32_t *)pixels)[i] = fill;
  *dc = CreateCompatibleDC(0);
  if (!*dc || !SelectObject(*dc, bitmap))
    return 0;
  *bits = pixels;
  return bitmap;
}
void host_canvas_free(DWORD dc, DWORD bitmap) {
  DeleteDC(dc);
  DeleteObject(bitmap);
}
DWORD host_mono(int w, int h, const uint8_t *rows) {
  return CreateBitmap(w, h, 1, 1, rows);
}
DWORD host_color(int w, int h, const uint32_t *pixels) {
  BITMAPINFO info = {{40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0}, {{0, 0, 0, 0}}};
  void *bits = 0;
  DWORD bitmap = CreateDIBSection(0, &info, 0, &bits, 0, 0);
  if (bitmap && bits)
    memcpy(bits, pixels, (size_t)w * h * 4);
  return bitmap;
}
void host_gdi_delete(DWORD object) { DeleteObject(object); }
/* Depth and pixels as read back through GetObject/GetDIBits (32-bpp). */
int host_bitmap(DWORD bitmap, int *w, int *h, int *bpp, uint32_t *out,
                int cap) {
  BITMAP b;
  if (GetObjectA(bitmap, sizeof(b), &b) != sizeof(b))
    return 0;
  *w = b.bmWidth;
  *h = b.bmHeight;
  *bpp = b.bmBitsPixel;
  if (b.bmWidth * b.bmHeight > cap)
    return 0;
  BITMAPINFO info = {{40, b.bmWidth, -b.bmHeight, 1, 32, 0, 0, 0, 0, 0, 0},
                     {{0, 0, 0, 0}}};
  return GetDIBits(0, bitmap, 0, (unsigned)b.bmHeight, out, &info, 0) ==
         b.bmHeight;
}
/* Logical palette realised into DC; returns the palette for AnimatePalette. */
DWORD host_palette(DWORD dc, unsigned *changed) {
  struct {
    uint16_t version, count;
    PALETTEENTRY entries[4];
  } logical = {0x300, 4, {{0x10, 0x20, 0x30, 4}, {0x40, 0x50, 0x60, 4},
                          {0x70, 0x80, 0x90, 4}, {0xa0, 0xb0, 0xc0, 4}}};
  DWORD palette = CreatePalette((const LOGPALETTE *)&logical);
  if (!palette || !SelectPalette(dc, palette, 0))
    return 0;
  *changed = RealizePalette(dc);
  return palette;
}
BOOL host_animate(DWORD palette) {
  PALETTEENTRY entry = {1, 2, 3, 4};
  return AnimatePalette(palette, 1, 1, &entry);
}
void host_gdi_exit(void) { w32_gdi_process_exit(); }
<<<<<<< HEAD

#include "../gdi32/pixel_format.inc"
=======
>>>>>>> main
