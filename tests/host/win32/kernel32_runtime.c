/* Exercise exported facade functions through production CIUKI_SYS_W32 VM,
 * object, wait and clock services. Native H0, no PE instruction execution.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "adapter/linux.h"
#include <stdio.h>
#include <stdlib.h>
#include <w32/kernel32.h>
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "kernel32 line %u: %s (error %u)\n", __LINE__, #c,       \
              GetLastError());                                                 \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
extern HANDLE W32API CreateEventA(SECURITY_ATTRIBUTES *, BOOL, BOOL,
                                  const char *);
extern BOOL W32API SetEvent(HANDLE);
extern DWORD W32API GetVersion(void);
extern BOOL W32API GetVersionExA(OSVERSIONINFOA *);
extern BOOL W32API QueryPerformanceCounter(int64_t *);
extern BOOL W32API QueryPerformanceFrequency(int64_t *);
extern void W32API GlobalMemoryStatus(MEMORYSTATUS *);
extern HANDLE W32API GlobalAlloc(DWORD, DWORD);
extern HANDLE W32API GlobalFree(HANDLE);
extern void *W32API GlobalLock(HANDLE);
extern BOOL W32API GlobalUnlock(HANDLE);
extern DWORD W32API GlobalSize(HANDLE);
extern HANDLE W32API GlobalReAlloc(HANDLE, DWORD, DWORD);
extern BOOL W32API SetEnvironmentVariableA(const char *, const char *);
extern HANDLE W32API CreateWaitableTimerA(SECURITY_ATTRIBUTES *, BOOL,
                                          const char *);
extern BOOL W32API SetWaitableTimer(HANDLE, const int64_t *, LONG, void *,
                                    void *, BOOL);
extern void *W32API ConvertThreadToFiber(void *);
extern BOOL W32API ConvertFiberToThread(void);
extern void *W32API GetFiberData(void);
extern HANDLE W32API CreateToolhelp32Snapshot(DWORD, DWORD);
struct thread_entry {
  DWORD size, usage, tid, owner;
  LONG priority, delta;
  DWORD flags;
};
extern BOOL W32API Thread32First(HANDLE, struct thread_entry *);
extern BOOL W32API Thread32Next(HANDLE, struct thread_entry *);
int w32_kernel32_fixed_alloc(void) {
  struct ciuki_w32_memory_info q;
  void *at = (void *)0x6fff0000;
  /* Exact guest arguments before the loader owns the page. */
  CHECK(VirtualAlloc(at, 4096, 0x3000, 4) == at);
  CHECK(VirtualQuery(at, &q, sizeof(q)) == sizeof(q) &&
        q.allocation_base == 0x6fff0000 && q.region_size == 4096 &&
        q.state == 0x1000 && q.protect == 4 && q.type == 0x20000);
  CHECK(!*(volatile uint32_t *)at);
  *(volatile uint32_t *)at = 0xfeed;
  CHECK(!VirtualAlloc(at, 4096, 0x3000, 4) && GetLastError() == 487);
  CHECK(*(volatile uint32_t *)at == 0xfeed);
  CHECK(VirtualFree(at, 0, 0x8000));
  /* Reserve rounds down to 64 KiB, end up to 4 KiB, including both
   * requested bytes that straddle a page. Commit rounds down to a page. */
  CHECK(VirtualAlloc((void *)0x6fff1fff, 2, 0x2000, 4) == at);
  CHECK(VirtualQuery(at, &q, sizeof(q)) == sizeof(q) &&
        q.region_size == 0x3000 && q.state == 0x2000);
  CHECK(!VirtualAlloc((void *)0x6fff2000, 4096, 0x2000, 4) && GetLastError() == 487);
  CHECK(VirtualAlloc((void *)0x6fff1fff, 2, 0x1000, 4) == (void *)0x6fff1000);
  CHECK(VirtualQuery((void *)0x6fff1000, &q, sizeof(q)) == sizeof(q) &&
        q.region_size == 0x2000 && q.state == 0x1000 && q.protect == 4);
  CHECK(!*(volatile uint32_t *)0x6fff1000 && !*(volatile uint32_t *)0x6fff2000);
  CHECK(VirtualFree(at, 0, 0x8000));
  CHECK(!VirtualAlloc(at, 4096, 0x1000, 4) && GetLastError() == 487);
  CHECK(!VirtualAlloc((void *)1, 4096, 0x3000, 4) && GetLastError() == 487);
  CHECK(!VirtualAlloc(at, 0, 0x3000, 4) && GetLastError() == 87);
  CHECK(!VirtualAlloc(at, 4096, 0x3000, 0) && GetLastError() == 87);
  CHECK(!VirtualAlloc((void *)0xfffff000, 8192, 0x3000, 4) && GetLastError() == 87);
  CHECK(!VirtualAlloc((void *)0x70000000, 4096, 0x3000, 4) && GetLastError() == 87);
  puts("PASS kernel32 fixed VirtualAlloc exact rounding collision=487 invalid=87 zero RW release");
  return 0;
}
int w32_kernel32_runtime(void) {
  static RTL_CRITICAL_SECTION cs;
  RtlInitializeCriticalSection(&cs);
  host.peb.FastPebLock = &cs;
  SetLastError(0xabc);
  CHECK(GetLastError() == 0xabc);
  CHECK(GetVersion() == 0x88ae0a04u);
  OSVERSIONINFOA version = {.size = sizeof(version)};
  CHECK(GetVersionExA(&version));
  CHECK(version.major == 4 && version.minor == 10 && version.build == 2222 &&
        version.platform == 1 && !w32_strcmp(version.csd, " A "));
  version.size = 1;
  version.major = 0x12345678;
  CHECK(!GetVersionExA(&version) && version.major == 0x12345678 &&
        GetLastError() == 87);
  void *p = VirtualAlloc(0, 8192, 0x3000, 4);
  CHECK(p);
  struct ciuki_w32_memory_info q;
  CHECK(VirtualQuery(p, &q, sizeof(q)) == 28 && q.state == 0x1000 &&
        q.type == 0x20000 && q.region_size == 8192);
  DWORD old = 0xfeed;
  CHECK(!VirtualProtect(p, 8192, 0, &old) && old == 0xfeed &&
        GetLastError() == 87);
  CHECK(VirtualProtect(p, 8192, 2, &old) && old == 4);
  CHECK(VirtualFree(p, 0, 0x8000));
  p=VirtualAlloc(0,8192,0x3000,4);CHECK(p);
  char *src=p,*dst=(char *)p+4096;src[0]='A';src[1]='B';DWORD copied=99;
  CHECK(ReadProcessMemory(K32_INVALID,src,dst,2,&copied)&&copied==2&&dst[0]=='A'&&dst[1]=='B');
  dst[0]='C';CHECK(WriteProcessMemory(K32_INVALID,src,dst,2,&copied)&&src[0]=='C'&&copied==2);
  CHECK(VirtualProtect(dst,4096,2,&old));
  CHECK(!WriteProcessMemory(K32_INVALID,dst,src,2,&copied)&&GetLastError()==299&&!copied&&dst[0]=='C');
  CHECK(!ReadProcessMemory((HANDLE)0xdead,src,src,2,&copied)&&GetLastError()==6&&!copied);
  CHECK(VirtualFree(p,0,0x8000));
  HANDLE heap = HeapCreate(0, 4096, 0);
  CHECK(heap);
  uint8_t *b = HeapAlloc(heap, 8, 31);
  CHECK(b && b[0] == 0 && b[30] == 0);
  b[0] = 0x5a;
  b = HeapReAlloc(heap, 8, b, 70000);
  CHECK(b && b[0] == 0x5a && b[69999] == 0);
  CHECK(HeapFree(heap, 0, b) && HeapDestroy(heap));
  HANDLE g = GlobalAlloc(0x42, 20);
  CHECK(g);
  b = GlobalLock(g);
  CHECK(b && b[19] == 0);
  CHECK(!GlobalUnlock(g) && GetLastError() == 0);
  g = GlobalReAlloc(g, 40, 0x42);
  CHECK(g && GlobalSize(g) == 40 && !GlobalFree(g));
  /* f5-20D: GMEM_NODISCARD (0x20) with a zero-byte moveable block, as coml2
   * and ole32 request it for their HGLOBAL lock bytes; GMEM_LOWER (0x1000)
   * and GMEM_NOTIFY (0x4000) are accepted and ignored; unknown bits fail. */
  g = GlobalAlloc(2 | 0x20, 0);
  CHECK(g && GlobalSize(g) == 0 && GlobalLock(g) == 0);
  g = GlobalReAlloc(g, 24, 0);
  b = GlobalLock(g);
  CHECK(g && b && GlobalSize(g) == 24 && !GlobalUnlock(g) && !GlobalFree(g));
  g = GlobalAlloc(2 | 0x1000 | 0x4000, 8);
  CHECK(g && !GlobalFree(g));
  CHECK(!GlobalAlloc(0x8000, 8) && GetLastError() == 87);
  HANDLE event = CreateEventA(0, 0, 0, 0);
  CHECK(event);
  CHECK(WaitForSingleObject(event, 0) == 258);
  CHECK(SetEvent(event));
  CHECK(WaitForSingleObject(event, 0) == 0 &&
        WaitForSingleObject(event, 0) == 258);
  CHECK(CloseHandle(event));
  CHECK(!CloseHandle(event) && GetLastError() == 6);
  CHECK(WaitForMultipleObjectsEx(0, 0, 0, 0, 0) == ~0u && GetLastError() == 87);
  HANDLE timer = CreateWaitableTimerA(0, 0, 0);
  CHECK(timer);
  int64_t due = 0;
  CHECK(SetWaitableTimer(timer, &due, 0, 0, 0, 0) &&
        WaitForSingleObject(timer, 0) == 0 && CloseHandle(timer));
  void *fiber = ConvertThreadToFiber((void *)0xc1a0);
  CHECK(fiber && *(void **)fiber == (void *)0xc1a0 &&
        GetFiberData() == (void *)0xc1a0 && ConvertFiberToThread());
  HANDLE snap = CreateToolhelp32Snapshot(4, 0);
  CHECK(snap != K32_INVALID);
  struct thread_entry te = {.size = sizeof(te)};
  BOOL seen = 0;
  for (BOOL more = Thread32First(snap, &te); more;
       more = Thread32Next(snap, &te))
    if (te.tid == K32_PTR(host.teb.ClientId.UniqueThread)) {
      CHECK(te.owner == K32_PTR(host.teb.ClientId.UniqueProcess));
      seen = 1;
    }
  CHECK(seen && GetLastError() == 18 && CloseHandle(snap));
  DWORD indices[64];
  for (unsigned i = 0; i < 64; i++) {
    indices[i] = TlsAlloc();
    CHECK(indices[i] == i);
  }
  CHECK(TlsAlloc() == ~0u && GetLastError() == 8);
  CHECK(TlsSetValue(7, (void *)0xdead));
  SetLastError(99);
  CHECK(TlsGetValue(7) == (void *)0xdead && GetLastError() == 0);
  CHECK(TlsFree(7));
  CHECK(TlsAlloc() == 7 && TlsGetValue(7) == 0);
  for (unsigned i = 0; i < 64; i++)
    CHECK(TlsFree(i));
  WCHAR out[5] = {0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa, 0xaaaa};
  CHECK(MultiByteToWideChar(1252, 0, "\x80\xe9", -1, 0, 0) == 3);
  CHECK(!MultiByteToWideChar(1252, 0, "\x80\xe9", -1, out, 2) &&
        GetLastError() == 122 && out[0] == 0xaaaa);
  CHECK(MultiByteToWideChar(1252, 0, "\x80\xe9", -1, out, 5) == 3 &&
        out[0] == 0x20ac && out[1] == 0xe9 && out[2] == 0 && out[3] == 0xaaaa);
  char ansi[5] = {1, 2, 3, 4, 5};
  BOOL used = 99;
  CHECK(WideCharToMultiByte(1252, 0, out, 3, ansi, 5, 0, &used) == 3 && !used &&
        (uint8_t)ansi[0] == 0x80 && ansi[3] == 4);
  out[0] = 0x2603;
  CHECK(WideCharToMultiByte(437, 0, out, 1, ansi, 5, 0, &used) == 1 &&
        ansi[0] == '?' && used);
  SYSTEMTIME date = {.year = 2000,
                     .month = 2,
                     .day = 29,
                     .hour = 23,
                     .minute = 59,
                     .second = 58,
                     .milliseconds = 123},
             round;
  FILETIME ft;
  CHECK(SystemTimeToFileTime(&date, &ft) && FileTimeToSystemTime(&ft, &round) &&
        round.year == 2000 && round.day == 29 && round.milliseconds == 123);
  date.year = 1900;
  ft = (FILETIME){0xaaaa, 0xbbbb};
  CHECK(!SystemTimeToFileTime(&date, &ft) && ft.low == 0xaaaa &&
        ft.high == 0xbbbb);
  date = (SYSTEMTIME){.year = 1601, .month = 1, .day = 1};
  CHECK(SystemTimeToFileTime(&date, &ft) && !ft.low && !ft.high);
  date = (SYSTEMTIME){.year = 1970, .month = 1, .day = 1};
  CHECK(SystemTimeToFileTime(&date, &ft) && ft.low == 0xd53e8000 &&
        ft.high == 0x019db1de);
  int64_t count, freq;
  CHECK(QueryPerformanceCounter(&count) && QueryPerformanceFrequency(&freq) &&
        freq == 1193182 && count >= 0);
  MEMORYSTATUS memory;
  GlobalMemoryStatus(&memory);
  CHECK(memory.length == 32 && memory.avail_phys <= memory.total_phys &&
        memory.total_virtual == 0x7ffe0000u);
  static struct w32_process_parameters params;
  host.peb.ProcessParameters = &params;
  CHECK(SetEnvironmentVariableA("TEMP", "C:\\TEMP"));
  char buffer[32];
  CHECK(GetEnvironmentVariableA("temp", buffer, sizeof(buffer)) == 7 &&
        !w32_strcmp(buffer, "C:\\TEMP"));
  CHECK(SetEnvironmentVariableA("A", "one") &&
        SetEnvironmentVariableA("B", "two"));
  CHECK(SetEnvironmentVariableA("a", "three"));
  CHECK(GetEnvironmentVariableA("B", buffer, sizeof(buffer)) == 3 &&
        !w32_strcmp(buffer, "two"));
  CHECK(SetEnvironmentVariableA("A", 0));
  CHECK(!GetEnvironmentVariableA("A", buffer, sizeof(buffer)) &&
        GetLastError() == 203);
  puts("PASS kernel32 VM heap objects waits TLS codepages time environment");
  /* f5-20A: process/thread queries and heap compaction on the production
   * object and heap services (named objects: kernel32_core_upper.c). */
  extern HANDLE W32API OpenSemaphoreA(DWORD, BOOL, const char *);
  extern BOOL W32API IsWow64Process(HANDLE, BOOL *);
  extern BOOL W32API ProcessIdToSessionId(DWORD, DWORD *);
  extern DWORD W32API GetThreadId(HANDLE);
  extern DWORD W32API HeapCompact(HANDLE, DWORD);
  extern HANDLE W32API GetCurrentProcess(void);
  extern HANDLE W32API GetCurrentThread(void);
  extern DWORD W32API GetCurrentProcessId(void);
  extern DWORD W32API GetCurrentThreadId(void);
  CHECK(!OpenSemaphoreA(0x1f0003, 0, 0) && GetLastError() == 87);
  HANDLE unnamed = CreateEventA(0, 0, 0, 0);
  CHECK(unnamed);
  BOOL wow64 = 7;
  CHECK(IsWow64Process(GetCurrentProcess(), &wow64) && !wow64);
  wow64 = 7;
  CHECK(!IsWow64Process(unnamed, &wow64) && GetLastError() == 6 && wow64 == 7);
  CHECK(!IsWow64Process(GetCurrentProcess(), 0) && GetLastError() == 87);
  DWORD session = 9;
  CHECK(ProcessIdToSessionId(GetCurrentProcessId(), &session) && !session);
  CHECK(!ProcessIdToSessionId(0x7ffffff0u, &session) && GetLastError() == 87);
  CHECK(!ProcessIdToSessionId(GetCurrentProcessId(), 0) && GetLastError() == 87);
  CHECK(GetThreadId(GetCurrentThread()) == GetCurrentThreadId());
  CHECK(!GetThreadId(unnamed) && GetLastError() == 6);
  CHECK(!GetThreadId((HANDLE)(uintptr_t)0x7ffc) && GetLastError() == 6);
  CHECK(CloseHandle(unnamed));
  HANDLE compact = HeapCreate(0, 65536, 0);
  void *block = HeapAlloc(compact, 0, 1000);
  CHECK(block && HeapFree(compact, 0, block));
  SetLastError(77);
  DWORD largest = HeapCompact(compact, 0);
  CHECK((largest >= 1000 || (!largest && !GetLastError())) && HeapDestroy(compact));
  static ULONG not_a_heap[64];
  CHECK(!HeapCompact(not_a_heap, 0) && GetLastError() == 6);
  puts("PASS kernel32 f5-20A process queries heap compaction");
  return 0;
}
