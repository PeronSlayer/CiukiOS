/* f5-20B: USER character functions (Char*, IsChar*, OEM conversions) and
 * wsprintf/wvsprintf. Code page 1252 is the ANSI code page and 437 the OEM
 * code page of the Windows 98 SE US system the personality reports. The
 * Unicode forms classify and map the 1252 repertoire with the same tables,
 * plus the case pairs of Latin Extended-A, basic Greek and basic Cyrillic;
 * other characters are neither letters nor case-mapped.
 * References: Microsoft "CharUpperA", "CharLowerBuffW", "CharNextA",
 * "CharPrevA", "CharToOemA", "OemToCharA", "IsCharAlphaA", "wsprintfA",
 * "wvsprintfA" (format types c C d hc hd hs hu i lc ld li ls lu lx p s S u
 * x X, flags - # 0, width, precision, 1024-byte output limit); the CP437 and
 * CP1252 mapping tables of the Unicode Consortium (MAPPINGS/VENDORS/MICSFT).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "unicode.h"
#include <stdarg.h>

/* ---- classification and case ---------------------------------------- */
static BOOL upper_1252(UINT c) {
  return (c >= 'A' && c <= 'Z') || c == 0x8a || c == 0x8c || c == 0x8e ||
         c == 0x9f || (c >= 0xc0 && c <= 0xde && c != 0xd7);
}
static BOOL lower_1252(UINT c) {
  return (c >= 'a' && c <= 'z') || c == 0x83 || c == 0x9a || c == 0x9c ||
         c == 0x9e || c == 0xaa || c == 0xb5 || c == 0xba ||
         (c >= 0xdf && c <= 0xff && c != 0xf7);
}
static UINT to_upper_1252(UINT c) {
  if (c >= 'a' && c <= 'z')
    return c - 32;
  if (c == 0x9a || c == 0x9c || c == 0x9e)
    return c - 16;
  if (c == 0xff)
    return 0x9f;
  if ((c >= 0xe0 && c <= 0xfe) && c != 0xf7)
    return c - 32;
  return c;
}
static UINT to_lower_1252(UINT c) {
  if (c >= 'A' && c <= 'Z')
    return c + 32;
  if (c == 0x8a || c == 0x8c || c == 0x8e)
    return c + 16;
  if (c == 0x9f)
    return 0xff;
  if ((c >= 0xc0 && c <= 0xde) && c != 0xd7)
    return c + 32;
  return c;
}
/* Unicode case pairs outside code page 1252. */
static int pair(UINT c, BOOL *upper) {
  if (c >= 0x100 && c <= 0x137 && c != 0x130 && c != 0x131) {
    *upper = !(c & 1);
    return 1;
  }
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e)) {
    *upper = (c & 1) != 0;
    return 1;
  }
  if (c >= 0x14a && c <= 0x177) {
    *upper = !(c & 1);
    return 1;
  }
  if ((c >= 0x391 && c <= 0x3a9 && c != 0x3a2) ||
      (c >= 0x410 && c <= 0x42f) || (c >= 0x400 && c <= 0x40f)) {
    *upper = 1;
    return 1;
  }
  if ((c >= 0x3b1 && c <= 0x3c9 && c != 0x3c2) ||
      (c >= 0x430 && c <= 0x44f) || (c >= 0x450 && c <= 0x45f)) {
    *upper = 0;
    return 1;
  }
  return 0;
}
static UINT pair_other(UINT c, BOOL upper) {
  if (c < 0x180) /* Latin Extended-A: the lower case follows its capital */
    return upper ? c + 1 : c - 1;
  if (c < 0x400)
    return upper ? c + 32 : c - 32;
  if (c >= 0x400 && c <= 0x40f)
    return c + 80;
  if (c >= 0x450 && c <= 0x45f)
    return c - 80;
  return upper ? c + 32 : c - 32;
}
/* Unicode scalar: its 1252 byte, or 0 when the code page lacks it. */
static UINT in_1252(UINT c) {
  if (c < 128)
    return c;
  int a = u32_ansi(c);
  return (uint8_t)a != '?' ? (uint8_t)a : 0;
}
UINT u32_upper(UINT c) {
  UINT a = in_1252(c);
  BOOL upper;
  if (a || !c)
    return u32_scalar((uint8_t)to_upper_1252(a));
  return pair(c, &upper) && !upper ? pair_other(c, 0) : c;
}
UINT u32_lower(UINT c) {
  UINT a = in_1252(c);
  BOOL upper;
  if (a || !c)
    return u32_scalar((uint8_t)to_lower_1252(a));
  return pair(c, &upper) && upper ? pair_other(c, 1) : c;
}
BOOL u32_alpha(UINT c) {
  UINT a = in_1252(c);
  BOOL upper;
  return a ? upper_1252(a) || lower_1252(a) : pair(c, &upper);
}
static BOOL w_upper(UINT c) {
  UINT a = in_1252(c);
  BOOL upper;
  return a ? upper_1252(a) : pair(c, &upper) && upper;
}
static BOOL w_lower(UINT c) {
  UINT a = in_1252(c);
  BOOL upper;
  return a ? lower_1252(a) : pair(c, &upper) && !upper;
}
static BOOL digit(UINT c) { return c >= '0' && c <= '9'; }

