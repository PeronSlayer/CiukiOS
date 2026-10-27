/* Registry entry points of NT 5/6 that the reused Wine 10.0 DLLs import
 * (f5-20A): RegDeleteKeyExW, RegDeleteTreeA/W, RegGetValueA/W and
 * RegLoadMUIStringA/W, over the f5-07A facade. Windows 9x deletes keys
 * recursively, so a named tree deletion is one RegDeleteKey.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright 2026 CiukiOS contributors. */
#include "internal.h"
extern uint32_t w32r_ansi_scalar(uint8_t);
extern uint8_t w32r_scalar_ansi(uint32_t);
#ifdef W32_HOST_MODEL
#define IMPORT
#else
#define IMPORT __declspec(dllimport)
#endif
IMPORT uint32_t W32API ExpandEnvironmentStringsA(const char *, char *, uint32_t);
IMPORT uint32_t W32API ExpandEnvironmentStringsW(const WCHAR *, WCHAR *, uint32_t);
IMPORT void *W32API LoadLibraryExA(const char *, void *, uint32_t);
IMPORT int W32API FreeLibrary(void *);
IMPORT void *W32API FindResourceA(void *, const char *, const char *);
IMPORT void *W32API LoadResource(void *, void *);
IMPORT uint32_t W32API SizeofResource(void *, void *);
IMPORT int W32API GetComputerNameA(char *, uint32_t *);
LONG W32API RegOpenKeyExA(uint32_t, const char *, uint32_t, uint32_t, uint32_t *);
LONG W32API RegOpenKeyExW(uint32_t, const WCHAR *, uint32_t, uint32_t, uint32_t *);
LONG W32API RegCloseKey(uint32_t);
LONG W32API RegDeleteKeyW(uint32_t, const WCHAR *);
LONG W32API RegDeleteValueW(uint32_t, const WCHAR *);
LONG W32API RegEnumKeyExW(uint32_t, uint32_t, WCHAR *, uint32_t *, void *, WCHAR *, uint32_t *,
                          void *);
LONG W32API RegEnumValueW(uint32_t, uint32_t, WCHAR *, uint32_t *, void *, uint32_t *, uint8_t *,
                          uint32_t *);
LONG W32API RegQueryValueExA(uint32_t, const char *, void *, uint32_t *, uint8_t *, uint32_t *);
LONG W32API RegQueryValueExW(uint32_t, const WCHAR *, void *, uint32_t *, uint8_t *, uint32_t *);
/* KEY_WOW64_* views do not exist on 32-bit Windows; reserved must be 0. */
LONG W32API RegDeleteKeyExW(uint32_t key, const WCHAR *sub, uint32_t access, uint32_t reserved) {
  (void)access;
  if (reserved || !sub)
    return 87;
  return RegDeleteKeyW(key, sub);
}
static int widen_name(const char *a, WCHAR *w, uint32_t cap) {
  uint32_t i = 0;
  for (; a[i]; i++) {
    if (i + 1 >= cap)
      return 0;
    w[i] = (WCHAR)w32r_ansi_scalar((uint8_t)a[i]);
  }
  w[i] = 0;
  return 1;
}
/* NULL or "" empties the key (subkeys, then values); a name deletes that
 * subkey and everything below it. */
