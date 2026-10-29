/* Original kernel32 facade over copied F5 records and ntdll.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
DWORD W32API GetLastError(void) { return NtCurrentTeb()->LastErrorValue; }
void W32API SetLastError(DWORD n) { NtCurrentTeb()->LastErrorValue = n; }
int k32_status(int32_t s) {
  if (!s)
    return 1;
  SetLastError(s < 0 && s >= -4095 ? (s == -14   ? 998
                                      : s == -12 ? 8
                                                 : 87)
                                   : RtlNtStatusToDosError(s));
  return 0;
}
void *k32_alloc(DWORD n) {
  const struct w32_services *s = w32_get_services();
  return s->alloc(s->opaque, n);
}
void k32_free(void *p, DWORD n) {
  if (p) {
    const struct w32_services *s = w32_get_services();
    s->free(s->opaque, p, n);
  }
}
HANDLE W32API GetCurrentProcess(void) { return K32_INVALID; }
HANDLE W32API GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
DWORD W32API GetCurrentProcessId(void) {
  return K32_PTR(NtCurrentTeb()->ClientId.UniqueProcess);
}
DWORD W32API GetCurrentThreadId(void) {
  return K32_PTR(NtCurrentTeb()->ClientId.UniqueThread);
}
void *W32API VirtualAlloc(void *p, DWORD n, DWORD flags, DWORD protect) {
  struct ciuki_w32_vm a = {
      .address = K32_PTR(p), .bytes = n, .flags = flags, .protect = protect};
  a.header = K32_HEADER(W32_VM_ALLOC, a);
  return k32_status(w32_ciuki_call(&a, sizeof(a)))
             ? (void *)(uintptr_t)a.address
             : 0;
}
BOOL W32API VirtualFree(void *p, DWORD n, DWORD flags) {
  struct ciuki_w32_vm a = {.address = K32_PTR(p), .bytes = n, .flags = flags};
  a.header = K32_HEADER(W32_VM_FREE, a);
  return k32_status(w32_ciuki_call(&a, sizeof(a)));
}
BOOL W32API VirtualProtect(void *p, DWORD n, DWORD prot, DWORD *old) {
  if (!old) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_vm a = {.address = K32_PTR(p), .bytes = n, .protect = prot};
  a.header = K32_HEADER(W32_VM_PROTECT, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
  *old = a.old_protect;
  return 1;
}
DWORD W32API VirtualQuery(const void *p, struct ciuki_w32_memory_info *out,
                          DWORD size) {
  if (!out || size < sizeof(*out)) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_query a = {.address = K32_PTR(p)};
  a.header = K32_HEADER(W32_VM_QUERY, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
  *out = a.info;
  return sizeof(*out);
}
HANDLE W32API GetProcessHeap(void) {
  struct w32_peb *p = NtCurrentTeb()->ProcessEnvironmentBlock;
  RtlEnterCriticalSection(p->FastPebLock);
  if (!p->ProcessHeap)
    p->ProcessHeap = RtlCreateHeap(HEAP_GROWABLE, 0, 0, 0, 0, 0);
  RtlLeaveCriticalSection(p->FastPebLock);
  return p->ProcessHeap;
}
HANDLE W32API HeapCreate(DWORD f, DWORD initial, DWORD max) {
  return RtlCreateHeap(f, 0, max, initial, 0, 0);
}
BOOL W32API HeapDestroy(HANDLE h) {
  if (!h || h == NtCurrentTeb()->ProcessEnvironmentBlock->ProcessHeap) {
    SetLastError(87);
    return 0;
  }
  return !RtlDestroyHeap(h);
}
void *W32API HeapAlloc(HANDLE h, DWORD f, DWORD n) {
  return RtlAllocateHeap(h, f, n);
}
void *W32API HeapReAlloc(HANDLE h, DWORD f, void *p, DWORD n) {
  return RtlReAllocateHeap(h, f, p, n);
}
BOOL W32API HeapFree(HANDLE h, DWORD f, void *p) {
  return RtlFreeHeap(h, f, p);
}
DWORD W32API HeapSize(HANDLE h, DWORD f, const void *p) {
  return RtlSizeHeap(h, f, p);
}
BOOL W32API HeapValidate(HANDLE h, DWORD f, const void *p) {
  return RtlValidateHeap(h, f, p);
}
BOOL W32API HeapLock(HANDLE h) { return RtlLockHeap(h); }
BOOL W32API HeapUnlock(HANDLE h) { return RtlUnlockHeap(h); }
BOOL W32API HeapWalk(HANDLE h, struct w32_heap_entry *e) {
  NTSTATUS s = RtlWalkHeap(h, e);
  if (s) {
    SetLastError((ULONG)s == 0x8000001a ? 259 : RtlNtStatusToDosError(s));
    return 0;
  }
  return 1;
}
static int object(struct ciuki_w32_object_args *a) {
  a->header = K32_HEADER(W32_OBJECT, *a);
  if (!k32_status(w32_ciuki_call(a, sizeof(*a))))
    return 0;
  if (a->error) {
    SetLastError(a->error);
    return a->error == 183;
  }
  return 1;
}
BOOL W32API CloseHandle(HANDLE h) {
  struct ciuki_w32_object_args a = {.action = W32_OBJ_CLOSE,
                                    .handle = K32_PTR(h)};
  return object(&a);
}
BOOL W32API DuplicateHandle(HANDLE src, HANDLE h, HANDLE dst, HANDLE *out,
                            DWORD access, BOOL inherit, DWORD options) {
  if (!out && !(options & 1)) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_object_args a = {.action = W32_OBJ_DUPLICATE,
                                    .handle = K32_PTR(h),
                                    .source = K32_PTR(src),
                                    .target = K32_PTR(dst),
                                    .access = access,
                                    .value = !!inherit,
                                    .flags = options};
  if (!object(&a))
    return 0;
  if (out)
    *out = (HANDLE)(uintptr_t)a.result;
  return 1;
}
static HANDLE named(DWORD type, DWORD action, const WCHAR *name, BOOL inherit,
                    DWORD value, DWORD maximum, DWORD access) {
  struct ciuki_w32_object_args a = {.action = action,
                                    .type = type,
                                    .name = K32_PTR(name),
                                    .flags = !!inherit,
                                    .value = value,
                                    .maximum = maximum,
                                    .access = access};
  if (!object(&a))
    return 0;
  if (action == W32_OBJ_CREATE)
    SetLastError(a.error);
  return (HANDLE)(uintptr_t)a.result;
}
static HANDLE named_a(DWORD type, DWORD action, const char *name, BOOL inherit,
                      DWORD v, DWORD max, DWORD access) {
  WCHAR w[261];
  if (name && !MultiByteToWideChar(1252, 0, name, -1, w, 261))
    return 0;
  return named(type, action, name ? w : 0, inherit, v, max, access);
}
HANDLE W32API CreateEventA(SECURITY_ATTRIBUTES *s, BOOL manual, BOOL initial,
                           const char *n) {
  return named_a(W32_OBJ_EVENT, 1, n, s && s->inherit, !!initial, !!manual,
                 ~0u);
}

HANDLE W32API OpenEventA(DWORD access, BOOL inherit, const char *n) {
  return named_a(W32_OBJ_EVENT, 2, n, inherit, 0, 0, access);
}
HANDLE W32API CreateMutexA(SECURITY_ATTRIBUTES *s, BOOL owner, const char *n) {
  return named_a(W32_OBJ_MUTEX, 1, n, s && s->inherit, !!owner, 0, ~0u);
}

HANDLE W32API CreateSemaphoreA(SECURITY_ATTRIBUTES *s, LONG initial, LONG max,
                               const char *n) {
  return named_a(W32_OBJ_SEMAPHORE, 1, n, s && s->inherit, (DWORD)initial,
                 (DWORD)max, ~0u);
}
static BOOL change(HANDLE h, DWORD op) {
  struct ciuki_w32_object_args a = {.action = op, .handle = K32_PTR(h)};
  return object(&a);
}
BOOL W32API SetEvent(HANDLE h) { return change(h, W32_OBJ_SET); }
BOOL W32API ResetEvent(HANDLE h) { return change(h, W32_OBJ_RESET); }
BOOL W32API PulseEvent(HANDLE h) { return change(h, W32_OBJ_PULSE); }
BOOL W32API ReleaseMutex(HANDLE h) { return change(h, W32_OBJ_RELEASE); }
BOOL W32API ReleaseSemaphore(HANDLE h, LONG count, LONG *old) {
  struct ciuki_w32_object_args a = {
      .action = W32_OBJ_RELEASE, .handle = K32_PTR(h), .value = (DWORD)count};
  if (!object(&a))
    return 0;
  if (old)
    *old = (LONG)a.result;
  return 1;
}
HANDLE W32API OpenProcess(DWORD access, BOOL inherit, DWORD id) {
  return named(W32_OBJ_PROCESS, 2, 0, inherit, id, 0, access);
}
static BOOL exit_code(HANDLE h, DWORD type, DWORD *out) {
  if (!out) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_object_args a = {.action = W32_OBJ_QUERY,
                                    .handle = K32_PTR(h)};
  if (!object(&a))
    return 0;
  if (a.type != type) {
    SetLastError(6);
    return 0;
  }
  *out = a.result;
  return 1;
}
BOOL W32API GetExitCodeProcess(HANDLE h, DWORD *out) {
  return exit_code(h, W32_OBJ_PROCESS, out);
}
BOOL W32API GetExitCodeThread(HANDLE h, DWORD *out) {
  return exit_code(h, W32_OBJ_THREAD, out);
}
BOOL W32API GetHandleInformation(HANDLE h, DWORD *out) {
  if (!out) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_object_args a = {.action = W32_OBJ_QUERY,
                                    .handle = K32_PTR(h)};
  if (!object(&a))
    return 0;
  *out = a.flags;
  return 1;
}
BOOL W32API SetHandleInformation(HANDLE h, DWORD mask, DWORD flags) {
  if (mask & ~1u) {
    SetLastError(87);
    return 0;
  }
  if (!mask) {
    DWORD out;
    return GetHandleInformation(h, &out);
  }
  struct ciuki_w32_object_args a = {
      .action = W32_OBJ_SET_INHERIT, .handle = K32_PTR(h), .value = flags & 1};
  return object(&a);
}
static DWORD wait(DWORD n, const HANDLE *handles, BOOL all, DWORD ms,
                  BOOL alertable) {
  extern int k32_apc_register(void);
  if (alertable && !k32_apc_register())
    return ~0u;
  DWORD *values = n ? k32_alloc(n * 4) : 0;
  if (n && !values) {
    SetLastError(8);
    return ~0u;
  }
  for (DWORD i = 0; i < n; i++)
    values[i] = K32_PTR(handles[i]);
  struct ciuki_w32_wait_args a = {.handles = K32_PTR(values),
                                  .count = n,
                                  .all = !!all,
                                  .timeout = ms,
                                  .alertable = !!alertable};
  a.header = K32_HEADER(W32_WAIT, a);
  int32_t status = w32_ciuki_call(&a, sizeof(a));
  k32_free(values, n * 4);
  if (!k32_status(status))
    return ~0u;
  if (a.error) {
    SetLastError(a.error);
    return ~0u;
  }
  return a.result;
}
DWORD W32API WaitForMultipleObjectsEx(DWORD n, const HANDLE *h, BOOL all,
                                      DWORD ms, BOOL alertable) {
  if (!n || n > 64 || !h) {
    SetLastError(87);
    return ~0u;
  }
  return wait(n, h, all, ms, alertable);
}
DWORD W32API WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all,
                                    DWORD ms) {
  return WaitForMultipleObjectsEx(n, h, all, ms, 0);
}
DWORD W32API WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable) {
  return wait(1, &h, 0, ms, alertable);
}
DWORD W32API WaitForSingleObject(HANDLE h, DWORD ms) {
  return wait(1, &h, 0, ms, 0);
}
DWORD W32API SleepEx(DWORD ms, BOOL alertable) {
  if (!ms) {
    struct ciuki_w32_system_args a = {.action = W32_S_YIELD};
    a.header = K32_HEADER(W32_SYSTEM, a);
    if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
      return ~0u;
  }
  DWORD r = wait(0, 0, 0, ms, alertable);
  return r == 192 ? r : 0;
}
void W32API Sleep(DWORD ms) { (void)SleepEx(ms, 0); }
DWORD W32API SuspendThread(HANDLE h) {
  struct ciuki_w32_context_args a = {.action = W32_CTX_SUSPEND,
                                     .thread = K32_PTR(h)};
  a.header = K32_HEADER(W32_CONTEXT, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return ~0u;
  if (a.error) {
    SetLastError(a.error);
    return ~0u;
  }
  return a.result;
}
DWORD W32API ResumeThread(HANDLE h) {
  struct ciuki_w32_context_args a = {.action = W32_CTX_RESUME,
                                     .thread = K32_PTR(h)};
  a.header = K32_HEADER(W32_CONTEXT, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return ~0u;
  if (a.error) {
    SetLastError(a.error);
    return ~0u;
  }
  return a.result;
}
static BOOL context(HANDLE h, struct w32_context *c, DWORD op) {
  if (!c) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_context_args a = {.action = op,
                                     .thread = K32_PTR(h),
                                     .context = K32_PTR(c),
                                     .flags = c->ContextFlags};
  a.header = K32_HEADER(W32_CONTEXT, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
  if (a.error) {
    SetLastError(a.error);
    return 0;
  }
  return 1;
}
BOOL W32API GetThreadContext(HANDLE h, struct w32_context *c) {
  return context(h, c, W32_CTX_GET);
}
BOOL W32API SetThreadContext(HANDLE h, const struct w32_context *c) {
  return context(h, (void *)c, W32_CTX_SET);
}
HANDLE W32API CreateFileMappingA(HANDLE file, SECURITY_ATTRIBUTES *sa,
                                 DWORD protect, DWORD high, DWORD low,
                                 const char *name) {
  if (high) {
    SetLastError(223);
    return 0;
  }
  WCHAR w[261];
  if (name && !MultiByteToWideChar(1252, 0, name, -1, w, 261))
    return 0;
  struct ciuki_w32_section_args a = {
      .kind = file == K32_INVALID ? W32_SEC_PAGEFILE : W32_SEC_FILE,
      .file = K32_PTR(file),
      .bytes = low,
      .protect = protect,
      .name = K32_PTR(name ? w : 0),
      .flags = sa && sa->inherit};
  a.header = K32_HEADER(W32_SECTION, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
  if (a.error && a.error != 183) {
    SetLastError(a.error);
    return 0;
  }
  SetLastError(a.error);
  return (HANDLE)(uintptr_t)a.result;
}
/* fx-hlrun (Half-Life's OpenGL renderer imports it): open a section named
 * by CreateFileMapping. A missing name is ERROR_INVALID_PARAMETER and an
 * unknown one ERROR_FILE_NOT_FOUND (Microsoft OpenFileMapping reference;
 * Wine 10.0 dlls/kernelbase/sync.c OpenFileMappingW). Windows 9x does not
 * check section access, so the handle is opened with full access (Wine
 * tries SECTION_MAP_READ|SECTION_MAP_WRITE first when it emulates 9x);
 * MapViewOfFile still applies the section's own page protection. */
