/* Search path, executable type, environment expansion and the Win16
 * compatibility file API (OpenFile, _lopen family) of the Windows 98
 * KERNEL32, over the CiukiOS ANSI file facade. f5-20A.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
extern DWORD W32API GetSystemDirectoryA(char *, DWORD);
extern DWORD W32API GetWindowsDirectoryA(char *, DWORD);
static int separator(char c) { return c == '\\' || c == '/'; }
static int exists(const char *dir, const char *name, char *out) {
  char joined[262];
  DWORD n = w32_strlen(dir), m = w32_strlen(name);
  if (n + m + 2 > sizeof(joined))
    return 0;
  w32_copy(joined, dir, n);
  if (n && !separator(dir[n - 1]) && dir[n - 1] != ':')
    joined[n++] = '\\';
  w32_copy(joined + n, name, m + 1);
  char *part;
  DWORD r = GetFullPathNameA(joined, 262, out, &part);
  return r && r < 262 && GetFileAttributesA(out) != ~0u;
}
/* Default order of the Windows 98 SearchPath: the application directory,
 * the current directory, the system and Windows directories, then PATH. */
DWORD W32API SearchPathA(const char *path, const char *file, const char *ext, DWORD cap,
                         char *out, char **part) {
  if (!file || !*file) {
    SetLastError(87);
    return 0;
  }
  char name[262], found[262];
  DWORD n = w32_strlen(file), e = ext ? w32_strlen(ext) : 0;
  int dotted = 0, pathed = 0;
  for (DWORD i = 0; i < n; i++) {
    if (separator(file[i]) || file[i] == ':') {
      pathed = 1;
      dotted = 0;
    } else if (file[i] == '.')
      dotted = 1;
  }
  if (dotted)
    e = 0;
  if (n + e >= sizeof(name)) {
    SetLastError(206);
    return 0;
  }
  w32_copy(name, file, n);
  w32_copy(name + n, ext ? ext : "", e);
  name[n + e] = 0;
  int ok = 0;
  if (pathed)
    ok = exists("", name, found);
  else if (path) {
    for (const char *p = path; *p && !ok;) {
      char dir[262];
      DWORD k = 0;
      while (p[k] && p[k] != ';')
        k++;
      if (k && k < sizeof(dir)) {
        w32_copy(dir, p, k);
        dir[k] = 0;
        ok = exists(dir, name, found);
      }
      p += k + (p[k] == ';');
    }
  } else {
    char dir[262];
    DWORD r = GetModuleFileNameA(0, dir, sizeof(dir));
    if (r && r < sizeof(dir)) {
      while (r && !separator(dir[r - 1]))
        r--;
      dir[r] = 0;
      ok = r && exists(dir, name, found);
    }
    if (!ok)
      ok = exists("", name, found);
    if (!ok && GetSystemDirectoryA(dir, sizeof(dir)) < sizeof(dir))
      ok = exists(dir, name, found);
    if (!ok && GetWindowsDirectoryA(dir, sizeof(dir)) < sizeof(dir))
      ok = exists(dir, name, found);
    DWORD size = ok ? 0 : GetEnvironmentVariableA("PATH", 0, 0);
    char *list = size ? k32_alloc(size) : 0;
    if (list && GetEnvironmentVariableA("PATH", list, size) < size)
      ok = SearchPathA(list, name, 0, sizeof(found), found, 0) != 0;
    k32_free(list, size);
  }
  if (!ok) {
    SetLastError(2);
    return 0;
  }
  DWORD length = w32_strlen(found);
  if (cap <= length)
    return length + 1;
  if (!out) {
    SetLastError(87);
    return 0;
  }
  w32_copy(out, found, length + 1);
  if (part) {
    *part = out;
    for (DWORD i = 0; i < length; i++)
      if (separator(out[i]))
        *part = out + i + 1;
  }
  return length;
}
/* SCS_32BIT_BINARY 0, SCS_DOS_BINARY 1, SCS_WOW_BINARY 2, SCS_PIF_BINARY 3,
 * SCS_OS216_BINARY 5. DLLs and non-i386 PE images are not executables. */
