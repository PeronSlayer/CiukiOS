/* f5-20A KERNEL32 entry points for the reused Wine DLLs: Unicode layer,
 * search/executable/environment/Win16 file API, NLS, FormatMessage, thread
 * pool/once/timers, delay-load, atoms, time zones, console, W21 failures.
 * Production sources over a small in-memory ANSI file model; the time and
 * code-page sources are the production ones. ASan/UBSan, no guest.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
// clang-format off
#include <w32/kernel32.h>
#include <w32/server.h>
#include <w32/shims.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
// clang-format on
#define CHECK(c)                                                               \
  do {                                                                         \
    checks++;                                                                  \
    if (!(c)) {                                                                \
      fprintf(stderr, "kernel32 upper line %u: %s (error %u)\n", __LINE__, #c, \
              last_error);                                                     \
      abort();                                                                 \
    }                                                                          \
  } while (0)
static unsigned checks;
static DWORD last_error, diagnostics;
static struct w32_teb teb;
static struct w32_peb peb;
static struct w32_process_parameters parameters;
_Thread_local struct w32_teb *w32_model_teb = &teb;
static struct ciuki_w32_compat compat;
const struct ciuki_w32_compat *w32_shim_current(void) { return &compat; }
void w32_shim_version(struct ciuki_w32_identity *i) { (void)i; }
DWORD W32API GetLastError(void) { return last_error; }
void W32API SetLastError(DWORD e) { last_error = e; }
int k32_status(int32_t s) {
  if (s)
    last_error = 87;
  return !s;
}
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
int w32_copy_string(char *d, uint32_t n, const char *s) {
  if (strlen(s) >= n)
    return 0;
  strcpy(d, s);
  return 1;
}
void w32_put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static char last_diag[128];
void w32_diag(const char *e, const char *m, const char *s) {
  (void)e;
  (void)m;
  snprintf(last_diag, sizeof(last_diag), "%s", s);
  diagnostics++;
}
uint32_t w32_shim_stub(volatile uint32_t *once, const char *dll, const char *name,
                       uint32_t convention) {
  if (!*once) {
    *once = 1;
    w32_diag("stub", dll, name);
  }
  last_error = convention == 5 ? 5 : 120;
  return convention == 1   ? 0x80004001u
         : convention == 3 ? 120
         : convention == 4 ? UINT32_MAX
                           : 0;
}
WCHAR W32API RtlUpcaseUnicodeChar(WCHAR c) {
  return (c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7) ? c - 32 : c;
}
NTSTATUS W32API RtlInitializeCriticalSectionAndSpinCount(RTL_CRITICAL_SECTION *cs, ULONG n) {
  cs->SpinCount = n;
  return 0;
}
/* Model process heap for the f5-20C debug blocks of InitializeCriticalSectionEx. */
static int debug_blocks, fail_heap;
HANDLE W32API GetProcessHeap(void) { return &peb; }
void *W32API RtlAllocateHeap(void *heap, ULONG flags, uint32_t n) {
  if (heap != &peb || flags != 8 || fail_heap)
    return 0;
  debug_blocks++;
  return calloc(1, n);
}
BOOLEAN W32API RtlFreeHeap(void *heap, ULONG flags, void *p) {
  (void)flags;
  if (heap != &peb || !p)
    return 0;
  debug_blocks--;
  free(p);
  return 1;
}
/* Counter at 1 MHz (QueryPerformance*, GetTickCount64) and a fixed clock:
 * 2026-10-19 12:34:56.789 UTC. */
static uint64_t counter = 3723004000ull;
int32_t w32_ciuki_call(void *record, uint32_t n) {
  struct ciuki_w32_header *h = record;
  if (h->operation == W32_COUNTER) {
    struct ciuki_w32_counter *c = record;
    c->value_lo = (uint32_t)counter;
    c->value_hi = (uint32_t)(counter >> 32);
    c->frequency_lo = 1000000;
    return 0;
  }
  CHECK(h->operation == W32_SYSTEM && n == sizeof(struct ciuki_w32_system_args));
  struct ciuki_w32_system_args *a = record;
  SYSTEMTIME s = {2026, 10, 1, 19, 12, 34, 56, 789};
  FILETIME f;
  SystemTimeToFileTime(&s, &f);
  a->low = f.low;
  a->high = f.high;
  return 0;
}
void W32API GetLocalTime(SYSTEMTIME *);
/* In-memory ANSI file model. */
struct file {
  char path[262];
  DWORD attributes;
  uint8_t *data;
  DWORD size;
};
static struct file files[32];
static struct {
  struct file *f;
  DWORD at;
  int used;
} handles[16];
static char cwd[262] = "C:\\CWD";
static struct file *lookup(const char *full) {
  for (unsigned i = 0; i < 32; i++)
    if (files[i].path[0] && !strcasecmp(files[i].path, full))
      return &files[i];
  return 0;
}
static void put_file(const char *path, const void *data, DWORD n, DWORD attributes) {
  for (unsigned i = 0; i < 32; i++)
    if (!files[i].path[0] || !strcasecmp(files[i].path, path)) {
      free(files[i].data);
      snprintf(files[i].path, sizeof(files[i].path), "%s", path);
      files[i].data = malloc(n ? n : 1);
      memcpy(files[i].data, data, n);
      files[i].size = n;
      files[i].attributes = attributes;
      return;
    }
  CHECK(0);
}
DWORD W32API GetFullPathNameA(const char *p, DWORD cap, char *out, char **part) {
  char full[600];
  if (strchr(p, ':'))
    snprintf(full, sizeof(full), "%s", p);
  else if (p[0] == '\\')
    snprintf(full, sizeof(full), "C:%s", p);
  else
    snprintf(full, sizeof(full), "%s\\%s", cwd, p);
  DWORD n = (DWORD)strlen(full);
  if (n >= 260) {
    last_error = 206;
    return 0;
  }
  if (cap <= n)
    return n + 1;
  strcpy(out, full);
  if (part) {
    char *s = strrchr(out, '\\');
    *part = s ? s + 1 : out;
  }
  return n;
}
DWORD W32API GetFileAttributesA(const char *p) {
  char full[262];
  struct file *f = GetFullPathNameA(p, sizeof(full), full, 0) ? lookup(full) : 0;
  if (!f) {
    last_error = 2;
    return ~0u;
  }
  return f->attributes;
}
HANDLE W32API CreateFileA(const char *p, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa,
                          DWORD disposition, DWORD flags, HANDLE t) {
  (void)access;
  (void)share;
  (void)sa;
  (void)flags;
  (void)t;
  char full[262];
  if (!GetFullPathNameA(p, sizeof(full), full, 0))
    return K32_INVALID;
  struct file *f = lookup(full);
  if (!f && disposition == 3) {
    last_error = 2;
    return K32_INVALID;
  }
  if (f && disposition == 1) {
    last_error = 80;
    return K32_INVALID;
  }
  if (!f || disposition == 2) {
    put_file(full, "", 0, 0x20);
    f = lookup(full);
  }
  for (unsigned i = 0; i < 16; i++)
    if (!handles[i].used) {
      handles[i].used = 1;
      handles[i].f = f;
      handles[i].at = 0;
      return (HANDLE)(uintptr_t)(0x100 + i * 4);
    }
  last_error = 4;
  return K32_INVALID;
}
static int slot(HANDLE h) {
  uintptr_t v = (uintptr_t)h;
  if (v < 0x100 || v >= 0x100 + 16 * 4 || !handles[(v - 0x100) / 4].used) {
    last_error = 6;
    return -1;
  }
  return (int)((v - 0x100) / 4);
}
BOOL W32API CloseHandle(HANDLE h) {
  int i = slot(h);
  if (i < 0)
    return 0;
  handles[i].used = 0;
  return 1;
}
BOOL W32API ReadFile(HANDLE h, void *p, DWORD n, DWORD *done, OVERLAPPED *ov) {
  (void)ov;
  int i = slot(h);
  if (i < 0)
    return 0;
  struct file *f = handles[i].f;
  DWORD at = handles[i].at, k = at < f->size ? f->size - at : 0;
  if (k > n)
    k = n;
  memcpy(p, f->data + at, k);
  handles[i].at += k;
  *done = k;
  return 1;
}
BOOL W32API WriteFile(HANDLE h, const void *p, DWORD n, DWORD *done, OVERLAPPED *ov) {
  (void)ov;
  int i = slot(h);
  if (i < 0)
    return 0;
  struct file *f = handles[i].f;
  DWORD end = handles[i].at + n;
  if (end > f->size) {
    f->data = realloc(f->data, end);
    memset(f->data + f->size, 0, end - f->size);
    f->size = end;
  }
  memcpy(f->data + handles[i].at, p, n);
  handles[i].at = end;
  *done = n;
  return 1;
}
DWORD W32API SetFilePointer(HANDLE h, LONG low, LONG *high, DWORD method) {
  (void)high;
  int i = slot(h);
  if (i < 0)
    return ~0u;
  LONG base = method == 0 ? 0 : method == 1 ? (LONG)handles[i].at : (LONG)handles[i].f->size;
  handles[i].at = (DWORD)(base + low);
  return handles[i].at;
}
BOOL W32API SetEndOfFile(HANDLE h) {
  int i = slot(h);
  if (i < 0)
    return 0;
  handles[i].f->size = handles[i].at;
  return 1;
}
BOOL W32API DeleteFileA(const char *p) {
  char full[262];
  struct file *f = GetFullPathNameA(p, sizeof(full), full, 0) ? lookup(full) : 0;
  if (!f) {
    last_error = 2;
    return 0;
  }
  f->path[0] = 0;
  return 1;
}
DWORD W32API GetModuleFileNameA(HANDLE h, char *out, DWORD n) {
  CHECK(!h);
  return (DWORD)snprintf(out, n, "C:\\APP\\GAME.EXE");
}
DWORD W32API GetSystemDirectoryA(char *p, DWORD n) {
  const char *s = "C:\\WINDOWS\\SYSTEM";
  if (n <= strlen(s))
    return (DWORD)strlen(s) + 1;
  strcpy(p, s);
  return (DWORD)strlen(s);
}
DWORD W32API GetWindowsDirectoryA(char *p, DWORD n) {
  const char *s = "C:\\WINDOWS";
  if (n <= strlen(s))
    return (DWORD)strlen(s) + 1;
  strcpy(p, s);
  return (DWORD)strlen(s);
}
static const char *environment[][2] = {{"PATH", "C:\\TOOLS;C:\\BIN"}, {"TEMP", "C:\\TEMP"},
                                       {"WINDIR", "C:\\WINDOWS"}, {"EMPTY", ""},
                                       {"CAFE", "caf\xe9"}};
