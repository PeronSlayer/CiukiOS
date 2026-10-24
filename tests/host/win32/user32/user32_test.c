/* Linux transport/scheduler adapter. All USER and queue policy is production.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "../../../../sdk/win32/user32/internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#undef _SC_PAGESIZE
#undef _SC_OPEN_MAX
#undef _SC_ARG_MAX
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include "../../../../apps/w32srv/registry.h"
#include "session.h"
#include <unistd.h>
#include <pthread.h>
#include <ciuki/present_abi.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "USER CHECK %s:%d: %s (error %u)\n", __FILE__, __LINE__, \
              #x, GetLastError());                                             \
      abort();                                                                 \
    }                                                                          \
  } while (0)
void ui_kernel_init(void), ui_kernel_finish(void), ui_kernel_switch(unsigned),
    ui_kernel_schedule(void (*)(void)), ui_kernel_pause(uint32_t);
unsigned ui_kernel_current(void);
uint32_t ui_kernel_tid(void), ui_kernel_pid(void), ui_kernel_ticks(void);
int32_t ui_kernel_call(void *, uint32_t);
static DWORD errors[3];
DWORD W32API GetLastError(void) { return errors[ui_kernel_current()]; }
void W32API SetLastError(DWORD n) { errors[ui_kernel_current()] = n; }
DWORD W32API GetCurrentThreadId(void) { return ui_kernel_tid(); }
DWORD W32API GetCurrentProcessId(void) { return ui_kernel_pid(); }
DWORD W32API GetTickCount(void) { return ui_kernel_ticks(); }
#ifndef U32_TEST_EXTERNAL_MODULES
/* tests/host/win32/dialogs supplies file, module and resource services. */
HANDLE W32API CreateFileA(const char *name, DWORD access, DWORD share,
                          SECURITY_ATTRIBUTES *security, DWORD creation,
                          DWORD flags, HANDLE template) {
  (void)name;
  (void)access;
  (void)share;
  (void)security;
  (void)creation;
  (void)flags;
  (void)template;
  return K32_INVALID;
}
BOOL W32API ReadFile(HANDLE h, void *data, DWORD bytes, DWORD *count,
                     OVERLAPPED *o) {
  (void)h;
  (void)data;
  (void)bytes;
  (void)count;
  (void)o;
  return 0;
}
BOOL W32API CloseHandle(HANDLE h) {
  (void)h;
  return 1;
}
#endif
HANDLE W32API GetProcessHeap(void) { return (HANDLE)1; }
static unsigned allocations,allocation_calls;
void *W32API HeapAlloc(HANDLE h, DWORD flags, DWORD n) {
  (void)h;
  (void)flags;
  DWORD size = (n + 32 + 4095) & ~4095u;
  DWORD *p = mmap(0, size, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if (p == MAP_FAILED)
    return 0;
  p[0] = size;
  p[1] = n;
  allocations++;
  allocation_calls++;
  return p + 8;
}
BOOL W32API HeapFree(HANDLE h, DWORD flags, void *v) {
  (void)h;
  (void)flags;
  if (v) {
    DWORD *p = (DWORD *)v - 8;
    CHECK(!munmap(p, p[0]));
    allocations--;
  }
  return 1;
}
HANDLE W32API GlobalAlloc(DWORD flags, DWORD n) {
  return HeapAlloc((HANDLE)1, flags, n);
}
HANDLE W32API GlobalFree(HANDLE h) {
  HeapFree((HANDLE)1, 0, h);
  return 0;
}
void *W32API GlobalLock(HANDLE h) { return h; }
BOOL W32API GlobalUnlock(HANDLE h) {
  (void)h;
  return 0;
}
DWORD W32API GlobalSize(HANDLE h) { return h ? ((DWORD *)h)[-7] : 0; }
#ifndef U32_TEST_EXTERNAL_MODULES
HANDLE W32API FindResourceA(HANDLE h, const void *n, const void *t) {
  (void)h;
  (void)n;
  (void)t;
  return 0;
}
HANDLE W32API LoadResource(HANDLE h, HANDLE r) {
  (void)h;
  (void)r;
  return 0;
}
DWORD W32API SizeofResource(HANDLE h, HANDLE r) {
  (void)h;
  (void)r;
  return 0;
}
HANDLE W32API GetModuleHandleA(const char *);
HANDLE W32API LoadLibraryA(const char *s) { return GetModuleHandleA(s); }
#endif
/* Until the GDI phase USER runs on the fixture adapter (test_gdi below); then
 * gdi32.dll and user32.dll resolve to the production objects linked in. */
static BOOL host_modules, real_gdi;
#define HOST_GDI ((HANDLE)(uintptr_t)0x6d000000)
#define HOST_USER ((HANDLE)(uintptr_t)0x6e000000)
/* Weak: tests/host/win32/input reuses this harness without gdi32/ddraw. */
__attribute__((weak)) void *host_gdi_proc(const char *);
#ifndef U32_TEST_EXTERNAL_MODULES
HANDLE W32API GetModuleHandleA(const char *s) {
  if (!host_modules || !s)
    return 0;
  return !strcmp(s, "gdi32.dll") ? HOST_GDI
         : !strcmp(s, "user32.dll") ? HOST_USER
                                    : 0;
}
#define USER_EXPORT(name) {#name, (void *)name}
void *W32API GetProcAddress(HANDLE h, const char *s) {
  static const struct {
    const char *name;
    void *proc;
  } user[] = {USER_EXPORT(w32_user_display_lease),
              USER_EXPORT(w32_user_display_release),
              USER_EXPORT(w32_user_display_poll),
              USER_EXPORT(w32_user_display_clip),
              USER_EXPORT(w32_user_thread_cleanup),
              USER_EXPORT(GetDC),
              USER_EXPORT(ReleaseDC),
              USER_EXPORT(ClientToScreen),
              USER_EXPORT(GetWindowLongA),
              USER_EXPORT(AdjustWindowRectEx),
              USER_EXPORT(SetWindowPos)};
  if (h == HOST_GDI)
    return host_gdi_proc ? host_gdi_proc(s) : 0;
  for (UINT i = 0; h == HOST_USER && i < sizeof(user) / sizeof(*user); i++)
    if (!strcmp(user[i].name, s))
      return user[i].proc;
  return 0;
}
#endif
HANDLE W32API CreateThread(SECURITY_ATTRIBUTES *security, DWORD stack,
                          DWORD(W32API *entry)(void *), void *argument,
                          DWORD flags, DWORD *id) {
  (void)security;
  (void)stack;
  (void)entry;
  (void)argument;
  (void)flags;
  (void)id;
  return 0; /* the fixture drives w32_ddraw_tick itself */
}
DWORD W32API WaitForSingleObject(HANDLE h, DWORD ms) {
  (void)h;
  (void)ms;
  return 0;
}
int32_t w32_ciuki_call(void *r, uint32_t n) { return ui_kernel_call(r, n); }
void w32_diag(const char *a, const char *b, const char *c) {
  printf("[win32] %s %s!%s\n", a, b, c);
}
int W32API MultiByteToWideChar(DWORD page, DWORD flags, const char *s, int n,
                               WCHAR *out, int cap) {
  (void)page;
  (void)flags;
  if (n < 0)
    n = strlen(s) + 1;
  if (out && cap < n)
    return 0;
  for (int i = 0; out && i < n; i++)
    out[i] = u32_scalar((uint8_t)s[i]);
  return n;
}
int W32API WideCharToMultiByte(DWORD page, DWORD flags, const WCHAR *s, int n,
                               char *out, int cap, const char *def,
                               BOOL *used) {
  (void)page;
  (void)flags;
  (void)def;
  (void)used;
  if (n < 0) {
    n = 0;
    do {
      n++;
    } while (s[n - 1]);
  }
  if (out && cap < n)
    return 0;
  for (int i = 0; out && i < n; i++)
    out[i] = u32_ansi(s[i]);
  return n;
}
static struct wr_server server;
static UINT exchanges;
static UINT session_polls;
DWORD w32r_exchange(const void *r, DWORD n, void *out, DWORD cap, DWORD *used) {
  exchanges++;
  const struct w32r_request *request=r;
  if(request->op==W32R_ENDSESSION){
    CHECK(cap==sizeof(struct w32r_reply));
    if(request->index==W32R_SESSION_POLL)session_polls++;
  }
  size_t total = 0;
  DWORD error = wr_dispatch(&server, ui_kernel_pid(), r, n, out, cap, &total);
  *used = total;
  return error;
}
static struct {
  int fd;
  struct u32_surface s;
} surfaces[64];
static int next_fd = 100;
static BOOL surface(UINT w, UINT h, struct u32_surface *s) {
  for (UINT i = 0; i < 64; i++)
    if (!surfaces[i].fd) {
      void *p = HeapAlloc((HANDLE)1, 8, w * h * 4);
      if (!p)
        return 0;
      *s = (struct u32_surface){p, w, h, w * 4, 1, next_fd++};
      surfaces[i].fd = s->fd;
      surfaces[i].s = *s;
      return 1;
    }
  return 0;
}
static void drop(struct u32_surface *s) {
  if (s->pixels) {
    for (UINT i = 0; i < 64; i++)
      if (surfaces[i].fd == s->fd)
        surfaces[i].fd = 0;
    HeapFree((HANDLE)1, 0, s->pixels);
  }
  memset(s, 0, sizeof(*s));
  s->fd = -1;
}
static struct ciuki_message outgoing[2048], incoming[256];
static UINT outgoing_count, in_head, in_tail, reject_opcode;
static DWORD lease_token = 0x40, lease_generation = 0x80, lease_serial;
static DWORD frames_seen, frames_refused, frame_token, frame_generation;
static uint64_t fake_ns;
/* Fake desktop lease: GRANTED (state 1) carries the full 72-byte tuple. */
static void lease_event(DWORD id, DWORD generation, DWORD state, DWORD w,
                        DWORD h, DWORD bpp) {
  CHECK(in_tail - in_head < 256);
  struct ciuki_message *m = &incoming[in_tail++ % 256];
  desk_v3_message(m, DESK_V3_LEASE_EVENT, 72, 0, id, generation);
  uint8_t *b = m->data + 16;
  DWORD pitch = (w * (bpp / 8) + 3) & ~3u;
  DWORD words[18] = {state, ++lease_serial, w, h, bpp};
  if (state == 1) {
    DWORD tuple[13] = {++lease_token, ++lease_generation, 7, 1, w, h, pitch,
                       pitch * h, bpp == 8 ? 4 : bpp == 16 ? 2 : 1, 800, 600,
                       32, 0};
    memcpy(words + 5, tuple, sizeof(tuple));
  }
  for (UINT i = 0; i < 18; i++)
    desk_put32(b + i * 4, words[i]);
}
static struct ciuki_screen_control saver_policy={.version=1,.size=sizeof(saver_policy),.timeout=600,.saver=1};
#include "g5_clipboard_fixture.h"
DWORD w32r_gate(DWORD call, DWORD a, DWORD b, DWORD c) {
  (void)a;
  (void)c;
  if (call==CIUKI_SYS_DISPLAY && a==CIUKI_DISPLAY_SCREEN) {
    struct ciuki_screen_control *q=(void *)(uintptr_t)b;
    int old=(int)saver_policy.running;
    if (q->action==15) saver_policy.timeout=(UINT)q->value;
    if (q->action==17) saver_policy.enabled=q->value!=0;
    if (q->action==97) saver_policy.running=q->value!=0;
    if (q->action==0xf140 || q->action==0xf170) { saver_policy.command=q->action; saver_policy.argument=q->value; }
    saver_policy.flags=q->flags; *q=saver_policy; q->value=old; return 0;
  }
  if (call == CIUKI_SYS_CHANNEL_SEND) {
    CHECK(outgoing_count < 2048);
    struct ciuki_message *sent = (void *)(uintptr_t)b;
    outgoing[outgoing_count++] = *sent;
    if (sent->data[1]==DESK_V3_CLIPBOARD) { fixture_clipboard(sent); return 0; }
    if (sent->data[1] == DESK_V3_LEASE && sent->data[1] != reject_opcode) {
      DWORD flags = desk_get32(sent->data + 16);
      lease_event(desk_get32(sent->data + 8), desk_get32(sent->data + 12),
                  flags ? 1 : 3, flags ? desk_get32(sent->data + 20) : 320,
                  flags ? desk_get32(sent->data + 24) : 200,
                  flags ? desk_get32(sent->data + 28) : 8);
    }
    if (sent->data[0] == 3 && desk_get32(sent->data + 4)) {
      CHECK(in_tail - in_head < 256);
      struct ciuki_message *reply = &incoming[in_tail++ % 256];
      desk_v3_message(reply, DESK_V3_RESULT, 8, desk_get32(sent->data + 4),
                      desk_get32(sent->data + 8), desk_get32(sent->data + 12));
      desk_put32(reply->data + 16, sent->data[1]);
      desk_put32(reply->data + 20,
                 sent->data[1] == reject_opcode ? (DWORD)-12 : 0);
      if (sent->data[1] == reject_opcode)
        reject_opcode = 0;
    }
    return 0;
  }
  if (call == CIUKI_SYS_CHANNEL_RECV) {
    if (in_head == in_tail)
      return (DWORD)-11;
    *(struct ciuki_message *)(uintptr_t)b = incoming[in_head++ % 256];
    return 1;
  }
  if (call == CIUKI_SYS_SURFACE_INFO) {
    for (UINT i = 0; i < 64; i++)
      if (surfaces[i].fd == (int)a) {
        struct u32_surface *s = &surfaces[i].s;
        *(struct ciuki_surface_info *)(uintptr_t)b =
            (struct ciuki_surface_info){sizeof(struct ciuki_surface_info),
                                        s->width,
                                        s->height,
                                        s->pitch,
                                        1,
                                        (s->pitch * s->height + 4095) & ~4095u};
        return 0;
      }
    return (DWORD)-9;
  }
  if (call == CIUKI_SYS_SURFACE_MAP) {
    for (UINT i=0;i<64;i++) if (surfaces[i].fd==(int)a) return U32_PTR(surfaces[i].s.pixels);
    return (DWORD)-9;
  }
  if (call == CIUKI_SYS_MUNMAP) return 0;
  if (call == CIUKI_SYS_W32 && host_modules) {
    struct ciuki_w32_display_args *r = (void *)(uintptr_t)a;
    if (r->header.operation != W32_DISPLAY)
      return (DWORD)-38;
    /* The kernel admits only the current holder's token and generation. */
    if (r->token != lease_token || r->generation != lease_generation) {
      r->error = 1;
      __atomic_add_fetch(&frames_refused, 1, __ATOMIC_SEQ_CST);
      return 0;
    }
    if (r->action == W32_DISPLAY_FRAME) {
      frame_token = r->token;
      frame_generation = r->generation;
      __atomic_add_fetch(&frames_seen, 1, __ATOMIC_SEQ_CST);
    }
    r->result = 1;
    return 0;
  }
  if (call == CIUKI_SYS_CLOCK_GETTIME && host_modules) {
    uint64_t ns = __atomic_add_fetch(&fake_ns, 20000000ull, __ATOMIC_SEQ_CST);
    struct ciuki_timespec *t = (void *)(uintptr_t)b;
    t->tv_sec = (int64_t)(ns / 1000000000ull);
    t->tv_nsec = (int32_t)(ns % 1000000000ull);
    return 0;
  }
  if (call == CIUKI_SYS_DISPLAY && host_modules && a == CIUKI_DISPLAY_QUERY) {
    struct ciuki_display_info *info = (void *)(uintptr_t)b;
    *info = (struct ciuki_display_info){.version = CIUKI_DISPLAY_VERSION,
                                        .size = sizeof(*info),
                                        .width = 800,
                                        .height = 600,
                                        .bpp = 32,
                                        .pitch = 3200,
                                        .vram_bytes = 8u << 20,
                                        .mode_count = 2};
    info->modes[0] = (struct ciuki_display_mode){1, 640, 480, 32, 2560, 1, 0};
    info->modes[1] = (struct ciuki_display_mode){2, 800, 600, 32, 3200, 1, 1};
    return 0;
  }
  if (call == CIUKI_SYS_CLOSE) {
    for (UINT i=0;i<64;i++) if (surfaces[i].fd==(int)a && clip_temp[i]) { clip_temp[i]=0; drop(&surfaces[i].s); }
    return 0;
  }
  if (call == CIUKI_SYS_SLEEP_MS) {
    ui_kernel_pause(a);
    return 0;
  }
  return (DWORD)-38;
}
struct test_dc {
  struct u32_surface surface;
  POINT origin;
  RECT clip[64], excluded[128], touched[64];
  UINT clips, excludes, count;
};
static HDC acquire(const struct u32_surface *s, POINT o, const RECT *r, UINT n,
                   const RECT *ex, UINT en) {
  struct test_dc *dc = HeapAlloc((HANDLE)1, 8, sizeof(*dc));
  CHECK(dc);
  dc->surface = *s;
  dc->origin = o;
  dc->clips = n;
  dc->excludes = en;
  memcpy(dc->clip, r, n * sizeof(*r));
  memcpy(dc->excluded, ex, en * sizeof(*ex));
  return dc;
}
static UINT release(HDC h, RECT *r, UINT cap) {
  struct test_dc *dc = h;
  CHECK(dc->count <= cap);
  UINT n = dc->count;
  memcpy(r, dc->touched, n * sizeof(*r));
  HeapFree((HANDLE)1, 0, dc);
  return n;
}
static BOOL fill(HDC h, const RECT *rect, HBRUSH brush) {
  (void)brush;
  struct test_dc *dc = h;
  RECT r = *rect;
  OffsetRect(&r, dc->origin.x, dc->origin.y);
  for (UINT i = 0; i < dc->clips; i++) {
    RECT hit;
    if (!IntersectRect(&hit, &r, &dc->clip[i]))
      continue;
    for (int y = hit.top; y < hit.bottom; y++)
      for (int x = hit.left; x < hit.right; x++) {
        BOOL excluded = 0;
        for (UINT e = 0; e < dc->excludes; e++)
          if (PtInRect(&dc->excluded[e], (POINT){x, y}))
            excluded = 1;
        if (!excluded)
          w32_put32((uint8_t *)dc->surface.pixels + y * dc->surface.pitch +
                        x * 4,
                    0x00aabbcc);
      }
    CHECK(dc->count < 64);
    dc->touched[dc->count++] = hit;
  }
  return 1;
}
static const struct u32_gdi test_gdi = {
    .version = 1, .acquire = acquire, .release = release, .fill = fill};
static HWND windows[3], child;
static struct {
  HWND h;
  UINT m;
  WPARAM wp;
  LPARAM lp;
} trace[2048];
static UINT traces;
static int nesting;
static int minmax_set;
static int keep_on_close; /* G3: WM_CLOSE answered by a cancelled "Save changes?" */
static unsigned g5_render_count,g5_all_count;
static HWND g5_all_window;
static LRESULT W32API procedure(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m==WM_RENDERALLFORMATS && h==g5_all_window) {
    CHECK(OpenClipboard(h));
    CHECK(GetClipboardOwner()==h);
    HANDLE text=GlobalAlloc(2,5); CHECK(text); memcpy(GlobalLock(text),"all!",5);
    CHECK(SetClipboardData(CF_TEXT,text)==text); CHECK(CloseClipboard()); g5_all_count++; return 0;
  }
  if (m==0x305) {
    HANDLE text=GlobalAlloc(2,6); CHECK(text); memcpy(GlobalLock(text),"later",6);
    CHECK(SetClipboardData((UINT)wp,text)==text); g5_render_count++; return 0;
  }
  CHECK(traces < 2048);
  trace[traces].h = h;
  trace[traces].m = m;
  trace[traces].wp = wp;
  trace[traces++].lp = lp;
  if (m == WM_CLOSE && keep_on_close)
    return 0;
  if (m == WM_USER + 1) {
    CHECK(InSendMessage());
    CHECK(SendMessageA(windows[0], WM_USER + 2, 0, 0) == 22);
    return 11;
  }
  if (m == WM_USER + 2) {
    CHECK(InSendMessage());
    return 22;
  }
  if (m == WM_USER + 3) {
    CHECK(ReplyMessage(33));
    CHECK(!ReplyMessage(44));
    return 55;
  }
  if (m == WM_USER + 7)
    return 0x103;
  if (m == WM_USER + 8)
    return 0x10000;
  if (m == WM_COPYDATA) {
    const COPYDATASTRUCT *c = (void *)lp;
    CHECK(c->dwData == 7 && c->cbData == 3 && !memcmp(c->lpData, "hi", 3));
    return 66;
  }
  if (m == WM_GETMINMAXINFO && minmax_set) {
    MINMAXINFO *info = (MINMAXINFO *)lp;
    info->ptMinTrackSize = (POINT){200, 100};
    info->ptMaxTrackSize = (POINT){500, 400};
    return 0;
  }
  if (m == WM_PAINT) {
    PAINTSTRUCT ps;
    CHECK(BeginPaint(h, &ps));
    RECT r;
    GetClientRect(h, &r);
    CHECK(real_gdi ? FillRect(ps.hdc, &r, (HBRUSH)1) : fill(ps.hdc, &r, (HBRUSH)1));
    CHECK(EndPaint(h, &ps));
    return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}
static UINT peer = 1;
static void schedule_peer(void) {
  if (nesting > 4)
    return;
  nesting++;
  unsigned before = ui_kernel_current();
  ui_kernel_switch(before == peer ? 0 : peer);
  MSG m;
  PeekMessageA(&m, 0, 0, 0, QS_SENDMESSAGE << 16);
  ui_kernel_switch(before);
  nesting--;
}
static void event(struct u32_window *w, UINT op, const UINT *values,
                  UINT count) {
  CHECK(in_tail - in_head < 256);
  struct ciuki_message *m = &incoming[in_tail++ % 256];
  desk_v3_message(m, op, count * 4, 0, w->wire, w->generation);
  for (UINT i = 0; i < count; i++)
    desk_put32(m->data + 16 + i * 4, values[i]);
}
static void input_event(struct u32_window *w, UINT type, UINT code,
                        UINT value) {
  UINT data[10] = {0, 0, 0, type, 0, 0, code, value, 0, 0};
  event(w, DESK_KEY, data, 10);
}
static void discard(void) {
  MSG msg;
  for (UINT i = 0; i < 1000 && PeekMessageA(&msg, 0, 0, 0, 1); i++)
    if (msg.message == WM_PAINT)
      DispatchMessageA(&msg);
}
static UINT close_acks(UINT from, UINT wire) {
  UINT n = 0;
  for (UINT i = from; i < outgoing_count; i++)
    if (outgoing[i].data[0] == 3 && outgoing[i].data[1] == DESK_CLOSE_ACK) {
      CHECK(desk_get32(outgoing[i].data + 8) == wire &&
            !desk_get32(outgoing[i].data + 16));
      n++;
    }
  return n;
}
/* G3: the desktop's close request reaches WM_CLOSE; a window that survives
 * (the program's prompt was cancelled) declines with CLOSE_ACK 0, so the
 * desktop's "Closing..." ends; a destroyed window sends no decline. */
static void test_desktop_close(void) {
  MSG msg;
  HWND h = CreateWindowExA(0, "USER fixture", "closing", WS_VISIBLE | WS_CAPTION,
                           40, 40, 60, 60, 0, 0, 0, 0);
  CHECK(h);
  ValidateRect(h, 0);
  struct u32_window *w = u32_window(h);
  CHECK(w && w->wire);
  UINT wire = w->wire, before = outgoing_count;
  keep_on_close = 1;
  event(w, DESK_CLOSE_REQUEST, 0, 0);
  for (UINT i = 0; i < 100 && PeekMessageA(&msg, 0, 0, 0, 1); i++)
    DispatchMessageA(&msg);
  CHECK(IsWindow(h) && close_acks(before, wire) == 1 && !w->desktop_close);
  before = outgoing_count; /* the program's own WM_CLOSE is not the desktop's */
  CHECK(PostMessageA(h, WM_CLOSE, 0, 0));
  for (UINT i = 0; i < 100 && PeekMessageA(&msg, 0, 0, 0, 1); i++)
    DispatchMessageA(&msg);
  CHECK(IsWindow(h) && !close_acks(before, wire));
  keep_on_close = 0;
  before = outgoing_count;
  event(w, DESK_CLOSE_REQUEST, 0, 0);
  for (UINT i = 0; i < 100 && PeekMessageA(&msg, 0, 0, 0, 1); i++)
    DispatchMessageA(&msg);
  CHECK(!IsWindow(h) && !close_acks(before, wire));
  puts("PASS G3 desktop close: WM_CLOSE posted, a surviving window declines once, own WM_CLOSE silent, destroyed window silent");
}
static void test_send(void) {
  ui_kernel_schedule(schedule_peer);
  traces = 0;
  CHECK(SendMessageA(windows[1], WM_USER + 1, 0, 0) == 11);
  CHECK(traces == 2 && trace[0].m == WM_USER + 1 && trace[1].m == WM_USER + 2);
  printf("TRACE nested-send: peer:%u caller:%u reply=22 reply=11\n", trace[0].m,
         trace[1].m);
  CHECK(SendMessageA(windows[1], WM_USER + 3, 0, 0) == 33);
  CHECK(SendMessageA(windows[1], WM_SETTEXT, 0, (LPARAM) "relay title"));
  char title[64];
  memset(title, 0x5a, sizeof(title));
  CHECK(SendMessageA(windows[1], WM_GETTEXT, 64, (LPARAM)title) == 11 &&
        !strcmp(title, "relay title") && title[12] == 0x5a);
  COPYDATASTRUCT copy = {7, 3, "hi"};
  CHECK(SendMessageA(windows[1], WM_COPYDATA, 0, (LPARAM)&copy) == 66);
  CHECK(SendMessageA(windows[1], WM_USER + 7, 0, 0) == 0x103);
  CHECK(SendMessageA(windows[1], WM_USER + 8, 0, 0) == 0x10000);
  peer = 2;
  CHECK(SendMessageA(windows[2], WM_COPYDATA, 0, (LPARAM)&copy) == 66);
  peer = 1;
  ui_kernel_schedule(0);
  uintptr_t result = 0;
  CHECK(!SendMessageTimeoutA(windows[1], WM_USER + 4, 0, 0, 0, 3, &result) &&
        GetLastError() == 1460);
  CHECK(SendNotifyMessageA(windows[1], WM_USER + 5, 0, 0));
  ui_kernel_switch(1);
  MSG m;
  CHECK(!PeekMessageA(&m, 0, WM_USER, WM_USER + 100, 1));
  ui_kernel_switch(0);
  CHECK(!SendMessageA(windows[2], WM_GETMINMAXINFO, 0, (LPARAM)&copy) &&
        GetLastError() == 87);
  printf("TRACE sends: early-reply=33 copied-text copied-copydata timeout "
         "notify pointer-refusal\n");
}
static void test_queues(void) {
  MSG m;
  ValidateRect(windows[0], 0);
  CHECK(PostMessageA(windows[0], WM_USER + 10, 1, 0));
  CHECK(PeekMessageA(&m, windows[0], WM_USER + 10, WM_USER + 10, 0) &&
        m.wParam == 1);
  CHECK(MsgWaitForMultipleObjectsEx(0, 0, 0, QS_POSTMESSAGE, 0) ==
        W32_WAIT_TIMEOUT);
  CHECK(MsgWaitForMultipleObjectsEx(0, 0, 0, QS_POSTMESSAGE,
                                    MWMO_INPUTAVAILABLE) == 0);
  CHECK(PeekMessageA(&m, windows[0], WM_USER + 10, WM_USER + 10, 1));
  CHECK(PostMessageA(windows[0], WM_USER + 11, 1, 0));
  CHECK(PostMessageA(child, WM_USER + 12, 2, 0));
  CHECK(PeekMessageA(&m, windows[0], WM_USER + 12, WM_USER + 12, 1) &&
        m.hwnd == child);
  CHECK(PeekMessageA(&m, windows[0], WM_USER + 11, WM_USER + 11, 1));
  struct u32_window *w = u32_window(windows[0]);
  UINT activate[2] = {1, 1};
  event(w, DESK_V3_ACTIVATE, activate, 2);
  discard();
  CHECK(GetFocus() == windows[0]);
  input_event(w, CIUKI_INPUT_KEY, 30, 1);
  CHECK(PostMessageA(windows[0], WM_USER + 13, 0, 0));
  CHECK(InvalidateRect(windows[0], &(RECT){1, 1, 2, 2}, 0));
  CHECK(SetTimer(windows[0], 1, 10, 0) == 1);
  ui_kernel_pause(10);
  CHECK(PeekMessageA(&m, 0, 0, 0, 1) && m.message == WM_USER + 13);
  CHECK(PeekMessageA(&m, 0, 0, 0, 1) && m.message == WM_KEYDOWN);
  CHECK(PeekMessageA(&m, 0, 0, 0, 1) && m.message == WM_PAINT);
  CHECK(PeekMessageA(&m, 0, 0, 0, 1) && m.message == WM_PAINT);
  DispatchMessageA(&m);
  CHECK(PeekMessageA(&m, 0, 0, 0, 1) && m.message == WM_TIMER);
  CHECK(!PeekMessageA(&m, 0, 0, 0, 1));
  CHECK(KillTimer(windows[0], 1));
  PostQuitMessage(7);
  PostQuitMessage(8);
  CHECK(PeekMessageA(&m, child, 0x7000, 0x7fff, 0) && m.message == WM_QUIT &&
        m.wParam == 8);
  CHECK(GetMessageA(&m, child, 0x7000, 0x7fff) == 0 && m.wParam == 8);
  CHECK(!PeekMessageA(&m, 0, 0, 0, 1));
  printf(
      "TRACE modal-filter new-input: posted input paint paint timer quit=8\n");
}
static void test_input(void) {
  MSG msg;
  struct u32_window *w = u32_window(windows[0]);
  for (UINT layout = 0; layout < 2; layout++) {
    u32_layout = layout;
    u32_clear_input();
    for (UINT k = 0; k < sizeof(vk_scans) / sizeof(*vk_scans); k++) {
      UINT expected = layout ? vk_it[k] : vk_us[k];
      CHECK(MapVirtualKeyA(vk_scans[k], 1) == expected);
      CHECK(MapVirtualKeyA(expected, 0) == vk_scans[k]);
    }
    const unsigned (*rows)[6] = layout ? it : us;
    UINT count = layout ? sizeof(it) / sizeof(*it) : sizeof(us) / sizeof(*us);
    for (UINT i = 0; i < count; i++) {
      input_event(w, rows[i][0], rows[i][1], rows[i][2]);
      CHECK(PeekMessageA(&msg, 0, 0, 0, 1));
      CHECK(msg.message == rows[i][3] && msg.wParam == rows[i][4] &&
            (DWORD)msg.lParam == rows[i][5]);
      if (msg.message == WM_KEYDOWN) {
        CHECK(GetKeyState(msg.wParam) & 0x8000);
        CHECK(TranslateMessage(&msg));
      }
    }
    CHECK(GetKeyboardLayout(0) == (layout ? 0x04100410 : 0x04090409));
    CHECK(MapVirtualKeyA(26, 1) == (layout ? 0xba : 0xdb));
    printf("TRACE protocol-v3 layout=%s %u pinned KEY/TEXT messages\n",
           layout ? "it" : "us", count);
  }
  CHECK(SetCapture(child) == 0 && GetCapture() == child);
  CHECK(ReleaseCapture() && GetCapture() == 0);
  SetFocus(child);
  CHECK(GetFocus() == child);
  UINT lose[2] = {0, 0};
  event(w, DESK_V3_ACTIVATE, lose, 2);
  discard();
  CHECK(!GetFocus() && !GetForegroundWindow() && !GetKeyState(17));
  UINT gain[2] = {1, 1};
  event(w, DESK_V3_ACTIVATE, gain, 2);
  discard();
  CHECK(GetFocus() == windows[0]);
  UINT intra[2] = {0, 1};
  UINT trace_start = traces;
  event(w, DESK_V3_ACTIVATE, intra, 2);
  event(u32_window(windows[1]), DESK_V3_ACTIVATE, gain, 2);
  u32_pump();
  ui_kernel_switch(1);
  MSG switch_message;
  PeekMessageA(&switch_message, 0, 0, 0, QS_SENDMESSAGE << 16);
  CHECK(GetActiveWindow() == windows[1]);
  ui_kernel_switch(0);
  CHECK(GetActiveWindow() == 0 && GetForegroundWindow() == windows[1]);
  event(u32_window(windows[1]), DESK_V3_ACTIVATE, intra, 2);
  event(w, DESK_V3_ACTIVATE, gain, 2);
  u32_pump();
  ui_kernel_switch(1);
  PeekMessageA(&switch_message, 0, 0, 0, QS_SENDMESSAGE << 16);
  ui_kernel_switch(0);
  CHECK(GetActiveWindow() == windows[0] && GetForegroundWindow() == windows[0]);
  for (UINT i = trace_start; i < traces; i++)
    CHECK(!(trace[i].h == windows[0] && trace[i].m == WM_ACTIVATEAPP &&
            !trace[i].wp));
  CHECK(ClipCursor(&(RECT){10, 10, 40, 40}));
  CHECK(ClipCursor(0));
  CHECK(SetCursorPos(50, 60));
  POINT p;
  CHECK(GetCursorPos(&p) && p.x == 50 && p.y == 60);
  HCURSOR cursor = LoadCursorA(0, (char *)(uintptr_t)32512);
  CHECK(cursor);
  SetCursor(cursor);
  CHECK(ShowCursor(0) == -1 && ShowCursor(1) == 0);
  printf("TRACE capture focus loss/resync cursor clip warp\n");
}
static void test_paint(void) {
  struct u32_window *w = u32_window(windows[0]);
  ValidateRect(windows[0], 0);
  outgoing_count = 0;
  ui_kernel_pause(20);
  RECT a = {1, 2, 4, 6}, b = {10, 10, 12, 12};
  CHECK(InvalidateRect(windows[0], &a, 0));
  CHECK(InvalidateRect(windows[0], &b, 0));
  CHECK(UpdateWindow(windows[0]));
  ui_kernel_pause(20);
  u32_flush();
  UINT damages = 0;
  for (UINT i = 0; i < outgoing_count; i++)
    if (outgoing[i].data[1] == DESK_DAMAGE) {
      UINT *expected =
          damages == 0 ? (UINT[4]){1, 2, 3, 4} : (UINT[4]){10, 10, 2, 2};
      for (UINT j = 0; j < 4; j++)
        CHECK(desk_get32(outgoing[i].data + 16 + j * 4) == expected[j]);
      damages++;
    }
  CHECK(damages == 2);
  CHECK(w32_u32((uint8_t *)w->surface.pixels + 2 * w->surface.pitch + 4) ==
        0xaabbcc);
  CHECK(w32_u32(w->surface.pixels) == 0);
  printf("TRACE paint damage: (1,2,3,4) (10,10,2,2)\n");
}
static void test_clipboard(void) {
  CHECK(OpenClipboard(windows[0]));
  CHECK(!OpenClipboard(windows[0]));
  CHECK(EmptyClipboard());
  HANDLE text = GlobalAlloc(2, 4);
  memcpy(GlobalLock(text), "hi\xe8", 4);
  CHECK(SetClipboardData(CF_TEXT, text) == text);
  CHECK(IsClipboardFormatAvailable(CF_UNICODETEXT));
  HANDLE wide = GetClipboardData(CF_UNICODETEXT);
  CHECK(wide);
  WCHAR *chars = GlobalLock(wide);
  CHECK(chars[0] == 'h' && chars[1] == 'i' && chars[2] == 0xe8 &&
        chars[3] == 0);
  CHECK(CloseClipboard());
  ui_kernel_switch(2);
  CHECK(OpenClipboard(windows[2]));
  HANDLE other = GetClipboardData(CF_TEXT);
  CHECK(other && !strcmp(GlobalLock(other), "hi\xe8"));
  CHECK(CloseClipboard());
  ui_kernel_switch(0);
  CHECK(OpenClipboard(windows[0]));
  CHECK(EmptyClipboard());
  CHECK(CloseClipboard());
  /* A native producer uses the very same store as USER. */
  DWORD sequence=GetClipboardSequenceNumber();
  struct desk_clip_wire request={.op=DESK_CLIP_OPEN},reply; void *data=NULL;
  CHECK(!clip_request(&fixture_clip_store,999,&request,NULL,&reply,&data));
  request.op=DESK_CLIP_EMPTY; CHECK(!clip_request(&fixture_clip_store,999,&request,NULL,&reply,&data));
  static const uint8_t native_text[]={'n',0,'a',0,'t',0,'i',0,'v',0,'e',0,0xe8,0,0,0};
  request=(struct desk_clip_wire){.op=DESK_CLIP_PUT,.format=13,.bytes=sizeof(native_text)};
  CHECK(!clip_request(&fixture_clip_store,999,&request,native_text,&reply,&data));
  request=(struct desk_clip_wire){.op=DESK_CLIP_CLOSE}; CHECK(!clip_request(&fixture_clip_store,999,&request,NULL,&reply,&data));
  CHECK(GetClipboardSequenceNumber()>sequence && OpenClipboard(windows[0]));
  HANDLE native=GetClipboardData(CF_TEXT); CHECK(native && !strcmp(GlobalLock(native),"native\xe8")); CHECK(CloseClipboard());
  /* Registered formats are mapped by name across independent ID tables. */
  UINT registered=RegisterClipboardFormatA("G5 format"); CHECK(registered>=0xc000);
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard());
  HANDLE bytes=GlobalAlloc(2,4); memcpy(GlobalLock(bytes),"g5!",4);
  CHECK(SetClipboardData(registered,bytes)==bytes && IsClipboardFormatAvailable(registered));
  CHECK(CloseClipboard() && OpenClipboard(windows[0]));
  HANDLE got=GetClipboardData(registered); CHECK(got && !strcmp(GlobalLock(got),"g5!"));
  CHECK(CloseClipboard());
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard());
  SetLastError(0); CHECK(!SetClipboardData(CF_TEXT,0) && !GetLastError()); CHECK(CloseClipboard());
  peer=2; ui_kernel_schedule(schedule_peer);
  ui_kernel_switch(2); CHECK(OpenClipboard(windows[2]));
  HANDLE delayed=GetClipboardData(CF_TEXT); CHECK(delayed && !strcmp(GlobalLock(delayed),"later") && g5_render_count==1);
  CHECK(CloseClipboard()); ui_kernel_switch(0); ui_kernel_schedule(0); peer=1;
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard() && CloseClipboard());
  UINT promised=RegisterClipboardFormatA("G5 delayed"); CHECK(promised>=0xc000);
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard());
  SetLastError(0); CHECK(!SetClipboardData(promised,0) && !GetLastError());
  CHECK(!SetClipboardData(CF_TEXT,0) && !GetLastError()); CHECK(CloseClipboard());
  CHECK(OpenClipboard(windows[0]));
  CHECK(GetClipboardData(promised) && GetClipboardData(CF_TEXT) && !u32_clip_has_delayed());
  CHECK(g5_render_count==3 && EmptyClipboard() && CloseClipboard());
  g5_all_window=CreateWindowExA(0,"USER fixture","Clipboard owner",WS_VISIBLE,50,50,100,100,0,0,0,0);
  CHECK(g5_all_window && OpenClipboard(g5_all_window) && EmptyClipboard());
  SetLastError(0); CHECK(!SetClipboardData(CF_TEXT,0) && !GetLastError()); CHECK(CloseClipboard());
  CHECK(DestroyWindow(g5_all_window) && g5_all_count==1 && !GetClipboardOwner());
  g5_all_window=0;
  CHECK(OpenClipboard(windows[0])); HANDLE all=GetClipboardData(CF_TEXT);
  CHECK(all && !strcmp(GlobalLock(all),"all!")); CHECK(EmptyClipboard() && CloseClipboard());
  printf("TRACE G5 WM_RENDERALLFORMATS owner destruction preserves rendered bytes and releases original blocks\n");
  struct w32r_reply retired;
  CHECK(!u32_directory(W32R_CLIP_GET,0,u32_tid(),CF_TEXT,0,"",0,0,&retired,0,0) && GetLastError()==120);
  printf("TRACE G5 native to Win32 text, shared sequence, registered formats, cross-thread delayed WM_RENDERFORMAT, retired second store refused\n");
  printf("TRACE clipboard ownership copied UTF-16 and cross-process text\n");
}
static UINT hook_order[8], hook_calls, timer_calls, enum_count;
static LRESULT W32API hook_old(int code, WPARAM wp, LPARAM lp) {
  hook_order[hook_calls++] = 1;
  return CallNextHookEx(0, code, wp, lp);
}
static LRESULT W32API hook_new(int code, WPARAM wp, LPARAM lp) {
  hook_order[hook_calls++] = 2;
  return CallNextHookEx(0, code, wp, lp);
}
static void W32API timer_callback(HWND hwnd, UINT message, uintptr_t id,
                                  DWORD time) {
  (void)hwnd;
  (void)id;
  CHECK(message == WM_TIMER && time == u32_ticks());
  timer_calls++;
}
static BOOL W32API enumerate(HWND h, LPARAM unused) {
  (void)unused;
  CHECK(IsWindow(h));
  enum_count++;
  return 1;
}
static void test_extras(void) {
  CHECK(!SetWindowPos(windows[0], 0, 0x7fffffff, 0, 0, 0,
                      SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) &&
        GetLastError() == 87);
  HWND outside = CreateWindowExA(0, "USER fixture", "outside", WS_CHILD, 300,
                                 300, 20, 20, windows[0], 0, 0, 0);
  CHECK(outside);
  CHECK(ScrollWindow(outside, 1, 1, 0, 0));
  CHECK(ScrollWindow(windows[0], 0x7fffffff, (int)0x80000000, 0, 0));
  ValidateRect(windows[0], 0);
  CHECK(DestroyWindow(outside));

  MSG msg;
  HANDLE old = SetWindowsHookExA(3, hook_old, 0, u32_tid()),
         new = SetWindowsHookExA(3, hook_new, 0, u32_tid());
  CHECK(old && new);
  CHECK(PostMessageA(windows[0], WM_USER + 50, 0, 0));
  CHECK(PeekMessageA(&msg, windows[0], WM_USER + 50, WM_USER + 50, 1));
  CHECK(hook_calls == 2 && hook_order[0] == 2 && hook_order[1] == 1);
  CHECK(UnhookWindowsHookEx(old) && UnhookWindowsHookEx(new));
  CHECK(!SetWindowsHookExA(13, hook_new, 0, 0) && GetLastError() == 120);
  uintptr_t timer = SetTimer(0, 0, 1, timer_callback);
  CHECK(timer);
  ui_kernel_pause(100);
  CHECK(PeekMessageA(&msg, (HWND)(intptr_t)-1, WM_TIMER, WM_TIMER, 0));
  CHECK(PeekMessageA(&msg, (HWND)(intptr_t)-1, WM_TIMER, WM_TIMER, 1));
  DispatchMessageA(&msg);
  CHECK(timer_calls == 1);
  CHECK(!PeekMessageA(&msg, (HWND)(intptr_t)-1, WM_TIMER, WM_TIMER, 1));
  CHECK(KillTimer(0, timer));
  CHECK(EnumWindows(enumerate, 0) && enum_count == 3);
  enum_count = 0;
  CHECK(EnumChildWindows(windows[0], enumerate, 0) && enum_count == 1);
  CHECK(FindWindowA("USER fixture", "relay title") == windows[1]);
  reject_opcode = DESK_CREATE_WINDOW;
  UINT before = traces;
  CHECK(!CreateWindowExA(0, "USER fixture", "refused", WS_VISIBLE | WS_CAPTION,
                         0, 0, 50, 50, 0, 0, 0, 0) &&
        GetLastError() == 8);
  CHECK(traces == before);
  HICON ciuki = LoadIconA(0, (char *)(uintptr_t)32512),
        warning = LoadIconA(0, (char *)(uintptr_t)32515);
  CHECK(ciuki && warning && ciuki != warning);
  CHECK(LoadIconA(0, (char *)(uintptr_t)32512) == ciuki);
  CHECK(DestroyIcon(ciuki) && DestroyIcon(warning));
  uint8_t nonclient[340] = {0};
  w32_put32(nonclient, 340);
  CHECK(SystemParametersInfoA(41, 340, nonclient, 0) &&
        w32_u32(nonclient + 20) == 21 &&
        !strcmp((char *)nonclient + 52, "MS Sans Serif"));
  UINT lose[2] = {0, 1};
  struct u32_window *other = u32_window(windows[1]);
  event(other, DESK_V3_ACTIVATE, lose, 2);
  u32_pump();
  ui_kernel_switch(1);
  PeekMessageA(&msg, 0, 0, 0, QS_SENDMESSAGE << 16);
  ui_kernel_switch(0);
  printf("TRACE hooks LIFO timer callback global enumeration desktop rejection "
         "standard icons metrics thread routing\n");
}

