/* Linux transport/scheduler adapter. All USER and queue policy is production.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "../../../../sdk/win32/user32/internal.h"
#include <w32/controls.h>
#include "../../../../apps/desktop/help_request.h"
#include "../../../../apps/desktop/sounds.h"
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
    ui_kernel_schedule(void (*)(void)), ui_kernel_pause(uint32_t),
    ui_kernel_refuse_send(uint32_t);
unsigned ui_kernel_current(void);
uint32_t ui_kernel_tid(void), ui_kernel_pid(void), ui_kernel_ticks(void);
int32_t ui_kernel_call(void *, uint32_t);
uint32_t ui_kernel_child_handle(uint32_t);
void ui_kernel_close_handle(uint32_t), ui_kernel_child_wait(uint32_t, uint32_t);
DWORD W32API WaitForInputIdle(HANDLE, DWORD);
static DWORD errors[3];
DWORD W32API GetLastError(void) { return errors[ui_kernel_current()]; }
void W32API SetLastError(DWORD n) { errors[ui_kernel_current()] = n; }
DWORD W32API GetCurrentThreadId(void) { return ui_kernel_tid(); }
DWORD W32API GetCurrentProcessId(void) { return ui_kernel_pid(); }
DWORD W32API GetTickCount(void) { return ui_kernel_ticks(); }
static int g23_files_enabled,g23_temporary,g23_flush_failure,g23_rename_failure;
static char g23_config[34000],g23_pending[34000];
BOOL host_appearance_write(void *h,const void *data,DWORD bytes,DWORD *written) {
  if(!g23_files_enabled || (uintptr_t)h!=0x732 || bytes>=sizeof(g23_pending)) return 0;
  w32_copy(g23_pending,data,bytes);g23_pending[bytes]=0;if(written)*written=bytes;return 1;
}
BOOL host_appearance_flush(void *h) {
  return g23_files_enabled && (uintptr_t)h==0x732 && !g23_flush_failure;
}
#ifndef U32_TEST_EXTERNAL_MODULES
/* tests/host/win32/dialogs supplies file, module and resource services. */
static int g18_snapshot_fds[8];
static unsigned g18_snapshot_closes,g18_snapshot_maps,g18_snapshot_unmaps;
static int g18_snapshot(const char *);
HANDLE W32API CreateFileA(const char *name, DWORD access, DWORD share,
                          SECURITY_ATTRIBUTES *security, DWORD creation,
                          DWORD flags, HANDLE template) {
  if(g23_files_enabled && !strcmp(name,"C:\\system\\settings\\desktop.cfg")) return (HANDLE)0x731;
  if(g23_files_enabled && !strcmp(name,"C:\\system\\settings\\desktop.cfg.appearance.tmp")) {
    if(creation!=1 || g23_temporary) {SetLastError(183);return K32_INVALID;}
    g23_temporary=1;g23_pending[0]=0;return (HANDLE)0x732;
  }
  (void)name;
  (void)access;
  (void)share;
  (void)security;
  (void)creation;
  (void)flags;
  (void)template;
  (void)name;
  return K32_INVALID;
}
DWORD W32API GetFileSize(HANDLE h,DWORD *high) {
  if(high)*high=0;
  (void)h;return ~0u;
}
BOOL W32API ReadFile(HANDLE h, void *data, DWORD bytes, DWORD *count,
                     OVERLAPPED *o) {
  if(g23_files_enabled && (uintptr_t)h==0x731) {
    DWORD n=(DWORD)strlen(g23_config); if(n>bytes)n=bytes;w32_copy(data,g23_config,n);*count=n;return 1;
  }
  (void)h;
  (void)data;
  (void)bytes;
  (void)count;
  (void)o;
  (void)h;(void)data;(void)bytes;(void)count;
  return 0;
}
BOOL W32API CloseHandle(HANDLE h) {
  (void)h;
  return 1;
}
/* Input reuses the USER fixture without gdi_host.c's file adapters. */
__attribute__((weak)) BOOL W32API WriteFile(HANDLE h, const void *data,
                                            DWORD bytes, DWORD *written,
                                            OVERLAPPED *o) {
  (void)o;
  return host_appearance_write(h, data, bytes, written);
}
__attribute__((weak)) BOOL W32API FlushFileBuffers(HANDLE h) {
  return host_appearance_flush(h);
}
__attribute__((weak)) BOOL W32API DeleteFileA(const char *name) {
  if (g23_files_enabled &&
      !strcmp(name, "C:\\system\\settings\\desktop.cfg.appearance.tmp") &&
      g23_temporary) {
    g23_temporary = 0;
    return 1;
  }
  SetLastError(2);
  return 0;
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
#ifndef U32_TEST_EXTERNAL_MODULES
static BOOL host_help;
#endif
#define HOST_GDI ((HANDLE)(uintptr_t)0x6d000000)
#define HOST_USER ((HANDLE)(uintptr_t)0x6e000000)
#define HOST_KERNEL ((HANDLE)(uintptr_t)0x6f000000)
#ifndef U32_TEST_EXTERNAL_MODULES
static DWORD W32API help_fullpath(const char *input,DWORD size,char *out,char **part)
{
    (void)part;
    const char *prefix=input[1]==':' ? "" : "C:\\Help\\";
    unsigned n=(unsigned)(strlen(prefix)+strlen(input));
    if (size<=n) return n+1;
    snprintf(out,size,"%s%s",prefix,input); return n;
}
static DWORD W32API help_attributes(const char *path)
{ if (strstr(path,"missing")) { SetLastError(2); return UINT32_MAX; } return 0x20; }
#endif
/* Weak: tests/host/win32/input reuses this harness without gdi32/ddraw. */
__attribute__((weak)) void *host_gdi_proc(const char *);
#ifndef U32_TEST_EXTERNAL_MODULES
HANDLE W32API GetModuleHandleA(const char *s) {
  if (host_help && s && !strcmp(s,"kernel32.dll")) return HOST_KERNEL;
  if (!host_modules || !s)
    return 0;
  return !strcmp(s, "gdi32.dll") ? HOST_GDI
         : !strcmp(s, "user32.dll") ? HOST_USER
                                    : 0;
}
#define USER_EXPORT(name) {#name, (void *)name}
void *W32API GetProcAddress(HANDLE h, const char *s) {
  if (h==HOST_KERNEL) return !strcmp(s,"GetFullPathNameA") ? (void *)help_fullpath : !strcmp(s,"GetFileAttributesA") ? (void *)help_attributes : 0;
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
#ifndef U32_TEST_EXTERNAL_MODULES
static int g18_snapshot(const char *text) {
  unsigned bytes=(unsigned)strlen(text);
  for(unsigned k=0;k<8;k++)if(!g18_snapshot_fds[k]) {
    struct u32_surface s;
    CHECK(surface(1024,(bytes+4+4095)/4096,&s));
    w32_put32(s.pixels,bytes);memcpy((uint8_t *)s.pixels+4,text,bytes);
    g18_snapshot_fds[k]=s.fd;return s.fd;
  }
  CHECK(0);return -1;
}
static void g18_snapshot_length(int fd,DWORD length) {
  for(unsigned k=0;k<64;k++)if(surfaces[k].fd==fd) {
    w32_put32(surfaces[k].s.pixels,length);return;
  }
  CHECK(0);
}
#endif
static struct ciuki_message outgoing[2048], incoming[256];
static UINT outgoing_count, in_head, in_tail, reject_opcode;
static DWORD lease_token = 0x40, lease_generation = 0x80, lease_serial;
static BOOL lease_eio;
static DWORD frames_seen, frames_refused, frame_token, frame_generation;
static uint64_t fake_ns;
/* q3exec3: a test may add the desktop's own events around a request's
 * RESULT (a PING queued before the desktop read a DESK_CLOSE, the
 * DESK_CLOSED it sends after the RESULT). */
static void (*desk_reply_hook)(const struct ciuki_message *sent, BOOL after_result);
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
static UINT shared_input[DESK_INPUT_VALUES]={0,1,31,530,0,0,3,0,10,500,1};
static UINT saved_input[DESK_INPUT_VALUES]={0,1,31,530,0,0,3,0,10,500,1};
static struct ciuki_screen_control saver_policy={.version=1,.size=sizeof(saver_policy),.timeout=600,.saver=1};
#include "g5_clipboard_fixture.h"
DWORD w32r_gate(DWORD call, DWORD a, DWORD b, DWORD c) {
  if(call==CIUKI_SYS_RENAME && g23_files_enabled) {
    CHECK(!strcmp((const char *)(uintptr_t)a,"/system/settings/desktop.cfg.appearance.tmp"));
    CHECK(!strcmp((const char *)(uintptr_t)b,"/system/settings/desktop.cfg"));
    if(g23_rename_failure) return (DWORD)-5;
    strcpy(g23_config,g23_pending);g23_temporary=0;return 0;
  }
  (void)a;
  (void)c;
#ifndef U32_TEST_EXTERNAL_MODULES
  for(unsigned k=0;k<8;k++)if(g18_snapshot_fds[k])
    for(unsigned j=0;j<64;j++)if(surfaces[j].fd==g18_snapshot_fds[k]) {
      struct u32_surface *s=&surfaces[j].s;
      if(a==(DWORD)s->fd && call==CIUKI_SYS_SURFACE_MAP) {
        CHECK(b==1 && !c);g18_snapshot_maps++;return U32_PTR(s->pixels);
      }
      if(a==U32_PTR(s->pixels) && call==CIUKI_SYS_MUNMAP) {
        CHECK(b==s->pitch*s->height && !c);g18_snapshot_unmaps++;return 0;
      }
      if(a==(DWORD)s->fd && call==CIUKI_SYS_CLOSE) {
        drop(s);g18_snapshot_fds[k]=0;g18_snapshot_closes++;return 0;
      }
    }
#endif
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
    if (desk_reply_hook)
      desk_reply_hook(sent, 0);
    if(sent->data[1]==DESK_INPUT_SETTINGS) {
      UINT key=desk_get32(sent->data+16),value=desk_get32(sent->data+20),flags=desk_get32(sent->data+24);
      if(key && key<=DESK_INPUT_VALUES){shared_input[key-1]=value;if(flags&1)saved_input[key-1]=value;}
      CHECK(in_tail-in_head<256);
      struct ciuki_message *state=&incoming[in_tail++%256];
      desk_v3_message(state,DESK_INPUT_STATE,DESK_INPUT_VALUES*4+8,desk_get32(sent->data+4),desk_get32(sent->data+8),desk_get32(sent->data+12));
      for(UINT k=0;k<DESK_INPUT_VALUES;k++)desk_put32(state->data+16+k*4,shared_input[k]);
      desk_put32(state->data+16+DESK_INPUT_VALUES*4,key);desk_put32(state->data+20+DESK_INPUT_VALUES*4,flags);
    }
    if (sent->data[1]==DESK_V3_CLIPBOARD) { fixture_clipboard(sent); return 0; }
    if (sent->data[1] == DESK_V3_LEASE && sent->data[1] != reject_opcode && !lease_eio) {
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
                 sent->data[1] == DESK_V3_LEASE && lease_eio ? (DWORD)-5 :
                 sent->data[1] == reject_opcode ? (DWORD)-12 : 0);
      if (sent->data[1] == reject_opcode)
        reject_opcode = 0;
    }
    if (desk_reply_hook)
      desk_reply_hook(sent, 1);
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
/* q3exec4 review: the v3 tail replaces a busy DC's visible region. */
static BOOL visible(HDC h, const RECT *r, UINT n) {
  struct test_dc *dc = h;
  CHECK(n <= 64);
  dc->clips = n;
  if (n)
    memcpy(dc->clip, r, n * sizeof(*r));
  return 1;
}
static const struct u32_gdi test_gdi = {
    .version = 3, .acquire = acquire, .release = release, .fill = fill,
    .visible = visible};
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
static int ignore_sc_close; /* T23 c42f556e: the Quake III installer background ignores SC_CLOSE */
static unsigned g5_render_count,g5_all_count;
static HWND g5_all_window;
static HANDLE g5b_bitmap;
static unsigned g5b_bitmap_renders,g5b_display_paints,g5b_display_sizes,g5b_display_scrolls,g5b_locale_renders;
static BOOL g5b_display;

static void (*quake_hook)(HWND h, UINT m);
static LRESULT W32API procedure(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (quake_hook && (m==WM_KEYDOWN || m==WM_KEYUP || m==WM_CHAR || m==WM_SIZE)) quake_hook(h,m);
  if (m==WM_RENDERFORMAT && wp==CF_LOCALE) {
    HANDLE locale=GlobalAlloc(2,4); CHECK(locale); w32_put32(GlobalLock(locale),0x409);
    CHECK(SetClipboardData(CF_LOCALE,locale)==locale); g5b_locale_renders++; return 0;
  }
  if (m==WM_RENDERFORMAT && wp==CF_BITMAP && g5b_bitmap) {
    CHECK(SetClipboardData(CF_BITMAP,g5b_bitmap)==g5b_bitmap); g5b_bitmap_renders++; return 0;
  }
  if (g5b_display) {
    if (m==WM_ASKCBFORMATNAME) { CHECK(wp>=10); memcpy((void *)lp,"G5b owner",10); return 0; }
    if (m==WM_SIZECLIPBOARD) { CHECK(GlobalSize((HANDLE)lp)==sizeof(RECT)); g5b_display_sizes++; return 0; }
    if (m==WM_VSCROLLCLIPBOARD || m==WM_HSCROLLCLIPBOARD) { g5b_display_scrolls++; return 0; }
    if (m==WM_PAINTCLIPBOARD) {
      PAINTSTRUCT *p=GlobalLock((HANDLE)lp); CHECK(p && p->hdc);
      CHECK(FillRect(p->hdc,&p->rcPaint,(HBRUSH)6)); GlobalUnlock((HANDLE)lp); g5b_display_paints++; return 0;
    }
  }
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
  if (m == WM_SYSCOMMAND && (wp & 0xfff0) == SC_CLOSE && ignore_sc_close)
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
static void g12_input_event(struct u32_window *w,UINT type,UINT code,UINT value)
{
  UINT data[10]={0,0,0,type,1000000000,0,code,value,0,0};event(w,DESK_KEY,data,10);
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
/* Review 3, finding 5: the desktop admits a program's save prompt from the
 * reserved prompt slot only when its CREATE packet already says modal and
 * names the owner; a later PLACE is too late. DialogBox must send it at
 * creation, a modeless CreateDialog must not. */
static intptr_t W32API end_at_init(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  (void)wp; (void)lp;
  if (m == WM_INITDIALOG) { EndDialog(h, 7); return 1; }
  return 0;
}
/* The template header and its three empty arrays (menu, class, title). */
static const struct { DLGTEMPLATE t; uint16_t menu, klass, title; } prompt_template = {   /* no padding: the WORDs follow the 18-byte header */
    .t = {.style = WS_POPUP | WS_CAPTION | WS_VISIBLE | DS_MODALFRAME, .cdit = 0, .x = 10, .y = 10, .cx = 120, .cy = 60}};
/* Review 4, finding 6: windows created while a modal dialog is being set up
 * (inside its WM_INITDIALOG) are not modal themselves. */
static HWND nested_window, nested_dialog;
static intptr_t W32API nest_at_init(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  (void)wp; (void)lp;
  if (m == WM_INITDIALOG) {
    nested_window = CreateWindowExA(0, "USER fixture", "nested", WS_VISIBLE | WS_CAPTION, 50, 50, 80, 60, h, 0, 0, 0);
    nested_dialog = CreateDialogIndirectParamA(0, &prompt_template.t, h, end_at_init, 0);
    EndDialog(h, 9);
    return 1;
  }
  return 0;
}
static int create_style_after(UINT from, UINT *style, UINT *owner) {
  for (UINT i = from; i < outgoing_count; i++)
    if (outgoing[i].data[0] == 3 && outgoing[i].data[1] == DESK_CREATE_WINDOW) {
      *style = desk_get32(outgoing[i].data + 16); *owner = desk_get32(outgoing[i].data + 20);
      return 1;
    }
  return 0;
}
static void test_modal_prompt_wire(void) {
  HWND owner = CreateWindowExA(0, "USER fixture", "program", WS_VISIBLE | WS_CAPTION, 40, 40, 200, 120, 0, 0, 0, 0);
  CHECK(owner);
  struct u32_window *ow = u32_window(owner);
  CHECK(ow && ow->wire);
  const DLGTEMPLATE *prompt = &prompt_template.t;
  UINT before = outgoing_count, style = 0, wire_owner = 0;
  CHECK(DialogBoxIndirectParamA(0, prompt, owner, end_at_init, 0) == 7);
  CHECK(create_style_after(before, &style, &wire_owner));
  CHECK((style & DESK_V3_MODAL) && wire_owner == ow->wire);
  before = outgoing_count;
  HWND modeless = CreateDialogIndirectParamA(0, prompt, owner, end_at_init, 0);
  CHECK(modeless && create_style_after(before, &style, &wire_owner));
  CHECK(!(style & DESK_V3_MODAL) && wire_owner == ow->wire);
  DestroyWindow(modeless);
  /* Nested creations during the modal dialog's WM_INITDIALOG: exactly one
   * CREATE packet is modal (the dialog's), the nested window's and the
   * nested modeless dialog's are not, and an ordinary owned window created
   * afterwards is not either. */
  before = outgoing_count;
  CHECK(DialogBoxIndirectParamA(0, prompt, owner, nest_at_init, 0) == 9);
  UINT modal_creates = 0, plain_creates = 0;
  for (UINT i = before; i < outgoing_count; i++)
    if (outgoing[i].data[0] == 3 && outgoing[i].data[1] == DESK_CREATE_WINDOW) {
      if (desk_get32(outgoing[i].data + 16) & DESK_V3_MODAL) modal_creates++; else plain_creates++;
    }
  CHECK(modal_creates == 1 && plain_creates == 2);
  CHECK(nested_window && nested_dialog && !IsWindow(nested_window) && !IsWindow(nested_dialog));   /* owned by the ended dialog */
  before = outgoing_count;
  HWND plain = CreateWindowExA(0, "USER fixture", "plain owned", WS_VISIBLE | WS_CAPTION, 60, 60, 80, 60, owner, 0, 0, 0);
  CHECK(plain && create_style_after(before, &style, &wire_owner) && !(style & DESK_V3_MODAL) && !(GetWindowLongA(plain, GWL_EXSTYLE) & U32_EX_MODAL_PROMPT));
  DestroyWindow(plain);
  DestroyWindow(owner);
  puts("PASS T23 modal prompt wire: DialogBox creates its desktop window modal and owned, CreateDialog and nested creations do not");
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
  /* T23 c42f556e: the close request is Windows' close button, WM_SYSCOMMAND
   * SC_CLOSE. The Quake III installer's background grays Close in its system
   * menu and ignores SC_CLOSE: it must survive (one decline, no WM_CLOSE);
   * a direct WM_CLOSE reached DefWindowProc and destroyed it. */
  h = CreateWindowExA(0, "USER fixture", "VISE background", WS_VISIBLE | WS_CAPTION | 0x80000u /* WS_SYSMENU */,
                      40, 40, 60, 60, 0, 0, 0, 0);
  CHECK(h);
  ValidateRect(h, 0);
  w = u32_window(h);
  CHECK(w && w->wire);
  wire = w->wire;
  ignore_sc_close = 1;
  before = outgoing_count;
  UINT first = traces;
  event(w, DESK_CLOSE_REQUEST, 0, 0);
  for (UINT i = 0; i < 100 && PeekMessageA(&msg, 0, 0, 0, 1); i++)
    DispatchMessageA(&msg);
  BOOL syscommand = 0, closed = 0;
  for (UINT i = first; i < traces; i++) {
    if (trace[i].h == h && trace[i].m == WM_SYSCOMMAND && (trace[i].wp & 0xfff0) == SC_CLOSE) syscommand = 1;
    if (trace[i].h == h && trace[i].m == WM_CLOSE) closed = 1;
  }
  CHECK(IsWindow(h) && syscommand && !closed && close_acks(before, wire) == 1 && !w->desktop_close);
  ignore_sc_close = 0;
  /* Close grayed in the system menu: declined at once, nothing dispatched. */
  CHECK(EnableMenuItem(GetSystemMenu(h, 0), SC_CLOSE, MF_BYCOMMAND | MF_GRAYED) != (UINT)-1);
  before = outgoing_count;
  first = traces;
  event(w, DESK_CLOSE_REQUEST, 0, 0);
  for (UINT i = 0; i < 100 && PeekMessageA(&msg, 0, 0, 0, 1); i++)
    DispatchMessageA(&msg);
  for (UINT i = first; i < traces; i++)
    CHECK(!(trace[i].h == h && (trace[i].m == WM_CLOSE || trace[i].m == WM_SYSCOMMAND)));
  CHECK(IsWindow(h) && close_acks(before, wire) == 1 && !w->desktop_close);
  /* Enabled again, the close button closes it through DefWindowProc. */
  CHECK(EnableMenuItem(GetSystemMenu(h, 0), SC_CLOSE, MF_BYCOMMAND) != (UINT)-1);
  before = outgoing_count;
  event(w, DESK_CLOSE_REQUEST, 0, 0);
  for (UINT i = 0; i < 100 && PeekMessageA(&msg, 0, 0, 0, 1); i++)
    DispatchMessageA(&msg);
  CHECK(!IsWindow(h) && !close_acks(before, wire));
  puts("PASS T23 desktop close is SC_CLOSE: an SC_CLOSE-ignoring window survives and declines, grayed Close declines at once, enabled Close destroys");
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
/* Quake III Arena 1.11 console (QEMU, 2026-10-26): "map q3dm17" arrived as
 * "ma 3m7", every other keystroke lost at any typing speed. Its pump is
 * PeekMessage(PM_NOREMOVE), GetMessage, TranslateMessage, DispatchMessage
 * per message; the console text comes from WM_CHAR. Each key is a KEY down,
 * its TEXT and a KEY up with its own millisecond stamp. */
static void q3_event(struct u32_window *w, UINT type, UINT code, UINT value, UINT ms)
{
  UINT data[10]={0,0,0,type,ms*1000000u,0,code,value,0,0};event(w,DESK_KEY,data,10);
}
static void q3_key(struct u32_window *w, char c, UINT ms)
{
  static const UINT scan[128]={['m']=0x32,['a']=0x1e,['p']=0x19,[' ']=0x39,['q']=0x10,['3']=0x04,['d']=0x20,['1']=0x02,['7']=0x08};
  q3_event(w,CIUKI_INPUT_KEY,scan[(unsigned char)c],1,ms);
  q3_event(w,CIUKI_INPUT_TEXT,(unsigned char)c,0,ms);
  q3_event(w,CIUKI_INPUT_KEY,scan[(unsigned char)c],0,ms+40);
}
/* Where the next key's three events reach the client, relative to Quake's
 * loop: 0 before PeekMessage (one key per pump), 1 all keys before one pump
 * (a slow software-GL frame), 2 between PeekMessage and GetMessage, 3
 * between GetMessage and TranslateMessage (TranslateMessage drains the
 * desktop channel itself), 4 between TranslateMessage and DispatchMessage,
 * 5 inside the window procedure during a SetCursorPos round trip (Quake's
 * mouse code recentres the cursor; the wire request pumps the channel). */
static struct u32_window *quake_window; static const char *quake_next; static UINT quake_ms;
static void quake_feed(void) { if (quake_next && *quake_next) { q3_key(quake_window,*quake_next,quake_ms); quake_ms+=400; quake_next++; } }
static void quake_cursor_hook(HWND h, UINT m) { (void)h; (void)m; quake_feed(); CHECK(SetCursorPos(100,100)); }
static void quake_pump(struct u32_window *w, const char *line, unsigned delivery)
{
  char typed[32]; UINT n=0, downs=0, ups=0; MSG msg;
  quake_window=w; quake_next=line; quake_ms=5000; quake_hook=delivery==5 ? quake_cursor_hook : 0;
  if (delivery==1) while (*quake_next) quake_feed();
  if (delivery==2 || delivery==3 || delivery==4 || delivery==5) quake_feed();   /* the first key starts the loop */
  for (;;) {
    if (delivery==0) quake_feed();
    BOOL pending=PeekMessageA(&msg,0,0,0,0);
    if (delivery==2 && pending) quake_feed();
    if (!pending) { if (*quake_next) { if (delivery!=0) quake_feed(); continue; } break; }
    CHECK(GetMessageA(&msg,0,0,0));
    if (delivery==3 && msg.message==WM_KEYDOWN) quake_feed();
    TranslateMessage(&msg);
    if (delivery==4 && msg.message==WM_KEYDOWN) quake_feed();
    if (msg.message==WM_CHAR && n<31) typed[n++]=(char)msg.wParam;
    if (msg.message==WM_KEYDOWN) downs++;
    if (msg.message==WM_KEYUP) ups++;
    DispatchMessageA(&msg);
  }
  quake_hook=0; typed[n]=0;
  if (strcmp(typed,line)) fprintf(stderr,"quake pump delivery=%u: \"%s\" -> \"%s\" downs=%u ups=%u\n",delivery,line,typed,downs,ups);
  CHECK(!strcmp(typed,line) && downs==strlen(line) && ups==strlen(line));
}
/* delivery 6: Quake III's second thread (Sys_InitStreamThread) drains the
 * desktop channel while the key is down; the key down is deferred to the
 * window's thread, which then drains the TEXT and the key up itself. The
 * deferred key down must still come first (every other keystroke was lost
 * in the console, QEMU 2026-10-26: the pump handled new packets before the
 * deferred ones, so the WM_CHAR stayed pending with the previous key's time). */
static void quake_other_thread_pump(struct u32_window *w, const char *line, const char *reordered)
{
  char typed[32]; UINT n=0, ms=9000, downs=0, ups=0; MSG msg;
  for (const char *c=line; *c; c++, ms+=400) {
    static const UINT scan[128]={['m']=0x32,['a']=0x1e,['p']=0x19,[' ']=0x39,['q']=0x10,['3']=0x04,['d']=0x20,['1']=0x02,['7']=0x08};
    q3_event(w,CIUKI_INPUT_KEY,scan[(unsigned char)*c],1,ms);
    if (strchr(reordered,*c)) {                                 /* the other thread drains this key down */
      ui_kernel_switch(1); u32_pump(); ui_kernel_switch(0);
      CHECK(u32_thread()->event_count==1);                      /* deferred to the window's thread */
    }
    q3_event(w,CIUKI_INPUT_TEXT,(unsigned char)*c,0,ms);
    q3_event(w,CIUKI_INPUT_KEY,scan[(unsigned char)*c],0,ms+40);
    while (PeekMessageA(&msg,0,0,0,0)) {
      CHECK(GetMessageA(&msg,0,0,0));
      TranslateMessage(&msg);
      if (msg.message==WM_CHAR && n<31) typed[n++]=(char)msg.wParam;
      if (msg.message==WM_KEYDOWN) downs++;
      if (msg.message==WM_KEYUP) ups++;
      DispatchMessageA(&msg);
    }
  }
  typed[n]=0;
  if (strcmp(typed,line)) fprintf(stderr,"quake pump other thread (%s): \"%s\" -> \"%s\" downs=%u ups=%u\n",reordered,line,typed,downs,ups);
  CHECK(!strcmp(typed,line) && downs==strlen(line) && ups==strlen(line));
  CHECK(!u32_thread()->text_count);                             /* no character left pending */
}
/* The QEMU case: 0.4 s per key and the one-second settings refresh
 * (DESK_INPUT_SETTINGS) due inside every other key down; each key's three
 * packets are already in the channel when the pump takes the key down. */
static void quake_refresh_pump(struct u32_window *w, const char *line)
{
  char typed[32]; UINT n=0, ms=12000, downs=0, ups=0; MSG msg;
  for (const char *c=line; *c; c++, ms+=400) {
    ui_kernel_pause(400);
    q3_key(w,*c,ms);
    while (PeekMessageA(&msg,0,0,0,0)) {
      CHECK(GetMessageA(&msg,0,0,0));
      TranslateMessage(&msg);
      if (msg.message==WM_CHAR && n<31) typed[n++]=(char)msg.wParam;
      if (msg.message==WM_KEYDOWN) downs++;
      if (msg.message==WM_KEYUP) ups++;
      DispatchMessageA(&msg);
    }
  }
  typed[n]=0;
  UINT refreshes=0;
  for (UINT k=0;k<outgoing_count;k++) if (outgoing[k].data[0]==DESK_V3_VERSION && outgoing[k].data[1]==DESK_INPUT_SETTINGS) refreshes++;
  fprintf(stderr,"quake pump settings refresh: \"%s\" -> \"%s\" downs=%u ups=%u settings_requests=%u pending=%u\n",line,typed,downs,ups,refreshes,u32_thread()->text_count);
  CHECK(refreshes>=3);                                            /* the one-second refresh fired during the typing */
  CHECK(!strcmp(typed,line) && downs==strlen(line) && ups==strlen(line) && !u32_thread()->text_count);
}
/* Codex review P2: a deferred DESK_CONFIGURE runs WM_SIZE, which releases
 * the USER lock; the other thread drains and defers the next key down during
 * it, and its TEXT and key up arrive afterwards. The pump must take that key
 * down before the newer packets. */
static unsigned size_hooks; static HWND size_window;
static void size_hook(HWND h, UINT m)
{
  if (m!=WM_SIZE || h!=size_window) return;
  size_hooks++;
  struct u32_window *w=u32_window(size_window);
  q3_event(w,CIUKI_INPUT_KEY,0x20,1,20000);                      /* 'd' down arrives while WM_SIZE runs */
  ui_kernel_switch(1); u32_pump(); ui_kernel_switch(0);           /* the other thread defers it to us */
  CHECK(u32_thread()->event_count==1);
  q3_event(w,CIUKI_INPUT_TEXT,'d',0,20000);                       /* its TEXT and key up come after */
  q3_event(w,CIUKI_INPUT_KEY,0x20,0,20040);
}
static void quake_configure_callback_pump(void)
{
  /* A window of its own: the CONFIGURE resizes it, the shared fixture
   * windows keep their surfaces for the later tests. */
  size_window=CreateWindowExA(0,"USER fixture","console",WS_VISIBLE|WS_CAPTION,20,20,200,120,0,0,0,0);
  CHECK(size_window); discard();
  struct u32_window *w=u32_window(size_window);
  discard(); u32_clear_input(); SetFocus(size_window);
  UINT configure[]={264,120,(UINT)w->serial+1,9,0,32,0,0};
  event(w,DESK_CONFIGURE,configure,8);
  ui_kernel_switch(1); u32_pump(); ui_kernel_switch(0);           /* the CONFIGURE is deferred to our thread */
  CHECK(u32_thread()->event_count==1);
  quake_hook=size_hook; size_hooks=0;
  char typed[8]; UINT n=0; MSG msg;
  while (PeekMessageA(&msg,0,0,0,0)) {                            /* the pump runs the deferred CONFIGURE: WM_SIZE */
    CHECK(GetMessageA(&msg,0,0,0));
    TranslateMessage(&msg);
    if (msg.message==WM_CHAR && n<7) typed[n++]=(char)msg.wParam;
    DispatchMessageA(&msg);
  }
  quake_hook=0; typed[n]=0;
  if (strcmp(typed,"d")) fprintf(stderr,"quake pump configure callback: typed \"%s\" size_hooks=%u pending=%u\n",typed,size_hooks,u32_thread()->text_count);
  CHECK(size_hooks==1 && !strcmp(typed,"d") && !u32_thread()->text_count);
  CHECK(DestroyWindow(size_window)); size_window=0; discard();
}
/* Codex review P2: the settings refresh must survive a failed 32 KiB
 * allocation (the guard was left set for the life of the process). */
static void settings_allocation_failure(void)
{
  void *hog=u32_alloc(4*1024*1024-32*1024-64); CHECK(hog);      /* leaves less than 32769 bytes of USER heap */
  ui_kernel_pause(1100); u32_pump();                             /* refresh due: the allocation fails */
  u32_free(hog);
  UINT before=outgoing_count;
  ui_kernel_pause(1100); u32_pump();                             /* due again: refreshes */
  UINT refreshes=0;
  for (UINT k=before;k<outgoing_count;k++) if (outgoing[k].data[0]==DESK_V3_VERSION && outgoing[k].data[1]==DESK_INPUT_SETTINGS) refreshes++;
  CHECK(refreshes==1);
  puts("PASS settings refresh: a failed allocation does not disable it");
}
static void test_quake_pump(void) {
  struct u32_window *w = u32_window(windows[0]);
  quake_configure_callback_pump();
  settings_allocation_failure();
  for (unsigned delivery=0; delivery<6; delivery++) {
    discard(); u32_clear_input(); SetFocus(w->handle);
    quake_pump(w,"map q3dm17",delivery);
  }
  discard(); u32_clear_input(); SetFocus(w->handle);
  quake_refresh_pump(w,"map q3dm17");
  /* One key drained by the other thread after keys the window's thread
   * drained itself (the QEMU case: "map q3dm17" arrived as "ma 3m7"), then
   * every key, then every other key. */
  for (unsigned v=0; v<3; v++) {
    discard(); u32_clear_input(); SetFocus(w->handle);
    quake_other_thread_pump(w,"map q3dm17",v==0 ? "d" : v==1 ? "map q3d17" : "pq1");
  }
  u32_clear_input();
  puts("PASS Quake III pump: PeekMessage(PM_NOREMOVE)/GetMessage/TranslateMessage per message types every character of \"map q3dm17\" for six arrival points of the next key");
}
static void test_input(void) {
  MSG msg;
  struct u32_window *w = u32_window(windows[0]);
  for (UINT layout = 0; layout < 2; layout++) {
    shared_input[0]=layout;u32_layout = layout;
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
  discard();u32_clear_input();SetFocus(w->handle);
  g12_input_event(w,CIUKI_INPUT_KEY,0x2a,1);discard();
  g12_input_event(w,CIUKI_INPUT_KEY,0x0d,1);
  CHECK(PeekMessageA(&msg,0,0,0,1) && msg.message==WM_KEYDOWN);CHECK(TranslateMessage(&msg));
  g12_input_event(w,CIUKI_INPUT_TEXT,'^',0);
  CHECK(PeekMessageA(&msg,0,0,0,1) && msg.message==0x103 && msg.wParam=='^');
  g12_input_event(w,CIUKI_INPUT_KEY,0x0d,0);g12_input_event(w,CIUKI_INPUT_KEY,0x2a,0);discard();
  g12_input_event(w,CIUKI_INPUT_KEY,0x12,1);
  CHECK(PeekMessageA(&msg,0,0,0,1) && msg.message==WM_KEYDOWN);CHECK(TranslateMessage(&msg));
  g12_input_event(w,CIUKI_INPUT_TEXT,'e',0);
  CHECK(PeekMessageA(&msg,0,0,0,1) && msg.message==WM_CHAR && msg.wParam==0xea);
  g12_input_event(w,CIUKI_INPUT_KEY,0x12,0);discard();u32_clear_input();
  puts("PASS G12 WM_DEADCHAR then composed WM_CHAR through TranslateMessage");
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
extern BOOL W32API WinHelpA(HWND,const char *,UINT,uintptr_t);
extern int W32API GetDlgCtrlID(HWND);
extern BOOL W32API WinHelpW(HWND,const WCHAR *,UINT,uintptr_t);
extern HWND W32API CiukiHtmlHelpA(HWND,const char *,UINT,uintptr_t);
extern HWND W32API CiukiHtmlHelpW(HWND,const WCHAR *,UINT,uintptr_t);
#ifndef U32_TEST_EXTERNAL_MODULES
static void test_help_requests(void)
{
    host_help=1;
    CHECK(WinHelpA(windows[0],"guide.hlp",1,42));
    struct desk_packet p; struct desk_help_request r;
    CHECK(desk_parse(&outgoing[outgoing_count-1],1,0,&p) && p.opcode==DESK_LAUNCH && desk_help_decode(p.argument,&r));
    CHECK(r.format==1 && r.command==1 && r.context==42 && !strcmp(r.path,"/Help/guide.hlp"));
    DWORD controls[]={ (DWORD)GetDlgCtrlID(child),42,0,0 };
    CHECK(WinHelpA(child,"guide.hlp",0xd,(uintptr_t)controls));
    CHECK(desk_parse(&outgoing[outgoing_count-1],1,0,&p) && desk_help_decode(p.argument,&r) && r.command==8 && r.context==42);
    controls[1]=UINT32_MAX;
    CHECK(!WinHelpA(child,"guide.hlp",0xc,(uintptr_t)controls) && GetLastError()==2);
    const WCHAR widefile[]={'D',':','\\','G','u','i','d','e','.','h','l','p',0};
    const WCHAR widekey[]={'O','v','e','r','v','i','e','w',0};
    CHECK(WinHelpW(windows[0],widefile,3,(uintptr_t)widekey));
    CHECK(desk_parse(&outgoing[outgoing_count-1],1,0,&p) && desk_help_decode(p.argument,&r));
    CHECK(!strcmp(r.path,"/mnt/D/Guide.hlp") && !strcmp(r.query,"Overview"));
    CHECK(!WinHelpA(windows[0],"missing.hlp",1,42) && GetLastError()==2);
    CHECK(!WinHelpA(windows[0],"guide.hlp",0xffff,0) && GetLastError()==120);
    CHECK(!WinHelpA(windows[0],"guide.hlp",3,0) && GetLastError()==87);
    CHECK(WinHelpA(windows[0],0,2,0));
    HWND proxy=CiukiHtmlHelpA(windows[0],"guide.chm::/first.htm",0,0);
    CHECK(proxy && IsWindow(proxy));
    CHECK(desk_parse(&outgoing[outgoing_count-1],1,0,&p) && desk_help_decode(p.argument,&r));
    CHECK(r.format==2 && !strcmp(r.query,"/first.htm"));
    CHECK(CiukiHtmlHelpA(windows[0],"guide.chm",15,42)==proxy);
    const WCHAR widechm[]={'g','u','i','d','e','.','c','h','m',0};
    CHECK(CiukiHtmlHelpW(windows[0],widechm,2,(uintptr_t)widekey)==proxy);
    CHECK(!CiukiHtmlHelpA(windows[0],0,18,0) && !IsWindow(proxy));
    CHECK(UnregisterClassA("CiukiHtmlHelpProxy",0));
    host_help=0;
    puts("TRACE WinHelp A/W and HtmlHelp A/W native topic, context, keyword, missing file, reuse and close bridge");
}
#endif
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
static void failed_destroy_ping(void);
static void review_round2(void);
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
  failed_destroy_ping();
  review_round2();
  CHECK(DestroyWindow(a));
  printf("TRACE window order: insert-after bottom topmost notopmost owner "
         "cycle refusal cross-thread owned destruction\n");
}

/* q3exec3 review: DestroyWindow destroys dependents first and fails when a
 * dependent on another thread cannot be reached; the window then lives on.
 * A PING the desktop sent it while a same-thread dependent was being closed
 * must still be answered, or the desktop (which pings again only after an
 * answer) marks a live, pumping window "not responding". */
static DWORD review_mine_wire, review_mine_generation;
static HWND review_parent;
static void review_close_hook(const struct ciuki_message *sent, BOOL after_result) {
  if (sent->data[0] != DESK_V3_VERSION || sent->data[1] != DESK_CLOSE ||
      desk_get32(sent->data + 8) != review_mine_wire ||
      desk_get32(sent->data + 12) != review_mine_generation)
    return;
  CHECK(in_tail - in_head < 256);
  struct ciuki_message *m = &incoming[in_tail++ % 256];
  if (!after_result) {
    struct u32_window *p = u32_window(review_parent);
    CHECK(p && p->wire && p->destroying && !p->closing);
    desk_v3_message(m, DESK_PING, 4, 0, p->wire, p->generation);
    desk_put32(m->data + 16, 0x61);
  } else
    desk_v3_message(m, DESK_CLOSED, 0, 0, review_mine_wire, review_mine_generation);
}
static void failed_destroy_ping(void) {
  review_parent = CreateWindowExA(0, "USER fixture", "parent", WS_VISIBLE | WS_CAPTION,
                                  300, 30, 60, 60, 0, 0, 0, 0);
  HWND mine = CreateWindowExA(0, "USER fixture", "mine", WS_VISIBLE | WS_CAPTION,
                              310, 40, 40, 40, review_parent, 0, 0, 0);
  ui_kernel_switch(1);
  HWND theirs = CreateWindowExA(0, "USER fixture", "theirs", WS_VISIBLE | WS_CAPTION,
                                320, 50, 40, 40, review_parent, 0, 0, 0);
  ValidateRect(theirs, 0);
  ui_kernel_switch(0);
  CHECK(review_parent && mine && theirs);
  ValidateRect(review_parent, 0);
  ValidateRect(mine, 0);
  struct u32_window *p = u32_window(review_parent), *w = u32_window(mine);
  CHECK(w < u32_window(theirs)); /* the same-thread dependent goes first */
  review_mine_wire = w->wire, review_mine_generation = w->generation;
  UINT from = outgoing_count;
  desk_reply_hook = review_close_hook;
  ui_kernel_refuse_send(U32_DESTROY);
  CHECK(!DestroyWindow(review_parent));
  desk_reply_hook = 0;
  CHECK(IsWindow(review_parent) && !IsWindow(mine) && IsWindow(theirs) &&
        !p->destroying && !p->closing && p->wire);
  MSG m;
  while (PeekMessageA(&m, 0, 0, 0, 1))
    DispatchMessageA(&m);
  UINT pongs = 0;
  for (UINT i = from; i < outgoing_count; i++) {
    const struct ciuki_message *o = &outgoing[i];
    if (o->data[0] != DESK_V3_VERSION)
      continue;
    CHECK(desk_get32(o->data + 8) && desk_get32(o->data + 12));
    if (o->data[1] == DESK_PONG) {
      CHECK(desk_get32(o->data + 8) == p->wire && desk_get32(o->data + 12) == p->generation &&
            desk_get32(o->data + 16) == 0x61);
      pongs++;
    }
  }
  CHECK(pongs == 1 && !p->ping);
  ui_kernel_schedule(schedule_peer);
  CHECK(DestroyWindow(review_parent));
  ui_kernel_schedule(0);
  CHECK(!IsWindow(review_parent) && !IsWindow(theirs));
}
/* q3exec4 review regressions (Codex P2 findings 1-5). */
static HWND late_popup, late_owner;
static LRESULT W32API late_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  /* A child's WM_DESTROY creates a popup owned by the window being
   * destroyed. */
  if (m == WM_DESTROY && late_owner)
    late_popup = CreateWindowExA(0, "USER fixture", "late", WS_POPUP | WS_CAPTION | WS_VISIBLE,
                                 400, 40, 40, 40, late_owner, 0, 0, 0);
  return DefWindowProcA(h, m, wp, lp);
}
static UINT selfhide_touched;
static BOOL selfhide_ran;
static LRESULT W32API selfhide_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  /* WM_PAINT hides its own window, lets the parent repaint, then keeps
   * drawing through the DC BeginPaint gave it. */
  if (m == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    CHECK(dc);
    ShowWindow(h, 0);
    UpdateWindow(GetParent(h));
    RECT r;
    GetClientRect(h, &r);
    fill(dc, &r, (HBRUSH)1);
    selfhide_touched = ((struct test_dc *)dc)->count;
    selfhide_ran = 1;
    EndPaint(h, &ps);
    return 0;
  }
  return DefWindowProcA(h, m, wp, lp);
}
static BOOL update_at(HWND h, int x, int y) {
  struct u32_window *w = u32_window(h);
  for (UINT i = 0; w && i < w->update.n; i++)
    if (PtInRect(&w->update.r[i], (POINT){x, y}))
      return 1;
  return 0;
}
static void paint_all(void) {
  MSG m;
  for (UINT i = 0; i < 1000 && PeekMessageA(&m, 0, 0, 0, 1); i++)
    DispatchMessageA(&m);
}
static void review_round2(void) {
  WNDCLASSA late = {.lpfnWndProc = late_proc, .lpszClassName = "q3exec4 late"};
  WNDCLASSA self = {.lpfnWndProc = selfhide_proc, .lpszClassName = "q3exec4 selfhide"};
  CHECK(RegisterClassA(&late) && RegisterClassA(&self));
  /* 1: the late popup goes with its owner. */
  late_owner = CreateWindowExA(0, "USER fixture", "owner", WS_VISIBLE | WS_CAPTION,
                               300, 30, 80, 80, 0, 0, 0, 0);
  HWND kid = CreateWindowExA(0, "q3exec4 late", "kid", WS_CHILD | WS_VISIBLE, 0, 0, 20, 20,
                             late_owner, (HANDLE)(uintptr_t)1, 0, 0);
  CHECK(late_owner && kid && DestroyWindow(late_owner));
  CHECK(late_popup && !IsWindow(late_popup) && !IsWindow(kid));
  late_owner = 0;
  /* 2: under WS_CLIPCHILDREN, hiding the upper of two overlapping
   * controls repaints the lower one where they overlapped. */
  HWND p = CreateWindowExA(0, "USER fixture", "clip", WS_VISIBLE | WS_CAPTION | WS_CLIPCHILDREN,
                           300, 30, 140, 140, 0, 0, 0, 0);
  HWND lower = CreateWindowExA(0, "USER fixture", "lower", WS_CHILD | WS_VISIBLE, 10, 10, 60, 60,
                               p, (HANDLE)(uintptr_t)1, 0, 0);
  HWND upper = CreateWindowExA(0, "USER fixture", "upper", WS_CHILD | WS_VISIBLE, 40, 40, 60, 60,
                               p, (HANDLE)(uintptr_t)2, 0, 0);
  CHECK(p && lower && upper);
  paint_all();
  CHECK(!u32_window(lower)->update.n);
  ShowWindow(upper, 0);
  CHECK(update_at(lower, 35, 35));
  /* 3: moving and hiding in one SetWindowPos exposes the old rectangle. */
  ShowWindow(upper, 5);
  paint_all();
  ValidateRect(p, 0);
  CHECK(SetWindowPos(upper, 0, 110, 110, 10, 10, SWP_HIDEWINDOW | SWP_NOZORDER | SWP_NOACTIVATE));
  CHECK(!IsWindowVisible(upper) && update_at(p, 45, 45) && update_at(p, 95, 95));
  /* 4: SWP_SHOWWINDOW without a move or size paints a control that was
   * painted, hidden and left without an update region. */
  paint_all();
  ShowWindow(lower, 0);
  paint_all();
  CHECK(!IsWindowVisible(lower) && !u32_window(lower)->update.n);
  CHECK(SetWindowPos(lower, 0, 0, 0, 0, 0,
                     SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE));
  CHECK(IsWindowVisible(lower) && update_at(lower, 30, 30));
  /* 5: a WM_PAINT handler that hides its window draws nothing more. */
  HWND hider = CreateWindowExA(0, "q3exec4 selfhide", "hider", WS_CHILD | WS_VISIBLE, 5, 5, 20, 20,
                               p, (HANDLE)(uintptr_t)3, 0, 0);
  CHECK(hider);
  paint_all();
  CHECK(selfhide_ran && !selfhide_touched && !IsWindowVisible(hider));
  CHECK(DestroyWindow(p));
  CHECK(UnregisterClassA("q3exec4 late", 0) && UnregisterClassA("q3exec4 selfhide", 0));
  printf("TRACE q3exec4 review: late owned popup destroyed, clipped sibling exposed, old "
         "rectangle exposed, SWP_SHOWWINDOW repaints, busy DC emptied on hide\n");
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
    k[i] = CreateWindowExA(0, "USER fixture", "kid", WS_CHILD | WS_VISIBLE | WS_THICKFRAME, 0,
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
    more[i] = CreateWindowExA(0, "USER fixture", "more", WS_CHILD | WS_VISIBLE | WS_THICKFRAME,
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
                             WS_VISIBLE | WS_CAPTION | WS_THICKFRAME, 5, 40, 60, 60, 0, 0, 0, 0);
  CHECK(SetWindowPos(top[2], HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
  RECT work = {0, 32, 800, 600};
  CHECK(TileWindows(0, 0, &work, 3, top) == 1);
  CHECK(local_rect(top[0]).left == 0 && local_rect(top[0]).top == 32 &&
        local_rect(top[0]).right == 800 && local_rect(top[1]).left == 5);
  /* A fixed-size property dialog moves but never takes the cell's size. */
  HWND fixed=CreateWindowExA(0,"USER fixture","Display Properties",WS_VISIBLE | WS_CAPTION,
                              27,61,260,180,0,0,0,0);
  RECT fixed_before=local_rect(fixed); HWND mixed[2]={top[0],fixed};
  CHECK(CascadeWindows(0,0,&work,2,mixed)==2);
  RECT fr=local_rect(fixed);
  CHECK(fr.right-fr.left==fixed_before.right-fixed_before.left && fr.bottom-fr.top==fixed_before.bottom-fixed_before.top);
  CHECK(TileWindows(0,MDITILE_HORIZONTAL,&work,2,mixed)==2);
  fr=local_rect(fixed);
  CHECK(fr.right-fr.left==fixed_before.right-fixed_before.left && fr.bottom-fr.top==fixed_before.bottom-fixed_before.top);
  CHECK(TileWindows(0,MDITILE_VERTICAL,&work,2,mixed)==2);
  fr=local_rect(fixed);
  CHECK(fr.right-fr.left==fixed_before.right-fixed_before.left && fr.bottom-fr.top==fixed_before.bottom-fixed_before.top);
  CHECK(DestroyWindow(fixed));
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
  /* fx-is5boot: GetWindowPlacement reports the state USER holds. */
  extern BOOL W32API GetWindowPlacement(HWND, void *);
  struct { UINT length, flags, show; POINT minimum, maximum; RECT normal; } place;
  memset(&place, 0x5a, sizeof(place));
  CHECK(GetWindowPlacement(h, &place) && place.length == 44 && !place.flags && place.show == 3 &&
        place.minimum.x == -1 && place.maximum.y == -1);
  CHECK(!GetWindowPlacement((HWND)(uintptr_t)0x7ff0, &place) && GetLastError() == 1400);
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
  RECT frame_rect, area;
  CHECK(GetWindowPlacement(h, &place) && place.show == 1 && GetWindowRect(h, &frame_rect) &&
        SystemParametersInfoA(48, 0, &area, 0) && place.normal.left == frame_rect.left - area.left &&
        place.normal.bottom == frame_rect.bottom - area.top);
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
/* f7 (Quake III Arena): ChangeDisplaySettingsA/EnumDisplaySettingsA over
 * the fixture display (800x600x32, hardware modes 640x480x32, 800x600x32),
 * CloseWindow and the gdi32 seams. */
static void q3_devmode(uint8_t *dm, UINT size, DWORD fields, DWORD w, DWORD h, DWORD bpp) {
  memset(dm, 0, 156);
  w32_put16(dm + 36, (uint16_t)size);
  w32_put32(dm + 40, fields);
  w32_put32(dm + 104, bpp);
  w32_put32(dm + 108, w);
  w32_put32(dm + 112, h);
}
static DWORD child_idle_at;
static void child_becomes_idle(void) {
  /* This callback runs on the caller's sleep. Another GUI thread must be
   * able to take USER's lock while the installer waits for the child. */
  unsigned previous = ui_kernel_current();
  ui_kernel_switch(1);
  (void)GetQueueStatus(QS_ALLINPUT);
  ui_kernel_switch(previous);
  if ((int32_t)(GetTickCount() - child_idle_at) >= 0) {
    ui_kernel_schedule(0);
    ui_kernel_child_wait(0, 1);
  }
}
static void test_input_idle(void) {
  HANDLE h = (HANDLE)(uintptr_t)ui_kernel_child_handle(1);
  DWORD start = GetTickCount();
  /* GUI PE metadata is not a runtime queue. INFINITE must not enter the
   * parent's sleep/pump loop until a child thread has created a queue. */
  CHECK(WaitForInputIdle(h, 25) == W32_WAIT_FAILED && GetLastError() == 1471);
  CHECK(GetTickCount() == start);
  CHECK(WaitForInputIdle(h, W32_INFINITE) == W32_WAIT_FAILED && GetTickCount() == start);
  ui_kernel_child_wait(0, 0); /* create a runtime queue without becoming idle */
  CHECK(WaitForInputIdle(h, 0) == W32_WAIT_TIMEOUT);
  CHECK(WaitForInputIdle(h, 25) == W32_WAIT_TIMEOUT && GetTickCount() - start == 25);
  ui_kernel_child_wait(1, 1); /* pending input is not idle */
  CHECK(WaitForInputIdle(h, 0) == W32_WAIT_TIMEOUT);
  ui_kernel_child_wait(0, 0); /* nonblocking poll is not a wait for input */
  CHECK(WaitForInputIdle(h, 0) == W32_WAIT_TIMEOUT);
  child_idle_at = GetTickCount() + 20;
  ui_kernel_schedule(child_becomes_idle);
  CHECK(WaitForInputIdle(h, 100) == 0);
  ui_kernel_child_wait(1, 1); /* a satisfied latch survives later work */
  CHECK(WaitForInputIdle(h, 0) == 0);
  ui_kernel_close_handle(U32_PTR(h));
  CHECK(WaitForInputIdle(h, 0) == W32_WAIT_FAILED && GetLastError() == 6);
  h = (HANDLE)(uintptr_t)ui_kernel_child_handle(1);
  ui_kernel_child_wait(0, 0);
  ui_kernel_switch(2);
  MSG msg;
  CHECK(!PeekMessageA(&msg,0,0,0,2 /* PM_NOYIELD */));
  ui_kernel_switch(0);
  CHECK(WaitForInputIdle(h,0)==W32_WAIT_TIMEOUT);
  ui_kernel_switch(2);
  CHECK(!PeekMessageA(&msg,0,0,0,0));
  ui_kernel_switch(0);
  CHECK(WaitForInputIdle(h,0)==0);
  ui_kernel_close_handle(U32_PTR(h));
  h = (HANDLE)(uintptr_t)ui_kernel_child_handle(0);
  start = GetTickCount();
  CHECK(WaitForInputIdle(h, W32_INFINITE) == W32_WAIT_FAILED && GetLastError() == 1471);
  CHECK(GetTickCount() == start);
  ui_kernel_close_handle(U32_PTR(h));
  ui_kernel_child_wait(0, 0);
  puts("TRACE fx-q3hang input idle: no-queue GUI fails immediately, busy timeout, pending input, first idle latch, invalid/console, USER lock released");
}
static void test_quake3(void) {
  BOOL saved = host_modules;
  host_modules = 1;
  uint8_t dm[156];
  DWORD mode[3], caps[3];
  CHECK(w32_user_display_caps(caps) && caps[0] == 800 && caps[1] == 600 && caps[2] == 32);
  CHECK(ChangeDisplaySettingsA(0, 0) == 0 && !w32_user_display_mode(mode));
  /* Quake III GLW_SetMode: dmSize = sizeof(DEVMODE) (148), width and height. */
  q3_devmode(dm, 148, 0x80000 | 0x100000, 640, 480, 0);
  CHECK(ChangeDisplaySettingsA(dm, 2 /* CDS_TEST */) == 0 && !w32_user_display_mode(mode));
  CHECK(ChangeDisplaySettingsA(dm, 4 /* CDS_FULLSCREEN */) == 0);
  CHECK(w32_user_display_mode(mode) && mode[0] == 640 && mode[1] == 480 && mode[2] == 32);
  UINT leases = sent_count(DESK_V3_LEASE);
  CHECK(ChangeDisplaySettingsA(dm, 4) == 0 && sent_count(DESK_V3_LEASE) == leases);
  /* An explicit reset must consult the driver even for the current mode. */
  lease_eio = 1;
  CHECK(ChangeDisplaySettingsA(dm, 4 | 0x40000000u /* CDS_RESET */) == -1);
  CHECK(!w32_user_display_mode(mode));
  lease_eio = 0;
  CHECK(ChangeDisplaySettingsA(dm, 4) == 0);
  HWND borrower=CreateWindowExA(0,"USER fixture","borrower",WS_POPUP|WS_VISIBLE,
                                  0,0,640,480,0,0,0,0);
  HWND alias=CreateWindowExA(0,"USER fixture","alias",WS_POPUP|WS_VISIBLE,
                               0,0,640,480,0,0,0,0);
  struct dd_lease borrowed, shared;
  CHECK(borrower && alias && !w32_user_display_lease((uintptr_t)borrower,640,480,32,&borrowed));
  CHECK(!w32_user_display_lease((uintptr_t)alias,640,480,32,&shared));
  HWND carrier=u32_window(borrower)->lease_carrier;
  CHECK(carrier && borrowed.active && shared.token==borrowed.token);
  leases=sent_count(DESK_V3_LEASE);
  w32_user_display_release((uintptr_t)borrower);
  CHECK(sent_count(DESK_V3_LEASE)==leases+1 && IsWindow(carrier));
  CHECK(!u32_window(carrier)->lease.active && !u32_window(carrier)->lease_wanted);
  w32_user_display_poll((uintptr_t)alias,&shared);
  CHECK(!shared.active && !u32_window(alias)->lease_carrier);
  w32_user_display_release((uintptr_t)borrower);
  w32_user_display_release((uintptr_t)alias);
  CHECK(sent_count(DESK_V3_LEASE)==leases+1);
  CHECK(DestroyWindow(borrower) && DestroyWindow(alias));
  CHECK(ChangeDisplaySettingsA(dm,4)==0);
  /* Keep the original active-carrier destruction regression as well. */
  borrower=CreateWindowExA(0,"USER fixture","carrier lifetime",WS_POPUP|WS_VISIBLE,
                            0,0,640,480,0,0,0,0);
  CHECK(borrower && !w32_user_display_lease((uintptr_t)borrower,640,480,32,&borrowed));
  carrier=u32_window(borrower)->lease_carrier;
  CHECK(borrowed.active && carrier && DestroyWindow(carrier) && !w32_user_display_mode(mode));
  w32_user_display_poll((uintptr_t)borrower,&shared);
  CHECK(!shared.active && !u32_window(borrower)->lease_carrier);
  CHECK(DestroyWindow(borrower) && ChangeDisplaySettingsA(dm,4)==0);
  HWND scaled=CreateWindowExA(0,"USER fixture","scaled drawable",WS_POPUP|WS_VISIBLE,
                               0,0,640,480,0,0,0,0);
  struct dd_lease scaled_lease;
  CHECK(scaled && !w32_user_display_lease((uintptr_t)scaled,320,240,16,&scaled_lease));
  CHECK(scaled_lease.active && scaled_lease.width==320 && scaled_lease.height==240 && scaled_lease.bpp==16);
  memset(dm, 0x55, sizeof(dm));
  w32_put16(dm + 36, 148);
  CHECK(EnumDisplaySettingsA(0, 0xffffffffu, dm));
  CHECK(w32_u32(dm + 40) == 0x7c0000u && w32_u32(dm + 104) == 32 && w32_u32(dm + 108) == 640 &&
        w32_u32(dm + 112) == 480 && w32_u32(dm + 116) == 0 && w32_u32(dm + 120) == 60);
  CHECK(w32_u16(dm + 36) == 148 && dm[0] == 0x55 && dm[124] == 0x55);
  CHECK(EnumDisplaySettingsA("DISPLAY", 0xfffffffeu, dm) && w32_u32(dm + 108) == 800 &&
        w32_u32(dm + 112) == 600);
  CHECK(ChangeDisplaySettingsA(0, 0) == 0 && !w32_user_display_mode(mode));
  CHECK(EnumDisplaySettingsA(0, 0xffffffffu, dm) && w32_u32(dm + 108) == 800);
  CHECK(DestroyWindow(scaled));
  /* Refusals: unknown size, 24 bpp, persistence, bad flags, short DEVMODE. */
  q3_devmode(dm, 148, 0x80000 | 0x100000, 641, 480, 0);
  CHECK(ChangeDisplaySettingsA(dm, 4) == -2);
  q3_devmode(dm, 148, 0x40000 | 0x80000 | 0x100000, 640, 480, 24);
  CHECK(ChangeDisplaySettingsA(dm, 4) == -2);
  q3_devmode(dm, 148, 0x80000 | 0x100000, 1024, 768, 0);
  CHECK(ChangeDisplaySettingsA(dm, 4) == -2); /* larger than every hardware mode */
  q3_devmode(dm, 148, 0x40000 | 0x80000 | 0x100000, 640, 480, 16);
  CHECK(ChangeDisplaySettingsA(dm, 1 /* CDS_UPDATEREGISTRY */) == -3);
  CHECK(ChangeDisplaySettingsA(dm, 4 | 1) == -4 && ChangeDisplaySettingsA(dm, 0x80) == -4);
  q3_devmode(dm, 68, 0x80000 | 0x100000, 640, 480, 0);
  CHECK(ChangeDisplaySettingsA(dm, 4) == -5);
  CHECK(!w32_user_display_mode(mode));
  q3_devmode(dm, 156, 0x40000 | 0x80000 | 0x100000 | 0x400000, 320, 200, 15);
  w32_put32(dm + 120, 75); /* a refresh rate is accepted and ignored */
  CHECK(ChangeDisplaySettingsA(dm, 0) == 0 && w32_user_display_mode(mode) && mode[2] == 16);
  CHECK(ChangeDisplaySettingsA(0, 0) == 0);
  /* The T23's quarantined BIOS returns EIO on every grant. CDS must report
   * the driver failure now, before GLW_SetMode commits to a popup drawable. */
  lease_eio = 1;
  q3_devmode(dm, 148, 0x80000 | 0x100000, 640, 480, 0);
  for (UINT attempt = 0; attempt < 3; attempt++) {
    CHECK(ChangeDisplaySettingsA(dm, 4) == -1);
    CHECK(!w32_user_display_mode(mode));
    CHECK(ChangeDisplaySettingsA(0, 0) == 0);
  }
  lease_eio = 0;
  /* Enumeration: 7 sizes up to 800x600 at 8, 16 and 32 bits, each accepted. */
  UINT n = 0, last_bpp = 0;
  BOOL saw = 0;
  for (; n < 64; n++) {
    q3_devmode(dm, 148, 0, 0, 0, 0);
    if (!EnumDisplaySettingsA(0, n, dm))
      break;
    DWORD bpp = w32_u32(dm + 104), w = w32_u32(dm + 108), h = w32_u32(dm + 112);
    CHECK(bpp >= last_bpp && w <= 800 && h <= 600 && w32_u32(dm + 120) == 60);
    last_bpp = bpp;
    saw |= w == 640 && h == 480 && bpp == 32;
    w32_put32(dm + 40, 0x40000 | 0x80000 | 0x100000);
    CHECK(ChangeDisplaySettingsA(dm, 2) == 0);
  }
  CHECK(n == 21 && saw);
  q3_devmode(dm, 120, 0, 0, 0, 0);
  CHECK(!EnumDisplaySettingsA(0, 0, dm) && GetLastError() == 87);
  q3_devmode(dm, 148, 0, 0, 0, 0);
  CHECK(!EnumDisplaySettingsA("\\\\.\\DISPLAY2", 0, dm) && GetLastError() == 87);
  /* CloseWindow minimizes a top-level window; a child is refused. */
  HWND top = CreateWindowExA(0, "USER fixture", "close", WS_VISIBLE | WS_CAPTION, 0, 0, 60, 40, 0, 0, 0, 0);
  CHECK(top && !IsIconic(top));
  CHECK(CloseWindow(top) && IsIconic(top) && IsWindow(top));
  HWND inner = CreateWindowExA(0, "USER fixture", "inner", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, top, 0, 0, 0);
  CHECK(inner && !CloseWindow(inner) && !IsIconic(inner));
  CHECK(DestroyWindow(top) && !IsWindow(inner));
  SetLastError(0);
  CHECK(!CloseWindow(top) && GetLastError() == 1400);
  host_modules = saved;
  puts("TRACE quake3: ChangeDisplaySettingsA full-screen record, EnumDisplaySettingsA, CloseWindow");
}
#ifdef U32_TEST_GL
extern BOOL ui_gl_frame(DWORD);
extern void ui_gl_close(void);
static void test_gl_failure(void) {
  host_modules=real_gdi=1;
  u32_gdi=0;
  u32_gdi_ensure();
  uint8_t dm[156];
  q3_devmode(dm,148,0x80000|0x100000,640,480,0);
  UINT before=sent_count(DESK_V3_LEASE);
  CHECK(ChangeDisplaySettingsA(dm,4)==0);
  CHECK(sent_count(DESK_V3_LEASE)==before+1);
  HWND good=CreateWindowExA(0,"USER fixture","GL lease",WS_POPUP|WS_VISIBLE,
                              0,0,640,480,0,0,0,0);
  CHECK(good);
  HDC good_dc=GetDC(good);
  CHECK(good_dc && ui_gl_frame(U32_PTR(good_dc)) && ui_gl_frame(U32_PTR(good_dc)));
  CHECK(sent_count(DESK_V3_LEASE)==before+1);
  HWND carrier=0;
  for (UINT i=0;i<U32_WINDOWS;i++)
    if (u32_windows[i].handle && u32_windows[i].lease_wanted && u32_windows[i].lease.active)
      carrier=u32_windows[i].handle;
  CHECK(carrier && carrier!=good);
  CHECK(u32_window(good)->lease_carrier==carrier);
  /* GL teardown must revoke the real grant without destroying its carrier. */
  before=sent_count(DESK_V3_LEASE);
  ui_gl_close();
  CHECK(sent_count(DESK_V3_LEASE)==before+1 && IsWindow(carrier));
  CHECK(!u32_window(carrier)->lease.active && !u32_window(carrier)->lease_wanted);
  struct dd_lease expired;
  w32_user_display_poll((uintptr_t)good,&expired);
  CHECK(!expired.active && !u32_window(good)->lease_carrier);
  /* Borrow again, then switch a live GL context to composed presentation. */
  CHECK(ChangeDisplaySettingsA(dm,4)==0);
  CHECK(ui_gl_frame(U32_PTR(good_dc)));
  carrier=u32_window(good)->lease_carrier;
  CHECK(carrier && IsWindow(carrier));
  before=sent_count(DESK_V3_LEASE);
  CHECK(SetWindowLongA(good,GWL_STYLE,WS_CAPTION|WS_VISIBLE));
  CHECK(ui_gl_frame(U32_PTR(good_dc)));
  CHECK(sent_count(DESK_V3_LEASE)==before+1 && IsWindow(carrier));
  CHECK(!u32_window(carrier)->lease.active && !u32_window(carrier)->lease_wanted);
  w32_user_display_poll((uintptr_t)good,&expired);
  CHECK(!expired.active && !u32_window(good)->lease_carrier);
  ui_gl_close();
  CHECK(DestroyWindow(carrier) && !w32_user_display_mode(0));
  CHECK(ChangeDisplaySettingsA(0,0)==0);
  CHECK(ReleaseDC(good,good_dc) && DestroyWindow(good));
  HWND w = CreateWindowExA(0,"USER fixture","GL failure",WS_POPUP|WS_VISIBLE,
                           0,0,640,480,0,0,0,0);
  CHECK(w);
  HDC dc=GetDC(w);
  CHECK(dc);
  before=sent_count(DESK_V3_LEASE);
  lease_eio=1;
  CHECK(!ui_gl_frame(U32_PTR(dc)));
  CHECK(sent_count(DESK_V3_LEASE)==before+1);
  for (UINT i=0;i<3;i++) CHECK(ui_gl_frame(U32_PTR(dc)));
  CHECK(sent_count(DESK_V3_LEASE)==before+1);
  struct u32_window *window=u32_window(w);
  CHECK(window->lease_failed && !window->lease.active);
  CHECK((w32_u32(window->surface.pixels)&0xffffff)==0xff0000);
  /* A sibling program still receives input while this drawable composes. */
  CHECK(PostMessageA(windows[0],WM_USER+83,42,0));
  MSG m;
  CHECK(PeekMessageA(&m,windows[0],WM_USER+83,WM_USER+83,PM_REMOVE) && m.wParam==42);
  ui_gl_close();
  lease_eio=0;
  CHECK(ReleaseDC(w,dc) && DestroyWindow(w));
  host_modules=real_gdi=0;
  u32_set_gdi(&test_gdi);
  puts("TRACE fx-q3hang production GL: EIO once, three composed red frames, sibling queue responsive");
}
#endif
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
static void test_g23_appearance(void) {
  struct appearance value; ap_default(&value,0); value.dpi=144;
  value.item[AP_SELECTED].colour=0x112233;
  char encoded[AP_HEX_BYTES+1]; ap_encode(&value,encoded); u32_appearance_load(encoded);
  BOOL colour_notice=0,metric_notice=0; MSG notice;
  for(unsigned i=0;i<64 && PeekMessageA(&notice,0,0,0,1);i++) {
    colour_notice|=notice.message==21; metric_notice|=notice.message==0x1a;
    DispatchMessageA(&notice);
  }
  CHECK(colour_notice && metric_notice);
  CHECK(GetSysColor(13)==0x332211);
  CHECK(GetSystemMetrics(2)==24 && GetSystemMetrics(4)==36 && GetSystemMetrics(38)==144);
  uint8_t ncm[340]={0}; w32_put32(ncm,340);
  CHECK(SystemParametersInfoA(41,340,ncm,0));
  CHECK(w32_u32(ncm+8)==24 && w32_u32(ncm+20)==36 && !strcmp((char *)ncm+52,"Inter"));
  CHECK((int32_t)w32_u32(ncm+280)==-18);
  BOOL enabled=0;
  CHECK(SystemParametersInfoA(38,0,&enabled,0) && enabled);
  CHECK(SystemParametersInfoA(74,0,&enabled,0) && enabled);
  CHECK(SystemParametersInfoA(0x1002,0,&enabled,0) && enabled);
  /* The fixture refuses disk writes: failed shared setters retain the value. */
  CHECK(!SystemParametersInfoA(37,0,0,1));
  CHECK(SystemParametersInfoA(38,0,&enabled,0) && enabled);
  w32_put32(ncm,339); CHECK(!SystemParametersInfoA(42,339,ncm,0) && GetLastError()==87);
#ifndef U32_TEST_EXTERNAL_MODULES
  g23_files_enabled=1; strcpy(g23_config,"keyboard=it\nfuture_key=kept\n");
  CHECK(SystemParametersInfoA(37,0,0,1));
  CHECK(strstr(g23_config,"future_key=kept\n") && strstr(g23_config,"appearance="));
  CHECK(SystemParametersInfoA(38,0,&enabled,0) && !enabled);
  w32_put32(ncm,340); CHECK(SystemParametersInfoA(41,340,ncm,0));
  CHECK(SystemParametersInfoA(42,340,ncm,1));
  CHECK(GetSystemMetrics(2)==24 && GetSystemMetrics(4)==36);
  g23_flush_failure=1;
  CHECK(!SystemParametersInfoA(37,1,0,1));
  CHECK(SystemParametersInfoA(38,0,&enabled,0) && !enabled);
  g23_flush_failure=0; g23_temporary=0;g23_rename_failure=1;
  CHECK(!SystemParametersInfoA(37,1,0,1));
  CHECK(SystemParametersInfoA(38,0,&enabled,0) && !enabled);
  g23_rename_failure=0;g23_temporary=0;g23_files_enabled=0;
#endif
  u32_appearance_load("");
  colour_notice=metric_notice=0;
  for(unsigned i=0;i<64 && PeekMessageA(&notice,0,0,0,1);i++) {
    colour_notice|=notice.message==21; metric_notice|=notice.message==0x1a;
    DispatchMessageA(&notice);
  }
  CHECK(colour_notice && metric_notice);
  w32_put32(ncm,340); CHECK(SystemParametersInfoA(41,340,ncm,0));
  CHECK(w32_u32(ncm+8)!=24); /* Rollback also drops cached scaled metrics. */
  puts("TRACE G23 shared palette, DPI metrics, NONCLIENTMETRICS fonts, Effects and failed-write rollback");
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
  {
    /* G16 MessageBeep: the icon picks the desktop's system sound, queued without a reply. */
    static const struct { UINT type, event; } beeps[] = {
        {0x10, SOUND_ERROR},       {0x30, SOUND_ERROR},         {0x20, SOUND_QUESTION},
        {0x40, SOUND_INFORMATION}, {0, SOUND_INFORMATION},      {0xffffffffu, SOUND_INFORMATION}};
    UINT from = outgoing_count, seen = 0;
    for (UINT i = 0; i < sizeof(beeps) / sizeof(*beeps); i++)
      CHECK(MessageBeep(beeps[i].type));
    for (UINT i = from; i < outgoing_count; i++)
      if (outgoing[i].data[1] == DESK_V3_SOUND) {
        CHECK(desk_get32(outgoing[i].data + 16) == beeps[seen].event);
        seen++;
      }
    CHECK(seen == sizeof(beeps) / sizeof(*beeps));
  }
  CHECK(SystemParametersInfoA(2, 0, 0, 0) &&
        SystemParametersInfoA(1, 0, &beep, 0) && !beep);
  {
    UINT from = outgoing_count;
    CHECK(MessageBeep(0x10));   /* SPI_SETBEEP off: nothing is played, the call still succeeds */
    for (UINT i = from; i < outgoing_count; i++)
      CHECK(outgoing[i].data[1] != DESK_V3_SOUND);
  }
  CHECK(SystemParametersInfoA(13, 0, &value, 0) && value == 75);
  CHECK(SystemParametersInfoA(13, 90, 0, 0) &&
        SystemParametersInfoA(13, 0, &value, 0) && value == 90 &&
        GetSystemMetrics(38) == 90);
  CHECK(SystemParametersInfoA(29, 8, 0, 0) && GetSystemMetrics(36) == 8);
  CHECK(SystemParametersInfoA(33, 1, 0, 0) && GetSystemMetrics(23) == 1);
  CHECK(SystemParametersInfoA(33, 0, 0, 0) && !GetSystemMetrics(23));
  CHECK(SystemParametersInfoA(23,3,0,1));
  CHECK(SystemParametersInfoA(22,0,&value,0) && value==3 && saved_input[1]==3);
  CHECK(SystemParametersInfoA(11,0,0,0));
  CHECK(SystemParametersInfoA(10,0,&value,0) && value==0 && saved_input[2]==31);
  CHECK(SystemParametersInfoA(93,7,0,1));
  CHECK(SystemParametersInfoA(94,0,&value,0) && value==7 && saved_input[5]==7);
  CHECK(SystemParametersInfoA(105,0xffffffffu,0,1));
  CHECK(SystemParametersInfoA(104,0,&value,0) && (UINT)value==0xffffffffu && saved_input[6]==101);
  CHECK(SystemParametersInfoA(23,1,0,0));
  CHECK(SystemParametersInfoA(11,31,0,0));
  CHECK(SystemParametersInfoA(105,3,0,0));
  CHECK(SystemParametersInfoA(93,0,0,0));
  uint8_t dead_keys[256]={0};uint16_t composed;
  dead_keys[16]=128;
  CHECK(ToAsciiEx(0xdd,0x0d,dead_keys,&composed,0,0x04100410u)==-1 && composed=='^');
  dead_keys[16]=0;
  CHECK(ToAsciiEx('A',0x1e,dead_keys,&composed,0,0x04100410u)==1 && composed==0xe2);
  dead_keys[16]=128;CHECK(ToAsciiEx(0xdd,0x0d,dead_keys,&composed,0,0x04100410u)==-1);
  dead_keys[16]=0;CHECK(ToAsciiEx('X',0x2d,dead_keys,&composed,0,0x04100410u)==2 && composed==('^'|('x'<<8)));
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
  /* G25: the accessibility records and SM_SHOWSOUNDS. */
  uint8_t rec[48];
  w32_fill(rec, 0, sizeof(rec));
  w32_put32(rec, 8);
  CHECK(SystemParametersInfoA(58, 8, rec, 0) && w32_u32(rec + 4) == 0x1fe);   /* StickyKeys defaults: Flags 510 */
  w32_put32(rec + 4, 0x1ff | 0x01000000u);                                   /* the latched bit is state: ignored */
  CHECK(SystemParametersInfoA(59, 8, rec, 0));
  w32_put32(rec + 4, 0);
  CHECK(SystemParametersInfoA(58, 8, rec, 0) && w32_u32(rec + 4) == 0x1ff);
  CHECK(!SystemParametersInfoA(58, 12, rec, 0) && GetLastError() == 87);    /* uiParam must be the size */
  w32_put32(rec, 4);
  CHECK(!SystemParametersInfoA(58, 8, rec, 0) && GetLastError() == 87);     /* and so must cbSize */
  w32_put32(rec, 24);
  CHECK(SystemParametersInfoA(50, 24, rec, 0) && w32_u32(rec + 4) == 0x7e &&
        w32_u32(rec + 8) == 1000 && w32_u32(rec + 12) == 1000 &&
        w32_u32(rec + 16) == 500 && w32_u32(rec + 20) == 0);
  w32_put32(rec + 20, 700);                                                  /* BounceKeys needs the others zero */
  CHECK(!SystemParametersInfoA(51, 24, rec, 0) && GetLastError() == 87);
  w32_put32(rec + 8, 0); w32_put32(rec + 12, 0); w32_put32(rec + 16, 0);
  CHECK(SystemParametersInfoA(51, 24, rec, 0) && SystemParametersInfoA(50, 24, rec, 0) && w32_u32(rec + 20) == 700);
  w32_put32(rec + 20, 0);                                                    /* and zero bounce needs them all */
  CHECK(!SystemParametersInfoA(51, 24, rec, 0));
  w32_put32(rec + 8, 20001); w32_put32(rec + 12, 1); w32_put32(rec + 16, 1);
  CHECK(!SystemParametersInfoA(51, 24, rec, 0) && GetLastError() == 87);
  w32_put32(rec + 8, 2000); w32_put32(rec + 12, 300); w32_put32(rec + 16, 1400);
  CHECK(SystemParametersInfoA(51, 24, rec, 0));
  w32_put32(rec, 8);
  CHECK(SystemParametersInfoA(52, 8, rec, 0) && w32_u32(rec + 4) == 0x3e);
  w32_put32(rec + 4, 0x3f);
  CHECK(SystemParametersInfoA(53, 8, rec, 0) && SystemParametersInfoA(52, 8, rec, 0) && w32_u32(rec + 4) == 0x3f);
  w32_fill(rec, 0, sizeof(rec)); w32_put32(rec, 28);
  CHECK(SystemParametersInfoA(54, 28, rec, 0) && w32_u32(rec + 4) == 0x7e && w32_u32(rec + 8) == 80 &&
        w32_u32(rec + 12) == 3000 && w32_u32(rec + 16) == 8);
  w32_put32(rec + 8, 9);                                                     /* iMaxSpeed 10..360 */
  CHECK(!SystemParametersInfoA(55, 28, rec, 0) && GetLastError() == 87);
  w32_put32(rec + 8, 360); w32_put32(rec + 12, 999);
  CHECK(!SystemParametersInfoA(55, 28, rec, 0));                              /* iTimeToMaxSpeed 1000..5000 */
  w32_put32(rec + 12, 5000); w32_put32(rec + 4, 0xff | 0x80000000u);          /* MKF_MOUSEMODE is state */
  CHECK(SystemParametersInfoA(55, 28, rec, 0) && SystemParametersInfoA(54, 28, rec, 0) &&
        w32_u32(rec + 4) == 0xff && w32_u32(rec + 8) == 360 && w32_u32(rec + 12) == 5000);
  w32_put32(rec + 24, 1);                                                    /* reserved */
  CHECK(!SystemParametersInfoA(55, 28, rec, 0));
  w32_fill(rec, 0, sizeof(rec)); w32_put32(rec, 48);
  CHECK(SystemParametersInfoA(64, 48, rec, 0) && w32_u32(rec + 4) == 2 && w32_u32(rec + 32) == 0);
  w32_put32(rec + 4, 1); w32_put32(rec + 32, 4);                              /* a custom effect is not available */
  CHECK(!SystemParametersInfoA(65, 48, rec, 0) && GetLastError() == 87);
  w32_put32(rec + 32, 3);
  CHECK(SystemParametersInfoA(65, 48, rec, 0) && SystemParametersInfoA(64, 48, rec, 0) &&
        w32_u32(rec + 4) == 3 && w32_u32(rec + 32) == 3);
  char scheme[64] = "";
  struct { UINT cb; DWORD flags; char *scheme; } hc = {sizeof(hc), 0, scheme};   /* HIGHCONTRASTA */
  CHECK(SystemParametersInfoA(66, sizeof(hc), &hc, 0) && hc.flags == 0x7e &&
        !strcmp(scheme, "High Contrast Black"));
  strcpy(scheme, "High Contrast White"); hc.flags = 0x7f;
  CHECK(SystemParametersInfoA(67, sizeof(hc), &hc, 0));
  scheme[0] = 0; hc.flags = 0;
  CHECK(SystemParametersInfoA(66, sizeof(hc), &hc, 0) && hc.flags == 0x7f && !strcmp(scheme, "High Contrast White"));
  w32_fill(rec, 0, sizeof(rec)); w32_put32(rec, 12);
  CHECK(SystemParametersInfoA(60, 12, rec, 0) && w32_u32(rec + 4) == 0 && w32_u32(rec + 8) == 300000);
  w32_put32(rec + 4, 3); w32_put32(rec + 8, 600000);
  CHECK(SystemParametersInfoA(61, 12, rec, 0) && SystemParametersInfoA(60, 12, rec, 0) &&
        w32_u32(rec + 4) == 3 && w32_u32(rec + 8) == 600000);
  CHECK(!SystemParametersInfoA(62, 16, rec, 0) && GetLastError() == 120);    /* SerialKeys is not provided */
  CHECK(!GetSystemMetrics(70));
  CHECK(SystemParametersInfoA(57, 1, 0, 0) && GetSystemMetrics(70) == 1 &&
        SystemParametersInfoA(56, 0, &old, 0) && old);
  CHECK(SystemParametersInfoA(57, 0, 0, 0) && !GetSystemMetrics(70));
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
#include "g5b_clipboard_test.h"
static void test_production_gdi(void) {
  CHECK(host_gdi_proc && host_ddraw_seam);
  /* An old v1 table ends before the v2 tail. ASan guards its exact size. */
  _Alignas(struct u32_gdi) unsigned char v1[__builtin_offsetof(struct u32_gdi,window_pixel_format)];
  memcpy(v1,&test_gdi,sizeof(v1));
  const DWORD v1_version=1; /* test_gdi is v3 (q3exec4); this copy is a v1 table */
  memcpy(v1,&v1_version,sizeof(v1_version));
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
  /* q3exec4 review: the production v3 tail empties a busy DC's visible
   * region (what USER does to the DCs of a window it hides). */
  CHECK(u32_gdi->version == 3 && u32_gdi->visible);
  HDC busy = GetDC(windows[0]);
  CHECK(busy && u32_gdi->visible(busy, 0, 0));
  RECT corner = {0, 0, 40, 40};
  FillRect(busy, &corner, (HBRUSH)9 /* COLOR_WINDOWTEXT + 1 */);
  CHECK(ReleaseDC(windows[0], busy));
  CHECK(w32_u32((uint8_t *)w->surface.pixels + 5 * w->surface.pitch + 5 * 4) == 0xc0c0c0);
  /* G30: GetFreeSystemResources from the window table and the production gdi32 object table. */
  {
    UINT windows_used = 0;
    for (UINT i = 0; i < U32_WINDOWS; i++)
      if (u32_windows[i].handle)
        windows_used++;
    UINT user_free = (U32_WINDOWS - windows_used) * 100 / U32_WINDOWS;
    CHECK(GetFreeSystemResources(2) == user_free && user_free < 100);
    UINT gdi_before = GetFreeSystemResources(1);
    DWORD many_dc[40], many_bitmap[40];
    uint32_t *many_bits;
    for (UINT i = 0; i < 40; i++) {
      many_bitmap[i] = host_canvas(8, 8, 0, &many_dc[i], &many_bits);
      CHECK(many_bitmap[i]);
    }
    CHECK(GetFreeSystemResources(1) < gdi_before && GetFreeSystemResources(1) >= 97);
    for (UINT i = 0; i < 40; i++)
      host_canvas_free(many_dc[i], many_bitmap[i]);
    CHECK(GetFreeSystemResources(1) >= gdi_before - 1);
    UINT system = GetFreeSystemResources(0);
    CHECK(system == (GetFreeSystemResources(1) < GetFreeSystemResources(2) ? GetFreeSystemResources(1) : GetFreeSystemResources(2)));
    SetLastError(0);
    CHECK(GetFreeSystemResources(3) == 0 && GetLastError() == 87);
    puts("TRACE GetFreeSystemResources: USER and GDI table shares, system is the lower, selector 3 fails");
  }
  test_cf_bitmap();
  test_g5b_handles();
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
#include "g18_test.inc"
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
  test_input_idle();
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
  test_quake_pump();
  puts("STEP paint");
  test_paint();
  puts("STEP clipboard");
  test_clipboard();
  test_extras();
  #ifndef U32_TEST_EXTERNAL_MODULES
  test_help_requests();
  #endif
  test_spi();
#ifndef U32_TEST_EXTERNAL_MODULES
  test_g18_shell();
#endif
  test_order();
  test_arrange();
  test_zoom_limits();
  test_display();
  test_quake3();
  test_production_gdi();
#ifdef U32_TEST_GL
  test_gl_failure();
#endif
  test_g23_appearance();
  traces = 0;
  CHECK(DestroyWindow(windows[0]));
  UINT child_destroy = 999, parent_destroy = 999, child_nc = 999, parent_nc = 999;
  for (UINT i = 0; i < traces; i++) {
    if (trace[i].m == WM_DESTROY) {
      if (trace[i].h == child)
        child_destroy = i;
      if (trace[i].h == windows[0])
        parent_destroy = i;
    }
    if (trace[i].m == WM_NCDESTROY) {
      if (trace[i].h == child)
        child_nc = i;
      if (trace[i].h == windows[0])
        parent_nc = i;
    }
  }
  /* q3exec4: WM_DESTROY reaches the window first, while its children
   * still exist, then each child (Microsoft Learn, WM_DESTROY). */
  CHECK(parent_destroy < child_destroy && child_destroy < child_nc &&
        child_nc < parent_nc && parent_nc < 999 && !IsWindow(child));
  printf("TRACE destruction: parent WM_DESTROY, child WM_DESTROY/WM_NCDESTROY, "
         "parent WM_NCDESTROY\n");
  for (UINT i = 1; i < 3; i++) {
    ui_kernel_switch(i);
    CHECK(DestroyWindow(windows[i]));
  }
  ui_kernel_switch(0);
  test_desktop_close();
  test_modal_prompt_wire();
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
