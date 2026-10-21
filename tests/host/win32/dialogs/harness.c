/* f5-10C host harness: production USER (dialogs, controls, menus, message
 * boxes) and GDI (font selection over FreeType) in one process over the
 * production kernel queues. Host services: files, registry, modules and the
 * fixture resources. SPDX-License-Identifier: LGPL-2.1-or-later */
#define U32_TEST_EXTERNAL_MODULES
#define main user32_fixture_main
#include "user32_test.c"
#undef main
#include "../../../../sdk/win32/user32/classic.h"
#include "resources.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
extern void *gdi_symbol(const char *);
extern int gdi_host_fonts(const char *, const char *);
#define GDI(type, name) ((type)gdi_symbol(#name))
#define FIXTURE ((HINSTANCE)(uintptr_t)0x400000)
static char fonts_dir[512], install_dir[512], drive_dir[512], out_dir[512];

/* ---- files: C:\WINDOWS\FONTS is the union of the read-only system faces
 * and the writable install directory; other C:\ paths map under drive_c. */
static int host_path(const char *w32, char *out, size_t cap, int writing) {
  static const char fonts[] = "C:\\WINDOWS\\FONTS\\";
  size_t n = strlen(fonts);
  if (!strncasecmp(w32, fonts, n) && !strchr(w32 + n, '\\')) {
    snprintf(out, cap, "%s/%s", install_dir, w32 + n);
    struct stat st;
    if (!writing && stat(out, &st))
      snprintf(out, cap, "%s/%s", fonts_dir, w32 + n);
    return 1;
  }
  if (strncasecmp(w32, "C:\\", 3))
    return 0;
  snprintf(out, cap, "%s/%s", drive_dir, w32 + 3);
  for (char *p = out + strlen(drive_dir); *p; p++)
    if (*p == '\\')
      *p = '/';
  if (writing)
    for (char *p = out + strlen(drive_dir) + 1; *p; p++)
      if (*p == '/') {
        *p = 0;
        mkdir(out, 0755);
        *p = '/';
      }
  return 1;
}
#define FILE_BASE 0x7000
HANDLE W32API CreateFileA(const char *name, DWORD access, DWORD share,
                          SECURITY_ATTRIBUTES *security, DWORD creation,
                          DWORD flags, HANDLE template) {
  (void)share, (void)security, (void)flags, (void)template;
  char path[1024];
  BOOL write = !!(access & 0x40000000u);
  if (!name || !host_path(name, path, sizeof(path), write)) {
    SetLastError(3);
    return K32_INVALID;
  }
  int mode = write ? O_RDWR : O_RDONLY;
  if (creation == 1)
    mode |= O_CREAT | O_EXCL;
  else if (creation == 2)
    mode |= O_CREAT | O_TRUNC;
  int fd = open(path, mode, 0644);
  if (fd < 0) {
    SetLastError(errno == EEXIST ? 80 : 2);
    return K32_INVALID;
  }
  return (HANDLE)(uintptr_t)(FILE_BASE + fd);
}
static int fd_of(HANDLE h) {
  uintptr_t v = (uintptr_t)h;
  return v >= FILE_BASE && v < FILE_BASE + 4096 ? (int)(v - FILE_BASE) : -1;
}
BOOL W32API ReadFile(HANDLE h, void *data, DWORD bytes, DWORD *count,
                     OVERLAPPED *o) {
  (void)o;
  int fd = fd_of(h);
  ssize_t n = fd >= 0 ? read(fd, data, bytes) : -1;
  if (count)
    *count = n > 0 ? (DWORD)n : 0;
  return n >= 0;
}
BOOL W32API WriteFile(HANDLE h, const void *data, DWORD bytes, DWORD *count,
                      OVERLAPPED *o) {
  (void)o;
  int fd = fd_of(h);
  ssize_t n = fd >= 0 ? write(fd, data, bytes) : -1;
  if (count)
    *count = n > 0 ? (DWORD)n : 0;
  return n >= 0;
}
void dialogs_fixture_stderr(const char *text, DWORD bytes) {
  CHECK(write(2, text, bytes) == bytes);
}
DWORD W32API GetFileSize(HANDLE h, DWORD *high) {
  struct stat st;
  int fd = fd_of(h);
  if (fd < 0 || fstat(fd, &st))
    return 0xffffffffu;
  if (high)
    *high = 0;
  return (DWORD)st.st_size;
}
DWORD W32API SetFilePointer(HANDLE h, LONG at, LONG *high, DWORD how) {
  (void)high;
  int fd = fd_of(h);
  off_t r = fd >= 0 ? lseek(fd, at, how == 0 ? SEEK_SET : how == 1 ? SEEK_CUR
                                                                   : SEEK_END)
                    : -1;
  return r < 0 ? 0xffffffffu : (DWORD)r;
}
BOOL W32API CloseHandle(HANDLE h) {
  int fd = fd_of(h);
  if (fd >= 0)
    close(fd);
  return 1;
}
static BOOL copy_host(const char *from, const char *to, BOOL fail_exists) {
  int in = open(from, O_RDONLY);
  if (in < 0)
    return 0;
  int out = open(to, O_WRONLY | O_CREAT | (fail_exists ? O_EXCL : O_TRUNC),
                 0644);
  if (out < 0) {
    close(in);
    return 0;
  }
  char buffer[65536];
  ssize_t n;
  BOOL ok = 1;
  while ((n = read(in, buffer, sizeof(buffer))) > 0)
    if (write(out, buffer, (size_t)n) != n)
      ok = 0;
  close(in);
  close(out);
  return ok && n == 0;
}
BOOL W32API CopyFileA(const char *a, const char *b, BOOL fail_exists) {
  char from[1024], to[1024];
  if (!host_path(a, from, sizeof(from), 0) || !host_path(b, to, sizeof(to), 1))
    return 0;
  if (!copy_host(from, to, fail_exists)) {
    SetLastError(80);
    return 0;
  }
  return 1;
}
BOOL W32API DeleteFileA(const char *a) {
  char path[1024];
  if (!host_path(a, path, sizeof(path), 1))
    return 0;
  return !unlink(path);
}

