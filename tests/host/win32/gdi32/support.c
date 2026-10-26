/* Only allocation/transport is mocked: every GDI operation is production code.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "support.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <w32/services.h>
long test_allocations, test_bytes, test_fail_after = -1;
static void *allocate(void *opaque, uint32_t bytes) {
  (void)opaque;
  if (!test_fail_after)
    return 0;
  if (test_fail_after > 0)
    test_fail_after--;
  void *p = malloc(bytes);
  if (p) {
    test_allocations++;
    test_bytes += bytes;
  }
  return p;
}
static void release(void *opaque, void *p, uint32_t bytes) {
  (void)opaque;
  if (p) {
    test_allocations--;
    test_bytes -= bytes;
    assert(test_bytes >= 0);
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
static void diagnostic(void *ctx,const char *level,const char *module,const char *line) {
  (void)ctx; (void)level; (void)module;
  extern void test_gl_diagnostic(const char *) __attribute__((weak));
  if (test_gl_diagnostic) test_gl_diagnostic(line);
}
static const struct w32_services services = {.alloc = allocate,
                                             .diagnostic = diagnostic,
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
void test_reset(void) {
  test_fail_after = -1;
  w32_gdi_process_exit();
  assert(w32_gdi_live_objects() == 0);
  assert(test_allocations == 0);
  assert(test_bytes == 0);
}
struct w32_dib test_dib(void *pixels, unsigned width, unsigned height,
                        unsigned bpp, unsigned padding, int top) {
  struct w32_dib d = {.bits = pixels,
                      .width = width,
                      .height = height,
                      .pitch = (width * bpp + 7) / 8 + padding,
                      .bpp = bpp,
                      .top_down = (DWORD)top};
  d.size = d.pitch * height;
  if (bpp <= 8) {
    d.color_count = 1u << bpp;
    for (unsigned i = 0; i < d.color_count; i++)
      d.colors[i] =
          ((i * 53u & 255) << 16) | ((i * 97u & 255) << 8) | (i * 193u & 255);
    if (bpp == 1) {
      d.colors[0] = 0;
      d.colors[1] = 0xffffff;
    }
  } else {
    d.masks[0] = bpp == 16 ? 0xf800 : 0xff0000;
    d.masks[1] = bpp == 16 ? 0x7e0 : 0xff00;
    d.masks[2] = bpp == 16 ? 0x1f : 0xff;
  }
  return d;
}
void test_save(const char *folder, const char *name, const struct w32_dib *d) {
  char path[1024];
  assert(snprintf(path, sizeof(path), "%s/%s.bin", folder, name) <
         (int)sizeof(path));
  FILE *f = fopen(path, "wb");
  assert(f);
  assert(fwrite(d->bits, 1, d->size, f) == d->size);
  assert(!fclose(f));
  printf("FRAME %s %u %u %u %u %u\n", name, d->width, d->height, d->bpp,
         d->pitch, d->size);
}
/* f5-10C selection layer imports. The engine tests install their own
 * providers; the font store is exercised by tests/host/win32/dialogs. */
void *W32API CreateFileA(const char *n, DWORD a, DWORD s, void *sa, DWORD c,
                         DWORD f, void *t) {
  (void)n, (void)a, (void)s, (void)sa, (void)c, (void)f, (void)t;
  return (void *)(intptr_t)-1;
}
BOOL W32API ReadFile(void *h, void *d, DWORD n, DWORD *got, void *o) {
  (void)h, (void)d, (void)n, (void)got, (void)o;
  return 0;
}
BOOL W32API WriteFile(void *h, const void *d, DWORD n, DWORD *put, void *o) {
  (void)h, (void)d, (void)n, (void)put, (void)o;
  return 0;
}
DWORD W32API GetFileSize(void *h, DWORD *high) {
  (void)h, (void)high;
  return 0xffffffffu;
}
DWORD W32API SetFilePointer(void *h, LONG at, LONG *high, DWORD how) {
  (void)h, (void)at, (void)high, (void)how;
  return 0xffffffffu;
}
BOOL W32API CloseHandle(void *h) {
  (void)h;
  return 1;
}
BOOL W32API CopyFileA(const char *a, const char *b, BOOL f) {
  (void)a, (void)b, (void)f;
  return 0;
}
BOOL W32API DeleteFileA(const char *a) {
  (void)a;
  return 0;
}
void *W32API LoadLibraryA(const char *a) {
  (void)a;
  return 0;
}
void *W32API GetProcAddress(void *m, const char *a) {
  (void)m, (void)a;
  return 0;
}
void W32API SetLastError(DWORD e) { (void)e; }
DWORD W32API GetTickCount(void) {
  struct timespec now;
  assert(!clock_gettime(CLOCK_MONOTONIC, &now));
  return (DWORD)((uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

/* G31 additional kernel facade operations; individual spool tests replace I/O. */
__attribute__((weak)) BOOL W32API CreateDirectoryA(const char *p, void *sa) { (void)p; (void)sa; return 0; }
__attribute__((weak)) BOOL W32API RemoveDirectoryA(const char *p) { (void)p; return 0; }
__attribute__((weak)) BOOL W32API MoveFileA(const char *a, const char *b) { (void)a; (void)b; return 0; }
__attribute__((weak)) DWORD W32API GetLastError(void) { return 2; }

__attribute__((weak)) DWORD W32API GetCurrentProcessId(void) { return 1; }

__attribute__((weak)) DWORD W32API GetFullPathNameA(const char *s, DWORD n, char *p, char **f) { (void)s; (void)n; (void)p; (void)f; return 0; }

__attribute__((weak)) BOOL W32API FlushFileBuffers(void *h) { (void)h; return 1; }
