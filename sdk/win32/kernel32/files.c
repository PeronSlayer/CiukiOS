/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/kernel32.h>
extern int k32_from_utf8(const char *, char *, DWORD);
int k32_file(struct ciuki_w32_file_args *a) {
  a->header = K32_HEADER(W32_FILE, *a);
  if (!k32_status(w32_ciuki_call(a, sizeof(*a))))
    return 0;
  if (a->error && !(a->error == 183 && a->action == W32_F_OPEN)) {
    SetLastError(a->error);
    return 0;
  }
  return 1;
}
HANDLE W32API CreateFileA(const char *name, DWORD access, DWORD share,
                          SECURITY_ATTRIBUTES *sa, DWORD disposition,
                          DWORD flags, HANDLE template) {
  (void)template;
  char path[778];
  if (!k32_path(name, path))
    return K32_INVALID;
  struct ciuki_w32_file_args a = {.action = W32_F_OPEN,
                                  .path = K32_PTR(path),
                                  .access = access,
                                  .share = share,
                                  .disposition = disposition,
                                  .flags = (flags & ~0x10000u) |
                                           (sa && sa->inherit ? 0x10000u : 0)};
  if (!k32_file(&a))
    return K32_INVALID;
  if (disposition == 2 || disposition == 4)
    SetLastError(a.error);
  return (HANDLE)(uintptr_t)a.result;
}
static BOOL io(HANDLE h, void *data, DWORD n, DWORD *done, OVERLAPPED *ov,
               DWORD op) {
  if (done)
    *done = 0;
  if ((!data && n) || (!done && !ov)) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_file_args a = {.action = op,
                                  .handle = K32_PTR(h),
                                  .buffer = K32_PTR(data),
                                  .bytes = n,
                                  .flags = ov ? 0x10000u : 0,
                                  .low = ov ? ov->offset : 0,
                                  .high = ov ? ov->offset_high : 0};
  int ok = k32_file(&a);
  if (done)
    *done = a.result;
  if (ov) {
    ov->internal = ok ? 0 : 0xc0070000u | (GetLastError() & 65535);
    ov->internal_high = a.result;
    if (ov->event) {
      struct ciuki_w32_object_args e = {.header = {1, sizeof(e), W32_OBJECT, 0},
                                        .action = W32_OBJ_SET,
                                        .handle = K32_PTR(ov->event)};
      w32_ciuki_call(&e, sizeof(e));
    }
  }
  return ok;
}
BOOL W32API ReadFile(HANDLE h, void *p, DWORD n, DWORD *done, OVERLAPPED *ov) {
  return io(h, p, n, done, ov, W32_F_READ);
}
BOOL W32API WriteFile(HANDLE h, const void *p, DWORD n, DWORD *done,
                      OVERLAPPED *ov) {
  return io(h, (void *)p, n, done, ov, W32_F_WRITE);
}
BOOL W32API GetOverlappedResult(HANDLE h, OVERLAPPED *ov, DWORD *done,
                                BOOL wait) {
  (void)h;
  (void)wait;
  if (!ov || !done) {
    SetLastError(87);
    return 0;
  }
  if (ov->internal) {
    SetLastError(ov->internal == 259 ? 996
                 : (ov->internal & 0xffff0000u) == 0xc0070000u
                     ? (DWORD)ov->internal & 65535
                     : 31);
    return 0;
  }
  *done = (DWORD)ov->internal_high;
  return 1;
}
DWORD W32API SetFilePointer(HANDLE h, LONG low, LONG *high, DWORD method) {
  struct ciuki_w32_file_args a = {.action = W32_F_SEEK,
                                  .handle = K32_PTR(h),
                                  .low = (DWORD)low,
                                  .high = high      ? (DWORD)*high
                                          : low < 0 ? ~0u
                                                    : 0,
                                  .flags = method};
  if (!k32_file(&a))
    return ~0u;
  if (high)
    *high = (LONG)a.high;
  if (a.low == ~0u)
    SetLastError(0);
  return a.low;
}
static BOOL handle_call(HANDLE h, DWORD op) {
  struct ciuki_w32_file_args a = {.action = op, .handle = K32_PTR(h)};
  return k32_file(&a);
}
BOOL W32API SetEndOfFile(HANDLE h) { return handle_call(h, W32_F_TRUNCATE); }
BOOL W32API FlushFileBuffers(HANDLE h) { return handle_call(h, W32_F_FLUSH); }
static BOOL range_lock(HANDLE h, DWORD flags, DWORD low, DWORD high,
                       DWORD bytes, DWORD upper, DWORD action) {
  struct ciuki_w32_file_args a = {.action=action,.handle=K32_PTR(h),
      .flags=flags,.low=low,.high=high,.bytes=bytes,.share=upper};
  return k32_file(&a);
}
BOOL W32API LockFile(HANDLE h, DWORD low, DWORD high, DWORD n, DWORD upper) {
  return range_lock(h,3,low,high,n,upper,W32_F_LOCK);
}
BOOL W32API UnlockFile(HANDLE h, DWORD low, DWORD high, DWORD n, DWORD upper) {
  return range_lock(h,0,low,high,n,upper,W32_F_UNLOCK);
}
static BOOL range_ex(HANDLE h, DWORD flags, DWORD reserved, DWORD n,
                     DWORD upper, OVERLAPPED *ov, DWORD action) {
  if (reserved || !ov || (flags & ~3u)) { SetLastError(87); return 0; }
  BOOL ok=range_lock(h,flags,ov->offset,ov->offset_high,n,upper,action);
  /* Contract: file OVERLAPPED requests complete synchronously. */
  if (ok) {
    ov->internal=0; ov->internal_high=0;
    if (ov->event) {
      struct ciuki_w32_object_args a={.header={1,sizeof(a),W32_OBJECT,0},
          .action=W32_OBJ_SET,.handle=K32_PTR(ov->event)};
      w32_ciuki_call(&a,sizeof(a));
    }
  }
  return ok;
}
BOOL W32API LockFileEx(HANDLE h, DWORD flags, DWORD reserved, DWORD n,
                       DWORD upper, OVERLAPPED *ov) {
  return range_ex(h,flags,reserved,n,upper,ov,W32_F_LOCK);
}
BOOL W32API UnlockFileEx(HANDLE h, DWORD reserved, DWORD n, DWORD upper,
                         OVERLAPPED *ov) {
  return range_ex(h,0,reserved,n,upper,ov,W32_F_UNLOCK);
}
static int stat_file(HANDLE h, const char *name,
                     struct ciuki_w32_file_info *f) {
  char path[778];
  if (name && !k32_path(name, path))
    return 0;
  struct ciuki_w32_file_args a = {.action = W32_F_STAT,
                                  .handle = K32_PTR(h),
                                  .path = name ? K32_PTR(path) : 0,
                                  .buffer = K32_PTR(f),
                                  .bytes = sizeof(*f)};
  return k32_file(&a);
}
DWORD W32API GetFileSize(HANDLE h, DWORD *high) {
  struct ciuki_w32_file_info f;
  if (!stat_file(h, 0, &f))
    return ~0u;
  if (high)
    *high = f.size_high;
  if (f.size_low == ~0u)
    SetLastError(0);
  return f.size_low;
}
DWORD W32API GetFileType(HANDLE h) {
  struct ciuki_w32_file_info f;
  return stat_file(h, 0, &f) ? 1 : 0;
}
DWORD W32API GetFileAttributesA(const char *name) {
  struct ciuki_w32_file_info f;
  return stat_file(0, name, &f) ? f.attributes : ~0u;
}
static BOOL path_call(const char *name, const char *to, DWORD op, DWORD flags) {
  char p[778], q[778];
  if (!k32_path(name, p) || (to && !k32_path(to, q)))
    return 0;
  struct ciuki_w32_file_args a = {.action = op,
                                  .path = K32_PTR(p),
                                  .path2 = to ? K32_PTR(q) : 0,
                                  .flags = flags};
  return k32_file(&a);
}
BOOL W32API SetFileAttributesA(const char *p, DWORD flags) {
  return path_call(p, 0, W32_F_ATTR, flags);
}
BOOL W32API DeleteFileA(const char *p) {
  return path_call(p, 0, W32_F_DELETE, 0);
}
BOOL W32API CreateDirectoryA(const char *p, SECURITY_ATTRIBUTES *sa) {
  (void)sa;
  return path_call(p, 0, W32_F_MKDIR, 0);
}
BOOL W32API RemoveDirectoryA(const char *p) {
  return path_call(p, 0, W32_F_RMDIR, 0);
}
BOOL W32API MoveFileExA(const char *from, const char *to, DWORD flags) {
  if (flags & 4) {
    extern BOOL k32_delay_move(const char *,const char *,DWORD);
    return k32_delay_move(from,to,flags);
  }
  if (flags) {
    SetLastError(120);
    return 0;
  }
  if (GetFileAttributesA(to) != ~0u) {
    SetLastError(183);
    return 0;
  }
  return path_call(from, to, W32_F_MOVE, 0);
}
BOOL W32API MoveFileA(const char *from, const char *to) {
  return MoveFileExA(from, to, 0);
}
BOOL W32API CopyFileA(const char *from, const char *to, BOOL fail) {
  HANDLE in = CreateFileA(from, 0x80000000u, 1, 0, 3, 0, 0);
  if (in == K32_INVALID)
    return 0;
  HANDLE out = CreateFileA(to, 0x40000000u, 0, 0, fail ? 1 : 2, 0, 0);
  if (out == K32_INVALID) {
    DWORD e = GetLastError();
    CloseHandle(in);
    SetLastError(e);
    return 0;
  }
  struct ciuki_w32_file_info metadata;
  BOOL ok = stat_file(in, 0, &metadata);
  DWORD err = ok ? 0 : GetLastError();
  char *buffer = ok ? k32_alloc(65536) : 0;
  if (ok && !buffer) {
    ok = 0;
    err = 8;
  }
  while (ok) {
    DWORD n, w;
    if (!ReadFile(in, buffer, 65536, &n, 0)) {
      err = GetLastError();
      ok = 0;
      break;
    }
    if (!n)
      break;
    if (!WriteFile(out, buffer, n, &w, 0) || w != n) {
      err = GetLastError();
      if (!err)
        err = 112;
      ok = 0;
      break;
    }
  }
  k32_free(buffer, 65536);
  if (ok) {
    struct ciuki_w32_file_args times = {.action = W32_F_TIMES,
                                        .handle = K32_PTR(out),
                                        .buffer = K32_PTR(&metadata),
                                        .bytes = sizeof(metadata),
                                        .flags = 7};
    ok = FlushFileBuffers(out) && k32_file(&times);
    if (!ok)
      err = GetLastError();
  }
  CloseHandle(in);
  CloseHandle(out);
  if (ok && !SetFileAttributesA(to, metadata.attributes)) {
    ok = 0;
    err = GetLastError();
  }
  if (!ok) {
    DeleteFileA(to);
    SetLastError(err);
  }
  return ok;
}
BOOL W32API SetCurrentDirectoryA(const char *p) {
  return path_call(p, 0, W32_F_CHDIR, 0);
}
static DWORD fullpath(const char *p, char *out, DWORD cap, DWORD action,
                      DWORD flags) {
  char path[778], utf[778], ansi[260];
  if (p && !k32_path(p, path))
    return 0;
  struct ciuki_w32_file_args a = {.action = action,
                                  .flags = flags,
                                  .path = p ? K32_PTR(path) : 0,
                                  .buffer = K32_PTR(utf),
                                  .bytes = sizeof(utf)};
  if (!k32_file(&a))
    return 0;
  if (!k32_from_utf8(utf, ansi, sizeof(ansi))) {
    SetLastError(1113);
    return 0;
  }
  DWORD n = w32_strlen(ansi);
  if (cap <= n)
    return n + 1;
  if (!out) {
    SetLastError(87);
    return 0;
  }
  w32_copy(out, ansi, n + 1);
  return n;
}
DWORD W32API GetCurrentDirectoryA(DWORD n, char *p) {
  return fullpath(0, p, n, W32_F_CWD, 0);
}
DWORD W32API GetFullPathNameA(const char *p, DWORD n, char *out, char **file) {
  DWORD r = fullpath(p, out, n, W32_F_FULLPATH, 0);
  if (r && r < n && file) {
    *file = out;
    for (DWORD i = 0; i < r; i++)
      if (out[i] == '\\')
        *file = out + i + 1;
  }
  return r;
}
static DWORD directory(const char *s, DWORD n, char *out) {
  DWORD length = w32_strlen(s);
  if (n <= length)
    return length + 1;
  if (!out) {
    SetLastError(87);
    return 0;
  }
  w32_copy(out, s, length + 1);
  return length;
}
DWORD W32API GetWindowsDirectoryA(char *p, DWORD n) {
  return directory("C:\\WINDOWS", n, p);
}
DWORD W32API GetSystemDirectoryA(char *p, DWORD n) {
  return directory("C:\\WINDOWS\\SYSTEM", n, p);
}
DWORD W32API GetTempPathA(DWORD n, char *out) {
  char value[260];
  const char *vars[] = {"TMP", "TEMP", "USERPROFILE"};
  for (unsigned i = 0; i < 3; i++) {
    DWORD r = GetEnvironmentVariableA(vars[i], value, sizeof(value));
    if (r && r < 259) {
      if (value[r - 1] != '\\' && value[r - 1] != '/') {
        value[r++] = '\\';
        value[r] = 0;
      }
      return directory(value, n, out);
    }
  }
  return directory("C:\\WINDOWS\\TEMP\\", n, out);
}
DWORD W32API GetTempFileNameA(const char *path, const char *prefix,
                              DWORD unique, char *out) {
  if (!path || !prefix || !out) {
    SetLastError(87);
    return 0;
  }
  DWORD len = w32_strlen(path);
  if (len > 246) {
    SetLastError(206);
    return 0;
  }
  char candidate[260];
  w32_copy(candidate, path, len);
  if (len && candidate[len - 1] != '\\' && candidate[len - 1] != '/')
    candidate[len++] = '\\';
  for (unsigned i = 0; i < 3 && prefix[i]; i++)
    candidate[len++] = prefix[i];
  DWORD start = unique ? unique & 65535 : (GetTickCount() & 65535);
  if (!start)
    start = 1;
  for (DWORD tries = 0; tries < 65535; tries++) {
    DWORD number = ((start + tries - 1) % 65535) + 1;
    for (unsigned i = 0; i < 4; i++)
      candidate[len + i] = "0123456789ABCDEF"[(number >> (12 - i * 4)) & 15];
    w32_copy(candidate + len + 4, ".TMP", 5);
    if (unique) {
      w32_copy(out, candidate, len + 9);
      return number;
    }
    HANDLE h = CreateFileA(candidate, 0x40000000u, 0, 0, 1, 128, 0);
    if (h != K32_INVALID) {
      CloseHandle(h);
      w32_copy(out, candidate, len + 9);
      return number;
    }
    if (GetLastError() != 80)
      return 0;
  }
  SetLastError(80);
  return 0;
}
extern BOOL W32API DosDateTimeToFileTime(uint16_t, uint16_t, FILETIME *);
extern BOOL W32API FileTimeToDosDateTime(const FILETIME *, uint16_t *,
                                         uint16_t *);
