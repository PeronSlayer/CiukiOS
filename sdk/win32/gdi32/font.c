/* Win32 font selection, metrics, enumeration and font resources over the
 * f5-10B text boundary. Candidate penalties use the published font-mapper
 * weights (R. Gery, "Windows Font Mapping", MSDN 1992); the alias table and
 * the Windows 98 dialog-font plan are documented in
 * docs/design/fonts-contract.md, section "Win32 font selection".
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"

#ifndef W32_HOST_MODEL
#define IMPORT __declspec(dllimport)
#else
#define IMPORT extern
#endif
IMPORT void *W32API CreateFileA(const char *, DWORD, DWORD, void *, DWORD,
                                DWORD, void *);
IMPORT BOOL W32API ReadFile(void *, void *, DWORD, DWORD *, void *);
IMPORT DWORD W32API GetFileSize(void *, DWORD *);
IMPORT DWORD W32API SetFilePointer(void *, LONG, LONG *, DWORD);
IMPORT BOOL W32API CloseHandle(void *);
IMPORT BOOL W32API CopyFileA(const char *, const char *, BOOL);
IMPORT BOOL W32API DeleteFileA(const char *);
IMPORT BOOL W32API WriteFile(void *, const void *, DWORD, DWORD *, void *);
IMPORT void *W32API LoadLibraryA(const char *);
IMPORT void *W32API GetProcAddress(void *, const char *);
IMPORT void W32API SetLastError(DWORD);

#define INVALID_FILE ((void *)(intptr_t)-1)
#define FACES_MAX 64u
#define FILE_MAX (16u << 20)
#define PATH_MAX_W32 260u
#define BUDGET (2u << 20)
#define GLYPH_BUDGET (256u << 10)
#define GLYPH_SLOTS 1024u
#define KERN_MAX 4096u
#define EM_MAX (128u * 64u)

/* Published mapper weights (Gery 1992, table 1). */
enum {
  P_CHARSET = 65000,
  P_FIXED = 15000,
  P_FACE = 10000,
  P_FAMILY = 9000,
  P_FAMILY_UNKNOWN = 8000,
  P_SUBST = 500,
  P_VARIABLE = 350,
  P_UNLIKELY = 50,
  P_ITALIC = 4,
  P_WEIGHT = 3,
  P_ITALIC_SIM = 1,
  P_DEFAULT_FIXED = 1
};

struct kern {
  uint8_t left, right;
  int16_t value;
};
struct face {
  char win32[PATH_MAX_W32], native[PATH_MAX_W32], origin[PATH_MAX_W32];
  char family[LF_FACESIZE], style[32], full[64];
  unsigned upem, weight, num_glyphs, num_hmetrics, id;
  int italic, fixed, symbol, installed, resources, copied, remove_pending;
  uint8_t pitch_family, panose[10];
  int hhea_ascent, hhea_descent, hhea_gap, typo_ascent, typo_descent,
      typo_gap, caret_rise, caret_run;
  unsigned win_ascent, win_descent, max_advance, min_ppem;
  int avg_width, x_height, cap_height, underline_pos, underline_size,
      strike_size, strike_pos, sub[4], sup[4], bbox[4], italic_angle;
  unsigned fs_selection, fs_type, first_char, last_char, ranges[6];
  uint16_t glyph[256], advance[256];
  int16_t lsb[256];
  struct kern *kerns;
  unsigned kern_count;
  DWORD file_size;
  void *native_face;
  DWORD resident, last_use, retry_wait;
};

/* Virtual families: the Windows 98 core names backed by Liberation faces. */
enum { PLAN_NONE, PLAN_RASTER, PLAN_SYSTEM, PLAN_FIXEDSYS, PLAN_TERMINAL };
struct alias {
  const char *name, *physical;
  uint8_t pitch_family, charset, plan, bold_only;
};
static const struct alias aliases[] = {
    {"Arial", "Liberation Sans", FF_SWISS | VARIABLE_PITCH, ANSI_CHARSET,
     PLAN_NONE, 0},
    {"Times New Roman", "Liberation Serif", FF_ROMAN | VARIABLE_PITCH,
     ANSI_CHARSET, PLAN_NONE, 0},
    {"Courier New", "Liberation Mono", FF_MODERN | FIXED_PITCH, ANSI_CHARSET,
     PLAN_NONE, 0},
    {"MS Sans Serif", "Liberation Sans", FF_SWISS | VARIABLE_PITCH,
     ANSI_CHARSET, PLAN_RASTER, 0},
    {"Tahoma", "Liberation Sans", FF_SWISS | VARIABLE_PITCH, ANSI_CHARSET,
     PLAN_RASTER, 0},
    {"MS Serif", "Liberation Serif", FF_ROMAN | VARIABLE_PITCH, ANSI_CHARSET,
     PLAN_RASTER, 0},
    {"System", "Liberation Sans", FF_SWISS | VARIABLE_PITCH, ANSI_CHARSET,
     PLAN_SYSTEM, 1},
    {"Courier", "Liberation Mono", FF_MODERN | FIXED_PITCH, ANSI_CHARSET,
     PLAN_RASTER, 0},
    {"Fixedsys", "Liberation Mono", FF_MODERN | FIXED_PITCH, ANSI_CHARSET,
     PLAN_FIXEDSYS, 0},
    {"Terminal", "Liberation Mono", FF_MODERN | FIXED_PITCH, OEM_CHARSET,
     PLAN_TERMINAL, 0},
    {"Small Fonts", "Liberation Sans", FF_SWISS | VARIABLE_PITCH,
     ANSI_CHARSET, PLAN_RASTER, 0},
};
#define ALIASES (sizeof(aliases) / sizeof(*aliases))
/* GDI's built-in substitutions; WIN.INI [FontSubstitutes] and the registry
 * FontSubstitutes key are consulted first. */
static const char *const substitutes[][2] = {
    {"Helv", "MS Sans Serif"},     {"Tms Rmn", "MS Serif"},
    {"MS Shell Dlg", "MS Sans Serif"}, {"MS Shell Dlg 2", "Tahoma"},
    {"Helvetica", "Arial"},        {"Times", "Times New Roman"},
    {"Courier 10 Pitch", "Courier New"},
};
static const char *const system_files[] = {
    "LiberationSans-Regular.ttf",    "LiberationSans-Bold.ttf",
    "LiberationSans-Italic.ttf",     "LiberationSans-BoldItalic.ttf",
    "LiberationSerif-Regular.ttf",   "LiberationSerif-Bold.ttf",
    "LiberationSerif-Italic.ttf",    "LiberationSerif-BoldItalic.ttf",
    "LiberationMono-Regular.ttf",    "LiberationMono-Bold.ttf",
    "LiberationMono-Italic.ttf",     "LiberationMono-BoldItalic.ttf"};
static const uint16_t cp1252[32] = {
    0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021,
    0x2c6,  0x2030, 0x160,  0x2039, 0x152,  0x8d,   0x17d,  0x8f,
    0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};

struct realized {
  struct realized *next;
  unsigned refs, serial;
  int stock, resolved, failed;
  LOGFONTA lf;
  char name[LF_FACESIZE];
  const struct alias *alias;
  struct face *face;
  unsigned em26;
  int ascent, descent, internal, external, average, maximum, weight, italic;
  int bold_synth, italic_synth, flags;
  uint8_t charset, pitch_family;
};
enum { G_BOLD = 1, G_ITALIC = 2, G_UNDERLINE = 4, G_STRIKE = 8, G_MONO = 16 };
struct gentry {
  unsigned face, em26, flags, glyph, stamp, pins;
  int used, next;
  struct w32_gdi_glyph g;
  DWORD bytes;
};

static struct {
  int loaded;
  struct face *faces[FACES_MAX];
  unsigned count, next_id, clock, serial;
  char system_win32[PATH_MAX_W32], system_native[PATH_MAX_W32],
      install_win32[PATH_MAX_W32], install_native[PATH_MAX_W32];
  const struct w32_gdi_face_backend *backend;
  struct gentry *cache;
  int heads[256];
  struct realized *live;
  struct w32_gdi_font_stats stats;
  DWORD mapper_flags;
#ifdef W32_HOST_MODEL
} S = {.system_win32 = "C:\\WINDOWS\\FONTS",
       .system_native = "/System/fonts",
       .install_win32 = "C:\\WINDOWS\\FONTS",
       .install_native = "/System/fonts"};
#else
/* f5-10C2: the guest personality draws glyphs through the PE-to-native
 * bridge (bridge.c). System faces are read through C:\System\fonts (the
 * native /System/fonts, read-only for Win32 processes) until the drive
 * service presents the union view at C:\WINDOWS\FONTS; AddFontResource
 * copies installed faces to C:\WINDOWS\FONTS (/WINDOWS/FONTS). */
} S = {.system_win32 = "C:\\System\\fonts",
       .system_native = "/System/fonts",
       .install_win32 = "C:\\WINDOWS\\FONTS",
       .install_native = "/WINDOWS/FONTS",
       .backend = &gdi_bridge_backend};
#endif
static struct realized stock_fonts[7];
static const unsigned stock_index[7] = {10, 11, 12, 13, 14, 16, 17};

/* ---- small helpers ---------------------------------------------------- */
static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) {
  return (uint32_t)be16(p) << 16 | be16(p + 2);
}
static int s16(const uint8_t *p) {
  unsigned v = be16(p);
  return v < 32768 ? (int)v : (int)v - 65536;
}
static int span(DWORD total, DWORD offset, DWORD bytes) {
  return offset <= total && bytes <= total - offset;
}
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int same(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  while (*a && lower((uint8_t)*a) == lower((uint8_t)*b))
    a++, b++;
  return !*a && !*b;
}
static void copy(char *out, unsigned cap, const char *s) {
  unsigned n = 0;
  while (s && s[n] && n + 1 < cap) {
    out[n] = s[n];
    n++;
  }
  out[n] = 0;
}
static int w32_memcmp_fot(const uint8_t *p) {
  for (unsigned i = 0; i < 8; i++)
    if (p[i] != (uint8_t)"CIUKIFOT"[i])
      return 1;
  return 0;
}
static void append(char *out, unsigned cap, const char *s) {
  unsigned n = w32_strlen(out);
  copy(out + n, cap > n ? cap - n : 0, s);
}
static DWORD unicode_of(unsigned c) {
  return c >= 128 && c < 160 ? cp1252[c - 128] : c;
}
static int ansi_of(DWORD u) {
  if (u < 128 || (u >= 160 && u < 256))
    return (int)u;
  for (unsigned i = 0; i < 32; i++)
    if (cp1252[i] == u)
      return (int)(128 + i);
  return -1;
}
DWORD gdi_cp1252_unicode(unsigned c) { return unicode_of(c & 0xff); }
int gdi_cp1252_ansi(DWORD u) { return ansi_of(u); }
/* Rounded and outward-rounded scaling of design units to whole pixels. */
static int scale(int value, unsigned em26, unsigned upem) {
  int64_t v = (int64_t)value * em26, d = (int64_t)upem * 64;
  return (int)(v >= 0 ? (v + d / 2) / d : -((-v + d / 2) / d));
}
/* Scaled to 26.6 (rounded, as the rasteriser's size metrics), then rounded
 * outward to whole pixels. */
static int scale_up(int value, unsigned em26, unsigned upem) {
  int64_t v = (int64_t)value * em26;
  int64_t v26 = v >= 0 ? (v + upem / 2) / upem : -((-v + upem / 2) / upem);
  return (int)(v26 >= 0 ? (v26 + 63) / 64 : -(-v26 / 64));
}

