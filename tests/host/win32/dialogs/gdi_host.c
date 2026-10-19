/* Host services for the production gdi32 sources inside the dialogs
 * harness: allocator, locks, views and the native FreeType face backend.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../../sdk/win32/gdi32/native_font.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <w32/services.h>
long gdi_host_allocations, gdi_host_bytes;
static void *allocate(void *opaque, uint32_t bytes) {
  (void)opaque;
  void *p = malloc(bytes ? bytes : 1);
  if (p) {
    gdi_host_allocations++;
    gdi_host_bytes += bytes;
  }
  return p;
}
static void release(void *opaque, void *p, uint32_t bytes) {
  (void)opaque;
  if (p) {
    gdi_host_allocations--;
    gdi_host_bytes -= bytes;
    free(p);
  }
}
static uint32_t tid(void *opaque) {
  (void)opaque;
  return 1;
}
static void wait_word(void *opaque, volatile uint32_t *p, uint32_t value) {
  (void)opaque, (void)p, (void)value;
}
static void wake_word(void *opaque, volatile uint32_t *p) {
  (void)opaque, (void)p;
}
static const struct w32_services services = {.alloc = allocate,
                                             .free = release,
                                             .thread_id = tid,
                                             .wait_word = wait_word,
                                             .wake_word = wake_word};
const struct w32_services *w32_get_services(void) { return &services; }
void *W32API MapViewOfFile(void *h, DWORD access, DWORD high, DWORD offset,
                           DWORD bytes) {
  (void)access, (void)high, (void)bytes;
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
/* The font store view: system faces read-only from the pinned assets, the
 * writable install directory mapped by the harness file service. */
int gdi_host_fonts(const char *system_native, const char *install_native) {
  return w32_gdi_font_store("C:\\WINDOWS\\FONTS", system_native,
                            "C:\\WINDOWS\\FONTS", install_native) &&
         w32_gdi_set_face_backend(w32_gdi_native_backend());
}
int gdi_host_backend(void) {
  return w32_gdi_set_face_backend(w32_gdi_native_backend());
}