HANDLE W32API OpenFileMappingA(DWORD access, BOOL inherit, const char *name) {
  (void)access;
  if (!name) {
    SetLastError(87);
    return 0;
  }
  return named_a(W32_OBJ_SECTION, W32_OBJ_OPEN, name, inherit, 0, 0, ~0u);
}
HANDLE W32API OpenFileMappingW(DWORD access, BOOL inherit, const WCHAR *name) {
  (void)access;
  if (!name) {
    SetLastError(87);
    return 0;
  }
  return named(W32_OBJ_SECTION, W32_OBJ_OPEN, name, inherit, 0, 0, ~0u);
}
void *W32API MapViewOfFileEx(HANDLE h, DWORD access, DWORD high, DWORD low,
                             DWORD bytes, void *base) {
  struct ciuki_w32_view_args a = {.action = W32_VIEW_MAP,
                                  .section = K32_PTR(h),
                                  .address = K32_PTR(base),
                                  .bytes = bytes,
                                  .offset_lo = low,
                                  .offset_hi = high,
                                  .access = access};
  a.header = K32_HEADER(W32_VIEW, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
  if (a.error) {
    SetLastError(a.error);
    return 0;
  }
  return (void *)(uintptr_t)a.result;
}
void *W32API MapViewOfFile(HANDLE h, DWORD access, DWORD high, DWORD low,
                           DWORD bytes) {
  return MapViewOfFileEx(h, access, high, low, bytes, 0);
}
static BOOL view(const void *p, DWORD n, DWORD op) {
  struct ciuki_w32_view_args a = {
      .action = op, .address = K32_PTR(p), .bytes = n};
  a.header = K32_HEADER(W32_VIEW, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
  if (a.error) {
    SetLastError(a.error);
    return 0;
  }
  return 1;
}
BOOL W32API UnmapViewOfFile(const void *p) {
  return view(p, 0, W32_VIEW_UNMAP);
}
BOOL W32API FlushViewOfFile(const void *p, DWORD n) {
  return view(p, n, W32_VIEW_FLUSH);
}
static volatile DWORD tls_lock, tls_bits[2], tls_generation[64];
static void tls_enter(void) {
  while (__atomic_exchange_n(&tls_lock, 1, __ATOMIC_ACQUIRE)) {
  }
}
static void tls_leave(void) {
  __atomic_store_n(&tls_lock, 0, __ATOMIC_RELEASE);
}
DWORD W32API TlsAlloc(void) {
  tls_enter();
  for (DWORD i = 0; i < 64; i++)
    if (!(tls_bits[i / 32] & (1u << (i % 32)))) {
      tls_bits[i / 32] |= 1u << (i % 32);
      if (!++tls_generation[i])
        ++tls_generation[i];
      NtCurrentTeb()->TlsSlots[i] = 0;
      NtCurrentTeb()->CoreTlsGeneration[i] = tls_generation[i];
      tls_leave();
      return i;
    }
  tls_leave();
  SetLastError(8);
  return ~0u;
}
BOOL W32API TlsFree(DWORD i) {
  if (i >= 64) {
    SetLastError(87);
    return 0;
  }
  tls_enter();
  DWORD bit = 1u << (i % 32);
  if (!(tls_bits[i / 32] & bit)) {
    tls_leave();
    SetLastError(87);
    return 0;
  }
  tls_bits[i / 32] &= ~bit;
  tls_leave();
  return 1;
}
void *W32API TlsGetValue(DWORD i) {
  if (i >= 64) {
    SetLastError(87);
    return 0;
  }
  if (NtCurrentTeb()->CoreTlsGeneration[i] != tls_generation[i]) {
    NtCurrentTeb()->TlsSlots[i] = 0;
    NtCurrentTeb()->CoreTlsGeneration[i] = tls_generation[i];
  }
  SetLastError(0);
  return NtCurrentTeb()->TlsSlots[i];
}
BOOL W32API TlsSetValue(DWORD i, void *p) {
  if (i >= 64) {
    SetLastError(87);
    return 0;
  }
  NtCurrentTeb()->TlsSlots[i] = p;
  NtCurrentTeb()->CoreTlsGeneration[i] = tls_generation[i];
  return 1;
}
void W32API OutputDebugStringA(const char *s) {
  DWORD saved = GetLastError();
  w32_diag("debug", "kernel32", s ? s : "");
  SetLastError(saved);
}
BOOL W32API TerminateProcess(HANDLE h, DWORD code) {
  struct ciuki_w32_system_args a = {
      .action = W32_S_TERMINATE_PROCESS, .handle = K32_PTR(h), .value = code};
  a.header = K32_HEADER(W32_SYSTEM, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
  if (a.error) {
    SetLastError(a.error);
    return 0;
  }
  return 1;
}
extern void W32API CiukiThreadDetach(void);
extern void W32API CiukiProcessDetach(void);
void W32API ExitThread(DWORD code) {
  CiukiThreadDetach();
  struct ciuki_w32_system_args a = {.action = W32_S_THREAD_EXIT, .value = code};
  a.header = K32_HEADER(W32_SYSTEM, a);
  w32_ciuki_call(&a, sizeof(a));
  w32_terminate(code);
}
void W32API ExitProcess(DWORD code) {
  CiukiProcessDetach();
  w32_terminate(code);
}
LONG W32API InterlockedIncrement(volatile LONG *p) {
  return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST);
}
LONG W32API InterlockedDecrement(volatile LONG *p) {
  return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST);
}
LONG W32API InterlockedExchange(volatile LONG *p, LONG v) {
  return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}
LONG W32API InterlockedCompareExchange(volatile LONG *p, LONG v, LONG old) {
  __atomic_compare_exchange_n(p, &old, v, 0, __ATOMIC_SEQ_CST,
                              __ATOMIC_SEQ_CST);
  return old;
}
int W32API MulDiv(int a, int b, int c) {
  if (!c)
    return -1;
  int64_t product = (int64_t)a * b;
  int neg = (product < 0) ^ (c < 0);
  uint64_t mag = product < 0 ? (uint64_t)-product : (uint64_t)product;
  uint32_t divisor = c < 0 ? 0u - (uint32_t)c : (uint32_t)c;
  uint64_t q = (mag + divisor / 2) / divisor;
  if (q > (neg ? 0x80000000u : 0x7fffffffu))
    return -1;
  return neg ? (int)(0u - (uint32_t)q) : (int)q;
}

extern int W32API CiukiThreadAttach(void);
struct thread_start {
  DWORD(W32API *entry)(void *);
  void *argument;
};
static void W32API thread_entry(void *argument) {
  struct thread_start start = *(struct thread_start *)argument;
  k32_free(argument, sizeof(start));
  if (!CiukiThreadAttach())
    ExitThread(1114);
  DWORD result = start.entry(start.argument);
  ExitThread(result);
}
/* TerminateThread runs no DLL detach (Microsoft TerminateThread remarks), but
 * the window manager still owns the victim's windows, timers, hooks and
 * clipboard lock. In-process USER releases them without calling the victim's
 * procedures; user32 is never loaded for this. */
HANDLE W32API GetModuleHandleA(const char *);
void *W32API GetProcAddress(HANDLE, const char *);
static void user_thread_cleanup(DWORD tid) {
  HANDLE user = GetModuleHandleA("user32.dll");
  void(W32API * cleanup)(DWORD) =
      user ? GetProcAddress(user, "w32_user_thread_cleanup") : 0;
  if (cleanup)
    cleanup(tid);
}
BOOL W32API TerminateThread(HANDLE h, DWORD code) {
  struct ciuki_w32_system_args a = {
      .action = W32_S_TERMINATE_THREAD, .handle = K32_PTR(h), .value = code};
  a.header = K32_HEADER(W32_SYSTEM, a);
  DWORD saved = GetLastError();
  /* A self-termination never returns to user mode: release first. */
  if (h == GetCurrentThread())
    user_thread_cleanup(GetCurrentThreadId());
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
  if (a.error) {
    SetLastError(a.error);
    return 0;
  }
  if (a.result && a.result != GetCurrentThreadId())
    user_thread_cleanup(a.result);
  SetLastError(saved);
  return 1;
}
HANDLE W32API CreateThread(SECURITY_ATTRIBUTES *sa, DWORD stack,
                           DWORD(W32API *entry)(void *), void *arg, DWORD flags,
                           DWORD *id) {
  if (!entry || flags & ~0x10004u) {
    SetLastError(87);
    return 0;
  }
  struct thread_start *start = k32_alloc(sizeof(*start));
  if (!start) {
    SetLastError(8);
    return 0;
  }
  *start = (struct thread_start){entry, arg};
  struct ciuki_w32_thread a = {.entry = K32_PTR(thread_entry),
                               .argument = K32_PTR(start),
                               .trampoline = K32_PTR(thread_entry),
                               .stack_bytes = stack,
                               .flags = flags | 4};
  a.header = K32_HEADER(W32_THREAD, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a)))) {
    k32_free(start, sizeof(*start));
    return 0;
  }
  HANDLE h = named(W32_OBJ_THREAD, 2, 0, sa && sa->inherit, a.tid, 0, ~0u);
  if (!h) {
    DWORD error = GetLastError();
    struct ciuki_w32_system_args cancel = {
        .action = W32_S_TERMINATE_THREAD, .extra = a.tid, .value = 8};
    cancel.header = K32_HEADER(W32_SYSTEM, cancel);
    w32_ciuki_call(&cancel, sizeof(cancel));
    k32_free(start, sizeof(*start));
    SetLastError(error);
    return 0;
  }
  if (id)
    *id = a.tid;
  if (!(flags & 4))
    ResumeThread(h);
  return h;
}