/* ---- file access through the drive-path service ------------------------ */
static uint8_t *read_file(const char *path, DWORD *size) {
  void *h = CreateFileA(path, 0x80000000u, 1, 0, 3, 0, 0);
  if (!h || h == INVALID_FILE)
    return 0;
  DWORD high = 0, n = GetFileSize(h, &high);
  if (n == 0xffffffffu || high || n < 12 || n > FILE_MAX) {
    CloseHandle(h);
    return 0;
  }
  uint8_t *data = gdi_alloc(n);
  DWORD at = 0;
  while (data && at < n) {
    DWORD got = 0;
    if (!ReadFile(h, data + at, n - at, &got, 0) || !got)
      break;
    at += got;
  }
  CloseHandle(h);
  if (data && at != n) {
    gdi_free(data, n);
    data = 0;
  }
  if (data)
    *size = n;
  return data;
}
static DWORD read_range(const char *path, DWORD offset, void *out,
                        DWORD bytes) {
  void *h = CreateFileA(path, 0x80000000u, 1, 0, 3, 0, 0);
  if (!h || h == INVALID_FILE)
    return GDI_ERROR;
  DWORD result = GDI_ERROR;
  if (SetFilePointer(h, (LONG)offset, 0, 0) == offset) {
    DWORD at = 0;
    while (at < bytes) {
      DWORD got = 0;
      if (!ReadFile(h, (uint8_t *)out + at, bytes - at, &got, 0) || !got)
        break;
      at += got;
    }
    if (at == bytes)
      result = bytes;
  }
  CloseHandle(h);
  return result;
}

/* ---- registry and WIN.INI, resolved lazily (advapi32 may be absent) ---- */
#define HKLM ((void *)(uintptr_t)0x80000002u)
typedef LONG(W32API *reg_open_t)(void *, const char *, DWORD, DWORD, void **);
typedef LONG(W32API *reg_create_t)(void *, const char *, DWORD, char *, DWORD,
                                   DWORD, void *, void **, DWORD *);
typedef LONG(W32API *reg_query_t)(void *, const char *, DWORD *, DWORD *,
                                  uint8_t *, DWORD *);
typedef LONG(W32API *reg_set_t)(void *, const char *, DWORD, DWORD,
                                const uint8_t *, DWORD);
typedef LONG(W32API *reg_enum_t)(void *, DWORD, char *, DWORD *, DWORD *,
                                 DWORD *, uint8_t *, DWORD *);
typedef LONG(W32API *reg_delete_t)(void *, const char *);
typedef LONG(W32API *reg_close_t)(void *);
typedef DWORD(W32API *profile_t)(const char *, const char *, const char *,
                                 char *, DWORD);
static struct {
  int tried;
  reg_open_t open;
  reg_create_t create;
  reg_query_t query;
  reg_set_t set;
  reg_enum_t enumerate;
  reg_delete_t remove;
  reg_close_t close;
  profile_t profile;
} R;
static const char FONTS_KEY[] = "Software\\Microsoft\\Windows\\CurrentVersion\\Fonts";
static const char ORIGIN_KEY[] = "Software\\CiukiOS\\Win32\\Fonts";
static const char SUBST_KEY[] =
    "Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes";
static void registry(void) {
  if (R.tried)
    return;
  R.tried = 1;
  void *a = LoadLibraryA("advapi32.dll");
  if (a) {
    *(void **)&R.open = GetProcAddress(a, "RegOpenKeyExA");
    *(void **)&R.create = GetProcAddress(a, "RegCreateKeyExA");
    *(void **)&R.query = GetProcAddress(a, "RegQueryValueExA");
    *(void **)&R.set = GetProcAddress(a, "RegSetValueExA");
    *(void **)&R.enumerate = GetProcAddress(a, "RegEnumValueA");
    *(void **)&R.remove = GetProcAddress(a, "RegDeleteValueA");
    *(void **)&R.close = GetProcAddress(a, "RegCloseKey");
  }
  void *k = LoadLibraryA("kernel32.dll");
  if (k)
    *(void **)&R.profile = GetProcAddress(k, "GetProfileStringA");
}
static void *key_open(const char *path, int create) {
  registry();
  void *key = 0;
  if (create && R.create) {
    DWORD disposition;
    if (R.create(HKLM, path, 0, 0, 0, 0x2001fu, 0, &key, &disposition))
      return 0;
    return key;
  }
  if (!R.open || R.open(HKLM, path, 0, 0x20019u, &key))
    return 0;
  return key;
}
static void key_close(void *key) {
  if (key && R.close)
    R.close(key);
}
static int key_string(const char *path, const char *name, char *out,
                      DWORD cap) {
  void *key = key_open(path, 0);
  DWORD type = 0, n = cap - 1;
  int ok = key && R.query &&
           !R.query(key, name, 0, &type, (uint8_t *)out, &n) && type == 1;
  key_close(key);
  if (ok)
    out[n < cap ? n : cap - 1] = 0;
  return ok && out[0];
}
static void key_put(const char *path, const char *name, const char *value) {
  void *key = key_open(path, 1);
  if (key && R.set)
    R.set(key, name, 0, 1, (const uint8_t *)value, w32_strlen(value) + 1);
  key_close(key);
}
static void key_delete(const char *path, const char *name) {
  void *key = key_open(path, 1);
  if (key && R.remove)
    R.remove(key, name);
  key_close(key);
}

/* ---- validated TrueType parser ---------------------------------------- */
static const uint8_t *table(const uint8_t *d, DWORD n, const char *tag,
                            DWORD minimum, DWORD *length) {
  unsigned count = be16(d + 4);
  for (unsigned i = 0; i < count; i++) {
    const uint8_t *r = d + 12 + 16 * i;
    if (r[0] == (uint8_t)tag[0] && r[1] == (uint8_t)tag[1] &&
        r[2] == (uint8_t)tag[2] && r[3] == (uint8_t)tag[3]) {
      DWORD offset = be32(r + 8), bytes = be32(r + 12);
      if (!span(n, offset, bytes) || bytes < minimum)
        return 0;
      if (length)
        *length = bytes;
      return d + offset;
    }
  }
  return 0;
}
static unsigned cmap_glyph(const uint8_t *sub, DWORD bytes, unsigned c) {
  unsigned segs = be16(sub + 6) / 2;
  if (!segs || 16 + 8u * segs > bytes)
    return 0;
  const uint8_t *ends = sub + 14, *starts = ends + 2 * segs + 2,
                *deltas = starts + 2 * segs, *offsets = deltas + 2 * segs;
  for (unsigned i = 0; i < segs; i++) {
    unsigned end = be16(ends + 2 * i), start = be16(starts + 2 * i);
    if (c > end)
      continue;
    if (c < start)
      return 0;
    unsigned delta = be16(deltas + 2 * i), range = be16(offsets + 2 * i);
    if (!range)
      return (c + delta) & 0xffffu;
    DWORD at = (DWORD)(offsets + 2 * i - sub) + range + 2 * (c - start);
    if (!span(bytes, at, 2))
      return 0;
    unsigned g = be16(sub + at);
    return g ? (g + delta) & 0xffffu : 0;
  }
  return 0;
}
static void name_string(const uint8_t *t, DWORD bytes, unsigned id, char *out,
                        unsigned cap) {
  unsigned count = be16(t + 2), base = be16(t + 4);
  int best = -1, score = 0;
  out[0] = 0;
  if (6 + 12u * count > bytes)
    return;
  for (unsigned i = 0; i < count; i++) {
    const uint8_t *r = t + 6 + 12 * i;
    unsigned platform = be16(r), encoding = be16(r + 2),
             language = be16(r + 4), nid = be16(r + 6);
    if (nid != id)
      continue;
    int s = platform == 3 && (encoding == 1 || encoding == 0)
                ? (language == 0x409 ? 4 : 3)
            : platform == 1 && encoding == 0 ? 2
                                             : 0;
    if (s > score) {
      score = s;
      best = (int)i;
    }
  }
  if (best < 0)
    return;
  const uint8_t *r = t + 6 + 12 * (unsigned)best;
  unsigned length = be16(r + 8), offset = be16(r + 10), n = 0;
  if (!span(bytes, base + offset, length))
    return;
  const uint8_t *s = t + base + offset;
  if (be16(r) == 3)
    for (unsigned i = 0; i + 1 < length && n + 1 < cap; i += 2) {
      int c = ansi_of(be16(s + i));
      out[n++] = c > 0 ? (char)c : '?';
    }
  else
    for (unsigned i = 0; i < length && n + 1 < cap; i++)
      out[n++] = s[i] < 128 ? (char)s[i] : '?';
  out[n] = 0;
}
/* Parses the metadata the selection layer needs. Every table range is
 * checked; truncated or overlong tables reject the file. */
