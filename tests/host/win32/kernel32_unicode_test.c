/* f5-20B (directive f5-20 amendment 1, item 1): the Unicode functions
 * Windows 98 refused, layered over the ANSI paths through code page 1252
 * (the ANSI functions are recording models here), and the production
 * CompareString/LCMapString A/W collation, GetLocaleInfoW and
 * GetStringTypeW. ASan/UBSan, no guest.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
// clang-format off
#include <w32/kernel32.h>
#include <w32/shims.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
// clang-format on
#define CHECK(c)                                                               \
  do {                                                                         \
    checks++;                                                                  \
    if (!(c)) {                                                                \
      fprintf(stderr, "kernel32 unicode line %u: %s (error %u)\n", __LINE__,   \
              #c, last_error);                                                 \
      abort();                                                                 \
    }                                                                          \
  } while (0)
static unsigned checks;
static DWORD last_error;
DWORD W32API GetLastError(void) { return last_error; }
void W32API SetLastError(DWORD e) { last_error = e; }
void *k32_alloc(DWORD n) { return calloc(1, n ? n : 1); }
void k32_free(void *p, DWORD n) {
  (void)n;
  free(p);
}
void *w32_copy(void *d, const void *s, uint32_t n) { return memmove(d, s, n); }
void *w32_fill(void *d, uint8_t v, uint32_t n) { return memset(d, v, n); }
uint32_t w32_strlen(const char *s) { return s ? (uint32_t)strlen(s) : 0; }
int w32_strcmp(const char *a, const char *b) { return strcmp(a, b); }
int w32_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
void w32_put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
void w32_diag(const char *e, const char *m, const char *s) {
  (void)e, (void)m, (void)s;
}

/* ---- recording ANSI layer ------------------------------------------- */
static char last_name[300];
static DWORD last_flags, calls;
static const void *last_env;
static char env_copy[64];
HANDLE W32API CreateFileA(const char *p, DWORD access, DWORD share,
                          SECURITY_ATTRIBUTES *sa, DWORD creation, DWORD flags,
                          HANDLE t) {
  (void)share, (void)sa, (void)t;
  calls++;
  snprintf(last_name, sizeof(last_name), "%s", p);
  last_flags = access ^ creation ^ flags;
  return (HANDLE)(uintptr_t)0x1234;
}
HANDLE W32API LoadLibraryExA(const char *n, HANDLE f, DWORD flags) {
  (void)f;
  calls++;
  snprintf(last_name, sizeof(last_name), "%s", n);
  last_flags = flags;
  return (HANDLE)(uintptr_t)0x10000000;
}
HANDLE W32API GetModuleHandleA(const char *n) {
  calls++;
  snprintf(last_name, sizeof(last_name), "%s", n ? n : "(null)");
  return (HANDLE)(uintptr_t)(n ? 0x70000000 : 0x400000);
}
/* The production truncation rule: the copied count, a terminator only
 * when the name fits. */
