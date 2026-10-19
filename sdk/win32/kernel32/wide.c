/* Unicode entry points the reused Wine 10.0 DLLs need functional (f5-20A).
 * Each one converts to the ANSI code page (1252) and calls the Windows 98
 * ANSI implementation, so both paths share one behaviour. A name with a
 * character outside the code page fails with ERROR_INVALID_NAME (123);
 * other text with ERROR_NO_UNICODE_TRANSLATION (1113). Results are widened
 * back; CP1252 is single byte, so character counts carry over unchanged.
 * The per-application w-stubs shim still gates every export ending in W.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#define NAME_ERROR 123u
extern BOOL W32API CopyFileA(const char *, const char *, BOOL);
extern DWORD W32API GetLogicalDriveStringsA(DWORD, char *);
extern DWORD W32API GetProfileStringA(const char *, const char *, const char *, char *, DWORD);
#define TEXT_ERROR 1113u
DWORD k32_wlen(const WCHAR *s) {
  DWORD n = 0;
  if (s)
    while (s[n])
      n++;
  return n;
}
int k32_narrow(const WCHAR *w, char *out, DWORD cap, DWORD error) {
  if (!w) {
    SetLastError(87);
    return 0;
  }
  DWORD n = k32_wlen(w);
  if (n >= cap) {
    SetLastError(206);
    return 0;
  }
  BOOL lossy = 0;
  if (!WideCharToMultiByte(1252, 0, w, (int)n + 1, out, (int)cap, 0, &lossy))
    return 0;
  if (lossy) {
    SetLastError(error);
    return 0;
  }
  return 1;
}
char *k32_narrow_dup(const WCHAR *w, DWORD *bytes, DWORD error) {
  DWORD n = k32_wlen(w) + 1;
  char *p = k32_alloc(n);
  if (!p) {
    SetLastError(8);
    return 0;
  }
  if (!k32_narrow(w, p, n, error)) {
    DWORD e = GetLastError();
    k32_free(p, n);
    SetLastError(e);
    return 0;
  }
  *bytes = n;
  return p;
}
void k32_widen(const char *a, WCHAR *out, DWORD n) {
  if (n)
    MultiByteToWideChar(1252, 0, a, (int)n, out, (int)n);
}
/* Optional argument: NULL stays NULL; a string becomes an owned ANSI copy. */
struct text {
  char *p;
  DWORD n;
};
static int text(const WCHAR *w, struct text *t, DWORD error) {
  t->p = 0;
  t->n = 0;
  return !w || (t->p = k32_narrow_dup(w, &t->n, error)) != 0;
}
static void release(struct text *t) {
  k32_free(t->p, t->n);
  t->p = 0;
}
/* "Length or required size" result of an ANSI call made with a local
 * buffer that was large enough: copy when the caller's buffer fits. */
static DWORD sized(DWORD r, const char *a, WCHAR *out, DWORD cap) {
  if (!r)
    return 0;
  if (cap <= r)
    return r + 1;
  if (!out) {
    SetLastError(87);
    return 0;
  }
  k32_widen(a, out, r + 1);
  return r;
}
#define PATH(w, a)                                                             \
  char a[262];                                                                 \
  if (!k32_narrow(w, a, sizeof(a), NAME_ERROR))