int gdi_font_parse(const uint8_t *d, DWORD n, struct face *f) {
  if (!d || n < 12 || n > FILE_MAX)
    return 0;
  uint32_t version = be32(d);
  unsigned count = be16(d + 4);
  if ((version != 0x00010000u && version != 0x74727565u) || !count ||
      count > 64 || 12 + 16u * count > n)
    return 0;
  for (unsigned i = 0; i < count; i++)
    if (!span(n, be32(d + 12 + 16 * i + 8), be32(d + 12 + 16 * i + 12)))
      return 0;
  DWORD head_n, hhea_n, maxp_n, hmtx_n, cmap_n, name_n, post_n, os2_n = 0,
                                                              kern_n = 0;
  const uint8_t *head = table(d, n, "head", 54, &head_n),
                *hhea = table(d, n, "hhea", 36, &hhea_n),
                *maxp = table(d, n, "maxp", 6, &maxp_n),
                *hmtx = table(d, n, "hmtx", 4, &hmtx_n),
                *cmap = table(d, n, "cmap", 4, &cmap_n),
                *name = table(d, n, "name", 6, &name_n),
                *post = table(d, n, "post", 32, &post_n),
                *os2 = table(d, n, "OS/2", 78, &os2_n),
                *kern = table(d, n, "kern", 4, &kern_n);
  if (!head || !hhea || !maxp || !hmtx || !cmap || !name || !post ||
      !table(d, n, "glyf", 0, 0) || !table(d, n, "loca", 0, 0) ||
      be32(head + 12) != 0x5f0f3cf5u)
    return 0;
  w32_fill(f, 0, sizeof(*f));
  f->upem = be16(head + 18);
  f->num_glyphs = be16(maxp + 4);
  f->num_hmetrics = be16(hhea + 34);
  if (f->upem < 16 || f->upem > 16384 || !f->num_glyphs ||
      !f->num_hmetrics || f->num_hmetrics > f->num_glyphs ||
      4u * f->num_hmetrics > hmtx_n)
    return 0;
  for (unsigned i = 0; i < 4; i++)
    f->bbox[i] = s16(head + 36 + 2 * i);
  f->min_ppem = be16(head + 46);
  unsigned mac_style = be16(head + 44);
  f->hhea_ascent = s16(hhea + 4);
  f->hhea_descent = s16(hhea + 6);
  f->hhea_gap = s16(hhea + 8);
  f->max_advance = be16(hhea + 10);
  f->caret_rise = s16(hhea + 18);
  f->caret_run = s16(hhea + 20);
  f->italic_angle = (int32_t)be32(post + 4);
  f->underline_pos = s16(post + 8);
  f->underline_size = s16(post + 10);
  f->fixed = be32(post + 12) != 0;
  f->italic = !!(mac_style & 2);
  f->weight = mac_style & 1 ? FW_BOLD : FW_NORMAL;
  f->win_ascent = (unsigned)(f->hhea_ascent > 0 ? f->hhea_ascent : 0);
  f->win_descent = (unsigned)(f->hhea_descent < 0 ? -f->hhea_descent : 0);
  f->typo_ascent = f->hhea_ascent;
  f->typo_descent = f->hhea_descent;
  f->typo_gap = f->hhea_gap;
  f->strike_size = f->underline_size;
  f->strike_pos = (int)f->upem / 4;
  f->ranges[4] = 1;
  if (os2) {
    unsigned v = be16(os2);
    f->avg_width = s16(os2 + 2);
    unsigned w = be16(os2 + 4);
    f->weight = w >= 1 && w <= 9 ? w * 100 : w > 1000 ? 1000 : w ? w : 400;
    f->fs_type = be16(os2 + 8);
    for (unsigned i = 0; i < 4; i++) {
      f->sub[i] = s16(os2 + 10 + 2 * i);
      f->sup[i] = s16(os2 + 18 + 2 * i);
    }
    f->strike_size = s16(os2 + 26);
    f->strike_pos = s16(os2 + 28);
    w32_copy(f->panose, os2 + 32, 10);
    for (unsigned i = 0; i < 4; i++)
      f->ranges[i] = be32(os2 + 42 + 4 * i);
    f->fs_selection = be16(os2 + 62);
    f->first_char = be16(os2 + 64);
    f->last_char = be16(os2 + 66);
    f->typo_ascent = s16(os2 + 68);
    f->typo_descent = s16(os2 + 70);
    f->typo_gap = s16(os2 + 72);
    f->win_ascent = be16(os2 + 74);
    f->win_descent = be16(os2 + 76);
    if (v >= 1 && os2_n >= 86) {
      f->ranges[4] = be32(os2 + 78);
      f->ranges[5] = be32(os2 + 82);
    }
    if (v >= 2 && os2_n >= 96) {
      f->x_height = s16(os2 + 86);
      f->cap_height = s16(os2 + 88);
    }
    f->italic = f->italic || (f->fs_selection & 1);
  } else {
    f->first_char = 32;
    f->last_char = 255;
  }
  if (!f->win_ascent && !f->win_descent)
    return 0;
  if (!f->x_height)
    f->x_height = (int)f->upem / 2;
  if (!f->cap_height)
    f->cap_height = (int)(f->win_ascent * 7 / 10);
  /* cmap: Windows Unicode BMP, symbol, or Unicode platform, format 4. */
  unsigned subtables = be16(cmap + 2);
  const uint8_t *chosen = 0;
  DWORD chosen_n = 0;
  int rank = 0;
  if (4 + 8u * subtables > cmap_n)
    return 0;
  for (unsigned i = 0; i < subtables; i++) {
    const uint8_t *r = cmap + 4 + 8 * i;
    unsigned platform = be16(r), encoding = be16(r + 2);
    DWORD offset = be32(r + 4);
    if (!span(cmap_n, offset, 6))
      continue;
    const uint8_t *sub = cmap + offset;
    DWORD length = be16(sub + 2);
    if (be16(sub) != 4 || length < 16 || !span(cmap_n, offset, length))
      continue;
    int s = platform == 3 && encoding == 1   ? 3
            : platform == 3 && encoding == 0 ? 2
            : platform == 0                  ? 1
                                             : 0;
    if (s > rank) {
      rank = s;
      chosen = sub;
      chosen_n = length;
      f->symbol = platform == 3 && encoding == 0;
    }
  }
  if (!chosen)
    return 0;
  if (f->ranges[4] & 0x80000000u)
    f->symbol = 1;
  for (unsigned c = 0; c < 256; c++) {
    unsigned g = cmap_glyph(chosen, chosen_n,
                            f->symbol ? 0xf000u + c : unicode_of(c));
    if (!g && f->symbol)
      g = cmap_glyph(chosen, chosen_n, c);
    f->glyph[c] = (uint16_t)(g < f->num_glyphs ? g : 0);
    unsigned m = f->glyph[c] < f->num_hmetrics ? f->glyph[c]
                                               : f->num_hmetrics - 1;
    f->advance[c] = (uint16_t)be16(hmtx + 4 * m);
    f->lsb[c] = (int16_t)(f->glyph[c] < f->num_hmetrics
                              ? s16(hmtx + 4 * m + 2)
                              : 0);
  }
  name_string(name, name_n, 1, f->family, sizeof(f->family));
  name_string(name, name_n, 2, f->style, sizeof(f->style));
  name_string(name, name_n, 4, f->full, sizeof(f->full));
  if (!f->family[0])
    return 0;
  if (!f->style[0])
    copy(f->style, sizeof(f->style), "Regular");
  if (!f->full[0]) {
    copy(f->full, sizeof(f->full), f->family);
    if (!same(f->style, "Regular")) {
      append(f->full, sizeof(f->full), " ");
      append(f->full, sizeof(f->full), f->style);
    }
  }
  unsigned type = f->panose[0], serif = f->panose[1];
  uint8_t family = FF_DONTCARE;
  if (f->fixed || f->panose[3] == 9)
    family = FF_MODERN, f->fixed = 1;
  else if (type == 3)
    family = FF_SCRIPT;
  else if (type == 4 || type == 5)
    family = FF_DECORATIVE;
  else if (type == 2)
    family = serif >= 11 && serif <= 13 ? FF_SWISS : FF_ROMAN;
  f->pitch_family = family | (f->fixed ? FIXED_PITCH : VARIABLE_PITCH);
  /* Legacy kern table, version 0, first horizontal format-0 subtable. */
  if (kern && be16(kern) == 0 && be16(kern + 2) && kern_n >= 18) {
    unsigned length = be16(kern + 6), coverage = be16(kern + 8),
             pairs = be16(kern + 10);
    if ((coverage >> 8) == 0 && (coverage & 7) == 1 && length >= 14 &&
        4 + length <= kern_n && 14 + 6u * pairs <= length) {
      uint8_t *chars = gdi_alloc(f->num_glyphs + 1u);
      if (!chars)
        return 0;
      w32_fill(chars, 0, f->num_glyphs + 1u);
      for (unsigned c = 255; c >= 32; c--)
        if (f->glyph[c])
          chars[f->glyph[c]] = (uint8_t)c;
      unsigned keep = 0;
      for (unsigned i = 0; i < pairs; i++) {
        const uint8_t *p = kern + 18 + 6 * i;
        unsigned l = be16(p), r = be16(p + 2);
        if (l < f->num_glyphs && r < f->num_glyphs && chars[l] && chars[r])
          keep++;
      }
      if (keep > KERN_MAX)
        keep = KERN_MAX;
      if (keep) {
        f->kerns = gdi_alloc(keep * sizeof(struct kern));
        if (!f->kerns) {
          gdi_free(chars, f->num_glyphs + 1u);
          return 0;
        }
        for (unsigned i = 0; i < pairs && f->kern_count < keep; i++) {
          const uint8_t *p = kern + 18 + 6 * i;
          unsigned l = be16(p), r = be16(p + 2);
          if (l < f->num_glyphs && r < f->num_glyphs && chars[l] && chars[r])
            f->kerns[f->kern_count++] =
                (struct kern){chars[l], chars[r], (int16_t)s16(p + 4)};
        }
      }
      gdi_free(chars, f->num_glyphs + 1u);
    }
  }
  f->file_size = n;
  return 1;
}
static void face_free(struct face *f) {
  if (!f)
    return;
  if (f->native_face && S.backend) {
    S.backend->close(f->native_face);
    S.stats.face_bytes -= f->resident;
    S.stats.open_faces--;
  }
  if (f->kerns)
    gdi_free(f->kerns, f->kern_count * sizeof(struct kern));
  S.stats.store_bytes -= sizeof(*f);
  gdi_free(f, sizeof(*f));
}
/* Hostile-input entry for the host fuzz bounds: parse and discard. */
int gdi_font_validate(const void *data, DWORD size) {
  struct face *f = gdi_alloc(sizeof(*f));
  if (!f)
    return 0;
  int ok = gdi_font_parse(data, size, f);
  if (f->kerns && ok)
    gdi_free(f->kerns, f->kern_count * sizeof(struct kern));
  gdi_free(f, sizeof(*f));
  return ok;
}

/* ---- store ------------------------------------------------------------- */
static const char *base_name(const char *path) {
  const char *b = path;
  for (const char *p = path; *p; p++)
    if (*p == '\\' || *p == '/' || *p == ':')
      b = p + 1;
  return b;
}
static void join(char *out, const char *dir, const char *name, char sep) {
  copy(out, PATH_MAX_W32, dir);
  unsigned n = w32_strlen(out);
  if (n && out[n - 1] != sep && n + 1 < PATH_MAX_W32) {
    out[n++] = sep;
    out[n] = 0;
  }
  append(out, PATH_MAX_W32, name);
}
static int inside(const char *path, const char *dir) {
  unsigned n = w32_strlen(dir);
  for (unsigned i = 0; i < n; i++)
    if (!path[i] || lower((uint8_t)path[i]) != lower((uint8_t)dir[i]))
      return 0;
  return path[n] == '\\' && path[n + 1] && base_name(path) == path + n + 1;
}
static struct face *load_face(const char *win32, const char *native,
                              int installed) {
  if (S.count >= FACES_MAX)
    return 0;
  DWORD n = 0;
  uint8_t *data = read_file(win32, &n);
  if (!data)
    return 0;
  struct face *f = gdi_alloc(sizeof(*f));
  int ok = f && gdi_font_parse(data, n, f);
  gdi_free(data, n);
  if (!ok) {
    if (f)
      gdi_free(f, sizeof(*f));
    return 0;
  }
  copy(f->win32, sizeof(f->win32), win32);
  copy(f->native, sizeof(f->native), native);
  f->installed = installed;
  f->id = ++S.next_id;
  S.faces[S.count++] = f;
  S.stats.faces = S.count;
  S.stats.store_bytes += sizeof(*f);
  return f;
}
static void store_load(void) {
  if (S.loaded)
    return;
  S.loaded = 1;
  for (unsigned i = 0; i < sizeof(system_files) / sizeof(*system_files);
       i++) {
    char win32[PATH_MAX_W32], native[PATH_MAX_W32];
    join(win32, S.system_win32, system_files[i], '\\');
    join(native, S.system_native, system_files[i], '/');
    load_face(win32, native, 0);
  }
  /* Faces installed in earlier sessions: HKLM\...\Fonts values. */
  void *key = key_open(FONTS_KEY, 0);
  for (DWORD i = 0; key && R.enumerate && i < 256; i++) {
    char value[128], data[PATH_MAX_W32];
    DWORD vn = sizeof(value), dn = sizeof(data) - 1, type = 0;
    if (R.enumerate(key, i, value, &vn, 0, &type, (uint8_t *)data, &dn))
      break;
    if (type != 1 || !dn)
      continue;
    data[dn < sizeof(data) ? dn : sizeof(data) - 1] = 0;
    const char *file = base_name(data);
    char win32[PATH_MAX_W32], native[PATH_MAX_W32];
    join(win32, S.install_win32, file, '\\');
    join(native, S.install_native, file, '/');
    int known = 0;
    for (unsigned k = 0; k < S.count; k++)
      known |= same(S.faces[k]->win32, win32);
    struct face *f = known ? 0 : load_face(win32, native, 1);
    if (f) {
      f->copied = 1;
      key_string(ORIGIN_KEY, file, f->origin, sizeof(f->origin));
    }
  }
  key_close(key);
}

/* ---- backend faces and the shared budget --------------------------------- */
static DWORD face_budget(void) {
  DWORD fixed = GLYPH_BUDGET + S.stats.store_bytes;
  return fixed < BUDGET ? BUDGET - fixed : 0;
}
static void peaks(void) {
  if (S.stats.face_bytes > S.stats.face_peak)
    S.stats.face_peak = S.stats.face_bytes;
  if (S.stats.glyph_bytes > S.stats.glyph_peak)
    S.stats.glyph_peak = S.stats.glyph_bytes;
  DWORD total = S.stats.face_bytes + S.stats.glyph_bytes + S.stats.store_bytes;
  if (total > S.stats.total_peak)
    S.stats.total_peak = total;
}
static void face_close(struct face *f) {
  if (!f->native_face)
    return;
  S.backend->close(f->native_face);
  f->native_face = 0;
  S.stats.face_bytes -= f->resident;
  f->resident = 0;
  S.stats.open_faces--;
}
/* Closes least recently used faces other than keep until the open faces
 * fit the share of the 2 MiB budget left by the glyph cache and store. */
