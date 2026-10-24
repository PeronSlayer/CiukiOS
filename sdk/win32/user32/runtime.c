/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
struct u32_window u32_windows[U32_WINDOWS];
struct u32_class u32_classes[U32_CLASSES];
struct u32_timer u32_timers[U32_TIMERS];
static struct u32_thread threads[U32_THREADS];
const struct u32_gdi *u32_gdi;
struct du3_connection u32_desktop;
HWND u32_capture, u32_foreground;
POINT u32_pointer;
UINT u32_screen_w = 800, u32_screen_h = 600, u32_layout;
static DWORD private_bytes;
int u32_errno;
extern DWORD w32r_gate(DWORD, DWORD, DWORD, DWORD);
extern DWORD w32r_exchange(const void *, DWORD, void *, DWORD, DWORD *);
extern DWORD W32API GetCurrentThreadId(void);
extern DWORD W32API GetCurrentProcessId(void);
static void pause_ms(DWORD n) { w32r_gate(CIUKI_SYS_SLEEP_MS, n, 0, 0); }
static BOOL surface(UINT w, UINT h, struct u32_surface *s) {
  int fd = (int)w32r_gate(CIUKI_SYS_SURFACE_CREATE, w, h, 1);
  if (fd < 0)
    return u32_fail(8);
  struct ciuki_surface_info i = {.size = sizeof(i)};
  if ((int)w32r_gate(CIUKI_SYS_SURFACE_INFO, fd, U32_PTR(&i), 0) < 0) {
    w32r_gate(CIUKI_SYS_CLOSE, fd, 0, 0);
    return u32_fail(8);
  }
  DWORD address = w32r_gate(CIUKI_SYS_SURFACE_MAP, fd, 3, 0);
  if ((int32_t)address < 0) {
    w32r_gate(CIUKI_SYS_CLOSE, fd, 0, 0);
    return u32_fail(8);
  }
  *s = (struct u32_surface){
      (void *)(uintptr_t)address, w, h, i.stride, i.format, fd};
  return 1;
}
static void drop_surface(struct u32_surface *s) {
  if (s->pixels) {
    w32r_gate(CIUKI_SYS_MUNMAP, U32_PTR(s->pixels),
              (s->pitch * s->height + 4095) & ~4095u, 0);
    w32r_gate(CIUKI_SYS_CLOSE, s->fd, 0, 0);
  }
  w32_fill(s, 0, sizeof(*s));
  s->fd = -1;
}
static DWORD current_tid(void) { return GetCurrentThreadId(); }
static DWORD current_pid(void) { return GetCurrentProcessId(); }
static DWORD ticks(void) { return GetTickCount(); }
struct u32_backend u32_backend = {w32_ciuki_call, w32r_gate,    w32r_exchange,
                                  current_tid,    current_pid,  ticks,
                                  surface,        drop_surface, pause_ms};