DWORD W32API GetEnvironmentVariableA(const char *name, char *out, DWORD cap) {
  for (unsigned i = 0; i < 5; i++)
    if (!strcasecmp(name, environment[i][0])) {
      DWORD n = (DWORD)strlen(environment[i][1]);
      if (cap <= n)
        return n ? n + 1 : 0;
      strcpy(out, environment[i][1]);
      return n;
    }
  last_error = 203;
  return 0;
}
/* Recording mocks for the ANSI file, profile and object entry points. */
static char seen[4][600];
static DWORD seen_flags;
static void record(int i, const char *s) { snprintf(seen[i], sizeof(seen[i]), "%s", s ? s : "<null>"); }
BOOL W32API CopyFileA(const char *a, const char *b, BOOL fail) {
  record(0, a);
  record(1, b);
  seen_flags = (DWORD)fail;
  return 1;
}
BOOL W32API CreateDirectoryA(const char *p, SECURITY_ATTRIBUTES *sa) {
  (void)sa;
  record(0, p);
  if (lookup(p)) {
    last_error = 183;
    return 0;
  }
  return 1;
}
BOOL W32API RemoveDirectoryA(const char *p) {
  record(0, p);
  return 1;
}
BOOL W32API SetCurrentDirectoryA(const char *p) {
  record(0, p);
  return 1;
}
BOOL W32API SetFileAttributesA(const char *p, DWORD a) {
  record(0, p);
  seen_flags = a;
  return 1;
}
BOOL W32API GetFileAttributesExA(const char *p, DWORD level, void *out) {
  record(0, p);
  if (level) {
    last_error = 87;
    return 0;
  }
  WIN32_FILE_ATTRIBUTE_DATA d = {.attributes = 0x21, .size_low = 9};
  memcpy(out, &d, sizeof(d));
  return 1;
}
BOOL W32API MoveFileExA(const char *a, const char *b, DWORD flags) {
  record(0, a);
  record(1, b);
  seen_flags = flags;
  return 1;
}
BOOL W32API MoveFileA(const char *a, const char *b) { return MoveFileExA(a, b, 0x55); }
static int find_calls;
HANDLE W32API FindFirstFileA(const char *p, WIN32_FIND_DATAA *d) {
  record(0, p);
  memset(d, 0, sizeof(*d));
  d->attributes = 0x20;
  d->size_low = 77;
  strcpy(d->name, "caf\xe9 long name.txt");
  strcpy(d->alternate, "CAFLON~1.TXT");
  find_calls = 0;
  return (HANDLE)(uintptr_t)0x400;
}
BOOL W32API FindNextFileA(HANDLE h, WIN32_FIND_DATAA *d) {
  CHECK(h == (HANDLE)(uintptr_t)0x400);
  if (find_calls++) {
    last_error = 18;
    return 0;
  }
  memset(d, 0, sizeof(*d));
  strcpy(d->name, "\x80uro");
  return 1;
}
DWORD W32API GetCurrentDirectoryA(DWORD n, char *p) {
  if (n <= strlen(cwd))
    return (DWORD)strlen(cwd) + 1;
  strcpy(p, cwd);
  return (DWORD)strlen(cwd);
}
DWORD W32API GetLongPathNameA(const char *p, char *out, DWORD n) {
  if (strcasecmp(p, "C:\\PROGRA~1")) {
    last_error = 2;
    return 0;
  }
  return (DWORD)snprintf(out, n, "C:\\Program Files");
}
DWORD W32API GetShortPathNameA(const char *p, char *out, DWORD n) {
  if (strcasecmp(p, "C:\\Program Files")) {
    last_error = 2;
    return 0;
  }
  return (DWORD)snprintf(out, n, "C:\\PROGRA~1");
}
DWORD W32API GetTempPathA(DWORD n, char *out) { return (DWORD)snprintf(out, n, "C:\\TEMP\\"); }
DWORD W32API GetTempFileNameA(const char *path, const char *prefix, DWORD unique, char *out) {
  if (!prefix) {
    last_error = 87;
    return 0;
  }
  DWORD n = unique ? unique : 1;
  sprintf(out, "%s%s%s%04X.TMP", path, path[strlen(path) - 1] == '\\' ? "" : "\\", prefix,
          (unsigned)n);
  if (!unique)
    put_file(out, "", 0, 0x20);
  return n;
}
DWORD W32API GetLogicalDriveStringsA(DWORD n, char *out) {
  if (n <= 8)
    return 9;
  memcpy(out, "A:\\\0C:\\\0\0", 9);
  return 8;
}
BOOL W32API GetDiskFreeSpaceA(const char *r, DWORD *a, DWORD *b, DWORD *c, DWORD *d) {
  record(0, r);
  *a = 8;
  *b = 512;
  *c = 100;
  *d = 200;
  return 1;
}
BOOL W32API GetDiskFreeSpaceExA(const char *r, uint64_t *a, uint64_t *t, uint64_t *f) {
  record(0, r);
  if (a)
    *a = 1;
  if (t)
    *t = 2;
  if (f)
    *f = 3;
  return 1;
}
DWORD W32API GetDriveTypeA(const char *r) {
  record(0, r);
  return 3;
}
BOOL W32API GetVolumeInformationA(const char *r, char *label, DWORD ls, DWORD *serial,
                                  DWORD *comp, DWORD *flags, char *fs, DWORD fss) {
  record(0, r);
  snprintf(label, ls, "CIUKI\xe9");
  snprintf(fs, fss, "FAT32");
  *serial = 0x1234;
  *comp = 255;
  *flags = 6;
  return 1;
}
DWORD W32API GetPrivateProfileStringA(const char *s, const char *k, const char *d, char *out,
                                      DWORD n, const char *f) {
  record(0, s);
  record(1, k);
  record(2, d);
  record(3, f);
  if (!s) {
    memcpy(out, "one\0tw\xe9\0\0", n < 9 ? n : 9);
    return 8 < n ? 7 : n - 2;
  }
  const char *v = "valu\xe9";
  if (n <= strlen(v)) {
    memcpy(out, v, n - 1);
    out[n - 1] = 0;
    return n - 1;
  }
  strcpy(out, v);
  return 5;
}
DWORD W32API GetProfileStringA(const char *s, const char *k, const char *d, char *out, DWORD n) {
  return GetPrivateProfileStringA(s, k, d, out, n, "WIN.INI");
}
BOOL W32API WritePrivateProfileStringA(const char *s, const char *k, const char *v,
                                       const char *f) {
  record(0, s);
  record(1, k);
  record(2, v);
  record(3, f);
  return 1;
}
DWORD W32API GetPrivateProfileIntA(const char *s, const char *k, int d, const char *f) {
  record(0, s);
  record(1, k);
  record(3, f);
  return (DWORD)(d + 1);
}
BOOL W32API GetPrivateProfileStructA(const char *s, const char *k, void *out, DWORD n,
                                     const char *f) {
  record(0, s);
  record(1, k);
  record(3, f);
  memset(out, 0x5a, n);
  return 1;
}
BOOL W32API WritePrivateProfileStructA(const char *s, const char *k, const void *data, DWORD n,
                                       const char *f) {
  (void)data;
  record(0, s);
  record(1, k);
  record(3, f);
  seen_flags = n;
  return 1;
}
void W32API OutputDebugStringA(const char *s) {
  record(0, s);
  last_error = 0xdead;
}
HANDLE W32API CreateSemaphoreA(SECURITY_ATTRIBUTES *sa, LONG i, LONG m, const char *n) {
  (void)sa;
  record(0, n);
  seen_flags = (DWORD)(i * 100 + m);
  return (HANDLE)(uintptr_t)0x500;
}
HANDLE W32API OpenSemaphoreA(DWORD a, BOOL inherit, const char *n) {
  (void)inherit;
  record(0, n);
  seen_flags = a;
  return (HANDLE)(uintptr_t)0x504;
}
HANDLE W32API OpenEventA(DWORD a, BOOL inherit, const char *n) {
  (void)inherit;
  record(0, n);
  seen_flags = a;
  if (!strcmp(n, "missing")) {
    last_error = 2;
    return 0;
  }
  return (HANDLE)(uintptr_t)0x508;
}
HANDLE W32API CreateFileMappingA(HANDLE f, SECURITY_ATTRIBUTES *sa, DWORD p, DWORD h, DWORD l,
                                 const char *n) {
  (void)f;
  (void)sa;
  (void)h;
  record(0, n);
  seen_flags = p + l;
  return (HANDLE)(uintptr_t)0x50c;
}
/* Memory map for IsBadStringPtr: [0x10000, 0x12000) readable, then a
 * no-access page. */
static uint8_t *readable;
static DWORD memory_protect = 4;
DWORD W32API VirtualQuery(const void *p, struct ciuki_w32_memory_info *m, DWORD n) {
  uintptr_t a = (uintptr_t)p, base = (uintptr_t)readable;
  memset(m, 0, sizeof(*m));
  if (a >= base && a < base + 8192) {
    m->base = 0;
    m->region_size = 8192;
    m->state = 0x1000;
    m->protect = memory_protect;
  } else {
    m->region_size = 4096;
    m->state = 0x1000;
    m->protect = 1;
  }
  /* The host addresses are 64-bit; the model publishes the region by
   * offset and the caller adds it to the queried base. */
  m->base = (uint32_t)(a >= base && a < base + 8192 ? base : a);
  return n;
}
/* Threads, events and waits run inline. */
static int waits, timer_calls, items;
HANDLE W32API CreateThread(SECURITY_ATTRIBUTES *sa, DWORD stack, DWORD(W32API *entry)(void *),
                           void *arg, DWORD flags, DWORD *id) {
  (void)sa;
  (void)stack;
  (void)flags;
  *id = 7;
  entry(arg);
  return (HANDLE)(uintptr_t)0x600;
}
HANDLE W32API CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL m, BOOL i, const char *n) {
  (void)sa;
  (void)m;
  (void)i;
  (void)n;
  return (HANDLE)(uintptr_t)0x604;
}
DWORD W32API WaitForSingleObject(HANDLE h, DWORD ms) {
  (void)h;
  (void)ms;
  return ++waits < 3 ? 258 : 0;
}
void W32API Sleep(DWORD ms) { (void)ms; }
/* Loader for ResolveDelayLoadedAPI. */
static int loads, frees;
HANDLE W32API LoadLibraryA(const char *n) {
  loads++;
  if (!strcmp(n, "absent.dll")) {
    last_error = 126;
    return 0;
  }
  return (HANDLE)(uintptr_t)0x700;
}
BOOL W32API FreeLibrary(HANDLE h) {
  (void)h;
  frees++;
  return 1;
}
static DWORD W32API target(void *p) { return (DWORD)(uintptr_t)p; }
void *W32API GetProcAddress(HANDLE h, const char *n) {
  CHECK(h == (HANDLE)(uintptr_t)0x700);
  if ((uintptr_t)n == 5 || ((uintptr_t)n > 0xffff && !strcmp(n, "Present")))
    return (void *)target;
  last_error = 127;
  return 0;
}
HANDLE W32API GetModuleHandleA(const char *n) {
  return strcmp(n, "absent.dll") ? (HANDLE)(uintptr_t)0x700 : 0;
}
static ULONG raised;
void W32API RaiseException(ULONG code, ULONG flags, ULONG n, const ULONG *args) {
  (void)args;
  CHECK(flags == 1 && n == 1);
  raised = code;
}
/* Message-table resource for FormatMessage FROM_HMODULE. */
static uint8_t table[64];
HANDLE k32_find_resource(HANDLE h, const void *name, const void *type, DWORD lang, int wide) {
  (void)lang;
  CHECK(wide && (uintptr_t)name == 1 && (uintptr_t)type == 11);
  return h == (HANDLE)(uintptr_t)0x800 ? (HANDLE)table : 0;
}
HANDLE W32API LoadResource(HANDLE h, HANDLE r) {
  (void)h;
  return r;
}
DWORD W32API SizeofResource(HANDLE h, HANDLE r) {
  (void)h;
  (void)r;
  return sizeof(table);
}
static int local_allocs;
HANDLE W32API LocalAlloc(DWORD f, DWORD n) {
  CHECK(f == 0x40);
  local_allocs++;
  return calloc(1, n);
}
/* w32srv global atom table (index 0 only). */
static char atom_names[8][64];
static DWORD atom_refs[8];
uint32_t w32_server_exchange(const void *in, uint32_t n, void *out, uint32_t cap, uint32_t *used) {
  struct w32r_request r;
  memcpy(&r, in, sizeof(r));
  CHECK(n == sizeof(r) + r.path_bytes + r.name_bytes && cap >= sizeof(struct w32r_reply) + 64);
  const char *name = (const char *)in + sizeof(r) + r.path_bytes;
  struct w32r_reply reply = {.version = 1};
  char data[64] = {0};
  unsigned i = 0;
  if (r.op == W32R_ATOM_ADD || r.op == W32R_ATOM_FIND) {
    for (; i < 8 && strcasecmp(atom_names[i], name); i++)
      ;
    if (i == 8 && r.op == W32R_ATOM_ADD)
      for (i = 0; i < 8 && atom_names[i][0]; i++)
        ;
    if (i == 8)
      reply.error = r.op == W32R_ATOM_FIND ? 6 : 8;
    else {
      if (r.op == W32R_ATOM_ADD) {
        if (!atom_names[i][0])
          snprintf(atom_names[i], 64, "%s", name);
        atom_refs[i]++;
      }
      reply.key = 0xc000 + i;
    }
  } else {
    i = r.key - 0xc000;
    if (i >= 8 || !atom_names[i][0])
      reply.error = 6;
    else if (r.op == W32R_ATOM_DELETE) {
      if (!--atom_refs[i])
        atom_names[i][0] = 0;
    } else {
      CHECK(r.op == W32R_ATOM_NAME);
      strcpy(data, atom_names[i]);
      reply.data_bytes = (uint32_t)strlen(data) + 1;
    }
  }
  memcpy(out, &reply, sizeof(reply));
  memcpy((uint8_t *)out + sizeof(reply), data, reply.data_bytes);
  *used = sizeof(reply) + reply.data_bytes;
  return 0;
}
/* Declarations of the entry points under test. */
#define DECL(ret, name, args) extern ret W32API name args
DECL(BOOL, CopyFileW, (const WCHAR *, const WCHAR *, BOOL));
DECL(BOOL, CreateDirectoryW, (const WCHAR *, SECURITY_ATTRIBUTES *));
DECL(BOOL, DeleteFileW, (const WCHAR *));
DECL(BOOL, RemoveDirectoryW, (const WCHAR *));
DECL(BOOL, SetCurrentDirectoryW, (const WCHAR *));
DECL(BOOL, SetFileAttributesW, (const WCHAR *, DWORD));
DECL(DWORD, GetFileAttributesW, (const WCHAR *));
DECL(BOOL, GetFileAttributesExW, (const WCHAR *, DWORD, void *));
DECL(BOOL, MoveFileExW, (const WCHAR *, const WCHAR *, DWORD));
DECL(BOOL, MoveFileW, (const WCHAR *, const WCHAR *));
DECL(HANDLE, FindFirstFileW, (const WCHAR *, WIN32_FIND_DATAW *));
DECL(BOOL, FindNextFileW, (HANDLE, WIN32_FIND_DATAW *));
DECL(DWORD, GetCurrentDirectoryW, (DWORD, WCHAR *));
DECL(DWORD, GetFullPathNameW, (const WCHAR *, DWORD, WCHAR *, WCHAR **));
DECL(DWORD, GetLongPathNameW, (const WCHAR *, WCHAR *, DWORD));
DECL(DWORD, GetShortPathNameW, (const WCHAR *, WCHAR *, DWORD));
DECL(DWORD, GetSystemDirectoryW, (WCHAR *, DWORD));
DECL(DWORD, GetWindowsDirectoryW, (WCHAR *, DWORD));
DECL(DWORD, GetSystemWindowsDirectoryA, (char *, DWORD));
DECL(DWORD, GetSystemWindowsDirectoryW, (WCHAR *, DWORD));
DECL(DWORD, GetTempPathW, (DWORD, WCHAR *));
DECL(DWORD, GetTempFileNameW, (const WCHAR *, const WCHAR *, DWORD, WCHAR *));
DECL(DWORD, GetLogicalDriveStringsW, (DWORD, WCHAR *));
DECL(BOOL, GetDiskFreeSpaceW, (const WCHAR *, DWORD *, DWORD *, DWORD *, DWORD *));
DECL(BOOL, GetDiskFreeSpaceExW, (const WCHAR *, uint64_t *, uint64_t *, uint64_t *));
DECL(DWORD, GetDriveTypeW, (const WCHAR *));
DECL(BOOL, GetVolumeInformationW,
     (const WCHAR *, WCHAR *, DWORD, DWORD *, DWORD *, DWORD *, WCHAR *, DWORD));