BOOL W32API GetBinaryTypeA(const char *name, DWORD *type) {
  if (!name || !type) {
    SetLastError(87);
    return 0;
  }
  HANDLE h = CreateFileA(name, 0x80000000u, 3, 0, 3, 0, 0);
  if (h == K32_INVALID)
    return 0;
  uint8_t mz[64], header[64];
  DWORD got = 0, more = 0, kind = ~0u;
  BOOL ok = ReadFile(h, mz, sizeof(mz), &got, 0);
  if (ok && got >= 2 && mz[0] == 'M' && mz[1] == 'Z') {
    kind = 1;
    DWORD at = got >= 64 ? k32_u32(mz + 60) : 0;
    if (at >= 64 && SetFilePointer(h, (LONG)at, 0, 0) == at &&
        ReadFile(h, header, sizeof(header), &more, 0) && more >= 4) {
      if (!w32_strcmp((const char *)header, "PE") && more >= 24 && !header[3]) {
        uint16_t machine = k32_u16(header + 4), flags = k32_u16(header + 22);
        kind = machine == 0x14c && !(flags & 0x2000) ? 0 : ~0u;
      } else if (header[0] == 'N' && header[1] == 'E')
        kind = more > 0x36 && header[0x36] == 1 ? 5 : 2;
    }
  } else if (ok) {
    DWORD n = w32_strlen(name);
    if (n >= 4 && !w32_stricmp(name + n - 4, ".com"))
      kind = 1;
    else if (n >= 4 && !w32_stricmp(name + n - 4, ".pif"))
      kind = 3;
  }
  DWORD error = ok ? 193 : GetLastError();
  CloseHandle(h);
  if (kind == ~0u) {
    SetLastError(error);
    return 0;
  }
  *type = kind;
  return 1;
}
/* %NAME% references are replaced by the variable's value; an unknown name
 * (with both percent signs) or an unterminated reference stays literal. The result counts the NUL;
 * a short buffer receives the truncated text and the required size. */
static DWORD expand(const char *src, char *dst, DWORD cap) {
  DWORD total = 0;
#define EMIT(c)                                                                \
  do {                                                                         \
    if (total + 1 < cap)                                                       \
      dst[total] = (c);                                                        \
    total++;                                                                   \
  } while (0)
  for (DWORD i = 0; src[i];) {
    DWORD close = i + 1;
    while (src[i] == '%' && src[close] && src[close] != '%')
      close++;
    if (src[i] != '%' || !src[close] || close == i + 1 || close - i - 1 >= 260) {
      EMIT(src[i]);
      i++;
      continue;
    }
    char name[260];
    w32_copy(name, src + i + 1, close - i - 1);
    name[close - i - 1] = 0;
    SetLastError(0);
    DWORD size = GetEnvironmentVariableA(name, 0, 0);
    if (!size && GetLastError() == 203) {
      for (; i <= close; i++)
        EMIT(src[i]);
      continue;
    }
    char *value = size ? k32_alloc(size) : 0;
    if (size && !value) {
      SetLastError(8);
      return 0;
    }
    if (value && GetEnvironmentVariableA(name, value, size) < size)
      for (DWORD j = 0; value[j]; j++)
        EMIT(value[j]);
    k32_free(value, size);
    i = close + 1;
  }
  if (cap)
    dst[total < cap ? total : cap - 1] = 0;
  return total + 1;
#undef EMIT
}
DWORD W32API ExpandEnvironmentStringsA(const char *src, char *dst, DWORD cap) {
  if (!src || (!dst && cap)) {
    SetLastError(87);
    return 0;
  }
  return expand(src, dst, cap);
}
DWORD W32API ExpandEnvironmentStringsW(const WCHAR *src, WCHAR *dst, DWORD cap) {
  if (!src || (!dst && cap)) {
    SetLastError(87);
    return 0;
  }
  DWORD n;
  char *a = k32_narrow_dup(src, &n, 1113);
  if (!a)
    return 0;
  DWORD need = expand(a, 0, 0), r = 0;
  char *out = need ? k32_alloc(need) : 0;
  if (need && !out)
    SetLastError(8);
  else if (need) {
    r = expand(a, out, need);
    if (cap) {
      DWORD copy = r <= cap ? r : cap;
      k32_widen(out, dst, copy);
      dst[copy - 1] = 0;
    }
  }
  k32_free(out, need);
  k32_free(a, n);
  return r;
}
/* Win16 compatibility file API. HFILE is the Win32 handle value. */
typedef struct {
  uint8_t bytes, fixed;
  uint16_t error, reserved1, reserved2;
  char path[128];
} OFSTRUCT;
static DWORD share(DWORD mode) {
  switch (mode & 0x70) {
  case 0x10: return 0;
  case 0x20: return 1;
  case 0x30: return 2;
  default: return 3;
  }
}
static DWORD access(DWORD mode) {
  return (mode & 3) == 1 ? 0x40000000u : (mode & 3) == 2 ? 0xc0000000u : 0x80000000u;
}
int W32API _lopen(const char *path, int mode) {
  HANDLE h = CreateFileA(path, access((DWORD)mode), share((DWORD)mode), 0, 3, 0, 0);
  return h == K32_INVALID ? -1 : (int)(uintptr_t)h;
}
int W32API _lcreat(const char *path, int attributes) {
  HANDLE h = CreateFileA(path, 0xc0000000u, 3, 0, 2, (DWORD)attributes & 7, 0);
  return h == K32_INVALID ? -1 : (int)(uintptr_t)h;
}
LONG W32API _hread(int f, void *p, LONG n) {
  DWORD done = 0;
  return n >= 0 && ReadFile((HANDLE)(uintptr_t)(DWORD)f, p, (DWORD)n, &done, 0) ? (LONG)done
                                                                               : -1;
}
DWORD W32API _lread(int f, void *p, DWORD n) {
  DWORD done = 0;
  return ReadFile((HANDLE)(uintptr_t)(DWORD)f, p, n, &done, 0) ? done : ~0u;
}
/* A zero-length write sets the end of file at the current position. */
LONG W32API _hwrite(int f, const void *p, LONG n) {
  HANDLE h = (HANDLE)(uintptr_t)(DWORD)f;
  DWORD done = 0;
  if (n < 0) {
    SetLastError(87);
    return -1;
  }
  if (!n)
    return SetEndOfFile(h) ? 0 : -1;
  return WriteFile(h, p, (DWORD)n, &done, 0) ? (LONG)done : -1;
}
DWORD W32API _lwrite(int f, const void *p, DWORD n) {
  return (DWORD)_hwrite(f, p, (LONG)n);
}
LONG W32API _llseek(int f, LONG offset, int origin) {
  return (LONG)SetFilePointer((HANDLE)(uintptr_t)(DWORD)f, offset, 0, (DWORD)origin);
}
int W32API _lclose(int f) { return CloseHandle((HANDLE)(uintptr_t)(DWORD)f) ? 0 : -1; }
/* OF_PARSE 0x100, OF_DELETE 0x200, OF_VERIFY 0x400, OF_CREATE 0x1000,
 * OF_EXIST 0x4000, OF_REOPEN 0x8000. szPathName holds OEM characters. */