static void enforce_budget(struct face *keep, DWORD extra) {
  while (S.stats.face_bytes + extra > face_budget()) {
    struct face *victim = 0;
    for (unsigned i = 0; i < S.count; i++)
      if (S.faces[i] != keep && S.faces[i]->native_face &&
          (!victim || S.faces[i]->last_use < victim->last_use))
        victim = S.faces[i];
    if (!victim)
      return;
    face_close(victim);
    S.stats.face_evictions++;
  }
}
static void face_account(struct face *f) {
  DWORD now = S.backend->resident ? S.backend->resident(f->native_face)
                                  : f->file_size;
  S.stats.face_bytes = S.stats.face_bytes - f->resident + now;
  f->resident = now;
  enforce_budget(f, 0);
  peaks();
}
static int face_open(struct face *f) {
  if (!S.backend || !f)
    return 0;
  f->last_use = ++S.clock;
  if (f->native_face)
    return 1;
  /* A refused or unreachable face (f5-10C2 bridge) is retried only after
   * 256 further requests for it, not on every glyph. */
  if (f->retry_wait) {
    f->retry_wait--;
    return 0;
  }
  /* File bytes plus the backend's 64 KiB face allowance and trimmed cache. */
  DWORD need = f->file_size + (128u << 10);
  enforce_budget(f, need);
  if (S.stats.face_bytes + need > face_budget())
    return 0;
  f->native_face = S.backend->open(f->native);
  if (!f->native_face) {
    f->retry_wait = 256;
    return 0;
  }
  S.stats.open_faces++;
  face_account(f);
  return 1;
}

/* ---- matching --------------------------------------------------------- */
struct candidate {
  const char *family;
  const struct alias *alias;
  struct face *face;
  int italic, weight;
  uint8_t charset, pitch_family;
};
static struct face *physical(const char *family, int bold, int italic) {
  struct face *best = 0;
  int score = -1;
  for (unsigned i = 0; i < S.count; i++) {
    struct face *f = S.faces[i];
    if (!same(f->family, family))
      continue;
    int s = (f->italic == italic ? 2 : 0) + ((f->weight >= 600) == bold);
    if (s > score)
      score = s, best = f;
  }
  return best;
}
static int face_charset(const struct face *f) {
  return f->symbol ? SYMBOL_CHARSET : ANSI_CHARSET;
}
/* Calls fn for every candidate: virtual families first, then physical. */
static int candidates(int (*fn)(const struct candidate *, void *),
                      void *opaque) {
  for (unsigned a = 0; a < ALIASES; a++)
    for (unsigned style = 0; style < 4; style++) {
      int bold = style & 1, italic = (style >> 1) & 1;
      if (aliases[a].bold_only && !bold)
        continue;
      struct face *f = physical(aliases[a].physical, bold, italic);
      if (!f)
        continue;
      struct candidate c = {aliases[a].name,
                            &aliases[a],
                            f,
                            italic,
                            bold ? FW_BOLD : FW_NORMAL,
                            aliases[a].charset,
                            aliases[a].pitch_family};
      if (!fn(&c, opaque))
        return 0;
    }
  for (unsigned i = 0; i < S.count; i++) {
    struct face *f = S.faces[i];
    struct candidate c = {f->family,      0, f, f->italic, (int)f->weight,
                          (uint8_t)face_charset(f), f->pitch_family};
    if (!fn(&c, opaque))
      return 0;
  }
  return 1;
}
struct match {
  const LOGFONTA *lf;
  const char *wanted;
  int substituted;
  struct candidate best;
  long penalty;
};
static int full_name_match(const struct candidate *c, const char *wanted) {
  char full[96];
  copy(full, sizeof(full), c->family);
  if (c->weight >= 600)
    append(full, sizeof(full), " Bold");
  if (c->italic)
    append(full, sizeof(full), " Italic");
  return same(full, wanted) || (!c->alias && same(c->face->full, wanted));
}
static int score(const struct candidate *c, void *opaque) {
  struct match *m = opaque;
  const LOGFONTA *lf = m->lf;
  long p = 0;
  int charset = lf->lfCharSet == DEFAULT_CHARSET ? ANSI_CHARSET : lf->lfCharSet;
  if (charset != c->charset)
    p += P_CHARSET;
  unsigned pitch = lf->lfPitchAndFamily & 3, family = lf->lfPitchAndFamily & 0xf0,
           cfamily = c->pitch_family & 0xf0;
  int fixed = (c->pitch_family & 3) == FIXED_PITCH;
  if (pitch == FIXED_PITCH && !fixed)
    p += P_FIXED;
  if (m->wanted[0]) {
    int named = same(c->family, m->wanted) || full_name_match(c, m->wanted);
    if (!named)
      p += P_FACE;
    else if (m->substituted)
      p += P_SUBST;
  }
  if (family != FF_DONTCARE && family != cfamily)
    p += cfamily == FF_DONTCARE ? P_FAMILY_UNKNOWN : P_FAMILY;
  if (pitch == VARIABLE_PITCH && fixed)
    p += P_VARIABLE;
  if (pitch == DEFAULT_PITCH && fixed)
    p += P_DEFAULT_FIXED;
  int text_family = family == FF_ROMAN || family == FF_MODERN ||
                    family == FF_SWISS;
  int fancy = cfamily == FF_DECORATIVE || cfamily == FF_SCRIPT;
  if ((text_family && fancy) ||
      ((family == FF_DECORATIVE || family == FF_SCRIPT) &&
       (cfamily == FF_ROMAN || cfamily == FF_MODERN || cfamily == FF_SWISS)))
    p += P_UNLIKELY;
  if (!!lf->lfItalic != c->italic)
    p += lf->lfItalic && !c->italic ? P_ITALIC_SIM : P_ITALIC;
  int weight = lf->lfWeight ? (int)lf->lfWeight : FW_NORMAL;
  int diff = weight - c->weight;
  p += P_WEIGHT * (diff < 0 ? -diff : diff) / 10;
  if (!m->best.face || p < m->penalty) {
    m->best = *c;
    m->penalty = p;
  }
  return 1;
}
static void substitute(const char *name, char *out, unsigned cap,
                       int *substituted) {
  copy(out, cap, name);
  *substituted = 0;
  if (!name[0])
    return;
  char value[LF_FACESIZE + 16];
  registry();
  if (R.profile && R.profile("FontSubstitutes", name, "", value,
                             sizeof(value)) &&
      value[0]) {
    for (unsigned i = 0; value[i]; i++)
      if (value[i] == ',')
        value[i] = 0;
    copy(out, cap, value);
    *substituted = 1;
    return;
  }
  if (key_string(SUBST_KEY, name, value, sizeof(value))) {
    for (unsigned i = 0; value[i]; i++)
      if (value[i] == ',')
        value[i] = 0;
    copy(out, cap, value);
    *substituted = 1;
    return;
  }
  for (unsigned i = 0; i < sizeof(substitutes) / sizeof(*substitutes); i++)
    if (same(name, substitutes[i][0])) {
      copy(out, cap, substitutes[i][1]);
      *substituted = 1;
      return;
    }
}
/* Natural cell of a face at an em, outward rounded (no VDMX in the store). */
static void cell(const struct face *f, unsigned em26, int *ascent,
                 int *descent) {
  *ascent = scale_up((int)f->win_ascent, em26, f->upem);
  *descent = scale_up((int)f->win_descent, em26, f->upem);
}
/* Raster-origin aliases emulate the bitmap selection rule: the largest size
 * whose cell is not taller than the request (Gery, "height penalty"). */
static unsigned raster_em(const struct face *f, int height) {
  unsigned best = 1;
  for (unsigned e = 1; e <= 128; e++) {
    int a, d;
    cell(f, e * 64, &a, &d);
    if (a + d <= height)
      best = e;
    else
      break;
  }
  return best * 64;
}
static int resolve(struct realized *r) {
  if (r->resolved)
    return !r->failed;
  r->resolved = 1;
  r->failed = 1;
  store_load();
  const LOGFONTA *lf = &r->lf;
  char wanted[LF_FACESIZE];
  int substituted;
  char name[LF_FACESIZE];
  copy(name, sizeof(name), lf->lfFaceName);
  substitute(name, wanted, sizeof(wanted), &substituted);
  struct match m = {lf, wanted, substituted, {0}, 0};
  candidates(score, &m);
  if (!m.best.face)
    return 0;
  struct face *f = m.best.face;
  const struct alias *a = m.best.alias;
  r->face = f;
  r->alias = a;
  copy(r->name, sizeof(r->name), m.best.family);
  r->charset = m.best.charset;
  r->pitch_family = m.best.pitch_family;
  int height = lf->lfHeight ? (int)lf->lfHeight : -16;
  unsigned plan = a ? a->plan : PLAN_NONE;
  if (plan == PLAN_SYSTEM || plan == PLAN_FIXEDSYS)
    r->em26 = 13 * 64;
  else if (plan == PLAN_TERMINAL)
    r->em26 = 10 * 64;
  else if (plan == PLAN_RASTER)
    r->em26 = height < 0 ? (unsigned)-height * 64 : raster_em(f, height);
  else if (height < 0)
    r->em26 = (unsigned)-height * 64;
  else
    r->em26 = (unsigned)((uint64_t)(unsigned)height * 64 * f->upem /
                         (f->win_ascent + f->win_descent));
  if (r->em26 < 64)
    r->em26 = 64;
  if (r->em26 > EM_MAX)
    r->em26 = EM_MAX;
  cell(f, r->em26, &r->ascent, &r->descent);
  if (plan == PLAN_SYSTEM) {
    /* Windows 98 System font: 16-pixel cell, dialog base units 8x16. */
    r->ascent = 13;
    r->descent = 3;
  } else if (height > 0 && plan == PLAN_NONE && height > r->ascent) {
    r->descent = height - r->ascent;
  }
  int em_px = (int)((r->em26 + 32) / 64);
  r->internal = r->ascent + r->descent - em_px;
  if (r->internal < 0)
    r->internal = 0;
  int gap = f->hhea_gap - (int)((f->win_ascent + f->win_descent) -
                                (unsigned)(f->hhea_ascent - f->hhea_descent));
  r->external = gap > 0 ? scale(gap, r->em26, f->upem) : 0;
  r->average = scale(f->avg_width ? f->avg_width : (int)f->advance['x'],
                     r->em26, f->upem);
  r->maximum = scale((int)f->max_advance, r->em26, f->upem);
  int weight = lf->lfWeight ? (int)lf->lfWeight : FW_NORMAL;
  r->bold_synth = weight >= FW_SEMIBOLD && m.best.weight < FW_SEMIBOLD;
  r->italic_synth = lf->lfItalic && !m.best.italic;
  r->weight = r->bold_synth ? FW_BOLD : m.best.weight;
  r->italic = lf->lfItalic || m.best.italic;
  if (r->bold_synth) {
    r->average++;
    r->maximum++;
  }
  r->flags = (r->bold_synth ? G_BOLD : 0) | (r->italic_synth ? G_ITALIC : 0) |
             (lf->lfUnderline ? G_UNDERLINE : 0) |
             (lf->lfStrikeOut ? G_STRIKE : 0) |
             (lf->lfQuality == NONANTIALIASED_QUALITY ? G_MONO : 0);
  r->serial = ++S.serial;
  r->failed = 0;
  return 1;
}

