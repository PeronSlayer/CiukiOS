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
/* Resource key from an ID, an ANSI or a UTF-16 name: "#123" is the ID 123
 * and string names compare upper-cased, the form resource compilers store. */
static int resource_key(const void *name, int wide, WCHAR w[260], uintptr_t *out) {
  uintptr_t v = (uintptr_t)name;
  if (v <= 65535) {
    *out = v;
    return 1;
  }
  if (wide) {
    DWORD n = 0;
    while (((const WCHAR *)name)[n] && n < 259)
      n++;
    if (((const WCHAR *)name)[n]) {
      SetLastError(1814);
      return 0;
    }
    w32_copy(w, name, (n + 1) * 2);
  } else if (!MultiByteToWideChar(1252, 0, name, -1, w, 260))
    return 0;
  if (w[0] == '#') {
    DWORD id = 0, i = 1;
    for (; w[i] >= '0' && w[i] <= '9' && id <= 65535; i++)
      id = id * 10 + (w[i] - '0');
    if (i == 1 || w[i] || id > 65535) {
      SetLastError(1814);
      return 0;
    }
    *out = id;
    return 1;
  }
  for (DWORD i = 0; w[i]; i++)
    w[i] = RtlUpcaseUnicodeChar(w[i]);
  *out = (uintptr_t)w;
  return 1;
}
static HANDLE resource_any(HANDLE h, const void *name, const void *type, DWORD lang,
                           int wide) {
  WCHAR n[260], t[260];
  uintptr_t a, b;
  if (!resource_key(name, wide, n, &a) || !resource_key(type, wide, t, &b))
    return 0;
  struct w32_resource_info info = {b, a, lang};
  void *p = 0;
  if (!h)
    h = NtCurrentTeb()->ProcessEnvironmentBlock->ImageBaseAddress;
  NTSTATUS s = LdrFindResource_U(h, &info, 3, &p);
  if (s)
    SetLastError(1814);
  return s ? 0 : p;
}
/* Internal callers (FormatMessage) bypass the per-application w-stubs gate. */
HANDLE k32_find_resource(HANDLE h, const void *name, const void *type, DWORD lang, int wide) {
  return resource_any(h, name, type, lang, wide);
}
static HANDLE resource(HANDLE h, const void *name, const void *type,
                       DWORD lang) {
  return resource_any(h, name, type, lang, 0);
}
HANDLE W32API FindResourceW(HANDLE h, const WCHAR *name, const WCHAR *type) {
  return resource_any(h, name, type, 0x409, 1);
}
HANDLE W32API FindResourceExW(HANDLE h, const WCHAR *type, const WCHAR *name,
                              uint16_t lang) {
  return resource_any(h, name, type, lang, 1);
}
typedef BOOL(W32API *k32_name_proc)(HANDLE, const void *, void *, intptr_t);
/* Names of one resource type in directory order (named entries first);
 * string names are handed to the callback NUL-terminated, in ACP 1252 for
 * the ANSI entry point. FALSE from the callback stops the enumeration. */
static BOOL enum_names(HANDLE h, const void *type, k32_name_proc proc, intptr_t param,
                       int wide) {
  WCHAR t[260];
  uintptr_t key;
  if (!proc) {
    SetLastError(87);
    return 0;
  }
  if (!h)
    h = NtCurrentTeb()->ProcessEnvironmentBlock->ImageBaseAddress;
  if (!resource_key(type, wide, t, &key))
    return 0;
  ULONG size = 0;
  const uint8_t *root = RtlImageDirectoryEntryToData(h, 1, 2, &size);
  struct w32_resource_info info = {key, 0, 0};
  void *found = 0;
  if (!root || size < 16) {
    SetLastError(1812);
    return 0;
  }
  if (LdrFindResourceDirectory_U(h, &info, 1, &found) || !found) {
    SetLastError(1813);
    return 0;
  }
  const uint8_t *dir = found;
  ULONG at = (ULONG)(dir - root);
  if (at > size - 16) {
    SetLastError(1812);
    return 0;
  }
  ULONG count = (ULONG)k32_u16(dir + 12) + k32_u16(dir + 14);
  if (count > (size - at - 16) / 8) {
    SetLastError(1812);
    return 0;
  }
  for (ULONG i = 0; i < count; i++) {
    ULONG id = k32_u32(dir + 16 + i * 8);
    WCHAR wname[260];
    char aname[260];
    void *arg = (void *)(uintptr_t)(id & 0xffff);
    if (id & 0x80000000u) {
      ULONG off = id & 0x7fffffffu;
      if (off > size - 2) {
        SetLastError(1812);
        return 0;
      }
      ULONG n = k32_u16(root + off);
      if (n > 259 || n > (size - off - 2) / 2) {
        SetLastError(1812);
        return 0;
      }
      w32_copy(wname, root + off + 2, n * 2);
      wname[n] = 0;
      if (!wide) {
        WideCharToMultiByte(1252, 0, wname, (int)n + 1, aname, sizeof(aname), 0, 0);
        arg = aname;
      } else
        arg = wname;
    }
    if (!proc(h, type, arg, param))
      break;
  }
  return 1;
}
BOOL W32API EnumResourceNamesA(HANDLE h, const char *type, k32_name_proc proc,
                               intptr_t param) {
  return enum_names(h, type, proc, param, 0);
}
BOOL W32API EnumResourceNamesW(HANDLE h, const WCHAR *type, k32_name_proc proc,
                               intptr_t param) {
  return enum_names(h, type, proc, param, 1);
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