static BOOL priority(HANDLE h, DWORD value, DWORD action, DWORD *result) {
  struct ciuki_w32_system_args a = {
      .action = action, .handle = K32_PTR(h), .value = value};
  a.header = K32_HEADER(W32_SYSTEM, a);
  if (!k32_status(w32_ciuki_call(&a, sizeof(a))))
    return 0;
  if (a.error) {
    SetLastError(a.error);
    return 0;
  }
  if (result)
    *result = a.result;
  return 1;
}
BOOL W32API SetThreadPriority(HANDLE h, int relative) {
  return priority(h, (DWORD)relative, W32_S_THREAD_PRIORITY, 0);
}
int W32API GetThreadPriority(HANDLE h) {
  DWORD result;
  return priority(h, 1, W32_S_QUERY_PRIORITY, &result) ? (int)result
                                                       : 0x7fffffff;
}
BOOL W32API SetPriorityClass(HANDLE h, DWORD cls) {
  return priority(h, cls, W32_S_PROCESS_PRIORITY, 0);
}
DWORD W32API GetPriorityClass(HANDLE h) {
  DWORD result;
  return priority(h, 2, W32_S_QUERY_PRIORITY, &result) ? result : 0;
}
BOOL W32API SetProcessAffinityMask(HANDLE h, uintptr_t mask) {
  DWORD ignored;
  if (!exit_code(h, W32_OBJ_PROCESS, &ignored))
    return 0;
  if (mask != 1) {
    SetLastError(87);
    return 0;
  }
  return 1;
}
BOOL W32API GetProcessAffinityMask(HANDLE h, uintptr_t *process,
                                   uintptr_t *system) {
  DWORD ignored;
  if (!process || !system) {
    SetLastError(87);
    return 0;
  }
  if (!exit_code(h, W32_OBJ_PROCESS, &ignored))
    return 0;
  *process = *system = 1;
  return 1;
}
BOOL W32API SwitchToThread(void) {
  struct ciuki_w32_system_args a = {.action = W32_S_YIELD};
  a.header = K32_HEADER(W32_SYSTEM, a);
  return k32_status(w32_ciuki_call(&a, sizeof(a)));
}