/* ---- glyph cache ------------------------------------------------------ */
static int cache_ready(void) {
  if (S.cache)
    return 1;
  S.cache = gdi_alloc(GLYPH_SLOTS * sizeof(*S.cache));
  if (!S.cache)
    return 0;
  w32_fill(S.cache, 0, GLYPH_SLOTS * sizeof(*S.cache));
  for (unsigned i = 0; i < 256; i++)
    S.heads[i] = -1;
  S.stats.store_bytes += GLYPH_SLOTS * sizeof(*S.cache);
  S.stats.budget = BUDGET;
  return 1;
}
static unsigned bucket(unsigned face, unsigned em, unsigned flags,
                       unsigned glyph) {
  return (face * 31u + em * 7u + flags * 131u + glyph * 2654435761u) >> 24;
}
static void unlink_entry(int index) {
  struct gentry *e = &S.cache[index];
  unsigned b = bucket(e->face, e->em26, e->flags, e->glyph);
  for (int *at = &S.heads[b]; *at >= 0; at = &S.cache[*at].next)
    if (*at == index) {
      *at = e->next;
      break;
    }
  if (e->g.coverage)
    gdi_free(e->g.coverage, e->bytes);
  S.stats.glyph_bytes -= e->bytes;
  S.stats.glyphs--;
  w32_fill(e, 0, sizeof(*e));
}
static int evict_one(void) {
  int victim = -1;
  for (unsigned i = 0; i < GLYPH_SLOTS; i++)
    if (S.cache[i].used && !S.cache[i].pins &&
        (victim < 0 || S.cache[i].stamp < S.cache[victim].stamp))
      victim = (int)i;
  if (victim < 0)
    return 0;
  unlink_entry(victim);
  S.stats.evictions++;
  return 1;
}
/* Synthesis on coverage: italic shear, emboldening, underline and
 * strike-out bars, monochrome threshold. Returns an owned glyph. */
static int synthesize(const struct realized *r, const struct w32_gdi_glyph *in,
                      struct w32_gdi_glyph *out, DWORD *bytes) {
  int left = in->left, top = in->top, width = (int)in->width,
      height = (int)in->height, advance = in->advance;
  int shift_min = 0, shift_max = 0;
  if ((r->flags & G_ITALIC) && height) {
    shift_max = (top * 21) >> 6;
    shift_min = ((top - height + 1) * 21) >> 6;
    if (shift_min > 0)
      shift_min = 0;
  }
  int bold = !!(r->flags & G_BOLD);
  int x0 = left + shift_min, x1 = left + width + shift_max + bold;
  int y0 = -top, y1 = -top + height;
  if (!width || !height) {
    x0 = x1 = 0;
    y0 = y1 = 0;
  }
  int adv_px = (advance + 32) / 64 + bold;
  int u_top = 0, u_size = 0, s_top = 0, s_size = 0;
  const struct face *f = r->face;
  if (r->flags & G_UNDERLINE) {
    u_top = -scale(f->underline_pos, r->em26, f->upem);
    if (u_top < 1)
      u_top = 1;
    u_size = scale(f->underline_size, r->em26, f->upem);
    if (u_size < 1)
      u_size = 1;
  }
  if (r->flags & G_STRIKE) {
    s_size = scale(f->strike_size, r->em26, f->upem);
    if (s_size < 1)
      s_size = 1;
    s_top = -scale(f->strike_pos, r->em26, f->upem);
  }
  int bars = (u_size || s_size) && adv_px > 0;
  if (bars) {
    int bx0 = 0, bx1 = adv_px;
    int by0 = u_size ? u_top : s_top, by1 = u_size ? u_top + u_size : s_top + s_size;
    if (s_size) {
      if (s_top < by0)
        by0 = s_top;
      if (s_top + s_size > by1)
        by1 = s_top + s_size;
    }
    if (x0 == x1) {
      x0 = bx0, x1 = bx1, y0 = by0, y1 = by1;
    } else {
      if (bx0 < x0)
        x0 = bx0;
      if (bx1 > x1)
        x1 = bx1;
      if (by0 < y0)
        y0 = by0;
      if (by1 > y1)
        y1 = by1;
    }
  }
  DWORD w = (DWORD)(x1 - x0), h = (DWORD)(y1 - y0);
  *out = (struct w32_gdi_glyph){w, h, w, x0, -y0, advance + bold * 64, 0};
  *bytes = w * h;
  if (!*bytes) {
    out->width = out->height = out->pitch = 0;
    return 1;
  }
  if (*bytes > 65536)
    return 0;
  uint8_t *c = gdi_alloc(*bytes);
  if (!c)
    return 0;
  w32_fill(c, 0, *bytes);
  for (int y = 0; y < height; y++) {
    int shift = (r->flags & G_ITALIC) ? ((top - y) * 21) >> 6 : 0;
    for (int x = 0; x < width; x++) {
      unsigned v = in->coverage[(unsigned)y * in->pitch + (unsigned)x];
      if (!v)
        continue;
      int ox = left + x + shift - x0, oy = -top + y - y0;
      for (int k = 0; k <= bold; k++) {
        uint8_t *p = &c[(unsigned)oy * w + (unsigned)(ox + k)];
        if (*p < v)
          *p = (uint8_t)v;
      }
    }
  }
  if (bars) {
    for (int pass = 0; pass < 2; pass++) {
      int t = pass ? s_top : u_top, n = pass ? s_size : u_size;
      for (int y = t; y < t + n; y++)
        for (int x = 0; x < adv_px; x++)
          c[(unsigned)(y - y0) * w + (unsigned)(x - x0)] = 255;
    }
  }
  if (r->flags & G_MONO)
    for (DWORD i = 0; i < *bytes; i++)
      c[i] = c[i] >= 128 ? 255 : 0;
  out->coverage = c;
  return 1;
}
static unsigned glyph_index(struct realized *r, DWORD cp, int index) {
  if (index)
    return cp < r->face->num_glyphs ? cp : 0;
  unsigned g = 0;
  if (S.backend && r->face->native_face &&
      S.backend->lookup(r->face->native_face,
                        r->face->symbol && cp < 256 ? 0xf000u + cp : cp, &g))
    return g < r->face->num_glyphs ? g : 0;
  int c = r->charset == OEM_CHARSET && cp < 256 ? (int)cp : ansi_of(cp);
  return c >= 0 ? r->face->glyph[c] : 0;
}
static int design_advance(const struct face *f, unsigned glyph) {
  for (unsigned c = 0; c < 256; c++)
    if (f->glyph[c] == glyph)
      return f->advance[c];
  return f->advance[0];
}
/* Returns a cached glyph, pinned until gdi_font_unpin. */
static struct gentry *cached(struct realized *r, DWORD cp, int index) {
  if (!resolve(r) || !cache_ready())
    return 0;
  int native = face_open(r->face);
  unsigned glyph = glyph_index(r, cp, index);
  unsigned flags = r->flags | (native ? 0 : 0x100);
  unsigned b = bucket(r->face->id, r->em26, flags, glyph);
  for (int i = S.heads[b]; i >= 0; i = S.cache[i].next) {
    struct gentry *e = &S.cache[i];
    if (e->face == r->face->id && e->em26 == r->em26 && e->flags == flags &&
        e->glyph == glyph) {
      e->stamp = ++S.clock;
      S.stats.hits++;
      return e;
    }
  }
  S.stats.misses++;
  struct w32_gdi_glyph raw = {0}, made;
  DWORD bytes = 0;
  if (native) {
    if (!S.backend->raster(r->face->native_face, glyph, r->em26, &raw))
      return 0;
    int ok = synthesize(r, &raw, &made, &bytes);
    S.backend->free_glyph(r->face->native_face, &raw);
    face_account(r->face);
    if (!ok)
      return 0;
  } else {
    /* Metric-only mode until the native backend is installed: advances
     * from hmtx rounded to whole pixels, no coverage. */
    int adv = scale(design_advance(r->face, glyph), r->em26, r->face->upem);
    made = (struct w32_gdi_glyph){0, 0, 0, 0, 0,
                                  (adv + (r->bold_synth ? 1 : 0)) * 64, 0};
  }
  while (S.stats.glyph_bytes + bytes > GLYPH_BUDGET || S.stats.glyphs >= GLYPH_SLOTS)
    if (!evict_one()) {
      if (made.coverage)
        gdi_free(made.coverage, bytes);
      return 0;
    }
  int slot = -1;
  for (unsigned i = 0; i < GLYPH_SLOTS && slot < 0; i++)
    if (!S.cache[i].used)
      slot = (int)i;
  if (slot < 0) {
    if (made.coverage)
      gdi_free(made.coverage, bytes);
    return 0;
  }
  struct gentry *e = &S.cache[slot];
  *e = (struct gentry){r->face->id, r->em26, flags, glyph, ++S.clock, 0, 1,
                       S.heads[b],   made,     bytes};
  S.heads[b] = slot;
  S.stats.glyph_bytes += bytes;
  S.stats.glyphs++;
  peaks();
  return e;
}

/* ---- provider over the f5-10B boundary --------------------------------- */
static int p_metrics(void *o, unsigned em, struct w32_gdi_font_metrics *m) {
  (void)em;
  struct realized *r = o;
  if (!resolve(r))
    return 0;
  *m = (struct w32_gdi_font_metrics){r->ascent * 64, r->descent * 64,
                                     r->external * 64, r->maximum * 64};
  return 1;
}
static int p_raster(void *o, unsigned em, DWORD cp, int index,
                    struct w32_gdi_glyph *g) {
  (void)em;
  struct gentry *e = cached(o, cp, index);
  if (!e)
    return 0;
  e->pins++;
  *g = e->g;
  return 1;
}
static void p_free(void *o, struct w32_gdi_glyph *g) {
  (void)o;
  if (g->coverage && S.cache)
    for (unsigned i = 0; i < GLYPH_SLOTS; i++)
      if (S.cache[i].used && S.cache[i].pins &&
          S.cache[i].g.coverage == g->coverage) {
        S.cache[i].pins--;
        break;
      }
  for (unsigned i = 0; S.cache && !g->coverage && i < GLYPH_SLOTS; i++)
    if (S.cache[i].used && S.cache[i].pins && !S.cache[i].g.coverage) {
      S.cache[i].pins--;
      break;
    }
  g->coverage = 0;
}
static void p_retain(void *o) {
  struct realized *r = o;
  if (!r->stock)
    r->refs++;
}
static void finish_remove(struct face *);
static int in_use(const struct face *);
static void p_release(void *o) {
  struct realized *r = o;
  if (r->stock || --r->refs)
    return;
  for (struct realized **at = &S.live; *at; at = &(*at)->next)
    if (*at == r) {
      *at = r->next;
      break;
    }
  struct face *face = r->face;
  gdi_free(r, sizeof(*r));
  /* RemoveFontResource on a face still realized completes now. */
  if (face && face->remove_pending && !in_use(face))
    finish_remove(face);
}
static int p_object(void *o, int bytes, void *out, int wide) {
  struct realized *r = o;
  int size = wide ? (int)sizeof(LOGFONTW) : (int)sizeof(LOGFONTA);
  if (!out)
    return size;
  if (bytes < size)
    bytes = bytes < 28 ? 0 : bytes;
  if (!bytes)
    return 0;
  if (!wide) {
    int n = bytes < size ? bytes : size;
    w32_copy(out, &r->lf, (DWORD)n);
    return n;
  }
  LOGFONTW w;
  w32_copy(&w, &r->lf, 28);
  for (unsigned i = 0; i < LF_FACESIZE; i++)
    w.lfFaceName[i] = (WCHAR)unicode_of((uint8_t)r->lf.lfFaceName[i]);
  int n = bytes < size ? bytes : size;
  w32_copy(out, &w, (DWORD)n);
  return n;
}
static const struct w32_gdi_font_provider provider = {
    p_metrics, p_raster, p_free, p_retain, p_release, 0, p_object};

static struct realized *realized_of(HDC h) {
  struct dc *d = gdi_dc(h);
  struct object *o = d ? gdi_object(d->s.font, GDI_FONT) : 0;
  if (!o || o->u.font.provider.raster != p_raster)
    return 0;
  struct realized *r = o->u.font.face;
  return resolve(r) ? r : 0;
}