/* ---- registry and WIN.INI ------------------------------------------------ */
struct reg_value {
  char key[160], name[128];
  DWORD type, size;
  uint8_t data[512];
};
static struct reg_value reg_values[128];
static char reg_keys[32][160];
static LONG W32API reg_open(void *root, const char *path, DWORD o, DWORD sam,
                            void **out) {
  (void)root, (void)o, (void)sam;
  for (UINT i = 0; i < 32; i++)
    if (reg_keys[i][0] && !strcasecmp(reg_keys[i], path)) {
      *out = (void *)(uintptr_t)(0x9000 + i);
      return 0;
    }
  return 2;
}
static LONG W32API reg_create(void *root, const char *path, DWORD r, char *cls,
                              DWORD o, DWORD sam, void *sa, void **out,
                              DWORD *disposition) {
  (void)r, (void)cls, (void)o, (void)sam, (void)sa;
  if (!reg_open(root, path, 0, 0, out)) {
    if (disposition)
      *disposition = 2;
    return 0;
  }
  for (UINT i = 0; i < 32; i++)
    if (!reg_keys[i][0]) {
      snprintf(reg_keys[i], sizeof(reg_keys[i]), "%s", path);
      *out = (void *)(uintptr_t)(0x9000 + i);
      if (disposition)
        *disposition = 1;
      return 0;
    }
  return 8;
}
static const char *key_name(void *key) {
  uintptr_t i = (uintptr_t)key - 0x9000;
  return i < 32 ? reg_keys[i] : "";
}
static struct reg_value *reg_find(void *key, const char *name, BOOL create) {
  const char *k = key_name(key);
  for (UINT i = 0; i < 128; i++)
    if (reg_values[i].key[0] && !strcasecmp(reg_values[i].key, k) &&
        !strcasecmp(reg_values[i].name, name ? name : ""))
      return &reg_values[i];
  if (!create)
    return 0;
  for (UINT i = 0; i < 128; i++)
    if (!reg_values[i].key[0]) {
      snprintf(reg_values[i].key, sizeof(reg_values[i].key), "%s", k);
      snprintf(reg_values[i].name, sizeof(reg_values[i].name), "%s",
               name ? name : "");
      return &reg_values[i];
    }
  return 0;
}
static LONG W32API reg_query(void *key, const char *name, DWORD *r,
                             DWORD *type, uint8_t *data, DWORD *size) {
  (void)r;
  struct reg_value *v = reg_find(key, name, 0);
  if (!v)
    return 2;
  if (type)
    *type = v->type;
  if (data && size && *size < v->size)
    return 234;
  if (data)
    memcpy(data, v->data, v->size);
  if (size)
    *size = v->size;
  return 0;
}
static LONG W32API reg_set(void *key, const char *name, DWORD r, DWORD type,
                           const uint8_t *data, DWORD size) {
  (void)r;
  struct reg_value *v = reg_find(key, name, 1);
  if (!v || size > sizeof(v->data))
    return 8;
  v->type = type;
  v->size = size;
  memcpy(v->data, data, size);
  return 0;
}
static LONG W32API reg_enum(void *key, DWORD index, char *name, DWORD *name_n,
                            DWORD *r, DWORD *type, uint8_t *data,
                            DWORD *data_n) {
  (void)r;
  const char *k = key_name(key);
  DWORD seen = 0;
  for (UINT i = 0; i < 128; i++) {
    struct reg_value *v = &reg_values[i];
    if (!v->key[0] || strcasecmp(v->key, k))
      continue;
    if (seen++ != index)
      continue;
    snprintf(name, *name_n, "%s", v->name);
    *name_n = (DWORD)strlen(v->name);
    if (type)
      *type = v->type;
    if (data && data_n) {
      DWORD n = v->size < *data_n ? v->size : *data_n;
      memcpy(data, v->data, n);
      *data_n = n;
    }
    return 0;
  }
  return 259;
}
static LONG W32API reg_delete(void *key, const char *name) {
  struct reg_value *v = reg_find(key, name, 0);
  if (!v)
    return 2;
  memset(v, 0, sizeof(*v));
  return 0;
}
static LONG W32API reg_close(void *key) {
  (void)key;
  return 0;
}
static char profile_substitute[2][64];
static DWORD W32API profile_string(const char *app, const char *key,
                                   const char *fallback, char *out,
                                   DWORD n) {
  const char *value = fallback ? fallback : "";
  if (app && key && !strcasecmp(app, "FontSubstitutes") &&
      profile_substitute[0][0] && !strcasecmp(key, profile_substitute[0]))
    value = profile_substitute[1];
  snprintf(out, n, "%s", value);
  return (DWORD)strlen(out);
}
static const char *reg_string(const char *key, const char *name) {
  for (UINT i = 0; i < 128; i++)
    if (reg_values[i].key[0] && !strcasecmp(reg_values[i].key, key) &&
        !strcasecmp(reg_values[i].name, name))
      return (const char *)reg_values[i].data;
  return 0;
}

