/* Loader facade; ntdll owns the module lists and loader lock.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
HANDLE W32API k32_load_w(const WCHAR *name, HANDLE file, DWORD flags) {
  if (!name || file || flags & ~3u) {
    SetLastError(87);
    return 0;
  }
  UNICODE_STRING u;
  RtlInitUnicodeString(&u, name);
  void *p = 0;
  NTSTATUS s = LdrLoadDll(0, &flags, &u, &p);
  if (s) {
    SetLastError(RtlNtStatusToDosError(s));
    return 0;
  }
  return p;
}
HANDLE W32API LoadLibraryExA(const char *name, HANDLE file, DWORD flags) {
  WCHAR w[260];
  if (!MultiByteToWideChar(1252, 0, name, -1, w, 260))
    return 0;
  return k32_load_w(w, file, flags);
}
HANDLE W32API LoadLibraryA(const char *name) {
  return LoadLibraryExA(name, 0, 0);
}

BOOL W32API FreeLibrary(HANDLE h) {
  NTSTATUS s = LdrUnloadDll(h);
  if (s) {
    SetLastError(RtlNtStatusToDosError(s));
    return 0;
  }
  return 1;
}
HANDLE W32API k32_module_w(const WCHAR *name) {
  UNICODE_STRING u;
  void *p = 0;
  if (name)
    RtlInitUnicodeString(&u, name);
  NTSTATUS s = LdrGetDllHandle(0, 0, name ? &u : 0, &p);
  if (s)
    SetLastError(RtlNtStatusToDosError(s));
  return s ? 0 : p;
}
HANDLE W32API GetModuleHandleA(const char *name) {
  WCHAR w[260];
  if (name && !MultiByteToWideChar(1252, 0, name, -1, w, 260))
    return 0;
  return k32_module_w(name ? w : 0);
}
void *W32API GetProcAddress(HANDLE h, const char *name) {
  if (!name) {
    SetLastError(127);
    return 0;
  }
  ANSI_STRING s = {0};
  DWORD ordinal = 0;
  void *p = 0;
  if ((uintptr_t)name > 65535)
    RtlInitAnsiString(&s, name);
  else
    ordinal = (DWORD)(uintptr_t)name;
  NTSTATUS result = LdrGetProcedureAddress(h, ordinal ? 0 : &s, ordinal, &p);
  if (result)
    SetLastError(RtlNtStatusToDosError(result));
  return result ? 0 : p;
}
DWORD W32API k32_filename_w(HANDLE h, WCHAR *out, DWORD cap) {
  struct w32_peb *peb = NtCurrentTeb()->ProcessEnvironmentBlock;
  if (!h)
    h = peb->ImageBaseAddress;
  if (!peb->Ldr || !cap || !out) {
    SetLastError(87);
    return 0;
  }
  LIST_ENTRY *head = &peb->Ldr->InLoadOrderModuleList;
  RtlEnterCriticalSection(peb->FastPebLock);
  for (LIST_ENTRY *e = head->Flink; e != head; e = e->Flink) {
    struct w32_ldr_entry *m = (void *)e;
    if (m->DllBase != h)
      continue;
    DWORD n = m->FullDllName.Length / 2;
    if (n >= cap)
      n = cap;
    w32_copy(out, m->FullDllName.Buffer, n * 2);
    if (n < cap)
      out[n] = 0;
    RtlLeaveCriticalSection(peb->FastPebLock);
    return n;
  }
  RtlLeaveCriticalSection(peb->FastPebLock);
  SetLastError(126);
  return 0;
}
DWORD W32API GetModuleFileNameA(HANDLE h, char *out, DWORD cap) {
  WCHAR w[260];
  DWORD n = k32_filename_w(h, w, 260);
  if (!n)
    return 0;
  if (!cap || !out) {
    SetLastError(87);
    return 0;
  }
  DWORD copy = n < cap ? n : cap;
  WideCharToMultiByte(1252, 0, w, (int)copy, out, (int)copy, 0, 0);
  if (n < cap)
    out[n] = 0;
  return copy;
}
BOOL W32API DisableThreadLibraryCalls(HANDLE h) {
  return k32_status(LdrDisableThreadCalloutsForDll(h));
}
struct w32_resource_info {
  uintptr_t Type, Name, Language;
};
static HANDLE resource(HANDLE h, const void *name, const void *type,
                       DWORD lang) {
  WCHAR n[260], t[260];
  uintptr_t a = (uintptr_t)name, b = (uintptr_t)type;
  if (a > 65535) {
    if (!MultiByteToWideChar(1252, 0, name, -1, n, 260))
      return 0;
    a = (uintptr_t)n;
  }
  if (b > 65535) {
    if (!MultiByteToWideChar(1252, 0, type, -1, t, 260))
      return 0;
    b = (uintptr_t)t;
  }
  struct w32_resource_info info = {b, a, lang};
  void *p = 0;
  if (!h)
    h = NtCurrentTeb()->ProcessEnvironmentBlock->ImageBaseAddress;
  NTSTATUS s = LdrFindResource_U(h, &info, 3, &p);
  if (s)
    SetLastError(1814);
  return s ? 0 : p;
}
HANDLE W32API FindResourceA(HANDLE h, const void *name, const void *type) {
  return resource(h, name, type, 0x409);
}
HANDLE W32API FindResourceExA(HANDLE h, const void *type, const void *name,
                              uint16_t lang) {
  return resource(h, name, type, lang);
}
HANDLE W32API LoadResource(HANDLE h, HANDLE r) {
  void *p = 0;
  if (!h)
    h = NtCurrentTeb()->ProcessEnvironmentBlock->ImageBaseAddress;
  NTSTATUS s = LdrAccessResource(h, r, &p, 0);
  if (s)
    SetLastError(1812);
  return s ? 0 : p;
}
DWORD W32API SizeofResource(HANDLE h, HANDLE r) {
  DWORD n = 0;
  if (!h)
    h = NtCurrentTeb()->ProcessEnvironmentBlock->ImageBaseAddress;
  NTSTATUS s = LdrAccessResource(h, r, 0, &n);
  if (s)
    SetLastError(1812);
  return s ? 0 : n;
}
void *W32API LockResource(HANDLE h) { return h; }
BOOL W32API FreeResource(HANDLE h) {
  (void)h;
  return 0;
}
DWORD W32API SetErrorMode(DWORD mode) {
  static DWORD current;
  DWORD old = current;
  current = mode;
  return old;
}
DWORD W32API SetHandleCount(DWORD n) { return n < 256 ? 256 : n; }