HANDLE W32API CreateWaitableTimerA(SECURITY_ATTRIBUTES *s, BOOL manual,
                                   const char *name) {
  return named_a(W32_OBJ_TIMER, W32_OBJ_CREATE, name, s && s->inherit, 0,
                 !!manual, ~0u);
}
BOOL W32API SetWaitableTimer(HANDLE h, const int64_t *due, LONG period,
                             void *routine, void *argument, BOOL resume) {
  (void)resume;
  if (!due || period < 0) {
    SetLastError(87);
    return 0;
  }
  uint64_t value = (uint64_t)*due;
  struct ciuki_w32_object_args a = {.action = W32_OBJ_TIMER_SET,
                                    .handle = K32_PTR(h),
                                    .value = (DWORD)value,
                                    .maximum = (DWORD)(value >> 32),
                                    .extra = (DWORD)period,
                                    .source = K32_PTR(routine),
                                    .target = K32_PTR(argument)};
  return object(&a);
}
BOOL W32API CancelWaitableTimer(HANDLE h) {
  return change(h, W32_OBJ_TIMER_CANCEL);
}
DWORD W32API SignalObjectAndWait(HANDLE signal, HANDLE object_, DWORD timeout,
                                 BOOL alertable) {
  extern int k32_apc_register(void);
  if (alertable && !k32_apc_register())
    return ~0u;
  DWORD *handle = k32_alloc(4);
  if (!handle) {
    SetLastError(8);
    return ~0u;
  }
  *handle = K32_PTR(object_);
  struct ciuki_w32_wait_args a = {.handles = K32_PTR(handle),
                                  .count = 1,
                                  .timeout = timeout,
                                  .alertable = !!alertable,
                                  .signal = K32_PTR(signal)};
  a.header = K32_HEADER(W32_WAIT, a);
  int32_t status = w32_ciuki_call(&a, sizeof(a));
  k32_free(handle, 4);
  if (!k32_status(status))
    return ~0u;
  if (a.error) {
    SetLastError(a.error);
    return ~0u;
  }
  return a.result;
}