DECL(DWORD, SearchPathW, (const WCHAR *, const WCHAR *, const WCHAR *, DWORD, WCHAR *, WCHAR **));
DECL(BOOL, GetBinaryTypeW, (const WCHAR *, DWORD *));
DECL(BOOL, GetBinaryTypeA, (const char *, DWORD *));
DECL(DWORD, GetPrivateProfileStringW,
     (const WCHAR *, const WCHAR *, const WCHAR *, WCHAR *, DWORD, const WCHAR *));
DECL(DWORD, GetProfileStringW, (const WCHAR *, const WCHAR *, const WCHAR *, WCHAR *, DWORD));
DECL(BOOL, WritePrivateProfileStringW,
     (const WCHAR *, const WCHAR *, const WCHAR *, const WCHAR *));
DECL(DWORD, GetPrivateProfileIntW, (const WCHAR *, const WCHAR *, int, const WCHAR *));
DECL(BOOL, GetPrivateProfileStructW, (const WCHAR *, const WCHAR *, void *, DWORD, const WCHAR *));
DECL(BOOL, WritePrivateProfileStructW,
     (const WCHAR *, const WCHAR *, const void *, DWORD, const WCHAR *));
DECL(void, OutputDebugStringW, (const WCHAR *));
DECL(int, lstrcmpW, (const WCHAR *, const WCHAR *));
DECL(int, lstrcmpiW, (const WCHAR *, const WCHAR *));
DECL(BOOL, IsBadStringPtrA, (const char *, DWORD));
DECL(BOOL, IsBadStringPtrW, (const WCHAR *, DWORD));
DECL(BOOL, IsBadReadPtr, (const void *, uintptr_t));
DECL(BOOL, IsBadWritePtr, (void *, uintptr_t));
DECL(BOOL, IsBadCodePtr, (void *));
DECL(HANDLE, CreateSemaphoreW, (SECURITY_ATTRIBUTES *, LONG, LONG, const WCHAR *));
DECL(HANDLE, OpenSemaphoreW, (DWORD, BOOL, const WCHAR *));
DECL(HANDLE, OpenEventW, (DWORD, BOOL, const WCHAR *));
DECL(HANDLE, CreateFileMappingW, (HANDLE, SECURITY_ATTRIBUTES *, DWORD, DWORD, DWORD, const WCHAR *));
DECL(DWORD, ExpandEnvironmentStringsA, (const char *, char *, DWORD));
DECL(DWORD, ExpandEnvironmentStringsW, (const WCHAR *, WCHAR *, DWORD));
DECL(int, OpenFile, (const char *, void *, DWORD));
DECL(int, _lopen, (const char *, int));
DECL(int, _lcreat, (const char *, int));
DECL(DWORD, _lread, (int, void *, DWORD));
DECL(LONG, _hread, (int, void *, LONG));
DECL(DWORD, _lwrite, (int, const void *, DWORD));
DECL(LONG, _hwrite, (int, const void *, LONG));
DECL(LONG, _llseek, (int, LONG, int));
DECL(int, _lclose, (int));
DECL(uint16_t, GetUserDefaultLangID, (void));
DECL(uint16_t, GetSystemDefaultLangID, (void));
DECL(uint16_t, GetUserDefaultUILanguage, (void));
DECL(uint16_t, GetSystemDefaultUILanguage, (void));
DECL(DWORD, GetThreadLocale, (void));
DECL(DWORD, ConvertDefaultLocale, (DWORD));
DECL(int, GetLocaleInfoEx, (const WCHAR *, DWORD, WCHAR *, int));
DECL(DWORD, LocaleNameToLCID, (const WCHAR *, DWORD));
DECL(int, LCIDToLocaleName, (DWORD, WCHAR *, int, DWORD));
DECL(BOOL, IsValidLocaleName, (const WCHAR *));
DECL(int, GetUserDefaultLocaleName, (WCHAR *, int));
DECL(BOOL, EnumSystemLocalesEx, (BOOL(W32API *)(WCHAR *, DWORD, intptr_t), DWORD, intptr_t, void *));
DECL(BOOL, EnumSystemLocalesA, (BOOL(W32API *)(char *), DWORD));
DECL(BOOL, GetCPInfoExW, (DWORD, DWORD, void *));
DECL(BOOL, GetStringTypeExW, (DWORD, DWORD, const WCHAR *, int, uint16_t *));
DECL(BOOL, GetStringTypeExA, (DWORD, DWORD, const char *, int, uint16_t *));
DECL(BOOL, GetStringTypeA, (DWORD, DWORD, const char *, int, uint16_t *));
DECL(int, CompareStringOrdinal, (const WCHAR *, int, const WCHAR *, int, BOOL));
DECL(int, GetDateFormatA, (DWORD, DWORD, const SYSTEMTIME *, const char *, char *, int));
DECL(int, GetDateFormatW, (DWORD, DWORD, const SYSTEMTIME *, const WCHAR *, WCHAR *, int));
DECL(int, GetTimeFormatA, (DWORD, DWORD, const SYSTEMTIME *, const char *, char *, int));
DECL(int, GetTimeFormatW, (DWORD, DWORD, const SYSTEMTIME *, const WCHAR *, WCHAR *, int));
DECL(int, GetNumberFormatA, (DWORD, DWORD, const char *, const void *, char *, int));
DECL(int, GetNumberFormatW, (DWORD, DWORD, const WCHAR *, const void *, WCHAR *, int));
DECL(int, GetCurrencyFormatA, (DWORD, DWORD, const char *, const void *, char *, int));
DECL(int, GetCurrencyFormatW, (DWORD, DWORD, const WCHAR *, const void *, WCHAR *, int));
DECL(DWORD, FormatMessageA, (DWORD, const void *, DWORD, DWORD, char *, DWORD, void *));
DECL(DWORD, FormatMessageW, (DWORD, const void *, DWORD, DWORD, WCHAR *, DWORD, void *));
DECL(BOOL, Beep, (DWORD, DWORD));
DECL(BOOL, CreatePipe, (HANDLE *, HANDLE *, SECURITY_ATTRIBUTES *, DWORD));
DECL(BOOL, PostQueuedCompletionStatus, (HANDLE, DWORD, uintptr_t, OVERLAPPED *));
DECL(uint64_t, GetTickCount64, (void));
DECL(BOOL, QueueUserWorkItem, (DWORD(W32API *)(void *), void *, ULONG));
DECL(BOOL, CreateTimerQueueTimer,
     (HANDLE *, HANDLE, void(W32API *)(void *, BOOLEAN), void *, DWORD, DWORD, ULONG));
DECL(BOOL, InitOnceExecuteOnce,
     (uintptr_t *, BOOL(W32API *)(uintptr_t *, void *, void **), void *, void **));
DECL(BOOL, InitializeCriticalSectionEx, (RTL_CRITICAL_SECTION *, DWORD, DWORD));
DECL(void *, ResolveDelayLoadedAPI, (void *, const uint8_t *, void *, void *, void **, ULONG));
DECL(void *, DelayLoadFailureHook, (const char *, const char *));
DECL(uint16_t, GlobalAddAtomA, (const char *));
DECL(uint16_t, GlobalAddAtomW, (const WCHAR *));
DECL(uint16_t, GlobalFindAtomA, (const char *));
DECL(uint16_t, GlobalFindAtomW, (const WCHAR *));
DECL(uint16_t, GlobalDeleteAtom, (uint16_t));
DECL(DWORD, GlobalGetAtomNameA, (uint16_t, char *, int));
DECL(DWORD, GlobalGetAtomNameW, (uint16_t, WCHAR *, int));
DECL(BOOL, SystemTimeToTzSpecificLocalTime,
     (const TIME_ZONE_INFORMATION *, const SYSTEMTIME *, SYSTEMTIME *));