/* ---- f5-20B: current directory, file search, icon module ------------------ */
static char host_cwd[260] = "C:\\";
static BOOL W32API fake_set_dir(const char *p) {
  char win[600], native[1024];
  struct stat st;
  if (!p)
    return 0;
  if (!strncasecmp(p, "C:\\", 3))
    snprintf(win, sizeof(win), "%s", p);
  else
    snprintf(win, sizeof(win), "%s%s%s", host_cwd,
             host_cwd[strlen(host_cwd) - 1] == '\\' ? "" : "\\", p);
  size_t n = strlen(win);
  while (n > 3 && win[n - 1] == '\\')
    win[--n] = 0;
  if (!host_path(win, native, sizeof(native), 0) || stat(native, &st) ||
      !S_ISDIR(st.st_mode)) {
    SetLastError(3);
    return 0;
  }
  snprintf(host_cwd, sizeof(host_cwd), "%s", win);
  return 1;
}
static DWORD W32API fake_get_dir(DWORD n, char *out) {
  DWORD k = (DWORD)strlen(host_cwd);
  if (n <= k)
    return k + 1;
  memcpy(out, host_cwd, k + 1);
  return k;
}
static struct {
  char names[64][260];
  DWORD attrs[64], count, at;
} found;
static void fill_find(uint8_t *data) {
  memset(data, 0, 320);
  w32_put32(data, found.attrs[found.at]);
  snprintf((char *)data + 44, 260, "%s", found.names[found.at]);
}
static HANDLE W32API fake_find_first(const char *pattern, uint8_t *data) {
  char dir[600], native[1024];
  const char *slash = strrchr(pattern, '\\'), *mask = slash ? slash + 1 : pattern;
  if (slash)
    snprintf(dir, sizeof(dir), "%.*s", (int)(slash - pattern), pattern);
  else
    snprintf(dir, sizeof(dir), "%s", host_cwd);
  if (!strncasecmp(dir, "C:", 2) && !dir[2])
    strcat(dir, "\\");
  if (!host_path(dir, native, sizeof(native), 0))
    return K32_INVALID;
  /* The fixture names are upper case; glob matches them as Windows does. */
  char spec[1400];
  snprintf(spec, sizeof(spec), "%s/%s", native, strcmp(mask, "*.*") ? mask : "*");
  glob_t g;
  found.count = found.at = 0;
  if (glob(spec, 0, 0, &g)) {
    SetLastError(2);
    return K32_INVALID;
  }
  for (size_t i = 0; i < g.gl_pathc && found.count < 64; i++) {
    struct stat st;
    const char *name = strrchr(g.gl_pathv[i], '/') + 1;
    if (stat(g.gl_pathv[i], &st))
      continue;
    snprintf(found.names[found.count], 260, "%s", name);
    found.attrs[found.count++] = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
  }
  globfree(&g);
  if (!found.count) {
    SetLastError(2);
    return K32_INVALID;
  }
  fill_find(data);
  return (HANDLE)(uintptr_t)0x9900;
}
static BOOL W32API fake_find_next(HANDLE h, uint8_t *data) {
  if (h != (HANDLE)(uintptr_t)0x9900 || found.at + 1 >= found.count) {
    SetLastError(18);
    return 0;
  }
  found.at++;
  fill_find(data);
  return 1;
}
static BOOL W32API fake_find_close(HANDLE h) {
  return h == (HANDLE)(uintptr_t)0x9900;
}
static DWORD W32API fake_drives(void) { return 1u << 2; }
#define ICON_MODULE ((HANDLE)(uintptr_t)0x6100)
static const uint8_t *icon_module_image;
static DWORD icon_module_bytes;
static uint8_t icon_group[20] = {0, 0, 1, 0, 1, 0, 32, 32, 0, 0, 1, 0, 32, 0};
static HANDLE W32API fake_load_ex(const char *path, HANDLE file, DWORD flags) {
  (void)file, (void)flags;
  if (path && !strcasecmp(path, "C:\\ICONS.EXE") && icon_module_image)
    return ICON_MODULE;
  SetLastError(2);
  return 0;
}
static BOOL W32API fake_free(HANDLE h) { return h == ICON_MODULE; }
typedef BOOL(W32API *name_proc)(HANDLE, const char *, char *, intptr_t);
static BOOL W32API fake_names(HANDLE m, const char *type, name_proc proc,
                              intptr_t param) {
  if (m != ICON_MODULE || (uintptr_t)type != 14)
    return 0;
  if (proc(m, type, (char *)(uintptr_t)5, param))
    proc(m, type, (char *)(uintptr_t)7, param);
  return 1;
}

