/* Direct-file Windows 98 INI facade. No NT IniFileMapping.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
#define INI_MAX 65535u
struct span {
  DWORD at, n;
};
struct line {
  DWORD start, end;
  struct span section, key, value;
  int kind;
};
static int space(char c) { return c == ' ' || c == '\t'; }
static struct span trim(const char *s, DWORD a, DWORD b) {
  while (a < b && space(s[a]))
    a++;
  while (b > a && space(s[b - 1]))
    b--;
  return (struct span){a, b - a};
}
static int equal(const char *s, struct span a, const char *b) {
  if (!b || a.n != w32_strlen(b))
    return 0;
  for (DWORD i = 0; i < a.n; i++) {
    unsigned x = (uint8_t)s[a.at + i], y = (uint8_t)b[i];
    if (x >= 'a' && x <= 'z')
      x -= 32;
    if (y >= 'a' && y <= 'z')
      y -= 32;
    if (x != y)
      return 0;
  }
  return 1;
}
static struct line next(const char *s, DWORD size, DWORD at) {
  struct line l = {.start = at};
  DWORD end = at;
  while (end < size && s[end] != '\r' && s[end] != '\n')
    end++;
  l.end = end;
  if (l.end < size && s[l.end] == '\r')
    l.end++;
  if (l.end < size && s[l.end] == '\n')
    l.end++;
  struct span t = trim(s, at, end);
  if (!t.n || s[t.at] == ';')
    return l;
  if (s[t.at] == '[') {
    DWORD p = t.at + 1;
    while (p < end && s[p] != ']')
      p++;
    if (p < end) {
      l.kind = 1;
      l.section = trim(s, t.at + 1, p);
    }
    return l;
  }
  DWORD p = t.at;
  while (p < end && s[p] != '=')
    p++;
  if (p < end) {
    l.kind = 2;
    l.key = trim(s, t.at, p);
    l.value = trim(s, p + 1, end);
  }
  return l;
}
static char *read_ini(const char *file, DWORD *size) {
  char path[260];
  if (!file)
    file = "WIN.INI";
  int explicit = 0;
  for (DWORD i = 0; file[i]; i++)
    if (file[i] == ':' || file[i] == '/' || file[i] == '\\')
      explicit = 1;
  if (explicit) {
    if (!w32_copy_string(path, sizeof(path), file)) {
      SetLastError(206);
      return 0;
    }
  } else {
    if (w32_strlen(file) + 12 >= sizeof(path)) {
      SetLastError(206);
      return 0;
    }
    w32_copy_string(path, sizeof(path), "C:\\WINDOWS\\");
    w32_copy_string(path + 11, sizeof(path) - 11, file);
  }
  char *text = k32_alloc(INI_MAX + 1);
  if (!text) {
    SetLastError(8);
    return 0;
  }
  *size = 0;
  text[0] = 0;
  HANDLE h = CreateFileA(path, 0x80000000u, 3, 0, 3, 0, 0);
  if (h == K32_INVALID) {
    if (GetLastError() == 2 || GetLastError() == 3)
      return text;
    k32_free(text, INI_MAX + 1);
    return 0;
  }
  DWORD high = 0, n = GetFileSize(h, &high);
  if (high || n > INI_MAX) {
    CloseHandle(h);
    k32_free(text, INI_MAX + 1);
    SetLastError(111);
    return 0;
  }
  DWORD got = 0;
  BOOL ok = ReadFile(h, text, n, &got, 0);
  DWORD e = GetLastError();
  CloseHandle(h);
  if (!ok || got != n) {
    k32_free(text, INI_MAX + 1);
    SetLastError(ok ? 1117 : e);
    return 0;
  }
  text[n] = 0;
  *size = n;
  return text;
}
static DWORD copy_value(const char *s, struct span v, char *out, DWORD cap) {
  if (!cap)
    return 0;
  DWORD n = v.n;
  if (n >= cap)
    n = cap - 1;
  for (DWORD i = 0; i < n; i++)
    out[i] = s[v.at + i];
  out[n] = 0;
  return n;
}
static DWORD get(const char *section, const char *key, const char *def,
                 char *out, DWORD cap, const char *file, int raw) {
  if (!cap || !out)
    return 0;
  DWORD size = 0;
  char *s = read_ini(file, &size);
  DWORD at = 0, total = 0;
  int active = 0, seen = 0, found = 0;
  struct span value = {0};
  if (s)
    while (at < size) {
      struct line l = next(s, size, at);
      at = l.end;
      if (l.kind == 1) {
        if (active)
          break;
        if (!section || equal(s, l.section, section)) {
          active = !!section;
          seen = 1;
          if (!section) {
            DWORD n = l.section.n;
            if (total + n + 2 > cap) {
              DWORD limit = cap > 1 ? cap - 2 : 0;
              for (DWORD j = 0; j < n && total < limit; j++)
                out[total++] = s[l.section.at + j];
              total = limit;
              goto truncated;
            }
            w32_copy(out + total, s + l.section.at, n);
            total += n;
            out[total++] = 0;
          }
        }
      } else if (active && l.kind == 2) {
        if (key) {
          if (equal(s, l.key, key)) {
            value = l.value;
            found = 1;
            break;
          }
        } else {
          DWORD n = raw ? l.key.n + 1 + l.value.n : l.key.n;
          if (total + n + 2 > cap) {
            DWORD limit = cap > 1 ? cap - 2 : 0;
            for (DWORD j = 0; j < n && total < limit; j++)
              out[total++] = j < l.key.n    ? s[l.key.at + j]
                             : j == l.key.n ? '='
                                            : s[l.value.at + j - l.key.n - 1];
            total = limit;
            goto truncated;
          }
          w32_copy(out + total, s + l.key.at, l.key.n);
          total += l.key.n;
          if (raw) {
            out[total++] = '=';
            w32_copy(out + total, s + l.value.at, l.value.n);
            total += l.value.n;
          }
          out[total++] = 0;
        }
      }
    }
  if (!section || !key) {
    if (total < cap)
      out[total] = 0;
    if (!total && cap > 1)
      out[1] = 0;
    k32_free(s, INI_MAX + 1);
    return total;
  }
  if (found) {
    if (value.n >= 2 &&
        ((s[value.at] == '"' && s[value.at + value.n - 1] == '"') ||
         (s[value.at] == '\'' && s[value.at + value.n - 1] == '\''))) {
      value.at++;
      value.n -= 2;
    }
    total = copy_value(s, value, out, cap);
  } else {
    if (!def)
      def = "";
    DWORD end = w32_strlen(def);
    while (end && def[end - 1] == ' ')
      end--;
    value = (struct span){0, end};
    total = copy_value(def, value, out, cap);
  }
  (void)seen;
  k32_free(s, INI_MAX + 1);
  return total;
truncated:
  /* Copy the prefix that fits, even when a single entry exceeds capacity. */
  if (cap > 1) {
    out[cap - 2] = 0;
    out[cap - 1] = 0;
  } else
    out[0] = 0;
  k32_free(s, INI_MAX + 1);
  return total;
}
DWORD W32API GetPrivateProfileStringA(const char *s, const char *k,
                                      const char *d, char *out, DWORD n,
                                      const char *f) {
  return get(s, k, d, out, n, f, 0);
}
DWORD W32API GetPrivateProfileSectionA(const char *s, char *out, DWORD n,
                                       const char *f) {
  if (!s) {
    SetLastError(87);
    return 0;
  }
  return get(s, 0, 0, out, n, f, 1);
}
DWORD W32API GetPrivateProfileSectionNamesA(char *out, DWORD n, const char *f) {
  return get(0, 0, 0, out, n, f, 0);
}
DWORD W32API GetPrivateProfileIntA(const char *s, const char *k, int def,
                                   const char *f) {
  char value[64];
  if (!GetPrivateProfileStringA(s, k, "", value, sizeof(value), f))
    return (DWORD)def;
  DWORD at = 0, n = 0, base = 10;
  int neg = 0;
  if (value[at] == '-' || value[at] == '+')
    neg = value[at++] == '-';
  if (value[at] == '0' && (value[at + 1] == 'x' || value[at + 1] == 'X')) {
    base = 16;
    at += 2;
  }
  for (; value[at]; at++) {
    unsigned c = (uint8_t)value[at];
    DWORD digit = c >= '0' && c <= '9'   ? c - '0'
                  : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                         : 99;
    if (digit >= base)
      break;
    n = n * base + digit;
  }
  return neg ? 0u - n : n;
}
static int append(char *out, DWORD *n, const char *s, DWORD bytes) {
  if (bytes > INI_MAX - *n)
    return 0;
  w32_copy(out + *n, s, bytes);
  *n += bytes;
  return 1;
}
BOOL W32API WritePrivateProfileStringA(const char *section, const char *key,
                                       const char *value, const char *file) {
  if (!section && !key && !value)
    return 1;
  if (!section) {
    SetLastError(87);
    return 0;
  }
  RTL_CRITICAL_SECTION *lock =
      NtCurrentTeb()->ProcessEnvironmentBlock->FastPebLock;
  RtlEnterCriticalSection(lock);
  DWORD size;
  char *s = read_ini(file, &size), *out = k32_alloc(INI_MAX + 1);
  BOOL ok = s && out;
  DWORD n = 0, at = 0;
  int active = 0, matched = 0, replaced = 0, ended = 0;
  while (ok && at < size) {
    struct line l = next(s, size, at);
    at = l.end;
    if (l.kind == 1) {
      if (active) {
        active = 0;
        ended = 1;
        if (key && value && !replaced) {
          ok = append(out, &n, key, w32_strlen(key)) &&
               append(out, &n, "=", 1) &&
               append(out, &n, value, w32_strlen(value)) &&
               append(out, &n, "\r\n", 2);
          replaced = 1;
        }
      }
      if (!matched && !ended && equal(s, l.section, section)) {
        active = 1;
        matched = 1;
      }
    }
    if (active && !key)
      continue;
    if (active && l.kind == 2 && equal(s, l.key, key)) {
      if (value && !replaced) {
        ok = append(out, &n, key, w32_strlen(key)) && append(out, &n, "=", 1) &&
             append(out, &n, value, w32_strlen(value)) &&
             append(out, &n, "\r\n", 2);
        replaced = 1;
      }
      continue;
    }
    ok = ok && append(out, &n, s + l.start, l.end - l.start);
  }
  if (ok && key && value && !replaced) {
    if (n && out[n - 1] != '\n')
      ok = append(out, &n, "\r\n", 2);
    if (!matched)
      ok = ok && append(out, &n, "[", 1) &&
           append(out, &n, section, w32_strlen(section)) &&
           append(out, &n, "]\r\n", 3);
    ok = ok && append(out, &n, key, w32_strlen(key)) &&
         append(out, &n, "=", 1) && append(out, &n, value, w32_strlen(value)) &&
         append(out, &n, "\r\n", 2);
  }
  if (ok) {
    char path[260];
    if (!file)
      file = "WIN.INI";
    int explicit = 0;
    for (DWORD i = 0; file[i]; i++)
      if (file[i] == ':' || file[i] == '/' || file[i] == '\\')
        explicit = 1;
    if (explicit)
      ok = w32_copy_string(path, sizeof(path), file);
    else {
      ok = w32_copy_string(path, sizeof(path), "C:\\WINDOWS\\") &&
           w32_copy_string(path + 11, sizeof(path) - 11, file);
    }
    if (ok) {
      HANDLE h = CreateFileA(path, 0x40000000u, 3, 0, 2, 128, 0);
      ok = h != K32_INVALID;
      if (ok) {
        DWORD written;
        ok = WriteFile(h, out, n, &written, 0) && written == n &&
             FlushFileBuffers(h);
        DWORD error = GetLastError();
        CloseHandle(h);
        SetLastError(error);
      }
    }
  } else
    SetLastError(s && out ? 112 : 8);
  k32_free(s, INI_MAX + 1);
  k32_free(out, INI_MAX + 1);
  RtlLeaveCriticalSection(lock);
  return ok;
}
DWORD W32API GetProfileStringA(const char *s, const char *k, const char *d,
                               char *o, DWORD n) {
  return GetPrivateProfileStringA(s, k, d, o, n, 0);
}
DWORD W32API GetProfileIntA(const char *s, const char *k, int d) {
  return GetPrivateProfileIntA(s, k, d, 0);
}
BOOL W32API WriteProfileStringA(const char *s, const char *k, const char *v) {
  return WritePrivateProfileStringA(s, k, v, 0);
}
static int hex_value(unsigned c) {
  return c >= '0' && c <= '9'   ? (int)c - '0'
         : c >= 'A' && c <= 'F' ? (int)c - 'A' + 10
         : c >= 'a' && c <= 'f' ? (int)c - 'a' + 10
                                : -1;
}
BOOL W32API GetPrivateProfileStructA(const char *section, const char *key,
                                     void *out, DWORD bytes, const char *file) {
  if (bytes > 32766 || (!out && bytes) || !section || !key) {
    SetLastError(87);
    return 0;
  }
  DWORD n = bytes * 2 + 4;
  char *text = k32_alloc(n);
  uint8_t *copy = k32_alloc(bytes ? bytes : 1);
  if (!text || !copy) {
    k32_free(text, n);
    k32_free(copy, bytes ? bytes : 1);
    SetLastError(8);
    return 0;
  }
  DWORD got = GetPrivateProfileStringA(section, key, "", text, n, file);
  unsigned sum = 0;
  BOOL ok = got == bytes * 2 + 2;
  for (DWORD i = 0; ok && i <= bytes; i++) {
    int a = hex_value((uint8_t)text[i * 2]),
        b = hex_value((uint8_t)text[i * 2 + 1]);
    if (a < 0 || b < 0) {
      ok = 0;
      break;
    }
    unsigned v = (unsigned)(a * 16 + b);
    if (i < bytes) {
      copy[i] = (uint8_t)v;
      sum = (sum + v) & 255;
    } else
      ok = v == sum;
  }
  if (ok && bytes)
    w32_copy(out, copy, bytes);
  k32_free(text, n);
  k32_free(copy, bytes ? bytes : 1);
  if (!ok)
    SetLastError(13);
  return ok;
}
BOOL W32API WritePrivateProfileStructA(const char *section, const char *key,
                                       const void *data, DWORD bytes,
                                       const char *file) {
  if (!data)
    return WritePrivateProfileStringA(section, key, 0, file);
  if (bytes > 32766) {
    SetLastError(87);
    return 0;
  }
  DWORD n = bytes * 2 + 3;
  char *text = k32_alloc(n);
  if (!text) {
    SetLastError(8);
    return 0;
  }
  unsigned sum = 0;
  for (DWORD i = 0; i <= bytes; i++) {
    unsigned v = i == bytes ? sum : ((const uint8_t *)data)[i];
    text[i * 2] = "0123456789ABCDEF"[v >> 4];
    text[i * 2 + 1] = "0123456789ABCDEF"[v & 15];
    sum = (sum + v) & 255;
  }
  text[n - 1] = 0;
  BOOL ok = WritePrivateProfileStringA(section, key, text, file);
  k32_free(text, n);
  return ok;
}
static BOOL save_ini(const char *file, const char *text, DWORD n) {
  char path[260];
  if (!file)
    file = "WIN.INI";
  int explicit = 0;
  for (DWORD i = 0; file[i]; i++)
    if (file[i] == ':' || file[i] == '/' || file[i] == '\\')
      explicit = 1;
  if (explicit) {
    if (!w32_copy_string(path, sizeof(path), file)) {
      SetLastError(206);
      return 0;
    }
  } else if (!w32_copy_string(path, sizeof(path), "C:\\WINDOWS\\") ||
             !w32_copy_string(path + 11, sizeof(path) - 11, file)) {
    SetLastError(206);
    return 0;
  }
  HANDLE h = CreateFileA(path, 0x40000000u, 3, 0, 2, 128, 0);
  if (h == K32_INVALID)
    return 0;
  DWORD written;
  BOOL ok =
      WriteFile(h, text, n, &written, 0) && written == n && FlushFileBuffers(h);
  DWORD e = GetLastError();
  CloseHandle(h);
  SetLastError(e);
  return ok;
}
static BOOL section_lines(char *out, DWORD *n, const char *values) {
  if (!values)
    return 1;
  for (const char *p = values; *p;) {
    DWORD length = w32_strlen(p);
    if (!append(out, n, p, length) || !append(out, n, "\r\n", 2))
      return 0;
    p += length + 1;
  }
  return 1;
}
BOOL W32API WritePrivateProfileSectionA(const char *section, const char *values,
                                        const char *file) {
  if (!section) {
    SetLastError(87);
    return 0;
  }
  RTL_CRITICAL_SECTION *lock =
      NtCurrentTeb()->ProcessEnvironmentBlock->FastPebLock;
  RtlEnterCriticalSection(lock);
  DWORD size = 0, n = 0;
  char *s = read_ini(file, &size), *out = k32_alloc(INI_MAX + 1);
  BOOL ok = s && out;
  int active = 0, matched = 0;
  for (DWORD at = 0; ok && at < size;) {
    struct line l = next(s, size, at);
    at = l.end;
    if (l.kind == 1) {
      active = 0;
      if (!matched && equal(s, l.section, section)) {
        active = 1;
        matched = 1;
        if (values)
          ok = append(out, &n, s + l.start, l.end - l.start) &&
               section_lines(out, &n, values);
        continue;
      }
    }
    if (!active)
      ok = append(out, &n, s + l.start, l.end - l.start);
  }
  if (ok && !matched && values) {
    if (n && out[n - 1] != '\n')
      ok = append(out, &n, "\r\n", 2);
    ok = ok && append(out, &n, "[", 1) &&
         append(out, &n, section, w32_strlen(section)) &&
         append(out, &n, "]\r\n", 3) && section_lines(out, &n, values);
  }
  if (ok)
    ok = save_ini(file, out, n);
  else
    SetLastError(s && out ? 112 : 8);
  k32_free(s, INI_MAX + 1);
  k32_free(out, INI_MAX + 1);
  RtlLeaveCriticalSection(lock);
  return ok;
}
BOOL W32API WriteProfileSectionA(const char *section, const char *values) {
  return WritePrivateProfileSectionA(section, values, 0);
}
DWORD W32API GetProfileSectionA(const char *section, char *out, DWORD n) {
  return GetPrivateProfileSectionA(section, out, n, 0);
}
