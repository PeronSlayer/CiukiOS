/* Fixed/moveable local and global blocks, serialized with the PEB lock.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
struct block {
  struct block *next;
  void *data;
  DWORD bytes, flags, locks;
};
static struct block *blocks;
static RTL_CRITICAL_SECTION *lock(void) {
  return NtCurrentTeb()->ProcessEnvironmentBlock->FastPebLock;
}
static struct block *find(HANDLE h) {
  for (struct block *b = blocks; b; b = b->next)
    if (((b->flags & 2) ? (void *)b : b->data) == h)
      return b;
  return 0;
}
HANDLE W32API GlobalAlloc(DWORD flags, DWORD n) {
  /* GMEM_MOVEABLE 2, NOCOMPACT 0x10, NODISCARD 0x20, ZEROINIT 0x40,
   * DISCARDABLE 0x100, LOWER/NOT_BANKED 0x1000, DDESHARE 0x2000 and NOTIFY
   * 0x4000 are valid on Windows 98; NODISCARD, LOWER and NOTIFY have no
   * effect in Win32. Reused DLLs pass NODISCARD (coml2 and ole32 lock bytes on
   * HGLOBAL: GlobalAlloc(GMEM_MOVEABLE | GMEM_NODISCARD, 0)). */
  if (flags & ~(0x4000u | 0x2000u | 0x1000u | 0x100u | 0x40u | 0x20u | 0x10u | 2u)) {
    SetLastError(87);
    return 0;
  }
  HANDLE heap = GetProcessHeap();
  struct block *b = HeapAlloc(heap, HEAP_ZERO_MEMORY, sizeof(*b));
  if (!b) {
    SetLastError(8);
    return 0;
  }
  b->flags = flags;
  b->bytes = n;
  if (n || !(flags & 2))
    b->data = HeapAlloc(heap, flags & 0x40 ? HEAP_ZERO_MEMORY : 0, n);
  if ((n || !(flags & 2)) && !b->data) {
    HeapFree(heap, 0, b);
    SetLastError(8);
    return 0;
  }
  RtlEnterCriticalSection(lock());
  b->next = blocks;
  blocks = b;
  RtlLeaveCriticalSection(lock());
  return flags & 2 ? (void *)b : b->data;
}
HANDLE W32API GlobalFree(HANDLE h) {
  if (!h)
    return 0;
  RtlEnterCriticalSection(lock());
  struct block **at = &blocks;
  while (*at && (((*at)->flags & 2) ? (void *)*at : (*at)->data) != h)
    at = &(*at)->next;
  if (!*at) {
    RtlLeaveCriticalSection(lock());
    SetLastError(6);
    return h;
  }
  struct block *b = *at;
  *at = b->next;
  RtlLeaveCriticalSection(lock());
  HANDLE heap = GetProcessHeap();
  if (b->data)
    HeapFree(heap, 0, b->data);
  HeapFree(heap, 0, b);
  return 0;
}
void *W32API GlobalLock(HANDLE h) {
  RtlEnterCriticalSection(lock());
  struct block *b = find(h);
  void *p = b ? b->data : 0;
  if (b && p && (b->flags & 2) && b->locks < 255)
    b->locks++;
  RtlLeaveCriticalSection(lock());
  if (!b)
    SetLastError(6);
  return p;
}
BOOL W32API GlobalUnlock(HANDLE h) {
  RtlEnterCriticalSection(lock());
  struct block *b = find(h);
  if (!b) {
    RtlLeaveCriticalSection(lock());
    SetLastError(6);
    return 0;
  }
  if (!(b->flags & 2)) {
    RtlLeaveCriticalSection(lock());
    return 1;
  }
  if (!b->locks) {
    RtlLeaveCriticalSection(lock());
    SetLastError(158);
    return 0;
  }
  BOOL remains = !!--b->locks;
  RtlLeaveCriticalSection(lock());
  if (!remains)
    SetLastError(0);
  return remains;
}
DWORD W32API GlobalSize(HANDLE h) {
  RtlEnterCriticalSection(lock());
  struct block *b = find(h);
  DWORD n = b ? b->bytes : 0;
  RtlLeaveCriticalSection(lock());
  if (!b)
    SetLastError(6);
  return n;
}
DWORD W32API GlobalFlags(HANDLE h) {
  RtlEnterCriticalSection(lock());
  struct block *b = find(h);
  DWORD f =
      b ? b->locks | (b->flags & 0x100) | (!b->data ? 0x4000 : 0) : 0x8000;
  RtlLeaveCriticalSection(lock());
  if (!b)
    SetLastError(6);
  return f;
}
HANDLE W32API GlobalHandle(const void *p) {
  RtlEnterCriticalSection(lock());
  HANDLE h = 0;
  for (struct block *b = blocks; b; b = b->next)
    if (b->data == p || b == p) {
      h = b->flags & 2 ? (void *)b : b->data;
      break;
    }
  RtlLeaveCriticalSection(lock());
  if (!h)
    SetLastError(6);
  return h;
}
HANDLE W32API GlobalReAlloc(HANDLE h, DWORD n, DWORD flags) {
  HANDLE heap = GetProcessHeap();
  RtlEnterCriticalSection(lock());
  struct block *b = find(h);
  if (!b) {
    RtlLeaveCriticalSection(lock());
    SetLastError(6);
    return 0;
  }
  if (flags & 0x80) {
    b->flags = (b->flags & ~0x100u) | (flags & 0x100);
    RtlLeaveCriticalSection(lock());
    return h;
  }
  if (!n && (b->flags & 2)) {
    if (b->locks) {
      RtlLeaveCriticalSection(lock());
      SetLastError(157);
      return 0;
    }
    if (b->data)
      HeapFree(heap, 0, b->data);
    b->data = 0;
    b->bytes = 0;
    RtlLeaveCriticalSection(lock());
    return h;
  }
  void *p = b->data ? HeapReAlloc(heap,
                                  (flags & 0x40 ? HEAP_ZERO_MEMORY : 0) |
                                      (!(b->flags & 2) && !(flags & 2)
                                           ? HEAP_REALLOC_IN_PLACE_ONLY
                                           : 0),
                                  b->data, n)
                    : HeapAlloc(heap, flags & 0x40 ? HEAP_ZERO_MEMORY : 0, n);
  if (!p) {
    RtlLeaveCriticalSection(lock());
    SetLastError(8);
    return 0;
  }
  b->data = p;
  b->bytes = n;
  h = b->flags & 2 ? (void *)b : p;
  RtlLeaveCriticalSection(lock());
  return h;
}
HANDLE W32API LocalAlloc(DWORD f, DWORD n) { return GlobalAlloc(f, n); }
HANDLE W32API LocalFree(HANDLE h) { return GlobalFree(h); }
void *W32API LocalLock(HANDLE h) { return GlobalLock(h); }
BOOL W32API LocalUnlock(HANDLE h) { return GlobalUnlock(h); }
DWORD W32API LocalSize(HANDLE h) { return GlobalSize(h); }
DWORD W32API LocalFlags(HANDLE h) { return GlobalFlags(h); }
HANDLE W32API LocalHandle(const void *p) { return GlobalHandle(p); }
HANDLE W32API LocalReAlloc(HANDLE h, DWORD n, DWORD f) {
  return GlobalReAlloc(h, n, f);
}
/* Win16 compatibility entry points kept by Win32 (the InstallShield 5 engine
 * imports GlobalCompact). There are no discardable segments to compact: the
 * result is the largest free block of the process heap, as on Windows NT and
 * ReactOS (RtlCompactHeap of the process heap); minimum-free and new-size
 * arguments are ignored. Fix/Wire pin a block by locking it. */
DWORD W32API HeapCompact(HANDLE, DWORD);
DWORD W32API GlobalCompact(DWORD min_free) {
  (void)min_free;
  return HeapCompact(GetProcessHeap(), 0);
}
DWORD W32API LocalCompact(DWORD min_free) { return GlobalCompact(min_free); }
DWORD W32API LocalShrink(HANDLE h, DWORD size) {
  (void)h;
  (void)size;
  return GlobalCompact(0);
}
void W32API GlobalFix(HANDLE h) { GlobalLock(h); }
void W32API GlobalUnfix(HANDLE h) { GlobalUnlock(h); }
void *W32API GlobalWire(HANDLE h) { return GlobalLock(h); }
BOOL W32API GlobalUnWire(HANDLE h) { return GlobalUnlock(h); }