DWORD W32API GetModuleFileNameA(HANDLE h, char *out, DWORD cap) {
  (void)h;
  static const char name[] = "C:\\WINDOWS\\CAF\xc9.EXE";
  DWORD n = sizeof(name) - 1, copy = n < cap ? n : cap;
  memcpy(out, name, copy);
  if (n < cap)
    out[n] = 0;
  return copy;
}
HANDLE W32API CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL m, BOOL i,
                           const char *n) {
  (void)sa, (void)m, (void)i;
  calls++;
  snprintf(last_name, sizeof(last_name), "%s", n ? n : "(null)");
  return (HANDLE)(uintptr_t)0x44;
}
HANDLE W32API CreateMutexA(SECURITY_ATTRIBUTES *sa, BOOL o, const char *n) {
  (void)sa, (void)o;
  calls++;
  snprintf(last_name, sizeof(last_name), "%s", n ? n : "(null)");
  return (HANDLE)(uintptr_t)0x48;
}
BOOL W32API GetVersionExA(OSVERSIONINFOA *v) {
  if (v->size != sizeof(OSVERSIONINFOA) &&
      v->size != sizeof(OSVERSIONINFOEXA)) {
    SetLastError(87);
    return 0;
  }
  v->major = 4, v->minor = 10, v->build = 0x040a08ae, v->platform = 1;
  strcpy(v->csd, " A");
  if (v->size == sizeof(OSVERSIONINFOEXA)) {
    OSVERSIONINFOEXA *x = (OSVERSIONINFOEXA *)v;
    x->service_major = 0, x->service_minor = 0, x->suite = 0, x->product = 1;
  }
  return 1;
}
DWORD W32API GetEnvironmentVariableA(const char *n, char *out, DWORD cap) {
  static const char value[] = "C:\\WINDOWS;C:\\\xc0" "B";
  if (strcasecmp(n, "PATH")) {
    SetLastError(203);
    return 0;
  }
  DWORD len = sizeof(value) - 1;
  if (cap <= len)
    return len + 1;
  memcpy(out, value, len + 1);
  return len;
}
char *W32API GetCommandLineA(void) {
  static char command[] = "C:\\GAME.EXE /\xe9";
  return command;
}
struct startup_a {
  DWORD size;
  char *reserved, *desktop, *title;
  DWORD x, y, width, height, chars_x, chars_y, fill, flags;
  uint16_t show, reserved_bytes;
  uint8_t *reserved2;
  HANDLE input, output, error;
};
static struct startup_a last_startup;
static char last_title[64], last_command[64];
BOOL W32API CreateProcessA(const char *app, char *cmd, SECURITY_ATTRIBUTES *p,
                           SECURITY_ATTRIBUTES *t, BOOL inherit, DWORD flags,
                           void *env, const char *dir, struct startup_a *s,
                           void *out) {
  (void)p, (void)t, (void)inherit, (void)out;
  calls++;
  snprintf(last_name, sizeof(last_name), "%s|%s", app ? app : "(null)",
           dir ? dir : "(null)");
  snprintf(last_command, sizeof(last_command), "%s", cmd ? cmd : "(null)");
  last_flags = flags;
  last_env = env;
  memset(env_copy, 0x55, sizeof(env_copy));
  for (size_t i = 0; env && i < sizeof(env_copy); i++) {
    env_copy[i] = ((const char *)env)[i];
    if (i && !env_copy[i] && !env_copy[i - 1])
      break;
  }
  last_startup = *s;
  snprintf(last_title, sizeof(last_title), "%s",
           s->title ? s->title : "(null)");
  return s->size == sizeof(*s);
}
extern HANDLE W32API CreateFileW(const WCHAR *, DWORD, DWORD,
                                 SECURITY_ATTRIBUTES *, DWORD, DWORD, HANDLE);
extern HANDLE W32API LoadLibraryW(const WCHAR *);
extern HANDLE W32API LoadLibraryExW(const WCHAR *, HANDLE, DWORD);
extern HANDLE W32API GetModuleHandleW(const WCHAR *);
extern DWORD W32API GetModuleFileNameW(HANDLE, WCHAR *, DWORD);
extern HANDLE W32API CreateEventW(SECURITY_ATTRIBUTES *, BOOL, BOOL,
                                  const WCHAR *);
extern HANDLE W32API CreateMutexW(SECURITY_ATTRIBUTES *, BOOL, const WCHAR *);
extern BOOL W32API GetVersionExW(OSVERSIONINFOW *);
extern DWORD W32API GetEnvironmentVariableW(const WCHAR *, WCHAR *, DWORD);
extern WCHAR *W32API GetCommandLineW(void);
struct startup_w {
  DWORD size;
  WCHAR *reserved, *desktop, *title;
  DWORD x, y, width, height, chars_x, chars_y, fill, flags;
  uint16_t show, reserved_bytes;
  uint8_t *reserved2;
  HANDLE input, output, error;
};
extern BOOL W32API CreateProcessW(const WCHAR *, WCHAR *, SECURITY_ATTRIBUTES *,
                                  SECURITY_ATTRIBUTES *, BOOL, DWORD, void *,
                                  const WCHAR *, struct startup_w *, void *);
extern int W32API GetLocaleInfoW(DWORD, DWORD, WCHAR *, int);
extern int W32API GetLocaleInfoA(DWORD, DWORD, char *, int);
extern BOOL W32API GetStringTypeW(DWORD, const WCHAR *, int, uint16_t *);
extern BOOL W32API GetStringTypeA(DWORD, DWORD, const char *, int, uint16_t *);
extern int W32API CompareStringW(DWORD, DWORD, const WCHAR *, int,
                                 const WCHAR *, int);
