/* Single-byte CP437/850/1252 conversion, with checked output publication.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "codepages.inc"
#include <w32/kernel32.h>
static const WCHAR *page(DWORD cp) {
  return cp == 0 || cp == 3 || cp == 1252 ? cp1252
         : cp == 1 || cp == 437           ? cp437
         : cp == 850                      ? cp850
                                          : 0;
}
DWORD W32API GetACP(void) { return 1252; }
DWORD W32API GetOEMCP(void) { return 437; }
BOOL W32API IsValidCodePage(DWORD cp) {
  return cp == 437 || cp == 850 || cp == 1252;
}
BOOL W32API IsDBCSLeadByte(uint8_t ch) {
  (void)ch;
  return 0;
}
BOOL W32API GetCPInfo(DWORD cp, void *out) {
  if (!page(cp) || !out) {
    SetLastError(87);
    return 0;
  }
  uint8_t data[20] = {0};
  w32_put32(data, 1);
  data[4] = '?';
  w32_copy(out, data, 20);
  return 1;
}
int W32API MultiByteToWideChar(DWORD cp, DWORD flags, const char *src, int n,
                               WCHAR *out, int cap) {
  const WCHAR *table = page(cp);
  if (!table || !src || !n || n < -1 || cap < 0 || (!out && cap)) {
    SetLastError(87);
    return 0;
  }
  if (flags & ~(1u | 2u | 4u | 8u) || ((flags & 1) && (flags & 2))) {
    SetLastError(1004);
    return 0;
  }
  /* Composite and glyph conversions need multicodepoint mappings. Refuse
   * rather than silently claiming those distinct modes. */
  if (flags & (2u | 4u)) {
    SetLastError(1004);
    return 0;
  }
  if (n == -1)
    n = (int)w32_strlen(src) + 1;
  if (!cap)
    return n;
  if (cap < n) {
    SetLastError(122);
    return 0;
  }
  for (int i = 0; i < n; i++) {
    uint8_t c = (uint8_t)src[i];
    out[i] = c < 128 ? c : table[c - 128];
  }
  return n;
}
int W32API WideCharToMultiByte(DWORD cp, DWORD flags, const WCHAR *src, int n,
                               char *out, int cap, const char *def,
                               BOOL *used) {
  const WCHAR *table = page(cp);
  if (!table || !src || !n || n < -1 || cap < 0 || (!out && cap)) {
    SetLastError(87);
    return 0;
  }
  if (flags & ~0x400u) {
    SetLastError(1004);
    return 0;
  }
  if (n == -1) {
    n = 0;
    while (src[n])
      n++;
    n++;
  }
  if (cap && cap < n) {
    SetLastError(122);
    return 0;
  }
  BOOL replace = 0;
  for (int i = 0; i < n; i++) {
    int c = src[i] < 128 ? src[i] : -1;
    if (c < 0)
      for (int j = 0; j < 128; j++)
        if (table[j] == src[i]) {
          c = j + 128;
          break;
        }
    if (c < 0) {
      c = def ? (uint8_t)*def : '?';
      replace = 1;
    }
    if (cap)
      out[i] = (char)c;
  }
  if (used)
    *used = replace;
  return n;
}
int k32_path(const char *src, char out[778]) {
  if (!src) {
    SetLastError(87);
    return 0;
  }
  DWORD at = 0, n = 0;
  for (; src[n]; n++) {
    if (n >= 259) {
      SetLastError(206);
      return 0;
    }
    WCHAR c =
        (uint8_t)src[n] < 128 ? (uint8_t)src[n] : cp1252[(uint8_t)src[n] - 128];
    if (c < 128)
      out[at++] = (char)c;
    else if (c < 2048) {
      out[at++] = (char)(0xc0 | (c >> 6));
      out[at++] = (char)(0x80 | (c & 63));
    } else {
      out[at++] = (char)(0xe0 | (c >> 12));
      out[at++] = (char)(0x80 | ((c >> 6) & 63));
      out[at++] = (char)(0x80 | (c & 63));
    }
  }
  out[at] = 0;
  return 1;
}
int k32_from_utf8(const char *src, char *out, DWORD cap) {
  DWORD n = 0;
  while (*src) {
    DWORD c = (uint8_t)*src++;
    if (c >= 0xc0 && c < 0xe0) {
      DWORD b = (uint8_t)*src++;
      if ((b & 0xc0) != 0x80)
        return 0;
      c = ((c & 31) << 6) | (b & 63);
    } else if (c >= 0xe0 && c < 0xf0) {
      DWORD b = (uint8_t)*src++;
      if ((b & 0xc0) != 0x80)
        return 0;
      DWORD d = (uint8_t)*src++;
      if ((d & 0xc0) != 0x80)
        return 0;
      c = ((c & 15) << 12) | ((b & 63) << 6) | (d & 63);
    } else if (c >= 128)
      return 0;
    int b = c < 128 ? (int)c : -1;
    if (b < 0)
      for (int i = 0; i < 128; i++)
        if (cp1252[i] == c) {
          b = i + 128;
          break;
        }
    if (b < 0 || n + 1 >= cap)
      return 0;
    out[n++] = (char)b;
  }
  if (n >= cap)
    return 0;
  out[n] = 0;
  return 1;
}
int W32API lstrlenA(const char *s) { return (int)w32_strlen(s); }
char *W32API lstrcpyA(char *d, const char *s) {
  w32_copy(d, s, w32_strlen(s) + 1);
  return d;
}
char *W32API lstrcatA(char *d, const char *s) {
  w32_copy(d + w32_strlen(d), s, w32_strlen(s) + 1);
  return d;
}
char *W32API lstrcpynA(char *d, const char *s, int n) {
  if (n > 0) {
    int i = 0;
    while (s[i] && i + 1 < n) {
      d[i] = s[i];
      i++;
    }
    d[i] = 0;
  }
  return d;
}
int W32API lstrcmpA(const char *a, const char *b) { return w32_strcmp(a, b); }
int W32API lstrcmpiA(const char *a, const char *b) { return w32_stricmp(a, b); }