DECL(BOOL, AllocConsole, (void));
DECL(BOOL, GetConsoleMode, (HANDLE, DWORD *));
DECL(BOOL, SetConsoleMode, (HANDLE, DWORD));
DECL(BOOL, VerifyConsoleIoHandle, (HANDLE));
DECL(BOOL, PeekConsoleInputA, (HANDLE, void *, DWORD, DWORD *));
DECL(BOOL, ReadConsoleA, (HANDLE, void *, DWORD, DWORD *, void *));
DECL(BOOL, ReadConsoleInputW, (HANDLE, void *, DWORD, DWORD *));
DECL(BOOL, ActivateActCtx, (HANDLE, uintptr_t *));
DECL(HANDLE, CreateActCtxW, (const void *));
DECL(BOOL, DeactivateActCtx, (DWORD, uintptr_t));
DECL(BOOL, FindActCtxSectionGuid, (DWORD, const void *, ULONG, const void *, void *));
DECL(BOOL, FindActCtxSectionStringW, (DWORD, const void *, ULONG, const WCHAR *, void *));
DECL(BOOL, GetCurrentActCtx, (HANDLE *));
DECL(void, ReleaseActCtx, (HANDLE));
DECL(DWORD, GetSystemWow64DirectoryW, (WCHAR *, DWORD));
DECL(BOOL, Wow64DisableWow64FsRedirection, (void **));
DECL(BOOL, Wow64RevertWow64FsRedirection, (void *));
DECL(LONG, SetThreadDescription, (HANDLE, const WCHAR *));
/* UTF-16 literal helper (ASCII plus \u escapes through the code page). */
static WCHAR *W(const char *s) {
  static WCHAR buffers[8][300];
  static unsigned next;
  WCHAR *w = buffers[next++ % 8];
  unsigned i = 0;
  for (; s[i]; i++)
    MultiByteToWideChar(1252, 0, s + i, 1, w + i, 1);
  w[i] = 0;
  return w;
}
static int wide_equal(const WCHAR *w, const char *a) { return !memcmp(w, W(a), (strlen(a) + 1) * 2); }
static const WCHAR han[] = {'C', ':', '\\', 0x4e00, 0};
static void unicode_layer(void) {
  CHECK(CopyFileW(W("C:\\caf\xe9.txt"), W("D:\\\x80.txt"), 1));
  CHECK(!strcmp(seen[0], "C:\\caf\xe9.txt") && !strcmp(seen[1], "D:\\\x80.txt") && seen_flags == 1);
  last_error = 0;
  CHECK(!CopyFileW(han, W("x"), 0) && last_error == 123);
  CHECK(!CopyFileW(0, W("x"), 0) && last_error == 87);
  WCHAR longname[300];
  for (unsigned i = 0; i < 299; i++)
    longname[i] = 'a';
  longname[299] = 0;
  CHECK(!DeleteFileW(longname) && last_error == 206);
  put_file("C:\\CWD\\gone.txt", "x", 1, 0x20);
  CHECK(DeleteFileW(W("gone.txt")) && !lookup("C:\\CWD\\gone.txt"));
  CHECK(!DeleteFileW(W("gone.txt")) && last_error == 2);
  CHECK(CreateDirectoryW(W("C:\\NEW"), 0) && !strcmp(seen[0], "C:\\NEW"));
  CHECK(!CreateDirectoryW(han, 0) && last_error == 123);
  CHECK(RemoveDirectoryW(W("C:\\OLD")) && !strcmp(seen[0], "C:\\OLD"));
  CHECK(!RemoveDirectoryW(han) && last_error == 123);
  CHECK(SetCurrentDirectoryW(W("C:\\GAME")) && !strcmp(seen[0], "C:\\GAME"));
  CHECK(!SetCurrentDirectoryW(han) && last_error == 123);
  CHECK(SetFileAttributesW(W("a.txt"), 2) && seen_flags == 2);
  CHECK(!SetFileAttributesW(han, 2) && last_error == 123);
  put_file("C:\\CWD\\here.txt", "x", 1, 0x21);
  CHECK(GetFileAttributesW(W("here.txt")) == 0x21);
  CHECK(GetFileAttributesW(W("nothere.txt")) == ~0u && last_error == 2);
  CHECK(GetFileAttributesW(han) == ~0u && last_error == 123);
  WIN32_FILE_ATTRIBUTE_DATA fa;
  CHECK(GetFileAttributesExW(W("x.txt"), 0, &fa) && fa.attributes == 0x21 && fa.size_low == 9);
  CHECK(!GetFileAttributesExW(W("x.txt"), 1, &fa) && last_error == 87);
  /* f5-15A amendment 1: Windows 98 delayed moves return error 120;
   * installers write WININIT.INI [rename] themselves. */
  CHECK(!MoveFileExW(W("a"), 0, 4) && last_error == 120);
  CHECK(!MoveFileExW(W("a"), han, 0) && last_error == 123);
  CHECK(MoveFileW(W("a"), W("b")) && !strcmp(seen[1], "b") && seen_flags == 0x55);
  CHECK(!MoveFileW(han, W("b")) && last_error == 123);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(W("C:\\*.*"), &fd);
  CHECK(h == (HANDLE)(uintptr_t)0x400 && fd.size_low == 77 && fd.name[3] == 0xe9 &&
        wide_equal(fd.name, "caf\xe9 long name.txt") && wide_equal(fd.alternate, "CAFLON~1.TXT"));
  CHECK(FindNextFileW(h, &fd) && fd.name[0] == 0x20ac);
  CHECK(!FindNextFileW(h, &fd) && last_error == 18);
  CHECK(FindFirstFileW(W("*"), 0) == K32_INVALID && last_error == 87);
  CHECK(FindFirstFileW(han, &fd) == K32_INVALID && last_error == 123);
  WCHAR out[300];
  CHECK(GetCurrentDirectoryW(300, out) == 6 && wide_equal(out, "C:\\CWD"));
  CHECK(GetCurrentDirectoryW(3, out) == 7);
  WCHAR *part = 0;
  CHECK(GetFullPathNameW(W("dir\\file.txt"), 300, out, &part) == 19 &&
        wide_equal(out, "C:\\CWD\\dir\\file.txt") && part == out + 11);
  CHECK(GetFullPathNameW(W("dir\\file.txt"), 5, out, &part) == 20);
  CHECK(!GetFullPathNameW(han, 300, out, &part) && last_error == 123);
  CHECK(GetLongPathNameW(W("C:\\PROGRA~1"), out, 300) == 16 && wide_equal(out, "C:\\Program Files"));
  CHECK(!GetLongPathNameW(W("C:\\NONE~1"), out, 300) && last_error == 2);
  CHECK(GetShortPathNameW(W("C:\\Program Files"), out, 300) == 11 && wide_equal(out, "C:\\PROGRA~1"));
  CHECK(GetShortPathNameW(W("C:\\Program Files"), out, 4) == 12);
  CHECK(GetSystemDirectoryW(out, 300) == 17 && wide_equal(out, "C:\\WINDOWS\\SYSTEM"));
  CHECK(GetSystemDirectoryW(out, 10) == 18);
  CHECK(GetWindowsDirectoryW(out, 300) == 10 && wide_equal(out, "C:\\WINDOWS"));
  CHECK(GetSystemWindowsDirectoryW(out, 300) == 10 && wide_equal(out, "C:\\WINDOWS"));
  CHECK(GetSystemWindowsDirectoryW(out, 2) == 11);
  char a[300];
  CHECK(GetSystemWindowsDirectoryA(a, 300) == 10 && !strcmp(a, "C:\\WINDOWS"));
  CHECK(GetTempPathW(300, out) == 8 && wide_equal(out, "C:\\TEMP\\"));
  CHECK(GetTempPathW(2, out) == 9);
  CHECK(GetTempFileNameW(W("C:\\TEMP"), W("wine"), 0x12, out) == 0x12 &&
        wide_equal(out, "C:\\TEMP\\win0012.TMP"));
  CHECK(!GetTempFileNameW(W("C:\\TEMP"), W("x"), 1, 0) && last_error == 87);
  CHECK(GetLogicalDriveStringsW(300, out) == 8 && out[0] == 'A' && out[3] == 0 && out[4] == 'C' &&
        !out[7] && !out[8]);
  CHECK(GetLogicalDriveStringsW(5, out) == 9);
  DWORD spc, bps, fc, tc;
  CHECK(GetDiskFreeSpaceW(W("C:\\"), &spc, &bps, &fc, &tc) && spc == 8 && !strcmp(seen[0], "C:\\"));
  CHECK(GetDiskFreeSpaceW(0, &spc, &bps, &fc, &tc) && !strcmp(seen[0], "<null>"));
  CHECK(!GetDiskFreeSpaceW(han, &spc, &bps, &fc, &tc) && last_error == 123);
  uint64_t f1, f2, f3;
  CHECK(GetDiskFreeSpaceExW(W("C:\\"), &f1, &f2, &f3) && f1 == 1 && f2 == 2 && f3 == 3);
  CHECK(!GetDiskFreeSpaceExW(han, &f1, 0, 0) && last_error == 123);
  CHECK(GetDriveTypeW(W("C:\\")) == 3 && GetDriveTypeW(han) == 1);
  WCHAR label[20], fs[20];
  DWORD serial = 0, comp = 0, vflags = 0;
  CHECK(GetVolumeInformationW(W("C:\\"), label, 20, &serial, &comp, &vflags, fs, 20) &&
        wide_equal(label, "CIUKI\xe9") && wide_equal(fs, "FAT32") && serial == 0x1234 &&
        comp == 255 && vflags == 6);
  serial = 0;
  CHECK(!GetVolumeInformationW(W("C:\\"), label, 3, &serial, 0, 0, 0, 0) && last_error == 122 &&
        !serial);
  /* Profiles. */
  memset(out, 0xff, sizeof(out));
  CHECK(GetPrivateProfileStringW(W("Sect\xe9"), W("Key"), W("def"), out, 300, W("C:\\A.INI")) == 5 &&
        wide_equal(out, "valu\xe9") && !strcmp(seen[0], "Sect\xe9") && !strcmp(seen[3], "C:\\A.INI"));
  CHECK(GetPrivateProfileStringW(W("S"), W("K"), 0, out, 3, 0) == 2 && out[2] == 0 &&
        !strcmp(seen[2], "<null>") && !strcmp(seen[3], "<null>"));
  CHECK(GetPrivateProfileStringW(0, 0, 0, out, 300, W("x.ini")) == 7 && out[3] == 0 &&
        out[6] == 0xe9 && !out[7] && !out[8]);
  CHECK(!GetPrivateProfileStringW(han, W("K"), 0, out, 300, 0) && last_error == 1113);
  CHECK(!GetPrivateProfileStringW(W("S"), W("K"), 0, out, 0, 0));
  CHECK(GetProfileStringW(W("windows"), W("device"), W(""), out, 300) == 5 &&
        !strcmp(seen[3], "WIN.INI"));
  CHECK(WritePrivateProfileStringW(W("S"), 0, 0, W("f.ini")) && !strcmp(seen[1], "<null>") &&
        !strcmp(seen[2], "<null>"));
  CHECK(WritePrivateProfileStringW(W("S"), W("K"), W("v\xe9"), W("f.ini")) &&
        !strcmp(seen[2], "v\xe9"));
  CHECK(!WritePrivateProfileStringW(W("S"), W("K"), han, W("f.ini")) && last_error == 1113);
  CHECK(GetPrivateProfileIntW(W("S"), W("K"), 41, W("f.ini")) == 42);
  CHECK(GetPrivateProfileIntW(han, W("K"), 41, W("f.ini")) == 41 && last_error == 1113);
  uint8_t blob[4];
  CHECK(GetPrivateProfileStructW(W("S"), W("K"), blob, 4, W("f.ini")) && blob[3] == 0x5a);
  CHECK(!GetPrivateProfileStructW(W("S"), han, blob, 4, W("f.ini")) && last_error == 1113);
  CHECK(WritePrivateProfileStructW(W("S"), W("K"), blob, 4, W("f.ini")) && seen_flags == 4);
  CHECK(!WritePrivateProfileStructW(W("S"), W("K"), blob, 4, han) && last_error == 123);
  last_error = 77;
  OutputDebugStringW(han);
  CHECK(!strcmp(seen[0], "C:\\?") && last_error == 77);
  OutputDebugStringW(0);
  CHECK(!strcmp(seen[0], "") && last_error == 77);
  CHECK(lstrcmpW(W("abc"), W("abd")) < 0 && lstrcmpW(W("b"), W("a")) > 0 && !lstrcmpW(W("x"), W("x")));
  CHECK(lstrcmpW(0, W("a")) < 0 && lstrcmpW(W("a"), 0) > 0 && !lstrcmpW(0, 0));
  CHECK(lstrcmpW(W("ab"), W("abc")) < 0);
  CHECK(!lstrcmpiW(W("Caf\xe9"), W("CAF\xc9")) && lstrcmpiW(W("a"), W("B")) < 0 && lstrcmpW(W("a"), W("B")) > 0);
  readable = mmap(0, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  CHECK(readable != MAP_FAILED);
  memset(readable, 'x', 8192);
  readable[10] = 0;
  CHECK(!IsBadStringPtrA((const char *)readable, 100) && !IsBadStringPtrA(0, 0));
  CHECK(IsBadStringPtrA(0, 1) && IsBadStringPtrA((const char *)readable + 8180, 100));
  CHECK(!IsBadStringPtrA((const char *)readable + 8180, 12));
  CHECK(!IsBadStringPtrW((const WCHAR *)readable, 3) && IsBadStringPtrW((const WCHAR *)(readable + 8188), 8));
  /* f5-20C: IsBad*Ptr over the same map; the no-access page is never read. */
  CHECK(!IsBadReadPtr(readable, 8192) && !IsBadReadPtr(readable + 8191, 1) && !IsBadReadPtr(0, 0));
  CHECK(IsBadReadPtr(readable + 8191, 2) && IsBadReadPtr(0, 1) && IsBadReadPtr(readable + 9000, 4));
  CHECK(IsBadReadPtr((void *)~(uintptr_t)0, 2) && !IsBadReadPtr((void *)~(uintptr_t)0, 0));
  CHECK(!IsBadWritePtr(readable, 4096) && IsBadWritePtr(readable + 4096, 8192) && IsBadWritePtr(0, 4));
  CHECK(!IsBadCodePtr(readable) && IsBadCodePtr(readable + 8192) && IsBadCodePtr(0));
  memory_protect = 2; /* PAGE_READONLY: readable, not writable */
  CHECK(!IsBadReadPtr(readable, 16) && IsBadWritePtr(readable, 16) && !IsBadCodePtr(readable));
  memory_protect = 0x40; /* PAGE_EXECUTE_READWRITE */
  CHECK(!IsBadReadPtr(readable, 16) && !IsBadWritePtr(readable, 16));
  memory_protect = 0x104; /* PAGE_READWRITE | PAGE_GUARD */
  CHECK(IsBadReadPtr(readable, 16) && IsBadWritePtr(readable, 16) && IsBadCodePtr(readable));
  memory_protect = 4;
  munmap(readable, 16384);
  CHECK(CreateSemaphoreW(0, 1, 5, W("sem\xe9")) == (HANDLE)(uintptr_t)0x500 && !strcmp(seen[0], "sem\xe9") &&
        seen_flags == 105);
  CHECK(CreateSemaphoreW(0, 1, 5, 0) && !strcmp(seen[0], "<null>"));
  CHECK(!CreateSemaphoreW(0, 1, 5, han) && last_error == 123);
  CHECK(OpenSemaphoreW(3, 0, W("sem")) == (HANDLE)(uintptr_t)0x504 && seen_flags == 3);
  CHECK(!OpenSemaphoreW(3, 0, 0) && last_error == 87);
  CHECK(OpenEventW(2, 0, W("evt")) == (HANDLE)(uintptr_t)0x508 && !strcmp(seen[0], "evt"));
  CHECK(!OpenEventW(2, 0, W("missing")) && last_error == 2);
  CHECK(!OpenEventW(2, 0, 0) && last_error == 87);
  CHECK(CreateFileMappingW(K32_INVALID, 0, 4, 0, 4096, W("map")) == (HANDLE)(uintptr_t)0x50c &&
        seen_flags == 4100);
  CHECK(!CreateFileMappingW(K32_INVALID, 0, 4, 0, 4096, han) && last_error == 123);
  puts("PASS kernel32 upper Unicode files paths profiles strings objects");
}
static void make_image(const char *path, const char *sig, uint16_t machine, uint16_t flags,
                       uint8_t ne_os) {
  uint8_t image[256] = {'M', 'Z'};
  w32_put32(image + 60, 128);
  memcpy(image + 128, sig, strlen(sig));
  memcpy(image + 132, &machine, 2);
  memcpy(image + 150, &flags, 2);
  image[128 + 0x36] = ne_os;
  put_file(path, image, sizeof(image), 0x20);
}
static void search_and_legacy_files(void) {
  put_file("C:\\APP\\app.dll", "x", 1, 0x20);
  put_file("C:\\CWD\\cwd.txt", "x", 1, 0x20);
  put_file("C:\\WINDOWS\\SYSTEM\\sys.dll", "x", 1, 0x20);
  put_file("C:\\BIN\\tool.hlp", "x", 1, 0x20);
  char out[300], *part = 0;
  CHECK(SearchPathA(0, "app.dll", 0, 300, out, &part) == 14 && !strcmp(out, "C:\\APP\\app.dll") &&
        part == out + 7);
  CHECK(SearchPathA(0, "cwd", ".txt", 300, out, 0) == 14 && !strcmp(out, "C:\\CWD\\cwd.txt"));
  CHECK(SearchPathA(0, "sys.dll", ".exe", 300, out, 0) && !strcmp(out, "C:\\WINDOWS\\SYSTEM\\sys.dll"));
  CHECK(SearchPathA(0, "tool.hlp", 0, 300, out, 0) && !strcmp(out, "C:\\BIN\\tool.hlp"));
  CHECK(SearchPathA("C:\\X;C:\\BIN", "tool.hlp", 0, 300, out, 0) == 15);
  CHECK(SearchPathA(0, "tool.hlp", 0, 5, out, 0) == 16);
  CHECK(SearchPathA(0, "C:\\APP\\app.dll", 0, 300, out, 0) == 14);
  CHECK(!SearchPathA(0, "absent.dll", 0, 300, out, 0) && last_error == 2);
  CHECK(!SearchPathA(0, "", 0, 300, out, 0) && last_error == 87);
  WCHAR wout[300], *wpart = 0;
  CHECK(SearchPathW(0, W("app"), W(".dll"), 300, wout, &wpart) == 14 &&
        wide_equal(wout, "C:\\APP\\app.dll") && wpart == wout + 7);
  CHECK(!SearchPathW(0, han, 0, 300, wout, 0) && last_error == 123);
  CHECK(!SearchPathW(0, 0, 0, 300, wout, 0) && last_error == 87);
  make_image("C:\\BIN\\tool.exe", "PE", 0x14c, 0x102, 0);
  make_image("C:\\BIN\\lib.dll", "PE", 0x14c, 0x2102, 0);
  make_image("C:\\BIN\\x64.exe", "PE", 0x8664, 0x22, 0);
  make_image("C:\\BIN\\win16.exe", "NE", 0, 0, 2);
  make_image("C:\\BIN\\os2.exe", "NE", 0, 0, 1);
  make_image("C:\\BIN\\vxd.386", "LE", 0, 0, 0);
  put_file("C:\\BIN\\dos.exe", "MZ\x10\x00", 4, 0x20);
  put_file("C:\\BIN\\cmd.com", "\xe9\x00", 2, 0x20);
  put_file("C:\\BIN\\app.pif", "\x00\x78", 2, 0x20);
  put_file("C:\\BIN\\readme.txt", "hello", 5, 0x20);
  DWORD type = 99;
  CHECK(GetBinaryTypeA("C:\\BIN\\tool.exe", &type) && type == 0);
  CHECK(GetBinaryTypeA("C:\\BIN\\win16.exe", &type) && type == 2);
  CHECK(GetBinaryTypeA("C:\\BIN\\os2.exe", &type) && type == 5);
  CHECK(GetBinaryTypeA("C:\\BIN\\vxd.386", &type) && type == 1);
  CHECK(GetBinaryTypeA("C:\\BIN\\dos.exe", &type) && type == 1);
  CHECK(GetBinaryTypeA("C:\\BIN\\cmd.com", &type) && type == 1);
  CHECK(GetBinaryTypeA("C:\\BIN\\app.pif", &type) && type == 3);
  type = 99;
  CHECK(!GetBinaryTypeA("C:\\BIN\\lib.dll", &type) && last_error == 193 && type == 99);
  CHECK(!GetBinaryTypeA("C:\\BIN\\x64.exe", &type) && last_error == 193);
  CHECK(!GetBinaryTypeA("C:\\BIN\\readme.txt", &type) && last_error == 193);
  CHECK(!GetBinaryTypeA("C:\\BIN\\none.exe", &type) && last_error == 2);
  CHECK(!GetBinaryTypeA("C:\\BIN\\tool.exe", 0) && last_error == 87);
  CHECK(GetBinaryTypeW(W("C:\\BIN\\tool.exe"), &type) && type == 0);
  CHECK(!GetBinaryTypeW(han, &type) && last_error == 123);
  for (unsigned i = 0; i < 16; i++)
    CHECK(!handles[i].used);
  char e[300];
  CHECK(ExpandEnvironmentStringsA("%WINDIR%\\x;%TEMP%", e, 300) == 21 &&
        !strcmp(e, "C:\\WINDOWS\\x;C:\\TEMP"));
  CHECK(ExpandEnvironmentStringsA("%NOPE%;%EMPTY%;%", e, 300) == 10 && !strcmp(e, "%NOPE%;;%"));
  CHECK(ExpandEnvironmentStringsA("x%CAFE%", e, 300) == 6 && !strcmp(e, "xcaf\xe9"));
  CHECK(ExpandEnvironmentStringsA("%WINDIR%", e, 4) == 11 && !strcmp(e, "C:\\"));
  CHECK(ExpandEnvironmentStringsA("%WINDIR%", 0, 0) == 11);
  CHECK(!ExpandEnvironmentStringsA(0, e, 300) && last_error == 87);
  WCHAR w[300];
  CHECK(ExpandEnvironmentStringsW(W("%CAFE%!"), w, 300) == 6 && wide_equal(w, "caf\xe9!"));
  CHECK(ExpandEnvironmentStringsW(W("%TEMP%"), w, 3) == 8 && w[2] == 0 && w[1] == ':');
  CHECK(!ExpandEnvironmentStringsW(han, w, 300) && last_error == 1113);
  /* Win16 file API. */
  uint8_t of[136];
  int f = OpenFile("readme.txt", of, 0);
  CHECK(f != -1 && of[0] == 136 && !strcmp((char *)of + 8, "C:\\BIN\\readme.txt"));
  char buffer[16] = {0};
  CHECK(_lread(f, buffer, 3) == 3 && !memcmp(buffer, "hel", 3));
  CHECK(_llseek(f, 1, 0) == 1 && _hread(f, buffer, 10) == 4 && !memcmp(buffer, "ello", 4));
  CHECK(_lclose(f) == 0 && _lclose(f) == -1);
  CHECK(OpenFile("missing.txt", of, 0) == -1 && of[2] == 2 && last_error == 2);
  CHECK(OpenFile("readme.txt", of, 0x4000) == 1);
  CHECK(OpenFile("dir\\new.txt", of, 0x100) == 0 && !strcmp((char *)of + 8, "C:\\CWD\\dir\\new.txt"));
  CHECK(OpenFile(0, of, 0x8000) == -1);
  f = OpenFile("created.txt", of, 0x1001);
  CHECK(f != -1 && _lwrite(f, "abcdef", 6) == 6 && _llseek(f, 2, 0) == 2 && _lwrite(f, "", 0) == 0);
  CHECK(lookup("C:\\CWD\\created.txt")->size == 2 && _hwrite(f, "Z", -1) == -1 && last_error == 87);
  CHECK(_lclose(f) == 0);
  f = OpenFile(0, of, 0x8000);
  CHECK(f != -1 && _lclose(f) == 0);
  CHECK(OpenFile("created.txt", of, 0x200) == 1 && !lookup("C:\\CWD\\created.txt"));
  CHECK(OpenFile("created.txt", of, 0x200) == -1 && of[2] == 2);
  f = _lcreat("C:\\CWD\\l.txt", 0);
  CHECK(f != -1 && _hwrite(f, "12345", 5) == 5 && _lclose(f) == 0);
  f = _lopen("C:\\CWD\\l.txt", 0x40);
  CHECK(f != -1 && _llseek(f, 0, 2) == 5 && _lclose(f) == 0);
  CHECK(_lopen("C:\\CWD\\none.txt", 0) == -1 && last_error == 2);
  CHECK(_lread(0x77, buffer, 1) == ~0u && last_error == 6);
  puts("PASS kernel32 upper SearchPath GetBinaryType ExpandEnvironmentStrings Win16 file API");
}
#include "regional_test.h"
static BOOL W32API enum_locale(WCHAR *name, DWORD flags, intptr_t param) {
  int *count = (int *)param;
  (*count)++;
  return !(flags & 0x10) || name[0] == 'e';
}
/* f7: the Quake III Arena C runtime's EnumSystemLocalesA (LCID strings). */
static char locale_seen[4][9];
static int locale_count, locale_stop;
static BOOL W32API enum_locale_a(char *lcid) {
  if (locale_count < 4)
    memcpy(locale_seen[locale_count], lcid, 9);
  locale_count++;
  return !locale_stop;
}
static void locale_tests(void) {
  CHECK(GetUserDefaultLCID() == 0x409 && GetSystemDefaultLCID() == 0x409 && GetThreadLocale() == 0x409);
  CHECK(GetUserDefaultLangID() == 0x409 && GetSystemDefaultLangID() == 0x409);
  CHECK(GetUserDefaultUILanguage() == 0x409 && GetSystemDefaultUILanguage() == 0x409);
  CHECK(ConvertDefaultLocale(0x800) == 0x409 && ConvertDefaultLocale(0x400) == 0x409 &&
        ConvertDefaultLocale(0) == 0x409 && ConvertDefaultLocale(0x09) == 0x409 &&
        ConvertDefaultLocale(0x07) == 0x407 && ConvertDefaultLocale(0x40c) == 0x40c);
  CHECK(IsValidLocale(0x409, 1) && IsValidLocale(0x409, 2) && !IsValidLocale(0x407, 1) &&
        !IsValidLocale(0x409, 0));
  char a[64];
  CHECK(GetLocaleInfoA(0x409, 0x1f, a, 64) == 7 && !strcmp(a, "M/d/yy"));
  CHECK(GetLocaleInfoA(0x400, 0x2a, a, 64) == 7 && !strcmp(a, "Monday"));
  CHECK(GetLocaleInfoA(0x409, 0x1004, 0, 0) == 5);
  CHECK(!GetLocaleInfoA(0x409, 0x1004, a, 4) && last_error == 122);
  DWORD number = 0;
  CHECK(GetLocaleInfoA(0x409, 0x20001004, (char *)&number, 4) == 4 && number == 1252);
  CHECK(GetLocaleInfoA(0x409, 0x20000001, (char *)&number, 4) == 4 && number == 0x409);
  CHECK(!GetLocaleInfoA(0x409, 0x20000002, (char *)&number, 4) && last_error == 1004);
  CHECK(!GetLocaleInfoA(0x407, 0x1f, a, 64) && last_error == 87);
  CHECK(!GetLocaleInfoA(0x409, 0x9999, a, 64) && last_error == 1004);
  CHECK(!GetLocaleInfoA(0x409, 0x1f, 0, 5) && last_error == 87);
  WCHAR w[300];
  CHECK(GetLocaleInfoEx(W("en-US"), 0x5c, w, 300) == 6 && wide_equal(w, "en-US"));
  CHECK(GetLocaleInfoEx(0, 0x20000000 | 0x1004, w, 2) == 2 && w[0] == 1252);
  CHECK(GetLocaleInfoEx(W(""), 0x5c, w, 300) == 1 && !w[0]);
  CHECK(GetLocaleInfoEx(W("en"), 0x5c, w, 300) == 3 && wide_equal(w, "en"));
  CHECK(!GetLocaleInfoEx(W("de-DE"), 0x5c, w, 300) && last_error == 87);
  CHECK(LocaleNameToLCID(W("EN-us"), 0) == 0x409 && LocaleNameToLCID(0, 0) == 0x409 &&
        LocaleNameToLCID(W(""), 0) == 0x7f && LocaleNameToLCID(W("en"), 0x08000000) == 0x09 &&
        LocaleNameToLCID(W("!x-sys-default-locale"), 0) == 0x409);
  CHECK(!LocaleNameToLCID(W("fr-FR"), 0) && last_error == 87);
  CHECK(LCIDToLocaleName(0x409, w, 300, 0) == 6 && wide_equal(w, "en-US"));
  CHECK(LCIDToLocaleName(0x400, 0, 0, 0) == 6 && LCIDToLocaleName(0x7f, w, 300, 0) == 1 && !w[0]);
  CHECK(LCIDToLocaleName(0x09, w, 300, 0x08000000) == 3 && wide_equal(w, "en"));
  CHECK(!LCIDToLocaleName(0x409, w, 3, 0) && last_error == 122);
  CHECK(!LCIDToLocaleName(0x411, w, 300, 0) && last_error == 87);
  CHECK(IsValidLocaleName(W("en-US")) && IsValidLocaleName(W("")) && !IsValidLocaleName(W("xx")) &&
        !IsValidLocaleName(0));
  CHECK(GetUserDefaultLocaleName(w, 85) == 6 && wide_equal(w, "en-US"));
  CHECK(!GetUserDefaultLocaleName(w, 2) && last_error == 122 && !GetUserDefaultLocaleName(0, 5));
  int count = 0;
  CHECK(EnumSystemLocalesEx(enum_locale, 0, (intptr_t)&count, 0) && count == 4);
  count = 0;
  CHECK(EnumSystemLocalesEx(enum_locale, 0x20, (intptr_t)&count, 0) && count == 2);
  CHECK(!EnumSystemLocalesEx(0, 0, 0, 0) && last_error == 87);
  CHECK(!EnumSystemLocalesEx(enum_locale, 0, 0, (void *)1) && last_error == 87);
  CHECK(EnumSystemLocalesA(enum_locale_a, 1) && locale_count == 2 &&
        !strcmp(locale_seen[0], "00000409") && !strcmp(locale_seen[1], "00000410"));
  for (int i = 0; i < 2; i++) {
    unsigned long value = strtoul(locale_seen[i], 0, 16);
    CHECK(IsValidLocale((DWORD)value, 1) && IsValidLocale((DWORD)value, 2));
  }
  locale_count = 0;
  CHECK(EnumSystemLocalesA(enum_locale_a, 2) && locale_count == 2);
  locale_count = 0, locale_stop = 1;
  CHECK(EnumSystemLocalesA(enum_locale_a, 1) && locale_count == 1);
  locale_count = 0, locale_stop = 0;
  CHECK(EnumSystemLocalesA(enum_locale_a, 4) && locale_count == 0);
  CHECK(!EnumSystemLocalesA(enum_locale_a, 3) && last_error == 1004);
  CHECK(!EnumSystemLocalesA(enum_locale_a, 8) && last_error == 1004);
  CHECK(!EnumSystemLocalesA(0, 1) && last_error == 87);
  struct {
    DWORD max;
    uint8_t def[2], lead[12];
    WCHAR udef;
    DWORD page;
    WCHAR name[260];
  } cp;
  CHECK(GetCPInfoExW(0, 0, &cp) && cp.max == 1 && cp.def[0] == '?' && cp.page == 1252 && cp.name[0] == '1');
  CHECK(GetCPInfoExW(1, 0, &cp) && cp.page == 437 && GetCPInfoExW(850, 0, &cp));
  CHECK(!GetCPInfoExW(932, 0, &cp) && last_error == 87 && !GetCPInfoExW(1252, 1, &cp));
  uint16_t types[8];
  CHECK(GetStringTypeExW(0x409, 1, W("Az5 ,\x80\xe9"), 7, types) && types[0] == 0x381 &&
        types[1] == 0x302 && types[2] == 0x284 && types[3] == 0x248 && types[4] == 0x210 &&
        types[5] == 0x210 && types[6] == 0x302);
  CHECK(GetStringTypeExW(0x409, 2, W("a1+$,\t "), -1, types) && types[0] == 1 && types[1] == 3 &&
        types[2] == 4 && types[3] == 5 && types[4] == 7 && types[5] == 9 && types[6] == 10 &&
        types[7] == 0);
  CHECK(GetStringTypeExW(0x409, 4, W("a,"), 2, types) && types[0] == 0x8000 && types[1] == 8);
  CHECK(!GetStringTypeExW(0x409, 3, W("a"), 1, types) && last_error == 1004);
  CHECK(!GetStringTypeExW(0x409, 1, 0, 1, types) && last_error == 87);
  CHECK(GetStringTypeExA(0x409, 1, "\xc9z", 2, types) && types[0] == 0x301 && types[1] == 0x302);
  CHECK(GetStringTypeA(0x409, 1, "9", -1, types) && types[0] == 0x284 && types[1] == 0x220);
  CHECK(!GetStringTypeA(0x409, 1, "9", 0, types) && last_error == 87);
  CHECK(CompareStringOrdinal(W("abc"), -1, W("abd"), -1, 0) == 1 &&
        CompareStringOrdinal(W("abc"), 3, W("ABC"), 3, 1) == 2 &&
        CompareStringOrdinal(W("abc"), -1, W("ab"), -1, 0) == 3 &&
        CompareStringOrdinal(W("a"), -1, W("B"), -1, 0) == 3 &&
        CompareStringOrdinal(W("a"), -1, W("B"), -1, 1) == 1);
  CHECK(!CompareStringOrdinal(0, 1, W("a"), 1, 0) && last_error == 87);
  SYSTEMTIME d = {1999, 12, 0, 31, 23, 5, 9, 0};
  CHECK(GetDateFormatA(0x409, 0, &d, 0, a, 64) == 9 && !strcmp(a, "12/31/99"));
  CHECK(GetDateFormatA(0x409, 2, &d, 0, a, 64) && !strcmp(a, "Friday, December 31, 1999"));
  CHECK(GetDateFormatA(0x409, 8, &d, 0, a, 64) && !strcmp(a, "December, 1999"));
  CHECK(GetDateFormatA(0x409, 0, &d, "ddd dd MMM yyyy 'at' ''g''", a, 64) &&
        !strcmp(a, "Fri 31 Dec 1999 at 'A.D.'"));
  CHECK(GetDateFormatA(0x409, 0, &d, "d/M/y", 0, 0) == 9);
  CHECK(!GetDateFormatA(0x409, 0, &d, 0, a, 3) && last_error == 122);
  CHECK(!GetDateFormatA(0x409, 1, &d, "d", a, 64) && last_error == 1004);
  CHECK(!GetDateFormatA(0x409, 3, &d, 0, a, 64) && last_error == 1004);
  SYSTEMTIME bad = {1999, 2, 0, 29, 0, 0, 0, 0};
  CHECK(!GetDateFormatA(0x409, 0, &bad, 0, a, 64) && last_error == 87);
  CHECK(!GetDateFormatA(0x40c, 0, &d, 0, a, 64) && last_error == 87);
  CHECK(GetDateFormatA(0x409, 0, 0, "yyyy-MM-dd", a, 64) == 11 && !strcmp(a, "2026-10-19"));
  CHECK(GetDateFormatW(0x409, 2, &d, 0, w, 300) == 26 && wide_equal(w, "Friday, December 31, 1999"));
  CHECK(GetDateFormatW(0x409, 0, &d, W("MMMM"), 0, 0) == 9);
  CHECK(!GetDateFormatW(0x409, 0, &d, han, w, 300) && last_error == 1113);
  CHECK(GetTimeFormatA(0x409, 0, &d, 0, a, 64) && !strcmp(a, "11:05:09 PM"));
  CHECK(GetTimeFormatA(0x409, 2, &d, 0, a, 64) && !strcmp(a, "11:05 PM"));
  CHECK(GetTimeFormatA(0x409, 1, &d, 0, a, 64) && !strcmp(a, "11 PM"));
  CHECK(GetTimeFormatA(0x409, 4, &d, 0, a, 64) && !strcmp(a, "11:05:09"));
  CHECK(GetTimeFormatA(0x409, 8, &d, "hh:mm t", a, 64) && !strcmp(a, "23:05 P"));
  SYSTEMTIME morning = {2000, 1, 0, 1, 0, 7, 0, 0};
  CHECK(GetTimeFormatA(0x409, 0, &morning, "h 'h' HH tt", a, 64) && !strcmp(a, "12 h 00 AM"));
  SYSTEMTIME late = {2000, 1, 0, 1, 24, 0, 0, 0};
  CHECK(!GetTimeFormatA(0x409, 0, &late, 0, a, 64) && last_error == 87);
  CHECK(!GetTimeFormatA(0x409, 0x10, &d, 0, a, 64) && last_error == 1004);
  CHECK(!GetTimeFormatA(0x409, 0, &d, 0, a, 4) && last_error == 122);
  CHECK(GetTimeFormatW(0x409, 0, 0, 0, w, 300) == 12 && wide_equal(w, "12:34:56 PM"));
  CHECK(GetNumberFormatA(0x409, 0, "1234567.891", 0, a, 64) && !strcmp(a, "1,234,567.89"));
  CHECK(GetNumberFormatA(0x409, 0, "-0.005", 0, a, 64) && !strcmp(a, "-0.01"));
  CHECK(GetNumberFormatA(0x409, 0, "-0.004", 0, a, 64) && !strcmp(a, "0.00"));
  CHECK(GetNumberFormatA(0x409, 0, "999.995", 0, a, 64) && !strcmp(a, "1,000.00"));
  struct {
    DWORD digits, lz, grouping;
    const char *dec, *thou;
    DWORD neg;
  } nf = {0, 0, 32, ",", ".", 0};
  CHECK(GetNumberFormatA(0x409, 0, "-12345678.5", &nf, a, 64) && !strcmp(a, "(1.23.45.679)"));
  nf.digits = 3;
  nf.neg = 4;
  CHECK(GetNumberFormatA(0x409, 0, "-.5", &nf, a, 64) && !strcmp(a, ",500 -"));
  CHECK(!GetNumberFormatA(0x409, 0, "12a", 0, a, 64) && last_error == 87);
  CHECK(!GetNumberFormatA(0x409, 0, "", 0, a, 64) && last_error == 87);
  CHECK(!GetNumberFormatA(0x409, 0x80000000u, "1", &nf, a, 64) && last_error == 1004);
  CHECK(!GetNumberFormatA(0x409, 0, "1234", 0, a, 4) && last_error == 122);
  CHECK(GetNumberFormatW(0x409, 0, W("1000"), 0, w, 300) == 9 && wide_equal(w, "1,000.00"));
  struct {
    DWORD digits, lz, grouping;
    const WCHAR *dec, *thou;
    DWORD neg;
  } nfw = {1, 1, 3, W(","), W(" "), 1};
  CHECK(GetNumberFormatW(0x409, 0, W("-1234.56"), &nfw, w, 300) && wide_equal(w, "-1 234,6"));
  CHECK(!GetNumberFormatW(0x409, 0, han, 0, w, 300) && last_error == 1113);
  CHECK(GetCurrencyFormatA(0x409, 0, "1234.5", 0, a, 64) && !strcmp(a, "$1,234.50"));
  CHECK(GetCurrencyFormatA(0x409, 0, "-1234.5", 0, a, 64) && !strcmp(a, "($1,234.50)"));
  struct {
    DWORD digits, lz, grouping;
    const char *dec, *thou;
    DWORD neg, pos;
    const char *sym;
  } cf = {2, 1, 3, ",", ".", 8, 3, "EUR"};
  CHECK(GetCurrencyFormatA(0x409, 0, "-5", &cf, a, 64) && !strcmp(a, "-5,00 EUR"));
  CHECK(GetCurrencyFormatA(0x409, 0, "5", &cf, a, 64) && !strcmp(a, "5,00 EUR"));
  cf.neg = 16;
  CHECK(!GetCurrencyFormatA(0x409, 0, "-5", &cf, a, 64) && last_error == 87);
  CHECK(GetCurrencyFormatW(0x409, 0, W("0.5"), 0, w, 300) == 6 && wide_equal(w, "$0.50"));
  CHECK(!GetCurrencyFormatW(0x409, 0, W("x"), 0, w, 300) && last_error == 87);
  regional_nls_tests();
  puts("PASS kernel32 upper NLS LCID names locale info code pages types dates times numbers");
}
static void message_tests(void) {
  char a[256];
  WCHAR w[256];
  uintptr_t args[6] = {(uintptr_t) "world", 42, (uintptr_t) "x", 5, 0xbeef, (uintptr_t)L""};
  CHECK(FormatMessageA(0x2400, "Hello %1!s! %2!d!%%%n%3", 0, 0, a, 256, args) == 18 &&
        !strcmp(a, "Hello world 42%\r\nx"));
  CHECK(FormatMessageA(0x2400, "[%2!5d!][%2!-4u!][%5!08X!][%5!#x!][%4!c!]%0tail", 0, 0, a, 256, args) &&
        !strcmp(a, "[   42][42  ][0000BEEF][0xbeef][\x05]"));
  uintptr_t star[3] = {6, 3, (uintptr_t) "abcdef"};
  CHECK(FormatMessageA(0x2400, "<%1!*.*s!>", 0, 0, a, 256, star) && !strcmp(a, "<   abc>"));
  CHECK(FormatMessageA(0x2600, "keep %1 and %2!d! % %. %!", 0, 0, a, 256, 0) &&
        !strcmp(a, "keep %1 and %2!d!  . !"));
  CHECK(!FormatMessageA(0x2400, "%1", 0, 0, a, 256, 0) && last_error == 87);
  CHECK(!FormatMessageA(0x2400, "%1!z!", 0, 0, a, 256, args) && last_error == 87);
  CHECK(!FormatMessageA(0x2400, "too long", 0, 0, a, 5, args) && last_error == 122);
  char *allocated = 0;
  CHECK(FormatMessageA(0x2500, "%1", 0, 0, (char *)&allocated, 0, args) == 5 &&
        !strcmp(allocated, "world") && local_allocs == 1);
  free(allocated);
  CHECK(FormatMessageA(0x1000, 0, 2, 0, a, 256, 0) == 44 &&
        !strcmp(a, "The system cannot find the file specified.\r\n"));
  CHECK(FormatMessageA(0x1200, 0, 193, 0x409, a, 256, 0) &&
        !strcmp(a, "%1 is not a valid Win32 application.\r\n"));
  uintptr_t app[1] = {(uintptr_t) "GAME.EXE"};
  CHECK(FormatMessageA(0x3000, 0, 193, 0, a, 256, app) &&
        !strcmp(a, "GAME.EXE is not a valid Win32 application.\r\n"));
  CHECK(FormatMessageA(0x1000, 0, 0x80070005u, 0, a, 256, 0) && !strcmp(a, "Access is denied.\r\n"));
  CHECK(FormatMessageA(0x10ff, 0, 5, 0, a, 256, 0) && !strcmp(a, "Access is denied. "));
  CHECK(!FormatMessageA(0x1000, 0, 12345, 0, a, 256, 0) && last_error == 317);
  CHECK(!FormatMessageA(0x1000, 0, 2, 0x407, a, 256, 0) && last_error == 15100);
  CHECK(!FormatMessageA(0x1400, "x", 2, 0, a, 256, 0) && last_error == 87);
  CHECK(!FormatMessageA(0, "x", 2, 0, a, 256, 0) && last_error == 87);
  /* MESSAGE_RESOURCE_DATA: one block 100..101; an ANSI and a UTF-16 entry. */
  memset(table, 0, sizeof(table));
  w32_put32(table, 1);
  w32_put32(table + 4, 100);
  w32_put32(table + 8, 101);
  w32_put32(table + 12, 16);
  table[16] = 12;
  memcpy(table + 20, "Ansi %1\0", 8);
  table[28] = 16;
  table[30] = 1;
  const WCHAR text[] = {'W', 'i', 'd', 'e', ' ', 0xe9, 0};
  memcpy(table + 32, text, sizeof(text));
  uintptr_t one[1] = {(uintptr_t) "m"};
  CHECK(FormatMessageA(0x2800, (void *)(uintptr_t)0x800, 100, 0, a, 256, one) == 6 && !strcmp(a, "Ansi m"));
  CHECK(FormatMessageW(0x800, (void *)(uintptr_t)0x800, 101, 0, w, 256, 0) == 6 && w[5] == 0xe9);
  CHECK(!FormatMessageW(0x800, (void *)(uintptr_t)0x800, 102, 0, w, 256, 0) && last_error == 317);
  CHECK(!FormatMessageW(0x800, (void *)(uintptr_t)0x804, 100, 0, w, 256, 0) && last_error == 317);
  CHECK(FormatMessageW(0x1800, (void *)(uintptr_t)0x804, 87, 0, w, 256, 0) == 29);
  uintptr_t wide_args[2] = {(uintptr_t)W("\x80uro"), (uintptr_t) "narrow"};
  CHECK(FormatMessageW(0x2400, W("%1 %2!S!"), 0, 0, w, 256, wide_args) == 11 && w[0] == 0x20ac &&
        wide_equal(w + 5, "narrow"));
  /* The i386 va_list form: a pointer to the argument words. */
  uintptr_t words[2] = {(uintptr_t) "va", 7};
  uintptr_t *list = words;
  CHECK(FormatMessageA(0x400, "%1-%2!u!", 0, 0, a, 256, &list) && !strcmp(a, "va-7"));
  (void)args[5];
  puts("PASS kernel32 upper FormatMessage strings tables system inserts va_list allocation");
}
static DWORD W32API work(void *p) {
  items += (int)(uintptr_t)p;
  return 0;
}
static void W32API timer_cb(void *p, BOOLEAN fired) {
  CHECK(fired && p == (void *)&timer_calls);
  timer_calls++;
}
static int once_runs;
static BOOL W32API once_cb(uintptr_t *o, void *p, void **c) {
  (void)o;
  once_runs++;
  if (!p)
    return 0;
  *c = p;
  return 1;
}
static void *W32API dll_hook(ULONG reason, void *info) {
  struct {
    ULONG size;
    const void *d;
    void **thunk;
    const char *dll;
    ULONG by_name;
    const char *name;
    void *module, *unused;
    ULONG error;
  } *i = info;
  CHECK(i->size == sizeof(*i) && i->by_name && reason == 4 && !strcmp(i->name, "Missing") &&
        i->error == 127);
  return (void *)work;
}
static void *W32API sys_hook(const char *dll, const char *name) {
  (void)name;
  return !strcmp(dll, "absent.dll") ? (void *)timer_cb : 0;
}
static void runtime_tests(void) {
  CHECK(Beep(440, 100) && Beep(0, 0));
  HANDLE r = 0, wpipe = 0;
  CHECK(!CreatePipe(&r, &wpipe, 0, 0) && last_error == 120 && !r && !wpipe);
  CHECK(!CreatePipe(0, &wpipe, 0, 0) && last_error == 87);
  CHECK(!PostQueuedCompletionStatus((HANDLE)1, 0, 0, 0) && last_error == 120);
  counter = 3723004000ull;
  CHECK(GetTickCount64() == 3723004);
  counter = 5000000000123ull;
  CHECK(GetTickCount64() == 5000000000ull);
  CHECK(QueueUserWorkItem(work, (void *)3, 0x10) && items == 3);
  CHECK(!QueueUserWorkItem(0, 0, 0) && last_error == 87);
  HANDLE t = 0;
  waits = 0;
  CHECK(CreateTimerQueueTimer(&t, 0, timer_cb, &timer_calls, 10, 5, 0) && t && timer_calls == 2);
  waits = 0;
  CHECK(CreateTimerQueueTimer(&t, 0, timer_cb, &timer_calls, 10, 5, 8) && timer_calls == 3);
  CHECK(!CreateTimerQueueTimer(0, 0, timer_cb, 0, 0, 0, 0) && last_error == 87);
  uintptr_t once = 0;
  void *ctx = 0;
  CHECK(!InitOnceExecuteOnce(&once, once_cb, 0, &ctx) && !once && once_runs == 1);
  CHECK(InitOnceExecuteOnce(&once, once_cb, (void *)0x1230, &ctx) && ctx == (void *)0x1230 &&
        once == 0x1232);
  ctx = 0;
  CHECK(InitOnceExecuteOnce(&once, once_cb, (void *)0x9990, &ctx) && ctx == (void *)0x1230 &&
        once_runs == 2);
  CHECK(!InitOnceExecuteOnce(0, once_cb, 0, 0) && last_error == 87);
  RTL_CRITICAL_SECTION cs;
  CHECK(InitializeCriticalSectionEx(&cs, 4000, 0x01000000) && cs.SpinCount == 4000);
  CHECK(!InitializeCriticalSectionEx(&cs, 0, 1) && last_error == 87);
  /* f5-20C: RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO allocates an owned
   * debug block (Wine names sections in Spare[0]); NO_DEBUG_INFO wins. */
  memset(&cs, 0, sizeof(cs));
  CHECK(InitializeCriticalSectionEx(&cs, 0, 0x10000000) && cs.DebugInfo && debug_blocks == 1);
  RTL_CRITICAL_SECTION_DEBUG *debug = cs.DebugInfo;
  CHECK(debug->CreatorBackTraceIndex == W32_CS_DEBUG_OWNED && debug->CriticalSection == &cs &&
        !debug->Spare[0] && debug->ProcessLocksList.Flink == &debug->ProcessLocksList);
  CHECK(RtlFreeHeap(&peb, 0, debug) && !debug_blocks);
  memset(&cs, 0, sizeof(cs));
  CHECK(InitializeCriticalSectionEx(&cs, 0, 0x11000000) && !cs.DebugInfo && !debug_blocks);
  CHECK(InitializeCriticalSectionEx(&cs, 0, 0) && !cs.DebugInfo && !debug_blocks);
  fail_heap = 1;
  CHECK(!InitializeCriticalSectionEx(&cs, 0, 0x10000000) && last_error == 8 && !debug_blocks);
  fail_heap = 0;
  /* Delay-load descriptor in a synthetic image: RVA-based attributes. */
  struct {
    uint32_t attributes, dll, module, iat, names, bound, unload, stamp;
    uintptr_t handle;
    void *table[3];
    uintptr_t lookup[3];
    char text[64];
  } image;
  memset(&image, 0, sizeof(image));
  uint8_t *base = (uint8_t *)&image;
  image.attributes = 1;
  image.dll = (uint32_t)((uint8_t *)image.text - base);
  image.module = (uint32_t)((uint8_t *)&image.handle - base);
  image.iat = (uint32_t)((uint8_t *)image.table - base);
  image.names = (uint32_t)((uint8_t *)image.lookup - base);
  strcpy(image.text, "real.dll");
  strcpy(image.text + 16 + 2, "Present");
  strcpy(image.text + 32 + 2, "Missing");
  image.lookup[0] = 16 + (uintptr_t)((uint8_t *)image.text - base);
  image.lookup[1] = ((uintptr_t)1 << (sizeof(uintptr_t) * 8 - 1)) | 5;
  image.lookup[2] = 32 + (uintptr_t)((uint8_t *)image.text - base);
  loads = frees = 0;
  CHECK(ResolveDelayLoadedAPI(base, base, 0, sys_hook, &image.table[0], 0) == (void *)target &&
        image.table[0] == (void *)target && image.handle == 0x700 && loads == 1);
  CHECK(ResolveDelayLoadedAPI(base, base, 0, sys_hook, &image.table[1], 0) == (void *)target &&
        loads == 1);
  CHECK(ResolveDelayLoadedAPI(base, base, dll_hook, sys_hook, &image.table[2], 0) == (void *)work &&
        image.table[2] == (void *)work);
  image.table[2] = 0;
  CHECK(!ResolveDelayLoadedAPI(base, base, 0, sys_hook, &image.table[2], 0) && !image.table[2]);
  strcpy(image.text, "absent.dll");
  image.handle = 0;
  CHECK(ResolveDelayLoadedAPI(base, base, 0, sys_hook, &image.table[0], 0) == (void *)timer_cb);
  diagnostics = 0;
  CHECK(!DelayLoadFailureHook("absent.dll", "Fn") && raised == 0xc06d007eu && last_error == 126 &&
        !strcmp(last_diag, "absent.dll!Fn"));
  CHECK(!DelayLoadFailureHook("real.dll", (const char *)7) && raised == 0xc06d007fu &&
        last_error == 127 && !strcmp(last_diag, "real.dll!#"));
  uint16_t x = GlobalAddAtomA("Caf\xe9"), y = GlobalAddAtomW(W("caf\xe9"));
  CHECK(x == 0xc000 && y == x && GlobalFindAtomA("caf\xe9") == x && GlobalFindAtomW(W("Caf\xe9")) == x);
  CHECK(!GlobalFindAtomA("none") && last_error == 2);
  CHECK(GlobalAddAtomA((const char *)(uintptr_t)42) == 42 && !GlobalAddAtomA(0) && last_error == 87);
  CHECK(!GlobalAddAtomA((const char *)(uintptr_t)0xc001) && last_error == 87);
  char name[16];
  WCHAR wname[16];
  CHECK(GlobalGetAtomNameA(x, name, 16) == 4 && !strcmp(name, "Caf\xe9"));
  CHECK(GlobalGetAtomNameW(x, wname, 16) == 4 && wname[3] == 0xe9);
  CHECK(GlobalGetAtomNameA(x, name, 3) == 2 && !strcmp(name, "Ca"));
  CHECK(!GlobalGetAtomNameA(x, name, 0) && last_error == 87);
  CHECK(!GlobalDeleteAtom(x) && GlobalFindAtomA("caf\xe9") == x && !GlobalDeleteAtom(x));
  CHECK(!GlobalFindAtomA("caf\xe9") && GlobalDeleteAtom(x) == x && last_error == 6);
  CHECK(!GlobalDeleteAtom(42));
  WCHAR long_atom[300];
  for (unsigned i = 0; i < 299; i++)
    long_atom[i] = 'a';
  long_atom[299] = 0;
  CHECK(!GlobalAddAtomW(long_atom) && last_error == 87);
  /* US Eastern: UTC-5, daylight -60 from the 2nd Sunday of March 2:00 to
   * the 1st Sunday of November 2:00. */
  TIME_ZONE_INFORMATION z;
  memset(&z, 0, sizeof(z));
  z.bias = 300;
  z.daylight_bias = -60;
  z.standard_date = (SYSTEMTIME){0, 11, 0, 1, 2, 0, 0, 0};
  z.daylight_date = (SYSTEMTIME){0, 3, 0, 2, 2, 0, 0, 0};
  SYSTEMTIME utc = {2026, 7, 0, 4, 16, 0, 0, 0}, local;
  CHECK(SystemTimeToTzSpecificLocalTime(&z, &utc, &local) && local.hour == 12 && local.day == 4);
  utc = (SYSTEMTIME){2026, 1, 0, 1, 3, 0, 0, 0};
  CHECK(SystemTimeToTzSpecificLocalTime(&z, &utc, &local) && local.year == 2025 &&
        local.day == 31 && local.hour == 22);
  utc = (SYSTEMTIME){2026, 3, 0, 8, 6, 59, 0, 0};
  CHECK(SystemTimeToTzSpecificLocalTime(&z, &utc, &local) && local.hour == 1);
  utc.minute = 0;
  utc.hour = 7;
  CHECK(SystemTimeToTzSpecificLocalTime(&z, &utc, &local) && local.hour == 3);
  utc = (SYSTEMTIME){2026, 11, 0, 1, 5, 59, 0, 0};
  CHECK(SystemTimeToTzSpecificLocalTime(&z, &utc, &local) && local.hour == 1 && local.minute == 59);
  utc.hour = 6;
  utc.minute = 0;
  CHECK(SystemTimeToTzSpecificLocalTime(&z, &utc, &local) && local.hour == 1 && !local.minute);
  CHECK(SystemTimeToTzSpecificLocalTime(0, &utc, &local) && local.hour == 6);
  CHECK(!SystemTimeToTzSpecificLocalTime(&z, 0, &local) && last_error == 87);
  SYSTEMTIME invalid = {2026, 13, 0, 1, 0, 0, 0, 0};
  CHECK(!SystemTimeToTzSpecificLocalTime(&z, &invalid, &local) && last_error == 87);
  puts("PASS kernel32 upper Beep pipes completion ticks work items timers once delay-load atoms time zones");
}
static void console_and_w21(void) {
  static RTL_CRITICAL_SECTION lock;
  (void)lock;
  DWORD mode = 0, n = 9;
  HANDLE file = CreateFileA("C:\\x.log", 0, 0, 0, 2, 0, 0);
  CHECK(!GetConsoleMode(file, &mode) && last_error == 6 && !VerifyConsoleIoHandle(file));
  CHECK(!GetConsoleMode(file, 0) && last_error == 87);
  CHECK(AllocConsole());
  HANDLE console = parameters.ConsoleHandle;
  CHECK(console && GetConsoleMode(console, &mode) && mode == 7 && VerifyConsoleIoHandle(console));
  CHECK(SetConsoleMode(console, 3) && GetConsoleMode(console, &mode) && mode == 3);
  CHECK(!SetConsoleMode(console, 0x400) && last_error == 87 && !SetConsoleMode(file, 1) && last_error == 6);
  uint8_t records[64];
  CHECK(PeekConsoleInputA(console, records, 1, &n) && n == 0);
  CHECK(!PeekConsoleInputA(file, records, 1, &n) && last_error == 6);
  char text[8];
  n = 9;
  CHECK(ReadConsoleA(console, text, 8, &n, 0) && n == 0);
  CHECK(!ReadConsoleA(file, text, 8, &n, 0) && last_error == 6);
  n = 9;
  CHECK(!ReadConsoleInputW(console, records, 1, &n) && n == 0 && last_error == 38);
  CHECK(!ReadConsoleInputW(file, records, 1, &n) && last_error == 6);
  CHECK(!ReadConsoleInputW(console, records, 1, 0) && last_error == 87);
  CloseHandle(file);
  uintptr_t cookie = 0xfeed;
  HANDLE ctx = (HANDLE)(uintptr_t)0xfeed;
  void *old = (void *)0xfeed;
  WCHAR out[4] = {0xaaaa};
  diagnostics = 0;
  CHECK(!ActivateActCtx(0, &cookie) && last_error == 120 && cookie == 0xfeed);
  CHECK(CreateActCtxW(0) == K32_INVALID && last_error == 120);
  CHECK(!DeactivateActCtx(0, cookie) && last_error == 120);
  CHECK(!FindActCtxSectionGuid(0, 0, 2, 0, 0) && last_error == 120);
  CHECK(!FindActCtxSectionStringW(0, 0, 2, W("x"), 0) && last_error == 120);
  CHECK(!GetCurrentActCtx(&ctx) && ctx == (HANDLE)(uintptr_t)0xfeed && last_error == 120);
  last_error = 0;
  ReleaseActCtx(ctx);
  CHECK(last_error == 120);
  CHECK(!GetSystemWow64DirectoryW(out, 4) && out[0] == 0xaaaa && last_error == 120);
  CHECK(!Wow64DisableWow64FsRedirection(&old) && old == (void *)0xfeed && last_error == 120);
  CHECK(!Wow64RevertWow64FsRedirection(old) && last_error == 120);
  CHECK(SetThreadDescription((HANDLE)1, W("t")) == (LONG)0x80004001 && last_error == 120);
  CHECK(diagnostics == 11);
  CHECK(!ActivateActCtx(0, &cookie) && diagnostics == 11);
  puts("PASS kernel32 upper console log handle queries W21 activation context WOW64 description");
}
int main(void) {
  teb.ProcessEnvironmentBlock = &peb;
  peb.ProcessParameters = &parameters;
  unicode_layer();
  search_and_legacy_files();
  locale_tests();
  message_tests();
  runtime_tests();
  console_and_w21();
  printf("PASS kernel32 upper checks=%u\n", checks);
  return 0;
}
