/* Thread pool, one-time initialization, timers, delay-load resolution,
 * global atoms and time-zone conversion for the reused Wine 10.0 DLLs
 * (f5-20A). Windows 98 lacks several of these NT 5/6 entry points; each
 * follows its documented contract over the existing CiukiOS services.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#include <w32/server.h>
extern BOOL W32API QueryPerformanceCounter(int64_t *);
extern BOOL W32API QueryPerformanceFrequency(int64_t *);
extern HANDLE W32API CreateEventA(SECURITY_ATTRIBUTES *, BOOL, BOOL, const char *);
extern HANDLE W32API LoadLibraryA(const char *);
extern BOOL W32API FreeLibrary(HANDLE);
extern void *W32API GetProcAddress(HANDLE, const char *);
extern HANDLE W32API GetModuleHandleA(const char *);
extern DWORD W32API GetTimeZoneInformation(TIME_ZONE_INFORMATION *);
/* DebugBreak (f5-20C, lead decision): an int 3 in the calling thread, as on
 * Windows; the kernel delivers vector 3 as EXCEPTION_BREAKPOINT through the
 * normal SEH dispatch (src/kernel/proc/win32/context.c), and an unhandled
 * breakpoint ends the process like any unhandled exception. */
void W32API DebugBreak(void) { __asm__ volatile("int3"); }
/* Windows 95/98 Beep ignores frequency and duration (default sound). */
BOOL W32API Beep(DWORD frequency, DWORD duration) {
  (void)frequency;
  (void)duration;
  return 1;
}
/* No pipe object exists in the CiukiOS Win32 object space yet. */
BOOL W32API CreatePipe(HANDLE *read, HANDLE *write, SECURITY_ATTRIBUTES *sa, DWORD size) {
  (void)sa;
  (void)size;
  static volatile ULONG once;
  if (!read || !write) {
    SetLastError(87);
    return 0;
  }
  if (!__atomic_exchange_n(&once, 1, __ATOMIC_RELAXED))
    w32_diag("stub", "kernel32.dll", "CreatePipe");
  SetLastError(120);
  return 0;
}
/* Windows 98 has no I/O completion ports (CreateIoCompletionPort fails),
 * so no handle can receive a completion packet. */
BOOL W32API PostQueuedCompletionStatus(HANDLE port, DWORD bytes, uintptr_t key, OVERLAPPED *ov) {
  (void)port;
  (void)bytes;
  (void)key;
  (void)ov;
  SetLastError(120);
  return 0;
}
uint64_t W32API GetTickCount64(void) {
  int64_t count = 0, frequency = 0;
  if (!QueryPerformanceCounter(&count) || !QueryPerformanceFrequency(&frequency) ||
      frequency <= 0)
    return 0;
  uint64_t c = (uint64_t)count, f = (uint64_t)frequency;
  return c / f * 1000 + c % f * 1000 / f;
}
/* The flags only steer the NT pool; every item runs on its own thread. */
BOOL W32API QueueUserWorkItem(DWORD(W32API *function)(void *), void *context, ULONG flags) {
  (void)flags;
  DWORD id;
  HANDLE h = function ? CreateThread(0, 0, function, context, 0, &id) : 0;
  if (!function)
    SetLastError(87);
  if (!h)
    return 0;
  CloseHandle(h);
  return 1;
}
typedef void(W32API *k32_timer_callback)(void *, BOOLEAN);
struct timer {
  k32_timer_callback callback;
  void *parameter;
  DWORD due, period;
  HANDLE cancel;
};
static DWORD W32API timer_thread(void *p) {
  struct timer *t = p;
  DWORD wait = t->due;
  while (WaitForSingleObject(t->cancel, wait) == 258) {
    t->callback(t->parameter, 1);
    if (!t->period)
      break;
    wait = t->period;
  }
  return 0;
}
/* WT_EXECUTEONLYONCE (8) or a zero period fire once. The returned handle
 * identifies the timer record; its thread waits on a private event. */