/* ---- public API: creation --------------------------------------------- */
BOOL W32API w32_gdi_set_face_backend(const struct w32_gdi_face_backend *b) {
  GDI_GUARD;
  if (b && (!b->open || !b->close || !b->lookup || !b->raster ||
            !b->free_glyph))
    return 0;
  for (unsigned i = 0; i < S.count; i++) {
    if (S.faces[i]->native_face)
      face_close(S.faces[i]);
    S.faces[i]->retry_wait = 0;
  }
  while (S.cache && S.stats.glyphs && evict_one()) {
  }
  S.backend = b;
  return 1;
}
BOOL W32API w32_gdi_font_store(const char *system_win32,
                               const char *system_native,
                               const char *install_win32,
                               const char *install_native) {
  GDI_GUARD;
  if (!system_win32 || !system_native || !install_win32 || !install_native ||
      S.loaded)
    return 0;
  copy(S.system_win32, sizeof(S.system_win32), system_win32);
  copy(S.system_native, sizeof(S.system_native), system_native);
  copy(S.install_win32, sizeof(S.install_win32), install_win32);
  copy(S.install_native, sizeof(S.install_native), install_native);
  return 1;
}
void W32API w32_gdi_font_statistics(struct w32_gdi_font_stats *out) {
  GDI_GUARD;
  if (out) {
    S.stats.budget = BUDGET;
    *out = S.stats;
  }
}
HFONT W32API CreateFontIndirectA(const LOGFONTA *lf) {
  GDI_GUARD;
  if (!lf) {
    SetLastError(87);
    return 0;
  }
  struct realized *r = gdi_alloc(sizeof(*r));
  if (!r)
    return 0;
  w32_fill(r, 0, sizeof(*r));
  r->lf = *lf;
  r->lf.lfFaceName[LF_FACESIZE - 1] = 0;
  if (r->lf.lfWeight < 0 || r->lf.lfWeight > 1000)
    r->lf.lfWeight = r->lf.lfWeight < 0 ? 0 : 1000;
  resolve(r);
  r->refs = 0;
  r->next = S.live;
  S.live = r;
  HFONT h = w32_gdi_create_font(&provider, r, r->em26 >= 64 ? r->em26 : 64);
  if (!h) {
    S.live = r->next;
    gdi_free(r, sizeof(*r));
  }
  return h;
}
HFONT W32API CreateFontIndirectW(const LOGFONTW *w) {
  if (!w) {
    SetLastError(87);
    return 0;
  }
  LOGFONTA a;
  w32_copy(&a, w, 28);
  for (unsigned i = 0; i < LF_FACESIZE; i++) {
    int c = ansi_of(w->lfFaceName[i]);
    a.lfFaceName[i] = (char)(c >= 0 ? c : '?');
    if (!w->lfFaceName[i])
      break;
  }
  a.lfFaceName[LF_FACESIZE - 1] = 0;
  return CreateFontIndirectA(&a);
}
HFONT W32API CreateFontA(int height, int width, int escapement,
                         int orientation, int weight, DWORD italic,
                         DWORD underline, DWORD strike, DWORD charset,
                         DWORD precision, DWORD clip, DWORD quality,
                         DWORD pitch, const char *face) {
  LOGFONTA lf = {height,
                 width,
                 escapement,
                 orientation,
                 weight,
                 (uint8_t)italic,
                 (uint8_t)underline,
                 (uint8_t)strike,
                 (uint8_t)charset,
                 (uint8_t)precision,
                 (uint8_t)clip,
                 (uint8_t)quality,
                 (uint8_t)pitch,
                 {0}};
  if (face)
    copy(lf.lfFaceName, LF_FACESIZE, face);
  return CreateFontIndirectA(&lf);
}
HFONT W32API CreateFontW(int height, int width, int escapement,
                         int orientation, int weight, DWORD italic,
                         DWORD underline, DWORD strike, DWORD charset,
                         DWORD precision, DWORD clip, DWORD quality,
                         DWORD pitch, const WCHAR *face) {
  LOGFONTW lf = {height,
                 width,
                 escapement,
                 orientation,
                 weight,
                 (uint8_t)italic,
                 (uint8_t)underline,
                 (uint8_t)strike,
                 (uint8_t)charset,
                 (uint8_t)precision,
                 (uint8_t)clip,
                 (uint8_t)quality,
                 (uint8_t)pitch,
                 {0}};
  for (unsigned i = 0; face && i + 1 < LF_FACESIZE && face[i]; i++)
    lf.lfFaceName[i] = face[i];
  return CreateFontIndirectW(&lf);
}

/* ---- metrics ------------------------------------------------------------ */
static void fill_metrics(const struct realized *r, TEXTMETRICA *tm) {
  const struct face *f = r->face;
  w32_fill(tm, 0, sizeof(*tm));
  tm->tmAscent = r->ascent;
  tm->tmDescent = r->descent;
  tm->tmHeight = r->ascent + r->descent;
  tm->tmInternalLeading = r->internal;
  tm->tmExternalLeading = r->external;
  tm->tmAveCharWidth = r->average;
  tm->tmMaxCharWidth = r->maximum;
  tm->tmWeight = r->weight;
  tm->tmDigitizedAspectX = tm->tmDigitizedAspectY = 96;
  int symbol = r->charset == SYMBOL_CHARSET, oem = r->charset == OEM_CHARSET;
  unsigned first = f->first_char < 32 ? f->first_char : 32;
  tm->tmFirstChar = symbol || oem ? (uint8_t)(oem ? 1 : first) : 30;
  tm->tmLastChar = (uint8_t)(f->last_char > 255 || oem ? 255 : f->last_char);
  tm->tmDefaultChar = symbol || oem ? (uint8_t)(oem ? 0 : first) : 31;
  tm->tmBreakChar = 32;
  tm->tmItalic = r->italic ? 255 : 0;
  tm->tmUnderlined = r->lf.lfUnderline ? 255 : 0;
  tm->tmStruckOut = r->lf.lfStrikeOut ? 255 : 0;
  int fixed = (r->pitch_family & 3) == FIXED_PITCH;
  tm->tmPitchAndFamily = (uint8_t)((r->pitch_family & 0xf0) | TMPF_TRUETYPE |
                                   TMPF_VECTOR | (fixed ? 0 : TMPF_FIXED_PITCH));
  tm->tmCharSet = r->charset;
}
BOOL W32API GetTextMetricsA(HDC h, TEXTMETRICA *tm) {
  GDI_GUARD;
  struct realized *r = realized_of(h);
  if (!r || !tm)
    return 0;
  fill_metrics(r, tm);
  return 1;
}
BOOL W32API GetTextMetricsW(HDC h, TEXTMETRICW *tm) {
  GDI_GUARD;
  TEXTMETRICA a;
  if (!tm || !GetTextMetricsA(h, &a))
    return 0;
  w32_copy(tm, &a, 44);
  tm->tmFirstChar = (WCHAR)a.tmFirstChar;
  tm->tmLastChar = a.tmCharSet == ANSI_CHARSET ? 0xfffc : (WCHAR)a.tmLastChar;
  tm->tmDefaultChar = (WCHAR)a.tmDefaultChar;
  tm->tmBreakChar = (WCHAR)a.tmBreakChar;
  tm->tmItalic = a.tmItalic;
  tm->tmUnderlined = a.tmUnderlined;
  tm->tmStruckOut = a.tmStruckOut;
  tm->tmPitchAndFamily = a.tmPitchAndFamily;
  tm->tmCharSet = a.tmCharSet;
  return 1;
}
int W32API GetTextFaceA(HDC h, int n, char *out) {
  GDI_GUARD;
  struct realized *r = realized_of(h);
  if (!r)
    return 0;
  int length = (int)w32_strlen(r->name);
  if (!out)
    return length + 1;
  if (n <= 0)
    return 0;
  int k = length < n - 1 ? length : n - 1;
  w32_copy(out, r->name, (DWORD)k);
  out[k] = 0;
  return k + 1;
}
int W32API GetTextFaceW(HDC h, int n, WCHAR *out) {
  char a[LF_FACESIZE];
  int k = GetTextFaceA(h, sizeof(a), a);
  if (!k || !out)
    return k;
  if (n <= 0)
    return 0;
  int i = 0;
  for (; i < n - 1 && a[i]; i++)
    out[i] = (WCHAR)unicode_of((uint8_t)a[i]);
  out[i] = 0;
  return i + 1;
}
int W32API GetTextCharset(HDC h) {
  GDI_GUARD;
  struct realized *r = realized_of(h);
  return r ? r->charset : DEFAULT_CHARSET;
}
/* f5-20B: the selected font's character set and its OS/2 Unicode and code
 * page ranges (the FONTSIGNATURE font enumeration reports). */
