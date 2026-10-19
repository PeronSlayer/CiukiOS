/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <w32/ntdll.h>
static struct w32_teb teb;
_Thread_local struct w32_teb *w32_model_teb = &teb;
static unsigned records;
void w32_diag(const char *event, const char *dll, const char *detail) {
  (void)event;
  (void)dll;
  (void)detail;
  records++;
}
extern int W32API WSAStartup(uint16_t, void *);
extern int W32API WSAGetLastError(void);
extern int W32API WSACleanup(void);
extern uintptr_t W32API socket(int, int, int);
extern int W32API getsockname(uintptr_t, void *, int *);
extern uint16_t W32API htons(uint16_t);
extern ULONG W32API inet_addr(const char *);
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "offline line %u: %s\n", __LINE__, #c);                  \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
int main(void) {
  unsigned char data[512];
  memset(data, 0x55, sizeof(data));
  teb.LastErrorValue = 42;
  CHECK(WSAStartup(0x101, data) == 10091 && data[0] == 0x55);
  CHECK(socket(2, 1, 0) == ~(uintptr_t)0 && WSAGetLastError() == 10050 &&
        teb.LastErrorValue == 42);
  int length = 16;
  CHECK(getsockname(0, data, &length) == -1 && length == 16 &&
        data[0] == 0x55 && records == 3);
  CHECK(getsockname(0,data,&length)==-1&&records==3);
  CHECK(WSACleanup() == -1 && WSAGetLastError() == 10093);
  CHECK(htons(0x1234) == 0x3412 && inet_addr("127.0.0.1") == 0x0100007f &&
        inet_addr("999.0.0.1") == ~0u);
  puts("PASS offline Winsock failures unchanged outputs separate error and "
       "record budget");
  return 0;
}
