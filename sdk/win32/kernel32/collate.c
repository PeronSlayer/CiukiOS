/* f5-20B: CompareStringA/W and LCMapStringA/W for the en-US locale (and
 * the invariant locale): one collation over Unicode scalars, which the ANSI
 * forms reach through code page 1252, so both forms order the code page
 * repertoire identically (amendment 1, item 1).
 * Collation (Microsoft "Sorting" / "CompareString" / "LCMapString"
 * documentation): three levels, then the word-sort specials. Level 1 orders
 * symbols (including space) before digits before letters, letters by their
 * base letter (diacritics removed, AE/OE/SS/TH ligatures expanded); level 2
 * orders diacritics (none first); level 3 orders case (lowercase first).
 * The word sort (default) ignores hyphen and apostrophe until every level is
 * equal; SORT_STRINGSORT sorts them as symbols. NORM_IGNORECASE drops level
 * 3, NORM_IGNORENONSPACE level 2, NORM_IGNORESYMBOLS the symbols; the
 * kana/width flags change nothing for this repertoire. Characters outside
 * code page 1252 and Latin Extended-A sort by code point after the Latin
 * letters (Greek and Cyrillic case-folded). Control characters are
 * ignorable. LCMAP_SORTKEY builds the matching key: level-1 weights (two
 * bytes each), 01, level 2, 01, level 3, 01, specials, 00; comparing two
 * keys byte by byte gives the CompareString result.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
typedef uint32_t UINT;
extern DWORD W32API ConvertDefaultLocale(DWORD);
#define ERROR_INVALID_FLAGS 1004u
#define NORM_IGNORECASE 0x1u
#define NORM_IGNORENONSPACE 0x2u
#define NORM_IGNORESYMBOLS 0x4u
#define LINGUISTIC_IGNORECASE 0x10u
#define LINGUISTIC_IGNOREDIACRITIC 0x20u
#define SORT_STRINGSORT 0x1000u
#define NORM_IGNOREKANATYPE 0x10000u
#define NORM_IGNOREWIDTH 0x20000u
#define NORM_LINGUISTIC_CASING 0x08000000u
#define COMPARE_FLAGS                                                          \
  (NORM_IGNORECASE | NORM_IGNORENONSPACE | NORM_IGNORESYMBOLS |                \
   LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC | SORT_STRINGSORT |      \
   NORM_IGNOREKANATYPE | NORM_IGNOREWIDTH | NORM_LINGUISTIC_CASING)
#define LCMAP_LOWERCASE 0x100u
#define LCMAP_UPPERCASE 0x200u
#define LCMAP_SORTKEY 0x400u
#define LCMAP_BYTEREV 0x800u
#define LCMAP_HIRAGANA 0x100000u
#define LCMAP_KATAKANA 0x200000u
#define LCMAP_HALFWIDTH 0x400000u
#define LCMAP_FULLWIDTH 0x800000u
#define LCMAP_LINGUISTIC_CASING 0x1000000u
#define LCMAP_FLAGS                                                            \
  (LCMAP_LOWERCASE | LCMAP_UPPERCASE | LCMAP_SORTKEY | LCMAP_BYTEREV |         \
   LCMAP_HIRAGANA | LCMAP_KATAKANA | LCMAP_HALFWIDTH | LCMAP_FULLWIDTH |       \
   LCMAP_LINGUISTIC_CASING | NORM_IGNORECASE | NORM_IGNORENONSPACE |           \
   NORM_IGNORESYMBOLS | SORT_STRINGSORT | NORM_IGNOREKANATYPE |                \
   NORM_IGNOREWIDTH | LINGUISTIC_IGNORECASE | LINGUISTIC_IGNOREDIACRITIC)

/* ---- character model -------------------------------------------------- */
enum { IGNORE, SPECIAL, SYMBOL, DIGIT, LETTER };
struct element {
  uint8_t kind, diacritic, upper, count;
  uint16_t primary[2];
};
/* Latin-1 letters 0xC0-0xFF: base letter and diacritic (1 acute, 2 grave,
 * 3 circumflex, 4 tilde, 5 diaeresis, 6 ring, 7 cedilla, 8 stroke,
 * 9 caron, 10 other). 0 marks the non-letters 0xD7 and 0xF7. */
