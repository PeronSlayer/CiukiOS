/* Display objects, mode enumeration and lease state. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include "internal.h"
#include <w32/shims.h>
static struct dd_device *exclusive;
static void describe_mode(const struct dd_mode *m, DDDESC2 *out,
                          DWORD generation) {
  w32_fill(out, 0, sizeof(*out));
  out->old.size = generation >= 4 ? sizeof(*out) : sizeof(out->old);
  out->old.flags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PITCH | DDSD_PIXELFORMAT |
                   DDSD_REFRESHRATE;
  out->old.width = m->width;
  out->old.height = m->height;
  out->old.pitch = (m->width * (m->bpp / 8) + 3) & ~3u;
  out->old.refresh = 60;
  struct w32_dib dib = {0};
  dd_set_format(&dib, m->bpp);
  dd_pixel_format(&dib, &out->old.pixel);
}
static BOOL mode_supported(struct dd_device *d, DWORD w, DWORD h, DWORD bpp) {
  if (!w || !h || (bpp != 8 && bpp != 16 && bpp != 32))
    return 0;
  BOOL listed = 0;
  const DWORD logical[][2] = {{320, 200}, {320, 240}, {400, 300}, {512, 384},
                              {640, 400}, {640, 480}, {800, 600}, {1024, 768}};
  for (unsigned i = 0; i < 8; i++)
    if (logical[i][0] == w && logical[i][1] == h)
      listed = 1;
  for (DWORD i = 0; i < d->display.count; i++)
    if (d->display.modes[i].width == w && d->display.modes[i].height == h)
      listed = 1;
  if (!listed)
    return 0;
  for (DWORD i = 0; i < d->display.count; i++) {
    struct dd_mode *m = &d->display.modes[i];
    if (m->width >= w && m->height >= h && (m->bpp == 16 || m->bpp == 32))
      return 1;
  }
  return 0;
}
void dd_lose(struct dd_device *d) {
  d->lease.active = 0;
  d->dirty = 0;
  for (struct dd_surface *s = d->surfaces; s; s = s->next)
    if (!((s->caps & DDSCAPS_SYSTEMMEMORY) &&
          (s->caps & DDSCAPS_OFFSCREENPLAIN) && !s->implicit))
      s->lost = 1;
}
void dd_sync(struct dd_device *d) {
  if (!(d->coop & DDSCL_EXCLUSIVE) || d->windowed_shim || !d->mode_set ||
      !dd_backend->poll)
    return;
  struct dd_lease now = d->lease;
  dd_backend->poll(d->hwnd, &now);
  if (d->lease.active &&
      (!now.active || now.token != d->lease.token ||
       now.generation != d->lease.generation || now.width != d->mode.width ||
       now.height != d->mode.height || now.bpp != d->mode.bpp))
    dd_lose(d);
  BOOL regrant =
      now.active && (!d->lease.active || now.token != d->lease.token ||
                     now.generation != d->lease.generation);
  d->lease = now;
  if (regrant && d->primary) {
    struct dd_surface *s = d->primary;
    if (s->palette && dd_backend->palette)
      d->palette_pending =
          dd_backend->palette(&d->lease, 0, 256, s->dib.colors) != 0;
    if (dd_backend->gamma)
      d->gamma_pending = dd_backend->gamma(&d->lease, &s->gamma) != 0;
  }
}
static DDRESULT API compact(DD *c) {
  (void)c;
  return 0;
}
static DDRESULT API create_surface(DD *c, const DDDESC *desc, DD **out,
                                   void *outer) {
  GUARD;
  return dd_create_surface(c, desc, out, outer);
}
static DDRESULT API create_palette(DD *c, DWORD flags, PALETTEENTRY *entries,
                                   DD **out, void *outer) {
  GUARD;
  return dd_create_palette(c, flags, entries, out, outer);
}
static DDRESULT API create_clipper(DD *c, DWORD flags, DD **out, void *outer) {
  GUARD;
  return dd_create_clipper(c, flags, out, outer);
}
static DDRESULT API duplicate(DD *c, DD *source, DD **out) {
  GUARD;
  (void)c;
  (void)source;
  if (out)
    *out = 0;
  return DDERR_UNSUPPORTED;
}
static DDRESULT API enum_modes(DD *c, DWORD flags, const DDDESC *filter,
                               void *context,
                               DDRESULT(API *callback)(DDDESC *, void *)) {
  GUARD;
  struct dd_device *d = c->owner;
  if (!callback || (flags & ~3u))
    return DDERR_INVALIDPARAMS;
  if (filter &&
      filter->size != (c->generation >= 4 ? sizeof(DDDESC2) : sizeof(DDDESC)))
    return DDERR_INVALIDPARAMS;
  struct dd_mode modes[128];
  DWORD count = 0;
  for (DWORD i = 0; i < d->display.count && count < 128; i++)
    modes[count++] = d->display.modes[i];
  const DWORD sizes[][2] = {{320, 200}, {320, 240}, {400, 300}, {512, 384},
                            {640, 400}, {640, 480}, {800, 600}, {1024, 768}};
  for (unsigned i = 0; i < 8; i++)
    for (DWORD b = 8; b <= 32; b *= 2) {
      if (!mode_supported(d, sizes[i][0], sizes[i][1], b))
        continue;
      BOOL found = 0;
      for (DWORD k = 0; k < count; k++)
        if (modes[k].width == sizes[i][0] && modes[k].height == sizes[i][1] &&
            modes[k].bpp == b)
          found = 1;
      if (!found && count < 128)
        modes[count++] = (struct dd_mode){sizes[i][0], sizes[i][1], b};
    }
  for (DWORD i = 0; i < count; i++) {
    struct dd_mode *m = &modes[i];
    if (filter &&
        (((filter->flags & DDSD_WIDTH) && filter->width != m->width) ||
         ((filter->flags & DDSD_HEIGHT) && filter->height != m->height) ||
         ((filter->flags & DDSD_PIXELFORMAT) && filter->pixel.bits != m->bpp) ||
         ((filter->flags & DDSD_REFRESHRATE) && filter->refresh &&
          filter->refresh != 60)))
      continue;
    DDDESC2 desc;
    describe_mode(m, &desc, c->generation);
    if (!(flags & DDEDM_REFRESHRATES))
      desc.old.flags &= ~DDSD_REFRESHRATE;
    if (!callback(&desc.old, context))
      break;
  }
  return 0;
}
static DDRESULT API enum_surfaces(DD *c, DWORD flags, const DDDESC *filter,
                                  void *context,
                                  DDRESULT(API *callback)(DD *, DDDESC *,
                                                          void *)) {
  GUARD;
  struct dd_device *d = c->owner;
  if (!callback || !(flags & 0x10u) || (flags & ~0x1fu))
    return DDERR_INVALIDPARAMS;
  if (filter &&
      filter->size != (c->generation >= 4 ? sizeof(DDDESC2) : sizeof(DDDESC)))
    return DDERR_INVALIDPARAMS;
  /* Retain a snapshot: callbacks may release or create surfaces. */
  DWORD count = 0;
  for (struct dd_surface *s = d->surfaces; s; s = s->next)
    count++;
  DD **list = dd_alloc(count * sizeof(*list));
  if (!list && count)
    return DDERR_OUTOFMEMORY;
  DWORD n = 0, index = c->generation == 7 ? 4 : c->generation == 4 ? 3 : 0;
  for (struct dd_surface *s = d->surfaces; s; s = s->next) {
    list[n++] = &s->base.interfaces[index];
    dd_add(list[n - 1]);
  }
  for (DWORD i = 0; i < n; i++) {
    DDDESC2 desc = {0};
    desc.old.size = c->generation >= 4 ? sizeof(desc) : sizeof(desc.old);
    dd_description(list[i], &desc.old, 0);
    BOOL match =
        !filter ||
        (!((filter->flags & DDSD_WIDTH) && filter->width != desc.old.width) &&
         !((filter->flags & DDSD_HEIGHT) &&
           filter->height != desc.old.height) &&
         !((filter->flags & DDSD_CAPS) &&
           (desc.old.caps & filter->caps) != filter->caps));
    BOOL select =
        (flags & 1u) || ((flags & 2u) && match) || ((flags & 4u) && !match);
    if (select) {
      dd_add(list[i]);
      if (!callback(list[i], &desc.old, context)) {
        for (DWORD j = i; j < n; j++)
          dd_release(list[j]);
        dd_free(list);
        return 0;
      }
    }
    dd_release(list[i]);
  }
  dd_free(list);
  return 0;
}
static DDRESULT API get_caps(DD *c, void *hardware, void *software) {
  GUARD;
  (void)c;
  if (!hardware && !software)
    return DDERR_INVALIDPARAMS;
  void *targets[2] = {hardware, software};
  for (unsigned i = 0; i < 2; i++)
    if (targets[i]) {
      DWORD size = w32_u32(targets[i]);
      if (size != 172 && size != 316 && size != 364 && size != 380)
        return DDERR_INVALIDPARAMS;
    }
  for (unsigned i = 0; i < 2; i++)
    if (targets[i]) {
      DWORD *p = targets[i], size = *p;
      w32_fill(p, 0, size);
      p[0] = size;
      /* BLT, BLTCOLORFILL, BLTSTRETCH, CANBLTSYSMEM, COLORKEY, PALETTE. */
      p[1] = 0x00000040u | 0x04000000u | 0x00000200u | 0x80000000u |
             0x00400000u | 0x00008000u | 0x60000000u;
      p[3] = 0x00000001u | 0x00000002u | 0x00000100u |
             0x00000200u; /* destination/source/range */
      p[2] = 0x00000800u | 0x00020000u | 0x00080000u;
      p[4] = 0x00000040u | 0x00000080u | 0x00000400u | 0x00000800u |
             0x00001000u | 0x00002000u | 0x00004000u | 0x00008000u |
             0x00010000u | 0x00020000u;
      p[6] = 0x00000004u | 0x00000010u | 0x00000040u;
      p[15] = dd_budget;
      p[16] = dd_budget - dd_bytes;
      const unsigned rops[] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
      for (unsigned k = 0; k < 16; k++)
        p[25 + rops[k] / 32] |= 1u << (rops[k] % 32);
      p[33] = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN |
              DDSCAPS_SYSTEMMEMORY | DDSCAPS_VIDEOMEMORY | DDSCAPS_FLIP |
              DDSCAPS_COMPLEX | DDSCAPS_BACKBUFFER | DDSCAPS_FRONTBUFFER;
      if (size >= 316)
        for (unsigned k = 0; k < 3; k++) {
          unsigned at = 43 + k * 11;
          p[at] = p[1];
          p[at + 1] = p[3];
          p[at + 2] = p[4];
          w32_copy(p + at + 3, p + 25, 32);
        }
      if (size >= 380)
        p[91] = p[33];
    }
  return 0;
}
static DDRESULT API get_mode(DD *c, DDDESC *out) {
  GUARD;
  if (!out ||
      out->size != (c->generation >= 4 ? sizeof(DDDESC2) : sizeof(DDDESC)))
    return DDERR_INVALIDPARAMS;
  DDDESC2 desc;
  describe_mode(&((struct dd_device *)c->owner)->mode, &desc, c->generation);
  w32_copy(out, &desc, desc.old.size);
  return 0;
}
static DDRESULT API get_fourcc(DD *c, DWORD *count, DWORD *codes) {
  (void)c;
  (void)codes;
  if (!count)
    return DDERR_INVALIDPARAMS;
  *count = 0;
  return 0;
}
static DDRESULT API get_gdi(DD *c, DD **out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  struct dd_surface *s = ((struct dd_device *)c->owner)->primary;
  if (!s)
    return DDERR_NOTFOUND;
  *out = &s->base.interfaces[c->generation == 7   ? 4
                             : c->generation == 4 ? 3
                                                  : 0];
  dd_add(*out);
  return 0;
}
static DDRESULT API flip_gdi(DD *c) {
  GUARD;
  struct dd_surface *s = ((struct dd_device *)c->owner)->primary;
  return s ? dd_present(s, 1, 1) : DDERR_NOTFOUND;
}
static DDRESULT API get_frequency(DD *c, DWORD *out) {
  (void)c;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 60;
  return 0;
}
static DDRESULT API get_scanline(DD *c, DWORD *out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = (DWORD)((dd_backend->clock_ns() % DD_SLOT_NS) *
                 ((struct dd_device *)c->owner)->mode.height / DD_SLOT_NS);
  return 0;
}
static DDRESULT API get_vblank(DD *c, BOOL *out) {
  (void)c;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  return 0;
}
static DDRESULT API initialize(DD *c, const DDGUID *guid) {
  (void)c;
  (void)guid;
  return DDERR(5);
}
static DDRESULT API restore_mode(DD *c) {
  GUARD;
  struct dd_device *d = c->owner;
  if (!(d->coop & DDSCL_EXCLUSIVE))
    return DDERR_NOEXCLUSIVEMODE;
  if (dd_backend->release)
    dd_backend->release(d->hwnd);
  dd_lose(d);
  d->mode_set = 0;
  d->mode =
      (struct dd_mode){d->display.width, d->display.height, d->display.bpp};
  return 0;
}
static DDRESULT API set_cooperative(DD *c, uintptr_t hwnd, DWORD flags) {
  GUARD;
  struct dd_device *d = c->owner;
  const DWORD allowed = DDSCL_FULLSCREEN | DDSCL_EXCLUSIVE | DDSCL_NORMAL |
                        DDSCL_ALLOWREBOOT | DDSCL_NOWINDOWCHANGES |
                        DDSCL_ALLOWMODEX | DDSCL_FPUSETUP | DDSCL_FPUPRESERVE;
  if (flags & ~allowed)
    return DDERR_INVALIDPARAMS;
  BOOL normal = !!(flags & DDSCL_NORMAL),
       full = (flags & (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN)) ==
              (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN);
  if (normal == full || (!normal && !full) ||
      ((flags & DDSCL_EXCLUSIVE) && !full) ||
      ((flags & DDSCL_FULLSCREEN) && !full))
    return DDERR_INVALIDPARAMS;
  if (full && !hwnd)
    return DDERR_INVALIDPARAMS;
  if (full && exclusive && exclusive != d)
    return DDERR_EXCLUSIVEMODEALREADYSET;
  if (normal && (d->coop & DDSCL_EXCLUSIVE)) {
    if (dd_backend->release)
      dd_backend->release(d->hwnd);
    dd_lose(d);
    d->mode_set = 0;
    if (exclusive == d)
      exclusive = 0;
    d->mode =
        (struct dd_mode){d->display.width, d->display.height, d->display.bpp};
  }
  if (full)
    exclusive = d;
  d->hwnd = hwnd;
  d->coop = flags;
  return 0;
}
static DDRESULT API set_mode(DD *c, DWORD w, DWORD h, DWORD bpp, DWORD refresh,
                             DWORD flags) {
  GUARD;
  struct dd_device *d = c->owner;
  if (!(d->coop & DDSCL_EXCLUSIVE))
    return DDERR_NOEXCLUSIVEMODE;
  if (flags || (refresh && refresh != 60) || !mode_supported(d, w, h, bpp))
    return DDERR_INVALIDMODE;
  struct dd_lease lease = {0};
  DDRESULT result =
      d->windowed_shim    ? (dd_backend == &dd_native_backend
                                 ? dd_native_window_mode(d->hwnd, w, h)
                                 : 0)
      : dd_backend->lease ? dd_backend->lease(d->hwnd, w, h, bpp, &lease)
                          : DDERR_UNSUPPORTED;
  if (result)
    return result;
  if (d->surfaces)
    dd_lose(d);
  if (d->windowed_shim)
    lease = (struct dd_lease){1, 0, 0, w, h, bpp, (w * (bpp / 8) + 3) & ~3u};
  d->mode = (struct dd_mode){w, h, bpp};
  d->lease = lease;
  d->mode_set = 1;
  d->generation++;
  d->next_frame = 0;
  return 0;
}
static DDRESULT API set_mode_old(DD *c, DWORD w, DWORD h, DWORD bpp) {
  return set_mode(c, w, h, bpp, 0, 0);
}
static DDRESULT API wait_vblank(DD *c, DWORD flags, void *event) {
  GUARD;
  (void)c;
  if (event || (flags != 1 && flags != 4))
    return DDERR_INVALIDPARAMS;
  uint64_t now = dd_backend->clock_ns();
  dd_backend->wait_ns((now / DD_SLOT_NS + 1) * DD_SLOT_NS);
  return 0;
}
static DDRESULT API available(DD *c, const DWORD *caps, DWORD *total,
                              DWORD *free_bytes) {
  GUARD;
  (void)c;
  if (!caps || (!total && !free_bytes))
    return DDERR_INVALIDPARAMS;
  if (total)
    *total = dd_budget;
  if (free_bytes)
    *free_bytes = dd_budget - dd_bytes;
  return 0;
}
static DDRESULT API from_dc(DD *c, HDC dc, DD **out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  for (struct dd_surface *s = ((struct dd_device *)c->owner)->surfaces; s;
       s = s->next)
    if (dc && s->dc == dc) {
      *out = &s->base.interfaces[c->generation == 7 ? 4 : 3];
      dd_add(*out);
      return 0;
    }
  return DDERR_NOTFOUND;
}
static DDRESULT API restore_all(DD *c) {
  GUARD;
  struct dd_device *d = c->owner;
  dd_sync(d);
  for (struct dd_surface *s = d->surfaces; s; s = s->next)
    if (!s->implicit) {
      DD *i = &s->base.interfaces[4];
      DDRESULT(API * restore)(DD *) = (void *)i->vt[27];
      DDRESULT r = restore(i);
      if (r)
        return r;
    }
  return 0;
}
static DDRESULT API test_cooperative(DD *c) {
  GUARD;
  struct dd_device *d = c->owner;
  dd_sync(d);
  if (d->mode_set && d->lease.active &&
      (d->lease.width != d->mode.width || d->lease.height != d->mode.height ||
       d->lease.bpp != d->mode.bpp))
    return DDERR_WRONGMODE;
  if ((d->coop & DDSCL_EXCLUSIVE) && d->mode_set && !d->lease.active)
    return DDERR_NOEXCLUSIVEMODE;
  return 0;
}
static DDRESULT API identifier(DD *c, void *out, DWORD flags) {
  GUARD;
  if (!out || (flags & ~1u))
    return DDERR_INVALIDPARAMS;
  DDIDENT2 id = {0};
  w32_copy(id.old.driver, "ciukiddraw", 11);
  w32_copy(id.old.description, "CiukiOS DirectDraw", 18);
  id.old.version = 0x0007000000000000ull;
  id.old.guid = dd_iids[3];
  w32_copy(out, &id, c->generation == 7 ? sizeof(id) : sizeof(id.old));
  return 0;
}
static DDRESULT API start_mode_test(DD *c, const SIZE *sizes, DWORD count,
                                    DWORD flags) {
  (void)c;
  (void)sizes;
  (void)count;
  (void)flags;
  return DDERR_UNSUPPORTED;
}
static DDRESULT API evaluate_mode(DD *c, DWORD flags, DWORD *timeout) {
  (void)c;
  (void)flags;
  if (timeout)
    *timeout = 0;
  return DDERR_UNSUPPORTED;
}
#define DEVICE_COMMON                                                          \
  dd_query, dd_add, dd_release, compact, create_clipper, create_palette,       \
      create_surface, duplicate, enum_modes, enum_surfaces, flip_gdi,          \
      get_caps, get_mode, get_fourcc, get_gdi, get_frequency, get_scanline,    \
      get_vblank, initialize, restore_mode, set_cooperative