static LONG delete_tree(uint32_t key, const WCHAR *sub) {
  uint32_t k = key;
  LONG e = 0;
  if (sub && *sub) {
    e = RegOpenKeyExW(key, sub, 0, 0xf003f, &k);
    if (e)
      return e;
  }
  WCHAR name[257];
  for (;;) {
    uint32_t n = 257;
    e = RegEnumKeyExW(k, 0, name, &n, 0, 0, 0, 0);
    if (e == 259) {
      e = 0;
      break;
    }
    if (e || (e = RegDeleteKeyW(k, name)))
      break;
  }
  for (; !e;) {
    uint32_t n = 257;
    e = RegEnumValueW(k, 0, name, &n, 0, 0, 0, 0);
    if (e == 259) {
      e = 0;
      break;
    }
    if (!e)
      e = RegDeleteValueW(k, name);
  }
  if (k != key) {
    RegCloseKey(k);
    if (!e)
      e = RegDeleteKeyW(key, sub);
  }
  return e;
}
LONG W32API RegDeleteTreeW(uint32_t key, const WCHAR *sub) { return delete_tree(key, sub); }
LONG W32API RegDeleteTreeA(uint32_t key, const char *sub) {
  WCHAR w[1024];
  if (sub && !widen_name(sub, w, 1024))
    return 87;
  return delete_tree(key, sub ? w : 0);
}
/* RRF_RT_* bit of a registry type (REG_NONE..REG_QWORD). */
static uint32_t type_bit(uint32_t t) {
  return t == 0 ? 1 : t == 1 ? 2 : t == 2 ? 4 : t == 3 ? 8 : t == 4 ? 0x10 : t == 7 ? 0x20
       : t == 11 ? 0x40 : 0;
}
static LONG get_value(uint32_t key, const void *sub, const void *value, uint32_t flags,
                      uint32_t *type, void *data, uint32_t *size, int wide) {
  const uint32_t noexpand = 0x10000000u, zero = 0x20000000u;
  uint32_t unit = wide ? 2 : 1, capacity = data && size ? *size : 0;
  /* RRF_RT_REG_EXPAND_SZ alone needs RRF_NOEXPAND; RRF_RT_ANY does not. */
  if (!(flags & 0xffff) || (data && !size) ||
      ((flags & 4) && !(flags & noexpand) && (flags & 0xffff) != 0xffff) ||
      (flags & ~(0xffffu | 0x30000u | noexpand | zero)))
    return 87;
  uint32_t k = key;
  LONG e = 0;
  if (sub && (wide ? *(const WCHAR *)sub : *(const char *)sub))
    e = wide ? RegOpenKeyExW(key, sub, 0, 1, &k) : RegOpenKeyExA(key, sub, 0, 1, &k);
  uint32_t t = 0, n = 0, need = 0;
  uint8_t *raw = 0, *expanded = 0;
  if (!e)
    e = wide ? RegQueryValueExW(k, value, 0, &t, 0, &n) : RegQueryValueExA(k, value, 0, &t, 0, &n);
  if (!e && !(raw = w32r_alloc(n + 2 * unit)))
    e = 8;
  if (!e) {
    w32_fill(raw, 0, n + 2 * unit);
    e = wide ? RegQueryValueExW(k, value, 0, &t, raw, &n) : RegQueryValueExA(k, value, 0, &t, raw, &n);
  }
  uint8_t *result = raw;
  need = n;
  int text = t == 1 || t == 2 || t == 7;
  if (!e && text && (n < unit || (wide ? ((WCHAR *)raw)[n / 2 - 1] : raw[n - 1])))
    need = n + unit; /* the stored string lacked its terminator */
  if (!e && t == 2 && !(flags & noexpand)) {
    uint32_t chars = wide ? ExpandEnvironmentStringsW((const WCHAR *)raw, 0, 0)
                          : ExpandEnvironmentStringsA((const char *)raw, 0, 0);
    if (!chars || !(expanded = w32r_alloc(chars * unit)))
      e = chars ? 8 : 13;
    else if (wide)
      ExpandEnvironmentStringsW((const WCHAR *)raw, (WCHAR *)expanded, chars);
    else
      ExpandEnvironmentStringsA((const char *)raw, (char *)expanded, chars);
    result = expanded;
    need = chars * unit;
    t = 1;
  }
  if (!e && !(type_bit(t) & flags))
    e = 1630;
  if (!e && t == 3 && ((flags & 0xffff) == 0x18 || (flags & 0xffff) == 0x48) &&
      n != ((flags & 0xffff) == 0x18 ? 4u : 8u))
    e = 1629;
  if (!e) {
    if (type)
      *type = t;
    if (data && capacity < need)
      e = 234;
    else if (data)
      w32_copy(data, result, need);
    if (size)
      *size = need;
  }
  if (e && (flags & zero) && data)
    w32_fill(data, 0, capacity);
  w32r_free(raw);
  w32r_free(expanded);
  if (k != key)
    RegCloseKey(k);
  return e;
}
LONG W32API RegGetValueW(uint32_t key, const WCHAR *sub, const WCHAR *value, uint32_t flags,
                         uint32_t *type, void *data, uint32_t *size) {
  return get_value(key, sub, value, flags, type, data, size, 1);
}
LONG W32API RegGetValueA(uint32_t key, const char *sub, const char *value, uint32_t flags,
                         uint32_t *type, void *data, uint32_t *size) {
  return get_value(key, sub, value, flags, type, data, size, 0);
}
/* The documented ANSI form is not supported (ERROR_CALL_NOT_IMPLEMENTED). */
LONG W32API RegLoadMUIStringA(uint32_t key, const char *value, char *out, uint32_t bytes,
                              uint32_t *copied, uint32_t flags, const char *directory) {
  (void)key;
  (void)value;
  (void)out;
  (void)bytes;
  (void)copied;
  (void)flags;
  (void)directory;
  return 120;
}
/* String `id` of a module's RT_STRING table (16 counted strings a block). */
static LONG module_string(const WCHAR *path, uint32_t id, WCHAR *out, uint32_t cap,
                          uint32_t *length) {
  char a[262];
  uint32_t i = 0;
  for (; path[i]; i++) {
    uint8_t c = w32r_scalar_ansi(path[i]);
    if (i + 1 >= sizeof(a) || (c == '?' && path[i] != '?'))
      return 2;
    a[i] = (char)c;
  }
  a[i] = 0;
  void *module = LoadLibraryExA(a, 0, 2);
  if (!module)
    return 2;
  void *r = FindResourceA(module, (const char *)(uintptr_t)(id / 16 + 1), (const char *)6);
  const uint8_t *p = r ? LoadResource(module, r) : 0;
  uint32_t size = r ? SizeofResource(module, r) : 0, at = 0;
  LONG e = 1814;
  for (uint32_t s = 0; p && s < 16 && at + 2 <= size; s++) {
    uint32_t n = w32_u16(p + at);
    if (n > (size - at - 2) / 2)
      break;
    if (s == id % 16) {
      if (n) {
        uint32_t copy = n < cap ? n : cap - 1;
        for (uint32_t c = 0; c < copy; c++)
          out[c] = w32_u16(p + at + 2 + c * 2);
        out[copy] = 0;
        *length = copy;
        e = 0;
      }
      break;
    }
    at += 2 + n * 2;
  }
  FreeLibrary(module);
  return e;
}
/* "@[path]dll,-id" loads string id from the module (environment strings
 * expanded, a relative path taken from `directory`); any other string is
 * returned as stored. REG_MUI_STRING_TRUNCATE (1) truncates. */