extern int W32API CompareStringA(DWORD, DWORD, const char *, int, const char *,
                                 int);
extern int W32API LCMapStringW(DWORD, DWORD, const WCHAR *, int, WCHAR *, int);
extern int W32API LCMapStringA(DWORD, DWORD, const char *, int, char *, int);

/* UTF-16 literal from a code-page-1252 C string (test convenience). */
static const WCHAR *W(const char *s) {
  static WCHAR pool[16][300];
  static unsigned at;
  WCHAR *w = pool[at++ % 16];
  int n = MultiByteToWideChar(1252, 0, s, -1, w, 300);
  CHECK(n > 0);
  return w;
}
static int wide_equal(const WCHAR *w, const char *a) {
  return !memcmp(w, W(a), (strlen(a) + 1) * 2);
}

static void files_modules_objects(void) {
  /* CreateFileW: code-page names pass, others fail before the ANSI call. */
  calls = 0;
  CHECK(CreateFileW(W("C:\\Caf\xe9\\x.txt"), 1, 0, 0, 3, 0, 0) ==
            (HANDLE)(uintptr_t)0x1234 &&
        !strcmp(last_name, "C:\\Caf\xe9\\x.txt") && calls == 1);
  WCHAR cjk[] = {'C', ':', '\\', 0x4e2d, 0};
  CHECK(CreateFileW(cjk, 1, 0, 0, 3, 0, 0) == K32_INVALID &&
        last_error == 123 && calls == 1);
  CHECK(CreateFileW(0, 1, 0, 0, 3, 0, 0) == K32_INVALID && last_error == 87);
  WCHAR longer[300];
  for (int i = 0; i < 299; i++)
    longer[i] = 'a';
  longer[299] = 0;
  CHECK(CreateFileW(longer, 1, 0, 0, 3, 0, 0) == K32_INVALID &&
        last_error == 206 && calls == 1);
  /* Modules. */
  CHECK(LoadLibraryW(W("SHELL32.DLL")) == (HANDLE)(uintptr_t)0x10000000 &&
        !strcmp(last_name, "SHELL32.DLL") && !last_flags);
  CHECK(LoadLibraryExW(W("RES.DLL"), 0, 2) == (HANDLE)(uintptr_t)0x10000000 &&
        last_flags == 2);
  CHECK(!LoadLibraryExW(cjk, 0, 0) && last_error == 123);
  CHECK(GetModuleHandleW(0) == (HANDLE)(uintptr_t)0x400000 &&
        !strcmp(last_name, "(null)"));
  CHECK(GetModuleHandleW(W("KERNEL32")) == (HANDLE)(uintptr_t)0x70000000 &&
        !strcmp(last_name, "KERNEL32"));
  WCHAR path[64];
  memset(path, 0xaa, sizeof(path));
  CHECK(GetModuleFileNameW(0, path, 64) == 19 &&
        wide_equal(path, "C:\\WINDOWS\\CAF\xc9.EXE") && path[11 + 3] == 0xc9);
  memset(path, 0xaa, sizeof(path));
  CHECK(GetModuleFileNameW(0, path, 5) == 5 && path[4] == 'I' &&
        path[5] == 0xaaaa);
  CHECK(!GetModuleFileNameW(0, path, 0) && last_error == 87);
  /* Named objects: optional names. */
  CHECK(CreateEventW(0, 1, 0, W("Ev\xe9nt")) == (HANDLE)(uintptr_t)0x44 &&
        !strcmp(last_name, "Ev\xe9nt"));
  CHECK(CreateEventW(0, 1, 0, 0) == (HANDLE)(uintptr_t)0x44 &&
        !strcmp(last_name, "(null)"));
  CHECK(CreateMutexW(0, 0, W("Mutex")) == (HANDLE)(uintptr_t)0x48 &&
        !strcmp(last_name, "Mutex"));
  CHECK(!CreateMutexW(0, 0, cjk) && last_error == 123);
  /* GetVersionExW still reports Windows 98 (platform 1, 4.10). */
  OSVERSIONINFOW v;
  memset(&v, 0xa5, sizeof(v));
  v.size = sizeof(v);
  CHECK(GetVersionExW(&v) && v.major == 4 && v.minor == 10 &&
        v.platform == 1 && v.build == 0x040a08ae && wide_equal(v.csd, " A") &&
        !v.csd[127]);
  OSVERSIONINFOEXW x;
  memset(&x, 0xa5, sizeof(x));
  x.base.size = sizeof(x);
  CHECK(GetVersionExW(&x.base) && x.product == 1 && !x.suite &&
        x.base.platform == 1);
  v.size = sizeof(OSVERSIONINFOA);
  CHECK(!GetVersionExW(&v) && last_error == 87);
  /* Environment: length, required size, missing variable. */
  WCHAR value[64];
  CHECK(GetEnvironmentVariableW(W("path"), value, 64) == 16 &&
        wide_equal(value, "C:\\WINDOWS;C:\\\xc0" "B") && value[14] == 0xc0);
  CHECK(GetEnvironmentVariableW(W("PATH"), value, 16) == 17);
  CHECK(GetEnvironmentVariableW(W("PATH"), 0, 0) == 17);
  CHECK(!GetEnvironmentVariableW(W("NOPE"), value, 64) && last_error == 203 &&
        !value[0]);
  CHECK(!GetEnvironmentVariableW(cjk, value, 64) && last_error == 123);
  /* Command line: one widened copy for the process. */
  WCHAR *command = GetCommandLineW();
  CHECK(command && wide_equal(command, "C:\\GAME.EXE /\xe9") &&
        GetCommandLineW() == command);
  /* CreateProcessW: command, title, directory and a Unicode environment
   * block become ANSI; CREATE_UNICODE_ENVIRONMENT is cleared. */
  struct startup_w s = {.size = sizeof(s), .title = (WCHAR *)W("T\xeft")};
  WCHAR cmd[32];
  memcpy(cmd, W("GAME.EXE /\xe9"), 24);
  static const WCHAR env[] = {'A', '=', '1', 0, 'B', '=', 0xe9, 0, 0};
  calls = 0;
  CHECK(CreateProcessW(0, cmd, 0, 0, 0, 0x400 | 0x10, (void *)env,
                       W("C:\\G"), &s, 0) &&
        calls == 1 && !strcmp(last_command, "GAME.EXE /\xe9") &&
        !strcmp(last_name, "(null)|C:\\G") && last_flags == 0x10 &&
        !strcmp(last_title, "T\xeft") &&
        last_startup.size == sizeof(struct startup_a) && last_env != env &&
        !memcmp(env_copy, "A=1\0B=\xe9\0\0", 9));
  CHECK(CreateProcessW(W("GAME.EXE"), 0, 0, 0, 0, 0, (void *)"A=1\0\0", 0, &s,
                       0) &&
        !strcmp(last_name, "GAME.EXE|(null)") &&
        !strcmp(last_command, "(null)"));
  static const WCHAR bad_env[] = {'A', '=', 0x4e2d, 0, 0};
  calls = 0;
  CHECK(!CreateProcessW(0, cmd, 0, 0, 0, 0x400, (void *)bad_env, 0, &s, 0) &&
        last_error == 1113 && !calls);
  s.size = 4;
  CHECK(!CreateProcessW(0, cmd, 0, 0, 0, 0, 0, 0, &s, 0) && last_error == 87 &&
        !calls);
  puts("PASS kernel32 unicode files modules objects version environment command line process");
}