BOOL W32API IsCharAlphaA(char c) { return u32_alpha(u32_scalar((uint8_t)c)); }
BOOL W32API IsCharAlphaW(WCHAR c) { return u32_alpha(c); }
BOOL W32API IsCharAlphaNumericA(char c) {
  return digit((uint8_t)c) || IsCharAlphaA(c);
}
BOOL W32API IsCharAlphaNumericW(WCHAR c) { return digit(c) || u32_alpha(c); }
BOOL W32API IsCharUpperA(char c) { return upper_1252((uint8_t)c); }
BOOL W32API IsCharUpperW(WCHAR c) { return w_upper(c); }
BOOL W32API IsCharLowerA(char c) { return lower_1252((uint8_t)c); }
BOOL W32API IsCharLowerW(WCHAR c) { return w_lower(c); }

/* A pointer whose high word is zero carries one character in its low word. */
#define SINGLE(p) (((uintptr_t)(p) >> 16) == 0)
char *W32API CharUpperA(char *s) {
  if (SINGLE(s))
    return (char *)(uintptr_t)to_upper_1252((UINT)(uintptr_t)s & 0xff);
  for (char *p = s; *p; p++)
    *p = (char)to_upper_1252((uint8_t)*p);
  return s;
}
char *W32API CharLowerA(char *s) {
  if (SINGLE(s))
    return (char *)(uintptr_t)to_lower_1252((UINT)(uintptr_t)s & 0xff);
  for (char *p = s; *p; p++)
    *p = (char)to_lower_1252((uint8_t)*p);
  return s;
}
DWORD W32API CharUpperBuffA(char *s, DWORD n) {
  if (!s)
    return 0;
  for (DWORD i = 0; i < n; i++)
    s[i] = (char)to_upper_1252((uint8_t)s[i]);
  return n;
}
DWORD W32API CharLowerBuffA(char *s, DWORD n) {
  if (!s)
    return 0;
  for (DWORD i = 0; i < n; i++)
    s[i] = (char)to_lower_1252((uint8_t)s[i]);
  return n;
}
WCHAR *W32API CharUpperW(WCHAR *s) {
  if (SINGLE(s))
    return (WCHAR *)(uintptr_t)u32_upper((UINT)(uintptr_t)s & 0xffff);
  for (WCHAR *p = s; *p; p++)
    *p = (WCHAR)u32_upper(*p);
  return s;
}
WCHAR *W32API CharLowerW(WCHAR *s) {
  if (SINGLE(s))
    return (WCHAR *)(uintptr_t)u32_lower((UINT)(uintptr_t)s & 0xffff);
  for (WCHAR *p = s; *p; p++)
    *p = (WCHAR)u32_lower(*p);
  return s;
}
DWORD W32API CharUpperBuffW(WCHAR *s, DWORD n) {
  if (!s)
    return 0;
  for (DWORD i = 0; i < n; i++)
    s[i] = (WCHAR)u32_upper(s[i]);
  return n;
}
DWORD W32API CharLowerBuffW(WCHAR *s, DWORD n) {
  if (!s)
    return 0;
  for (DWORD i = 0; i < n; i++)
    s[i] = (WCHAR)u32_lower(s[i]);
  return n;
}
/* Code page 1252 is single byte: every character is one byte. */
char *W32API CharNextA(const char *s) {
  return (char *)(s && *s ? s + 1 : s);
}
char *W32API CharPrevA(const char *start, const char *s) {
  return (char *)(s > start ? s - 1 : start);
}
WCHAR *W32API CharNextW(const WCHAR *s) {
  return (WCHAR *)(s && *s ? s + 1 : s);
}
WCHAR *W32API CharPrevW(const WCHAR *start, const WCHAR *s) {
  return (WCHAR *)(s > start ? s - 1 : start);
}

