/* Scalar transfers over the existing DIB engine. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include "internal.h"
static BOOL in_key(DWORD pixel, DDKEY key) {
  return pixel >= key.low && pixel <= key.high;
}
static BOOL visible(const RECT *rects, DWORD n, int x, int y) {
  for (DWORD i = 0; i < n; i++)
    if (x >= rects[i].left && x < rects[i].right && y >= rects[i].top &&
        y < rects[i].bottom)
      return 1;
  return 0;
}
DDRESULT API dd_blt(DD *c, const RECT *dest, DD *source, const RECT *src,
                    DWORD flags, const DDFX *fx) {
  GUARD;
  if (!c || c->kind != 2 || (source && source->kind != 2))
    return DDERR_INVALIDPARAMS;
  struct dd_surface *d = c->owner, *s = source ? source->owner : 0;
  if (flags & ~(DDBLT_COLORFILL | DDBLT_DDFX | DDBLT_KEYDEST |
                DDBLT_KEYDESTOVERRIDE | DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE |
                DDBLT_ROP | DDBLT_WAIT | DDBLT_DONOTWAIT))
    return DDERR_UNSUPPORTED;
  if ((flags & (DDBLT_COLORFILL | DDBLT_DDFX | DDBLT_KEYDESTOVERRIDE |
                DDBLT_KEYSRCOVERRIDE | DDBLT_ROP)) &&
      (!fx || fx->size != sizeof(*fx)))
    return DDERR_INVALIDPARAMS;
  if ((flags & DDBLT_DDFX) &&
      (fx->effects & ~(DDBLTFX_MIRRORLEFTRIGHT | DDBLTFX_MIRRORUPDOWN)))
    return DDERR_UNSUPPORTED;
  unsigned rop = flags & DDBLT_ROP ? (fx->rop >> 16) & 255 : 0xcc;
  /* All sixteen source/destination ROPs; pattern ROPs have no DirectDraw brush.
   */
  if ((rop & 15) != (rop >> 4))
    return DDERR_UNSUPPORTED;
  BOOL fill = !!(flags & DDBLT_COLORFILL),
       uses_source =
           !fill && rop != 0 && rop != 0x55 && rop != 0xaa && rop != 0xff;
  if (uses_source && !s)
    return DDERR_INVALIDPARAMS;
  if ((flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) && !s)
    return DDERR_INVALIDPARAMS;
  DDRESULT r = dd_surface_ready(d, flags, DDBLT_WAIT);
  if (r)
    return r;
  if (s) {
    r = dd_surface_ready(s, flags, DDBLT_WAIT);
    if (r)
      return r;
  }
  RECT dr = dest ? *dest : (RECT){0, 0, d->dib.width, d->dib.height};
  if (dr.left >= dr.right || dr.top >= dr.bottom || dr.left < -32767 ||
      dr.top < -32767 || dr.right > 32767 || dr.bottom > 32767)
    return DDERR_INVALIDRECT;
  RECT sr = {0};
  if (s) {
    sr = src ? *src : (RECT){0, 0, s->dib.width, s->dib.height};
    if (!dd_rect_valid(sr, &s->dib))
      return DDERR_INVALIDRECT;
  }
  RECT clips[DD_MAX_RECTS];
  DWORD count = DD_MAX_RECTS;
  if (d->clipper) {
    r = dd_clip_rects(d->clipper, clips, &count);
    if (r)
      return r;
  } else {
    if ((d->caps & DDSCAPS_PRIMARYSURFACE) &&
        !(d->device->coop & DDSCL_EXCLUSIVE))
      return DDERR_NOCLIPLIST;
    count = 1;
    clips[0] = (RECT){0, 0, d->dib.width, d->dib.height};
  }
  DDKEY sk = {0}, dk = {0};
  BOOL source_key = !!(flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)),
       dest_key = !!(flags & (DDBLT_KEYDEST | DDBLT_KEYDESTOVERRIDE));
  if (source_key) {
    if (flags & DDBLT_KEYSRCOVERRIDE)
      sk = fx->srckey;
    else {
      if (!(s->keys & DDCKEY_SRCBLT))
        return DDERR_NOCOLORKEY;
      sk = s->source_key;
    }
  }
  if (dest_key) {
    if (flags & DDBLT_KEYDESTOVERRIDE)
      dk = fx->destkey;
    else {
      if (!(d->keys & DDCKEY_DESTBLT))
        return DDERR_NOCOLORKEY;
      dk = d->dest_key;
    }
  }
  if ((source_key && sk.low > sk.high) || (dest_key && dk.low > dk.high))
    return DDERR_INVALIDPARAMS;
  struct w32_dib input = {0};
  void *snapshot = 0;
  if (s) {
    input = s->dib;
    uintptr_t a = (uintptr_t)d->dib.bits, b = (uintptr_t)s->dib.bits;
    if (a < b + s->dib.size && b < a + d->dib.size) {
      snapshot = dd_alloc(s->dib.size);
      if (!snapshot)
        return DDERR_OUTOFMEMORY;
      w32_copy(snapshot, s->dib.bits, s->dib.size);
      input.bits = snapshot;
    }
  }
  BOOL raw =
      s && input.bpp == d->dib.bpp && input.masks[0] == d->dib.masks[0] &&
      input.masks[1] == d->dib.masks[1] && input.masks[2] == d->dib.masks[2];
  LONG left = dr.left < 0 ? 0 : dr.left, top = dr.top < 0 ? 0 : dr.top,
       right = dr.right, bottom = dr.bottom;
  if (right > (LONG)d->dib.width)
    right = d->dib.width;
  if (bottom > (LONG)d->dib.height)
    bottom = d->dib.height;
  DWORD effects = flags & DDBLT_DDFX ? fx->effects : 0;
  for (LONG y = top; y < bottom; y++)
    for (LONG x = left; x < right; x++) {
      if (!visible(clips, count, x, y))
        continue;
      DWORD mask = d->dib.bpp == 8
                       ? 255
                       : d->dib.masks[0] | d->dib.masks[1] | d->dib.masks[2];
      DWORD dp = w32_dib_raw(&d->dib, x, y) & mask;
      if (dest_key && !in_key(dp, dk))
        continue;
      DWORD sp = 0;
      if (s && !fill) {
        LONG sx = (LONG)((uint64_t)(x - dr.left) * (sr.right - sr.left) /
                         (dr.right - dr.left));
        LONG sy = (LONG)((uint64_t)(y - dr.top) * (sr.bottom - sr.top) /
                         (dr.bottom - dr.top));
        if (effects & DDBLTFX_MIRRORLEFTRIGHT)
          sx = sr.right - sr.left - 1 - sx;
        if (effects & DDBLTFX_MIRRORUPDOWN)
          sy = sr.bottom - sr.top - 1 - sy;
        DWORD source_mask =
            input.bpp == 8 ? 255
                           : input.masks[0] | input.masks[1] | input.masks[2];
        sx += sr.left;
        sy += sr.top;
        sp = w32_dib_raw(&input, sx, sy) & source_mask;
        if (source_key && in_key(sp, sk))
          continue;
        if (!raw)
          sp = w32_dib_encode(&d->dib, w32_dib_rgb(&input, sx, sy));
      }
      w32_dib_put(&d->dib, x, y,
                  fill ? (DWORD)fx->fill : w32_gdi_rop3(rop, 0, sp, dp));
    }
  dd_free(snapshot);
  d->uniqueness++;
  return dd_present(d, 0, 1);
}
DDRESULT API dd_blt_fast(DD *c, DWORD x, DWORD y, DD *source, const RECT *src,
                         DWORD flags) {
  GUARD;
  if (!c || !source || source->kind != 2 || c->kind != 2 || (flags & ~0x33u))
    return DDERR_INVALIDPARAMS;
  struct dd_surface *d = c->owner, *s = source->owner;
  if (d->clipper)
    return DDERR(574);
  RECT sr = src ? *src : (RECT){0, 0, s->dib.width, s->dib.height};
  if (!dd_rect_valid(sr, &s->dib) || x > d->dib.width || y > d->dib.height ||
      (DWORD)(sr.right - sr.left) > d->dib.width - x ||
      (DWORD)(sr.bottom - sr.top) > d->dib.height - y)
    return DDERR_INVALIDRECT;
  RECT dr = {(LONG)x, (LONG)y, (LONG)x + sr.right - sr.left,
             (LONG)y + sr.bottom - sr.top};
  DWORD mapped = (flags & DDBLTFAST_WAIT ? DDBLT_WAIT : 0) |
                 (flags & DDBLTFAST_SRCCOLORKEY ? DDBLT_KEYSRC : 0) |
                 (flags & DDBLTFAST_DESTCOLORKEY ? DDBLT_KEYDEST : 0);
  return dd_blt(c, &dr, source, &sr, mapped, 0);
}