static const char latin1_base[] = "AAAAAAACEEEEIIIIDNOOOOO0OUUUUYTSaaaaaaaceeeeiiiidnooooo0ouuuuyty";
static const uint8_t latin1_mark[] = {2, 1, 3, 4, 5, 6, 0, 7, 2, 1, 3, 5, 2, 1, 3, 5,
                                      10, 4, 2, 1, 3, 4, 5, 0, 8, 2, 1, 3, 5, 1, 0, 0,
                                      2, 1, 3, 4, 5, 6, 0, 7, 2, 1, 3, 5, 2, 1, 3, 5,
                                      10, 4, 2, 1, 3, 4, 5, 0, 8, 2, 1, 3, 5, 1, 0, 5};
/* Latin Extended-A base letters (0x100-0x17F, case pairs). */
static const char latin_a[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIi"
                              "\x01\x01JjKkkLlLlLlLlLlNnNnNnnNnOoOoOo\x02\x02RrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
static int letter_primary(UINT base) { return 0x4000 + (int)(base | 32) * 4; }
static void classify(UINT c, BOOL string_sort, struct element *e) {
  *e = (struct element){IGNORE, 0, 0, 0, {0, 0}};
  if (c < 0x20 || (c >= 0x7f && c < 0xa0) || c == 0xad)
    return;
  if (!string_sort && (c == '-' || c == '\'')) {
    e->kind = SPECIAL;
    e->count = 1;
    e->primary[0] = (uint16_t)c;
    return;
  }
  if (c >= '0' && c <= '9') {
    *e = (struct element){DIGIT, 0, 0, 1, {(uint16_t)(0x3000 + c), 0}};
    return;
  }
  if (c == 0xb2 || c == 0xb3 || c == 0xb9) { /* superscript digits */
    UINT d = c == 0xb9 ? '1' : c == 0xb2 ? '2' : '3';
    *e = (struct element){DIGIT, 10, 0, 1, {(uint16_t)(0x3000 + d), 0}};
    return;
  }
  UINT base = 0, mark = 0, upper = 0, second = 0;
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
    base = c;
    upper = c <= 'Z';
  } else if (c >= 0xc0 && c <= 0xff && latin1_base[c - 0xc0] != '0') {
    base = (uint8_t)latin1_base[c - 0xc0];
    mark = latin1_mark[c - 0xc0];
    upper = c < 0xdf;
    if (c == 0xc6 || c == 0xe6) /* AE */
      second = 'e', mark = 0;
    else if (c == 0xdf) /* sharp s: ss */
      base = 's', second = 's', upper = 0, mark = 0;
    else if (c == 0xde || c == 0xfe) /* thorn: th */
      second = 'h', mark = 0;
  } else if (c >= 0x100 && c <= 0x17f) {
    UINT b = (uint8_t)latin_a[c - 0x100];
    upper = b >= 'A' && b <= 'Z';
    if (b == 1) /* IJ */
      b = c == 0x132 ? 'I' : 'i', second = 'j', upper = c == 0x132;
    else if (b == 2) /* OE */
      b = c == 0x152 ? 'O' : 'o', second = 'e', upper = c == 0x152;
    base = b;
    mark = c == 0x178 ? 5 : 10; /* Y diaeresis pairs with Latin-1 y diaeresis */
  } else if (c == 0x192) { /* f with hook (1252: 0x83) */
    base = 'f', mark = 10;
  } else if (c == 0xaa || c == 0xba) { /* ordinal indicators */
    base = c == 0xaa ? 'a' : 'o', mark = 10;
  } else if (c == 0xb5) { /* micro sign: Greek mu */
    *e = (struct element){LETTER, 0, 0, 1, {(uint16_t)(0x6000 + 0x3bc), 0}};
    return;
  }
  if (base) {
    e->kind = LETTER;
    e->diacritic = (uint8_t)mark;
    e->upper = (uint8_t)upper;
    e->count = second ? 2 : 1;
    e->primary[0] = (uint16_t)letter_primary(base);
    e->primary[1] = (uint16_t)(second ? letter_primary(second) : 0);
    return;
  }
  if ((c >= 0x391 && c <= 0x3a9) || (c >= 0x410 && c <= 0x42f)) {
    *e = (struct element){LETTER, 0, 1, 1, {(uint16_t)(0x6000 + c + 32), 0}};
    return;
  }
  if (c >= 0x400 && c <= 0x40f) {
    *e = (struct element){LETTER, 0, 1, 1, {(uint16_t)(0x6000 + c + 80), 0}};
    return;
  }
  if ((c >= 0x370 && c <= 0x3ff) || (c >= 0x430 && c <= 0x4ff)) {
    *e = (struct element){LETTER, 0, 0, 1, {(uint16_t)(0x6000 + c), 0}};
    return;
  }
  if (c < 0x100 || (c >= 0x2000 && c <= 0x2bff)) {
    /* Space, punctuation and symbols in code-point order. */
    *e = (struct element){SYMBOL, 0, 0, 1, {(uint16_t)(0x200 + (c < 0x100 ? c : 0x100 + (c & 0x1fff) / 2)), 0}};
    return;
  }
  /* Everything else after the Latin letters, by code point. */
  e->kind = LETTER;
  e->count = 1;
  e->primary[0] = (uint16_t)(c < 0x9fff ? 0x6000 + c : 0xffff);
}

