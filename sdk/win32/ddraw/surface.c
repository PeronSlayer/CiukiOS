/* Personality-owned surfaces, visibility, loss and storage.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/shims.h>
void dd_set_format(struct w32_dib *d, DWORD bpp) {
  d->bpp = bpp;
  d->top_down = 1;
  if (bpp == 8) {
    d->color_count = 256;
    for (DWORD i = 0; i < 256; i++)
      d->colors[i] = i * 0x010101;
  } else {
    d->masks[0] = bpp == 16 ? 0xf800 : 0xff0000;
    d->masks[1] = bpp == 16 ? 0x7e0 : 0xff00;
    d->masks[2] = bpp == 16 ? 31 : 255;
  }
}
void dd_pixel_format(const struct w32_dib *d, DDPF *p) {
  *p = (DDPF){32,          DDPF_RGB | (d->bpp == 8 ? DDPF_PALETTEINDEXED8 : 0),
              0,           d->bpp,
              d->masks[0], d->masks[1],
              d->masks[2], 0};
}
BOOL dd_rect_valid(RECT r, const struct w32_dib *d) {
  return r.left >= 0 && r.top >= 0 && r.right > r.left && r.bottom > r.top &&
         (DWORD)r.right <= d->width && (DWORD)r.bottom <= d->height;
}
static void init_surface(struct dd_surface *s, struct dd_device *d,
                         DWORD caps) {
  DWORD gens[] = {1, 2, 3, 4, 7, 1, 1};
  const void *const *tables[] = {
      dd_surface_vt[0], dd_surface_vt[1], dd_surface_vt[2], dd_surface_vt[3],
      dd_surface_vt[4], dd_gamma_vt,      dd_color_vt};
  dd_init(&s->base, 2, sizeof(*s), gens, tables, 7);
  s->device = d;
  s->caps = caps;
  s->uniqueness = 1;
  s->color.size = sizeof(s->color);
  s->color.contrast = 10000;
  s->color.saturation = 10000;
  for (unsigned i = 0; i < 256; i++)
    s->gamma.red[i] = s->gamma.green[i] = s->gamma.blue[i] =
        (uint16_t)(i * 257);
  s->next = d->surfaces;
  d->surfaces = s;
  dd_add(&d->base.interfaces[0]);
}
static DDRESULT allocate_pixels(struct dd_surface *s) {
  DWORD size = s->dib.size;
  if (dd_bytes > dd_budget || size > dd_budget - dd_bytes)
    return DDERR_OUTOFVIDEOMEMORY;
  s->allocation = dd_alloc(size);
  if (!s->allocation)
    return DDERR_OUTOFMEMORY;
  s->dib.bits = s->allocation;
  s->allocation_bytes = size;
  s->charge = size;
  dd_bytes += size;
  return 0;
}
DDRESULT dd_create_surface(DD *c, const DDDESC *desc, DD **out, void *outer) {
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  if (outer)
    return DDERR_INVALIDPARAMS;
  if (!desc ||
      desc->size != (c->generation >= 4 ? sizeof(DDDESC2) : sizeof(DDDESC)) ||
      !(desc->flags & DDSD_CAPS))
    return DDERR_INVALIDPARAMS;
  struct dd_device *d = c->owner;
  dd_sync(d);
  DWORD caps = desc->caps;
  if (caps & DDSCAPS_OVERLAY)
    return DDERR_NOOVERLAYHW;
  if (caps & (DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_3DDEVICE))
    return DDERR_INVALIDCAPS;
  if (caps &
      ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY |
        DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM | DDSCAPS_COMPLEX |
        DDSCAPS_FLIP | DDSCAPS_BACKBUFFER | DDSCAPS_FRONTBUFFER))
    return DDERR_INVALIDCAPS;
  if ((caps & DDSCAPS_SYSTEMMEMORY) && (caps & DDSCAPS_VIDEOMEMORY))
    return DDERR_INVALIDCAPS;
  BOOL primary = !!(caps & DDSCAPS_PRIMARYSURFACE);
  if (!primary && !(caps & DDSCAPS_OFFSCREENPLAIN))
    return DDERR_INVALIDCAPS;
  if (primary && !d->coop)
    return DDERR_NOCOOPERATIVELEVELSET;
  if (primary && d->primary)
    return DDERR_PRIMARYSURFACEALREADYEXISTS;
  if (primary && (desc->flags & (DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT |
                                 DDSD_LPSURFACE)))
    return DDERR_INVALIDPARAMS;
  if (primary && (d->coop & DDSCL_EXCLUSIVE) && !d->lease.active &&
      !d->windowed_shim)
    return DDERR_NOEXCLUSIVEMODE;
  DWORD back = (desc->flags & DDSD_BACKBUFFERCOUNT) ? desc->backbuffers : 0;
  if (back > 3 ||
      ((caps & DDSCAPS_FLIP) &&
       (!primary || !(caps & DDSCAPS_COMPLEX) || !back)) ||
      (!(caps & DDSCAPS_FLIP) && back))
    return DDERR_INVALIDCAPS;
  DWORD w = primary ? d->mode.width : desc->width,
        h = primary ? d->mode.height : desc->height;
  DWORD depth = desc->flags & DDSD_PIXELFORMAT ? desc->pixel.bits : d->mode.bpp;
  if (!w || !h || w > 8192 || h > 8192 ||
      (!primary && (desc->flags & (DDSD_WIDTH | DDSD_HEIGHT)) !=
                       (DDSD_WIDTH | DDSD_HEIGHT)))
    return DDERR_INVALIDPARAMS;
  if (depth != 8 && depth != 16 && depth != 24 && depth != 32)
    return DDERR_INVALIDPIXELFORMAT;
  if (!primary && (desc->flags & DDSD_PIXELFORMAT) &&
      (desc->pixel.size != 32 || desc->pixel.fourcc || desc->pixel.alpha ||
       (desc->pixel.flags & ~(DDPF_RGB | DDPF_PALETTEINDEXED8))))
    return DDERR_INVALIDPIXELFORMAT;
  if (!(caps & DDSCAPS_SYSTEMMEMORY))
    caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
  struct dd_surface *s = dd_alloc(sizeof(*s));
  if (!s)
    return DDERR_OUTOFMEMORY;
  init_surface(s, d, caps);
  dd_set_format(&s->dib, depth);
  const struct ciuki_w32_compat *compat = w32_shim_current();
  if (depth == 16 && compat &&
      (compat->mask & W32_SHIM_BIT(W32_SH_DDRAW_555))) {
    s->dib.masks[0] = 0x7c00;
    s->dib.masks[1] = 0x3e0;
    s->dib.masks[2] = 31;
  }
  s->dib.width = w;
  s->dib.height = h;
  s->dib.pitch = (w * (depth / 8) + 3) & ~3u;
  s->dib.size = s->dib.pitch * h;
  if (!primary && (desc->flags & DDSD_PIXELFORMAT) && depth > 8) {
    s->dib.masks[0] = desc->pixel.red;
    s->dib.masks[1] = desc->pixel.green;
    s->dib.masks[2] = desc->pixel.blue;
  }
  DDRESULT r = 0;
  if (desc->flags & DDSD_LPSURFACE) {
    if (!(caps & DDSCAPS_SYSTEMMEMORY) || !(desc->flags & DDSD_PITCH) ||
        desc->pitch <= 0 || (uint64_t)desc->pitch * h > 0x7fffffff)
      r = DDERR_INVALIDPARAMS;
    else {
      s->dib.bits = desc->pixels;
      s->dib.pitch = desc->pitch;
      s->dib.size = s->dib.pitch * h;
    }
  } else
    r = allocate_pixels(s);
  if (!r && !w32_dib_validate(&s->dib))
    r = DDERR_INVALIDPIXELFORMAT;
  if (!r && back)
    for (DWORD i = 0; i < back; i++) {
      struct dd_surface *b = dd_alloc(sizeof(*b));
      if (!b) {
        r = DDERR_OUTOFMEMORY;
        break;
      }
      init_surface(b, d,
                   (caps & ~(DDSCAPS_PRIMARYSURFACE | DDSCAPS_FRONTBUFFER)) |
                       DDSCAPS_BACKBUFFER);
      b->implicit = 1;
      b->dib = s->dib;
      b->dib.bits = 0;
      r = allocate_pixels(b);
      if (r) {
        dd_release(&b->base.interfaces[0]);
        break;
      }
      s->attached[s->count++] = b;
    }
  if (r) {
    dd_release(&s->base.interfaces[0]);
    return r;
  }
  if (primary && d->windowed_shim) {
    DD *clipper = 0;
    r = DirectDrawCreateClipper(0, &clipper, 0);
    if (!r) {
      DDRESULT(API * set_hwnd)(DD *, DWORD, uintptr_t) = (void *)clipper->vt[8];
      r = set_hwnd(clipper, 0, d->hwnd);
    }
    if (r) {
      if (clipper)
        dd_release(clipper);
      dd_release(&s->base.interfaces[0]);
      return r;
    }
    s->clipper = clipper->owner;
  }
  if (primary) {
    s->caps |= DDSCAPS_FRONTBUFFER;
    d->primary = s;
  }
  *out = &s->base.interfaces[c->generation == 7   ? 4
                             : c->generation == 4 ? 3
                                                  : 0];
  return 0;
}
void dd_surface_destroy(struct dd_surface *s) {
  struct dd_device *d = s->device;
  if (s->dc)
    DeleteDC(s->dc);
  for (DWORD i = 0; i < s->count; i++)
    if (s->attached[i])
      dd_release(&s->attached[i]->base.interfaces[0]);
  if (s->palette)
    dd_release(&s->palette->base.interfaces[0]);
  if (s->clipper)
    dd_release(&s->clipper->base.interfaces[0]);
  while (s->private_data) {
    struct dd_private *p = s->private_data;
    s->private_data = p->next;
    if (p->flags & DDSPD_IUNKNOWNPOINTER) {
      DD *obj = p->data;
      DWORD(API * release)(DD *) = (void *)obj->vt[2];
      release(obj);
    } else
      dd_free(p->data);
    dd_free(p);
  }
  struct dd_surface **at = &d->surfaces;
  while (*at && *at != s)
    at = &(*at)->next;
  if (*at)
    *at = s->next;
  if (d->primary == s)
    d->primary = 0;
  dd_bytes -= s->charge;
  dd_free(s->allocation);
  dd_release(&d->base.interfaces[0]);
}
DDRESULT dd_description(DD *c, DDDESC *out, BOOL lock) {
  struct dd_surface *s = c->owner;
  if (!out ||
      out->size != (c->generation >= 4 ? sizeof(DDDESC2) : sizeof(DDDESC)))
    return DDERR_INVALIDPARAMS;
  DDDESC2 result = {0};
  result.old.size = out->size;
  result.old.flags =
      DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
  result.old.width = s->dib.width;
  result.old.height = s->dib.height;
  result.old.pitch = s->dib.pitch;
  result.old.caps = s->caps;
  result.old.backbuffers = s->count;
  if (s->count)
    result.old.flags |= DDSD_BACKBUFFERCOUNT;
  if (lock) {
    result.old.flags |= DDSD_LPSURFACE;
    result.old.pixels = s->lock_pointer;
  }
  dd_pixel_format(&s->dib, &result.old.pixel);
  w32_copy(out, &result, result.old.size);
  return 0;
}
DDRESULT dd_surface_ready(struct dd_surface *s, DWORD flags, DWORD wait_flag) {
  dd_sync(s->device);
  if (s->lost)
    return DDERR_SURFACELOST;
  if (s->locked || s->dc)
    return DDERR_SURFACEBUSY;
  if (s->busy) {
    if (!(flags & wait_flag))
      return DDERR_WASSTILLDRAWING;
    return DDERR_SURFACEBUSY;
  }
  return 0;
}
DDRESULT dd_present(struct dd_surface *s, BOOL wait, DWORD interval) {
  struct dd_device *d = s->device;
  if (d->primary != s)
    return 0;
  dd_sync(d);
  if (s->lost)
    return DDERR_SURFACELOST;
  if (s->busy) {
    d->dirty = 1;
    return DDERR_WASSTILLDRAWING;
  }
  if (s->locked || s->dc) {
    d->dirty = 1;
    return 0;
  }
  uint64_t now = dd_backend->clock_ns();
  if (now < d->next_frame) {
    d->dirty = 1;
    if (!wait)
      return 0;
    dd_backend->wait_ns(d->next_frame);
    dd_sync(d);
    if (s->lost)
      return DDERR_SURFACELOST;
    now = dd_backend->clock_ns();
  }
  DDRESULT r;
  BOOL retained = 0;
  DWORD snapshot_unique = s->uniqueness;
  if ((d->coop & DDSCL_EXCLUSIVE) && !d->windowed_shim) {
    if (!d->lease.active)
      return DDERR_NOEXCLUSIVEMODE;
    if (d->palette_pending && s->palette && dd_backend->palette) {
      r = dd_backend->palette(&d->lease, 0, 256, s->dib.colors);
      if (r) {
        d->dirty = 1;
        return r;
      }
      d->palette_pending = 0;
    }
    if (d->gamma_pending && dd_backend->gamma) {
      r = dd_backend->gamma(&d->lease, &s->gamma);
      if (r) {
        d->dirty = 1;
        return r;
      }
      d->gamma_pending = 0;
    }
    r = dd_backend->frame ? dd_backend->frame(&d->lease, &s->dib)
                          : DDERR_UNSUPPORTED;
  } else {
    RECT rects[DD_MAX_RECTS];
    DWORD n = DD_MAX_RECTS;
    if (!s->clipper || !s->clipper->hwnd)
      return DDERR_NOCLIPLIST;
    /* USER may enter DirectDraw from a window procedure. Never take USER's
     * window lock while retaining ours; pin the buffer and its parent instead.
     * Busy blocks mutation/rebinding while the borrowed DIB is published. */
    uintptr_t hwnd = s->clipper->hwnd;
    struct w32_dib snapshot = s->dib;
    dd_add(&s->base.interfaces[0]);
    retained = 1;
    s->busy = 1;
    DWORD depth = dd_suspend();
    r = dd_backend->clip ? dd_backend->clip(hwnd, rects, &n) : DDERR_NOCLIPLIST;
    if (!r)
      r = dd_backend->window ? dd_backend->window(hwnd, &snapshot, rects, n)
                             : DDERR_UNSUPPORTED;
    dd_resume(depth);
    s->busy = 0;
    if (s->lost)
      r = DDERR_SURFACELOST;
  }
  if (!r) {
    d->dirty = retained && s->uniqueness != snapshot_unique;
    d->next_frame = (now / DD_SLOT_NS + (interval ? interval : 1)) * DD_SLOT_NS;
  } else if (r == DDERR_SURFACELOST)
    dd_lose(d);
  else {
    d->dirty = 1;
    d->next_frame = (now / DD_SLOT_NS + 1) * DD_SLOT_NS;
  }
  if (retained)
    dd_release(&s->base.interfaces[0]);
  return r;
}
void dd_palette_update(struct dd_palette *p, DWORD first, DWORD count) {
  for (struct dd_device *d = dd_devices; d; d = d->next)
    for (struct dd_surface *s = d->surfaces; s; s = s->next)
      if (s->palette == p) {
        s->uniqueness++;
        for (DWORD i = first; i < first + count; i++)
          s->dib.colors[i] = ((DWORD)p->colors[i].peRed << 16) |
                             ((DWORD)p->colors[i].peGreen << 8) |
                             p->colors[i].peBlue;
        if (s->dc)
          w32_gdi_bind_surface(s->dc, &s->dib);
        if (d->primary == s) {
          dd_sync(d);
          if (d->lease.active && count && dd_backend->palette)
            d->palette_pending =
                dd_backend->palette(&d->lease, first, count,
                                    s->dib.colors + first) != 0;
          dd_present(s, 0, 1);
        }
      }
}
static DDRESULT API get_desc(DD *c, DDDESC *out) {
  GUARD;
  return dd_description(c, out, 0);
}
static DDRESULT API get_pixel(DD *c, DDPF *out) {
  GUARD;
  if (!out || out->size != 32)
    return DDERR_INVALIDPARAMS;
  dd_pixel_format(&((struct dd_surface *)c->owner)->dib, out);
  return 0;
}
static DDRESULT API get_caps(DD *c, DWORD *out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  out[0] = ((struct dd_surface *)c->owner)->caps;
  if (c->generation >= 4)
    out[1] = out[2] = out[3] = 0;
  return 0;
}
static DDRESULT API is_lost(DD *c) {
  GUARD;
  struct dd_surface *s = c->owner;
  dd_sync(s->device);
  return s->lost ? DDERR_SURFACELOST : 0;
}
static DDRESULT API restore(DD *c) {
  GUARD;
  struct dd_surface *s = c->owner;
  struct dd_device *d = s->device;
  dd_sync(d);
  if (!s->lost)
    return 0;
  if (s->implicit)
    return DDERR_IMPLICITLYCREATED;
  if (s->locked || s->dc || s->busy)
    return DDERR_SURFACEBUSY;
  if (d->mode_set && d->lease.active &&
      (d->lease.width != d->mode.width || d->lease.height != d->mode.height ||
       d->lease.bpp != d->mode.bpp))
    return DDERR_WRONGMODE;
  if ((d->coop & DDSCL_EXCLUSIVE) && !d->lease.active && !d->windowed_shim)
    return DDERR_NOEXCLUSIVEMODE;
  if ((s->caps & DDSCAPS_PRIMARYSURFACE) &&
      (s->dib.width != d->mode.width || s->dib.height != d->mode.height ||
       s->dib.bpp != d->mode.bpp))
    return DDERR_WRONGMODE;
  for (DWORD i = 0; i < s->count; i++)
    if (s->attached[i]->locked || s->attached[i]->dc)
      return DDERR_SURFACEBUSY;
  s->lost = 0;
  s->uniqueness++;
  if (s->allocation)
    w32_fill(s->allocation, 0, s->allocation_bytes);
  for (DWORD i = 0; i < s->count; i++)
    if (s->attached[i]->implicit) {
      struct dd_surface *b = s->attached[i];
      b->lost = 0;
      b->uniqueness++;
      if (b->allocation)
        w32_fill(b->allocation, 0, b->allocation_bytes);
    }
  return 0;
}
static DDRESULT API lock_surface(DD *c, const RECT *rect, DDDESC *out,
                                 DWORD flags, void *event) {
  GUARD;
  struct dd_surface *s = c->owner;
  dd_sync(s->device);
  if (!out || event ||
      out->size != (c->generation >= 4 ? sizeof(DDDESC2) : sizeof(DDDESC)) ||
      (flags & ~0x70b9u))
    return DDERR_INVALIDPARAMS;
  if (s->lost)
    return DDERR_SURFACELOST;
  if (s->locked || s->dc)
    return DDERR_SURFACEBUSY;
  DWORD waiting = (flags & DDLOCK_WAIT) ||
                  (c->generation == 7 && !(flags & DDLOCK_DONOTWAIT));
  DDRESULT r = dd_surface_ready(s, waiting, 1);
  if (r)
    return r;
  RECT area = rect ? *rect : (RECT){0, 0, s->dib.width, s->dib.height};
  if (!dd_rect_valid(area, &s->dib))
    return DDERR_INVALIDRECT;
  s->locked = 1;
  s->lock_flags = flags;
  s->lock_rect = area;
  s->lock_pointer =
      s->dib.bits + area.top * s->dib.pitch + area.left * (s->dib.bpp / 8);
  dd_description(c, out, 1);
  out->width = area.right - area.left;
  out->height = area.bottom - area.top;
  return 0;
}
static DDRESULT API unlock_surface(DD *c, const void *rect_or_pointer) {
  GUARD;
  struct dd_surface *s = c->owner;
  if (!s->locked)
    return DDERR_NOTLOCKED;
  if (rect_or_pointer) {
    if (c->generation < 4) {
      if (rect_or_pointer != s->lock_pointer)
        return DDERR_INVALIDPARAMS;
    } else {
      const RECT *r = rect_or_pointer;
      if (r->left != s->lock_rect.left || r->top != s->lock_rect.top ||
          r->right != s->lock_rect.right || r->bottom != s->lock_rect.bottom)
        return DDERR_INVALIDRECT;
    }
  }
  DWORD flags = s->lock_flags;
  s->locked = 0;
  s->lock_pointer = 0;
  dd_sync(s->device);
  if (s->lost)
    return DDERR_SURFACELOST;
  if (!(flags & DDLOCK_READONLY)) {
    s->uniqueness++;
    return dd_present(s, 0, 1);
  }
  return 0;
}
static DDRESULT API get_dc(DD *c, HDC *out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  struct dd_surface *s = c->owner;
  if (s->dc)
    return DDERR_DCALREADYCREATED;
  DDRESULT r = dd_surface_ready(s, 1, 1);
  if (r)
    return r;
  s->dc = w32_gdi_surface_dc(&s->dib, (POINT){0, 0}, 0, 0);
  if (!s->dc)
    return DDERR_OUTOFMEMORY;
  *out = s->dc;
  return 0;
}
static DDRESULT API release_dc(DD *c, HDC dc) {
  GUARD;
  struct dd_surface *s = c->owner;
  if (!dc || dc != s->dc)
    return DDERR_INVALIDPARAMS;
  if (!DeleteDC(dc))
    return DDERR_INVALIDPARAMS;
  s->dc = 0;
  s->uniqueness++;
  dd_sync(s->device);
  return s->lost ? DDERR_SURFACELOST : dd_present(s, 0, 1);
}
static DDRESULT API set_palette(DD *c, DD *palette) {
  GUARD;
  struct dd_surface *s = c->owner;
  if (s->dib.bpp != 8)
    return DDERR(320);
  if (palette && palette->kind != 3)
    return DDERR_INVALIDPARAMS;
  struct dd_palette *p = palette ? palette->owner : 0;
  if (p)
    dd_add(palette);
  if (s->palette)
    dd_release(&s->palette->base.interfaces[0]);
  s->palette = p;
  if (p)
    dd_palette_update(p, 0, 256);
  return 0;
}
static DDRESULT API get_palette(DD *c, DD **out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  struct dd_palette *p = ((struct dd_surface *)c->owner)->palette;
  if (!p)
    return DDERR_NOPALETTEATTACHED;
  *out = &p->base.interfaces[0];
  dd_add(*out);
  return 0;
}
static DDRESULT API set_clipper(DD *c, DD *clipper) {
  GUARD;
  if (clipper && clipper->kind != 4)
    return DDERR_INVALIDPARAMS;
  struct dd_surface *s = c->owner;
  if (clipper)
    dd_add(clipper);
  if (s->clipper)
    dd_release(&s->clipper->base.interfaces[0]);
  s->clipper = clipper ? clipper->owner : 0;
  return 0;
}
static DDRESULT API get_clipper(DD *c, DD **out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  struct dd_clipper *p = ((struct dd_surface *)c->owner)->clipper;
  if (!p)
    return DDERR_NOCLIPPERATTACHED;
  *out = &p->base.interfaces[0];
  dd_add(*out);
  return 0;
}
static DDRESULT API set_key(DD *c, DWORD flags, const DDKEY *key) {
  GUARD;
  struct dd_surface *s = c->owner;
  DWORD kind = flags & ~DDCKEY_COLORSPACE;
  if (kind != DDCKEY_SRCBLT && kind != DDCKEY_DESTBLT)
    return DDERR_INVALIDPARAMS;
  if (key && key->low > key->high && (flags & DDCKEY_COLORSPACE))
    return DDERR_INVALIDPARAMS;
  if (!key) {
    s->keys &= ~kind;
    return 0;
  }
  DDKEY value = *key;
  if (!(flags & DDCKEY_COLORSPACE))
    value.high = value.low;
  if (kind == DDCKEY_SRCBLT)
    s->source_key = value;
  else
    s->dest_key = value;
  s->keys |= kind;
  return 0;
}
static DDRESULT API get_key(DD *c, DWORD flags, DDKEY *out) {
  GUARD;
  struct dd_surface *s = c->owner;
  if (!out || (flags != DDCKEY_SRCBLT && flags != DDCKEY_DESTBLT))
    return DDERR_INVALIDPARAMS;
  if (!(s->keys & flags))
    return DDERR_NOCOLORKEY;
  *out = flags == DDCKEY_SRCBLT ? s->source_key : s->dest_key;
  return 0;
}
static DDRESULT API add_attached(DD *c, DD *other) {
  GUARD;
  if (!other || other->kind != 2)
    return DDERR_INVALIDPARAMS;
  struct dd_surface *s = c->owner, *b = other->owner;
  if (s == b || s->count >= 3 || b->device != s->device ||
      b->dib.width != s->dib.width || b->dib.height != s->dib.height ||
      b->dib.bpp != s->dib.bpp || b->dib.pitch != s->dib.pitch ||
      b->dib.masks[0] != s->dib.masks[0] ||
      b->dib.masks[1] != s->dib.masks[1] ||
      b->dib.masks[2] != s->dib.masks[2] || b->count || b->implicit)
    return DDERR_INVALIDCAPS;
  for (DWORD i = 0; i < s->count; i++)
    if (s->attached[i] == b)
      return DDERR(10);
  s->attached[s->count++] = b;
  dd_add(other);
  return 0;
}
static DDRESULT API delete_attached(DD *c, DWORD flags, DD *other) {
  GUARD;
  if (flags || !other)
    return DDERR_INVALIDPARAMS;
  struct dd_surface *s = c->owner;
  for (DWORD i = 0; i < s->count; i++)
    if (s->attached[i] == other->owner) {
      if (s->attached[i]->implicit)
        return DDERR_IMPLICITLYCREATED;
      struct dd_surface *b = s->attached[i];
      for (DWORD k = i + 1; k < s->count; k++)
        s->attached[k - 1] = s->attached[k];
      s->count--;
      dd_release(&b->base.interfaces[0]);
      return 0;
    }
  return DDERR_NOTFOUND;
}
static DDRESULT API get_attached(DD *c, const DWORD *caps, DD **out) {
  GUARD;
  if (!out || !caps)
    return DDERR_INVALIDPARAMS;
  *out = 0;
  struct dd_surface *s = c->owner;
  for (DWORD i = 0; i < s->count; i++)
    if ((s->attached[i]->caps & *caps) == *caps) {
      *out = &s->attached[i]->base.interfaces[c->generation == 7   ? 4
                                              : c->generation == 4 ? 3
                                              : c->generation == 3 ? 2
                                              : c->generation == 2 ? 1
                                                                   : 0];
      dd_add(*out);
      return 0;
    }
  return DDERR_NOTFOUND;
}
static DDRESULT API enum_attached(DD *c, void *context,
                                  DDRESULT(API *callback)(DD *, DDDESC *,
                                                          void *)) {
  GUARD;
  if (!callback)
    return DDERR_INVALIDPARAMS;
  struct dd_surface *s = c->owner;
  DD *snapshot[3];
  DWORD count = s->count, index = c->generation == 7   ? 4
                                  : c->generation == 4 ? 3
                                  : c->generation == 3 ? 2
                                  : c->generation == 2 ? 1
                                                       : 0;
  for (DWORD i = 0; i < count; i++) {
    snapshot[i] = &s->attached[i]->base.interfaces[index];
    dd_add(snapshot[i]);
  }
  BOOL more = 1;
  for (DWORD i = 0; i < count; i++) {
    if (more) {
      DDDESC2 desc = {0};
      desc.old.size = c->generation >= 4 ? sizeof(desc) : sizeof(desc.old);
      dd_description(snapshot[i], &desc.old, 0);
      dd_add(snapshot[i]);
      more = callback(snapshot[i], &desc.old, context) != 0;
    }
    dd_release(snapshot[i]);
  }
  return 0;
}
static DDRESULT API flip(DD *c, DD *target, DWORD flags) {
  GUARD;
  struct dd_surface *s = c->owner;
  if (flags & ~(DDFLIP_WAIT | DDFLIP_NOVSYNC | DDFLIP_DONOTWAIT | 0x07000000u))
    return DDERR_INVALIDPARAMS;
  DWORD interval = (flags >> 24) & 7;
  if (interval > 4)
    return DDERR_INVALIDPARAMS;
  if (!interval)
    interval = 1;
  DDRESULT r = dd_surface_ready(s, flags, DDFLIP_WAIT);
  if (r)
    return r;
  if (!(s->caps & DDSCAPS_FLIP) || !s->count)
    return DDERR_NOTFLIPPABLE;
  DWORD offset = 1;
  if (target) {
    for (offset = 1; offset <= s->count; offset++)
      if (s->attached[offset - 1] == target->owner)
        break;
    if (offset > s->count)
      return DDERR_INVALIDPARAMS;
  }
  for (DWORD i = 0; i < s->count; i++) {
    struct dd_surface *back = s->attached[i];
    if (back->dib.pitch != s->dib.pitch || back->dib.size != s->dib.size ||
        back->dib.masks[0] != s->dib.masks[0] ||
        back->dib.masks[1] != s->dib.masks[1] ||
        back->dib.masks[2] != s->dib.masks[2])
      return DDERR_INVALIDCAPS;
    r = dd_surface_ready(back, flags, DDFLIP_WAIT);
    if (r)
      return r;
  }
  struct dd_device *d = s->device;
  BOOL wait = (flags & DDFLIP_WAIT) ||
              (c->generation == 7 && !(flags & DDFLIP_DONOTWAIT));
  if (!(flags & DDFLIP_NOVSYNC) && dd_backend->clock_ns() < d->next_frame &&
      !wait)
    return DDERR_WASSTILLDRAWING;
  if (!(d->coop & DDSCL_EXCLUSIVE) || d->windowed_shim) {
    struct dd_surface *b = s->attached[offset - 1];
    for (DWORD y = 0; y < s->dib.height; y++)
      w32_copy(s->dib.bits + y * s->dib.pitch, b->dib.bits + y * b->dib.pitch,
               s->dib.width * (s->dib.bpp / 8));
    s->uniqueness++;
    return dd_present(s, wait, interval);
  }
  /* Rotate backing ownership, preserving identity and attached palettes/DCs. */
  for (DWORD k = 0; k < offset; k++) {
    void *allocation = s->allocation;
    DWORD bytes = s->allocation_bytes;
    uint8_t *bits = s->dib.bits;
    struct dd_surface *previous = s;
    for (DWORD i = 0; i < s->count; i++) {
      struct dd_surface *b = s->attached[i];
      previous->allocation = b->allocation;
      previous->allocation_bytes = b->allocation_bytes;
      previous->dib.bits = b->dib.bits;
      previous->uniqueness++;
      previous = b;
    }
    previous->allocation = allocation;
    previous->allocation_bytes = bytes;
    previous->dib.bits = bits;
    previous->uniqueness++;
  }
  return dd_present(s, wait, interval);
}
static DDRESULT API status(DD *c, DWORD flags) {
  GUARD;
  if (flags != 1 && flags != 2)
    return DDERR_INVALIDPARAMS;
  struct dd_surface *s = c->owner;
  dd_sync(s->device);
  return s->lost ? DDERR_SURFACELOST : s->busy ? DDERR_WASSTILLDRAWING : 0;
}
static DDRESULT API get_dd(DD *c, void **out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  struct dd_surface *s = c->owner;
  *out = &s->device->base.interfaces[c->generation == 7   ? 3
                                     : c->generation >= 4 ? 2
                                                          : 0];
  dd_add(*out);
  return 0;
}
static DDRESULT API page(DD *c, DWORD flags) {
  (void)c;
  return flags ? DDERR_INVALIDPARAMS : 0;
}
static DDRESULT API set_desc(DD *c, const DDDESC *in, DWORD flags) {
  GUARD;
  struct dd_surface *s = c->owner;
  if (!in || flags ||
      in->size != (c->generation >= 4 ? sizeof(DDDESC2) : sizeof(DDDESC)) ||
      (in->flags & ~(DDSD_LPSURFACE | DDSD_PITCH)) ||
      !(in->flags & DDSD_LPSURFACE) || !in->pixels ||
      !(s->caps & DDSCAPS_SYSTEMMEMORY) || s->implicit)
    return DDERR_INVALIDPARAMS;
  DDRESULT r = dd_surface_ready(s, 1, 1);
  if (r)
    return r;
  struct w32_dib d = s->dib;
  d.bits = in->pixels;
  if (in->flags & DDSD_PITCH) {
    if (in->pitch <= 0)
      return DDERR_INVALIDPARAMS;
    d.pitch = in->pitch;
  }
  if ((uint64_t)d.pitch * d.height > 0x7fffffff)
    return DDERR_INVALIDPARAMS;
  d.size = d.pitch * d.height;
  if (!w32_dib_validate(&d))
    return DDERR_INVALIDPARAMS;
  dd_free(s->allocation);
  dd_bytes -= s->charge;
  s->allocation = 0;
  s->allocation_bytes = 0;
  s->charge = 0;
  s->dib = d;
  s->uniqueness++;
  return 0;
}
static struct dd_private **private_slot(struct dd_surface *s,
                                        const DDGUID *key) {
  struct dd_private **at = &s->private_data;
  while (*at && !dd_equal(&(*at)->key, key))
    at = &(*at)->next;
  return at;
}
static DDRESULT API free_private(DD *c, const DDGUID *key) {
  GUARD;
  if (!key)
    return DDERR_INVALIDPARAMS;
  struct dd_private **at = private_slot(c->owner, key), *p = *at;
  if (!p)
    return DDERR_NOTFOUND;
  *at = p->next;
  if (p->flags & DDSPD_IUNKNOWNPOINTER) {
    DD *o = p->data;
    DWORD(API * release)(DD *) = (void *)o->vt[2];
    release(o);
  } else
    dd_free(p->data);
  dd_free(p);
  return 0;
}
static DDRESULT API set_private(DD *c, const DDGUID *key, void *in, DWORD size,
                                DWORD flags) {
  GUARD;
  if (!key || !in || (flags & ~3u) || size > 16 * 1024 * 1024 ||
      ((flags & DDSPD_IUNKNOWNPOINTER) && size != sizeof(void *)))
    return DDERR_INVALIDPARAMS;
  struct dd_surface *s = c->owner;
  struct dd_private *p = dd_alloc(sizeof(*p));
  if (!p)
    return DDERR_OUTOFMEMORY;
  p->key = *key;
  p->size = size;
  p->flags = flags;
  p->uniqueness = s->uniqueness;
  if (flags & DDSPD_IUNKNOWNPOINTER) {
    p->data = in;
    DD *o = in;
    DWORD(API * add)(DD *) = (void *)o->vt[1];
    add(o);
  } else {
    p->data = dd_alloc(size);
    if (!p->data) {
      dd_free(p);
      return DDERR_OUTOFMEMORY;
    }
    w32_copy(p->data, in, size);
  }
  free_private(c, key);
  p->next = s->private_data;
  s->private_data = p;
  return 0;
}
static DDRESULT API get_private(DD *c, const DDGUID *key, void *out,
                                DWORD *size) {
  GUARD;
  if (!key || !size)
    return DDERR_INVALIDPARAMS;
  struct dd_surface *s = c->owner;
  struct dd_private *p = *private_slot(s, key);
  if (!p)
    return DDERR_NOTFOUND;
  if ((p->flags & DDSPD_VOLATILE) && p->uniqueness != s->uniqueness)
    return DDERR_EXPIRED;
  DWORD capacity = *size;
  *size = p->size;
  if (!out || capacity < p->size)
    return DDERR_MOREDATA;
  if (p->flags & DDSPD_IUNKNOWNPOINTER) {
    *(void **)out = p->data;
    DD *o = p->data;
    DWORD(API * add)(DD *) = (void *)o->vt[1];
    add(o);
  } else
    w32_copy(out, p->data, p->size);
  return 0;
}
static DDRESULT API get_unique(DD *c, DWORD *out) {
  GUARD;
  if (!out)
    return DDERR_INVALIDPARAMS;
  *out = ((struct dd_surface *)c->owner)->uniqueness;
  return 0;
}
static DDRESULT API change_unique(DD *c) {
  GUARD;
  struct dd_surface *s = c->owner;
  if (!++s->uniqueness)
    s->uniqueness++;
  return 0;
}
static DDRESULT API gamma_get(DD *c, DWORD flags, DDGAMMA *out) {
  GUARD;
  if (flags || !out)
    return DDERR_INVALIDPARAMS;
  *out = ((struct dd_surface *)c->owner)->gamma;
  return 0;
}
static DDRESULT API gamma_set(DD *c, DWORD flags, const DDGAMMA *in) {
  GUARD;
  if ((flags & ~1u) || !in)
    return DDERR_INVALIDPARAMS;
  struct dd_surface *s = c->owner;
  s->gamma = *in;
  dd_sync(s->device);
  if (s->device->lease.active && dd_backend->gamma) {
    DDRESULT result = dd_backend->gamma(&s->device->lease, in);
    s->device->gamma_pending = result != 0;
    return result;
  }
  return 0;
}
static DDRESULT API color_get(DD *c, DDCOLOR *out) {
  GUARD;
  if (!out || out->size != sizeof(*out))
    return DDERR_INVALIDPARAMS;
  *out = ((struct dd_surface *)c->owner)->color;
  return 0;
}
static DDRESULT API color_set(DD *c, const DDCOLOR *in) {
  GUARD;
  if (!in || in->size != sizeof(*in))
    return DDERR_INVALIDPARAMS;
  ((struct dd_surface *)c->owner)->color = *in;
  return 0;
}
const void *const dd_gamma_vt[] = {dd_query, dd_add, dd_release, gamma_get,
                                   gamma_set};
