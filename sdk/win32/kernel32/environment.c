/* Process parameters are the source of environment and command-line state.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
static WCHAR *owned_env;
static DWORD owned_bytes;
static struct w32_process_parameters *params(void) {
  return NtCurrentTeb()->ProcessEnvironmentBlock->ProcessParameters;
}
static DWORD ulen(const WCHAR *s) {
  DWORD n = 0;
  while (s && s[n])
    n++;
  return n;
}
static WCHAR upper(WCHAR c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int matches(const WCHAR *entry, const WCHAR *name, DWORD n) {
  for (DWORD i = 0; i < n; i++)
    if (!entry[i] || upper(entry[i]) != upper(name[i]))
      return 0;
  return entry[n] == '=';
}
static DWORD env_size(const WCHAR *s) {
  DWORD n = 0;
  if (s)
    while (s[n]) {
      while (s[n])
        n++;
      n++;
    }
  return n + 1 + (n == 0);
}
DWORD W32API GetEnvironmentVariableW(const WCHAR *name, WCHAR *out, DWORD cap) {
  if (!name || !*name) {
    SetLastError(87);
    return 0;
  }
  DWORD n = ulen(name);
  for (DWORD i = 0; i < n; i++)
    if (name[i] == '=') {
      SetLastError(87);
      return 0;
    }
  RTL_CRITICAL_SECTION *lock =
      NtCurrentTeb()->ProcessEnvironmentBlock->FastPebLock;
  RtlEnterCriticalSection(lock);
  const WCHAR *value = 0;
  WCHAR *env = params()->Environment;
  for (DWORD i = 0; env && env[i];) {
    if (matches(env + i, name, n)) {
      value = env + i + n + 1;
      break;
    }
    i += ulen(env + i) + 1;
  }
  if (!value) {
    RtlLeaveCriticalSection(lock);
    SetLastError(203);
    return 0;
  }
  DWORD len = ulen(value);
  if (cap <= len) {
    RtlLeaveCriticalSection(lock);
    return len + 1;
  }
  if (!out) {
    RtlLeaveCriticalSection(lock);
    SetLastError(87);
    return 0;
  }
  w32_copy(out, value, (len + 1) * 2);
  RtlLeaveCriticalSection(lock);
  return len;
}
DWORD W32API GetEnvironmentVariableA(const char *name, char *out, DWORD cap) {
  WCHAR key[260];
  if (!name || !MultiByteToWideChar(1252, 0, name, -1, key, 260))
    return 0;
  WCHAR *value = k32_alloc(65536);
  if (!value) {
    SetLastError(8);
    return 0;
  }
  value[0] = 0;
  DWORD r = GetEnvironmentVariableW(key, value, 32768);
  if (r >= 32768) {
    k32_free(value, 65536);
    return r;
  }
  if (!r) {
    if (cap && out)
      out[0] = 0;
    k32_free(value, 65536);
    return 0;
  }
  if (cap <= r) {
    k32_free(value, 65536);
    return r + 1;
  }
  if (!out) {
    k32_free(value, 65536);
    SetLastError(87);
    return 0;
  }
  int ok = WideCharToMultiByte(1252, 0, value, (int)r + 1, out, (int)cap, 0, 0);
  k32_free(value, 65536);
  return ok ? r : 0;
}
BOOL W32API SetEnvironmentVariableW(const WCHAR *name, const WCHAR *value) {
  DWORD n = ulen(name);
  if (!n) {
    SetLastError(87);
    return 0;
  }
  for (DWORD i = 0; i < n; i++)
    if (name[i] == '=') {
      SetLastError(87);
      return 0;
    }
  DWORD v = ulen(value);
  RTL_CRITICAL_SECTION *lock =
      NtCurrentTeb()->ProcessEnvironmentBlock->FastPebLock;
  RtlEnterCriticalSection(lock);
  WCHAR *old = params()->Environment;
  DWORD len = env_size(old), remove = 0, pos = len > 2 ? len - 1 : 0;
  for (DWORD i = 0; old && old[i];) {
    DWORD k = ulen(old + i) + 1;
    if (matches(old + i, name, n)) {
      pos = i;
      remove = k;
      break;
    }
    i += k;
  }
  DWORD add = value ? n + v + 2 : 0;
  if (len + add > 32767 + remove) {
    RtlLeaveCriticalSection(lock);
    SetLastError(8);
    return 0;
  }
  DWORD size = len - remove + add;
  if (size < 2)
    size = 2;
  WCHAR *env = k32_alloc(size * 2);
  if (!env) {
    RtlLeaveCriticalSection(lock);
    SetLastError(8);
    return 0;
  }
  w32_fill(env, 0, size * 2);
  DWORD at = 0;
  if (old && pos) {
    w32_copy(env, old, pos * 2);
    at = pos;
  }
  if (value) {
    w32_copy(env + at, name, n * 2);
    at += n;
    env[at++] = '=';
    w32_copy(env + at, value, v * 2);
    at += v;
    env[at++] = 0;
  }
  if (old && remove) {
    DWORD tail = pos + remove;
    while (old[tail]) {
      DWORD k = ulen(old + tail) + 1;
      w32_copy(env + at, old + tail, k * 2);
      at += k;
      tail += k;
    }
  }
  params()->Environment = env;
  k32_free(owned_env, owned_bytes);
  owned_env = env;
  owned_bytes = size * 2;
  RtlLeaveCriticalSection(lock);
  return 1;
}
BOOL W32API SetEnvironmentVariableA(const char *name, const char *value) {
  if (!name) {
    SetLastError(87);
    return 0;
  }
  DWORD n = w32_strlen(name) + 1, v = value ? w32_strlen(value) + 1 : 0;
  if (n > 32767 || v > 32767) {
    SetLastError(87);
    return 0;
  }
  WCHAR *p = k32_alloc((n + v) * 2);
  if (!p) {
    SetLastError(8);
    return 0;
  }
  BOOL ok =
      MultiByteToWideChar(1252, 0, name, -1, p, (int)n) &&
      (value ? MultiByteToWideChar(1252, 0, value, -1, p + n, (int)v) : 1);
  if (ok)
    ok = SetEnvironmentVariableW(p, value ? p + n : 0);
  k32_free(p, (n + v) * 2);
  return ok;
}
WCHAR *W32API GetEnvironmentStringsW(void) {
  RTL_CRITICAL_SECTION *lock =
      NtCurrentTeb()->ProcessEnvironmentBlock->FastPebLock;
  RtlEnterCriticalSection(lock);
  WCHAR *s = params()->Environment;
  DWORD n = env_size(s);
  DWORD *allocation = k32_alloc(n * 2 + 4);
  if (allocation) {
    *allocation = n * 2 + 4;
    if (s)
      w32_copy(allocation + 1, s, n * 2);
    else
      w32_fill(allocation + 1, 0, n * 2);
  }
  RtlLeaveCriticalSection(lock);
  if (!allocation)
    SetLastError(8);
  return allocation ? (void *)(allocation + 1) : 0;
}
BOOL W32API FreeEnvironmentStringsW(WCHAR *s) {
  if (!s) {
    SetLastError(87);
    return 0;
  }
  DWORD *p = (DWORD *)s - 1;
  k32_free(p, *p);
  return 1;
}
char *W32API GetEnvironmentStrings(void) {
  WCHAR *w = GetEnvironmentStringsW();
  if (!w)
    return 0;
  DWORD n = env_size(w);
  DWORD *a = k32_alloc(n + 4);
  if (a) {
    *a = n + 4;
    WideCharToMultiByte(1252, 0, w, (int)n, (void *)(a + 1), (int)n, 0, 0);
  }
  FreeEnvironmentStringsW(w);
  return a ? (void *)(a + 1) : 0;
}
char *W32API GetEnvironmentStringsA(void) { return GetEnvironmentStrings(); }
BOOL W32API FreeEnvironmentStringsA(char *s) {
  return FreeEnvironmentStringsW((void *)s);
}
WCHAR *W32API GetCommandLineW(void) { return params()->CommandLine.Buffer; }
char *W32API GetCommandLineA(void) {
  static char *command;
  RTL_CRITICAL_SECTION *lock =
      NtCurrentTeb()->ProcessEnvironmentBlock->FastPebLock;
  RtlEnterCriticalSection(lock);
  if (!command) {
    DWORD n = ulen(params()->CommandLine.Buffer) + 1;
    command = k32_alloc(n);
    if (command)
      WideCharToMultiByte(1252, 0, params()->CommandLine.Buffer, -1, command,
                          (int)n, 0, 0);
  }
  RtlLeaveCriticalSection(lock);
  return command;
}