LONG W32API RegLoadMUIStringW(uint32_t key, const WCHAR *value, WCHAR *out, uint32_t bytes,
                              uint32_t *copied, uint32_t flags, const WCHAR *directory) {
  if ((flags & ~1u) || (!out && bytes) || (bytes & 1) || ((flags & 1) && copied))
    return 87;
  uint32_t t = 0, n = 0;
  LONG e = RegQueryValueExW(key, value, 0, &t, 0, &n);
  if (e)
    return e;
  if (t != 1 && t != 2)
    return 13;
  WCHAR *text = w32r_alloc(n + 4), *path = 0, *loaded = 0;
  if (!text)
    return 8;
  w32_fill(text, 0, n + 4);
  e = RegQueryValueExW(key, value, 0, &t, (uint8_t *)text, &n);
  const WCHAR *result = text;
  uint32_t length = 0;
  if (!e && text[0] == '@') {
    uint32_t comma = 1;
    while (text[comma] && text[comma] != ',')
      comma++;
    uint32_t id = 0, at = comma + 1;
    if (text[comma] != ',' || text[at] != '-')
      e = 13;
    for (at++; !e && text[at] >= '0' && text[at] <= '9'; at++)
      id = id * 10 + (text[at] - '0');
    if (!e && (text[at] || id > 0xffff || at == comma + 2))
      e = 13;
    if (!e) {
      text[comma] = 0;
      uint32_t chars = ExpandEnvironmentStringsW(text + 1, 0, 0), prefix = 0;
      if (directory && text[1] != '\\' && !(text[1] && text[2] == ':'))
        while (directory[prefix])
          prefix++;
      path = chars ? w32r_alloc((chars + prefix + 2) * 2) : 0;
      loaded = w32r_alloc(4096 * 2);
      if (!path || !loaded)
        e = 8;
      else {
        w32_copy(path, directory, prefix * 2);
        if (prefix && path[prefix - 1] != '\\')
          path[prefix++] = '\\';
        ExpandEnvironmentStringsW(text + 1, path + prefix, chars);
        e = module_string(path, id, loaded, 4096, &length);
        result = loaded;
      }
    }
  }
  if (!e && result == text)
    while (text[length])
      length++;
  if (!e) {
    uint32_t need = (length + 1) * 2;
    if (copied)
      *copied = need;
    if (bytes >= need)
      w32_copy(out, result, need);
    else if ((flags & 1) && bytes >= 2) {
      w32_copy(out, result, bytes - 2);
      out[bytes / 2 - 1] = 0;
    } else
      e = 234;
  }
  w32r_free(text);
  w32r_free(path);
  w32r_free(loaded);
  return e;
}
/* RegConnectRegistry: the local computer (NULL, "" or its own name, with or
 * without the leading backslashes) opens the named predefined root, as
 * Windows and Wine do through RegOpenKey(hKey, NULL); w32srv answers that
 * with the root's own handle, which RegCloseKey accepts. CiukiOS has no
 * remote registry service, so any other computer is ERROR_BAD_NETPATH (53).
 * The InstallShield 5 engine imports the ANSI entry point. */
static LONG connect_registry(const void *machine, int wide, uint32_t key, uint32_t *out) {
  if (!out)
    return 87;
  *out = 0;
  const uint8_t *a = machine;
  const WCHAR *w = machine;
  unsigned at = 0;
  if (machine && (wide ? w[0] : a[0]) == '\\' && (wide ? w[1] : a[1]) == '\\')
    at = 2;
  if (machine && (wide ? w[at] : a[at])) {
    char local[32];
    uint32_t n = sizeof(local);
    if (!GetComputerNameA(local, &n))
      return 53;
    for (unsigned i = 0;; i++, at++) {
      uint32_t c = wide ? w[at] : a[at], l = (uint8_t)local[i];
      if (c >= 'a' && c <= 'z')
        c -= 32;
      if (l >= 'a' && l <= 'z')
        l -= 32;
      if (c != l)
        return 53;
      if (!c)
        break;
    }
  }
  return RegOpenKeyExA(key, NULL, 0, 0xf003f, out);
}
LONG W32API RegConnectRegistryA(const char *machine, uint32_t key, uint32_t *out) {
  return connect_registry(machine, 0, key, out);
}
LONG W32API RegConnectRegistryW(const WCHAR *machine, uint32_t key, uint32_t *out) {
  return connect_registry(machine, 1, key, out);
}