/* ---- modules and resources ------------------------------------------------ */
/* Bytes returned by the real loader/resource APIs in kernel_loader_main.c.
 * This marshals between native harnesses with different platform ABIs. */
static struct {
  unsigned id, size;
  uint8_t *data;
} pe_resources[2];
static BOOL refuse_dialog_resource;
HANDLE W32API LoadLibraryA(const char *s) {
  if (!s)
    return 0;
  if (!strcasecmp(s, "gdi32.dll"))
    return (HANDLE)(uintptr_t)0x6001;
  if (!strcasecmp(s, "advapi32.dll"))
    return (HANDLE)(uintptr_t)0x6002;
  if (!strcasecmp(s, "kernel32.dll"))
    return (HANDLE)(uintptr_t)0x6003;
  return 0;
}
HANDLE W32API GetModuleHandleA(const char *s) { return LoadLibraryA(s); }
void *W32API GetProcAddress(HANDLE h, const char *s) {
  uintptr_t m = (uintptr_t)h;
  if (m == 0x6001)
    return gdi_symbol(s);
  if (m == 0x6002) {
    static const struct {
      const char *name;
      void *fn;
    } table[] = {{"RegOpenKeyExA", (void *)reg_open},
                 {"RegCreateKeyExA", (void *)reg_create},
                 {"RegQueryValueExA", (void *)reg_query},
                 {"RegSetValueExA", (void *)reg_set},
                 {"RegEnumValueA", (void *)reg_enum},
                 {"RegDeleteValueA", (void *)reg_delete},
                 {"RegCloseKey", (void *)reg_close}};
    for (UINT i = 0; i < sizeof(table) / sizeof(*table); i++)
      if (!strcmp(table[i].name, s))
        return table[i].fn;
  }
  if (m == 0x6003 && !strcmp(s, "GetProfileStringA"))
    return (void *)profile_string;
  if (m == 0x6003) {
    static const struct {
      const char *name;
      void *fn;
    } kernel[] = {{"WaitForSingleObject", (void *)WaitForSingleObject},
                  {"GetProcessHeap", (void *)GetProcessHeap},
                  {"HeapAlloc", (void *)HeapAlloc},
                  {"LoadLibraryExA", (void *)fake_load_ex},
                  {"FreeLibrary", (void *)fake_free},
                  {"EnumResourceNamesA", (void *)fake_names},
                  {"FindFirstFileA", (void *)fake_find_first},
                  {"FindNextFileA", (void *)fake_find_next},
                  {"FindClose", (void *)fake_find_close},
                  {"SetCurrentDirectoryA", (void *)fake_set_dir},
                  {"GetCurrentDirectoryA", (void *)fake_get_dir},
                  {"GetLogicalDrives", (void *)fake_drives}};
    for (UINT i = 0; i < sizeof(kernel) / sizeof(*kernel); i++)
      if (!strcmp(kernel[i].name, s))
        return kernel[i].fn;
  }
  return 0;
}
HANDLE W32API FindResourceA(HANDLE module, const void *name,
                            const void *type) {
  if (module == ICON_MODULE) {
    uintptr_t t = (uintptr_t)type, n = (uintptr_t)name;
    if (t == 14 && (n == 5 || n == 7)) {
      w32_put32(icon_group + 14, icon_module_bytes);
      w32_put16(icon_group + 18, 1);
      return (HANDLE)(uintptr_t)0x8001;
    }
    if (t == 3 && n == 1)
      return (HANDLE)(uintptr_t)0x8002;
    SetLastError(1813);
    return 0;
  }
  if (module != FIXTURE)
    return 0;
  uintptr_t t = (uintptr_t)type, n = (uintptr_t)name;
  if (refuse_dialog_resource && t == 5 && n == 600) {
    SetLastError(1814);
    return 0;
  }
  for (unsigned i = 0; i < 2; i++)
    if (pe_resources[i].data && pe_resources[i].id == n && t == (i ? 4u : 5u))
      return (HANDLE)&pe_resources[i];
  if (t > 0xffff)
    return 0;
  for (UINT i = 0; i < sizeof(fixture_resources) / sizeof(*fixture_resources);
       i++) {
    const struct fixture_resource *r = &fixture_resources[i];
    if (r->type != t)
      continue;
    if (n <= 0xffff ? r->id == n && !r->name
                    : r->name && !strcasecmp(r->name, (const char *)name))
      if (r->language == 1033 || r->type != 10)
        return (HANDLE)(uintptr_t)(i + 1);
  }
  SetLastError(1813);
  return 0;
}
HANDLE W32API LoadResource(HANDLE module, HANDLE r) {
  (void)module;
  for (unsigned i = 0; i < 2; i++)
    if (r == (HANDLE)&pe_resources[i])
      return pe_resources[i].data;
  uintptr_t i = (uintptr_t)r;
  if (i == 0x8001)
    return (HANDLE)icon_group;
  if (i == 0x8002)
    return (HANDLE)icon_module_image;
  return i ? (HANDLE)fixture_resources[i - 1].data : 0;
}
void *W32API LockResource(HANDLE h) { return h; }
DWORD W32API SizeofResource(HANDLE module, HANDLE r) {
  (void)module;
  for (unsigned i = 0; i < 2; i++)
    if (r == (HANDLE)&pe_resources[i])
      return pe_resources[i].size;
  uintptr_t i = (uintptr_t)r;
  if (i == 0x8001)
    return sizeof(icon_group);
  if (i == 0x8002)
    return icon_module_bytes;
  return i ? fixture_resources[i - 1].size : 0;
}
static const uint8_t *resource(unsigned type, unsigned id, DWORD *size) {
  HANDLE r = FindResourceA(FIXTURE, (const void *)(uintptr_t)id,
                           (const void *)(uintptr_t)type);
  CHECK(r);
  *size = SizeofResource(FIXTURE, r);
  return LoadResource(FIXTURE, r);
}

