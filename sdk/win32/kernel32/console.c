/* Minimum console: ordinary per-process log handles, no interactive screen.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
static HANDLE log_handle;
static struct w32_process_parameters *parameters(void) {
  return NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters;
}
HANDLE W32API GetStdHandle(DWORD which) {
  struct w32_process_parameters *p = parameters();
  if (which == (DWORD)-10)
    return p->StandardInput;
  if (which == (DWORD)-11)
    return p->StandardOutput;
  if (which == (DWORD)-12)
    return p->StandardError;
  SetLastError(87);
  return K32_INVALID;
}
BOOL W32API SetStdHandle(DWORD which, HANDLE h) {
  struct w32_process_parameters *p = parameters();
  if (which == (DWORD)-10)
    p->StandardInput = h;
  else if (which == (DWORD)-11)
    p->StandardOutput = h;
  else if (which == (DWORD)-12)
    p->StandardError = h;
  else {
    SetLastError(87);
    return 0;
  }
  return 1;
}
BOOL W32API AllocConsole(void) {
  if (log_handle) {
    SetLastError(5);
    return 0;
  }
  char path[260] = "C:\\WINDOWS\\TEMP\\", file[260];
  extern BOOL W32API CreateDirectoryA(const char *, SECURITY_ATTRIBUTES *);
  if (GetFileAttributesA(path) == ~0u && !CreateDirectoryA(path, 0))
    return 0;
  extern DWORD W32API GetTempFileNameA(const char *, const char *, DWORD,
                                       char *);
  if (!GetTempFileNameA(path, "W32", 0, file))
    return 0;
  HANDLE h = CreateFileA(file, 0xc0000000u, 3, 0, 3, 128, 0);
  if (h == K32_INVALID)
    return 0;
  log_handle = h;
  parameters()->ConsoleHandle = h;
  parameters()->StandardInput = parameters()->StandardOutput =
      parameters()->StandardError = h;
  return 1;
}
BOOL W32API FreeConsole(void) {
  if (!log_handle) {
    SetLastError(87);
    return 0;
  }
  HANDLE h = log_handle;
  log_handle = 0;
  struct w32_process_parameters *p = parameters();
  p->ConsoleHandle = 0;
  if (p->StandardInput == h)
    p->StandardInput = 0;
  if (p->StandardOutput == h)
    p->StandardOutput = 0;
  if (p->StandardError == h)
    p->StandardError = 0;
  return CloseHandle(h);
}
BOOL W32API WriteConsoleA(HANDLE h, const void *data, DWORD n, DWORD *done,
                          void *reserved) {
  if (reserved) {
    SetLastError(87);
    return 0;
  }
  return WriteFile(h, data, n, done, 0);
}
BOOL W32API WriteConsoleW(HANDLE h, const WCHAR *data, DWORD n, DWORD *done,
                          void *reserved) {
  if (reserved || (!data && n)) {
    SetLastError(87);
    return 0;
  }
  if (done)
    *done = 0;
  if (!n)
    return 1;
  char *bytes = k32_alloc(n);
  if (!bytes) {
    SetLastError(8);
    return 0;
  }
  int count = WideCharToMultiByte(437, 0, data, (int)n, bytes, (int)n, 0, 0);
  DWORD written = 0;
  BOOL ok = count && WriteFile(h, bytes, (DWORD)count, &written, 0);
  if (done)
    *done = written;
  k32_free(bytes, n);
  return ok;
}
/* f5-20A: console queries over the log console. The console handle is the
 * one AllocConsole created; it has a mode word but no interactive input, so
 * reads find nothing pending (Peek: none; ReadConsole: zero bytes; a
 * blocking ReadConsoleInput: end of input). Other handles are not console
 * handles (ERROR_INVALID_HANDLE), as on Windows. */
static DWORD console_mode = 7;
static int console(HANDLE h) {
  if (h && h == log_handle)
    return 1;
  SetLastError(6);
  return 0;
}
BOOL W32API GetConsoleMode(HANDLE h, DWORD *mode) {
  if (!mode) {
    SetLastError(87);
    return 0;
  }
  if (!console(h))
    return 0;
  *mode = console_mode;
  return 1;
}
BOOL W32API SetConsoleMode(HANDLE h, DWORD mode) {
  if (!console(h))
    return 0;
  if (mode & ~0x1ffu) {
    SetLastError(87);
    return 0;
  }
  console_mode = mode;
  return 1;
}
BOOL W32API VerifyConsoleIoHandle(HANDLE h) { return h && h == log_handle; }
BOOL W32API PeekConsoleInputA(HANDLE h, void *records, DWORD n, DWORD *done) {
  (void)records;
  (void)n;
  if (!done) {
    SetLastError(87);
    return 0;
  }
  if (!console(h))
    return 0;
  *done = 0;
  return 1;
}
BOOL W32API ReadConsoleA(HANDLE h, void *buffer, DWORD n, DWORD *done, void *control) {
  (void)control;
  if (!done || (!buffer && n)) {
    SetLastError(87);
    return 0;
  }
  if (!console(h))
    return 0;
  *done = 0;
  return 1;
}
BOOL W32API ReadConsoleInputW(HANDLE h, void *records, DWORD n, DWORD *done) {
  if (!done || !records || !n) {
    SetLastError(87);
    return 0;
  }
  *done = 0;
  if (!console(h))
    return 0;
  SetLastError(38);
  return 0;
}