BOOL W32API CreateTimerQueueTimer(HANDLE *timer, HANDLE queue, k32_timer_callback callback,
                                  void *parameter, DWORD due, DWORD period, ULONG flags) {
  (void)queue;
  if (!timer || !callback) {
    SetLastError(87);
    return 0;
  }
  struct timer *t = k32_alloc(sizeof(*t));
  if (!t) {
    SetLastError(8);
    return 0;
  }
  *t = (struct timer){callback, parameter, due, flags & 8 ? 0 : period, 0};
  t->cancel = CreateEventA(0, 1, 0, 0);
  DWORD id;
  HANDLE h = t->cancel ? CreateThread(0, 0, timer_thread, t, 0, &id) : 0;
  if (!h) {
    DWORD e = GetLastError();
    if (t->cancel)
      CloseHandle(t->cancel);
    k32_free(t, sizeof(*t));
    SetLastError(e);
    return 0;
  }
  CloseHandle(h);
  *timer = t;
  return 1;
}
/* INIT_ONCE: 0 not started, 1 running, context|2 done. Losers wait. */
typedef BOOL(W32API *k32_once_callback)(uintptr_t *, void *, void **);
BOOL W32API InitOnceExecuteOnce(uintptr_t *once, k32_once_callback callback, void *parameter,
                                void **context) {
  if (!once || !callback) {
    SetLastError(87);
    return 0;
  }
  for (;;) {
    uintptr_t v = __atomic_load_n(once, __ATOMIC_ACQUIRE), expected = 0;
    if ((v & 3) == 2) {
      if (context)
        *context = (void *)(v & ~(uintptr_t)3);
      return 1;
    }
    if (!v && __atomic_compare_exchange_n(once, &expected, 1, 0, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE)) {
      void *result = 0;
      if (!callback(once, parameter, &result)) {
        __atomic_store_n(once, 0, __ATOMIC_RELEASE);
        return 0;
      }
      __atomic_store_n(once, ((uintptr_t)result & ~(uintptr_t)3) | 2, __ATOMIC_RELEASE);
      if (context)
        *context = (void *)((uintptr_t)result & ~(uintptr_t)3);
      return 1;
    }
    Sleep(1);
  }
}
/* Only the documented RTL_CRITICAL_SECTION_FLAG_* bits are accepted. */
/* RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO (0x10000000, winnt.h) makes NT
 * allocate the RTL_CRITICAL_SECTION_DEBUG block; the reused Wine DLLs ask for
 * it and then name the section through DebugInfo->Spare[0] (msvcrt lock
 * table, combase apartments, rpcrt4, comctl32 animation, ole32 property
 * storage). CRITICAL_SECTION_NO_DEBUG_INFO (0x01000000) wins; without the
 * force flag DebugInfo stays NULL. RtlDeleteCriticalSection frees it. */
BOOL W32API InitializeCriticalSectionEx(RTL_CRITICAL_SECTION *section, DWORD spin, DWORD flags) {
  if (!section || (flags & 0x00ffffffu)) {
    SetLastError(87);
    return 0;
  }
  RTL_CRITICAL_SECTION_DEBUG *debug = 0;
  if ((flags & 0x10000000u) && !(flags & 0x01000000u)) {
    debug = RtlAllocateHeap(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*debug));
    if (!debug) {
      SetLastError(8);
      return 0;
    }
  }
  if (!k32_status(RtlInitializeCriticalSectionAndSpinCount(section, spin & 0x00ffffffu))) {
    if (debug)
      RtlFreeHeap(GetProcessHeap(), 0, debug);
    return 0;
  }
  if (debug) {
    debug->CreatorBackTraceIndex = W32_CS_DEBUG_OWNED;
    debug->CriticalSection = section;
    debug->ProcessLocksList.Flink = debug->ProcessLocksList.Blink = &debug->ProcessLocksList;
    section->DebugInfo = debug;
  }
  return 1;
}
/* Delay-load resolution (IMAGE_DELAYLOAD_DESCRIPTOR, RVA or VA form): load
 * the DLL once per descriptor, bind the thunk, call the DLL failure hook,
 * then the system hook, on a missing DLL (3) or entry point (4). */