BOOL W32API TryEnterCriticalSection(RTL_CRITICAL_SECTION *section) {
  return !!RtlTryEnterCriticalSection(section);
}
BOOL W32API InitializeCriticalSectionAndSpinCount(RTL_CRITICAL_SECTION *section,
                                                  DWORD spin) {
  return k32_status(RtlInitializeCriticalSectionAndSpinCount(section, spin));
}

static BOOL process_memory(HANDLE h, const void *src, void *dst, DWORD n,
                            DWORD *done, DWORD op) {
  struct ciuki_w32_system_args a={.header={1,sizeof(a),W32_SYSTEM,0},
      .action=op,.handle=K32_PTR(h),.low=K32_PTR(src),.high=K32_PTR(dst),.extra=n};
  if (done) *done=0;
  if (!k32_status(w32_ciuki_call(&a,sizeof(a)))) return 0;
  if (a.error) { SetLastError(a.error); return 0; }
  if (done) *done=a.result;
  return 1;
}
BOOL W32API ReadProcessMemory(HANDLE h, const void *src, void *dst, DWORD n, DWORD *done) {
  return process_memory(h,src,dst,n,done,W32_S_READ_MEMORY);
}
BOOL W32API WriteProcessMemory(HANDLE h, void *dst, const void *src, DWORD n, DWORD *done) {
  return process_memory(h,src,dst,n,done,W32_S_WRITE_MEMORY);
}
/* f5-20A: synchronization, process and heap queries the reused Wine DLLs
 * import. The kernel validates a semaphore maximum even for an open, so
 * OpenSemaphore passes a neutral one; the open ignores it. */