void u32_set_backend(const struct u32_backend *b) {
  if (b)
    u32_backend = *b;
}
void u32_set_gdi(const struct u32_gdi *g) {
  u32_gdi = g && (g->version == 1 || g->version == 2) ? g : 0;
}
static volatile DWORD owner;
static UINT depth;
void u32_lock(void) {
  DWORD tid = u32_tid();
  if (__atomic_load_n(&owner, __ATOMIC_ACQUIRE) == tid) {
    depth++;
    return;
  }
  while (!__sync_bool_compare_and_swap(&owner, 0, tid))
    u32_backend.pause(1);
  depth = 1;
}
void u32_unlock(int *unused) {
  (void)unused;
  if (!--depth)
    __atomic_store_n(&owner, 0, __ATOMIC_RELEASE);
}
UINT u32_suspend(void) {
  if (__atomic_load_n(&owner, __ATOMIC_ACQUIRE) != u32_tid())
    return 0;
  UINT n = depth;
  depth = 0;
  __atomic_store_n(&owner, 0, __ATOMIC_RELEASE);
  return n;
}
void u32_resume(UINT n) {
  if (n) {
    u32_lock();
    depth = n;
  }
}
BOOL u32_fail(DWORD e) {
  SetLastError(e);
  return 0;
}
void *u32_alloc(DWORD n) {
  U32_GUARD;
  if (n > 4 * 1024 * 1024 - private_bytes - 8)
    return (void *)(uintptr_t)u32_fail(8);
  DWORD *p = HeapAlloc(GetProcessHeap(), 8, n + 8);
  if (!p) {
    u32_fail(8);
    return 0;
  }
  p[0] = n;
  private_bytes += n + 8;
  return p + 2;
}
void u32_free(void *v) {
  U32_GUARD;
  if (v) {
    DWORD *p = (DWORD *)v - 2;
    private_bytes -= p[0] + 8;
    HeapFree(GetProcessHeap(), 0, p);
  }
}
DWORD u32_tid(void) { return u32_backend.tid(); }
DWORD u32_pid(void) { return u32_backend.pid(); }
DWORD u32_ticks(void) { return u32_backend.ticks(); }
struct u32_thread *u32_thread_for(DWORD id) {
  for (UINT i = 0; i < U32_THREADS; i++)
    if (threads[i].id == id)
      return &threads[i];
  for (UINT i = 0; i < U32_THREADS; i++)
    if (!threads[i].id) {
      threads[i].id = id;
      return &threads[i];
    }
  u32_fail(8);
  return 0;
}
struct u32_thread *u32_thread(void) { return u32_thread_for(u32_tid()); }
struct u32_window *u32_window(HWND h) {
  for (UINT i = 0; i < U32_WINDOWS; i++)
    if (h && u32_windows[i].handle == h)
      return &u32_windows[i];
  return 0;
}
struct u32_window *u32_root(struct u32_window *w) {
  for (UINT i = 0; w && w->parent && i < U32_WINDOWS; i++)
    w = u32_window(w->parent);
  return w;
}
static const uint16_t cp1252[32] = {
    0x20ac, 0x81,   0x201a, 0x192,  0x201e, 0x2026, 0x2020, 0x2021,
    0x2c6,  0x2030, 0x160,  0x2039, 0x152,  0x8d,   0x17d,  0x8f,
    0x90,   0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x2dc,  0x2122, 0x161,  0x203a, 0x153,  0x9d,   0x17e,  0x178};