struct delay_info {
  ULONG size;
  const void *descriptor;
  void **thunk;
  const char *dll;
  ULONG by_name;
  const char *name;
  void *module, *unused;
  ULONG error;
};
typedef void *(W32API *k32_dll_hook)(ULONG, void *);
typedef void *(W32API *k32_system_hook)(const char *, const char *);
void *W32API ResolveDelayLoadedAPI(void *base, const uint8_t *d, k32_dll_hook dll_hook,
                                   k32_system_hook system_hook, void **thunk, ULONG flags) {
  (void)flags;
  uintptr_t b = (uintptr_t)base, shift = k32_u32(d) & 1 ? b : 0;
  const char *dll = (const char *)(shift + k32_u32(d + 4));
  HANDLE *slot = (HANDLE *)(shift + k32_u32(d + 8));
  void **iat = (void **)(shift + k32_u32(d + 12));
  const uintptr_t *names = (const uintptr_t *)(shift + k32_u32(d + 16));
  uintptr_t entry = names[thunk - iat];
  const uintptr_t ordinal_flag = (uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1);
  const char *name = entry & ordinal_flag ? (const char *)(entry & 0xffff)
                                          : (const char *)(shift + (DWORD)entry + 2);
  struct delay_info info = {sizeof(info), d, thunk, dll, !(entry & ordinal_flag), name, 0, 0, 0};
  void *proc = 0;
  HANDLE h = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  if (!h) {
    HANDLE loaded = LoadLibraryA(dll), expected = 0;
    if (loaded && !__atomic_compare_exchange_n(slot, &expected, loaded, 0, __ATOMIC_ACQ_REL,
                                               __ATOMIC_ACQUIRE))
      FreeLibrary(loaded);
    h = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
  }
  ULONG reason = 3;
  if (h) {
    proc = GetProcAddress(h, name);
    reason = 4;
  }
  if (!proc) {
    info.module = h;
    info.error = GetLastError();
    if (dll_hook)
      proc = dll_hook(reason, &info);
    if (!proc && system_hook)
      proc = system_hook(dll, name);
  }
  if (proc)
    *thunk = proc;
  return proc;
}
/* Default system hook: one W21 diagnostic, then the non-continuable
 * Visual C++ delay-load exception (module 0xC06D007E or entry point
 * 0xC06D007F), as a delay-load helper without a hook raises. */
void *W32API DelayLoadFailureHook(const char *dll, const char *name) {
  char text[96];
  DWORD n = 0;
  for (const char *p = dll ? dll : "?"; *p && n < 60; p++)
    text[n++] = *p;
  text[n++] = '!';
  if ((uintptr_t)name >> 16)
    for (const char *p = name; *p && n < sizeof(text) - 1; p++)
      text[n++] = *p;
  else
    text[n++] = '#';
  text[n] = 0;
  w32_diag("stub", "kernel32.dll", text);
  HANDLE loaded = dll ? GetModuleHandleA(dll) : 0;
  ULONG code = loaded ? 0xc06d007fu : 0xc06d007eu, argument = 0;
  SetLastError(loaded ? 127 : 126);
  RaiseException(code, 1, 1, &argument);
  return 0;
}
/* Global atoms live in w32srv (shared, reference counted, UTF-8 names);
 * integer atoms 1..0xBFFF never reach the service. */
