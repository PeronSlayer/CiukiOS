/* Production gdi32 surfaces for the AVI harness windows, kept apart from
 * the kernel32 model (the headers define overlapping Win32 types). Only
 * allocation/locking services are modelled, as in the gdi32 host tests;
 * kernel32 files come from mci_host.c. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <w32/gdi32.h>
#include <w32/services.h>
static long gdi_bytes;
static void *allocate(void *opaque, uint32_t bytes) {
  (void)opaque;
  void *p = malloc(bytes);
  if (p)
    gdi_bytes += bytes;
  return p;
}
static void release(void *opaque, void *p, uint32_t bytes) {
  (void)opaque;
  if (p) {
    gdi_bytes -= bytes;
    free(p);
  }
}
static uint32_t tid(void *opaque) {
  (void)opaque;
  return (uint32_t)syscall(SYS_gettid);
}
static void wait_word(void *opaque, volatile uint32_t *p, uint32_t value) {
  (void)opaque;
  syscall(SYS_futex, p, 128, value, 0, 0, 0);
}
static void wake_word(void *opaque, volatile uint32_t *p) {
  (void)opaque;
  syscall(SYS_futex, p, 129, 0x7fffffff, 0, 0, 0);
}
static const struct w32_services services = {.alloc = allocate,
                                             .free = release,
                                             .thread_id = tid,
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
BOOL W32API WriteFile(void *h, const void *d, DWORD n, DWORD *put, void *o) {
  (void)h, (void)d, (void)n, (void)put, (void)o;
  return 0;
}
BOOL W32API CopyFileA(const char *a, const char *b, BOOL f) {
  (void)a, (void)b, (void)f;
  return 0;
}
BOOL W32API DeleteFileA(const char *a) {
  (void)a;
  return 0;
}
void W32API SetLastError(DWORD e) { (void)e; }
int w32_stricmp(const char *a, const char *b) {
  while (*a && *b) {
    unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
    if (x >= 'a' && x <= 'z')
      x -= 32;
    if (y >= 'a' && y <= 'z')
      y -= 32;
    if (x != y)
      return x < y ? -1 : 1;
  }
  return (unsigned char)*a - (unsigned char)*b;
}
struct surface {
  struct w32_dib dib;
  uint32_t *pixels;
  HDC dc;
};
static void damage(void *opaque, const RECT *r) {
  (void)opaque;
  (void)r;
}
void *ag_create(int width, int height, uintptr_t *dc, uint32_t **pixels) {
  struct surface *s = calloc(1, sizeof(*s));
  if (!s)
    return 0;
  s->pixels = calloc((size_t)width * (size_t)height, 4);
  if (!s->pixels) {
    free(s);
    return 0;
  }
  s->dib = (struct w32_dib){.bits = (uint8_t *)s->pixels,
                            .width = (DWORD)width,
                            .height = (DWORD)height,
                            .pitch = (DWORD)width * 4,
                            .size = (DWORD)(width * height * 4),
                            .bpp = 32,
                            .top_down = 1,
                            .masks = {0xff0000, 0xff00, 0xff}};
  s->dc = w32_gdi_surface_dc(&s->dib, (POINT){0, 0}, damage, 0);
  if (!s->dc) {
    free(s->pixels);
    free(s);
    return 0;
  }
  *dc = s->dc;
  *pixels = s->pixels;
  return s;
}
void ag_destroy(void *p) {
  struct surface *s = p;
  if (!s)
    return;
  DeleteDC(s->dc);
  free(s->pixels);
  free(s);
}
void ag_exit(void) { w32_gdi_process_exit(); }
long ag_live(void) { return (long)w32_gdi_live_objects() + (gdi_bytes ? 1 : 0); }