unsigned u32_scalar(uint8_t c) {
  return c >= 128 && c < 160 ? cp1252[c - 128] : c;
}
int u32_ansi(unsigned c) {
  if (c < 128 || (c >= 160 && c <= 255))
    return c;
  for (UINT i = 0; i < 32; i++)
    if (cp1252[i] == c)
      return 128 + i;
  return '?';
}
UINT u32_utf8(char *out, UINT cap, const char *s) {
  UINT n = 0;
  for (; s && *s; s++) {
    unsigned c = u32_scalar((uint8_t)*s), need = c < 128 ? 1 : c < 2048 ? 2 : 3;
    if (c < 32 || c == 127 || (c >= 128 && c < 160))
      c = '?', need = 1;
    if (n + need >= cap)
      break;
    if (need == 3)
      out[n++] = 0xe0 | (c >> 12), out[n++] = 0x80 | ((c >> 6) & 63),
      out[n++] = 0x80 | (c & 63);
    else if (need == 2)
      out[n++] = 0xc0 | (c >> 6), out[n++] = 0x80 | (c & 63);
    else
      out[n++] = c;
  }
  out[n] = 0;
  return n;
}
BOOL u32_directory(UINT op, HWND h, DWORD index, DWORD type, DWORD flags,
                   const char *name, const void *data, DWORD n,
                   struct w32r_reply *reply, void *result, DWORD cap) {
  char utf8[1021];
  u32_utf8(utf8, sizeof(utf8), name ? name : "");
  DWORD len = w32_strlen(utf8) + 1,
        total = sizeof(struct w32r_request) + 1 + len + n;
  uint8_t *in = u32_alloc(total), *out = u32_alloc(W32R_MAX);
  if (!in || !out) {
    u32_free(in);
    u32_free(out);
    return 0;
  }
  struct w32r_request r = {.version = 1,
                           .op = op,
                           .key = U32_PTR(h),
                           .index = index,
                           .type = type,
                           .flags = flags,
                           .path_bytes = 1,
                           .name_bytes = len,
                           .data_bytes = n};
  w32_copy(in, &r, sizeof(r));
  in[sizeof(r)] = 0;
  w32_copy(in + sizeof(r) + 1, utf8, len);
  if (n)
    w32_copy(in + sizeof(r) + 1 + len, data, n);
  DWORD used = 0;
  DWORD e = u32_backend.rpc(in, total, out, W32R_MAX, &used);
  if (!e) {
    if (used < sizeof(*reply))
      e = 13;
    else {
      w32_copy(reply, out, sizeof(*reply));
      e = reply->error;
      if (!e &&
          (reply->version != 1 || reply->data_bytes > cap ||
           reply->name_bytes || used != sizeof(*reply) + reply->data_bytes))
        e = 13;
      if (!e && reply->data_bytes)
        w32_copy(result, out + sizeof(*reply), reply->data_bytes);
    }
  }
  u32_free(in);
  u32_free(out);
  return e ? u32_fail(e) : 1;
}
BOOL u32_directory_window(HWND h, struct w32r_window *v, char *title) {
  uint8_t b[1045];
  struct w32r_reply r;
  if (!u32_directory(W32R_WINDOW_GET, h, 0, 0, 0, "", 0, 0, &r, b, sizeof(b)) ||
      r.data_bytes < 25)
    return 0;
  w32_copy(v, b, 24);
  if (title) {
    UINT at = 0;
    const uint8_t *p = b + 24;
    while (*p && at < 255) {
      unsigned c = *p++;
      if (c >= 0xe0) {
        c &= 15;
        c = (c << 6) | (*p++ & 63);
        c = (c << 6) | (*p++ & 63);
      } else if (c >= 0xc0) {
        c &= 31;
        c = (c << 6) | (*p++ & 63);
      }
      title[at++] = u32_ansi(c);
    }
    title[at] = 0;
  }
  return 1;
}
BOOL u32_sync_window(struct u32_window *w) {
  uint8_t data[1045];
  struct w32r_window v = {U32_PTR(w->handle), w->pid,       w->tid,
                          U32_PTR(w->parent), w->cls->atom, w->style};
  w32_copy(data, &v, 24);
  UINT n = u32_utf8((char *)data + 24, 1021, w->title);
  struct w32r_reply r;
  BOOL ok = u32_directory(W32R_WINDOW_SET, w->handle, 0, 0, 0, "", data, 25 + n,
                          &r, 0, 0);
  u32_display_cache();
  return ok;
}
BOOL W32API SetRect(RECT *r, int l, int t, int x, int b) {
  if (!r)
    return 0;
  *r = (RECT){l, t, x, b};
  return 1;
}
BOOL W32API SetRectEmpty(RECT *r) { return SetRect(r, 0, 0, 0, 0); }
BOOL W32API CopyRect(RECT *r, const RECT *s) {
  if (!r || !s)
    return 0;
  *r = *s;
  return 1;
}
BOOL W32API IsRectEmpty(const RECT *r) {
  return !r || r->left >= r->right || r->top >= r->bottom;
}
BOOL W32API EqualRect(const RECT *a, const RECT *b) {
  return a && b && a->left == b->left && a->right == b->right &&
         a->top == b->top && a->bottom == b->bottom;
}
BOOL u32_rect_intersect(RECT *r, RECT a, RECT b) {
  r->left = a.left > b.left ? a.left : b.left;
  r->top = a.top > b.top ? a.top : b.top;
  r->right = a.right < b.right ? a.right : b.right;
  r->bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
  if (IsRectEmpty(r)) {
    SetRectEmpty(r);
    return 0;
  }
  return 1;
}
BOOL W32API IntersectRect(RECT *r, const RECT *a, const RECT *b) {
  return r && a && b ? u32_rect_intersect(r, *a, *b) : 0;
}
BOOL W32API UnionRect(RECT *r, const RECT *a, const RECT *b) {
  if (!r || !a || !b)
    return 0;
  if (IsRectEmpty(a)) {
    *r = *b;
    return !IsRectEmpty(r);
  }
  if (IsRectEmpty(b)) {
    *r = *a;
    return 1;
  }
  *r = (RECT){a->left < b->left ? a->left : b->left,
              a->top < b->top ? a->top : b->top,
              a->right > b->right ? a->right : b->right,
              a->bottom > b->bottom ? a->bottom : b->bottom};
  return 1;
}
BOOL W32API OffsetRect(RECT *r, int x, int y) {
  if (!r)
    return 0;
  r->left += x;
  r->right += x;
  r->top += y;
  r->bottom += y;
  return 1;
}
BOOL W32API InflateRect(RECT *r, int x, int y) {
  if (!r)
    return 0;
  r->left -= x;
  r->right += x;
  r->top -= y;
  r->bottom += y;
  return 1;
}
BOOL W32API PtInRect(const RECT *r, POINT p) {
  return r && p.x >= r->left && p.y >= r->top && p.x < r->right &&
         p.y < r->bottom;
}
#ifndef W32_HOST_MODEL
void *memcpy(void *d, const void *s, size_t n) { return w32_copy(d, s, n); }
void *memset(void *d, int c, size_t n) { return w32_fill(d, c, n); }
int strcmp(const char *a, const char *b) {
  while (*a && *a == *b)
    a++, b++;
  return (uint8_t)*a - (uint8_t)*b;
}
#endif