/* ---- frames ---------------------------------------------------------------- */
struct gdi_dib {
  uint8_t *bits;
  DWORD width, height, pitch, size, bpp, top_down, masks[3], colors[256],
      color_count;
};
typedef DWORD(W32API *surface_dc_t)(const struct gdi_dib *, POINT,
                                    void (*)(void *, const RECT *), void *);
static void no_damage(void *o, const RECT *r) { (void)o, (void)r; }
static void put(uint32_t *screen, UINT w, UINT h, int x, int y, uint32_t c) {
  if (x >= 0 && y >= 0 && (UINT)x < w && (UINT)y < h)
    screen[(UINT)y * w + (UINT)x] = c;
}
static void box(uint32_t *s, UINT w, UINT h, RECT r, uint32_t c) {
  for (int y = r.top; y < r.bottom; y++)
    for (int x = r.left; x < r.right; x++)
      put(s, w, h, x, y, c);
}
static uint32_t rgb(DWORD colorref) {
  return ((colorref & 0xff) << 16) | (colorref & 0xff00) |
         ((colorref >> 16) & 0xff);
}
/* Emulates the desktop's frame and caption so reviewers see placement; the
 * personality never draws these pixels itself. */
static struct u32_window *frame_active;
static void settle(void);
static void desktop_frame(uint32_t *s, UINT sw, UINT sh, struct u32_window *w,
                          POINT o, UINT width, UINT height) {
  int bx, by;
  u32_frame(w->style, w->exstyle, &bx, &by);
  if (!bx && !by)
    return;
  RECT r = {o.x - bx, o.y - by, o.x + (LONG)width + bx, o.y + (LONG)height + bx};
  box(s, sw, sh, r, rgb(GetSysColor(COLOR_BTNFACE)));
  box(s, sw, sh, (RECT){r.left, r.top, r.right, r.top + 1}, rgb(GetSysColor(22)));
  box(s, sw, sh, (RECT){r.left, r.top, r.left + 1, r.bottom}, rgb(GetSysColor(22)));
  box(s, sw, sh, (RECT){r.left + 1, r.top + 1, r.right - 1, r.top + 2}, 0xffffff);
  box(s, sw, sh, (RECT){r.left + 1, r.top + 1, r.left + 2, r.bottom - 1}, 0xffffff);
  box(s, sw, sh, (RECT){r.left, r.bottom - 1, r.right, r.bottom}, 0);
  box(s, sw, sh, (RECT){r.right - 1, r.top, r.right, r.bottom}, 0);
  box(s, sw, sh, (RECT){r.left + 1, r.bottom - 2, r.right - 1, r.bottom - 1},
      0x808080);
  box(s, sw, sh, (RECT){r.right - 2, r.top + 1, r.right - 1, r.bottom - 1},
      0x808080);
  RECT caption = {r.left + 3, r.top + 3, r.right - 3, o.y - 1};
  BOOL active = w == frame_active;
  for (int x = caption.left; x < caption.right; x++) {
    int span = caption.right - caption.left, k = x - caption.left;
    uint32_t a = active ? 0x000080 : 0x808080, b = active ? 0x1084d0 : 0xc0c0c0;
    uint32_t c = 0;
    for (int ch = 0; ch < 3; ch++) {
      int ca = (a >> (ch * 8)) & 255, cb = (b >> (ch * 8)) & 255;
      c |= (uint32_t)(ca + (cb - ca) * k / (span > 1 ? span - 1 : 1)) << (ch * 8);
    }
    box(s, sw, sh, (RECT){x, caption.top, x + 1, caption.bottom}, c);
  }
  struct gdi_dib dib = {(uint8_t *)s, sw, sh, sw * 4, sw * sh * 4, 32, 1,
                        {0, 0, 0}, {0}, 0};
  surface_dc_t surface_dc = GDI(surface_dc_t, w32_gdi_surface_dc);
  const struct u32_graphics *g = u32_graphics();
  DWORD dc = surface_dc ? surface_dc(&dib, (POINT){0, 0}, no_damage, 0) : 0;
  if (dc && g) {
    struct u32_logfont lf = {0};
    lf.height = -11;
    lf.weight = 700;
    strcpy(lf.face, "MS Sans Serif");
    DWORD font = g->font_indirect(&lf);
    DWORD old = g->select(dc, font);
    g->bk_mode(dc, 1);
    g->text_color(dc, active ? 0xffffff : 0xc0c0c0);
    RECT t = caption;
    t.left += 4;
    g->draw_text(dc, w->title, -1, &t, 0x24u | 0x8000u, 0, 0);
    g->select(dc, old);
    g->delete_object(font);
    g->delete_dc(dc);
  }
}
static UINT frame_count;
static void frame(const char *name) {
  settle();
  MSG pending;
  for (UINT guard = 0; guard < 200 && PeekMessageA(&pending, 0, WM_PAINT,
                                                   WM_PAINT, 1);
       guard++)
    DispatchMessageA(&pending);
  UINT sw = u32_screen_w, sh = u32_screen_h;
  uint32_t *screen = calloc((size_t)sw * sh, 4);
  CHECK(screen);
  for (UINT i = 0; i < sw * sh; i++)
    screen[i] = rgb(GetSysColor(1));
  struct u32_window *order[U32_WINDOWS];
  UINT n = 0;
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    if (w->handle && !(w->style & WS_CHILD) && (w->style & WS_VISIBLE) &&
        w->surface.pixels)
      order[n++] = w;
  }
  for (UINT i = 1; i < n; i++)
    for (UINT j = i; j > 0; j--) {
      struct u32_window *a = order[j - 1], *b = order[j];
      int ta = !!(a->exstyle & WS_EX_TOPMOST), tb = !!(b->exstyle & WS_EX_TOPMOST);
      if (ta < tb || (ta == tb && a->z <= b->z))
        break;
      order[j - 1] = b;
      order[j] = a;
    }
  /* The desktop activates the top-most captioned window. */
  frame_active = 0;
  for (UINT i = 0; i < n; i++)
    if ((order[i]->style & WS_CAPTION) == WS_CAPTION)
      frame_active = order[i];
  for (UINT i = 0; i < n; i++) {
    struct u32_window *w = order[i];
    POINT o = u32_surface_origin(w);
    POINT client = u32_origin(w);
    int top = client.y - o.y;
    desktop_frame(screen, sw, sh, w, (POINT){o.x, o.y}, w->surface.width,
                  w->surface.height - 0);
    (void)top;
    for (UINT y = 0; y < w->surface.height; y++)
      for (UINT x = 0; x < w->surface.width; x++)
        put(screen, sw, sh, o.x + (int)x, o.y + (int)y,
            ((uint32_t *)((uint8_t *)w->surface.pixels +
                          y * w->surface.pitch))[x] &
                0xffffff);
  }
  char path[1024];
  snprintf(path, sizeof(path), "%s/frames/%s.bin", out_dir, name);
  FILE *f = fopen(path, "wb");
  CHECK(f);
  CHECK(fwrite(screen, 4, (size_t)sw * sh, f) == (size_t)sw * sh);
  fclose(f);
  free(screen);
  printf("FRAME %s %u %u\n", name, sw, sh);
  frame_count++;
}

