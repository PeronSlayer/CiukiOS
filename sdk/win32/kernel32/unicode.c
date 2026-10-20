/* f5-20B (directive f5-20 amendment 1, item 1): the Unicode functions
 * Windows 98 refused and the reused Wine DLLs call, layered over the ANSI
 * implementations through the active code page (1252), in the model of
 * the Microsoft Layer for Unicode on Windows 95/98/ME. GetVersion and
 * GetVersionEx still report Windows 98. A name with a character outside the
 * code page fails with ERROR_INVALID_NAME (123), other text with
 * ERROR_NO_UNICODE_TRANSLATION (1113), a path longer than MAX_PATH with
 * ERROR_FILENAME_EXCED_RANGE (206); outputs are widened back (one code
 * unit per byte). The per-application w-stubs shim (contract, compatibility
 * shims) still makes any of them fail as Windows 98 did.
 * GetLocaleInfoW and GetStringTypeW live in locale.c, CompareStringW and
 * LCMapStringW in collate.c, with their ANSI forms.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#define NAME_ERROR 123u
#define TEXT_ERROR 1113u
extern HANDLE W32API CreateMutexA(SECURITY_ATTRIBUTES *, BOOL, const char *);
extern HANDLE W32API CreateEventA(SECURITY_ATTRIBUTES *, BOOL, BOOL,
                                  const char *);
extern HANDLE W32API LoadLibraryExA(const char *, HANDLE, DWORD);
extern HANDLE W32API GetModuleHandleA(const char *);
extern DWORD W32API GetModuleFileNameA(HANDLE, char *, DWORD);
extern DWORD W32API GetEnvironmentVariableA(const char *, char *, DWORD);
extern char *W32API GetCommandLineA(void);
extern BOOL W32API GetVersionExA(OSVERSIONINFOA *);
/* Optional name: NULL stays NULL. */
#define NAME_OR_NULL(w, a)                                                     \
  char a[262];                                                                 \
  if ((w) && !k32_narrow(w, a, sizeof(a), NAME_ERROR))
HANDLE W32API CreateFileW(const WCHAR *p, DWORD access, DWORD share,
                          SECURITY_ATTRIBUTES *sa, DWORD creation, DWORD flags,
                          HANDLE template_file) {
  char a[262];
  if (!k32_narrow(p, a, sizeof(a), NAME_ERROR))
    return K32_INVALID;
  return CreateFileA(a, access, share, sa, creation, flags, template_file);
}
HANDLE W32API LoadLibraryExW(const WCHAR *name, HANDLE file, DWORD flags) {
  char a[262];
  if (!k32_narrow(name, a, sizeof(a), NAME_ERROR))
    return 0;
  return LoadLibraryExA(a, file, flags);
}
HANDLE W32API LoadLibraryW(const WCHAR *name) {
  return LoadLibraryExW(name, 0, 0);
}
HANDLE W32API GetModuleHandleW(const WCHAR *name) {
  NAME_OR_NULL(name, a) return 0;
  return GetModuleHandleA(name ? a : 0);
}
/* Same count and truncation rules as the ANSI form: the copied length, and
 * a terminator only when the name fits. */
DWORD W32API GetModuleFileNameW(HANDLE h, WCHAR *out, DWORD cap) {
  char a[262];
  if (!out || !cap) {
    SetLastError(87);
    return 0;
  }
  DWORD n = GetModuleFileNameA(h, a, cap < sizeof(a) ? cap : sizeof(a));
  if (!n)
    return 0;
  k32_widen(a, out, n);
  if (n < cap)
    out[n] = 0;
  return n;
}
HANDLE W32API CreateEventW(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial,
                           const WCHAR *name) {
  NAME_OR_NULL(name, a) return 0;
  return CreateEventA(sa, manual, initial, name ? a : 0);
}
HANDLE W32API CreateMutexW(SECURITY_ATTRIBUTES *sa, BOOL owner,
                           const WCHAR *name) {
  NAME_OR_NULL(name, a) return 0;
  return CreateMutexA(sa, owner, name ? a : 0);
}
/* OSVERSIONINFOW (276 bytes) or OSVERSIONINFOEXW (284): the ANSI record
 * with the service-pack text widened; the platform stays Windows 98. */
BOOL W32API GetVersionExW(OSVERSIONINFOW *out) {
  if (!out ||
      (out->size != sizeof(*out) && out->size != sizeof(OSVERSIONINFOEXW))) {
    SetLastError(87);
    return 0;
  }
  OSVERSIONINFOEXA a;
  w32_fill(&a, 0, sizeof(a));
  BOOL ex = out->size == sizeof(OSVERSIONINFOEXW);
  a.base.size = ex ? sizeof(OSVERSIONINFOEXA) : sizeof(OSVERSIONINFOA);
  if (!GetVersionExA(&a.base))
    return 0;
  out->major = a.base.major;
  out->minor = a.base.minor;
  out->build = a.base.build;
  out->platform = a.base.platform;
  DWORD n = w32_strlen(a.base.csd);
  if (n > 127)
    n = 127;
  k32_widen(a.base.csd, out->csd, n);
  w32_fill(out->csd + n, 0, (128 - n) * 2);
  if (ex) {
    OSVERSIONINFOEXW *x = (OSVERSIONINFOEXW *)out;
    x->service_major = a.service_major;
    x->service_minor = a.service_minor;
    x->suite = a.suite;
    x->product = a.product;
    x->reserved = a.reserved;
  }
  return 1;
}
/* Required size (with the terminator) when the buffer is too small, the
 * length otherwise; 0 with ERROR_ENVVAR_NOT_FOUND from the ANSI call. */