/* ---- OEM (437) conversions ------------------------------------------ */
static const uint16_t cp437[128] = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7, 0x00ea,
    0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5, 0x00c9, 0x00e6,
    0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9, 0x00ff, 0x00d6, 0x00dc,
    0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192, 0x00e1, 0x00ed, 0x00f3, 0x00fa,
    0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc,
    0x00a1, 0x00ab, 0x00bb, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561,
    0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b,
    0x2510, 0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
    0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567, 0x2568,
    0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b, 0x256a, 0x2518,
    0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580, 0x03b1, 0x00df, 0x0393,
    0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4, 0x03a6, 0x0398, 0x03a9, 0x03b4,
    0x221e, 0x03c6, 0x03b5, 0x2229, 0x2261, 0x00b1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00f7, 0x2248, 0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2,
    0x25a0, 0x00a0};
static UINT oem_scalar(uint8_t c) { return c < 128 ? c : cp437[c - 128]; }
static uint8_t oem_byte(UINT c) {
  if (c < 128)
    return (uint8_t)c;
  for (UINT i = 0; i < 128; i++)
    if (cp437[i] == c)
      return (uint8_t)(128 + i);
  return '?';
}
BOOL W32API CharToOemBuffA(const char *s, char *d, DWORD n) {
  if (!s || !d)
    return u32_fail(87);
  for (DWORD i = 0; i < n; i++)
    d[i] = (char)oem_byte(u32_scalar((uint8_t)s[i]));
  return 1;
}
BOOL W32API CharToOemA(const char *s, char *d) {
  if (!s || !d)
    return u32_fail(87);
  DWORD i = 0;
  do
    d[i] = (char)oem_byte(u32_scalar((uint8_t)s[i]));
  while (s[i++]);
  return 1;
}
BOOL W32API OemToCharBuffA(const char *s, char *d, DWORD n) {
  if (!s || !d)
    return u32_fail(87);
  for (DWORD i = 0; i < n; i++)
    d[i] = (char)u32_ansi(oem_scalar((uint8_t)s[i]));
  return 1;
}
BOOL W32API OemToCharA(const char *s, char *d) {
  if (!s || !d)
    return u32_fail(87);
  DWORD i = 0;
  do
    d[i] = (char)u32_ansi(oem_scalar((uint8_t)s[i]));
  while (s[i++]);
  return 1;
}
BOOL W32API CharToOemBuffW(const WCHAR *s, char *d, DWORD n) {
  if (!s || !d)
    return u32_fail(87);
  for (DWORD i = 0; i < n; i++)
    d[i] = (char)oem_byte(s[i]);
  return 1;
}
BOOL W32API CharToOemW(const WCHAR *s, char *d) {
  if (!s || !d || (const void *)s == (const void *)d)
    return u32_fail(87);
  DWORD i = 0;
  do
    d[i] = (char)oem_byte(s[i]);
  while (s[i++]);
  return 1;
}
BOOL W32API OemToCharBuffW(const char *s, WCHAR *d, DWORD n) {
  if (!s || !d)
    return u32_fail(87);
  for (DWORD i = 0; i < n; i++)
    d[i] = (WCHAR)oem_scalar((uint8_t)s[i]);
  return 1;
}
BOOL W32API OemToCharW(const char *s, WCHAR *d) {
  if (!s || !d || (const void *)s == (const void *)d)
    return u32_fail(87);
  DWORD i = 0;
  do
    d[i] = (WCHAR)oem_scalar((uint8_t)s[i]);
  while (s[i++]);
  return 1;
}

