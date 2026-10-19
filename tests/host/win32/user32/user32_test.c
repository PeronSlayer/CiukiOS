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
HANDLE W32API GetProcessHeap(void) { return (HANDLE)1; }
static unsigned allocations;
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
HANDLE W32API LoadLibraryA(const char *s) {
  (void)s;
  return 0;
}
HANDLE W32API GetModuleHandleA(const char *s) {
  (void)s;
  return 0;
}
void *W32API GetProcAddress(HANDLE h, const char *s) {
  (void)h;
  (void)s;
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
DWORD w32r_exchange(const void *r, DWORD n, void *out, DWORD cap, DWORD *used) {
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
DWORD w32r_gate(DWORD call, DWORD a, DWORD b, DWORD c) {
  (void)a;
  (void)c;
  if (call == CIUKI_SYS_CHANNEL_SEND) {
    CHECK(outgoing_count < 2048);
    struct ciuki_message *sent = (void *)(uintptr_t)b;
    outgoing[outgoing_count++] = *sent;
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
  if (call == CIUKI_SYS_CLOSE)
    return 0;
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
static LRESULT W32API procedure(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  CHECK(traces < 2048);
  trace[traces].h = h;
  trace[traces].m = m;
  trace[traces].wp = wp;
  trace[traces++].lp = lp;
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
  if (m == WM_PAINT) {
    PAINTSTRUCT ps;
    CHECK(BeginPaint(h, &ps));
    RECT r;
    GetClientRect(h, &r);
    CHECK(fill(ps.hdc, &r, (HBRUSH)1));
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
  CHECK(UnregisterClassA("USER fixture", 0));
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