static FILETIME fattime(uint16_t d, uint16_t t) {
  FILETIME f = {0, 0};
  if (d)
    DosDateTimeToFileTime(d, t, &f);
  return f;
}
BOOL W32API GetFileTime(HANDLE h, FILETIME *create, FILETIME *access,
                        FILETIME *write) {
  struct ciuki_w32_file_info f;
  if (!stat_file(h, 0, &f))
    return 0;
  FILETIME c = fattime(f.create_date, f.create_time),
           a = fattime(f.access_date, 0),
           w = fattime(f.write_date, f.write_time);
  uint64_t n = ((uint64_t)c.high << 32) | c.low;
  n += (uint64_t)f.create_tenths * 100000;
  c = (FILETIME){(DWORD)n, (DWORD)(n >> 32)};
  if (create)
    *create = c;
  if (access)
    *access = a;
  if (write)
    *write = w;
  return 1;
}
BOOL W32API SetFileTime(HANDLE h, const FILETIME *c, const FILETIME *a,
                        const FILETIME *w) {
  struct ciuki_w32_file_info f = {0};
  DWORD mask = 0;
  uint16_t unused;
  if (c) {
    if (!FileTimeToDosDateTime(c, &f.create_date, &f.create_time))
      return 0;
    SYSTEMTIME s;
    if (!FileTimeToSystemTime(c, &s))
      return 0;
    f.create_tenths = (uint8_t)((s.second % 2) * 100 + s.milliseconds / 10);
    mask |= 1;
  }
  if (a) {
    if (!FileTimeToDosDateTime(a, &f.access_date, &unused))
      return 0;
    mask |= 2;
  }
  if (w) {
    if (!FileTimeToDosDateTime(w, &f.write_date, &f.write_time))
      return 0;
    mask |= 4;
  }
  struct ciuki_w32_file_args args = {.action = W32_F_TIMES,
                                     .handle = K32_PTR(h),
                                     .buffer = K32_PTR(&f),
                                     .bytes = sizeof(f),
                                     .flags = mask};
  return k32_file(&args);
}
static int find_data(const struct ciuki_w32_file_info *f,
                     WIN32_FIND_DATAA *out) {
  WIN32_FIND_DATAA d = {.attributes = f->attributes,
                        .size_high = f->size_high,
                        .size_low = f->size_low};
  d.creation = fattime(f->create_date, f->create_time);
  d.access = fattime(f->access_date, 0);
  d.write = fattime(f->write_date, f->write_time);
  if (!k32_from_utf8(f->name, d.name, sizeof(d.name)) ||
      !k32_from_utf8(f->alias, d.alternate, sizeof(d.alternate))) {
    SetLastError(1113);
    return 0;
  }
  *out = d;
  return 1;
}
HANDLE W32API FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *out) {
  char path[778];
  if (!out) {
    SetLastError(87);
    return K32_INVALID;
  }
  if (!k32_path(pattern, path))
    return K32_INVALID;
  struct ciuki_w32_file_info info;
  struct ciuki_w32_file_args a = {.action = W32_F_FIRST,
                                  .path = K32_PTR(path),
                                  .buffer = K32_PTR(&info),
                                  .bytes = sizeof(info)};
  if (!k32_file(&a))
    return K32_INVALID;
  if (!find_data(&info, out)) {
    CloseHandle((HANDLE)(uintptr_t)a.result);
    return K32_INVALID;
  }
  return (HANDLE)(uintptr_t)a.result;
}
BOOL W32API FindNextFileA(HANDLE h, WIN32_FIND_DATAA *out) {
  if (!out) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_file_info info;
  struct ciuki_w32_file_args a = {.action = W32_F_NEXT,
                                  .handle = K32_PTR(h),
                                  .buffer = K32_PTR(&info),
                                  .bytes = sizeof(info)};
  return k32_file(&a) && find_data(&info, out);
}
BOOL W32API FindClose(HANDLE h) { return CloseHandle(h); }
static BOOL disk_info(const char *root, struct ciuki_w32_disk_info *info) {
  char path[778];
  if (root && !k32_path(root, path))
    return 0;
  struct ciuki_w32_file_args a = {.action = W32_F_DISK,
                                  .path = root ? K32_PTR(path) : 0,
                                  .buffer = K32_PTR(info),
                                  .bytes = sizeof(*info)};
  return k32_file(&a);
}
DWORD W32API GetLogicalDrives(void) {
  struct ciuki_w32_disk_info i;
  return disk_info(0, &i) ? i.drives : 0;
}
DWORD W32API GetDriveTypeA(const char *p) {
  struct ciuki_w32_disk_info i;
  return disk_info(p, &i) ? i.type : 1;
}
DWORD W32API GetLogicalDriveStringsA(DWORD cap, char *out) {
  DWORD bits = GetLogicalDrives(), n = 0;
  for (unsigned d = 0; d < 26; d++)
    if (bits & (1u << d))
      n += 4;
  if (cap <= n)
    return n + 1;
  if (!out) {
    SetLastError(87);
    return 0;
  }
  DWORD at = 0;
  for (unsigned d = 0; d < 26; d++)
    if (bits & (1u << d)) {
      out[at++] = 'A' + d;
      out[at++] = ':';
      out[at++] = '\\';
      out[at++] = 0;
    }
  out[at] = 0;
  return n;
}
BOOL W32API GetDiskFreeSpaceA(const char *p, DWORD *spc, DWORD *bps,
                              DWORD *free_clusters, DWORD *total_clusters) {
  if (!spc || !bps || !free_clusters || !total_clusters) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_disk_info i;
  if (!disk_info(p, &i))
    return 0;
  uint64_t f = ((uint64_t)i.free_high << 32) | i.free_low,
           t = ((uint64_t)i.total_high << 32) | i.total_low;
  if (f > 0x7fffffff)
    f = 0x7fffffff;
  if (t > 0x7fffffff)
    t = 0x7fffffff;
  *spc = i.cluster_bytes / i.sector_bytes;
  *bps = i.sector_bytes;
  *free_clusters = (DWORD)(f / i.cluster_bytes);
  *total_clusters = (DWORD)(t / i.cluster_bytes);
  return 1;
}
BOOL W32API GetDiskFreeSpaceExA(const char *p, uint64_t *available,
                                uint64_t *total, uint64_t *free) {
  struct ciuki_w32_disk_info i;
  if (!disk_info(p, &i))
    return 0;
  uint64_t f = ((uint64_t)i.free_high << 32) | i.free_low,
           t = ((uint64_t)i.total_high << 32) | i.total_low;
  if (available)
    *available = f;
  if (total)
    *total = t;
  if (free)
    *free = f;
  return 1;
}
BOOL W32API GetVolumeInformationA(const char *p, char *label, DWORD label_size,
                                  DWORD *serial, DWORD *component, DWORD *flags,
                                  char *fs, DWORD fs_size) {
  struct ciuki_w32_disk_info i;
  if (!disk_info(p, &i))
    return 0;
  if ((label && w32_strlen(i.label) >= label_size) ||
      (fs && w32_strlen(i.filesystem) >= fs_size)) {
    SetLastError(122);
    return 0;
  }
  if (label)
    w32_copy(label, i.label, w32_strlen(i.label) + 1);
  if (fs)
    w32_copy(fs, i.filesystem, w32_strlen(i.filesystem) + 1);
  if (serial)
    *serial = i.serial;
  if (component)
    *component = i.max_component;
  if (flags)
    *flags = 2u | 4u | (i.type == 5 ? 0x80000u : 0);
  return 1;
}