/* Last WINDOW_ORDER request sent for W, as {flags, after, gen, owner, gen}. */
static BOOL last_order(struct u32_window *w, UINT out[5]) {
  for (UINT i = outgoing_count; i--;)
    if (outgoing[i].data[1] == DESK_V3_WINDOW_ORDER &&
        desk_get32(outgoing[i].data + 8) == w->wire &&
        desk_get32(outgoing[i].data + 12) == w->generation) {
      CHECK(outgoing[i].length == 36 && !outgoing[i].fd_count);
      for (UINT k = 0; k < 5; k++)
        out[k] = desk_get32(outgoing[i].data + 16 + k * 4);
      return 1;
    }
  return 0;
}
static UINT trace_index(HWND h, UINT m) {
  for (UINT i = 0; i < traces; i++)
    if (trace[i].h == h && trace[i].m == m)
      return i;
  return 9999;
}
static void test_order(void) {
  const UINT quiet = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
  HWND a = CreateWindowExA(0, "USER fixture", "a", WS_VISIBLE | WS_CAPTION,
                           200, 10, 40, 40, 0, 0, 0, 0),
       b = CreateWindowExA(0, "USER fixture", "b", WS_VISIBLE | WS_CAPTION,
                           210, 20, 40, 40, 0, 0, 0, 0);
  CHECK(a && b);
  struct u32_window *wa = u32_window(a), *wb = u32_window(b);
  CHECK(GetWindow(b, 2) == a);
  UINT o[5];
  outgoing_count = 0;
  /* hWndInsertAfter names the window that precedes: b goes directly below a. */
  CHECK(SetWindowPos(b, a, 0, 0, 0, 0, quiet));
  CHECK(last_order(wb, o) && o[0] == DESK_V3_ORDER_Z && o[1] == wa->wire &&
        o[2] == wa->generation && !o[3] && !o[4]);
  CHECK(GetWindow(a, 2) == b && wb->z < wa->z);
  CHECK(SetWindowPos(a, HWND_BOTTOM, 0, 0, 0, 0, quiet));
  CHECK(last_order(wa, o) && o[1] == DESK_V3_ORDER_BOTTOM && !o[2]);
  CHECK(GetWindow(a, 1) == a);
  CHECK(SetWindowPos(a, HWND_TOPMOST, 0, 0, 0, 0, quiet));
  CHECK(last_order(wa, o) && o[1] == DESK_V3_ORDER_TOPMOST &&
        (GetWindowLongA(a, GWL_EXSTYLE) & WS_EX_TOPMOST) &&
        GetWindow(b, 0) == a);
  /* Behind a topmost window: the topmost band is joined. */
  CHECK(SetWindowPos(b, a, 0, 0, 0, 0, quiet));
  CHECK((GetWindowLongA(b, GWL_EXSTYLE) & WS_EX_TOPMOST) &&
        GetWindow(a, 2) == b);
  CHECK(SetWindowPos(a, HWND_NOTOPMOST, 0, 0, 0, 0, quiet));
  CHECK(last_order(wa, o) && o[1] == DESK_V3_ORDER_NOTOPMOST &&
        !(GetWindowLongA(a, GWL_EXSTYLE) & WS_EX_TOPMOST) &&
        (GetWindowLongA(b, GWL_EXSTYLE) & WS_EX_TOPMOST));
  CHECK(!SetWindowPos(a, child, 0, 0, 0, 0, quiet) && GetLastError() == 1400);
  CHECK(!SetWindowPos(a, a, 0, 0, 0, 0, quiet) && GetLastError() == 1400);
  /* A refused desktop transaction leaves the local order untouched. */
  UINT before = wa->z;
  reject_opcode = DESK_V3_WINDOW_ORDER;
  CHECK(!SetWindowPos(a, HWND_TOP, 0, 0, 0, 0, quiet) && wa->z == before);
  reject_opcode = 0;
  /* GWL_HWNDPARENT changes the owner of a top-level window. */
  outgoing_count = 0;
  CHECK(SetWindowLongA(a, GWL_HWNDPARENT, (LONG)(uintptr_t)b) == 0);
  CHECK(GetWindow(a, 4) == b && last_order(wa, o) &&
        o[0] == DESK_V3_ORDER_OWNER && !o[1] && !o[2] && o[3] == wb->wire &&
        o[4] == wb->generation);
  /* The owner is topmost, so the owned window joins its band above it. */
  CHECK((GetWindowLongA(a, GWL_EXSTYLE) & WS_EX_TOPMOST) && GetWindow(a, 2) == b);
  CHECK(!SetWindowLongA(b, GWL_HWNDPARENT, (LONG)(uintptr_t)a) &&
        GetLastError() == 87 && !GetWindow(b, 4));
  CHECK(!SetWindowLongA(a, GWL_HWNDPARENT, (LONG)(uintptr_t)child) &&
        GetLastError() == 87);
  /* Leaving the topmost band takes owned windows along. */
  CHECK(SetWindowPos(b, HWND_NOTOPMOST, 0, 0, 0, 0, quiet));
  CHECK(!(GetWindowLongA(a, GWL_EXSTYLE) & WS_EX_TOPMOST));
  CHECK(SetWindowLongA(a, GWL_HWNDPARENT, 0) == (LONG)(uintptr_t)b &&
        !GetWindow(a, 4) && last_order(wa, o) && o[0] == DESK_V3_ORDER_OWNER &&
        !o[3] && !o[4]);
  /* Cross-thread dependent destruction: thread 1's window owned by b is
   * destroyed on thread 1 (WM_DESTROY there) before b itself. */
  ui_kernel_switch(1);
  HWND owned = CreateWindowExA(0, "USER fixture", "owned",
                               WS_VISIBLE | WS_CAPTION, 230, 30, 40, 40, b, 0,
                               0, 0);
  CHECK(owned && GetWindow(owned, 4) == b);
  ValidateRect(owned, 0);
  /* The private destroy request cannot be forged by an application. */
  CHECK(!SendMessageA(owned, U32_DESTROY, 0, 0) && IsWindow(owned));
  ui_kernel_switch(0);
  traces = 0;
  ui_kernel_schedule(schedule_peer);
  CHECK(DestroyWindow(b));
  ui_kernel_schedule(0);
  CHECK(!IsWindow(owned) && !IsWindow(b));
  CHECK(trace_index(owned, WM_DESTROY) < trace_index(b, WM_DESTROY) &&
        trace_index(owned, WM_NCDESTROY) < trace_index(b, WM_DESTROY));
  CHECK(DestroyWindow(a));
  printf("TRACE window order: insert-after bottom topmost notopmost owner "
         "cycle refusal cross-thread owned destruction\n");
}