static DWORD atom_call(DWORD op, DWORD key, const char *utf8, struct w32r_reply *reply,
                       char *name, DWORD cap) {
  struct {
    struct w32r_request r;
    char tail[1 + 768];
  } a;
  DWORD n = utf8 ? w32_strlen(utf8) + 1 : 1;
  if (n > 768)
    return 87;
  a.r = (struct w32r_request){.version = 1, .op = op, .key = key, .path_bytes = 1,
                              .name_bytes = n};
  a.tail[0] = 0;
  w32_copy(a.tail + 1, utf8 ? utf8 : "", n);
  struct {
    struct w32r_reply r;
    char data[768];
  } out;
  DWORD used = 0, e = w32_server_exchange(&a, sizeof(a.r) + 1 + n, &out, sizeof(out), &used);
  if (!e && (used < sizeof(out.r) || out.r.version != 1 || out.r.name_bytes ||
             used != sizeof(out.r) + out.r.data_bytes || out.r.data_bytes > sizeof(out.data)))
    e = 13;
  if (!e)
    e = out.r.error;
  if (!e) {
    *reply = out.r;
    if (name && cap) {
      DWORD k = out.r.data_bytes < cap ? out.r.data_bytes : cap;
      w32_copy(name, out.data, k);
      name[k ? k - 1 : 0] = 0;
    }
  }
  return e;
}
static int utf8(const WCHAR *w, char out[768]) {
  DWORD at = 0;
  for (; *w; w++) {
    DWORD c = *w;
    if (c >= 0xd800 && c <= 0xdfff)
      return 0;
    DWORD size = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
    if (at + size >= 768)
      return 0;
    if (size == 1)
      out[at++] = (char)c;
    else if (size == 2) {
      out[at++] = (char)(0xc0 | (c >> 6));
      out[at++] = (char)(0x80 | (c & 63));
    } else {
      out[at++] = (char)(0xe0 | (c >> 12));
      out[at++] = (char)(0x80 | ((c >> 6) & 63));
      out[at++] = (char)(0x80 | (c & 63));
    }
  }
  out[at] = 0;
  return 1;
}
static uint16_t atom(DWORD op, const void *name, int wide) {
  uintptr_t v = (uintptr_t)name;
  if (v <= 0xffff) {
    if (!v || v >= 0xc000) {
      SetLastError(87);
      return 0;
    }
    return (uint16_t)v;
  }
  WCHAR w[256];
  char text[768];
  if (wide) {
    DWORD n = k32_wlen(name);
    if (n > 255) {
      SetLastError(87);
      return 0;
    }
    w32_copy(w, name, (n + 1) * 2);
  } else if (w32_strlen(name) > 255 || !MultiByteToWideChar(1252, 0, name, -1, w, 256)) {
    SetLastError(87);
    return 0;
  }
  if (!utf8(w, text)) {
    SetLastError(87);
    return 0;
  }
  struct w32r_reply reply;
  DWORD e = atom_call(op, 0, text, &reply, 0, 0);
  if (e) {
    SetLastError(op == W32R_ATOM_FIND && e == 6 ? 2 : e);
    return 0;
  }
  return (uint16_t)reply.key;
}
uint16_t W32API GlobalAddAtomA(const char *name) { return atom(W32R_ATOM_ADD, name, 0); }
uint16_t W32API GlobalAddAtomW(const WCHAR *name) { return atom(W32R_ATOM_ADD, name, 1); }
uint16_t W32API GlobalFindAtomA(const char *name) { return atom(W32R_ATOM_FIND, name, 0); }
uint16_t W32API GlobalFindAtomW(const WCHAR *name) { return atom(W32R_ATOM_FIND, name, 1); }
/* Zero on success, the atom itself on failure. */
uint16_t W32API GlobalDeleteAtom(uint16_t value) {
  if (value < 0xc000)
    return 0;
  struct w32r_reply reply;
  DWORD e = atom_call(W32R_ATOM_DELETE, value, 0, &reply, 0, 0);
  if (e)
    SetLastError(e);
  return e ? value : 0;
}
static DWORD atom_name(uint16_t value, void *out, int cap, int wide) {
  if (!out || cap <= 0 || !value) {
    SetLastError(87);
    return 0;
  }
  struct w32r_reply reply;
  char text[768];
  DWORD e = atom_call(W32R_ATOM_NAME, value, 0, &reply, text, sizeof(text));
  if (e) {
    SetLastError(e);
    return 0;
  }
  WCHAR w[256];
  DWORD n = 0;
  for (const uint8_t *p = (const uint8_t *)text; *p && n < 255;) {
    DWORD c = *p++;
    if (c >= 0xe0 && p[0] && p[1]) {
      c = ((c & 15) << 12) | ((p[0] & 63u) << 6) | (p[1] & 63u);
      p += 2;
    } else if (c >= 0xc0 && p[0]) {
      c = ((c & 31) << 6) | (p[0] & 63u);
      p++;
    }
    w[n++] = (WCHAR)c;
  }
  DWORD copy = n < (DWORD)cap ? n : (DWORD)cap - 1;
  if (wide) {
    w32_copy(out, w, copy * 2);
    ((WCHAR *)out)[copy] = 0;
  } else {
    if (copy)
      WideCharToMultiByte(1252, 0, w, (int)copy, out, (int)copy, 0, 0);
    ((char *)out)[copy] = 0;
  }
  return copy;
}
DWORD W32API GlobalGetAtomNameA(uint16_t value, char *out, int cap) {
  return atom_name(value, out, cap, 0);
}
DWORD W32API GlobalGetAtomNameW(uint16_t value, WCHAR *out, int cap) {
  return atom_name(value, out, cap, 1);
}
/* Time-zone conversion with the documented TIME_ZONE_INFORMATION rules:
 * a zero daylight month means no daylight time; a zero year means the
 * wDay-th (5 = last) wDayOfWeek of the month at wHour:wMinute. */