/* ---- message helpers ---------------------------------------------------- */
/* The desktop answers each placement with CONFIGURE; the host transport
 * does not, so granted sizes are applied here before draining. */
static void settle(void) {
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    if (w->handle && w->wire && !(w->style & WS_CHILD)) {
      if (!w->serial)
        w->serial = 1;
      u32_resize(w);
    }
  }
}
static void drain(HWND dialog) {
  settle();
  MSG msg;
  for (UINT guard = 0; guard < 10000 && PeekMessageA(&msg, 0, 0, 0, 1);
       guard++)
    if (!dialog || !IsWindow(dialog) || !IsDialogMessageA(dialog, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
}
static HWND target(void) {
  HWND f = GetFocus();
  return f ? f : GetActiveWindow();
}
static void post_key(HWND h, UINT vk, BOOL down, BOOL sys) {
  PostMessageA(h, sys ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP)
                      : (down ? WM_KEYDOWN : WM_KEYUP),
               vk, (LPARAM)(down ? 1u : 0xc0000001u));
}
static void key(UINT vk) {
  HWND h = target();
  post_key(h, vk, 1, 0);
  post_key(h, vk, 0, 0);
}
static void key_with(UINT modifier, UINT vk) {
  HWND h = target();
  post_key(h, modifier, 1, 0);
  post_key(h, vk, 1, modifier == VK_MENU);
  post_key(h, vk, 0, modifier == VK_MENU);
  post_key(h, modifier, 0, 0);
}
static void chr(UINT c) { PostMessageA(target(), WM_CHAR, c, 1); }
static void syschr(UINT c) { PostMessageA(target(), WM_SYSCHAR, c, 1); }

/* The owner window drives modal loops from WM_ENTERIDLE. */
static void (*idle_step)(HWND, UINT, UINT);
static UINT idle_count, idle_calls;
static UINT owner_messages[2048], owner_message_count;
static WPARAM owner_last_command;
static UINT owner_fontchange, owner_contextmenu;
static LRESULT W32API owner_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (owner_message_count < 2048)
    owner_messages[owner_message_count++] = m;
  if (m == WM_ENTERIDLE) {
    idle_calls++;
    CHECK(idle_calls < 400);
    if (idle_step)
      idle_step((HWND)lp, (UINT)wp, idle_count++);
    return 0;
  }
  if (m == WM_COMMAND)
    owner_last_command = wp;
  if (m == WM_SYSCOMMAND && ((wp & 0xfff0) == SC_CLOSE)) {
    owner_last_command = wp;
    return 0;
  }
  if (m == WM_FONTCHANGE)
    owner_fontchange++;
  if (m == WM_CONTEXTMENU)
    owner_contextmenu++;
  return DefWindowProcA(h, m, wp, lp);
}
static BOOL seen(UINT m) {
  for (UINT i = 0; i < owner_message_count; i++)
    if (owner_messages[i] == m)
      return 1;
  return 0;
}
static void script(void (*step)(HWND, UINT, UINT)) {
  idle_step = step;
  idle_count = 0;
  idle_calls = 0;
}
static HWND owner_window(const char *title, DWORD style, int w, int h) {
  static BOOL registered;
  if (!registered) {
    WNDCLASSA c = {.style = 8,
                   .lpfnWndProc = owner_proc,
                   .hbrBackground = (HBRUSH)(uintptr_t)(COLOR_WINDOW + 1),
                   .lpszClassName = "F5 Owner"};
    CHECK(RegisterClassA(&c));
    registered = 1;
  }
  HWND owner = CreateWindowExA(0, "F5 Owner", title, style | WS_VISIBLE, 16, 48,
                               w, h, 0, 0, FIXTURE, 0);
  CHECK(owner);
  owner_message_count = 0;
  drain(0);
  return owner;
}
static void menus_frames(HWND, const char *);
#include "fonts.inc"
#include "dialogs.inc"
#include "menus.inc"
#include "upper.inc"
#include "bridge.inc"

