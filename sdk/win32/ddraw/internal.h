/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef CIUKI_DD_INTERNAL_H
#define CIUKI_DD_INTERNAL_H
#include <w32/ddraw.h>
#include <w32/services.h>
#define DD_SLOT_NS 16666667ull
#define DD_MAX_RECTS 1024u
#define API W32API
struct dd_object {
  struct dd_object *next;
  DWORD kind, refs, bytes;
  DD interfaces[7];
};
struct dd_device;
struct dd_palette {
  struct dd_object base;
  DWORD flags;
  PALETTEENTRY colors[256];
};
struct dd_clipper {
  struct dd_object base;
  uintptr_t hwnd;
  RECT *rects;
  DWORD count, changed, initialized;
};
struct dd_private {
  struct dd_private *next;
  DDGUID key;
  DWORD size, flags, uniqueness;
  void *data;
};
struct dd_surface {
  struct dd_object base;
  struct dd_surface *next, *attached[3];
  struct dd_device *device;
  DWORD count, implicit, caps, lost, locked, lock_flags, busy, uniqueness;
  struct w32_dib dib;
  void *allocation, *lock_pointer;
  DWORD allocation_bytes, charge;
  RECT lock_rect;
  DDKEY source_key, dest_key;
  DWORD keys;
  struct dd_palette *palette;
  struct dd_clipper *clipper;
  HDC dc;
  struct dd_private *private_data;
  DDCOLOR color;
  DDGAMMA gamma;
};
struct dd_device {
  struct dd_object base;
  struct dd_device *next;
  struct dd_surface *surfaces, *primary;
  uintptr_t hwnd;
  DWORD coop, mode_set, generation, dirty, windowed_shim;
  DWORD palette_pending, gamma_pending;
  struct dd_display display;
  struct dd_mode mode;
  struct dd_lease lease;
  uint64_t next_frame;
};
extern const void *const dd_device_vt[4][30];
extern const void *const dd_surface_vt[5][49];
extern const void *const dd_palette_vt[],
    *const dd_clipper_vt[], *const dd_gamma_vt[], *const dd_color_vt[];
extern const struct dd_backend dd_native_backend;
extern const struct dd_backend *dd_backend;
extern struct dd_device *dd_devices;
extern DWORD dd_bytes, dd_budget;
void dd_lock(void);
void dd_unlock(int *);
DWORD dd_suspend(void);
void dd_resume(DWORD);
#define GUARD int dd_guard __attribute__((cleanup(dd_unlock))) = (dd_lock(), 0)
void *dd_alloc(DWORD);
void dd_free(void *);
void dd_init(struct dd_object *, DWORD, DWORD, const DWORD *,
             const void *const *const *, DWORD);
DDRESULT API dd_query(DD *, const DDGUID *, void **);
DWORD API dd_add(DD *);
DWORD API dd_release(DD *);
BOOL dd_equal(const DDGUID *, const DDGUID *);
void dd_sync(struct dd_device *);
void dd_lose(struct dd_device *);
void dd_device_finalized(struct dd_device *);
void dd_native_start(void);
void dd_native_stop(void);
DDRESULT dd_native_window_mode(uintptr_t, DWORD, DWORD);
DDRESULT dd_present(struct dd_surface *, BOOL, DWORD);
DDRESULT dd_create_surface(DD *, const DDDESC *, DD **, void *);
DDRESULT dd_create_palette(DD *, DWORD, PALETTEENTRY *, DD **, void *);
DDRESULT dd_create_clipper(DD *, DWORD, DD **, void *);
void dd_palette_update(struct dd_palette *, DWORD, DWORD);
void dd_surface_destroy(struct dd_surface *);
DDRESULT dd_description(DD *, DDDESC *, BOOL);
DDRESULT dd_surface_ready(struct dd_surface *, DWORD, DWORD);
BOOL dd_rect_valid(RECT, const struct w32_dib *);
DDRESULT dd_clip_rects(struct dd_clipper *, RECT *, DWORD *);
void dd_set_format(struct w32_dib *, DWORD);
void dd_pixel_format(const struct w32_dib *, DDPF *);
#endif