int W32API GetTextCharsetInfo(HDC h, FONTSIGNATURE *sig, DWORD flags) {
  GDI_GUARD;
  (void)flags;
  struct realized *r = realized_of(h);
  if (sig)
    w32_fill(sig, 0, sizeof(*sig));
  if (!r)
    return DEFAULT_CHARSET;
  if (sig && r->face) {
    for (unsigned i = 0; i < 4; i++)
      sig->fsUsb[i] = r->face->ranges[i];
    sig->fsCsb[0] = r->face->ranges[4];
    sig->fsCsb[1] = r->face->ranges[5];
  }
  return r->charset;
}
DWORD W32API SetMapperFlags(HDC h, DWORD flags) {
  GDI_GUARD;
  if (!gdi_dc(h))
    return GDI_ERROR;
  DWORD old = S.mapper_flags;
  S.mapper_flags = flags & 1;
  return old;
}
static int widths(HDC h, unsigned first, unsigned last, int *out, ABC *abc,
                  int wide) {
  GDI_GUARD;
  struct realized *r = realized_of(h);
  if (!r || (!out && !abc) || last < first || last - first > 65535)
    return 0;
  for (unsigned c = first; c <= last; c++) {
    DWORD cp = wide ? c : (c < 256 ? unicode_of(c) : c);
    struct gentry *e = cached(r, cp, 0);
    if (!e)
      return 0;
    int advance = (e->g.advance + 32) / 64;
    if (out)
      out[c - first] = advance;
    if (abc) {
      int a = e->g.width ? e->g.left : 0, b = (int)e->g.width;
      abc[c - first] = (ABC){a, (unsigned)b, advance - a - b};
    }
  }
  return 1;
}
BOOL W32API GetCharWidth32A(HDC h, unsigned a, unsigned b, int *out) {
  return widths(h, a, b, out, 0, 0);
}
BOOL W32API GetCharWidthA(HDC h, unsigned a, unsigned b, int *out) {
  return widths(h, a, b, out, 0, 0);
}
BOOL W32API GetCharWidth32W(HDC h, unsigned a, unsigned b, int *out) {
  return widths(h, a, b, out, 0, 1);
}
BOOL W32API GetCharWidthW(HDC h, unsigned a, unsigned b, int *out) {
  return widths(h, a, b, out, 0, 1);
}
BOOL W32API GetCharABCWidthsA(HDC h, unsigned a, unsigned b, ABC *out) {
  return widths(h, a, b, 0, out, 0);
}
BOOL W32API GetCharABCWidthsW(HDC h, unsigned a, unsigned b, ABC *out) {
  return widths(h, a, b, 0, out, 1);
}
static void put_string(uint8_t *base, DWORD *at, DWORD *field,
                       const char *s) {
  DWORD n = w32_strlen(s) + 1;
  w32_copy(base + *at, s, n);
  *field = *at;
  *at += n;
}
unsigned W32API GetOutlineTextMetricsA(HDC h, unsigned size,
                                       OUTLINETEXTMETRICA *out) {
  GDI_GUARD;
  struct realized *r = realized_of(h);
  if (!r)
    return 0;
  const struct face *f = r->face;
  char style[48];
  copy(style, sizeof(style),
       r->weight >= 600 ? (r->italic ? "Bold Italic" : "Bold")
                        : (r->italic ? "Italic" : "Regular"));
  char full[96];
  copy(full, sizeof(full), r->name);
  if (!same(style, "Regular")) {
    append(full, sizeof(full), " ");
    append(full, sizeof(full), style);
  }
  DWORD total = sizeof(OUTLINETEXTMETRICA) + w32_strlen(r->name) + 1 +
                w32_strlen(full) + 1 + w32_strlen(style) + 1 +
                w32_strlen(full) + 1;
  if (!out)
    return total;
  if (size < total)
    return 0;
  uint8_t *base = (uint8_t *)out;
  w32_fill(out, 0, total);
  OUTLINETEXTMETRICA *o = out;
  unsigned em = r->em26, u = f->upem;
  o->otmSize = total;
  fill_metrics(r, &o->otmTextMetrics);
  w32_copy(&o->otmPanoseNumber, f->panose, 10);
  o->otmfsSelection = f->fs_selection;
  o->otmfsType = f->fs_type;
  o->otmsCharSlopeRise = r->italic && f->caret_run ? f->caret_rise : 1;
  o->otmsCharSlopeRun = r->italic ? f->caret_run : 0;
  o->otmItalicAngle = (int)(((int64_t)f->italic_angle * 10) >> 16);
  o->otmEMSquare = u;
  o->otmAscent = scale(f->typo_ascent, em, u);
  o->otmDescent = scale(f->typo_descent, em, u);
  o->otmLineGap = (unsigned)scale(f->typo_gap, em, u);
  o->otmsCapEmHeight = (unsigned)scale(f->cap_height, em, u);
  o->otmsXHeight = (unsigned)scale(f->x_height, em, u);
  o->otmrcFontBox = (RECT){scale(f->bbox[0], em, u), scale(f->bbox[3], em, u),
                           scale(f->bbox[2], em, u), scale(f->bbox[1], em, u)};
  o->otmMacAscent = scale(f->hhea_ascent, em, u);
  o->otmMacDescent = scale(f->hhea_descent, em, u);
  o->otmMacLineGap = (unsigned)scale(f->hhea_gap, em, u);
  o->otmusMinimumPPEM = f->min_ppem;
  o->otmptSubscriptSize = (POINT){scale(f->sub[0], em, u), scale(f->sub[1], em, u)};
  o->otmptSubscriptOffset = (POINT){scale(f->sub[2], em, u), scale(f->sub[3], em, u)};
  o->otmptSuperscriptSize = (POINT){scale(f->sup[0], em, u), scale(f->sup[1], em, u)};
  o->otmptSuperscriptOffset = (POINT){scale(f->sup[2], em, u), scale(f->sup[3], em, u)};
  o->otmsStrikeoutSize = (unsigned)scale(f->strike_size, em, u);
  o->otmsStrikeoutPosition = scale(f->strike_pos, em, u);
  o->otmsUnderscoreSize = scale(f->underline_size, em, u);
  o->otmsUnderscorePosition = scale(f->underline_pos, em, u);
  DWORD at = sizeof(OUTLINETEXTMETRICA);
  put_string(base, &at, &o->otmpFamilyName, r->name);
  put_string(base, &at, &o->otmpFaceName, full);
  put_string(base, &at, &o->otmpStyleName, style);
  put_string(base, &at, &o->otmpFullName, full);
  return total;
}
static int identity(const MAT2 *m) {
  return !m || (m->eM11.value == 1 && !m->eM11.fract && !m->eM12.value &&
                !m->eM12.fract && !m->eM21.value && !m->eM21.fract &&
                m->eM22.value == 1 && !m->eM22.fract);
}
static DWORD glyph_outline(HDC h, DWORD cp, unsigned format, GLYPHMETRICS *gm,
                           DWORD size, void *buffer, const MAT2 *mat) {
  GDI_GUARD;
  struct realized *r = realized_of(h);
  unsigned kind = format & ~GGO_GLYPH_INDEX;
  if (!r || !gm || !identity(mat) ||
      (kind != GGO_METRICS && kind != GGO_BITMAP && kind != GGO_GRAY2_BITMAP &&
       kind != GGO_GRAY4_BITMAP && kind != GGO_GRAY8_BITMAP))
    return GDI_ERROR;
  struct gentry *e = cached(r, cp, !!(format & GGO_GLYPH_INDEX));
  if (!e)
    return GDI_ERROR;
  const struct w32_gdi_glyph *g = &e->g;
  *gm = (GLYPHMETRICS){g->width ? g->width : 1, g->height ? g->height : 1,
                       {g->left, g->top}, (int16_t)((g->advance + 32) / 64),
                       0};
  if (kind == GGO_METRICS)
    return 0;
  if (!g->coverage && (g->width || g->height))
    return GDI_ERROR;
  if (!S.backend)
    return GDI_ERROR;
  DWORD pitch = kind == GGO_BITMAP ? ((g->width + 31) / 32) * 4
                                   : ((g->width + 3) / 4) * 4;
  DWORD need = pitch * g->height;
  if (!buffer || !size)
    return need;
  if (size < need)
    return GDI_ERROR;
  uint8_t *out = buffer;
  w32_fill(out, 0, need);
  unsigned levels = kind == GGO_GRAY2_BITMAP   ? 4
                    : kind == GGO_GRAY4_BITMAP ? 16
                                               : 64;
  for (DWORD y = 0; y < g->height; y++)
    for (DWORD x = 0; x < g->width; x++) {
      unsigned v = g->coverage[y * g->pitch + x];
      if (kind == GGO_BITMAP) {
        if (v >= 128)
          out[y * pitch + x / 8] |= (uint8_t)(0x80u >> (x & 7));
      } else
        out[y * pitch + x] = (uint8_t)((v * levels + 127) / 255);
    }
  return need;
}
DWORD W32API GetGlyphOutlineA(HDC h, unsigned c, unsigned format,
                              GLYPHMETRICS *gm, DWORD size, void *buffer,
                              const MAT2 *mat) {
  DWORD cp = format & GGO_GLYPH_INDEX ? c : (c < 256 ? unicode_of(c) : c);
  return glyph_outline(h, cp, format, gm, size, buffer, mat);
}
DWORD W32API GetGlyphOutlineW(HDC h, unsigned c, unsigned format,
                              GLYPHMETRICS *gm, DWORD size, void *buffer,
                              const MAT2 *mat) {
  return glyph_outline(h, c, format, gm, size, buffer, mat);
}
DWORD W32API GetKerningPairsA(HDC h, DWORD n, KERNINGPAIR *out) {
  GDI_GUARD;
  struct realized *r = realized_of(h);
  if (!r)
    return 0;
  const struct face *f = r->face;
  if (!out)
    return f->kern_count;
  DWORD k = n < f->kern_count ? n : f->kern_count;
  for (DWORD i = 0; i < k; i++)
    out[i] = (KERNINGPAIR){f->kerns[i].left, f->kerns[i].right,
                           scale(f->kerns[i].value, r->em26, f->upem)};
  return k;
}
DWORD W32API GetFontData(HDC h, DWORD tag, DWORD offset, void *out,
                         DWORD bytes) {
  GDI_GUARD;
  struct realized *r = realized_of(h);
  if (!r)
    return GDI_ERROR;
  const struct face *f = r->face;
  DWORD start = 0, length = f->file_size;
  if (tag) {
    uint8_t directory[12 + 16 * 64];
    if (read_range(f->win32, 0, directory, 12) == GDI_ERROR)
      return GDI_ERROR;
    unsigned count = be16(directory + 4);
    if (!count || count > 64 ||
        read_range(f->win32, 12, directory + 12, 16 * count) == GDI_ERROR)
      return GDI_ERROR;
    int found = 0;
    for (unsigned i = 0; i < count && !found; i++) {
      const uint8_t *e = directory + 12 + 16 * i;
      if (e[0] == (uint8_t)tag && e[1] == (uint8_t)(tag >> 8) &&
          e[2] == (uint8_t)(tag >> 16) && e[3] == (uint8_t)(tag >> 24)) {
        start = be32(e + 8);
        length = be32(e + 12);
        found = span(f->file_size, start, length);
      }
    }
    if (!found)
      return GDI_ERROR;
  }
  if (offset > length)
    return GDI_ERROR;
  DWORD available = length - offset;
  if (!out || !bytes)
    return available;
  DWORD n = bytes < available ? bytes : available;
  return read_range(f->win32, start + offset, out, n);
}

/* ---- enumeration -------------------------------------------------------- */
struct enumeration {
  const char *family;
  unsigned charset;
  int extended, legacy;
  FONTENUMPROCA proc;
  intptr_t lparam;
  int result, stopped;
  char seen[FACES_MAX + 16][LF_FACESIZE];
  unsigned seen_count;
};
static int enumerate_one(const struct candidate *c, void *opaque) {
  struct enumeration *e = opaque;
  if (e->stopped)
    return 0;
  if (e->charset != DEFAULT_CHARSET && e->charset != c->charset)
    return 1;
  if (e->family) {
    if (!same(e->family, c->family))
      return 1;
  } else {
    for (unsigned i = 0; i < e->seen_count; i++)
      if (same(e->seen[i], c->family))
        return 1;
    if (e->seen_count < sizeof(e->seen) / sizeof(*e->seen))
      copy(e->seen[e->seen_count++], LF_FACESIZE, c->family);
  }
  struct realized r = {0};
  r.lf.lfHeight = 16;
  r.lf.lfWeight = c->weight;
  r.lf.lfItalic = (uint8_t)c->italic;
  r.lf.lfCharSet = c->charset;
  r.lf.lfPitchAndFamily = c->pitch_family;
  copy(r.lf.lfFaceName, LF_FACESIZE, c->family);
  if (!resolve(&r))
    return 1;
  ENUMLOGFONTEXA elf;
  NEWTEXTMETRICEXA ntm;
  w32_fill(&elf, 0, sizeof(elf));
  w32_fill(&ntm, 0, sizeof(ntm));
  fill_metrics(&r, (TEXTMETRICA *)&ntm.ntmTm);
  const struct face *f = c->face;
  ntm.ntmTm.ntmFlags = (c->italic ? NTM_ITALIC : 0) |
                       (c->weight >= 600 ? NTM_BOLD : 0) |
                       (!c->italic && c->weight < 600 ? NTM_REGULAR : 0);
  ntm.ntmTm.ntmSizeEM = f->upem;
  ntm.ntmTm.ntmCellHeight = f->win_ascent + f->win_descent;
  ntm.ntmTm.ntmAvgWidth = (unsigned)f->avg_width;
  for (unsigned i = 0; i < 4; i++)
    ntm.ntmFontSig.fsUsb[i] = f->ranges[i];
  ntm.ntmFontSig.fsCsb[0] = f->ranges[4];
  ntm.ntmFontSig.fsCsb[1] = f->ranges[5];
  elf.elfLogFont = (LOGFONTA){r.ascent + r.descent,
                              r.average,
                              0,
                              0,
                              c->weight,
                              (uint8_t)(c->italic ? 255 : 0),
                              0,
                              0,
                              c->charset,
                              3,
                              2,
                              1,
                              c->pitch_family,
                              {0}};
  copy(elf.elfLogFont.lfFaceName, LF_FACESIZE, c->family);
  const char *style = c->weight >= 600 ? (c->italic ? "Bold Italic" : "Bold")
                                       : (c->italic ? "Italic" : "Regular");
  copy((char *)elf.elfFullName, sizeof(elf.elfFullName), c->family);
  if (!same(style, "Regular")) {
    append((char *)elf.elfFullName, sizeof(elf.elfFullName), " ");
    append((char *)elf.elfFullName, sizeof(elf.elfFullName), style);
  }
  copy((char *)elf.elfStyle, sizeof(elf.elfStyle), style);
  copy((char *)elf.elfScript, sizeof(elf.elfScript),
       c->charset == SYMBOL_CHARSET ? "Symbol"
       : c->charset == OEM_CHARSET  ? "OEM/DOS"
                                    : "Western");
  if (e->legacy) {
    /* EnumFonts: the face name reported is the family. */
    elf.elfLogFont.lfFaceName[LF_FACESIZE - 1] = 0;
  }
  e->result = e->proc(&elf.elfLogFont, (const TEXTMETRICA *)&ntm,
                      TRUETYPE_FONTTYPE, e->lparam);
  if (!e->result)
    e->stopped = 1;
  return !e->stopped;
}
static int enumerate(const char *family, unsigned charset, int legacy,
                     FONTENUMPROCA proc, intptr_t lparam) {
  GDI_GUARD;
  if (!proc)
    return 0;
  store_load();
  struct enumeration *e = gdi_alloc(sizeof(*e));
  if (!e)
    return 0;
  w32_fill(e, 0, sizeof(*e));
  e->family = family && family[0] ? family : 0;
  e->charset = charset;
  e->legacy = legacy;
  e->proc = proc;
  e->lparam = lparam;
  e->result = 1;
  candidates(enumerate_one, e);
  int result = e->result;
  gdi_free(e, sizeof(*e));
  return result;
}
int W32API EnumFontFamiliesExA(HDC h, LOGFONTA *lf, FONTENUMPROCA proc,
                               intptr_t lparam, DWORD flags) {
  (void)flags;
  if (!gdi_dc(h) || !lf)
    return 0;
  return enumerate(lf->lfFaceName, lf->lfCharSet, 0, proc, lparam);
}
int W32API EnumFontFamiliesA(HDC h, const char *family, FONTENUMPROCA proc,
                             intptr_t lparam) {
  if (!gdi_dc(h))
    return 0;
  return enumerate(family, DEFAULT_CHARSET, 0, proc, lparam);
}
int W32API EnumFontsA(HDC h, const char *face, FONTENUMPROCA proc,
                      intptr_t lparam) {
  if (!gdi_dc(h))
    return 0;
  return enumerate(face, DEFAULT_CHARSET, 1, proc, lparam);
}

