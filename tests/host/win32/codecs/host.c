/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Native H0 adapter: memory file and libc heap, no PE or guest execution.
 */
#include "codec.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t live;
uint32_t test_live(void) { return live; }
static const uint8_t *file_data;
static uint32_t file_size, position;
void test_file(const uint8_t *p, uint32_t n) {
  file_data = p;
  file_size = n;
  position = 0;
}
void *GetProcessHeap(void) { return (void *)1; }
void *HeapAlloc(void *h, uint32_t flags, uint32_t n) {
  (void)h;
  void *p = flags & 8 ? calloc(1, n) : malloc(n);
  if (p)
    live++;
  return p;
}
int HeapFree(void *h, uint32_t flags, void *p) {
  (void)h;
  (void)flags;
  if (p)
    live--;
  free(p);
  return 1;
}
void *CreateFileA(const char *p, uint32_t a, uint32_t share, void *sec,
                  uint32_t create, uint32_t attr, void *t) {
  (void)p;
  (void)a;
  (void)share;
  (void)sec;
  (void)create;
  (void)attr;
  (void)t;
  position = 0;
  return (void *)2;
}
void *CreateFileW(const WCHAR *p, uint32_t a, uint32_t share, void *sec,
                  uint32_t create, uint32_t attr, void *t) {
  return CreateFileA((const char *)p, a, share, sec, create, attr, t);
}
uint32_t GetFileSize(void *h, uint32_t *hi) {
  (void)h;
  if (hi)
    *hi = 0;
  return file_size;
}
uint32_t SetFilePointer(void *h, int32_t p, int32_t *hi, uint32_t method) {
  (void)h;
  (void)hi;
  (void)method;
  if (p < 0 || (uint32_t)p > file_size)
    return ~0u;
  position = (uint32_t)p;
  return position;
}
int ReadFile(void *h, void *out, uint32_t n, uint32_t *done, void *ov) {
  (void)h;
  (void)ov;
  *done = 0;
  if (n > file_size - position)
    return 0;
  memcpy(out, file_data + position, n);
  position += n;
  *done = n;
  return 1;
}
int CloseHandle(void *h) {
  (void)h;
  return 1;
}

int WideCharToMultiByte(uint32_t cp, uint32_t flags, const WCHAR *s, int n,
                        char *out, int cap, const char *def, int *used) {
  (void)cp;
  (void)flags;
  (void)def;
  if (n > cap)
    return 0;
  *used = 0;
  for (int i = 0; i < n; i++) {
    if (s[i] > 255)
      *used = 1;
    out[i] = (char)s[i];
  }
  return n;
}