static const char *mode;
static void harness_run(void) {
  ui_kernel_init();
  struct wr_io io;
  char dir[600];
  snprintf(dir, sizeof(dir), "%s/state-XXXXXX", out_dir);
  CHECK(mkdtemp(dir));
  wr_posix_io(&io, dir);
  CHECK(wr_init(&server, &io, 0));
  struct u32_backend backend = {ui_kernel_call, w32r_gate,     w32r_exchange,
                                ui_kernel_tid,  ui_kernel_pid, ui_kernel_ticks,
                                surface,        drop,          ui_kernel_pause};
  u32_set_backend(&backend);
  u32_screen_w = 640;
  u32_screen_h = 480;
  CHECK(u32_graphics());
  CHECK(gdi_host_fonts(fonts_dir, install_dir));
  SetCaretBlinkTime(0xffffffffu);
  if (!strcmp(mode, "fonts"))
    test_fonts();
  else if (!strcmp(mode, "parser"))
    test_parser();
  else if (!strcmp(mode, "templates"))
    test_templates();
  else if (!strcmp(mode, "keyboard"))
    test_keyboard();
  else if (!strcmp(mode, "edit"))
    test_edit();
  else if (!strcmp(mode, "lists"))
    test_lists();
  else if (!strcmp(mode, "menus"))
    test_menus();
  else if (!strcmp(mode, "msgbox"))
    test_msgbox();
  else if (!strcmp(mode, "installer"))
    test_installer();
  else if (!strcmp(mode, "frames"))
    test_frames();
  else if (!strcmp(mode, "upper-user"))
    test_upper_user();
  else if (!strcmp(mode, "upper-gdi"))
    test_upper_gdi();
  else if (!strcmp(mode, "bridge"))
    test_bridge();
  else if (!strcmp(mode, "frames-bridge"))
    frames_bridge();
  else if (!strcmp(mode, "exercise"))
    test_exercise();
  else if (!strcmp(mode, "exercise-pe"))
    test_exercise_pe();
  else if (!strcmp(mode, "exercise-missing-resource"))
    test_exercise_missing_resource();
  else if (!strcmp(mode, "exercise-entry"))
    test_exercise_entry();
  else
    CHECK(!"unknown mode");
  wr_destroy(&server);
  for (unsigned i = 0; i < 2; i++) {
    char path[700];
    snprintf(path, sizeof(path), "%s/SERVICE.CRS.%u", dir, i);
    unlink(path);
  }
  rmdir(dir);
  ui_kernel_finish();
  printf("PASS %s\n", mode);
}
int main(int argc, char **argv) {
  setbuf(stdout, 0);
  setbuf(stderr, 0);
  CHECK(argc == 2);
  mode = argv[1];
  const char *fonts = getenv("CIUKI_FONTS"), *out = getenv("CIUKI_OUT");
  CHECK(fonts && out);
  snprintf(fonts_dir, sizeof(fonts_dir), "%s", fonts);
  snprintf(out_dir, sizeof(out_dir), "%s", out);
  snprintf(install_dir, sizeof(install_dir), "%s/fonts-%s", out, mode);
  snprintf(drive_dir, sizeof(drive_dir), "%s/drive-%s", out, mode);
  char command[2200];
  snprintf(command, sizeof(command), "rm -rf '%s' '%s'", install_dir, drive_dir);
  CHECK(!system(command));
  CHECK(!mkdir(install_dir, 0755) && !mkdir(drive_dir, 0755));
  void *stack = mmap(0, 8 * 1024 * 1024, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  CHECK(stack != MAP_FAILED);
  ucontext_t current_context, low;
  getcontext(&low);
  low.uc_stack.ss_sp = stack;
  low.uc_stack.ss_size = 8 * 1024 * 1024;
  low.uc_link = &current_context;
  makecontext(&low, harness_run, 0);
  swapcontext(&current_context, &low);
  munmap(stack, 8 * 1024 * 1024);
  return 0;
}