/* ---- keys ------------------------------------------------------------- */
struct key {
  uint8_t *p;
  DWORD n, cap;
};
static void put(struct key *k, uint8_t b) {
  if (k->n < k->cap)
    k->p[k->n] = b;
  k->n++;
}
/* The sort key of `count` scalars; a NULL buffer measures. */
static DWORD sort_key(const WCHAR *w, const char *a, int count, DWORD flags,
                      uint8_t *out, DWORD cap) {
  BOOL ignore_case = !!(flags & (NORM_IGNORECASE | LINGUISTIC_IGNORECASE)),
       ignore_marks =
           !!(flags & (NORM_IGNORENONSPACE | LINGUISTIC_IGNOREDIACRITIC)),
       ignore_symbols = !!(flags & NORM_IGNORESYMBOLS),
       string_sort = !!(flags & SORT_STRINGSORT);
  struct key k = {out, 0, out ? cap : 0};
  for (int level = 0; level < 4; level++) {
    UINT position = 0;
    for (int i = 0; i < count; i++) {
      UINT c = w ? w[i] : 0;
      if (!w) {
        WCHAR x;
        MultiByteToWideChar(1252, 0, a + i, 1, &x, 1);
        c = x;
      }
      struct element e;
      classify(c, string_sort, &e);
      if (e.kind == IGNORE || (ignore_symbols && e.kind == SYMBOL))
        continue;
      if (e.kind == SPECIAL) {
        if (level == 3 && !ignore_symbols) {
          put(&k, (uint8_t)(2 + (position > 250 ? 250 : position)));
          put(&k, (uint8_t)(2 + (e.primary[0] == '\'' ? 0 : 1)));
        }
        continue;
      }
      for (UINT j = 0; j < e.count; j++, position++) {
        if (level == 0) {
          put(&k, (uint8_t)(e.primary[j] >> 8));
          put(&k, (uint8_t)e.primary[j]);
        } else if (level == 1 && !ignore_marks)
          put(&k, (uint8_t)(2 + e.diacritic));
        else if (level == 2 && !ignore_case)
          put(&k, (uint8_t)(2 + e.upper));
      }
    }
    put(&k, level < 3 ? 1 : 0);
  }
  return k.n;
}
static BOOL valid_locale(DWORD lcid) {
  DWORD l = ConvertDefaultLocale(lcid) & 0xffffu;
  if (l == 0x0409u || l == 0x0410u || l == 0x007fu)
    return 1;
  SetLastError(87);
  return 0;
}
static int string_length(const void *s, int n, BOOL wide) {
  if (n >= 0)
    return n;
  int k = 0;
  if (wide)
    while (((const WCHAR *)s)[k])
      k++;
  else
    while (((const char *)s)[k])
      k++;
  return k;
}
static int compare(DWORD lcid, DWORD flags, const void *a, int n, const void *b,
                   int m, BOOL wide) {
  if (!valid_locale(lcid))
    return 0;
  if (!a || !b) {
    SetLastError(87);
    return 0;
  }
  if (flags & ~COMPARE_FLAGS) {
    SetLastError(ERROR_INVALID_FLAGS);
    return 0;
  }
  n = string_length(a, n, wide);
  m = string_length(b, m, wide);
  DWORD ka = sort_key(wide ? a : 0, wide ? 0 : a, n, flags, 0, 0),
        kb = sort_key(wide ? b : 0, wide ? 0 : b, m, flags, 0, 0);
  uint8_t *x = k32_alloc(ka + kb);
  if (!x) {
    SetLastError(8);
    return 0;
  }
  sort_key(wide ? a : 0, wide ? 0 : a, n, flags, x, ka);
  sort_key(wide ? b : 0, wide ? 0 : b, m, flags, x + ka, kb);
  int r = 2;
  for (DWORD i = 0; i < ka && i < kb && r == 2; i++)
    if (x[i] != x[ka + i])
      r = x[i] < x[ka + i] ? 1 : 3;
  if (r == 2 && ka != kb)
    r = ka < kb ? 1 : 3;
  k32_free(x, ka + kb);
  return r;
}
int W32API CompareStringW(DWORD lcid, DWORD flags, const WCHAR *a, int n,
                          const WCHAR *b, int m) {
  return compare(lcid, flags, a, n, b, m, 1);
}
int W32API CompareStringA(DWORD lcid, DWORD flags, const char *a, int n,
                          const char *b, int m) {
  return compare(lcid, flags, a, n, b, m, 0);
}