HANDLE W32API OpenSemaphoreA(DWORD access, BOOL inherit, const char *n) {
  if (!n) {
    SetLastError(87);
    return 0;
  }
  return named_a(W32_OBJ_SEMAPHORE, W32_OBJ_OPEN, n, inherit, 0, 1, access);
}
static int process_handle(HANDLE h) {
  if (h == K32_INVALID)
    return 1;
  struct ciuki_w32_object_args a = {.action = W32_OBJ_QUERY,
                                    .handle = K32_PTR(h)};
  if (!object(&a))
    return 0;
  if (a.type != W32_OBJ_PROCESS) {
    SetLastError(6);
    return 0;
  }
  return 1;
}
/* 32-bit Windows: a valid process handle is never a WOW64 process. */
BOOL W32API IsWow64Process(HANDLE h, BOOL *wow64) {
  if (!wow64) {
    SetLastError(87);
    return 0;
  }
  if (!process_handle(h))
    return 0;
  *wow64 = 0;
  return 1;
}
/* Windows 98 has a single session: every live process is in session 0. */
BOOL W32API ProcessIdToSessionId(DWORD pid, DWORD *session) {
  if (!session) {
    SetLastError(87);
    return 0;
  }
  if (pid != GetCurrentProcessId()) {
    HANDLE h = OpenProcess(0x400, 0, pid);
    if (!h) {
      SetLastError(87);
      return 0;
    }
    CloseHandle(h);
  }
  *session = 0;
  return 1;
}
/* The thread object query carries no identifier: the current-thread pseudo
 * handle answers; another valid thread handle gets ERROR_NOT_SUPPORTED. */
DWORD W32API GetThreadId(HANDLE h) {
  if (h == GetCurrentThread())
    return GetCurrentThreadId();
  struct ciuki_w32_object_args a = {.action = W32_OBJ_QUERY,
                                    .handle = K32_PTR(h)};
  if (!object(&a))
    return 0;
  SetLastError(a.type == W32_OBJ_THREAD ? 50 : 6);
  return 0;
}
/* Largest free block of the heap; NO_ERROR with zero when none is free. */
DWORD W32API HeapCompact(HANDLE h, DWORD flags) {
  (void)flags;
  struct w32_heap_entry e;
  w32_fill(&e, 0, sizeof(e));
  DWORD largest = 0;
  NTSTATUS s;
  while (!(s = RtlWalkHeap(h, &e)))
    if (!(e.Flags & 3) && e.DataSize > largest)
      largest = e.DataSize;
  if ((ULONG)s != 0x8000001au) {
    SetLastError(6);
    return 0;
  }
  SetLastError(0);
  return largest;
}