int W32API OpenFile(const char *name, OFSTRUCT *of, DWORD style) {
  char ansi[262], full[262], *part;
  if (!of || (!name && !(style & 0x8000))) {
    SetLastError(87);
    return -1;
  }
  if (style & 0x8000) {
    WCHAR w[128];
    if (!MultiByteToWideChar(437, 0, of->path, -1, w, 128) ||
        !WideCharToMultiByte(1252, 0, w, -1, ansi, sizeof(ansi), 0, 0))
      return -1;
  } else if (!w32_copy_string(ansi, sizeof(ansi), name)) {
    SetLastError(206);
    return -1;
  }
  DWORD n = w32_strlen(ansi), error = 0;
  int pathed = 0;
  for (DWORD i = 0; i < n; i++)
    pathed |= separator(ansi[i]) || ansi[i] == ':';
  if (!pathed && !(style & 0x1100) && SearchPathA(0, ansi, 0, sizeof(full), full, &part))
    ;
  else if (!GetFullPathNameA(ansi, sizeof(full), full, &part))
    error = GetLastError();
  WCHAR w[262];
  char oem[262];
  if (!error && (w32_strlen(full) >= sizeof(of->path) ||
                 !MultiByteToWideChar(1252, 0, full, -1, w, 262) ||
                 !WideCharToMultiByte(437, 0, w, -1, oem, sizeof(oem), 0, 0)))
    error = 206;
  w32_fill(of, 0, sizeof(*of));
  of->bytes = sizeof(*of);
  of->fixed = 1;
  if (!error)
    w32_copy(of->path, oem, w32_strlen(oem) + 1);
  int result = -1;
  if (error)
    ;
  else if (style & 0x100)
    result = 0;
  else if (style & 0x200)
    result = DeleteFileA(full) ? 1 : -1;
  else {
    HANDLE h = CreateFileA(full, access(style), share(style), 0, style & 0x1000 ? 2 : 3, 0, 0);
    if (h != K32_INVALID) {
      result = (int)(uintptr_t)h;
      if (style & 0x4000) {
        CloseHandle(h);
        result = 1;
      }
    }
  }
  if (result == -1) {
    of->error = (uint16_t)(error ? error : GetLastError());
    SetLastError(of->error);
  }
  return result;
}