/* ---- LCMapString ------------------------------------------------------ */
/* Latin Extended-A pairs: the lowercase letter follows its capital. */
static UINT upper_case(UINT c) {
  if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7))
    return c - 32;
  if (c == 0xff)
    return 0x178;
  if (c >= 0x100 && c <= 0x17f) {
    if (c == 0x131)
      return 'I';
    if (c == 0x17f)
      return 'S';
    if (c == 0x138 || c == 0x149)
      return c;
    struct element e;
    classify(c, 1, &e);
    return e.upper ? c : c - 1;
  }
  if ((c >= 0x3b1 && c <= 0x3c9 && c != 0x3c2) || (c >= 0x430 && c <= 0x44f))
    return c - 32;
  if (c >= 0x450 && c <= 0x45f)
    return c - 80;
  return c;
}
static UINT lower_case(UINT c) {
  if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7))
    return c + 32;
  if (c == 0x178)
    return 0xff;
  if (c >= 0x100 && c <= 0x17f) {
    if (c == 0x130)
      return 'i';
    struct element e;
    classify(c, 1, &e);
    return e.upper ? c + 1 : c;
  }
  if ((c >= 0x391 && c <= 0x3a9 && c != 0x3a2) || (c >= 0x410 && c <= 0x42f))
    return c + 32;
  if (c >= 0x400 && c <= 0x40f)
    return c + 80;
  return c;
}
/* NORM_IGNORENONSPACE on text: the base letter of a single accented
 * letter; NORM_IGNORESYMBOLS: symbols removed. */
