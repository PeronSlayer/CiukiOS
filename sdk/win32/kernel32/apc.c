/* APC callbacks execute only on the alertable thread's user stack.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
static void W32API dispatcher(void *routine, DWORD argument, DWORD low,
                              DWORD high) {
#ifndef W32_HOST_MODEL
  /* QueueUserAPC has one stdcall argument; timer completions have three.
   * Restore ESP explicitly after either callee cleanup convention. ESI is
   * callee-saved, and frame-pointer addressing keeps all operands stable. */
  __asm__ volatile("movl %%esp,%%esi; pushl %3; pushl %2; pushl %1; call *%0; "
                   "movl %%esi,%%esp"
                   :
                   : "m"(routine), "m"(argument), "m"(low), "m"(high)
                   : "eax", "ecx", "edx", "esi", "memory", "cc");
#else
  ((void (*)(DWORD, DWORD, DWORD))routine)(argument, low, high);
#endif
  struct ciuki_w32_apc_args a = {.action = W32_APC_RETURN};
  a.header = K32_HEADER(W32_APC, a);
  w32_ciuki_call(&a, sizeof(a));
  __builtin_trap();
}
int k32_apc_register(void) {
  struct ciuki_w32_apc_args a = {.action = W32_APC_REGISTER,
                                 .routine = K32_PTR(dispatcher)};
  a.header = K32_HEADER(W32_APC, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
  if (a.error) {
    SetLastError(a.error);
    return 0;
  }
  return 1;
}
DWORD W32API QueueUserAPC(void(W32API *routine)(uintptr_t), HANDLE thread,
                          uintptr_t argument) {
  struct ciuki_w32_apc_args a = {.action = W32_APC_QUEUE,
                                 .thread = K32_PTR(thread),
                                 .routine = K32_PTR(routine),
                                 .argument = (DWORD)argument};
  a.header = K32_HEADER(W32_APC, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
  if (a.error) {
    SetLastError(a.error);
    return 0;
  }
  return a.result;
}
typedef void(W32API *completion_fn)(DWORD, DWORD, OVERLAPPED *);
struct completion {
  completion_fn routine;
  DWORD error, bytes;
  OVERLAPPED *overlapped;
};
static void W32API complete(uintptr_t arg) {
  struct completion c = *(struct completion *)arg;
  k32_free((void *)arg, sizeof(c));
  c.routine(c.error, c.bytes, c.overlapped);
}
static BOOL transfer(HANDLE h, void *buffer, DWORD n, OVERLAPPED *ov,
                     completion_fn routine, BOOL write) {
  if (!ov || !routine) {
    SetLastError(87);
    return 0;
  }
  struct completion *c = k32_alloc(sizeof(*c));
  if (!c) {
    SetLastError(8);
    return 0;
  }
  DWORD done = 0;
  BOOL ok = write ? WriteFile(h, buffer, n, &done, ov)
                  : ReadFile(h, buffer, n, &done, ov);
  if (!ok) {
    k32_free(c, sizeof(*c));
    return 0;
  }
  *c = (struct completion){routine, 0, done, ov};
  if (!QueueUserAPC(complete, (HANDLE)(intptr_t)-2, (uintptr_t)c)) {
    k32_free(c, sizeof(*c));
    return 0;
  }
  return 1;
}
BOOL W32API ReadFileEx(HANDLE h, void *buffer, DWORD n, OVERLAPPED *ov,
                       completion_fn routine) {
  return transfer(h, buffer, n, ov, routine, 0);
}
BOOL W32API WriteFileEx(HANDLE h, const void *buffer, DWORD n, OVERLAPPED *ov,
                        completion_fn routine) {
  return transfer(h, (void *)buffer, n, ov, routine, 1);
}
