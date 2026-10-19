/* Authored fixture games: raw COM calls, real GDI, independent frame oracle in
 * Python. SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../../sdk/win32/ddraw/internal.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern long host_allocations, host_bytes, host_fail_after;
void host_canaries(void);
int host_replay(const uint8_t *, DWORD, struct dd_lease *);
void host_shim_mask(DWORD);
#define METHOD(o, n, signature) ((signature)((o)->vt[n]))
#define Q(o, id, out)                                                          \
  METHOD(o, 0, DDRESULT (*)(DD *, const DDGUID *, void **))(o, id,             \
                                                            (void **)(out))
#define ADD(o) METHOD(o, 1, DWORD (*)(DD *))(o)
#define RELEASE(o) METHOD(o, 2, DWORD (*)(DD *))(o)
#define COOP(o, h, f)                                                          \
  METHOD(o, 20, DDRESULT (*)(DD *, uintptr_t, DWORD))(o, h, f)
#define SETMODE(o, w, h, b)                                                    \
  METHOD(o, 21, DDRESULT (*)(DD *, DWORD, DWORD, DWORD, DWORD, DWORD))(        \
      o, w, h, b, 60, 0)
#define CREATE(o, desc, out)                                                   \
  METHOD(o, 6, DDRESULT (*)(DD *, const DDDESC *, DD **, void *))(o, desc,     \
                                                                  out, 0)
#define BLT(o, dr, s, sr, f, fx)                                               \
  METHOD(o, 5,                                                                 \
         DDRESULT (*)(DD *, const RECT *, DD *, const RECT *, DWORD,           \
                      const DDFX *))(o, dr, s, sr, f, fx)
#define LOCK(o, r, desc, f)                                                    \
  METHOD(o, 25, DDRESULT (*)(DD *, const RECT *, DDDESC *, DWORD, void *))(    \
      o, r, desc, f, 0)
#define UNLOCK(o, r) METHOD(o, 32, DDRESULT (*)(DD *, const void *))(o, r)
#define LOST(o) METHOD(o, 24, DDRESULT (*)(DD *))(o)
#define RESTORE(o) METHOD(o, 27, DDRESULT (*)(DD *))(o)
#define FLIP(o, t, f) METHOD(o, 11, DDRESULT (*)(DD *, DD *, DWORD))(o, t, f)
#define KEY(o, f, k)                                                           \
  METHOD(o, 29, DDRESULT (*)(DD *, DWORD, const DDKEY *))(o, f, k)
#define ATTACHED(o, cap, out)                                                  \
  METHOD(o, 12, DDRESULT (*)(DD *, const DWORD *, DD **))(o, cap, out)
#define SETPAL(o, p) METHOD(o, 31, DDRESULT (*)(DD *, DD *))(o, p)
static uint64_t now;
static DWORD frame_count, window_count, palette_count, gamma_count;
static BOOL benchmark_enabled;
static DD *release_window_surface, *release_window_device;
static uint8_t benchmark_sink[640 * 480];
static struct dd_lease current;
static DWORD budget = 4 * 1024 * 1024;
static uint32_t digest(const void *data, DWORD bytes) {
  const uint8_t *p = data;
  uint32_t h = 2166136261u;
  for (DWORD i = 0; i < bytes; i++)
    h = (h ^ p[i]) * 16777619u;
  return h;
}
static BOOL display(struct dd_display *d) {
  *d = (struct dd_display){
      .width = 800,
      .height = 600,
      .bpp = 32,
      .vram = budget,
      .count = 3,
      .modes = {{640, 480, 16}, {800, 600, 32}, {1024, 768, 32}}};
  return 1;
}
static DDRESULT lease(uintptr_t hwnd, DWORD w, DWORD h, DWORD b,
                      struct dd_lease *out) {
  assert(hwnd);
  current =
      (struct dd_lease){1, current.token + 1,      current.generation + 1, w, h,
                        b, (w * (b / 8) + 3) & ~3u};
  *out = current;
  return 0;
}
static void revoke(uintptr_t hwnd) {
  (void)hwnd;
  current.active = 0;
}
static void poll(uintptr_t hwnd, struct dd_lease *out) {
  (void)hwnd;
  *out = current;
}
static DDRESULT frame(const struct dd_lease *l, const struct w32_dib *d) {
  assert(l->active && l->token == current.token &&
         l->generation == current.generation);
  frame_count++;
  if (benchmark_enabled) {
    assert(d->size == sizeof(benchmark_sink));
    w32_copy(benchmark_sink, d->bits, d->size);
    return 0;
  }
  printf("PRESENT %u %u %u %08x\n", d->width, d->height, d->bpp,
         digest(d->bits, d->size));
  return 0;
}
static DDRESULT palette(const struct dd_lease *l, DWORD first, DWORD count,
                        const DWORD *colors) {
  assert(l->active && first + count <= 256 && colors);
  palette_count++;
  return 0;
}
static DDRESULT gamma(const struct dd_lease *l, const DDGAMMA *g) {
  assert(l->active && g);
  gamma_count++;
  return 0;
}
static RECT visible_rects[] = {{2, 1, 8, 6}, {10, 3, 13, 9}};
static DDRESULT clip(uintptr_t hwnd, RECT *rects, DWORD *count) {
  if (hwnd != 1)
    return DDERR_INVALIDPARAMS;
  if (!rects) {
    *count = 2;
    return 0;
  }
  if (*count < 2) {
    *count = 2;
    return DDERR_MOREDATA;
  }
  memcpy(rects, visible_rects, sizeof(visible_rects));
  *count = 2;
  return 0;
}
static DDRESULT window(uintptr_t hwnd, const struct w32_dib *d,
                       const RECT *rects, DWORD count) {
  assert(hwnd == 1 && count == 2 && rects);
  window_count++;
  if (release_window_surface) {
    DD *surface = release_window_surface, *device = release_window_device;
    release_window_surface = release_window_device = 0;
    RELEASE(surface);
    RELEASE(device);
  }
  printf("WINDOW %u %u %u %08x\n", d->width, d->height, d->bpp,
         digest(d->bits, d->size));
  return 0;
}
static uint64_t clock_ns(void) { return now; }
static void wait_ns(uint64_t target) {
  assert(target >= now);
  now = target;
}
static const struct dd_backend backend = {display, lease,    revoke, poll,
                                          frame,   palette,  gamma,  clip,
                                          window,  clock_ns, wait_ns};
static DD *device(void) {
  DD *d = 0;
  assert(DirectDrawCreateEx(0, (void **)&d, &dd_iids[3], 0) == 0);
  return d;
}
static DD *surface(DD *d, DWORD w, DWORD h, DWORD bpp, DWORD caps) {
  DDDESC2 desc = {0};
  desc.old = (DDDESC){
      .size = sizeof(desc),
      .flags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT,
      .width = w,
      .height = h,
      .caps = caps,
      .pixel = {32, DDPF_RGB | (bpp == 8 ? DDPF_PALETTEINDEXED8 : 0), 0, bpp,
                bpp == 16  ? 0xf800
                : bpp == 8 ? 0
                           : 0xff0000,
                bpp == 16  ? 0x7e0
                : bpp == 8 ? 0
                           : 0xff00,
                bpp == 8 ? 0 : 255, 0}};
  if (bpp == 16)
    desc.old.pixel.blue = 31;
  DD *s = 0;
  assert(CREATE(d, &desc.old, &s) == 0);
  return s;
}
static DD *primary(DD *d, DWORD back) {
  DDDESC2 desc = {0};
  desc.old = (DDDESC){.size = sizeof(desc),
                      .flags = DDSD_CAPS | (back ? DDSD_BACKBUFFERCOUNT : 0),
                      .caps = DDSCAPS_PRIMARYSURFACE |
                              (back ? DDSCAPS_FLIP | DDSCAPS_COMPLEX : 0),
                      .backbuffers = back};
  DD *s = 0;
  assert(CREATE(d, &desc.old, &s) == 0);
  return s;
}
static void clean(DD *d) {
  RELEASE(d);
  w32_ddraw_process_exit();
  w32_gdi_process_exit();
  host_canaries();
  assert(host_allocations == 0 && host_bytes == 0 &&
         w32_ddraw_surface_bytes() == 0);
}
static void abi(void) {
  DD *d = device();
  void *out = (void *)1;
  assert(DirectDrawCreateEx(0, &out, &dd_iids[0], 0) == DDERR_INVALIDPARAMS &&
         out == 0);
  for (unsigned i = 0; i < 4; i++) {
    DD *view = 0;
    assert(Q(d, &dd_iids[i], &view) == 0);
    DD *identity = 0;
    assert(Q(view, &dd_unknown_iid, &identity) == 0);
    assert(identity == &((struct dd_device *)d->owner)->base.interfaces[0]);
    RELEASE(identity);
    uint8_t buffer[160];
    memset(buffer, 0xa5, sizeof(buffer));
    DDDESC *desc = (void *)buffer;
    desc->size = view->generation >= 4 ? sizeof(DDDESC2) : sizeof(DDDESC);
    DWORD size = desc->size;
    assert(METHOD(view, 12, DDRESULT (*)(DD *, DDDESC *))(view, desc) == 0);
    for (DWORD k = size; k < 160; k++)
      assert(buffer[k] == 0xa5);
    RELEASE(view);
  }
  const DWORD sizes[] = {172, 316, 364, 380};
  for (unsigned i = 0; i < 4; i++) {
    uint8_t caps[400];
    memset(caps, 0xa5, sizeof(caps));
    w32_put32(caps, sizes[i]);
    assert(METHOD(d, 11, DDRESULT (*)(DD *, void *, void *))(d, caps, 0) == 0);
    assert(w32_u32(caps + 60) == budget);
    for (DWORD k = sizes[i]; k < 400; k++)
      assert(caps[k] == 0xa5);
  }
  assert(COOP(d, 1, DDSCL_NORMAL) == 0);
  DD *s = surface(d, 13, 9, 16, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
  for (unsigned i = 0; i < 5; i++) {
    DD *view = 0;
    assert(Q(s, &dd_surface_iids[i], &view) == 0);
    DDDESC2 desc = {0};
    desc.old.size = view->generation >= 4 ? sizeof(desc) : sizeof(desc.old);
    assert(METHOD(view, 22, DDRESULT (*)(DD *, DDDESC *))(view, &desc.old) ==
           0);
    desc.old.size = 107;
    assert(METHOD(view, 22, DDRESULT (*)(DD *, DDDESC *))(view, &desc.old) ==
           DDERR_INVALIDPARAMS);
    RELEASE(view);
  }
  DD *g = 0, *color = 0;
  assert(Q(s, &dd_gamma_iid, &g) == 0 && Q(s, &dd_color_iid, &color) == 0);
  DDGAMMA ramp = {0};
  assert(METHOD(g, 4, DDRESULT (*)(DD *, DWORD, const DDGAMMA *))(g, 0,
                                                                  &ramp) == 0);
  RELEASE(g);
  RELEASE(color);
  assert(ADD(s) == 2);
  assert(RELEASE(s) == 1);
  RELEASE(s);
  clean(d);
  puts("PASS abi");
}
static void state(const char *path) {
  FILE *recording = fopen(path, "rb");
  assert(recording);
  uint8_t wire[88];
  DD *d = device();
  assert(COOP(d, 1, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN) == 0);
  assert(SETMODE(d, 640, 480, 8) == 0);
  DD *p = primary(d, 2);
  DWORD caps = DDSCAPS_BACKBUFFER;
  DD *back = 0;
  assert(ATTACHED(p, &caps, &back) == 0);
  DDDESC2 lock = {0};
  lock.old.size = sizeof(lock);
  RECT r = {2, 3, 11, 8};
  assert(LOCK(back, &r, &lock.old, DDLOCK_WAIT) == 0);
  assert(lock.old.pitch == 640 && lock.old.width == 9 && lock.old.height == 5);
  assert(LOCK(back, 0, &lock.old, DDLOCK_WAIT) == DDERR_SURFACEBUSY);
  assert(FLIP(p, 0, DDFLIP_WAIT) == DDERR_SURFACEBUSY);
  assert(BLT(back, 0, 0, 0, 0, 0) == DDERR_INVALIDPARAMS);
  memset(lock.old.pixels, 0x19, 9);
  assert(UNLOCK(back, &r) == 0);
  assert(UNLOCK(back, 0) == DDERR_NOTLOCKED);
  assert(FLIP(p, 0, DDFLIP_WAIT) == 0);
  assert(FLIP(p, 0, DDFLIP_DONOTWAIT) == DDERR_WASSTILLDRAWING);
  struct dd_surface *s = back->owner;
  s->busy = 1;
  assert(LOCK(back, 0, &lock.old, DDLOCK_DONOTWAIT) == DDERR_WASSTILLDRAWING);
  s->busy = 0;
  HDC dc = 0;
  assert(METHOD(p, 17, DDRESULT (*)(DD *, HDC *))(p, &dc) == 0);
  assert(LOCK(p, 0, &lock.old, DDLOCK_WAIT) == DDERR_SURFACEBUSY);
  assert(SetPixel(dc, 4, 5, RGB(255, 255, 255)) != CLR_INVALID);
  DWORD before = frame_count;
  now += DD_SLOT_NS;
  assert(METHOD(p, 26, DDRESULT (*)(DD *, HDC))(p, dc) == 0);
  assert(frame_count == before + 1);
  DD *sys = surface(d, 13, 9, 8, DDSCAPS_SYSTEMMEMORY | DDSCAPS_OFFSCREENPLAIN),
     *video =
         surface(d, 13, 9, 8, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY);
  ((struct dd_surface *)sys->owner)->dib.bits[0] = 73;
  ((struct dd_surface *)video->owner)->dib.bits[0] = 92;
  for (unsigned i = 0; i < 10; i++) {
    DWORD bytes = dd_bytes;
    long allocations = host_allocations;
    assert(fread(wire, 1, 88, recording) == 88 &&
           host_replay(wire, 88, &current));
    assert(LOST(p) == DDERR_SURFACELOST && LOST(back) == DDERR_SURFACELOST &&
           LOST(video) == DDERR_SURFACELOST && LOST(sys) == 0);
    assert(RESTORE(p) == DDERR_NOEXCLUSIVEMODE);
    assert(fread(wire, 1, 88, recording) == 88 &&
           host_replay(wire, 88, &current));
    assert(LOST(p) == DDERR_SURFACELOST);
    assert(RESTORE(p) == 0 && RESTORE(video) == 0);
    assert(LOST(back) == 0 &&
           ((struct dd_surface *)sys->owner)->dib.bits[0] == 73 &&
           ((struct dd_surface *)video->owner)->dib.bits[0] == 0);
    assert(dd_bytes == bytes && host_allocations == allocations);
  }
  DWORD before_unlock = frame_count;
  lock.old.size = sizeof(lock);
  assert(LOCK(p, 0, &lock.old, DDLOCK_WAIT) == 0);
  ((uint8_t *)lock.old.pixels)[0] = 37;
  assert(UNLOCK(p, 0) == 0);
  now += DD_SLOT_NS;
  w32_ddraw_tick();
  assert(frame_count > before_unlock);
  current.width = 800;
  current.token++;
  current.generation++;
  assert(LOST(p) == DDERR_SURFACELOST && RESTORE(p) == DDERR_WRONGMODE);
  current.width = 640;
  current.token++;
  current.generation++;
  assert(RESTORE(p) == 0);
  fclose(recording);
  RELEASE(sys);
  RELEASE(video);
  RELEASE(back);
  RELEASE(p);
  clean(d);
  puts("PASS state ten Alt+Tabs");
}
static void palettes(void) {
  DD *d = device();
  assert(COOP(d, 1, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN) == 0 &&
         SETMODE(d, 320, 200, 8) == 0);
  DD *p = primary(d, 0), *pal = 0;
  PALETTEENTRY entries[256];
  for (unsigned i = 0; i < 256; i++)
    entries[i] = (PALETTEENTRY){i, 255 - i, i ^ 73, 0};
  assert(METHOD(d, 5, DDRESULT (*)(DD *, DWORD, PALETTEENTRY *, DD **, void *))(
             d, DDPCAPS_8BIT | DDPCAPS_ALLOW256, entries, &pal, 0) == 0);
  assert(SETPAL(p, pal) == 0 && palette_count == 1);
  now += DD_SLOT_NS;
  DWORD frames = frame_count;
  entries[19] = (PALETTEENTRY){123, 45, 67, 0};
  assert(
      METHOD(pal, 6, DDRESULT (*)(DD *, DWORD, DWORD, DWORD, PALETTEENTRY *))(
          pal, 0, 19, 1, entries + 19) == 0);
  assert(palette_count == 2 && frame_count == frames + 1);
  assert(((struct dd_surface *)p->owner)->dib.colors[19] == 0x7b2d43);
  assert(
      METHOD(pal, 6, DDRESULT (*)(DD *, DWORD, DWORD, DWORD, PALETTEENTRY *))(
          pal, 0, 255, 2, entries) == DDERR_INVALIDPARAMS);
  RELEASE(pal);
  RELEASE(p);
  clean(d);
  puts("PASS palettes");
}
static void memory_test(void) {
  budget = 4096;
  DD *d = device();
  DD *surfaces[4];
  for (unsigned i = 0; i < 4; i++)
    surfaces[i] =
        surface(d, 32, 32, 8, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
  DDDESC2 desc = {0};
  desc.old = (DDDESC){.size = sizeof(desc),
                      .flags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT,
                      .width = 1,
                      .height = 1,
                      .caps = DDSCAPS_OFFSCREENPLAIN};
  DD *out = (void *)1;
  assert(CREATE(d, &desc.old, &out) == DDERR_OUTOFVIDEOMEMORY && out == 0);
  RELEASE(surfaces[0]);
  surfaces[0] = surface(d, 32, 32, 8, DDSCAPS_OFFSCREENPLAIN);
  for (unsigned i = 0; i < 4; i++)
    RELEASE(surfaces[i]);
  for (long fail = 0; fail < 8; fail++) {
    long n = host_allocations;
    host_fail_after = fail;
    out = 0;
    DDRESULT r = CREATE(d, &desc.old, &out);
    host_fail_after = -1;
    if (!r)
      RELEASE(out);
    assert(host_allocations == n);
  }
  clean(d);
  budget = 4 * 1024 * 1024;
  puts("PASS memory");
}
static void windowed(void) {
  DD *d = device();
  assert(COOP(d, 1, DDSCL_NORMAL) == 0);
  DD *p = primary(d, 0), *clipper = 0;
  DDFX fill = {.size = sizeof(fill), .fill = 0x345678};
  assert(BLT(p, 0, 0, 0, DDBLT_COLORFILL, &fill) == DDERR_NOCLIPLIST);
  assert(DirectDrawCreateClipper(0, &clipper, 0) == 0);
  assert(METHOD(clipper, 8, DDRESULT (*)(DD *, DWORD, uintptr_t))(clipper, 0,
                                                                  1) == 0);
  assert(METHOD(p, 28, DDRESULT (*)(DD *, DD *))(p, clipper) == 0);
  assert(BLT(p, 0, 0, 0, DDBLT_COLORFILL, &fill) == 0 && window_count == 1);
  struct dd_surface *surface = p->owner;
  assert(w32_dib_raw(&surface->dib, 3, 2) == 0x345678 &&
         w32_dib_raw(&surface->dib, 9, 2) == 0);
  DWORD bytes = 0;
  assert(METHOD(clipper, 3, DDRESULT (*)(DD *, RECT *, RGNDATA *, DWORD *))(
             clipper, 0, 0, &bytes) == 0 &&
         bytes == 64);
  assert(COOP(d, 1, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN) == 0 &&
         SETMODE(d, 640, 480, 32) == 0);
  DD *gamma_control = 0;
  assert(Q(p, &dd_gamma_iid, &gamma_control) == 0);
  DDGAMMA gamma_ramp = {0};
  assert(METHOD(gamma_control, 4, DDRESULT (*)(DD *, DWORD, const DDGAMMA *))(
             gamma_control, 0, &gamma_ramp) == 0 &&
         gamma_count == 1);
  RELEASE(gamma_control);
  RELEASE(clipper);
  RELEASE(p);
  clean(d);
  puts("PASS windowed clipping gamma");
}
static DWORD mode_count;
static DDRESULT mode_callback(DDDESC *desc, void *opaque) {
  (void)opaque;
  assert(desc->refresh == 60);
  assert(desc->pixel.bits == 8 || desc->pixel.bits == 16 ||
         desc->pixel.bits == 32);
  mode_count++;
  return 1;
}
static DDRESULT surface_callback(DD *surface, DDDESC *desc, void *opaque) {
  DWORD *count = opaque;
  assert(desc->width);
  (*count)++;
  RELEASE(surface);
  return 1;
}
static void operations(void) {
  DD *d = device();
  assert(COOP(d, 1, DDSCL_NORMAL) == 0);
  assert(METHOD(d, 8,
                DDRESULT (*)(DD *, DWORD, const DDDESC *, void *,
                             DDRESULT (*)(DDDESC *, void *)))(
             d, DDEDM_REFRESHRATES, 0, 0, mode_callback) == 0 &&
         mode_count >= 24);
  DWORD line = 0;
  assert(METHOD(d, 22, DDRESULT (*)(DD *, DWORD, void *))(d, 1, 0) == 0 &&
         now == DD_SLOT_NS);
  assert(METHOD(d, 16, DDRESULT (*)(DD *, DWORD *))(d, &line) == 0 &&
         line == 0);
  DD *s = surface(d, 13, 9, 8, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY),
     *t = surface(d, 13, 9, 8, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
  DWORD count = 0;
  assert(METHOD(d, 9,
                DDRESULT (*)(DD *, DWORD, const DDDESC *, void *,
                             DDRESULT (*)(DD *, DDDESC *, void *)))(
             d, 0x11, 0, &count, surface_callback) == 0 &&
         count == 2);
  assert(METHOD(s, 3, DDRESULT (*)(DD *, DD *))(s, t) == 0);
  DWORD caps = DDSCAPS_OFFSCREENPLAIN;
  DD *attached = 0;
  assert(ATTACHED(s, &caps, &attached) == 0 && attached == t);
  RELEASE(attached);
  assert(METHOD(s, 8, DDRESULT (*)(DD *, DWORD, DD *))(s, 0, t) == 0);
  DDGUID key = {1, 2, 3, {4}};
  DWORD payload = 0x12345678, copy = 0, bytes = 4;
  assert(
      METHOD(s, 40, DDRESULT (*)(DD *, const DDGUID *, void *, DWORD, DWORD))(
          s, &key, &payload, 4, DDSPD_VOLATILE) == 0);
  assert(METHOD(s, 41, DDRESULT (*)(DD *, const DDGUID *, void *, DWORD *))(
             s, &key, &copy, &bytes) == 0 &&
         copy == payload);
  assert(METHOD(s, 44, DDRESULT (*)(DD *))(s) == 0);
  assert(METHOD(s, 41, DDRESULT (*)(DD *, const DDGUID *, void *, DWORD *))(
             s, &key, &copy, &bytes) == DDERR_EXPIRED);
  assert(METHOD(s, 42, DDRESULT (*)(DD *, const DDGUID *))(s, &key) == 0);
  uint8_t client[9 * 20 + 64];
  memset(client, 0xa5, sizeof(client));
  DDDESC2 desc = {0};
  desc.old = (DDDESC){.size = sizeof(desc),
                      .flags = DDSD_LPSURFACE | DDSD_PITCH,
                      .pitch = 20,
                      .pixels = client + 32};
  assert(METHOD(s, 39, DDRESULT (*)(DD *, const DDDESC *, DWORD))(s, &desc.old,
                                                                  0) == 0);
  assert(METHOD(s, 3, DDRESULT (*)(DD *, DD *))(s, t) == DDERR_INVALIDCAPS);
  assert(METHOD(s, 37, DDRESULT (*)(DD *, DWORD))(s, 0) == 0 &&
         METHOD(s, 38, DDRESULT (*)(DD *, DWORD))(s, 0) == 0);
  DDFX fill = {.size = sizeof(fill), .fill = 17};
  assert(BLT(s, 0, 0, 0, DDBLT_COLORFILL, &fill) == 0);
  assert(
      METHOD(t, 7, DDRESULT (*)(DD *, DWORD, DWORD, DD *, const RECT *, DWORD))(
          t, 0, 0, s, 0, DDBLTFAST_WAIT) == 0);
  struct dd_surface *target = t->owner;
  assert(target->dib.bits[0] == 17 && target->dib.bits[12] == 17);
  for (unsigned i = 0; i < 32; i++)
    assert(client[i] == 0xa5 && client[9 * 20 + 32 + i] == 0xa5);
  HDC dc = 0;
  assert(METHOD(t, 17, DDRESULT (*)(DD *, HDC *))(t, &dc) == 0);
  DD *from = 0;
  assert(METHOD(d, 24, DDRESULT (*)(DD *, HDC, DD **))(d, dc, &from) == 0 &&
         from == t);
  RELEASE(from);
  assert(METHOD(t, 26, DDRESULT (*)(DD *, HDC))(t, dc) == 0);
  DD *parent = 0;
  assert(METHOD(t, 36, DDRESULT (*)(DD *, void **))(t, (void **)&parent) == 0 &&
         parent == d);
  RELEASE(parent);
  assert(METHOD(s, 6, DDRESULT (*)(DD *, void *, DWORD, DWORD))(s, 0, 0, 0) ==
         DDERR_UNSUPPORTED);
  RELEASE(s);
  RELEASE(t);
  clean(d);
  puts("PASS modes clippers private data client storage bltfast");
}
static void shims(void) {
  host_shim_mask((1u << 3) | (1u << 1));
  DD *d = device();
  assert(COOP(d, 1, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN) == 0 &&
         SETMODE(d, 640, 480, 16) == 0);
  DD *p = primary(d, 1);
  assert(((struct dd_surface *)p->owner)->dib.masks[0] == 0x7c00);
  assert(((struct dd_device *)d->owner)->windowed_shim);
  RELEASE(p);
  clean(d);
  host_shim_mask(0);
  puts("PASS windowed and 555 shims");
}
static DWORD pattern(unsigned x, unsigned y, unsigned seed, DWORD bpp) {
  DWORD v = (x * 37 + y * 61 + seed * 19) ^ (x * y * 13 + seed * 71);
  return bpp == 8    ? v & 255
         : bpp == 16 ? v * 149 & 0xffff
                     : v * 0x010307u & 0xffffff;
}
static void authored(DD *surface, unsigned seed) {
  struct dd_surface *s = surface->owner;
  memset(s->dib.bits, 0xa5, s->dib.size);
  for (DWORD y = 0; y < s->dib.height; y++)
    for (DWORD x = 0; x < s->dib.width; x++)
      w32_dib_put(&s->dib, x, y, pattern(x, y, seed, s->dib.bpp));
}
static void frame_cases(const char *folder) {
  DD *d = device();
  char path[1024];
  for (DWORD bpp = 8; bpp <= 32; bpp *= 2)
    for (unsigned variant = 0; variant < 27; variant++) {
      DD *s = surface(d, 13, 9, bpp,
                      DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY),
         *t = surface(d, 13, 9, bpp,
                      DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
      authored(s, 1);
      authored(t, 3);
      RECT sr = {1, 1, 12, 8}, dr = {0, 0, 13, 9};
      DWORD flags = DDBLT_WAIT;
      DDFX fx = {.size = sizeof(fx)};
      DD *input = s;
      if (variant < 16) {
        flags |= DDBLT_ROP;
        fx.rop = ((variant << 4) | variant) << 16;
      } else if (variant == 16) {
        input = t;
        sr = (RECT){0, 0, 10, 7};
        dr = (RECT){2, 1, 12, 8};
      } else if (variant == 17) {
        DDKEY key = {pattern(3, 2, 1, bpp), pattern(3, 2, 1, bpp)};
        assert(KEY(s, DDCKEY_SRCBLT, &key) == 0);
        flags |= DDBLT_KEYSRC;
      } else if (variant == 18) {
        DDKEY key = {0, bpp == 8 ? 120 : bpp == 16 ? 30000 : 0x800000};
        assert(KEY(t, DDCKEY_DESTBLT | DDCKEY_COLORSPACE, &key) == 0);
        flags |= DDBLT_KEYDEST;
      } else if (variant == 19) {
        flags |= DDBLT_KEYSRCOVERRIDE | DDBLT_KEYDESTOVERRIDE;
        fx.srckey = (DDKEY){0, bpp == 8 ? 120 : bpp == 16 ? 30000 : 0x800000};
        fx.destkey = fx.srckey;
      } else if (variant == 20) {
        dr = (RECT){-3, -2, 16, 11};
      } else if (variant == 21 || variant == 22 || variant == 23) {
        flags |= DDBLT_DDFX;
        fx.effects = variant == 21 ? 2 : variant == 22 ? 4 : 6;
      } else if (variant == 24) {
        DD *clipper = 0;
        assert(DirectDrawCreateClipper(0, &clipper, 0) == 0);
        assert(METHOD(clipper, 8, DDRESULT (*)(DD *, DWORD, uintptr_t))(
                   clipper, 0, 1) == 0);
        assert(METHOD(t, 28, DDRESULT (*)(DD *, DD *))(t, clipper) == 0);
        RELEASE(clipper);
      } else if (variant == 25) {
        flags |= DDBLT_COLORFILL;
        fx.fill = 0x3b4c5d;
        input = 0;
      } else {
        input = t;
        sr = (RECT){2, 1, 12, 8};
        dr = (RECT){0, 0, 13, 9};
        flags |= DDBLT_DDFX;
        fx.effects = 6;
      }
      assert(BLT(t, &dr, input, &sr, flags, &fx) == 0);
      struct dd_surface *v = t->owner;
      snprintf(path, sizeof(path), "%s/%u-%02u.bin", folder, bpp, variant);
      FILE *file = fopen(path, "wb");
      assert(file && fwrite(v->dib.bits, 1, v->dib.size, file) == v->dib.size);
      fclose(file);
      printf("FRAME %u-%02u %u %u %u %u\n", bpp, variant, v->dib.width,
             v->dib.height, v->dib.pitch, v->dib.size);
      host_canaries();
      RELEASE(s);
      RELEASE(t);
    }
  clean(d);
  puts("PASS frames");
}
static void game_scenes(const char *folder) {
  for (DWORD bpp = 8; bpp <= 32; bpp *= 2)
    for (unsigned fullscreen = 0; fullscreen < 2; fullscreen++) {
      DD *d = device();
      assert(COOP(d, 1,
                  fullscreen ? DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN
                             : DDSCL_NORMAL) == 0);
      if (fullscreen)
        assert(SETMODE(d, 320, 200, bpp) == 0);
      DD *p = primary(d, fullscreen ? 3 : 0),
         *s = surface(d, 13, 9, bpp,
                      DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
      if (!fullscreen) {
        DD *clipper = 0;
        assert(DirectDrawCreateClipper(0, &clipper, 0) == 0);
        assert(METHOD(clipper, 8, DDRESULT (*)(DD *, DWORD, uintptr_t))(
                   clipper, 0, 1) == 0);
        assert(METHOD(p, 28, DDRESULT (*)(DD *, DD *))(p, clipper) == 0);
        RELEASE(clipper);
      }
      authored(s, 1);
      DDKEY key = {pattern(3, 2, 1, bpp), pattern(3, 2, 1, bpp)};
      assert(KEY(s, DDCKEY_SRCBLT, &key) == 0);
      RECT destination = {2, 1, 15, 10};
      assert(BLT(p, &destination, s, 0, DDBLT_KEYSRC | DDBLT_WAIT, 0) == 0);
      HDC dc = 0;
      assert(METHOD(p, 17, DDRESULT (*)(DD *, HDC *))(p, &dc) == 0);
      assert(SetPixel(dc, 4, 5, RGB(255, 255, 255)) != CLR_INVALID);
      assert(METHOD(p, 26, DDRESULT (*)(DD *, HDC))(p, dc) == 0);
      now += DD_SLOT_NS;
      w32_ddraw_tick();
      struct dd_surface *image = p->owner;
      char name[1024];
      snprintf(name, sizeof(name), "%s/game-%s-%u.bin", folder,
               fullscreen ? "full" : "window", bpp);
      FILE *file = fopen(name, "wb");
      assert(file && fwrite(image->dib.bits, 1, image->dib.size, file) ==
                         image->dib.size);
      fclose(file);
      host_canaries();
      RELEASE(s);
      RELEASE(p);
      clean(d);
    }
  puts("PASS authored fixture game scenes 8 16 32 windowed fullscreen");
}
static void *concurrent_worker(void *opaque) {
  (void)opaque;
  DD *d = device();
  assert(COOP(d, 1, DDSCL_NORMAL) == 0);
  for (unsigned i = 0; i < 200; i++) {
    DD *s = surface(d, 13, 9, 8, DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY);
    DDDESC2 desc = {0};
    desc.old.size = sizeof(desc);
    assert(LOCK(s, 0, &desc.old, DDLOCK_WAIT) == 0);
    ((uint8_t *)desc.old.pixels)[0] = 42;
    assert(UNLOCK(s, 0) == 0);
    RELEASE(s);
  }
  RELEASE(d);
  return 0;
}
static void concurrency(void) {
  pthread_t threads[4];
  for (unsigned i = 0; i < 4; i++)
    assert(!pthread_create(&threads[i], 0, concurrent_worker, 0));
  for (unsigned i = 0; i < 4; i++)
    assert(!pthread_join(threads[i], 0));
  w32_ddraw_process_exit();
  host_canaries();
  assert(!host_allocations && !host_bytes && !dd_bytes);
  puts("PASS concurrent lifetimes");
}
static void callback_lifetime(void) {
  DD *d = device();
  assert(COOP(d, 1, DDSCL_NORMAL) == 0);
  DD *p = primary(d, 0), *clipper = 0;
  assert(DirectDrawCreateClipper(0, &clipper, 0) == 0);
  assert(METHOD(clipper, 8, DDRESULT (*)(DD *, DWORD, uintptr_t))(clipper, 0,
                                                                  1) == 0);
  assert(METHOD(p, 28, DDRESULT (*)(DD *, DD *))(p, clipper) == 0);
  RELEASE(clipper);
  DDFX fill = {.size = sizeof(fill), .fill = 0x375564};
  assert(BLT(p, 0, 0, 0, DDBLT_COLORFILL, &fill) == 0);
  assert(BLT(p, 0, 0, 0, DDBLT_COLORFILL, &fill) == 0);
  release_window_surface = p;
  release_window_device = d;
  now += DD_SLOT_NS;
  w32_ddraw_tick();
  assert(!release_window_surface && window_count == 2);
  w32_ddraw_process_exit();
  w32_gdi_process_exit();
  host_canaries();
  assert(!host_allocations && !host_bytes && !dd_bytes);
  puts("PASS reentrant USER callback lifetime");
}
static void benchmark(void) {
  DD *d = device();
  assert(COOP(d, 1, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN) == 0 &&
         SETMODE(d, 640, 480, 8) == 0);
  DD *p = primary(d, 1);
  benchmark_enabled = 1;
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  for (unsigned i = 0; i < 1000; i++) {
    now += DD_SLOT_NS;
    assert(FLIP(p, 0, DDFLIP_WAIT) == 0);
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  benchmark_enabled = 0;
  uint64_t elapsed = (uint64_t)(end.tv_sec - start.tv_sec) * 1000000000ull +
                     end.tv_nsec - start.tv_nsec;
  printf("BENCH flip_640480x8_scalar_ns=%llu iterations=1000 bytes=307200\n",
         (unsigned long long)(elapsed / 1000));
  RELEASE(p);
  clean(d);
  puts("PASS benchmark");
}
int main(int argc, char **argv) {
  assert(argc >= 2);
  w32_ddraw_set_backend(&backend);
  if (!strcmp(argv[1], "games")) {
    assert(argc == 3);
    game_scenes(argv[2]);
  } else if (!strcmp(argv[1], "callback-lifetime"))
    callback_lifetime();
  else if (!strcmp(argv[1], "concurrency"))
    concurrency();
  else if (!strcmp(argv[1], "benchmark"))
    benchmark();
  else if (!strcmp(argv[1], "abi"))
    abi();
  else if (!strcmp(argv[1], "state")) {
    assert(argc == 3);
    state(argv[2]);
  } else if (!strcmp(argv[1], "palettes"))
    palettes();
  else if (!strcmp(argv[1], "memory"))
    memory_test();
  else if (!strcmp(argv[1], "windowed"))
    windowed();
  else if (!strcmp(argv[1], "operations"))
    operations();
  else if (!strcmp(argv[1], "shims"))
    shims();
  else if (!strcmp(argv[1], "frames")) {
    assert(argc == 3);
    frame_cases(argv[2]);
  } else
    abort();
  return 0;
}