static int text_filter(UINT c, DWORD flags, UINT *out) {
  struct element e;
  classify(c, 1, &e);
  if ((flags & NORM_IGNORESYMBOLS) && e.kind == SYMBOL)
    return 0;
  if ((flags & (NORM_IGNORENONSPACE | LINGUISTIC_IGNOREDIACRITIC)) &&
      e.kind == LETTER && e.diacritic && e.count == 1 &&
      e.primary[0] >= 0x4000 && e.primary[0] < 0x6000 &&
      e.diacritic != 10) {
    UINT base = (UINT)(e.primary[0] - 0x4000) / 4;
    c = e.upper ? base - 32 : base;
  }
  *out = c;
  return 1;
}
static int map(DWORD lcid, DWORD flags, const void *src, int n, void *dst,
               int cap, BOOL wide) {
  if (!valid_locale(lcid))
    return 0;
  if (!src || cap < 0 || (cap && !dst) || n == 0 || n < -1) {
    SetLastError(87);
    return 0;
  }
  if ((flags & ~LCMAP_FLAGS) ||
      (flags & (LCMAP_LOWERCASE | LCMAP_UPPERCASE)) ==
          (LCMAP_LOWERCASE | LCMAP_UPPERCASE) ||
      ((flags & LCMAP_SORTKEY) &&
       (flags & (LCMAP_LOWERCASE | LCMAP_UPPERCASE | LCMAP_BYTEREV))) ||
      (!wide && (flags & LCMAP_BYTEREV)) ||
      (!(flags & LCMAP_SORTKEY) && (flags & SORT_STRINGSORT))) {
    SetLastError(ERROR_INVALID_FLAGS);
    return 0;
  }
  BOOL terminated = n == -1;
  n = string_length(src, n, wide);
  if (flags & LCMAP_SORTKEY) {
    /* The key is bytes for both forms; cap counts bytes. */
    DWORD size = sort_key(wide ? src : 0, wide ? 0 : src, n, flags, 0, 0);
    if (!cap)
      return (int)size;
    if ((DWORD)cap < size) {
      SetLastError(122);
      return 0;
    }
    if ((const void *)src == dst) {
      SetLastError(87);
      return 0;
    }
    sort_key(wide ? src : 0, wide ? 0 : src, n, flags, dst, size);
    return (int)size;
  }
  int total = n + (terminated ? 1 : 0), written = 0;
  for (int pass = 0; pass < 2; pass++) {
    written = 0;
    for (int i = 0; i < total; i++) {
      UINT c;
      if (wide)
        c = ((const WCHAR *)src)[i];
      else {
        WCHAR x;
        MultiByteToWideChar(1252, 0, (const char *)src + i, 1, &x, 1);
        c = x;
      }
      if (i < n && !text_filter(c, flags, &c))
        continue;
      if (flags & LCMAP_UPPERCASE)
        c = upper_case(c);
      else if (flags & LCMAP_LOWERCASE)
        c = lower_case(c);
      if (flags & LCMAP_BYTEREV)
        c = ((c & 0xff) << 8) | ((c >> 8) & 0xff);
      if (pass) {
        if (wide)
          ((WCHAR *)dst)[written] = (WCHAR)c;
        else {
          WCHAR x = (WCHAR)c;
          char b = '?';
          WideCharToMultiByte(1252, 0, &x, 1, &b, 1, 0, 0);
          ((char *)dst)[written] = b;
        }
      }
      written++;
    }
    if (!cap)
      return written;
    if (written > cap) {
      SetLastError(122);
      return 0;
    }
  }
  return written;
}
int W32API LCMapStringW(DWORD lcid, DWORD flags, const WCHAR *src, int n,
                        WCHAR *dst, int cap) {
  return map(lcid, flags, src, n, dst, cap, 1);
}
int W32API LCMapStringA(DWORD lcid, DWORD flags, const char *src, int n,
                        char *dst, int cap) {
  return map(lcid, flags, src, n, dst, cap, 0);
}