/* ---- font resources ---------------------------------------------------- */
static struct face *by_path(const char *path) {
  const char *file = base_name(path);
  for (unsigned i = 0; i < S.count; i++) {
    struct face *f = S.faces[i];
    if (same(f->win32, path) || same(f->origin, path) ||
        (path == file && same(base_name(f->win32), file)))
      return f;
  }
  return 0;
}
/* CreateScalableFontResource writes a CiukiOS font-reference file: an
 * eight-byte signature followed by the TrueType path. Windows NE-format .FOT
 * files are not parsed (W21); this pairs with AddFontResource below. */
static const char FOT_MAGIC[8] = {'C', 'I', 'U', 'K', 'I', 'F', 'O', 'T'};
BOOL W32API CreateScalableFontResourceA(DWORD hidden, const char *fot,
                                        const char *ttf, const char *path) {
  GDI_GUARD;
  (void)hidden;
  char target[PATH_MAX_W32];
  if (!fot || !ttf || !ttf[0]) {
    SetLastError(87);
    return 0;
  }
  if (path && path[0])
    join(target, path, ttf, '\\');
  else
    copy(target, sizeof(target), ttf);
  DWORD n = 0;
  uint8_t *data = read_file(target, &n);
  struct face probe;
  int ok = data && gdi_font_parse(data, n, &probe);
  if (data)
    gdi_free(data, n);
  if (!ok) {
    SetLastError(2);
    return 0;
  }
  if (probe.kerns)
    gdi_free(probe.kerns, probe.kern_count * sizeof(struct kern));
  void *h = CreateFileA(fot, 0x40000000u, 0, 0, 1, 0, 0);
  if (!h || h == INVALID_FILE)
    return 0;
  DWORD wrote = 0, length = w32_strlen(target) + 1;
  int done = WriteFile(h, FOT_MAGIC, 8, &wrote, 0) && wrote == 8 &&
             WriteFile(h, target, length, &wrote, 0) && wrote == length;
  CloseHandle(h);
  if (!done)
    DeleteFileA(fot);
  return done;
}
static int resolve_reference(const char *path, char *out) {
  uint8_t header[8 + PATH_MAX_W32];
  void *h = CreateFileA(path, 0x80000000u, 1, 0, 3, 0, 0);
  if (!h || h == INVALID_FILE)
    return 0;
  DWORD got = 0;
  int ok = ReadFile(h, header, sizeof(header), &got, 0) && got > 9 &&
           !w32_memcmp_fot(header);
  CloseHandle(h);
  if (!ok)
    return 0;
  header[got < sizeof(header) ? got : sizeof(header) - 1] = 0;
  copy(out, PATH_MAX_W32, (const char *)header + 8);
  return out[0] != 0;
}
int W32API AddFontResourceA(const char *path) {
  GDI_GUARD;
  if (!path || !path[0] || w32_strlen(path) >= PATH_MAX_W32) {
    SetLastError(87);
    return 0;
  }
  char reference[PATH_MAX_W32];
  if (resolve_reference(path, reference))
    path = reference;
  store_load();
  struct face *existing = by_path(path);
  if (existing) {
    existing->resources++;
    return 1;
  }
  const char *file = base_name(path);
  char source[PATH_MAX_W32];
  if (file == path)
    join(source, S.install_win32, file, '\\');
  else
    copy(source, sizeof(source), path);
  DWORD n = 0;
  uint8_t *data = read_file(source, &n);
  if (!data) {
    SetLastError(2);
    return 0;
  }
  struct face probe;
  int ok = gdi_font_parse(data, n, &probe);
  gdi_free(data, n);
  if (!ok) {
    /* .FON/.FNT raster resources and CFF outlines are not accepted (W21). */
    SetLastError(13);
    return 0;
  }
  if (probe.kerns)
    gdi_free(probe.kerns, probe.kern_count * sizeof(struct kern));
  if (S.count >= FACES_MAX) {
    SetLastError(8);
    return 0;
  }
  char win32[PATH_MAX_W32], native[PATH_MAX_W32];
  int copied = 0;
  if (inside(source, S.install_win32) || inside(source, S.system_win32)) {
    copy(win32, sizeof(win32), source);
  } else {
    join(win32, S.install_win32, file, '\\');
    if (!CopyFileA(source, win32, 1)) {
      struct face *other = by_path(win32);
      if (!other) {
        DWORD m = 0;
        uint8_t *d = read_file(win32, &m);
        struct face check;
        int same_face = d && gdi_font_parse(d, m, &check) &&
                        same(check.family, probe.family) &&
                        same(check.style, probe.style);
        if (d)
          gdi_free(d, m);
        if (same_face && check.kerns)
          gdi_free(check.kerns, check.kern_count * sizeof(struct kern));
        if (!same_face) {
          SetLastError(80);
          return 0;
        }
      }
    } else
      copied = 1;
  }
  join(native, S.install_native, base_name(win32), '/');
  if (inside(win32, S.system_win32) && !inside(win32, S.install_win32))
    join(native, S.system_native, base_name(win32), '/');
  struct face *f = by_path(win32);
  if (!f)
    f = load_face(win32, native, 1);
  if (!f) {
    if (copied)
      DeleteFileA(win32);
    SetLastError(13);
    return 0;
  }
  f->resources++;
  if (copied) {
    f->copied = 1;
    copy(f->origin, sizeof(f->origin), source);
    key_put(ORIGIN_KEY, base_name(win32), source);
  }
  char value[96];
  copy(value, sizeof(value), f->full);
  append(value, sizeof(value), " (TrueType)");
  key_put(FONTS_KEY, value, base_name(win32));
  return 1;
}
static int in_use(const struct face *f) {
  for (struct realized *r = S.live; r; r = r->next)
    if (r->face == f)
      return 1;
  for (unsigned i = 0; i < 7; i++)
    if (stock_fonts[i].face == f)
      return 1;
  return 0;
}
BOOL W32API RemoveFontResourceA(const char *path) {
  GDI_GUARD;
  if (!path || !path[0])
    return 0;
  store_load();
  struct face *f = by_path(path);
  if (!f || !f->installed) {
    SetLastError(2);
    return 0;
  }
  if (f->resources > 0)
    f->resources--;
  if (f->resources)
    return 1;
  if (in_use(f)) {
    f->remove_pending = 1;
    return 1;
  }
  finish_remove(f);
  return 1;
}
static void finish_remove(struct face *f) {
  char value[96];
  copy(value, sizeof(value), f->full);
  append(value, sizeof(value), " (TrueType)");
  key_delete(FONTS_KEY, value);
  if (f->copied) {
    key_delete(ORIGIN_KEY, base_name(f->win32));
    DeleteFileA(f->win32);
  }
  for (unsigned i = 0; i < S.count; i++)
    if (S.faces[i] == f) {
      for (unsigned j = i + 1; j < S.count; j++)
        S.faces[j - 1] = S.faces[j];
      S.count--;
      break;
    }
  if (S.cache)
    for (unsigned i = 0; i < GLYPH_SLOTS; i++)
      if (S.cache[i].used && S.cache[i].face == f->id && !S.cache[i].pins)
        unlink_entry((int)i);
  face_free(f);
  S.stats.faces = S.count;
}

/* ---- stock objects and process teardown -------------------------------- */
static const struct {
  int height;
  const char *face;
  uint8_t charset, pitch;
  int weight;
} stock_specs[7] = {
    {12, "Terminal", OEM_CHARSET, FF_MODERN | FIXED_PITCH, FW_NORMAL},
    {13, "Courier", ANSI_CHARSET, FF_MODERN | FIXED_PITCH, FW_NORMAL},
    {13, "MS Sans Serif", ANSI_CHARSET, FF_SWISS | VARIABLE_PITCH, FW_NORMAL},
    {16, "System", ANSI_CHARSET, FF_SWISS | VARIABLE_PITCH, FW_BOLD},
    {16, "System", ANSI_CHARSET, FF_SWISS | VARIABLE_PITCH, FW_BOLD},
    {15, "Fixedsys", ANSI_CHARSET, FF_MODERN | FIXED_PITCH, FW_NORMAL},
    {-11, "MS Sans Serif", ANSI_CHARSET, FF_SWISS | VARIABLE_PITCH,
     FW_NORMAL},
};
void gdi_font_stock(unsigned index, struct font *out) {
  for (unsigned i = 0; i < 7; i++) {
    if (stock_index[i] != index)
      continue;
    struct realized *r = &stock_fonts[i];
    w32_fill(r, 0, sizeof(*r));
    r->stock = 1;
    r->lf.lfHeight = stock_specs[i].height;
    r->lf.lfWeight = stock_specs[i].weight;
    r->lf.lfCharSet = stock_specs[i].charset;
    r->lf.lfPitchAndFamily = stock_specs[i].pitch;
    copy(r->lf.lfFaceName, LF_FACESIZE, stock_specs[i].face);
    *out = (struct font){provider, r, 16 * 64};
    return;
  }
}
void gdi_font_exit(void) {
  while (S.live) {
    struct realized *r = S.live;
    S.live = r->next;
    gdi_free(r, sizeof(*r));
  }
  if (S.cache) {
    for (unsigned i = 0; i < GLYPH_SLOTS; i++)
      if (S.cache[i].used) {
        S.cache[i].pins = 0;
        unlink_entry((int)i);
      }
    gdi_free(S.cache, GLYPH_SLOTS * sizeof(*S.cache));
    S.stats.store_bytes -= GLYPH_SLOTS * sizeof(*S.cache);
    S.cache = 0;
  }
  for (unsigned i = 0; i < S.count; i++)
    face_free(S.faces[i]);
  S.count = 0;
  S.loaded = 0;
  S.stats.faces = 0;
  S.stats.open_faces = 0;
  S.stats.face_bytes = 0;
  S.stats.glyph_bytes = 0;
  S.stats.glyphs = 0;
  S.stats.store_bytes = 0;
  R.tried = 0;
  for (unsigned i = 0; i < 7; i++)
    stock_fonts[i].resolved = 0, stock_fonts[i].face = 0;
}