/* Slots 21 and 23 were extended in generations 2 and 4 respectively. */
const void *const dd_device_vt[4][30] = {
    {DEVICE_COMMON, set_mode_old, wait_vblank},
    {DEVICE_COMMON, set_mode, wait_vblank, available},
    {DEVICE_COMMON, set_mode, wait_vblank, available, from_dc, restore_all,
     test_cooperative, identifier},
    {DEVICE_COMMON, set_mode, wait_vblank, available, from_dc, restore_all,
     test_cooperative, identifier, start_mode_test, evaluate_mode}};
DDRESULT API DirectDrawCreate(const DDGUID *guid, DD **out, void *outer) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  if (outer)
    return DDERR_INVALIDPARAMS;
  if ((uintptr_t)guid > 2)
    return DDERR(561);
  struct dd_device *d = dd_alloc(sizeof(*d));
  if (!d)
    return DDERR_OUTOFMEMORY;
  if (!dd_backend->display || !dd_backend->display(&d->display) ||
      !d->display.vram || d->display.count > 96) {
    dd_free(d);
    return DDERR(563);
  }
  DWORD generations[] = {1, 2, 4, 7};
  const void *const *tables[] = {dd_device_vt[0], dd_device_vt[1],
                                 dd_device_vt[2], dd_device_vt[3]};
  dd_init(&d->base, 1, sizeof(*d), generations, tables, 4);
  d->mode =
      (struct dd_mode){d->display.width, d->display.height, d->display.bpp};
  const struct ciuki_w32_compat *compat = w32_shim_current();
  if (compat)
    d->windowed_shim = !!(compat->mask & W32_SHIM_BIT(W32_SH_DDRAW_WINDOWED));
  dd_budget = d->display.vram;
  d->next = dd_devices;
  dd_devices = d;
  *out = &d->base.interfaces[0];
  if (dd_backend == &dd_native_backend)
    dd_native_start();
  return 0;
}
DDRESULT API DirectDrawCreateEx(const DDGUID *guid, void **out,
                                const DDGUID *iid, void *outer) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  if (!dd_equal(iid, &dd_iids[3]) || outer)
    return DDERR_INVALIDPARAMS;
  DD *d;
  DDRESULT r = DirectDrawCreate(guid, &d, 0);
  if (r)
    return r;
  r = dd_query(d, iid, out);
  dd_release(d);
  return r;
}
DDRESULT API DirectDrawEnumerateA(dd_enum_a callback, void *context) {
  if (!callback)
    return DDERR_INVALIDPARAMS;
  char desc[] = "CiukiOS DirectDraw", name[] = "display";
  callback(0, desc, name, context);
  return 0;
}
DDRESULT API DirectDrawEnumerateW(dd_enum_w callback, void *context) {
  if (!callback)
    return DDERR_INVALIDPARAMS;
  WCHAR desc[] = {'C', 'i', 'u', 'k', 'i', 'O', 'S', ' ', 'D', 'i',
                  'r', 'e', 'c', 't', 'D', 'r', 'a', 'w', 0},
        name[] = {'d', 'i', 's', 'p', 'l', 'a', 'y', 0};
  callback(0, desc, name, context);
  return 0;
}
DDRESULT API DirectDrawEnumerateExA(dd_enum_exa callback, void *context,
                                    DWORD flags) {
  if (!callback || (flags & ~7u))
    return DDERR_INVALIDPARAMS;
  char desc[] = "CiukiOS DirectDraw", name[] = "display";
  callback(0, desc, name, context, 0);
  return 0;
}
DDRESULT API DirectDrawEnumerateExW(dd_enum_exw callback, void *context,
                                    DWORD flags) {
  if (!callback || (flags & ~7u))
    return DDERR_INVALIDPARAMS;
  WCHAR desc[] = {'C', 'i', 'u', 'k', 'i', 'O', 'S', ' ', 'D', 'i',
                  'r', 'e', 'c', 't', 'D', 'r', 'a', 'w', 0},
        name[] = {'d', 'i', 's', 'p', 'l', 'a', 'y', 0};
  callback(0, desc, name, context, 0);
  return 0;
}
void dd_device_finalized(struct dd_device *d) {
  if (exclusive == d)
    exclusive = 0;
}