static void locale_types(void) {
  WCHAR out[64];
  char a[64];
  CHECK(GetLocaleInfoW(0x409, 0x1001, out, 64) == 8 &&
        wide_equal(out, "English"));
  CHECK(GetLocaleInfoA(0x409, 0x1001, a, 64) == 8 && !strcmp(a, "English"));
  CHECK(GetLocaleInfoW(0x400, 0x1001, 0, 0) == 8);
  CHECK(!GetLocaleInfoW(0x409, 0x1001, out, 3) && last_error == 122);
  DWORD number = 0;
  CHECK(GetLocaleInfoW(0x409, 0x20000000 | 0x0b, (WCHAR *)&number, 2) == 2 &&
        number == 437);
  CHECK(!GetLocaleInfoW(0x411, 0x1001, out, 64) && last_error == 87);
  uint16_t types[4], typesa[4];
  CHECK(GetStringTypeW(1, W("aZ1 "), 4, types));
  CHECK(GetStringTypeA(0x409, 1, "aZ1 ", 4, typesa) &&
        !memcmp(types, typesa, sizeof(types)));
  CHECK((types[0] & 0x102) == 0x102 && (types[1] & 0x101) == 0x101 &&
        (types[2] & 4) && (types[3] & 8));
  puts("PASS kernel32 unicode locale info string types");
}

