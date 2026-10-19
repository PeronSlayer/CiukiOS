/* User-mode fibers: only the contract's x87 control word is switched.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
struct fiber_context {
  DWORD ebx, esi, edi, ebp, esp;
  uint16_t control, pad;
};
struct fiber {
  void
      *data; /* GetFiberData is also an inline dereference in period headers. */
  struct fiber *next;
  void *stack;
  void(W32API *start)(void *);
  DWORD bytes;
  struct fiber_context context;
  struct w32_seh *exception;
  void *base, *limit;
};
static struct fiber *fibers;
#ifndef W32_HOST_MODEL
extern void k32_fiber_switch(struct fiber_context *,
                             const struct fiber_context *);
#endif
static RTL_CRITICAL_SECTION *fiber_lock(void) {
  return NtCurrentTeb()->ProcessEnvironmentBlock->FastPebLock;
}
static void add(struct fiber *f) {
  RtlEnterCriticalSection(fiber_lock());
  f->next = fibers;
  fibers = f;
  RtlLeaveCriticalSection(fiber_lock());
}
static struct fiber *find(void *p) {
  RtlEnterCriticalSection(fiber_lock());
  for (struct fiber *f = fibers; f; f = f->next)
    if (f == p) {
      RtlLeaveCriticalSection(fiber_lock());
      return f;
    }
  RtlLeaveCriticalSection(fiber_lock());
  return 0;
}
void *W32API ConvertThreadToFiber(void *data) {
  struct w32_teb *t = NtCurrentTeb();
  if (find(t->Tib.FiberData)) {
    SetLastError(1280);
    return 0;
  }
  struct fiber *f = k32_alloc(sizeof(*f));
  if (!f) {
    SetLastError(8);
    return 0;
  }
  w32_fill(f, 0, sizeof(*f));
  f->data = data;
  f->base = t->Tib.StackBase;
  f->limit = t->Tib.StackLimit;
  f->exception = t->Tib.ExceptionList;
  add(f);
  t->Tib.FiberData = f;
  return f;
}
#ifndef W32_HOST_MODEL
static W32_NORETURN void entry(void) {
  struct fiber *f = NtCurrentTeb()->Tib.FiberData;
  f->start(f->data);
  ExitThread(0);
  __builtin_trap();
}
#endif
void *W32API CreateFiber(DWORD bytes, void(W32API *start)(void *), void *data) {
  if (!start || bytes > 0x6fff0000u - 8192) {
    SetLastError(87);
    return 0;
  }
  DWORD size = bytes ? bytes : 1024 * 1024;
  size = (size + 4095) & ~4095u;
  if (size < 16384)
    size = 16384;
  struct fiber *f = k32_alloc(sizeof(*f));
  if (!f) {
    SetLastError(8);
    return 0;
  }
  w32_fill(f, 0, sizeof(*f));
  f->stack = VirtualAlloc(0, size + 4096, 0x3000, 4);
  if (!f->stack) {
    k32_free(f, sizeof(*f));
    return 0;
  }
  DWORD old;
  VirtualProtect(f->stack, 4096, 1, &old);
  f->bytes = size + 4096;
  f->data = data;
  f->start = start;
  f->base = (uint8_t *)f->stack + f->bytes;
  f->limit = (uint8_t *)f->stack + 4096;
  f->exception = (void *)(intptr_t)-1;
  f->context.esp = K32_PTR((uint8_t *)f->stack + f->bytes - 16);
  f->context.control = 0x027f;
#ifndef W32_HOST_MODEL
  *(DWORD *)(uintptr_t)f->context.esp = K32_PTR(entry);
#endif
  add(f);
  return f;
}
void W32API SwitchToFiber(void *handle) {
  struct w32_teb *t = NtCurrentTeb();
  struct fiber *old = find(t->Tib.FiberData), *f = find(handle);
  if (!old || !f) {
    SetLastError(87);
    return;
  }
  if (old == f)
    return;
#ifndef W32_HOST_MODEL
  old->exception = t->Tib.ExceptionList;
  old->base = t->Tib.StackBase;
  old->limit = t->Tib.StackLimit;
  t->Tib.ExceptionList = f->exception;
  t->Tib.StackBase = f->base;
  t->Tib.StackLimit = f->limit;
  t->Tib.FiberData = f;
  k32_fiber_switch(&old->context, &f->context);
#else
  SetLastError(120); /* Native H0 cannot execute i386 stack switches. */
#endif
}
void W32API DeleteFiber(void *handle) {
  RtlEnterCriticalSection(fiber_lock());
  struct fiber **at = &fibers;
  while (*at && *at != handle)
    at = &(*at)->next;
  if (!*at) {
    RtlLeaveCriticalSection(fiber_lock());
    SetLastError(87);
    return;
  }
  struct fiber *f = *at;
  if (NtCurrentTeb()->Tib.FiberData == f) {
    RtlLeaveCriticalSection(fiber_lock());
    ExitThread(0);
    return;
  }
  *at = f->next;
  RtlLeaveCriticalSection(fiber_lock());
  if (f->stack)
    VirtualFree(f->stack, 0, 0x8000);
  k32_free(f, sizeof(*f));
}
void *W32API GetFiberData(void) {
  struct fiber *f = find(NtCurrentTeb()->Tib.FiberData);
  return f ? f->data : 0;
}

void *W32API GetCurrentFiber(void) { return NtCurrentTeb()->Tib.FiberData; }
BOOL W32API ConvertFiberToThread(void) {
  struct fiber *f = find(NtCurrentTeb()->Tib.FiberData);
  if (!f || f->stack) {
    SetLastError(87);
    return 0;
  }
  NtCurrentTeb()->Tib.FiberData = (void *)0x1e00;
  DeleteFiber(f);
  return 1;
}
