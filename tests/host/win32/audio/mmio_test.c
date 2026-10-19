#include "../../../../sdk/win32/audio/mmio.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *wa_alloc(uint32_t n) { return calloc(1, n); }
void wa_free(void *p) { free(p); }
static uint32_t file_position;
HANDLE wa_file_open(const void *p, int wide) {
  (void)p;
  (void)wide;
  file_position = 0;
  return (HANDLE)1;
}
DWORD W32API SetFilePointer(HANDLE h, LONG offset, LONG *high, DWORD origin) {
  (void)h;
  (void)high;
  assert(origin == 0);
  file_position = (uint32_t)offset;
  return file_position;
}
BOOL W32API CloseHandle(HANDLE h) {
  (void)h;
  return 1;
}
DWORD W32API GetFileSize(HANDLE h, DWORD *p) {
  (void)h;
  (void)p;
  return 3 * 1024 * 1024;
}
BOOL W32API ReadFile(HANDLE h, void *p, DWORD n, DWORD *got, OVERLAPPED *ov) {
  (void)h;
  (void)ov;
  for (unsigned i = 0; i < n; i++)
    ((uint8_t *)p)[i] = (uint8_t)(file_position + i);
  file_position += n;
  *got = n;
  return 1;
}

int main(void) {
  uint8_t bytes[48] = {0};
  w32_put32(bytes, 0x46464952);
  w32_put32(bytes + 4, 40);
  w32_put32(bytes + 8, 0x45564157);
  w32_put32(bytes + 12, 0x4b4e554a);
  w32_put32(bytes + 16, 3);
  memcpy(bytes + 20, "abc", 3);
  w32_put32(bytes + 24, 0x61746164);
  w32_put32(bytes + 28, 16);
  for (unsigned i = 0; i < 16; i++)
    bytes[32 + i] = (uint8_t)i;
  struct mmioinfo info = {.fourcc = 0x204d454d,
                          .buffer = (void *)bytes,
                          .buffer_bytes = sizeof(bytes)};
  uintptr_t h = mmioOpenA(0, &info, 0);
  assert(h);
  struct chunk parent = {.type = 0x45564157}, data = {.id = 0x61746164};
  assert(!mmioDescend(h, &parent, 0, 0x20));
  assert(parent.offset == 8 && parent.size == 40);
  assert(!mmioDescend(h, &data, &parent, 0x10));
  assert(data.offset == 32 && data.size == 16);
  char out[20] = {0};
  assert(mmioRead(h, out, 20) == 16 && out[15] == 15);
  assert(!mmioAscend(h, &data, 0));
  assert(mmioSeek(h, -16, 2) == 32);
  assert(!mmioGetInfo(h, &info, 0));
  info.next += 3;
  assert(!mmioSetInfo(h, &info, 0));
  assert(mmioSeek(h, 0, 1) == 35);
  assert(mmioSeek(h, -99, 0) == -1);
  assert(!mmioClose(h, 0));
  assert(mmioRead(h, out, 1) == -1);
  w32_put32(bytes + 28, UINT32_MAX);
  info = (struct mmioinfo){.fourcc = 0x204d454d,
                           .buffer = (void *)bytes,
                           .buffer_bytes = sizeof(bytes)};
  h = mmioOpenA(0, &info, 0);
  assert(h);
  assert(mmioSeek(h, 24, 0) == 24);
  assert(mmioDescend(h, &data, 0, 0x10) == 256);
  mmioClose(h, 0);
  assert(mmioStringToFOURCCA("wave", 0x10) == 0x45564157);
  h = mmioOpenA("large.wav", 0, 0);
  assert(h);
  assert(!mmioGetInfo(h, &info, 0) && info.buffer_bytes == 8192);
  assert(mmioSeek(h, 2 * 1024 * 1024 + 123, 0) == 2 * 1024 * 1024 + 123);
  assert(mmioRead(h, out, 20) == 20 && (uint8_t)out[0] == 123 &&
         (uint8_t)out[19] == 142);
  assert(!mmioGetInfo(h, &info, 0));
  info.next = info.read_end;
  assert(!mmioAdvance(h, &info, 0));
  assert(info.buffer_offset > 2 * 1024 * 1024);
  assert(!mmioClose(h, 0));
  puts("PASS MMIO nested RIFF/padding/seek/ownership/truncation/overflow");
  return 0;
}