static int cmp(DWORD flags, const char *a, const char *b) {
  int w = CompareStringW(0x409, flags, W(a), -1, W(b), -1);
  int n = CompareStringA(0x409, flags, a, -1, b, -1);
  CHECK(w == n); /* the A and W forms agree on the code page repertoire */
  return w;
}
static void collation(void) {
  /* Levels: base letters, then diacritics, then case (lowercase first). */
  CHECK(cmp(0, "abc", "abc") == 2);
  CHECK(cmp(0, "abc", "abd") == 1 && cmp(0, "abd", "abc") == 3);
  CHECK(cmp(0, "ab", "abc") == 1);
  CHECK(cmp(0, "a", "A") == 1 && cmp(0, "A", "b") == 1);
  CHECK(cmp(1, "Hello", "hELLO") == 2);
  CHECK(cmp(0, "cote", "c\xf4te") == 1 && cmp(0, "c\xf4te", "coter") == 1);
  CHECK(cmp(2, "cote", "c\xf4te") == 2 && cmp(3, "COTE", "c\xf4te") == 2);
  CHECK(cmp(1, "\xc9t\xe9", "\xe9T\xc9") == 2);
  CHECK(cmp(1, "\x9f", "\xff") == 2 && cmp(1, "\x8a", "\x9a") == 2);
  /* Symbols before digits before letters. */
  CHECK(cmp(0, " ", "!") == 1 && cmp(0, "!", "0") == 1 && cmp(0, "9", "a") == 1);
  /* Ligatures expand: AE, SS. */
  CHECK(cmp(2, "\xe6", "ae") == 2 && cmp(0, "\xdf", "ss") == 2);
  CHECK(cmp(0, "\xe6", "af") == 1);
  /* Word sort: hyphen and apostrophe count last; string sort: symbols. */
  CHECK(cmp(0, "coop", "co-op") == 1 && cmp(0, "co-op", "cop") == 1);
  CHECK(cmp(0x1000, "co-op", "coop") == 1 && cmp(0x1000, "co-op", "cop") == 1);
  CHECK(cmp(4, "a.b", "ab") == 2 && cmp(4, "a b", "a-b") == 2);
  /* Counted strings and edge cases. */
  CHECK(CompareStringW(0x409, 0, W("abcd"), 2, W("abxx"), 2) == 2);
  CHECK(CompareStringA(0x409, 0, "", 0, "", -1) == 2);
  CHECK(CompareStringA(0x409, 0, "a\0", 2, "a", 1) == 2); /* NUL ignorable */
  CHECK(!CompareStringW(0x409, 0x40000000, W("a"), -1, W("a"), -1) &&
        last_error == 1004);
  CHECK(!CompareStringW(0x411, 0, W("a"), -1, W("a"), -1) && last_error == 87);
  CHECK(!CompareStringA(0x409, 0, 0, -1, "a", -1) && last_error == 87);
  CHECK(CompareStringW(0x800, 0, W("a"), -1, W("b"), -1) == 1);
  CHECK(CompareStringW(0x7f, 1, W("A"), -1, W("a"), -1) == 2);
  /* Outside code page 1252: Greek/Cyrillic case-fold, code-point order. */
  WCHAR alpha[] = {0x391, 0}, alpha_lower[] = {0x3b1, 0}, be[] = {0x431, 0};
  CHECK(CompareStringW(0x409, 1, alpha, -1, alpha_lower, -1) == 2);
  CHECK(CompareStringW(0x409, 0, W("z"), -1, alpha, -1) == 1);
  CHECK(CompareStringW(0x409, 0, alpha_lower, -1, be, -1) == 1);
  puts("PASS kernel32 unicode CompareString levels word sort flags A/W");
}