static uint64_t ticks(const SYSTEMTIME *s) {
  FILETIME f = {0, 0};
  SystemTimeToFileTime(s, &f);
  return ((uint64_t)f.high << 32) | f.low;
}
static int month_days(unsigned y, unsigned m) {
  static const uint8_t d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return d[m - 1] + (m == 2 && !(y % 4) && (y % 100 || !(y % 400)));
}
static uint64_t transition(const SYSTEMTIME *rule, unsigned year) {
  SYSTEMTIME s = *rule;
  if (!rule->year) {
    s.year = (uint16_t)year;
    SYSTEMTIME first = {(uint16_t)year, rule->month, 0, 1, 0, 0, 0, 0};
    FILETIME f;
    SystemTimeToFileTime(&first, &f);
    FileTimeToSystemTime(&f, &first);
    unsigned day = 1 + (7 + rule->weekday - first.weekday) % 7 + 7 * (rule->day - 1u);
    while (day > (unsigned)month_days(year, rule->month))
      day -= 7;
    s.day = (uint16_t)day;
  }
  s.weekday = 0;
  return ticks(&s);
}
BOOL W32API SystemTimeToTzSpecificLocalTime(const TIME_ZONE_INFORMATION *zone,
                                            const SYSTEMTIME *utc, SYSTEMTIME *local) {
  TIME_ZONE_INFORMATION current;
  if (!zone) {
    GetTimeZoneInformation(&current);
    zone = &current;
  }
  FILETIME f;
  if (!utc || !local || !SystemTimeToFileTime(utc, &f)) {
    SetLastError(87);
    return 0;
  }
  int64_t t = (int64_t)(((uint64_t)f.high << 32) | f.low), minute = 600000000;
  LONG bias = zone->bias + zone->standard_bias;
  if (zone->daylight_date.month && zone->standard_date.month) {
    int64_t start = (int64_t)transition(&zone->daylight_date, utc->year) +
                    (int64_t)(zone->bias + zone->standard_bias) * minute;
    int64_t end = (int64_t)transition(&zone->standard_date, utc->year) +
                  (int64_t)(zone->bias + zone->daylight_bias) * minute;
    int daylight = start < end ? t >= start && t < end : t >= start || t < end;
    if (daylight)
      bias = zone->bias + zone->daylight_bias;
  }
  t -= (int64_t)bias * minute;
  if (t < 0) {
    SetLastError(87);
    return 0;
  }
  f = (FILETIME){(DWORD)(uint64_t)t, (DWORD)((uint64_t)t >> 32)};
  return FileTimeToSystemTime(&f, local);
}