const void *const dd_color_vt[] = {dd_query, dd_add, dd_release, color_get,
                                   color_set};
static DDRESULT API batch(DD *c, void *batch, DWORD count, DWORD flags) {
  (void)c;
  (void)batch;
  (void)count;
  (void)flags;
  return DDERR_UNSUPPORTED;
}
static DDRESULT API overlay_dirty(DD *c, RECT *r) {
  (void)c;
  (void)r;
  return DDERR_NOOVERLAYHW;
}
static DDRESULT API overlay_enum(DD *c, DWORD flags, void *context,
                                 void *callback) {
  (void)c;
  (void)flags;
  (void)context;
  (void)callback;
  return DDERR_NOOVERLAYHW;
}
static DDRESULT API overlay_position(DD *c, LONG *x, LONG *y) {
  (void)c;
  (void)x;
  (void)y;
  return DDERR_NOOVERLAYHW;
}
static DDRESULT API surface_initialize(DD *c, DD *d, DDDESC *desc) {
  (void)c;
  (void)d;
  (void)desc;
  return DDERR(5);
}
static DDRESULT API overlay_set_position(DD *c, LONG x, LONG y) {
  (void)c;
  (void)x;
  (void)y;
  return DDERR_NOOVERLAYHW;
}
static DDRESULT API overlay_update(DD *c, RECT *source, DD *dest,
                                   RECT *destination, DWORD flags, void *fx) {
  (void)c;
  (void)source;
  (void)dest;
  (void)destination;
  (void)flags;
  (void)fx;
  return DDERR_NOOVERLAYHW;
}
static DDRESULT API overlay_display(DD *c, DWORD flags) {
  (void)c;
  (void)flags;
  return DDERR_NOOVERLAYHW;
}
static DDRESULT API overlay_order(DD *c, DWORD flags, DD *reference) {
  (void)c;
  (void)flags;
  (void)reference;
  return DDERR_NOOVERLAYHW;
}
static DDRESULT API texture_set(DD *c, DWORD value) {
  (void)c;
  (void)value;
  return DDERR_INVALIDCAPS;
}
static DDRESULT API texture_get(DD *c, DWORD *value) {
  (void)c;
  if (value)
    *value = 0;
  return DDERR_INVALIDCAPS;
}
DDRESULT API dd_blt(DD *, const RECT *, DD *, const RECT *, DWORD,
                    const DDFX *);
DDRESULT API dd_blt_fast(DD *, DWORD, DWORD, DD *, const RECT *, DWORD);
#define SURFACE_COMMON                                                         \
  dd_query, dd_add, dd_release, add_attached, overlay_dirty, dd_blt, batch,    \
      dd_blt_fast, delete_attached, enum_attached, overlay_enum, flip,         \
      get_attached, status, get_caps, get_clipper, get_key, get_dc, status,    \
      overlay_position, get_palette, get_pixel, get_desc, surface_initialize,  \
      is_lost, lock_surface, release_dc, restore, set_clipper, set_key,        \
      overlay_set_position, set_palette, unlock_surface, overlay_update,       \
      overlay_display, overlay_order
const void *const dd_surface_vt[5][49] = {
    {SURFACE_COMMON},
    {SURFACE_COMMON, get_dd, page, page},
    {SURFACE_COMMON, get_dd, page, page, set_desc},
    {SURFACE_COMMON, get_dd, page, page, set_desc, set_private, get_private,
     free_private, get_unique, change_unique},
    {SURFACE_COMMON, get_dd, page, page, set_desc, set_private, get_private,
     free_private, get_unique, change_unique, texture_set, texture_get,
     texture_set, texture_get}};