static void mapping(void) {
  WCHAR out[64];
  char a[64];
  CHECK(LCMapStringW(0x409, 0x200, W("caf\xe9 \xff \x9a!"), -1, out, 64) ==
            10 &&
        wide_equal(out, "CAF\xc9 \x9f \x8a!"));
  CHECK(LCMapStringA(0x409, 0x200, "caf\xe9 \xff \x9a!", -1, a, 64) == 10 &&
        !strcmp(a, "CAF\xc9 \x9f \x8a!"));
  CHECK(LCMapStringA(0x409, 0x100, "\xc0\xc9\x8c\xdf", 4, a, 64) == 4 &&
        !memcmp(a, "\xe0\xe9\x9c\xdf", 4));
  WCHAR greek[] = {0x3b1, 0x431, 0x101, 0x17e, 0};
  CHECK(LCMapStringW(0x409, 0x200, greek, -1, out, 64) == 5 &&
        out[0] == 0x391 && out[1] == 0x411 && out[2] == 0x100 &&
        out[3] == 0x17d && !out[4]);
  CHECK(LCMapStringW(0x409, 0x100, out, 4, out, 64) == 4 && out[0] == 0x3b1 &&
        out[3] == 0x17e); /* in place */
  CHECK(LCMapStringW(0x409, 0x200, W("abc"), 3, 0, 0) == 3);
  CHECK(!LCMapStringW(0x409, 0x200, W("abc"), -1, out, 3) && last_error == 122);
  CHECK(LCMapStringW(0x409, 0x800, W("AB"), 2, out, 64) == 2 &&
        out[0] == 0x4100 && out[1] == 0x4200);
  CHECK(!LCMapStringA(0x409, 0x800, "AB", 2, a, 64) && last_error == 1004);
  CHECK(!LCMapStringW(0x409, 0x300, W("a"), -1, out, 64) && last_error == 1004);
  CHECK(!LCMapStringW(0x409, 0x600, W("a"), -1, out, 64) && last_error == 1004);
  CHECK(!LCMapStringW(0x409, 0x200, W("a"), 0, out, 64) && last_error == 87);
  CHECK(LCMapStringW(0x409, 4, W("a,b.c"), -1, out, 64) == 4 &&
        wide_equal(out, "abc"));
  CHECK(LCMapStringW(0x409, 2, W("\xe9t\xc9"), -1, out, 64) == 4 &&
        wide_equal(out, "etE"));
  /* Sort keys order like CompareString, for A and W, with flags. */
  static const char *const words[] = {
      "", " ", "!", "0", "9", "a", "A", "ab", "aB", "abc", "\xe0", "\xc0",
      "ae", "\xe6", "af", "co-op", "coop", "cop", "c\xf4te", "cote", "coter",
      "ss", "\xdf", "z", "\x9a", "\x8a", "s", "t", "\xff", "\x9f", "y"};
  static const DWORD sets[] = {0, 1, 2, 3, 4, 0x1000, 0x1001};
  size_t n = sizeof(words) / sizeof(*words);
  for (size_t f = 0; f < sizeof(sets) / sizeof(*sets); f++)
    for (size_t i = 0; i < n; i++)
      for (size_t j = 0; j < n; j++) {
        uint8_t ka[128], kb[128], wa[128];
        int la = LCMapStringA(0x409, 0x400 | sets[f], words[i], -1, (char *)ka,
                              sizeof(ka));
        int lb = LCMapStringA(0x409, 0x400 | sets[f], words[j], -1, (char *)kb,
                              sizeof(kb));
        int lw = LCMapStringW(0x409, 0x400 | sets[f], W(words[i]), -1,
                              (WCHAR *)wa, sizeof(wa));
        CHECK(la > 0 && lb > 0 && lw == la && !memcmp(ka, wa, (size_t)la));
        CHECK(LCMapStringA(0x409, 0x400 | sets[f], words[i], -1, 0, 0) == la);
        int m = memcmp(ka, kb, (size_t)(la < lb ? la : lb));
        int key = m ? (m < 0 ? 1 : 3) : la == lb ? 2 : la < lb ? 1 : 3;
        CHECK(key == cmp(sets[f], words[i], words[j]));
      }
  CHECK(!LCMapStringA(0x409, 0x400, "abc", -1, a, 3) && last_error == 122);
  CHECK(!LCMapStringA(0x409, 0x500, "abc", -1, a, 64) && last_error == 1004);
  CHECK(!LCMapStringA(0x409, 0x1000 | 0x200, "abc", -1, a, 64) &&
        last_error == 1004);
  puts("PASS kernel32 unicode LCMapString case byte reversal sort keys");
}

int main(void) {
  files_modules_objects();
  locale_types();
  collation();
  mapping();
  printf("PASS kernel32 unicode checks=%u\n", checks);
  return 0;
}