/* The desktop owns this existing plain configuration file. Only its keyboard
 * and double-click keys cross this boundary; the desktop theme never does. */
static int compare_prefix(const char *a, const char *b, UINT n) {
  for (UINT i = 0; i < n; i++)
    if (a[i] != b[i])
      return 1;
  return 0;
}
/* Merge only the shared appearance keys. CREATE_NEW serializes writers;
 * flush and replace publish a complete file, preserving unrelated settings. */
BOOL u32_appearance_commit(const struct appearance *a) {
  if(!ap_valid(a)) return u32_fail(87);
  char *data=u32_alloc(32769), *output=u32_alloc(32769+AP_HEX_BYTES+64);
  if(!data || !output) { u32_free(data); u32_free(output); return u32_fail(8); }
  char hex[AP_HEX_BYTES+1]; ap_encode(a,hex);
  UINT saved=u32_suspend(); DWORD count=0, used=0, error=0;
  const char *path="C:\\system\\settings\\desktop.cfg";
  const char *temporary="C:\\system\\settings\\desktop.cfg.appearance.tmp";
  HANDLE in=CreateFileA(path,0x80000000u,7,0,3,0,0);
  if(in!=K32_INVALID) {
    if(!ReadFile(in,data,32768,&count,0) || count==32768) error=13;
    CloseHandle(in);
  } else if(GetLastError()!=2 && GetLastError()!=3) error=GetLastError();
  data[count]=0;
  for(DWORD at=0; !error && at<count;) {
    DWORD start=at; while(at<count && data[at]!='\n') at++;
    if(at<count) at++;
    DWORD n=at-start;
    if((n>=11 && !compare_prefix(data+start,"appearance=",11)) ||
       (n>=14 && !compare_prefix(data+start,"desktop_color=",14))) continue;
    w32_copy(output+used,data+start,n); used+=n;
  }
  if(used && output[used-1]!='\n') output[used++]='\n';
  w32_copy(output+used,"appearance=",11); used+=11;
  w32_copy(output+used,hex,AP_HEX_BYTES); used+=AP_HEX_BYTES; output[used++]='\n';
  w32_copy(output+used,"desktop_color=",14); used+=14;
  ap_hexword(output+used,a->item[AP_DESKTOP].colour,6); used+=6; output[used++]='\n';
  HANDLE out=K32_INVALID;
  if(!error) {
    out=CreateFileA(temporary,0x40000000u,0,0,1,0,0);
    if(out==K32_INVALID) error=GetLastError();
  }
  if(out!=K32_INVALID) {
    DWORD written=0;
    if(!WriteFile(out,output,used,&written,0) || written!=used || !FlushFileBuffers(out)) error=GetLastError() ? GetLastError() : 29;
    CloseHandle(out);
    if(!error) {
      int32_t status=(int32_t)u32_backend.gate(CIUKI_SYS_RENAME,
        U32_PTR("/system/settings/desktop.cfg.appearance.tmp"),U32_PTR("/system/settings/desktop.cfg"),0);
      if(status<0) error=status==-13 ? 5 : 29;
    }
    if(error) DeleteFileA(temporary);
  }
  u32_resume(saved); u32_free(data); u32_free(output);
  return error ? u32_fail(error) : 1;
}
void u32_settings(void) {
  static BOOL read_once;
  static DWORD last;
  DWORD now = u32_ticks();
  if (read_once && (DWORD)(now - last) < 1000)
    return;
  read_once = 1;
  last = now;
  DWORD saved_error = GetLastError();
  char *data = u32_alloc(32769);
  if (!data)
    return;
  UINT saved = u32_suspend();
  HANDLE file = CreateFileA("C:\\system\\settings\\desktop.cfg", 0x80000000, 7,
                            0, 3, 0, 0);
  DWORD count = 0;
  BOOL ok = file != K32_INVALID && ReadFile(file, data, 32768, &count, 0);
  if (file != K32_INVALID)
    CloseHandle(file);
  u32_resume(saved);
  if (ok && count <= 32768) {
    data[count] = 0;
    struct appearance appearance;
    if(ap_settings_decode(&appearance,data,count)) {
      char encoded[AP_HEX_BYTES+1]; ap_encode(&appearance,encoded); u32_appearance_load(encoded);
    } else u32_appearance_load("");
    for (char *line = data; *line;) {
      char *end = line;
      while (*end && *end != '\n')
        end++;
      char finish = *end;
      *end = 0;
      if (w32_strlen(line) >= 11 && !compare_prefix(line, "keyboard=it", 11))
        u32_layout = 1;
      else if (w32_strlen(line) >= 11 &&
               !compare_prefix(line, "keyboard=us", 11))
        u32_layout = 0;
      else if (w32_strlen(line) >= 16 &&
               !compare_prefix(line, "double_click_ms=", 16)) {
        UINT n = 0;
        for (char *v = line + 16; *v >= '0' && *v <= '9' && n <= 5000; v++)
          n = n * 10 + (*v - '0');
        if (n >= 200 && n <= 900)
          SetDoubleClickTime(n);
      }
      if (!finish)
        break;
      line = end + 1;
    }
  }
  u32_free(data);
  SetLastError(saved_error);
}