DWORD W32API GetEnvironmentVariableW(const WCHAR *name, WCHAR *out,
                                     DWORD cap) {
  char key[262];
  if (!k32_narrow(name, key, sizeof(key), NAME_ERROR))
    return 0;
  char *value = k32_alloc(32768);
  if (!value) {
    SetLastError(8);
    return 0;
  }
  DWORD r = GetEnvironmentVariableA(key, value, 32768);
  if (!r || r >= 32768) {
    if (!r && out && cap)
      out[0] = 0;
    k32_free(value, 32768);
    return r;
  }
  if (cap <= r) {
    k32_free(value, 32768);
    return r + 1;
  }
  if (!out) {
    k32_free(value, 32768);
    SetLastError(87);
    return 0;
  }
  k32_widen(value, out, r + 1);
  k32_free(value, 32768);
  return r;
}
/* One process-lifetime copy of the ANSI command line, widened. */
WCHAR *W32API GetCommandLineW(void) {
  static WCHAR *volatile command;
  WCHAR *c = command;
  if (c)
    return c;
  const char *a = GetCommandLineA();
  if (!a)
    return 0;
  DWORD n = w32_strlen(a) + 1;
  c = k32_alloc(n * 2);
  if (!c) {
    SetLastError(8);
    return 0;
  }
  k32_widen(a, c, n);
  WCHAR *expected = 0;
  if (!__atomic_compare_exchange_n(&command, &expected, c, 0, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE)) {
    k32_free(c, n * 2);
    return expected;
  }
  return c;
}

/* ---- CreateProcessW ------------------------------------------------------ */
struct startup_w {
  DWORD size;
  WCHAR *reserved, *desktop, *title;
  DWORD x, y, width, height, chars_x, chars_y, fill, flags;
  uint16_t show, reserved_bytes;
  uint8_t *reserved2;
  HANDLE input, output, error;
};
struct startup_a {
  DWORD size;
  char *reserved, *desktop, *title;
  DWORD x, y, width, height, chars_x, chars_y, fill, flags;
  uint16_t show, reserved_bytes;
  uint8_t *reserved2;
  HANDLE input, output, error;
};
extern BOOL W32API CreateProcessA(const char *, char *, SECURITY_ATTRIBUTES *,
                                  SECURITY_ATTRIBUTES *, BOOL, DWORD, void *,
                                  const char *, struct startup_a *, void *);
#define CREATE_UNICODE_ENVIRONMENT 0x400u
/* A Unicode environment block (strings, then an empty string) in ANSI. */
static char *environment_ansi(const WCHAR *w, DWORD *bytes) {
  DWORD n = 0;
  while (w[n] || w[n + 1])
    n++;
  n += 2;
  char *a = k32_alloc(n);
  if (!a) {
    SetLastError(8);
    return 0;
  }
  for (DWORD i = 0; i < n; i++) {
    BOOL lossy = 0;
    if (!WideCharToMultiByte(1252, 0, w + i, 1, a + i, 1, 0, &lossy) ||
        lossy) {
      k32_free(a, n);
      SetLastError(TEXT_ERROR);
      return 0;
    }
  }
  *bytes = n;
  return a;
}
BOOL W32API CreateProcessW(const WCHAR *application, WCHAR *command,
                           SECURITY_ATTRIBUTES *ps, SECURITY_ATTRIBUTES *ts,
                           BOOL inherit, DWORD flags, void *environment,
                           const WCHAR *directory, struct startup_w *startup,
                           void *out) {
  if (!startup || startup->size != sizeof(*startup)) {
    SetLastError(87);
    return 0;
  }
  char app[262], dir[262];
  if ((application && !k32_narrow(application, app, sizeof(app), NAME_ERROR)) ||
      (directory && !k32_narrow(directory, dir, sizeof(dir), NAME_ERROR)))
    return 0;
  DWORD command_bytes = 0, desktop_bytes = 0, title_bytes = 0, env_bytes = 0;
  char *cmd = 0, *desktop = 0, *title = 0, *env = 0;
  BOOL ok = 0;
  if (command && !(cmd = k32_narrow_dup(command, &command_bytes, TEXT_ERROR)))
    goto done;
  if (startup->desktop &&
      !(desktop = k32_narrow_dup(startup->desktop, &desktop_bytes, TEXT_ERROR)))
    goto done;
  if (startup->title &&
      !(title = k32_narrow_dup(startup->title, &title_bytes, TEXT_ERROR)))
    goto done;
  if (environment && (flags & CREATE_UNICODE_ENVIRONMENT) &&
      !(env = environment_ansi(environment, &env_bytes)))
    goto done;
  struct startup_a a = {sizeof(a),       0,
                        desktop,         title,
                        startup->x,      startup->y,
                        startup->width,  startup->height,
                        startup->chars_x, startup->chars_y,
                        startup->fill,   startup->flags,
                        startup->show,   startup->reserved_bytes,
                        startup->reserved2, startup->input,
                        startup->output, startup->error};
  ok = CreateProcessA(application ? app : 0, cmd, ps, ts, inherit,
                      flags & ~CREATE_UNICODE_ENVIRONMENT,
                      env ? env : environment, directory ? dir : 0, &a, out);
done:;
  DWORD e = GetLastError();
  k32_free(cmd, command_bytes);
  k32_free(desktop, desktop_bytes);
  k32_free(title, title_bytes);
  k32_free(env, env_bytes);
  if (!ok)
    SetLastError(e);
  return ok;
}