BOOL W32API CopyFileW(const WCHAR *from, const WCHAR *to, BOOL fail) {
  PATH(from, a) return 0;
  PATH(to, b) return 0;
  return CopyFileA(a, b, fail);
}
BOOL W32API CreateDirectoryW(const WCHAR *p, SECURITY_ATTRIBUTES *sa) {
  PATH(p, a) return 0;
  return CreateDirectoryA(a, sa);
}
BOOL W32API DeleteFileW(const WCHAR *p) {
  PATH(p, a) return 0;
  return DeleteFileA(a);
}
BOOL W32API RemoveDirectoryW(const WCHAR *p) {
  extern BOOL W32API RemoveDirectoryA(const char *);
  PATH(p, a) return 0;
  return RemoveDirectoryA(a);
}
BOOL W32API SetCurrentDirectoryW(const WCHAR *p) {
  extern BOOL W32API SetCurrentDirectoryA(const char *);
  PATH(p, a) return 0;
  return SetCurrentDirectoryA(a);
}
BOOL W32API SetFileAttributesW(const WCHAR *p, DWORD attributes) {
  PATH(p, a) return 0;
  return SetFileAttributesA(a, attributes);
}
DWORD W32API GetFileAttributesW(const WCHAR *p) {
  PATH(p, a) return ~0u;
  return GetFileAttributesA(a);
}
BOOL W32API GetFileAttributesExW(const WCHAR *p, DWORD level, void *out) {
  PATH(p, a) return 0;
  return GetFileAttributesExA(a, level, out);
}
BOOL W32API MoveFileExW(const WCHAR *from, const WCHAR *to, DWORD flags) {
  extern BOOL W32API MoveFileExA(const char *, const char *, DWORD);
  PATH(from, a) return 0;
  char b[262];
  if (to && !k32_narrow(to, b, sizeof(b), NAME_ERROR))
    return 0;
  return MoveFileExA(a, to ? b : 0, flags);
}
BOOL W32API MoveFileW(const WCHAR *from, const WCHAR *to) {
  extern BOOL W32API MoveFileA(const char *, const char *);
  PATH(from, a) return 0;
  PATH(to, b) return 0;
  return MoveFileA(a, b);
}
static void find_data(const WIN32_FIND_DATAA *a, WIN32_FIND_DATAW *w) {
  WIN32_FIND_DATAW d = {.attributes = a->attributes,
                        .creation = a->creation,
                        .access = a->access,
                        .write = a->write,
                        .size_high = a->size_high,
                        .size_low = a->size_low,
                        .reserved = {a->reserved[0], a->reserved[1]}};
  k32_widen(a->name, d.name, w32_strlen(a->name) + 1);
  k32_widen(a->alternate, d.alternate, w32_strlen(a->alternate) + 1);
  *w = d;
}
HANDLE W32API FindFirstFileW(const WCHAR *pattern, WIN32_FIND_DATAW *out) {
  if (!out) {
    SetLastError(87);
    return K32_INVALID;
  }
  PATH(pattern, a) return K32_INVALID;
  WIN32_FIND_DATAA d;
  HANDLE h = FindFirstFileA(a, &d);
  if (h != K32_INVALID)
    find_data(&d, out);
  return h;
}
BOOL W32API FindNextFileW(HANDLE h, WIN32_FIND_DATAW *out) {
  if (!out) {
    SetLastError(87);
    return 0;
  }
  WIN32_FIND_DATAA d;
  if (!FindNextFileA(h, &d))
    return 0;
  find_data(&d, out);
  return 1;
}
DWORD W32API GetCurrentDirectoryW(DWORD cap, WCHAR *out) {
  char a[780];
  return sized(GetCurrentDirectoryA(sizeof(a), a), a, out, cap);
}
DWORD W32API GetFullPathNameW(const WCHAR *p, DWORD cap, WCHAR *out, WCHAR **file) {
  PATH(p, in) return 0;
  char a[780], *part = 0;
  DWORD r = sized(GetFullPathNameA(in, sizeof(a), a, &part), a, out, cap);
  if (r && r < cap && file)
    *file = part && *part ? out + (part - a) : 0;
  return r;
}
DWORD W32API GetLongPathNameW(const WCHAR *p, WCHAR *out, DWORD cap) {
  extern DWORD W32API GetLongPathNameA(const char *, char *, DWORD);
  PATH(p, in) return 0;
  char a[780];
  return sized(GetLongPathNameA(in, a, sizeof(a)), a, out, cap);
}
DWORD W32API GetShortPathNameW(const WCHAR *p, WCHAR *out, DWORD cap) {
  extern DWORD W32API GetShortPathNameA(const char *, char *, DWORD);
  PATH(p, in) return 0;
  char a[780];
  return sized(GetShortPathNameA(in, a, sizeof(a)), a, out, cap);
}
DWORD W32API GetSystemDirectoryW(WCHAR *out, DWORD cap) {
  extern DWORD W32API GetSystemDirectoryA(char *, DWORD);
  char a[262];
  return sized(GetSystemDirectoryA(a, sizeof(a)), a, out, cap);
}
DWORD W32API GetWindowsDirectoryW(WCHAR *out, DWORD cap) {
  extern DWORD W32API GetWindowsDirectoryA(char *, DWORD);
  char a[262];
  return sized(GetWindowsDirectoryA(a, sizeof(a)), a, out, cap);
}
/* Single-user Windows 98: the shared Windows directory is the directory. */
DWORD W32API GetSystemWindowsDirectoryA(char *out, DWORD cap) {
  extern DWORD W32API GetWindowsDirectoryA(char *, DWORD);
  return GetWindowsDirectoryA(out, cap);
}
DWORD W32API GetSystemWindowsDirectoryW(WCHAR *out, DWORD cap) {
  extern DWORD W32API GetWindowsDirectoryA(char *, DWORD);
  char a[262];
  return sized(GetWindowsDirectoryA(a, sizeof(a)), a, out, cap);
}
DWORD W32API GetTempPathW(DWORD cap, WCHAR *out) {
  char a[262];
  return sized(GetTempPathA(sizeof(a), a), a, out, cap);
}
DWORD W32API GetTempFileNameW(const WCHAR *path, const WCHAR *prefix, DWORD unique,
                              WCHAR *out) {
  if (!out) {
    SetLastError(87);
    return 0;
  }
  PATH(path, p) return 0;
  char pre[4] = {0}, result[262];
  if (prefix) {
    WCHAR three[4] = {0};
    for (unsigned i = 0; i < 3 && prefix[i]; i++)
      three[i] = prefix[i];
    if (!k32_narrow(three, pre, sizeof(pre), NAME_ERROR))
      return 0;
  }
  DWORD r = GetTempFileNameA(p, prefix ? pre : 0, unique, result);
  if (r)
    k32_widen(result, out, w32_strlen(result) + 1);
  return r;
}
DWORD W32API GetLogicalDriveStringsW(DWORD cap, WCHAR *out) {
  char a[128];
  DWORD r = GetLogicalDriveStringsA(sizeof(a), a);
  if (!r)
    return 0;
  if (cap <= r)
    return r + 1;
  if (!out) {
    SetLastError(87);
    return 0;
  }
  k32_widen(a, out, r + 1);
  return r;
}
#define ROOT(w, a)                                                             \
  char a##_buffer[262], *a = 0;                                                \
  if (w && !k32_narrow(w, a = a##_buffer, sizeof(a##_buffer), NAME_ERROR))
BOOL W32API GetDiskFreeSpaceW(const WCHAR *root, DWORD *spc, DWORD *bps, DWORD *free_clusters,
                              DWORD *total_clusters) {
  extern BOOL W32API GetDiskFreeSpaceA(const char *, DWORD *, DWORD *, DWORD *, DWORD *);
  ROOT(root, a) return 0;
  return GetDiskFreeSpaceA(a, spc, bps, free_clusters, total_clusters);
}
BOOL W32API GetDiskFreeSpaceExW(const WCHAR *root, uint64_t *available, uint64_t *total,
                                uint64_t *free) {
  extern BOOL W32API GetDiskFreeSpaceExA(const char *, uint64_t *, uint64_t *, uint64_t *);
  ROOT(root, a) return 0;
  return GetDiskFreeSpaceExA(a, available, total, free);
}
DWORD W32API GetDriveTypeW(const WCHAR *root) {
  extern DWORD W32API GetDriveTypeA(const char *);
  ROOT(root, a) return 1;
  return GetDriveTypeA(a);
}
BOOL W32API GetVolumeInformationW(const WCHAR *root, WCHAR *label, DWORD label_size,
                                  DWORD *serial, DWORD *component, DWORD *flags, WCHAR *fs,
                                  DWORD fs_size) {
  ROOT(root, a) return 0;
  char l[80], f[40];
  DWORD s, c, v;
  if (!GetVolumeInformationA(a, l, sizeof(l), &s, &c, &v, f, sizeof(f)))
    return 0;
  if ((label && w32_strlen(l) >= label_size) || (fs && w32_strlen(f) >= fs_size)) {
    SetLastError(122);
    return 0;
  }
  if (label)
    k32_widen(l, label, w32_strlen(l) + 1);
  if (fs)
    k32_widen(f, fs, w32_strlen(f) + 1);
  if (serial)
    *serial = s;
  if (component)
    *component = c;
  if (flags)
    *flags = v;
  return 1;
}
DWORD W32API SearchPathW(const WCHAR *path, const WCHAR *file, const WCHAR *ext, DWORD cap,
                         WCHAR *out, WCHAR **part) {
  struct text p, e;
  if (!file) {
    SetLastError(87);
    return 0;
  }
  PATH(file, f) return 0;
  if (!text(path, &p, NAME_ERROR))
    return 0;
  if (!text(ext, &e, NAME_ERROR)) {
    release(&p);
    return 0;
  }
  char a[780], *name = 0;
  DWORD r = sized(SearchPathA(p.p, f, e.p, sizeof(a), a, &name), a, out, cap);
  release(&p);
  release(&e);
  if (r && r < cap && part)
    *part = name ? out + (name - a) : 0;
  return r;
}
BOOL W32API GetBinaryTypeW(const WCHAR *name, DWORD *type) {
  extern BOOL W32API GetBinaryTypeA(const char *, DWORD *);
  PATH(name, a) return 0;
  return GetBinaryTypeA(a, type);
}
/* Profile (INI) strings: keys, values and file names in ACP 1252. */
static DWORD profile_string(const WCHAR *s, const WCHAR *k, const WCHAR *d, WCHAR *out, DWORD n,
                            const WCHAR *f, int win_ini) {
  struct text ts, tk, td, tf;
  if (!out || !n)
    return 0;
  DWORD r = 0;
  char *buffer = 0;
  if (text(s, &ts, TEXT_ERROR)) {
    if (text(k, &tk, TEXT_ERROR)) {
      if (text(d, &td, TEXT_ERROR)) {
        if (text(f, &tf, NAME_ERROR)) {
          buffer = k32_alloc(n);
          if (!buffer)
            SetLastError(8);
          else {
            w32_fill(buffer, 0, n);
            r = win_ini ? GetProfileStringA(ts.p, tk.p, td.p, buffer, n)
                        : GetPrivateProfileStringA(ts.p, tk.p, td.p, buffer, n, tf.p);
            k32_widen(buffer, out, r + 2 < n ? r + 2 : n);
            k32_free(buffer, n);
          }
          release(&tf);
        }
        release(&td);
      }
      release(&tk);
    }
    release(&ts);
  }
  return r;
}
DWORD W32API GetPrivateProfileStringW(const WCHAR *s, const WCHAR *k, const WCHAR *d,
                                      WCHAR *out, DWORD n, const WCHAR *f) {
  return profile_string(s, k, d, out, n, f, 0);
}
DWORD W32API GetProfileStringW(const WCHAR *s, const WCHAR *k, const WCHAR *d, WCHAR *out,
                               DWORD n) {
  return profile_string(s, k, d, out, n, 0, 1);
}
BOOL W32API WritePrivateProfileStringW(const WCHAR *s, const WCHAR *k, const WCHAR *v,
                                       const WCHAR *f) {
  struct text ts, tk, tv, tf;
  BOOL ok = 0;
  if (text(s, &ts, TEXT_ERROR)) {
    if (text(k, &tk, TEXT_ERROR)) {
      if (text(v, &tv, TEXT_ERROR)) {
        if (text(f, &tf, NAME_ERROR)) {
          ok = WritePrivateProfileStringA(ts.p, tk.p, tv.p, tf.p);
          release(&tf);
        }
        release(&tv);
      }
      release(&tk);
    }
    release(&ts);
  }
  return ok;
}
DWORD W32API GetPrivateProfileIntW(const WCHAR *s, const WCHAR *k, int def, const WCHAR *f) {
  struct text ts, tk, tf;
  DWORD r = (DWORD)def;
  if (text(s, &ts, TEXT_ERROR)) {
    if (text(k, &tk, TEXT_ERROR)) {
      if (text(f, &tf, NAME_ERROR)) {
        r = GetPrivateProfileIntA(ts.p, tk.p, def, tf.p);
        release(&tf);
      }
      release(&tk);
    }
    release(&ts);
  }
  return r;
}
static BOOL profile_struct(const WCHAR *s, const WCHAR *k, void *data, DWORD bytes,
                           const WCHAR *f, int write) {
  extern BOOL W32API GetPrivateProfileStructA(const char *, const char *, void *, DWORD,
                                              const char *);
  extern BOOL W32API WritePrivateProfileStructA(const char *, const char *, const void *,
                                                DWORD, const char *);
  struct text ts, tk, tf;
  BOOL ok = 0;
  if (text(s, &ts, TEXT_ERROR)) {
    if (text(k, &tk, TEXT_ERROR)) {
      if (text(f, &tf, NAME_ERROR)) {
        ok = write ? WritePrivateProfileStructA(ts.p, tk.p, data, bytes, tf.p)
                   : GetPrivateProfileStructA(ts.p, tk.p, data, bytes, tf.p);
        release(&tf);
      }
      release(&tk);
    }
    release(&ts);
  }
  return ok;
}
BOOL W32API GetPrivateProfileStructW(const WCHAR *s, const WCHAR *k, void *out, DWORD bytes,
                                     const WCHAR *f) {
  return profile_struct(s, k, out, bytes, f, 0);
}
BOOL W32API WritePrivateProfileStructW(const WCHAR *s, const WCHAR *k, const void *data,
                                       DWORD bytes, const WCHAR *f) {
  return profile_struct(s, k, (void *)data, bytes, f, 1);
}
/* Debug text keeps going even with characters outside the code page. */
void W32API OutputDebugStringW(const WCHAR *s) {
  static const WCHAR empty[1];
  DWORD saved = GetLastError(), n = k32_wlen(s) + 1;
  char *a = k32_alloc(n);
  if (a && WideCharToMultiByte(1252, 0, s ? s : empty, (int)n, a, (int)n, 0, 0))
    OutputDebugStringA(a);
  k32_free(a, n);
  SetLastError(saved);
}
/* Ordinal comparison of UTF-16 code units; NULL sorts first. lstrcmpiW
 * folds with the NT upcase table (Latin-1 and CP1252 letters). */
static int compare(const WCHAR *a, const WCHAR *b, int fold) {
  if (!a || !b)
    return !a && !b ? 0 : a ? 1 : -1;
  for (;; a++, b++) {
    WCHAR x = *a, y = *b;
    if (fold) {
      x = RtlUpcaseUnicodeChar(x);
      y = RtlUpcaseUnicodeChar(y);
    }
    if (x != y)
      return x < y ? -1 : 1;
    if (!x)
      return 0;
  }
}
int W32API lstrcmpW(const WCHAR *a, const WCHAR *b) { return compare(a, b, 0); }
int W32API lstrcmpiW(const WCHAR *a, const WCHAR *b) { return compare(a, b, 1); }
/* Readable committed pages (VirtualQuery), never touching a page that is
 * not; NULL with a non-zero bound is bad, a zero bound never is. */
static BOOL bad_string(const void *p, DWORD max, DWORD unit) {
  if (!max)
    return 0;
  if (!p)
    return 1;
  uintptr_t at = (uintptr_t)p, limit = 0;
  for (DWORD i = 0; i < max; i++, at += unit) {
    if (at + unit - 1 < at)
      return 1;
    while (at + unit > limit) {
      struct ciuki_w32_memory_info m;
      uintptr_t probe = limit > at ? limit : at;
      if (VirtualQuery((const void *)probe, &m, sizeof(m)) != sizeof(m) || m.state != 0x1000 ||
          (m.protect & 0x101) || !(m.protect & 0xee) || !m.region_size)
        return 1;
      limit = (uintptr_t)m.base + m.region_size;
    }
    if (unit == 1 ? !*(const volatile char *)at : !*(const volatile WCHAR *)at)
      return 0;
  }
  return 0;
}
BOOL W32API IsBadStringPtrA(const char *p, DWORD max) { return bad_string(p, max, 1); }
BOOL W32API IsBadStringPtrW(const WCHAR *p, DWORD max) { return bad_string(p, max, 2); }
/* Named kernel objects: the names round-trip through ACP 1252, the form in
 * which the ANSI entry points already hand them to the kernel. */
#define OBJECT_NAME(w, a)                                                      \
  char a##_buffer[262], *a = 0;                                                \
  if (w && !k32_narrow(w, a = a##_buffer, sizeof(a##_buffer), NAME_ERROR))
HANDLE W32API CreateSemaphoreW(SECURITY_ATTRIBUTES *sa, LONG initial, LONG max,
                               const WCHAR *name) {
  extern HANDLE W32API CreateSemaphoreA(SECURITY_ATTRIBUTES *, LONG, LONG, const char *);
  OBJECT_NAME(name, a) return 0;
  return CreateSemaphoreA(sa, initial, max, a);
}
HANDLE W32API OpenSemaphoreW(DWORD access, BOOL inherit, const WCHAR *name) {
  extern HANDLE W32API OpenSemaphoreA(DWORD, BOOL, const char *);
  if (!name) {
    SetLastError(87);
    return 0;
  }
  OBJECT_NAME(name, a) return 0;
  return OpenSemaphoreA(access, inherit, a);
}
HANDLE W32API OpenEventW(DWORD access, BOOL inherit, const WCHAR *name) {
  extern HANDLE W32API OpenEventA(DWORD, BOOL, const char *);
  if (!name) {
    SetLastError(87);
    return 0;
  }
  OBJECT_NAME(name, a) return 0;
  return OpenEventA(access, inherit, a);
}
HANDLE W32API CreateFileMappingW(HANDLE file, SECURITY_ATTRIBUTES *sa, DWORD protect,
                                 DWORD high, DWORD low, const WCHAR *name) {
  extern HANDLE W32API CreateFileMappingA(HANDLE, SECURITY_ATTRIBUTES *, DWORD, DWORD, DWORD,
                                          const char *);
  OBJECT_NAME(name, a) return 0;
  return CreateFileMappingA(file, sa, protect, high, low, a);
}
