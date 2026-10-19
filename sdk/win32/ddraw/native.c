/* Copied-record presenter and USER/GDI boundary. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include "internal.h"
#include <ciuki/abi.h>
#include <ciuki/present_abi.h>
#include <w32/shims.h>
extern DWORD w32r_gate(DWORD, DWORD, DWORD, DWORD);
extern void *API GetModuleHandleA(const char *);
extern void *API LoadLibraryA(const char *);
extern void *API GetProcAddress(void *, const char *);
static struct {
  DDRESULT(API *lease)(uintptr_t, DWORD, DWORD, DWORD, struct dd_lease *);
  void(API *release)(uintptr_t);
  void(API *poll)(uintptr_t, struct dd_lease *);
  DDRESULT(API *clip)(uintptr_t, RECT *, DWORD *);
  HDC(API *get_dc)(uintptr_t);
  int(API *release_dc)(uintptr_t, HDC);
  BOOL(API *client_to_screen)(uintptr_t, POINT *);
  LONG(API *get_window_long)(uintptr_t, int);
  BOOL(API *adjust)(RECT *, DWORD, BOOL, DWORD);
  BOOL(API *place)(uintptr_t, uintptr_t, int, int, int, int, DWORD);
} user;
static BOOL bind_user(void) {
  if (user.get_dc)
    return 1;
  void *module = GetModuleHandleA("user32.dll");
  if (!module)
    module = LoadLibraryA("user32.dll");
  if (!module)
    return 0;
  user.lease = GetProcAddress(module, "w32_user_display_lease");
  user.release = GetProcAddress(module, "w32_user_display_release");
  user.poll = GetProcAddress(module, "w32_user_display_poll");
  user.clip = GetProcAddress(module, "w32_user_display_clip");
  user.get_dc = GetProcAddress(module, "GetDC");
  user.release_dc = GetProcAddress(module, "ReleaseDC");
  user.client_to_screen = GetProcAddress(module, "ClientToScreen");
  user.get_window_long = GetProcAddress(module, "GetWindowLongA");
  user.adjust = GetProcAddress(module, "AdjustWindowRectEx");
  user.place = GetProcAddress(module, "SetWindowPos");
  return user.get_dc && user.release_dc && user.client_to_screen;
}
static BOOL display(struct dd_display *out) {
  struct ciuki_display_info info = {.version = CIUKI_DISPLAY_VERSION,
                                    .size = sizeof(info)};
  if ((int32_t)w32r_gate(CIUKI_SYS_DISPLAY, CIUKI_DISPLAY_QUERY,
                         (DWORD)(uintptr_t)&info, 0) < 0 ||
      info.mode_count > 32)
    return 0;
  out->width = info.width;
  out->height = info.height;
  out->bpp = info.bpp;
  out->vram = info.vram_bytes;
  out->count = info.mode_count;
  for (DWORD i = 0; i < info.mode_count; i++)
    out->modes[i] = (struct dd_mode){info.modes[i].width, info.modes[i].height,
                                     info.modes[i].bpp};
  return 1;
}
static DDRESULT lease(uintptr_t hwnd, DWORD w, DWORD h, DWORD bpp,
                      struct dd_lease *out) {
  if (!bind_user() || !user.lease)
    return DDERR_UNSUPPORTED;
  return user.lease(hwnd, w, h, bpp, out);
}
static void release(uintptr_t hwnd) {
  if (bind_user() && user.release)
    user.release(hwnd);
}
static void poll(uintptr_t hwnd, struct dd_lease *out) {
  if (bind_user() && user.poll)
    user.poll(hwnd, out);
  else
    out->active = 0;
}
static DDRESULT submit(DWORD action, const struct dd_lease *lease,
                       const void *data, DWORD first, DWORD count) {
  struct ciuki_w32_display_args a = {.header = {1, sizeof(a), W32_DISPLAY, 0},
                                     .action = action,
                                     .token = lease->token,
                                     .generation = lease->generation,
                                     .address = (DWORD)(uintptr_t)data,
                                     .first = first,
                                     .count = count};
  if ((int32_t)w32r_gate(CIUKI_SYS_W32, (DWORD)(uintptr_t)&a, sizeof(a), 0) <
          0 ||
      a.error || !a.result)
    return a.error == 16 ? DDERR_WASSTILLDRAWING : DDERR_SURFACELOST;
  return 0;
}
static DDRESULT frame(const struct dd_lease *lease, const struct w32_dib *dib) {
  BOOL convert555 = dib->bpp == 16 && dib->masks[0] == 0x7c00;
  if (dib->pitch == lease->pitch && !convert555)
    return submit(W32_DISPLAY_FRAME, lease, dib->bits, 0, 0);
  if (lease->pitch < dib->width * (dib->bpp / 8) ||
      (uint64_t)lease->pitch * dib->height > 0x7fffffff)
    return DDERR_INVALIDPARAMS;
  uint8_t *copy = dd_alloc(lease->pitch * dib->height);
  if (!copy)
    return DDERR_OUTOFMEMORY;
  if (convert555) {
    struct w32_dib target = {.bits = copy,
                             .width = dib->width,
                             .height = dib->height,
                             .pitch = lease->pitch,
                             .size = lease->pitch * dib->height};
    dd_set_format(&target, 16);
    for (DWORD y = 0; y < dib->height; y++)
      for (DWORD x = 0; x < dib->width; x++)
        w32_dib_put(&target, x, y,
                    w32_dib_encode(&target, w32_dib_rgb(dib, x, y)));
  } else {
    for (DWORD y = 0; y < dib->height; y++)
      w32_copy(copy + y * lease->pitch, dib->bits + y * dib->pitch,
               dib->width * (dib->bpp / 8));
  }
  DDRESULT r = submit(W32_DISPLAY_FRAME, lease, copy, 0, 0);
  dd_free(copy);
  return r;
}
static DDRESULT palette(const struct dd_lease *lease, DWORD first, DWORD count,
                        const DWORD *colors) {
  return submit(W32_DISPLAY_PALETTE, lease, colors, first, count);
}
static DDRESULT gamma(const struct dd_lease *lease, const DDGAMMA *gamma) {
  return submit(W32_DISPLAY_GAMMA, lease, gamma, 0, 0);
}
static DDRESULT clip(uintptr_t hwnd, RECT *out, DWORD *count) {
  return bind_user() && user.clip ? user.clip(hwnd, out, count)
                                  : DDERR_NOCLIPLIST;
}
static DDRESULT window(uintptr_t hwnd, const struct w32_dib *dib,
                       const RECT *clips, DWORD count) {
  (void)clips;
  (void)count;
  if (!bind_user())
    return DDERR_UNSUPPORTED;
  HDC dc = user.get_dc(hwnd);
  if (!dc)
    return DDERR_NOCLIPLIST;
  POINT origin = {0};
  if (!user.client_to_screen(hwnd, &origin)) {
    user.release_dc(hwnd, dc);
    return DDERR_NOCLIPLIST;
  }
  struct {
    BITMAPINFOHEADER header;
    DWORD colors[256];
  } info = {0};
  info.header = (BITMAPINFOHEADER){.biSize = 40,
                                   .biWidth = dib->pitch / (dib->bpp / 8),
                                   .biHeight = -(LONG)dib->height,
                                   .biPlanes = 1,
                                   .biBitCount = (uint16_t)dib->bpp,
                                   .biCompression =
                                       dib->bpp == 16 ? BI_BITFIELDS : BI_RGB,
                                   .biClrUsed = dib->bpp == 8 ? 256 : 0};
  if (dib->bpp == 8)
    w32_copy(info.colors, dib->colors, 1024);
  else if (dib->bpp == 16)
    w32_copy(info.colors, dib->masks, 12);
  const struct ciuki_w32_compat *compat = w32_shim_current();
  if (compat && (compat->mask & W32_SHIM_BIT(W32_SH_DDRAW_WINDOWED)))
    origin = (POINT){0};
  int n = StretchDIBits(dc, -origin.x, -origin.y, dib->width, dib->height, 0, 0,
                        dib->width, dib->height, dib->bits,
                        (const BITMAPINFO *)&info, 0, SRCCOPY);
  user.release_dc(hwnd, dc);
  return n == -1 ? DDERR_UNSUPPORTED : 0;
}
static uint64_t clock_ns(void) {
  struct ciuki_timespec t = {0};
  w32r_gate(CIUKI_SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (DWORD)(uintptr_t)&t, 0);
  return (uint64_t)(uint32_t)t.tv_sec * 1000000000u + (uint32_t)t.tv_nsec;
}
static void wait_ns(uint64_t deadline) {
  uint64_t now = clock_ns();
  if (now >= deadline)
    return;
  DWORD ms = (DWORD)((deadline - now + 999999) / 1000000);
  w32r_gate(CIUKI_SYS_SLEEP_MS, ms, 0, 0);
}
const struct dd_backend dd_native_backend = {display, lease,    release, poll,
                                             frame,   palette,  gamma,   clip,
                                             window,  clock_ns, wait_ns};
BOOL API DDrawDllMain(void *module, DWORD reason, void *reserved) {
  (void)module;
  (void)reserved;
  if (reason == 0)
    w32_ddraw_process_exit();
  return 1;
}

extern void *API CreateThread(void *, DWORD, DWORD(API *)(void *), void *,
                              DWORD, DWORD *);
extern DWORD API WaitForSingleObject(void *, DWORD);
extern BOOL API CloseHandle(void *);
static void *worker;
static volatile DWORD stopping;
static DWORD API pacing_worker(void *arg) {
  (void)arg;
  while (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE)) {
    w32r_gate(CIUKI_SYS_SLEEP_MS, 1, 0, 0);
    if (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE))
      w32_ddraw_tick();
  }
  return 0;
}
void dd_native_start(void) {
  if (worker)
    return;
  bind_user();
  __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
  worker = CreateThread(0, 65536, pacing_worker, 0, 0, 0);
}
void dd_native_stop(void) {
  if (!worker)
    return;
  __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
  WaitForSingleObject(worker, 0xffffffffu);
  CloseHandle(worker);
  worker = 0;
}

DDRESULT dd_native_window_mode(uintptr_t hwnd, DWORD width, DWORD height) {
  if (!bind_user() || !user.get_window_long || !user.adjust || !user.place)
    return DDERR_UNSUPPORTED;
  RECT client = {0, 0, width, height};
  if (!user.adjust(&client, user.get_window_long(hwnd, -16), 0,
                   user.get_window_long(hwnd, -20)))
    return DDERR_INVALIDPARAMS;
  return user.place(hwnd, 0, 0, 0, client.right - client.left,
                    client.bottom - client.top, 0x16)
             ? 0
             : DDERR_INVALIDPARAMS;
}