DWORD W32API GetShortPathNameA(const char *path, char *out, DWORD n) {
  return fullpath(path, out, n, W32_F_FULLPATH, 1);
}
DWORD W32API GetLongPathNameA(const char *path, char *out, DWORD n) {
  return fullpath(path, out, n, W32_F_FULLPATH, 2);
}
/* f5-20A: handle and attribute queries the reused Wine DLLs import. */
static void info_times(const struct ciuki_w32_file_info *f, FILETIME *c,
                       FILETIME *a, FILETIME *w) {
  *c = fattime(f->create_date, f->create_time);
  uint64_t n = (((uint64_t)c->high << 32) | c->low) +
               (f->create_date ? (uint64_t)f->create_tenths * 100000 : 0);
  *c = (FILETIME){(DWORD)n, (DWORD)(n >> 32)};
  *a = fattime(f->access_date, 0);
  *w = fattime(f->write_date, f->write_time);
}
/* GetFileExInfoStandard (0) is the only information level. */
BOOL W32API GetFileAttributesExA(const char *name, DWORD level, void *out) {
  if (level || !out) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_file_info f;
  if (!stat_file(0, name, &f))
    return 0;
  WIN32_FILE_ATTRIBUTE_DATA d = {.attributes = f.attributes,
                                 .size_high = f.size_high,
                                 .size_low = f.size_low};
  info_times(&f, &d.creation, &d.access, &d.write);
  w32_copy(out, &d, sizeof(d));
  return 1;
}
/* FAT descriptions supply the retained VFS identity; other providers use zero. */
BOOL W32API GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION *out) {
  if (!out) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_file_info f;
  if (!stat_file(h, 0, &f))
    return 0;
  BY_HANDLE_FILE_INFORMATION d = {.attributes = f.attributes,
                                  .serial = f.volume_serial,
                                  .index_high = (DWORD)(f.file_id >> 32),
                                  .index_low = (DWORD)f.file_id,
                                  .size_high = f.size_high,
                                  .size_low = f.size_low,
                                  .links = 1};
  info_times(&f, &d.creation, &d.access, &d.write);
  *out = d;
  return 1;
}
BOOL W32API GetFileSizeEx(HANDLE h, int64_t *out) {
  if (!out) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_file_info f;
  if (!stat_file(h, 0, &f))
    return 0;
  *out = (int64_t)(((uint64_t)f.size_high << 32) | f.size_low);
  return 1;
}
BOOL W32API SetFilePointerEx(HANDLE h, int64_t distance, int64_t *out,
                             DWORD method) {
  if (method > 2) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_file_args a = {.action = W32_F_SEEK,
                                  .handle = K32_PTR(h),
                                  .low = (DWORD)(uint64_t)distance,
                                  .high = (DWORD)((uint64_t)distance >> 32),
                                  .flags = method};
  if (!k32_file(&a))
    return 0;
  if (out)
    *out = (int64_t)(((uint64_t)a.high << 32) | a.low);
  return 1;
}
/* Windows 98 answers device I/O only on VxD handles (\\.\name), which the
 * CiukiOS facade refuses to open; a file handle gets ERROR_NOT_SUPPORTED. */
BOOL W32API DeviceIoControl(HANDLE h, DWORD code, void *in, DWORD in_size,
                            void *out, DWORD out_size, DWORD *done,
                            OVERLAPPED *ov) {
  (void)code;
  (void)in;
  (void)in_size;
  (void)out;
  (void)out_size;
  (void)ov;
  if (done)
    *done = 0;
  struct ciuki_w32_file_info f;
  if (!stat_file(h, 0, &f))
    return 0;
  SetLastError(50);
  return 0;
}
/* Vista API: flags are validated, the handle must be an open file; the VFS
 * keeps no path for an open description, so the query itself answers
 * ERROR_NOT_SUPPORTED (callers keep the name they opened). */
DWORD W32API GetFinalPathNameByHandleW(HANDLE h, WCHAR *out, DWORD cap,
                                       DWORD flags) {
  (void)out;
  (void)cap;
  if (flags & ~0xfu || (flags & 7) > 4 || (flags & 7) == 3) {
    SetLastError(87);
    return 0;
  }
  struct ciuki_w32_file_info f;
  if (!stat_file(h, 0, &f))
    return 0;
  SetLastError(50);
  return 0;
}
