/* Host allocation and recorded transport only; COM/GDI execute production code.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <w32/ddraw.h>
#include <w32/services.h>

long host_allocations, host_bytes, host_fail_after = -1;
struct allocation {
  struct allocation *next;
  void *data;
  DWORD bytes;
};
static struct allocation *allocations;
static void *allocate(void *opaque, DWORD bytes) {
  (void)opaque;
  if (host_fail_after == 0)
    return 0;
  if (host_fail_after > 0)
    host_fail_after--;
  uint8_t *p = malloc(bytes + 64);
  struct allocation *a = malloc(sizeof(*a));
  assert(p && a);
  memset(p, 0xa5, bytes + 64);
  a->data = p + 32;
  a->bytes = bytes;
  a->next = allocations;
  allocations = a;
  host_allocations++;
  host_bytes += bytes;
  return a->data;
}
void host_canaries(void) {
  for (struct allocation *a = allocations; a; a = a->next)
    for (unsigned i = 0; i < 32; i++) {
      assert(((uint8_t *)a->data)[(int)i - 32] == 0xa5);
      assert(((uint8_t *)a->data)[a->bytes + i] == 0xa5);
    }
}
static void release(void *opaque, void *p, DWORD bytes) {
  (void)opaque;
  host_canaries();
  struct allocation **at = &allocations;
  while (*at && (*at)->data != p)
    at = &(*at)->next;
  assert(*at && (*at)->bytes == bytes);
  struct allocation *a = *at;
  *at = a->next;
  free((uint8_t *)p - 32);
  free(a);
  host_allocations--;
  host_bytes -= bytes;
}
static DWORD tid(void *p) {
  (void)p;
  return (DWORD)syscall(SYS_gettid);
}
static void wait_word(void *p, volatile DWORD *at, DWORD value) {
  (void)p;
  syscall(SYS_futex, at, 128, value, 0, 0, 0);
}
static void wake_word(void *p, volatile DWORD *at) {
  (void)p;
  syscall(SYS_futex, at, 129, 0x7fffffff, 0, 0, 0);
}
static const struct w32_services services = {.alloc = allocate,
                                             .free = release,
                                             .thread_id = tid,
                                             .wait_word = wait_word,
                                             .wake_word = wake_word};
const struct w32_services *w32_get_services(void) { return &services; }
const struct dd_backend dd_native_backend = {0};
void dd_native_start(void) {}
void dd_native_stop(void) {}
DDRESULT dd_native_window_mode(uintptr_t h, DWORD w, DWORD y) {
  (void)h;
  (void)w;
  (void)y;
  return 0;
}
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
