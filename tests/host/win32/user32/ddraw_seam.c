/* f5-11 DirectDraw through its production native adapter (sdk/win32/ddraw/
 * native.c) onto the production USER display seam and the production gdi32
 * engine. Only the desktop and kernel transports are fixtures.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "../../../../sdk/win32/ddraw/internal.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "DDRAW SEAM CHECK %s:%d: %s\n", __FILE__, __LINE__, #x); \
      abort();                                                                 \
    }                                                                          \
  } while (0)
#define METHOD(o, n, signature) ((signature)((o)->vt[n]))
#define RELEASE(o) METHOD(o, 2, DWORD (*)(DD *))(o)
#define COOP(o, h, f)                                                          \
  METHOD(o, 20, DDRESULT (*)(DD *, uintptr_t, DWORD))(o, h, f)
#define SETMODE(o, w, h, b)                                                    \
  METHOD(o, 21, DDRESULT (*)(DD *, DWORD, DWORD, DWORD, DWORD, DWORD))(        \
      o, w, h, b, 60, 0)
#define CREATE(o, desc, out)                                                   \
  METHOD(o, 6, DDRESULT (*)(DD *, const DDDESC *, DD **, void *))(o, desc,     \
                                                                  out, 0)
#define LOCK(o, r, desc, f)                                                    \
  METHOD(o, 25, DDRESULT (*)(DD *, const RECT *, DDDESC *, DWORD, void *))(    \
      o, r, desc, f, 0)
#define UNLOCK(o, r) METHOD(o, 32, DDRESULT (*)(DD *, const void *))(o, r)
#define LOST(o) METHOD(o, 24, DDRESULT (*)(DD *))(o)
#define RESTORE(o) METHOD(o, 27, DDRESULT (*)(DD *))(o)
#define FLIP(o, t, f) METHOD(o, 11, DDRESULT (*)(DD *, DD *, DWORD))(o, t, f)
#define ATTACHED(o, cap, out)                                                  \
  METHOD(o, 12, DDRESULT (*)(DD *, const DWORD *, DD **))(o, cap, out)
struct seam_hooks {
  void (*deactivate)(void);
  void (*regrant)(void);
  void (*hold_user)(int);
  DWORD (*frames)(DWORD *token, DWORD *generation, DWORD *refused);
  DWORD (*lease_token)(void);
  uintptr_t (*cover)(void);
  uint32_t (*pixel)(uintptr_t hwnd, int x, int y);
};
static DD *primary(DD *d, DWORD back) {
  DDDESC2 desc = {0};
  desc.old = (DDDESC){.size = sizeof(desc),
                      .flags = DDSD_CAPS | (back ? DDSD_BACKBUFFERCOUNT : 0),
                      .caps = DDSCAPS_PRIMARYSURFACE |
                              (back ? DDSCAPS_FLIP | DDSCAPS_COMPLEX : 0),
                      .backbuffers = back};
  DD *s = 0;
  DDRESULT r = CREATE(d, &desc.old, &s);
  if (r)
    fprintf(stderr, "CreateSurface primary back=%u: %08x\n", back, (unsigned)r);
  CHECK(!r);
  return s;
}
static void paint(DD *s, uint32_t value, DWORD bytes_per_pixel) {
  DDDESC2 lock = {0};
  lock.old.size = sizeof(lock);
  CHECK(LOCK(s, 0, &lock.old, DDLOCK_WAIT) == 0);
  for (DWORD y = 0; y < lock.old.height; y++)
    for (DWORD x = 0; x < lock.old.width; x++)
      memcpy((uint8_t *)lock.old.pixels + y * lock.old.pitch +
                 x * bytes_per_pixel,
             &value, bytes_per_pixel);
  CHECK(UNLOCK(s, 0) == 0);
}
static volatile int ticking;
static void *tick_worker(void *unused) {
  (void)unused;
  /* The pacing worker: polls USER's cached lease through dd_sync. */
  for (unsigned i = 0; i < 200; i++)
    w32_ddraw_tick();
  __atomic_store_n(&ticking, 0, __ATOMIC_RELEASE);
  return 0;
}
void host_ddraw_seam(uintptr_t hwnd, const struct seam_hooks *h) {
  DWORD token = 0, generation = 0, refused = 0;
  DD *d = 0;
  CHECK(DirectDrawCreateEx(0, (void **)&d, &dd_iids[3], 0) == 0);
  CHECK(COOP(d, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN) == 0);
  /* SetDisplayMode leases through w32_user_display_lease on this thread. */
  CHECK(SETMODE(d, 320, 200, 8) == 0);
  DWORD first = h->lease_token();
  DD *p = primary(d, 1), *back = 0;
  DWORD caps = DDSCAPS_BACKBUFFER;
  CHECK(ATTACHED(p, &caps, &back) == 0);
  DWORD frames = h->frames(&token, &generation, &refused);
  paint(back, 0x5a, 1);
  CHECK(FLIP(p, 0, DDFLIP_WAIT) == 0);
  CHECK(h->frames(&token, &generation, &refused) == frames + 1 &&
        token == first && !refused);
  /* The pacing worker reads cached state while the GUI holds the USER lock. */
  paint(p, 0x21, 1);
  __atomic_store_n(&ticking, 1, __ATOMIC_RELEASE);
  h->hold_user(1);
  /* The native adapter passes 32-bit record addresses: a low worker stack. */
  pthread_t thread;
  pthread_attr_t attr;
  void *stack = mmap(0, 1u << 20, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  CHECK(stack != MAP_FAILED && !pthread_attr_init(&attr) &&
        !pthread_attr_setstack(&attr, stack, 1u << 20));
  CHECK(!pthread_create(&thread, &attr, tick_worker, 0));
  for (unsigned i = 0;
       i < 5000 && __atomic_load_n(&ticking, __ATOMIC_ACQUIRE); i++)
    usleep(1000);
  CHECK(!__atomic_load_n(&ticking, __ATOMIC_ACQUIRE));
  h->hold_user(0);
  CHECK(!pthread_join(thread, 0) && !pthread_attr_destroy(&attr));
  munmap(stack, 1u << 20);
  CHECK(h->frames(&token, &generation, &refused) > frames + 1 && !refused);
  /* Deactivation on the owner thread: the next poll loses the surfaces. */
  h->deactivate();
  CHECK(LOST(p) == DDERR_SURFACELOST && LOST(back) == DDERR_SURFACELOST);
  CHECK(RESTORE(p) == DDERR_NOEXCLUSIVEMODE);
  /* Same-mode regrant: a new token; Restore is still required. */
  h->regrant();
  CHECK(h->lease_token() != first);
  CHECK(LOST(p) == DDERR_SURFACELOST && RESTORE(p) == 0 && LOST(back) == 0);
  frames = h->frames(&token, &generation, &refused);
  paint(back, 0x33, 1);
  CHECK(FLIP(p, 0, DDFLIP_WAIT) == 0);
  CHECK(h->frames(&token, &generation, &refused) == frames + 1 &&
        token == h->lease_token() && !refused);
  RELEASE(back);
  RELEASE(p);
  /* Windowed: release the lease, clip lists and publication through USER
   * GetDC/ClientToScreen and gdi32 StretchDIBits into the window surface. */
  CHECK(COOP(d, hwnd, DDSCL_NORMAL) == 0);
  DD *w = primary(d, 0), *clipper = 0;
  CHECK(DirectDrawCreateClipper(0, &clipper, 0) == 0);
  CHECK(METHOD(clipper, 8, DDRESULT (*)(DD *, DWORD, uintptr_t))(clipper, 0,
                                                                  hwnd) == 0);
  CHECK(METHOD(w, 28, DDRESULT (*)(DD *, DD *))(w, clipper) == 0);
  DWORD bytes = 0;
  CHECK(METHOD(clipper, 3, DDRESULT (*)(DD *, RECT *, void *, DWORD *))(
            clipper, 0, 0, &bytes) == 0 &&
        bytes == 32 + 16);
  uintptr_t cover = h->cover();
  CHECK(cover);
  CHECK(METHOD(clipper, 3, DDRESULT (*)(DD *, RECT *, void *, DWORD *))(
            clipper, 0, 0, &bytes) == 0 &&
        bytes == 32 + 4 * 16);
  paint(w, 0x00336699, 4);
  w32_ddraw_tick();
  CHECK(h->pixel(hwnd, 10, 10) == 0x336699);
  RELEASE(clipper);
  RELEASE(w);
  CHECK(!RELEASE(d));
  w32_ddraw_process_exit();
  printf("TRACE ddraw on USER seam: exclusive lease, flip frames with USER "
         "token, lock-free pacing worker, deactivation loss, same-mode regrant "
         "and Restore, windowed clip list and GDI publication\n");
}