/* ---- wsprintf ------------------------------------------------------- */
#define WSPRINTF_MAX 1024u
struct sink {
  void *out;
  BOOL wide;
  UINT n;
};
static void emit(struct sink *k, UINT c) {
  if (k->n + 1 >= WSPRINTF_MAX)
    return;
  if (k->wide)
    ((WCHAR *)k->out)[k->n++] = (WCHAR)c;
  else
    ((char *)k->out)[k->n++] = (char)(c < 256 ? c : (UINT)(uint8_t)u32_ansi(c));
}
static void pad(struct sink *k, UINT c, int n) {
  while (n-- > 0)
    emit(k, c);
}
/* 64-bit by small-divisor division in 32-bit steps: USER links no
 * compiler runtime for the i686 64-bit helpers. Returns the remainder. */
static UINT divide(uint64_t *v, UINT base) {
  uint32_t hi = (uint32_t)(*v >> 32), lo = (uint32_t)*v;
  uint32_t qh = hi / base, r = hi % base;
  uint32_t mid = (r << 16) | (lo >> 16), qm = mid / base;
  r = mid % base;
  uint32_t low = (r << 16) | (lo & 0xffffu), ql = low / base;
  r = low % base;
  *v = ((uint64_t)qh << 32) | ((uint64_t)qm << 16) | ql;
  return r;
}
static int format(void *out, BOOL wide, const void *fmt, va_list ap) {
  struct sink k = {out, wide, 0};
  UINT at = 0;
  static const char null_text[] = "(null)";
#define FMT(i) (wide ? ((const WCHAR *)fmt)[i] : (UINT)((const uint8_t *)fmt)[i])
  for (UINT c; (c = FMT(at)) != 0 && k.n + 1 < WSPRINTF_MAX; at++) {
    if (c != '%') {
      emit(&k, wide ? c : u32_scalar((uint8_t)c));
      continue;
    }
    at++;
    BOOL left = 0, alternate = 0, zero = 0;
    for (;; at++) {
      c = FMT(at);
      if (c == '-')
        left = 1;
      else if (c == '#')
        alternate = 1;
      else if (c == '0')
        zero = 1;
      else
        break;
    }
    int width = 0, precision = -1;
    while (digit(FMT(at)))
      width = width * 10 + (int)(FMT(at++) - '0');
    if (FMT(at) == '.') {
      at++;
      precision = 0;
      while (digit(FMT(at)))
        precision = precision * 10 + (int)(FMT(at++) - '0');
    }
    if (width > (int)WSPRINTF_MAX)
      width = WSPRINTF_MAX;
    if (precision > (int)WSPRINTF_MAX)
      precision = WSPRINTF_MAX;
    /* Size prefixes: h (short / ANSI), l (long / wide), I64. */
    int size = 0;
    if (FMT(at) == 'h') {
      size = 1;
      at++;
    } else if (FMT(at) == 'l' || FMT(at) == 'w') {
      size = 2;
      at++;
    } else if (FMT(at) == 'I' && FMT(at + 1) == '6' && FMT(at + 2) == '4') {
      size = 3;
      at += 3;
    } else if (FMT(at) == 'I' && (FMT(at + 1) == 'x' || FMT(at + 1) == 'X' ||
                                  FMT(at + 1) == 'd' || FMT(at + 1) == 'u')) {
      at++; /* pointer-sized: 32 bits on i386 */
    }
    c = FMT(at);
    if (!c)
      break;
    switch (c) {
    case 'c':
    case 'C': {
      UINT ch = (UINT)va_arg(ap, int);
      /* %c is the caller's own width; %C the other one; h/l force it. */
      BOOL arg_wide = size == 1 ? 0 : size == 2 ? 1 : (c == 'C') != !!wide;
      ch = arg_wide ? ch & 0xffff : u32_scalar((uint8_t)ch);
      if (!ch && size) /* %hc and %lc ignore a zero character */
        break;
      if (!left)
        pad(&k, ' ', width - 1);
      emit(&k, ch);
      if (left)
        pad(&k, ' ', width - 1);
      break;
    }
    case 's':
    case 'S': {
      const void *s = va_arg(ap, const void *);
      BOOL arg_wide = size == 1 ? 0 : size == 2 ? 1 : (c == 'S') != !!wide;
      if (!s) {
        s = null_text;
        arg_wide = 0;
      }
      int len = 0;
      while ((precision < 0 || len < precision) &&
             (arg_wide ? ((const WCHAR *)s)[len] : ((const char *)s)[len]))
        len++;
      if (!left)
        pad(&k, ' ', width - len);
      for (int i = 0; i < len; i++)
        emit(&k, arg_wide ? ((const WCHAR *)s)[i]
                          : u32_scalar(((const uint8_t *)s)[i]));
      if (left)
        pad(&k, ' ', width - len);
      break;
    }
    case 'd':
    case 'i':
    case 'u':
    case 'x':
    case 'X':
    case 'p': {
      uint64_t v;
      BOOL negative = 0;
      if (c == 'p') {
        v = (uintptr_t)va_arg(ap, void *);
        if (precision < 0)
          precision = (int)(sizeof(void *) * 2);
      } else if (size == 3)
        v = va_arg(ap, uint64_t);
      else
        v = va_arg(ap, unsigned int);
      if (size == 1 && c != 'p')
        v &= 0xffff;
      if (c == 'd' || c == 'i') {
        int64_t s = size == 3   ? (int64_t)v
                    : size == 1 ? (int64_t)(int16_t)v
                                : (int64_t)(int32_t)(uint32_t)v;
        if (s < 0) {
          negative = 1;
          v = (uint64_t)(-(s + 1)) + 1;
        } else
          v = (uint64_t)s;
      }
      char digits[24];
      int nd = 0;
      UINT base = c == 'x' || c == 'X' || c == 'p' ? 16 : 10;
      const char *set = c == 'X' || c == 'p' ? "0123456789ABCDEF"
                                             : "0123456789abcdef";
      do
        digits[nd++] = set[divide(&v, base)];
      while (v);
      int zeros = precision > nd ? precision - nd : 0;
      const char *prefix = alternate && base == 16 && c != 'p'
                               ? (c == 'X' ? "0X" : "0x")
                               : negative ? "-" : "";
      int prefix_len = (int)w32_strlen(prefix);
      int body = prefix_len + zeros + nd;
      if (zero && !left && precision < 0 && width > body) {
        zeros += width - body;
        body = width;
      }
      if (!left)
        pad(&k, ' ', width - body);
      for (int i = 0; i < prefix_len; i++)
        emit(&k, (uint8_t)prefix[i]);
      pad(&k, '0', zeros);
      while (nd)
        emit(&k, (uint8_t)digits[--nd]);
      if (left)
        pad(&k, ' ', width - body);
      break;
    }
    default:
      /* An unknown type is copied as is (%% gives %). */
      if (c == '%')
        emit(&k, '%');
      else
        emit(&k, wide ? c : u32_scalar((uint8_t)c));
      break;
    }
  }
#undef FMT
  if (wide)
    ((WCHAR *)out)[k.n] = 0;
  else
    ((char *)out)[k.n] = 0;
  return (int)k.n;
}
int W32API wvsprintfA(char *out, const char *fmt, va_list ap) {
  if (!out || !fmt)
    return -1;
  return format(out, 0, fmt, ap);
}
int W32API wvsprintfW(WCHAR *out, const WCHAR *fmt, va_list ap) {
  if (!out || !fmt)
    return -1;
  return format(out, 1, fmt, ap);
}
int wsprintfA(char *out, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = wvsprintfA(out, fmt, ap);
  va_end(ap);
  return n;
}
int wsprintfW(WCHAR *out, const WCHAR *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = wvsprintfW(out, fmt, ap);
  va_end(ap);
  return n;
}