static RECT local_rect(HWND h) { return u32_window(h)->rect; }
/* G4: CascadeWindows, TileWindows and ArrangeIconicWindows (Microsoft Learn,
 * winuser.h): explicit lists keep their order, MDITILE_ZORDER follows the
 * stacking, disabled/minimised/tool windows are left alone, a maximised
 * window is restored, minimised children line up along the bottom. */
static void test_arrange(void) {
  HWND p = CreateWindowExA(0, "USER fixture", "arrange", WS_VISIBLE | WS_CAPTION,
                           0, 0, 400, 300, 0, 0, 0, 0);
  CHECK(p);
  HWND k[3];
  for (UINT i = 0; i < 3; i++) {
    k[i] = CreateWindowExA(0, "USER fixture", "kid", WS_CHILD | WS_VISIBLE, 0,
                           0, 50, 50, p, (HANDLE)(uintptr_t)(20 + i), 0, 0);
    CHECK(k[i]);
  }
  RECT area = {0, 0, 400, 200}, r0, r1, r2;
  CHECK(CascadeWindows(p, 0, &area, 3, k) == 3);
  r0 = local_rect(k[0]); r1 = local_rect(k[1]); r2 = local_rect(k[2]);
  CHECK(r0.left == 0 && r0.top == 0 && r0.right == 300 && r0.bottom == 150);
  CHECK(r1.left == 24 && r1.top == 24 && r2.left == 48 && r2.top == 48);
  CHECK(r2.right - r2.left == 300 && r2.bottom - r2.top == 150);
  /* side by side, stacked, and the stacking decides with MDITILE_ZORDER */
  CHECK(TileWindows(p, MDITILE_VERTICAL, &area, 3, k) == 3);
  r0 = local_rect(k[0]); r1 = local_rect(k[1]); r2 = local_rect(k[2]);
  CHECK(r0.left == 0 && r0.right == 133 && r1.left == 133 && r1.right == 266 &&
        r2.left == 266 && r2.right == 400 && r0.top == 0 && r0.bottom == 200);
  CHECK(TileWindows(p, MDITILE_HORIZONTAL, &area, 3, k) == 3);
  r0 = local_rect(k[0]); r1 = local_rect(k[1]); r2 = local_rect(k[2]);
  CHECK(r0.top == 0 && r0.bottom == 66 && r1.top == 66 && r1.bottom == 133 &&
        r2.top == 133 && r2.bottom == 200 && r0.left == 0 && r0.right == 400);
  CHECK(SetWindowPos(k[2], HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
  CHECK(TileWindows(p, MDITILE_VERTICAL | MDITILE_ZORDER, &area, 3, 0) == 3);
  CHECK(local_rect(k[2]).left == 0);   /* the window in front takes the first cell */
  /* four and five windows fill a grid; the short last line shares its width */
  HWND more[2];
  for (UINT i = 0; i < 2; i++)
    more[i] = CreateWindowExA(0, "USER fixture", "more", WS_CHILD | WS_VISIBLE,
                              0, 0, 50, 50, p, (HANDLE)(uintptr_t)(30 + i), 0, 0);
  HWND five[5] = {k[0], k[1], k[2], more[0], more[1]};
  CHECK(TileWindows(p, MDITILE_VERTICAL, &area, 5, five) == 5);
  r0 = local_rect(five[0]); r2 = local_rect(five[2]);
  RECT r3 = local_rect(five[3]), r4 = local_rect(five[4]);
  CHECK(r0.right == 133 && r2.right == 400 && r0.bottom == 100 &&
        r3.top == 100 && r3.left == 0 && r3.right == 200 && r4.left == 200 &&
        r4.right == 400 && r4.bottom == 200);
  /* disabled, minimised and a maximised window */
  EnableWindow(k[1], 0);
  CHECK(CascadeWindows(p, MDITILE_SKIPDISABLED, &area, 3, k) == 2);
  EnableWindow(k[1], 1);
  ShowWindow(k[0], 3);
  CHECK(IsZoomed(k[0]));
  CHECK(TileWindows(p, 0, &area, 3, k) == 3 && !IsZoomed(k[0]));
  ShowWindow(k[2], 6);
  CHECK(IsIconic(k[2]) && TileWindows(p, 0, &area, 3, k) == 2);
  /* minimised children line up along the bottom of the parent */
  ShowWindow(k[1], 6);
  RECT client;
  CHECK(GetClientRect(p, &client) && ArrangeIconicWindows(p) == 24);
  r1 = local_rect(k[1]); r2 = local_rect(k[2]);
  CHECK(r2.left == 0 && r2.top == client.bottom - 24 && r1.left == 160 &&
        r1.top == client.bottom - 24);
  CHECK(ArrangeIconicWindows(U32_DESKTOP) == 24);
  CHECK(!CascadeWindows((HWND)(uintptr_t)0x7777, 0, 0, 0, 0) &&
        GetLastError() == 1400);
  /* top-level windows: the tool window and the topmost one are not arranged */
  HWND top[3];
  for (UINT i = 0; i < 3; i++)
    top[i] = CreateWindowExA(i == 1 ? WS_EX_TOOLWINDOW : 0, "USER fixture", "top",
                             WS_VISIBLE | WS_CAPTION, 5, 40, 60, 60, 0, 0, 0, 0);
  CHECK(SetWindowPos(top[2], HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
  RECT work = {0, 32, 800, 600};
  CHECK(TileWindows(0, 0, &work, 3, top) == 1);
  CHECK(local_rect(top[0]).left == 0 && local_rect(top[0]).top == 32 &&
        local_rect(top[0]).right == 800 && local_rect(top[1]).left == 5);
  for (UINT i = 0; i < 3; i++) CHECK(DestroyWindow(top[i]));
  CHECK(DestroyWindow(p));
  printf("TRACE arrange: cascade tile zorder grid skip restore iconic "
         "toplevel\n");
}
/* G4 (WM-7, WM-8): the maximised state travels with the placement (state 3),
 * the desktop's zoom reaches the program as WM_SIZE SIZE_MAXIMIZED, and the
 * track sizes of WM_GETMINMAXINFO go to the desktop once and when they change. */
static UINT sent_count(UINT opcode);
static const struct ciuki_message *last_sent(UINT opcode) {
  for (UINT i = outgoing_count; i-- > 0;)
    if (outgoing[i].data[1] == opcode)
      return &outgoing[i];
  return 0;
}
static void test_zoom_limits(void) {
  HWND h = CreateWindowExA(0, "USER fixture", "zoom", WS_VISIBLE | WS_CAPTION,
                           30, 40, 220, 160, 0, 0, 0, 0);
  CHECK(h);
  struct u32_window *w = u32_window(h);
  ValidateRect(h, 0);
  outgoing_count = 0;
  minmax_set = 0;
  CHECK(SetWindowPos(h, 0, 31, 41, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
  CHECK(last_sent(DESK_V3_PLACE) && !sent_count(DESK_V3_LIMITS));   /* defaults are not sent */
  minmax_set = 1;
  CHECK(SetWindowPos(h, 0, 32, 41, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
  const struct ciuki_message *l = last_sent(DESK_V3_LIMITS);
  CHECK(l && sent_count(DESK_V3_LIMITS) == 1 && l->length == 32 &&
        desk_get32(l->data + 16) == 194 && desk_get32(l->data + 20) == 73 &&
        desk_get32(l->data + 24) == 494 && desk_get32(l->data + 28) == 373);
  CHECK(SetWindowPos(h, 0, 33, 41, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
  CHECK(sent_count(DESK_V3_LIMITS) == 1);   /* unchanged */
  minmax_set = 0;
  /* ShowWindow(SW_MAXIMIZE): the placement asks for state 3 */
  traces = 0;
  ShowWindow(h, 3);
  const struct ciuki_message *pl = last_sent(DESK_V3_PLACE);
  CHECK(IsZoomed(h) && pl && desk_get32(pl->data + 16 + 20) == 3);
  /* the desktop zoomed the window: WM_SIZE with SIZE_MAXIMIZED */
  UINT zoomed[] = {700, 500, 1, 9, 0, 32, 3, 0};
  event(w, DESK_CONFIGURE, zoomed, 8);
  u32_pump();
  UINT size_zoom = 0;
  for (UINT i = 0; i < traces; i++)
    if (trace[i].h == h && trace[i].m == WM_SIZE && trace[i].wp == SIZE_MAXIMIZED)
      size_zoom++;
  CHECK(size_zoom == 1 && IsZoomed(h) && !IsIconic(h));
  /* the desktop restores it (title button): state 0 clears the maximised state */
  traces = 0;
  UINT normal[] = {220, 160, 1, 10, 30, 40, 0, 0};
  event(w, DESK_CONFIGURE, normal, 8);
  u32_pump();
  UINT size_restore = 0;
  for (UINT i = 0; i < traces; i++)
    if (trace[i].h == h && trace[i].m == WM_SIZE && trace[i].wp == SIZE_RESTORED)
      size_restore++;
  CHECK(size_restore == 1 && !IsZoomed(h));
  CHECK(DestroyWindow(h));
  printf("TRACE zoom: placement state 3, SIZE_MAXIMIZED, SIZE_RESTORED, "
         "WM_GETMINMAXINFO track sizes sent once\n");
}
#define SEAM_INVALID ((int32_t)0x80070057u)
#define SEAM_NOCLIP ((int32_t)0x887602cdu)
#define SEAM_MORE ((int32_t)0x887602b2u)
static UINT sent_count(UINT opcode) {
  UINT n = 0;
  for (UINT i = 0; i < outgoing_count; i++)
    n += outgoing[i].data[1] == opcode;
  return n;
}
static struct {
  uintptr_t hwnd;
  volatile int done;
  struct dd_lease lease;
  DWORD count;
  int32_t clip;
} worker;
/* The DirectDraw pacing worker: no USER lock, no desktop I/O. */
static void *poll_worker(void *unused) {
  (void)unused;
  for (UINT i = 0; i < 1000; i++) {
    w32_user_display_poll(worker.hwnd, &worker.lease);
    worker.count = 0;
    worker.clip = w32_user_display_clip(worker.hwnd, 0, &worker.count);
  }
  __atomic_store_n(&worker.done, 1, __ATOMIC_RELEASE);
  return 0;
}
static void test_display(void) {
  HWND g = CreateWindowExA(0, "USER fixture", "game", WS_VISIBLE | WS_POPUP, 0,
                           0, 320, 200, 0, 0, 0, 0);
  CHECK(g);
  ValidateRect(g, 0);
  struct u32_window *w = u32_window(g);
  struct dd_lease l, p;
  outgoing_count = 0;
  CHECK(w32_user_display_lease((uintptr_t)g, 320, 200, 8, &l) == 0);
  CHECK(sent_count(DESK_V3_LEASE) == 1 && l.active && l.token == lease_token &&
        l.generation == lease_generation && l.width == 320 && l.height == 200 &&
        l.bpp == 8 && l.pitch == 320);
  w32_user_display_poll((uintptr_t)g, &p);
  CHECK(!memcmp(&p, &l, sizeof(p)));
  /* Same logical mode: the cached grant, no second desktop transaction. */
  CHECK(w32_user_display_lease((uintptr_t)g, 320, 200, 8, &p) == 0 &&
        !memcmp(&p, &l, sizeof(p)) && sent_count(DESK_V3_LEASE) == 1);
  CHECK(w32_user_display_lease((uintptr_t)g, 320, 200, 12, &p) == SEAM_INVALID &&
        !p.active);
  /* Only the owner thread sends lease requests. */
  ui_kernel_switch(1);
  CHECK(w32_user_display_lease((uintptr_t)g, 320, 200, 8, &p) == SEAM_INVALID);
  ui_kernel_switch(0);
  /* Deactivation clears the cached lease before any procedure runs. */
  UINT off[2] = {0, 0}, on[2] = {1, 1};
  event(w, DESK_V3_ACTIVATE, off, 2);
  u32_pump();
  w32_user_display_poll((uintptr_t)g, &p);
  CHECK(!p.active && !p.token);
  /* Same-mode regrant after reactivation: new token and generation. */
  event(w, DESK_V3_ACTIVATE, on, 2);
  lease_event(w->wire, w->generation, 1, 320, 200, 8);
  u32_pump();
  discard();
  w32_user_display_poll((uintptr_t)g, &p);
  CHECK(p.active && p.token == lease_token && p.token != l.token &&
        p.generation == lease_generation && p.width == 320);
  /* A grant for another logical mode is not this window's lease. */
  lease_event(w->wire, w->generation, 1, 640, 480, 16);
  u32_pump();
  w32_user_display_poll((uintptr_t)g, &p);
  CHECK(!p.active);
  lease_event(w->wire, w->generation, 1, 320, 200, 8);
  u32_pump();
  /* Visible clip: half-open screen rectangles minus windows above. */
  RECT r[8];
  DWORD n = 0;
  CHECK(w32_user_display_clip((uintptr_t)g, 0, &n) == 0 && n == 1);
  n = 8;
  CHECK(w32_user_display_clip((uintptr_t)g, r, &n) == 0 && n == 1 &&
        r[0].left == 0 && r[0].top == 0 && r[0].right == 320 &&
        r[0].bottom == 200);
  HWND cover = CreateWindowExA(0, "USER fixture", "cover",
                               WS_VISIBLE | WS_POPUP, 100, 50, 50, 50, 0, 0, 0,
                               0);
  CHECK(cover);
  ValidateRect(cover, 0);
  n = 2;
  CHECK(w32_user_display_clip((uintptr_t)g, r, &n) == SEAM_MORE && n == 4);
  n = 8;
  CHECK(w32_user_display_clip((uintptr_t)g, r, &n) == 0 && n == 4);
  long area = 0;
  for (UINT i = 0; i < n; i++) {
    RECT hole;
    CHECK(!IntersectRect(&hole, &r[i], &(RECT){100, 50, 150, 100}));
    area += (r[i].right - r[i].left) * (r[i].bottom - r[i].top);
  }
  CHECK(area == 320 * 200 - 50 * 50);
  CHECK(w32_user_display_clip((uintptr_t)g, r, 0) == SEAM_INVALID);
  CHECK(w32_user_display_clip(0x7777, r, &n) == SEAM_NOCLIP && !n);
  /* A child window's list is clipped by its parent's client area. */
  HWND inner = CreateWindowExA(0, "USER fixture", "inner",
                               WS_CHILD | WS_VISIBLE, 300, 10, 40, 30, g, 0, 0,
                               0);
  CHECK(inner);
  ValidateRect(inner, 0);
  n = 8;
  CHECK(w32_user_display_clip((uintptr_t)inner, r, &n) == 0 && n == 1 &&
        r[0].left == 300 && r[0].right == 320 && r[0].top == 10 &&
        r[0].bottom == 40);
  w32_user_display_poll((uintptr_t)inner, &p);
  CHECK(!p.active);
  /* Worker reads complete while the GUI side holds the USER lock. */
  worker.hwnd = (uintptr_t)g;
  worker.done = 0;
  u32_lock();
  pthread_t thread;
  CHECK(!pthread_create(&thread, 0, poll_worker, 0));
  for (UINT i = 0; i < 5000 && !__atomic_load_n(&worker.done, __ATOMIC_ACQUIRE); i++)
    usleep(1000);
  CHECK(__atomic_load_n(&worker.done, __ATOMIC_ACQUIRE));
  u32_unlock(0);
  CHECK(!pthread_join(thread, 0));
  CHECK(worker.lease.active && worker.lease.token == lease_token &&
        worker.clip == 0 && worker.count == 4);
  /* Release: one LEASE(0) transaction, cached state cleared at once. */
  outgoing_count = 0;
  w32_user_display_release((uintptr_t)g);
  w32_user_display_poll((uintptr_t)g, &p);
  CHECK(!p.active && sent_count(DESK_V3_LEASE) == 1 &&
        !desk_get32(outgoing[0].data + 16));
  u32_pump();
  CHECK(DestroyWindow(cover) && DestroyWindow(g));
  CHECK(w32_user_display_clip((uintptr_t)g, r, &n) == SEAM_NOCLIP);
  printf("TRACE display seam: lease poll same-mode regrant deactivation clip "
         "lists child clip lock-free worker release\n");
}

BOOL W32API UserDllMain(void *, DWORD, void *);
static UINT hook_hits;
static LRESULT W32API hook_count(int code, WPARAM wp, LPARAM lp) {
  hook_hits++;
  return CallNextHookEx(0, code, wp, lp);
}
static void test_lifecycle(void) {
  MSG msg;
  /* Forced termination: the victim's windows, timers, hooks and clipboard
   * lock are released without running its procedures; a dependent owned by
   * the terminating thread is destroyed there with WM_DESTROY. */
  ui_kernel_switch(1);
  DWORD victim = u32_tid();
  HWND v = CreateWindowExA(0, "USER fixture", "victim",
                           WS_VISIBLE | WS_CAPTION, 10, 10, 50, 50, 0, 0, 0, 0);
  CHECK(v);
  ValidateRect(v, 0);
  CHECK(SetTimer(v, 9, 10, 0) == 9);
  CHECK(SetWindowsHookExA(3, hook_count, 0, victim));
  CHECK(OpenClipboard(v));
  ui_kernel_switch(0);
  HWND dependent = CreateWindowExA(0, "USER fixture", "dependent",
                                   WS_VISIBLE | WS_CAPTION, 20, 20, 50, 50, v,
                                   0, 0, 0);
  CHECK(dependent && GetWindow(dependent, 4) == v);
  ValidateRect(dependent, 0);
  CHECK(!OpenClipboard(dependent));
  traces = 0;
  w32_user_thread_cleanup(victim);
  CHECK(!IsWindow(v) && !IsWindow(dependent));
  CHECK(trace_index(v, WM_DESTROY) == 9999 &&
        trace_index(v, WM_NCDESTROY) == 9999 &&
        trace_index(dependent, WM_DESTROY) <
            trace_index(dependent, WM_NCDESTROY));
  CHECK(OpenClipboard(0) && CloseClipboard());
  ui_kernel_switch(1);
  ui_kernel_pause(20);
  CHECK(!PeekMessageA(&msg, 0, WM_TIMER, WM_TIMER, 1));
  HWND w1 = CreateWindowExA(0, "USER fixture", "exiting",
                            WS_VISIBLE | WS_CAPTION, 30, 30, 50, 50, 0, 0, 0,
                            0);
  CHECK(w1);
  ValidateRect(w1, 0);
  hook_hits = 0;
  CHECK(PostMessageA(w1, WM_USER + 60, 0, 0) &&
        PeekMessageA(&msg, w1, WM_USER + 60, WM_USER + 60, 1) && !hook_hits);
  /* Ordinary ExitThread: DLL_THREAD_DETACH destroys the exiting thread's
   * windows on that thread, procedures included. */
  traces = 0;
  CHECK(UserDllMain(0, 3, 0));
  CHECK(!IsWindow(w1) &&
        trace_index(w1, WM_DESTROY) < trace_index(w1, WM_NCDESTROY) &&
        trace_index(w1, WM_NCDESTROY) < 9999);
  /* A thread without USER state (here: already detached) costs no service
   * transaction at DLL_THREAD_DETACH. */
  ui_kernel_switch(2);
  CHECK(UserDllMain(0, 3, 0));
  UINT before = exchanges;
  CHECK(UserDllMain(0, 3, 0) && exchanges == before);
  ui_kernel_switch(0);
  /* Service identity teardown: the per-thread request removes a record the
   * process left behind; malformed requests are refused. */
  struct w32r_reply r;
  CHECK(u32_directory(W32R_WINDOW_ALLOC, 0, victim, 1, 0, "orphan", 0, 0, &r,
                      0, 0));
  HWND orphan = (HWND)(uintptr_t)r.key;
  struct w32r_window value;
  char title[1024];
  CHECK(u32_directory_window(orphan, &value, title) && value.tid == victim);
  CHECK(!u32_directory(W32R_USER_CLEANUP, (HWND)1, victim, 0, 0, "", 0, 0, &r,
                       0, 0) &&
        GetLastError() == 87);
  CHECK(u32_directory(W32R_USER_CLEANUP, 0, victim, 0, 0, "", 0, 0, &r, 0, 0));
  CHECK(!u32_directory_window(orphan, &value, title));
  enum_count = 0;
  CHECK(EnumWindows(enumerate, 0) && enum_count == 0);
  printf("TRACE lifecycle: forced thread cleanup without callbacks, live "
         "dependent WM_DESTROY, timers hooks clipboard released, thread detach "
         "destruction, service identity teardown\n");
}
static void test_spi(void) {
  UINT timeout=0; BOOL active=1;
  CHECK(SystemParametersInfoA(14,0,&timeout,0) && timeout==600);
  CHECK(SystemParametersInfoA(15,120,0,1) && saver_policy.flags==1);
  CHECK(SystemParametersInfoA(14,0,&timeout,0) && timeout==120);
  CHECK(SystemParametersInfoA(16,0,&active,0) && !active);
  CHECK(SystemParametersInfoA(17,1,0,0) && SystemParametersInfoA(16,0,&active,0) && active);
  CHECK(!SystemParametersInfoA(14,0,0,0) && GetLastError()==87);
  CHECK(!SystemParametersInfoA(15,0x80000000u,0,0) && GetLastError()==87);
  CHECK(!SystemParametersInfoA(119,1,0,0) && GetLastError()==120);
  CHECK(SystemParametersInfoA(118,0,&active,0) && !active);
  DefWindowProcA(windows[0],WM_SYSCOMMAND,0xf141,0);
  CHECK(saver_policy.command==0xf140 && saver_policy.argument==-1);
  DefWindowProcA(windows[0],WM_SYSCOMMAND,0xf170,2);
  CHECK(saver_policy.command==0xf170 && saver_policy.argument==2);

  int mouse[3] = {0}, value = 0;
  CHECK(SystemParametersInfoA(3, 0, mouse, 0) && mouse[0] == 6 &&
        mouse[1] == 10 && mouse[2] == 1);
  int faster[3] = {4, 12, 2};
  CHECK(SystemParametersInfoA(4, 0, faster, 0) &&
        SystemParametersInfoA(3, 0, mouse, 0) && mouse[1] == 12);
  BOOL beep = 0;
  CHECK(SystemParametersInfoA(1, 0, &beep, 0) && beep);
  CHECK(SystemParametersInfoA(2, 0, 0, 0) &&
        SystemParametersInfoA(1, 0, &beep, 0) && !beep);
  CHECK(SystemParametersInfoA(13, 0, &value, 0) && value == 75);
  CHECK(SystemParametersInfoA(13, 90, 0, 0) &&
        SystemParametersInfoA(13, 0, &value, 0) && value == 90 &&
        GetSystemMetrics(38) == 90);
  CHECK(SystemParametersInfoA(29, 8, 0, 0) && GetSystemMetrics(36) == 8);
  CHECK(SystemParametersInfoA(33, 1, 0, 0) && GetSystemMetrics(23) == 1);
  CHECK(SystemParametersInfoA(33, 0, 0, 0) && !GetSystemMetrics(23));
  RECT work = {0, 0, 0, 0}, set = {0, 0, 640, 400};
  CHECK(SystemParametersInfoA(48, 0, &work, 0) && work.top == 32);
  CHECK(SystemParametersInfoA(47, 0, &set, 0) &&
        SystemParametersInfoA(48, 0, &work, 0) && work.bottom == 400);
  set.right = 5000;
  CHECK(!SystemParametersInfoA(47, 0, &set, 0) && GetLastError() == 87);
  CHECK(SystemParametersInfoA(113, 0, (void *)(uintptr_t)15, 0) &&
        SystemParametersInfoA(112, 0, &value, 0) && value == 15);
  CHECK(!SystemParametersInfoA(113, 0, (void *)(uintptr_t)21, 0));
  BOOL old = 1;
  CHECK(SystemParametersInfoA(97, 1, &old, 0) && !old);
  CHECK(SystemParametersInfoA(97, 0, &old, 0) && old);
  uint8_t ncm[340] = {0};
  w32_put32(ncm, 340);
  CHECK(SystemParametersInfoA(41, 340, ncm, 0));
  w32_put32(ncm + 20, 25);
  CHECK(SystemParametersInfoA(42, 340, ncm, 0));
  w32_fill(ncm + 4, 0, 336);
  CHECK(SystemParametersInfoA(41, 340, ncm, 0) && w32_u32(ncm + 20) == 25);
  UINT window = traces;
  CHECK(SystemParametersInfoA(105, 5, 0, 2));
  CHECK(SystemParametersInfoA(104, 0, &value, 0) && value == 5);
  MSG msg;
  for (UINT i = 0; i < 16 && PeekMessageA(&msg, 0, 0, 0, 1); i++)
    DispatchMessageA(&msg);
  BOOL notified = 0;
  for (UINT i = window; i < traces; i++)
    notified |= trace[i].h == windows[0] && trace[i].m == 0x1a &&
                trace[i].wp == 105;
  CHECK(notified);
  CHECK(!SystemParametersInfoA(9999, 0, 0, 0) && GetLastError() == 120);
  printf("TRACE SystemParametersInfo setters: uiParam pvParam-value and "
         "structure conventions, metrics, SPIF_SENDCHANGE\n");
}

#define HOST_WEAK __attribute__((weak))
HOST_WEAK DWORD host_canvas(int, int, DWORD, DWORD *, uint32_t **);
HOST_WEAK void host_canvas_free(DWORD, DWORD);
HOST_WEAK DWORD host_mono(int, int, const uint8_t *);
HOST_WEAK DWORD host_color(int, int, const uint32_t *);
HOST_WEAK void host_gdi_delete(DWORD);
HOST_WEAK int host_bitmap(DWORD, int *, int *, int *, uint32_t *, int);
HOST_WEAK DWORD host_palette(DWORD, unsigned *);
HOST_WEAK BOOL host_animate(DWORD);
HOST_WEAK void host_gdi_exit(void);
struct seam_hooks {
  void (*deactivate)(void);
  void (*regrant)(void);
  void (*hold_user)(int);
  DWORD (*frames)(DWORD *, DWORD *, DWORD *);
  DWORD (*lease_token)(void);
  uintptr_t (*cover)(void);
  uint32_t (*pixel)(uintptr_t, int, int);
};
HOST_WEAK void host_ddraw_seam(uintptr_t, const struct seam_hooks *);
static HWND dd_window, dd_cover;
static void seam_deactivate(void) {
  UINT off[2] = {0, 0};
  struct u32_window *w = u32_window(dd_window);
  event(w, DESK_V3_ACTIVATE, off, 2);
  lease_event(w->wire, w->generation, 3, 320, 200, 8);
  u32_pump();
  discard();
}
static void seam_regrant(void) {
  UINT on[2] = {1, 1};
  struct u32_window *w = u32_window(dd_window);
  traces = 0;
  event(w, DESK_V3_ACTIVATE, on, 2);
  lease_event(w->wire, w->generation, 1, 320, 200, 8);
  u32_pump();
  discard();
  /* Activation offers the foreground palette before the focus arrives. */
  CHECK(trace_index(dd_window, WM_QUERYNEWPALETTE) <
        trace_index(dd_window, WM_SETFOCUS));
}
static void seam_hold(int on) {
  if (on)
    u32_lock();
  else
    u32_unlock(0);
}
static DWORD seam_frames(DWORD *token, DWORD *generation, DWORD *refused) {
  *token = frame_token;
  *generation = frame_generation;
  *refused = __atomic_load_n(&frames_refused, __ATOMIC_SEQ_CST);
  return __atomic_load_n(&frames_seen, __ATOMIC_SEQ_CST);
}
static DWORD seam_token(void) { return lease_token; }
static uintptr_t seam_cover(void) {
  dd_cover = CreateWindowExA(0, "USER fixture", "cover", WS_VISIBLE | WS_POPUP,
                             100, 50, 50, 50, 0, 0, 0, 0);
  ValidateRect(dd_cover, 0);
  return (uintptr_t)dd_cover;
}
static uint32_t seam_pixel(uintptr_t h, int x, int y) {
  struct u32_window *w = u32_window((HWND)h);
  return w32_u32((uint8_t *)w->surface.pixels + y * w->surface.pitch + x * 4) &
         0xffffff;
}
/* 3x2 test image, top row first: red green blue / white black yellow. */
static const uint32_t rgb6[6] = {0xff0000, 0x00ff00, 0x0000ff,
                                 0xffffff, 0x000000, 0xffff00};
static void dib_header(uint8_t *b, int height, UINT bpp, UINT compression,
                       UINT colors) {
  memset(b, 0, 40);
  w32_put32(b, 40);
  w32_put32(b + 4, 3);
  w32_put32(b + 8, (DWORD)height);
  w32_put16(b + 12, 1);
  w32_put16(b + 14, bpp);
  w32_put32(b + 16, compression);
  w32_put32(b + 32, colors);
}
static void clipboard_bitmap(const uint8_t *dib, DWORD n,
                             const uint32_t *expected) {
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard());
  HANDLE block = GlobalAlloc(2, n);
  memcpy(GlobalLock(block), dib, n);
  CHECK(SetClipboardData(CF_DIB, block) == block);
  HANDLE bitmap = GetClipboardData(CF_BITMAP);
  if (!expected) {
    CHECK(!bitmap && GetLastError() == 120);
  } else {
    int w = 0, h = 0, bpp = 0;
    uint32_t pixels[6];
    CHECK(bitmap && host_bitmap(U32_PTR(bitmap), &w, &h, &bpp, pixels, 6));
    CHECK(w == 3 && h == 2 && bpp == 32);
    for (UINT i = 0; i < 6; i++)
      if ((pixels[i] & 0xffffff) != expected[i]) {
        fprintf(stderr, "CF_BITMAP bpp=%u height=%d pixel %u: %06x != %06x\n",
                w32_u16(dib + 14), (int)w32_u32(dib + 8), i,
                pixels[i] & 0xffffff, expected[i]);
        CHECK(0);
      }
  }
  CHECK(CloseClipboard());
}
static void test_cf_bitmap(void) {
  uint8_t b[256];
  /* 32 bpp top-down BI_RGB. */
  dib_header(b, -2, 32, 0, 0);
  memcpy(b + 40, rgb6, 24);
  clipboard_bitmap(b, 64, rgb6);
  /* 24 bpp bottom-up, 12-byte padded rows (B, G, R). */
  dib_header(b, 2, 24, 0, 0);
  for (UINT row = 0; row < 2; row++)
    for (UINT x = 0; x < 3; x++) {
      uint32_t c = rgb6[(1 - row) * 3 + x];
      uint8_t *p = b + 40 + row * 12 + x * 3;
      p[0] = c & 255, p[1] = c >> 8 & 255, p[2] = c >> 16;
    }
  clipboard_bitmap(b, 64, rgb6);
  /* 16 bpp BI_RGB (5-5-5) bottom-up. */
  const uint16_t x555[6] = {0x7c00, 0x03e0, 0x001f, 0x7fff, 0, 0x7fe0};
  dib_header(b, 2, 16, 0, 0);
  for (UINT row = 0; row < 2; row++)
    for (UINT x = 0; x < 3; x++)
      w32_put16(b + 40 + row * 8 + x * 2, x555[(1 - row) * 3 + x]);
  clipboard_bitmap(b, 56, rgb6);
  /* 16 bpp BI_BITFIELDS (5-6-5) top-down, masks after the 40-byte header. */
  const uint16_t x565[6] = {0xf800, 0x07e0, 0x001f, 0xffff, 0, 0xffe0};
  dib_header(b, -2, 16, 3, 0);
  w32_put32(b + 40, 0xf800);
  w32_put32(b + 44, 0x07e0);
  w32_put32(b + 48, 0x001f);
  for (UINT i = 0; i < 6; i++)
    w32_put16(b + 52 + (i / 3) * 8 + (i % 3) * 2, x565[i]);
  clipboard_bitmap(b, 68, rgb6);
  /* 8 bpp bottom-up with a six-entry colour table (B, G, R, 0). */
  dib_header(b, 2, 8, 0, 6);
  for (UINT i = 0; i < 6; i++)
    w32_put32(b + 40 + i * 4, rgb6[i]);
  for (UINT row = 0; row < 2; row++)
    for (UINT x = 0; x < 3; x++)
      b[64 + row * 4 + x] = (uint8_t)((1 - row) * 3 + x);
  clipboard_bitmap(b, 72, rgb6);
  /* 4 bpp top-down, packed high nibble first. */
  dib_header(b, -2, 4, 0, 6);
  for (UINT i = 0; i < 6; i++)
    w32_put32(b + 40 + i * 4, rgb6[i]);
  memset(b + 64, 0, 8);
  b[64] = 0x01, b[65] = 0x20, b[68] = 0x34, b[69] = 0x50;
  clipboard_bitmap(b, 72, rgb6);
  /* 1 bpp bottom-up with a black/white table. */
  const uint32_t mono[6] = {0xffffff, 0, 0xffffff, 0, 0xffffff, 0};
  dib_header(b, 2, 1, 0, 2);
  w32_put32(b + 40, 0);
  w32_put32(b + 44, 0xffffff);
  memset(b + 48, 0, 8);
  b[48] = 0x40; /* bottom row: 0 1 0 */
  b[52] = 0xa0; /* top row: 1 0 1 */
  clipboard_bitmap(b, 56, mono);
  /* Truncated pixel data is refused, never read past the block. */
  dib_header(b, -2, 32, 0, 0);
  clipboard_bitmap(b, 60, 0);
  CHECK(OpenClipboard(windows[0]) && EmptyClipboard() && CloseClipboard());
  printf("TRACE CF_BITMAP from CF_DIB through gdi32: 32 24 16(555) "
         "16(565 bitfields) 8 4 1 bpp, both orientations, truncation\n");
}
static void icon_case(HICON icon, HBRUSH brush, UINT flags, DWORD background,
                      const uint32_t expected[4]) {
  DWORD dc = 0;
  uint32_t *bits = 0;
  DWORD canvas = host_canvas(32, 32, background, &dc, &bits);
  CHECK(canvas);
  CHECK(DrawIconEx((HDC)(uintptr_t)dc, 0, 0, icon, 0, 0, 0, brush, flags));
  /* Quadrants: top-left, top-right (mask 1), bottom-left, bottom-right. */
  const UINT at[4] = {2 * 32 + 2, 2 * 32 + 20, 20 * 32 + 2, 20 * 32 + 20};
  for (UINT i = 0; i < 4; i++)
    CHECK((bits[at[i]] & 0xffffff) == expected[i]);
  host_canvas_free(dc, canvas);
}
static void test_draw_icon(void) {
  uint32_t color[32 * 32];
  uint8_t mask[32 * 4];
  for (UINT i = 0; i < 32 * 32; i++)
    color[i] = i % 32 < 16 ? 0xff0000 : 0x00ff00;
  memset(mask, 0xff, 16 * 4);
  memset(mask + 16 * 4, 0, 16 * 4);
  DWORD cb = host_color(32, 32, color), mb = host_mono(32, 32, mask);
  CHECK(cb && mb);
  ICONINFO info = {1, 0, 0, (HANDLE)(uintptr_t)mb, (HANDLE)(uintptr_t)cb};
  HICON icon = CreateIconIndirect(&info);
  CHECK(icon);
  /* DI_NORMAL: destination AND mask, then XOR image (Microsoft DrawIconEx). */
  icon_case(icon, 0, 3, 0x0000ff,
            (const uint32_t[4]){0xff00ff, 0x00ffff, 0xff0000, 0x00ff00});
  /* DI_IMAGE alone copies the image; DI_MASK alone copies the mask
   * (1 = background colour white, 0 = text colour black). */
  icon_case(icon, 0, 2, 0x0000ff,
            (const uint32_t[4]){0xff0000, 0x00ff00, 0xff0000, 0x00ff00});
  icon_case(icon, 0, 1, 0x0000ff,
            (const uint32_t[4]){0xffffff, 0xffffff, 0, 0});
  /* hbrFlickerFreeDraw: background brush first (COLOR_WINDOW, white). */
  icon_case(icon, (HBRUSH)(uintptr_t)6, 3 | 8, 0x0000ff,
            (const uint32_t[4]){0x00ffff, 0xff00ff, 0xff0000, 0x00ff00});
  CHECK(!DrawIconEx(0, 0, 0, icon, 0, 0, 0, 0, 0) && GetLastError() == 87);
  CHECK(!DrawIconEx(0, 0, 0, icon, 0, 0, 0, 0, 0x13) && GetLastError() == 87);
  CHECK(!DrawIconEx(0, 0, 0, icon, -1, 0, 0, 0, 3) && GetLastError() == 87);
  CHECK(DestroyIcon(icon));
  host_gdi_delete(cb);
  host_gdi_delete(mb);
  printf("TRACE DrawIconEx DI_NORMAL DI_IMAGE DI_MASK flicker-free brush, "
         "flag validation\n");
}
static BOOL take(UINT message, HWND source) {
  MSG m;
  BOOL found = 0;
  while (PeekMessageA(&m, 0, message, message, 1))
    found |= m.wParam == (WPARAM)source;
  return found;
}
static void test_palette(void) {
  HDC dc = GetDC(windows[0]);
  CHECK(dc);
  unsigned changed = 0;
  DWORD palette = host_palette(U32_PTR(dc), &changed);
  CHECK(palette);
  /* Delivered at the next USER entry, never from inside gdi32's lock. */
  CHECK(take(WM_PALETTEISCHANGING, windows[0]) &&
        take(WM_PALETTECHANGED, windows[0]));
  ui_kernel_switch(1);
  CHECK(take(WM_PALETTECHANGED, windows[0]));
  ui_kernel_switch(0);
  CHECK(host_animate(palette));
  MSG m;
  CHECK(PeekMessageA(&m, 0, WM_PALETTECHANGED, WM_PALETTECHANGED, 1));
  CHECK(ReleaseDC(windows[0], dc));
  host_gdi_delete(palette);
  discard();
  ui_kernel_switch(1);
  discard();
  ui_kernel_switch(0);
  printf("TRACE palette callbacks: WM_PALETTEISCHANGING WM_PALETTECHANGED to "
         "every top-level window, AnimatePalette\n");
}
extern int host_pixel_format(DWORD,int);
extern BOOL W32API w32_user_gl_target(DWORD,DWORD *);
extern BOOL W32API w32_user_gl_damage(DWORD);
static void test_production_gdi(void) {
  CHECK(host_gdi_proc && host_ddraw_seam);
  /* An old v1 table ends before the v2 tail. ASan guards its exact size. */
  _Alignas(struct u32_gdi) unsigned char v1[__builtin_offsetof(struct u32_gdi,window_pixel_format)];
  memcpy(v1,&test_gdi,sizeof(v1));
  u32_set_gdi((const struct u32_gdi *)v1);
  HDC v1_dc=GetDC(windows[0]);
  CHECK(v1_dc && ReleaseDC(windows[0],v1_dc));
  u32_set_gdi(&test_gdi);
  host_modules = 1;
  real_gdi = 1;
  u32_gdi = 0; /* resolve the production gdi32 through GetProcAddress */
  u32_gdi_ensure();
  CHECK(u32_gdi && u32_gdi != &test_gdi);
  /* The window procedure paints through the production engine. */
  struct u32_window *w = u32_window(windows[0]);
  CHECK(InvalidateRect(windows[0], 0, 1) && UpdateWindow(windows[0]));
  CHECK(w32_u32((uint8_t *)w->surface.pixels + 5 * w->surface.pitch + 5 * 4) ==
        0xc0c0c0);
  HDC first_dc=GetDC(windows[0]), simultaneous_dc=GetDC(windows[0]);
  CHECK(first_dc && simultaneous_dc);
  CHECK(!host_pixel_format(U32_PTR(first_dc),0));
  CHECK(host_pixel_format(U32_PTR(first_dc),1));
  CHECK(host_pixel_format(U32_PTR(simultaneous_dc),0)==1);
  CHECK(!host_pixel_format(U32_PTR(simultaneous_dc),2));
  CHECK(ReleaseDC(windows[0],first_dc) && ReleaseDC(windows[0],simultaneous_dc));
  first_dc=GetDC(windows[0]);
  CHECK(first_dc && host_pixel_format(U32_PTR(first_dc),0)==1);
  CHECK(!host_pixel_format(U32_PTR(first_dc),1));
  DWORD target[4]={0};
  CHECK(w32_user_gl_target(U32_PTR(first_dc),target)==1 && target[2]==U32_PTR(windows[0]));
  ui_kernel_switch(1);
  CHECK(w32_user_gl_target(U32_PTR(first_dc),target)==-1 && target[2]==U32_PTR(windows[0]));
  ui_kernel_switch(0);
  unsigned presents=sent_count(DESK_V3_PRESENT);
  CHECK(w32_user_gl_damage(U32_PTR(first_dc)) && sent_count(DESK_V3_PRESENT)==presents+1);
  reject_opcode=DESK_V3_PRESENT;
  CHECK(!w32_user_gl_damage(U32_PTR(first_dc)));
  CHECK(ReleaseDC(windows[0],first_dc));
  puts("TRACE window pixel format survives simultaneous and reacquired DCs");
  test_cf_bitmap();
  test_draw_icon();
  test_palette();
  dd_window = CreateWindowExA(0, "USER fixture", "dd", WS_VISIBLE | WS_POPUP,
                              0, 0, 320, 200, 0, 0, 0, 0);
  CHECK(dd_window);
  ValidateRect(dd_window, 0);
  const struct seam_hooks hooks = {seam_deactivate, seam_regrant,
                                   seam_hold,       seam_frames,
                                   seam_token,      seam_cover,
                                   seam_pixel};
  host_ddraw_seam((uintptr_t)dd_window, &hooks);
  CHECK(DestroyWindow(dd_cover) && DestroyWindow(dd_window));
  discard();
  u32_set_gdi(&test_gdi);
  real_gdi = 0;
  host_modules = 0;
  host_gdi_exit();
}
#ifndef U32_TEST_EXTERNAL_MODULES
#include "../shell/clients.inc"
#endif
static void run(void) {
  puts("STEP init");
  ui_kernel_init();
  puts("STEP server");
  struct wr_io io;
  char dir[] = "build/host/win32/user32/state-XXXXXX";
  CHECK(mkdtemp(dir));
  wr_posix_io(&io, dir);
  CHECK(wr_init(&server, &io, 0));
  struct u32_backend backend = {ui_kernel_call, w32r_gate,     w32r_exchange,
                                ui_kernel_tid,  ui_kernel_pid, ui_kernel_ticks,
                                surface,        drop,          ui_kernel_pause};
  u32_set_backend(&backend);
  u32_set_gdi(&test_gdi);
  WNDCLASSA cls = {.style = 8,
                   .lpfnWndProc = procedure,
                   .cbClsExtra = 8,
                   .cbWndExtra = 8,
                   .lpszClassName = "USER fixture"};
  puts("STEP classes");
  CHECK(RegisterClassA(&cls));
  CHECK(!RegisterClassA(&cls) && GetLastError() == 1410);
  for (UINT i = 0; i < 3; i++) {
    printf("STEP window %u\n", i);
    ui_kernel_switch(i);
    windows[i] =
        CreateWindowExA(0, "USER fixture", "fixture", WS_VISIBLE | WS_CAPTION,
                        10 + i * 30, 10, 80, 80, 0, 0, 0, 0);
    CHECK(windows[i]);
    ValidateRect(windows[i], 0);
  }
  ui_kernel_switch(0);
  child = CreateWindowExA(0, "USER fixture", "child", WS_CHILD | WS_VISIBLE, 5,
                          5, 20, 20, windows[0], (HANDLE)5, 0, 0);
  CHECK(child);
  ValidateRect(child, 0);
  CHECK(SetPropA(child, "property", (HANDLE)42));
  CHECK(GetPropA(child, "property") == (HANDLE)42);
  CHECK(SetWindowLongA(child, 0, 123) == 0 && GetWindowLongA(child, 0) == 123);
  CHECK(GetParent(child) == windows[0]);
  CHECK(GetWindow(windows[0], 5) == child);
  CHECK(!UnregisterClassA("USER fixture", 0) && GetLastError() == 1412);
  puts("STEP send");
  test_send();
  puts("STEP queues");
  test_queues();
  puts("STEP input");
  test_input();
  puts("STEP paint");
  test_paint();
  puts("STEP clipboard");
  test_clipboard();
  test_extras();
  test_spi();
  test_order();
  test_arrange();
  test_zoom_limits();
  test_display();
  test_production_gdi();
  traces = 0;
  CHECK(DestroyWindow(windows[0]));
  UINT child_destroy = 999, parent_destroy = 999;
  for (UINT i = 0; i < traces; i++)
    if (trace[i].m == WM_DESTROY) {
      if (trace[i].h == child)
        child_destroy = i;
      if (trace[i].h == windows[0])
        parent_destroy = i;
    }
  CHECK(child_destroy < parent_destroy && !IsWindow(child));
  printf("TRACE destruction: child WM_DESTROY/WM_NCDESTROY parent "
         "WM_DESTROY/WM_NCDESTROY\n");
  for (UINT i = 1; i < 3; i++) {
    ui_kernel_switch(i);
    CHECK(DestroyWindow(windows[i]));
  }
  ui_kernel_switch(0);
  test_desktop_close();
  test_lifecycle();
  CHECK(UnregisterClassA("USER fixture", 0));
  #ifndef U32_TEST_EXTERNAL_MODULES
  shell_clients(dir);
  #endif
  clip_destroy(&fixture_clip_store);
  wr_destroy(&server);
  for (unsigned i = 0; i < 2; i++) {
    char path[256];
    snprintf(path, sizeof(path), "%s/SERVICE.CRS.%u", dir, i);
    unlink(path);
  }
  rmdir(dir);
  CHECK(!allocations);
  ui_kernel_finish();
  puts("PASS USER windows messages input paint clipboard");
}
int main(void) {
  setbuf(stdout, 0);
  setbuf(stderr, 0);
  void *stack = mmap(0, 4 * 1024 * 1024, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  assert(stack != MAP_FAILED);
  ucontext_t current_context, low;
  getcontext(&low);
  low.uc_stack.ss_sp = stack;
  low.uc_stack.ss_size = 4 * 1024 * 1024;
  low.uc_link = &current_context;
  makecontext(&low, run, 0);
  swapcontext(&current_context, &low);
  munmap(stack, 4 * 1024 * 1024);
  return 0;
}