void u32_clear_thread_input(void) {
  for (UINT i=0;i<U32_THREADS;i++) if (threads[i].id) {
    w32_fill(threads[i].keys,0,256);threads[i].text_count=0;
    threads[i].key_window=threads[i].translate_window=0;threads[i].key_lparam=0;
  }
}
BOOL u32_exiting;
/* TID zero selects every thread of the process (DLL_PROCESS_DETACH). */
static void cleanup_thread(DWORD tid, UINT mode) {
  BOOL identity = 0;
  for (UINT i = 0; i < U32_WINDOWS; i++) {
    struct u32_window *w = &u32_windows[i];
    if (w->handle && (!tid || w->tid == tid)) {
      identity = 1;
      /* A victim may have died inside DestroyWindow: finish that teardown. */
      if (mode != U32_NORMAL) w->destroying = 0;
      u32_destroy(w, mode);
    }
  }
  for (UINT i = 0; i < U32_TIMERS; i++)
    if (!tid || u32_timers[i].tid == tid) w32_fill(&u32_timers[i], 0, sizeof(u32_timers[i]));
  u32_hooks_cleanup(tid);
  u32_clipboard_cleanup(tid);
  u32_desktop_cleanup(tid);
  for (UINT i = 0; i < U32_THREADS; i++) {
    struct u32_thread *t = &threads[i];
    if (t->id && (!tid || t->id == tid)) {
      identity = 1;
      while (t->events) {
        struct u32_pending_event *e = t->events; t->events = e->next; u32_free(e);
      }
      for (struct u32_send_frame *f = t->send; f; f = f->prev) {
        u32_free(f->data); f->data = 0;
      }
      w32_fill(t, 0, sizeof(*t));
    }
  }
  u32_progman_cleanup(tid);
  /* Safety net for the server-side identity (a window free or clipboard close
   * refused above); threads that never used USER cost no transaction. */
  if (identity) {
    struct w32r_reply r;
    u32_directory(W32R_USER_CLEANUP, 0, tid, 0, 0, "", 0, 0, &r, 0, 0);
  }
}
void W32API w32_user_thread_cleanup(DWORD tid) {
  if (!tid) return;
  /* The admitted victim cannot resume user instructions. Its recursive USER
   * lock may have been abandoned at a syscall boundary. */
  DWORD expected = tid;
  if (tid != u32_tid() && __atomic_compare_exchange_n(&owner, &expected, 0, 0,
      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) depth = 0;
  U32_GUARD;
  cleanup_thread(tid, U32_VICTIM);
}
BOOL W32API UserDllMain(void *module, DWORD reason, void *reserved) {
  (void)module; (void)reserved;
  if (reason == 3) {
    U32_GUARD;
    cleanup_thread(u32_tid(), U32_NORMAL);
  } else if (!reason) {
    U32_GUARD;
    u32_exiting = 1;
    cleanup_thread(0, U32_TEARDOWN);
    for (UINT i = 0; i < U32_CLASSES; i++) {
      u32_free(u32_classes[i].extra);
      w32_fill(&u32_classes[i], 0, sizeof(u32_classes[i]));
    }
    u32_icons_cleanup();
  }
  return 1;
}
