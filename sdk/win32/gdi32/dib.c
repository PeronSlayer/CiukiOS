/* Pitch-aware, integer-only DIB engine; no platform/window dependencies.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
static DWORD pixel_mask(const struct w32_dib *d) {
  return d->bpp <= 8 ? (1u << d->bpp) - 1
                     : d->masks[0] | d->masks[1] | d->masks[2];
}
BOOL w32_dib_validate(const struct w32_dib *d) {
  if (!d || !d->bits || !d->width || !d->height || d->width > 8192 ||
      d->height > 8192 ||
      (d->bpp != 1 && d->bpp != 4 && d->bpp != 8 && d->bpp != 16 &&
       d->bpp != 24 && d->bpp != 32) ||
      d->pitch < (d->width * d->bpp + 7) / 8 ||
      (uint64_t)(d->height - 1) * d->pitch + (d->width * d->bpp + 7) / 8 >
          d->size ||
      d->top_down > 1)
    return 0;
  if (d->bpp <= 8)
    return d->color_count && d->color_count <= (1u << d->bpp);
  DWORD used = 0;
  for (unsigned i = 0; i < 3; i++) {
    DWORD m = d->masks[i];
    if (!m || (m & used) || (d->bpp < 32 && (m >> d->bpp)))
      return 0;
    used |= m;
    while (!(m & 1))
      m >>= 1;
    if (m & (m + 1))
      return 0;
  }
  return 1;
}
static uint8_t *row(const struct w32_dib *d, int y) {
  return d->bits +
         (d->top_down ? (unsigned)y : d->height - 1 - (unsigned)y) * d->pitch;
}
DWORD w32_dib_raw(const struct w32_dib *d, int x, int y) {
  const uint8_t *p = row(d, y);
  if (d->bpp == 1)
    return (p[x >> 3] >> (7 - (x & 7))) & 1;
  if (d->bpp == 4)
    return (p[x >> 1] >> ((1 - (x & 1)) * 4)) & 15;
  p += (unsigned)x * (d->bpp / 8);
  DWORD v = 0;
  for (unsigned i = 0; i < d->bpp / 8; i++)
    v |= (DWORD)p[i] << (i * 8);
  return v;
}
static unsigned expand(DWORD v, DWORD m) {
  unsigned shift = 0;
  while (!(m & 1)) {
    m >>= 1;
    shift++;
  }
  v = (v >> shift) & m;
  if (m == 31)
    return (v << 3) | (v >> 2);
  if (m == 63)
    return (v << 2) | (v >> 4);
  return (unsigned)((uint64_t)v * 255 / m);
}
DWORD w32_dib_rgb(const struct w32_dib *d, int x, int y) {
  DWORD v = w32_dib_raw(d, x, y);
  if (d->bpp <= 8)
    return v < d->color_count ? d->colors[v] & 0xffffff : 0;
  if (d->masks[0] == 0xff0000 && d->masks[1] == 0xff00 && d->masks[2] == 0xff)
    return v & 0xffffff;
  return (expand(v, d->masks[0]) << 16) | (expand(v, d->masks[1]) << 8) |
         expand(v, d->masks[2]);
}
DWORD w32_dib_encode(const struct w32_dib *d, DWORD rgb) {
  if (d->bpp >= 24 && d->masks[0] == 0xff0000 && d->masks[1] == 0xff00 &&
      d->masks[2] == 0xff)
    return rgb & 0xffffff;
  if (d->bpp == 16 && d->masks[0] == 0xf800 && d->masks[1] == 0x7e0 &&
      d->masks[2] == 31)
    return ((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x7e0) | ((rgb >> 3) & 31);
  if (d->bpp <= 8) {
    unsigned best = 0;
    DWORD distance = 0xffffffff;
    for (unsigned i = 0; i < d->color_count; i++) {
      int r = (int)((rgb >> 16) & 255) - (int)((d->colors[i] >> 16) & 255);
      int g = (int)((rgb >> 8) & 255) - (int)((d->colors[i] >> 8) & 255);
      int b = (int)(rgb & 255) - (int)(d->colors[i] & 255);
      DWORD n = (DWORD)(r * r + g * g + b * b);
      if (n < distance) {
        distance = n;
        best = i;
        if (!n)
          break;
      }
    }
    return best;
  }
  DWORD result = 0;
  for (unsigned i = 0; i < 3; i++) {
    DWORD m = d->masks[i];
    unsigned shift = 0;
    while (!(m & 1)) {
      m >>= 1;
      shift++;
    }
    DWORD c = (rgb >> ((2 - i) * 8)) & 255;
    /* Truncation matches the presenter's 5/6/8-bit quantisation. */
    unsigned bits = 0;
    for (DWORD n = m; n; n >>= 1)
      bits++;
    DWORD value = bits <= 8 ? c >> (8 - bits) : (DWORD)((uint64_t)c * m / 255);
    result |= (value << shift) & d->masks[i];
  }
  return result;
}
void w32_dib_put(struct w32_dib *d, int x, int y, DWORD v) {
  uint8_t *p = row(d, y);
  DWORD mask = pixel_mask(d);
  if (d->bpp == 1) {
    unsigned bit = 7 - (x & 7);
    p[x >> 3] = (uint8_t)((p[x >> 3] & ~(1 << bit)) | ((v & 1) << bit));
    return;
  }
  if (d->bpp == 4) {
    unsigned bit = (1 - (x & 1)) * 4;
    p[x >> 1] = (uint8_t)((p[x >> 1] & ~(15 << bit)) | ((v & 15) << bit));
    return;
  }
  v = (w32_dib_raw(d, x, y) & ~mask) | (v & mask);
  p += (unsigned)x * (d->bpp / 8);
  for (unsigned i = 0; i < d->bpp / 8; i++)
    p[i] = (uint8_t)(v >> (i * 8));
}
/* Truth-table minterms are independent bit planes. This covers all 256 ROPs,
 * including operations whose low Windows encoding word is undocumented. */
DWORD w32_gdi_rop3(unsigned rop, DWORD p, DWORD s, DWORD d) {
  DWORD input[8] = {~p & ~s & ~d, ~p & ~s & d, ~p & s & ~d, ~p & s & d,
                    p & ~s & ~d,  p & ~s & d,  p & s & ~d,  p & s & d};
  DWORD out = 0;
  for (unsigned i = 0; i < 8; i++)
    out |= input[i] & (0u - ((rop >> i) & 1));
  return out;
}
static int source_used(unsigned r) { return ((r ^ (r >> 2)) & 0x33) != 0; }
static int valid_extent(int v) { return v >= -GDI_LIMIT && v <= GDI_LIMIT; }
static int abs_i(int v) { return v < 0 ? -v : v; }
static int source_coord(int origin, int extent, int index) {
  return extent < 0 ? origin - 1 - index : origin + index;
}
static DWORD sample(const struct w32_dib *s, int sx, int sy, int sw, int sh,
                    int i, int j, int dw, int dh, unsigned mode, int *ok) {
  int aw = abs_i(sw), ah = abs_i(sh);
  int x0 = aw == dw ? i : i * aw / dw, y0 = ah == dh ? j : j * ah / dh;
  int x1 = x0 + 1, y1 = y0 + 1;
  if (mode != COLORONCOLOR) {
    if (aw > dw || mode == HALFTONE)
      x1 = ((i + 1) * aw + dw - 1) / dw;
    if (ah > dh || mode == HALFTONE)
      y1 = ((j + 1) * ah + dh - 1) / dh;
  }
  DWORD color = mode == BLACKONWHITE ? 0xffffff : 0;
  uint64_t sum[3] = {0, 0, 0}, weight = 0;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) {
      int px = source_coord(sx, sw, x), py = source_coord(sy, sh, y);
      if (px < 0 || py < 0 || (unsigned)px >= s->width ||
          (unsigned)py >= s->height) {
        *ok = 0;
        return 0;
      }
      DWORD c = w32_dib_rgb(s, px, py);
      if (mode == COLORONCOLOR)
        return c;
      if (mode == BLACKONWHITE)
        color &= c;
      else if (mode == WHITEONBLACK)
        color |= c;
      else {
        int64_t l = (int64_t)i * aw, r = (int64_t)(i + 1) * aw,
                t = (int64_t)j * ah, b = (int64_t)(j + 1) * ah;
        int64_t xl = (int64_t)x * dw, xr = (int64_t)(x + 1) * dw,
                yt = (int64_t)y * dh, yb = (int64_t)(y + 1) * dh;
        if (xl < l)
          xl = l;
        if (xr > r)
          xr = r;
        if (yt < t)
          yt = t;
        if (yb > b)
          yb = b;
        uint64_t w = (uint64_t)(xr - xl) * (uint64_t)(yb - yt);
        weight += w;
        for (unsigned k = 0; k < 3; k++)
          sum[k] += ((c >> (k * 8)) & 255) * w;
      }
    }
  if (mode == HALFTONE && weight) {
    color = 0;
    for (unsigned k = 0; k < 3; k++)
      color |= (DWORD)((sum[k] + weight / 2) / weight) << (k * 8);
  }
  return color;
}
BOOL w32_dib_blt(struct w32_dib *d, int dx, int dy, int dw, int dh,
                 const struct w32_dib *source, int sx, int sy, int sw, int sh,
                 unsigned rop, unsigned mode, const struct w32_dib *pattern,
                 POINT org, DWORD solid, int64_t transparent,
                 int (*clip)(void *, int, int), void *opaque) {
  if (!w32_dib_validate(d) || mode < 1 || mode > 4 || rop > 255 ||
      !valid_extent(dx) || !valid_extent(dy) || !valid_extent(dw) ||
      !valid_extent(dh) || !valid_extent(sx) || !valid_extent(sy) ||
      !valid_extent(sw) || !valid_extent(sh) ||
      (pattern && !w32_dib_validate(pattern)))
    return 0;
  int used = source_used(rop);
  if (used && (!w32_dib_validate(source) || !sw || !sh))
    return 0;
  if (!dw || !dh)
    return 1;
  int w = abs_i(dw), h = abs_i(dh), left = dw < 0 ? dx + dw : dx,
      top = dh < 0 ? dy + dh : dy;
  int right = left + w, bottom = top + h;
  if (left < 0)
    left = 0;
  if (top < 0)
    top = 0;
  if (right > (int)d->width)
    right = (int)d->width;
  if (bottom > (int)d->height)
    bottom = (int)d->height;
  if (right <= left || bottom <= top)
    return 1;
  struct w32_dib copy;
  void *snapshot = 0;
  const struct w32_dib *s = source;
  if (used && (uintptr_t)s->bits < (uintptr_t)d->bits + d->size &&
      (uintptr_t)d->bits < (uintptr_t)s->bits + s->size) {
    snapshot = gdi_alloc(s->size);
    if (!snapshot)
      return 0;
    w32_copy(snapshot, s->bits, s->size);
    copy = *s;
    copy.bits = snapshot;
    s = &copy;
  }
  struct w32_dib pattern_copy;
  void *pattern_snapshot = 0;
  if (pattern && (uintptr_t)pattern->bits < (uintptr_t)d->bits + d->size &&
      (uintptr_t)d->bits < (uintptr_t)pattern->bits + pattern->size) {
    pattern_snapshot = gdi_alloc(pattern->size);
    if (!pattern_snapshot) {
      if (snapshot)
        gdi_free(snapshot, s->size);
      return 0;
    }
    w32_copy(pattern_snapshot, pattern->bits, pattern->size);
    pattern_copy = *pattern;
    pattern_copy.bits = pattern_snapshot;
    pattern = &pattern_copy;
  }
  int same_index = used && s->bpp <= 8 && s->bpp == d->bpp &&
                   s->color_count == d->color_count && mode == COLORONCOLOR;
  if (same_index)
    for (unsigned k = 0; k < s->color_count; k++)
      if (s->colors[k] != d->colors[k]) {
        same_index = 0;
        break;
      }
  int same_pattern = pattern && pattern->bpp <= 8 && pattern->bpp == d->bpp &&
                     pattern->color_count == d->color_count;
  if (same_pattern)
    for (unsigned k = 0; k < pattern->color_count; k++)
      if (pattern->colors[k] != d->colors[k]) {
        same_pattern = 0;
        break;
      }
  for (int y = top; y < bottom; y++)
    for (int x = left; x < right; x++) {
      if (clip && !clip(opaque, x, y))
        continue;
      int i = dw < 0 ? dx - 1 - x : x - dx, j = dh < 0 ? dy - 1 - y : y - dy,
          ok = 1;
      DWORD c = used ? sample(s, sx, sy, sw, sh, i, j, w, h, mode, &ok) : 0;
      if (!ok || (transparent >= 0 && c == (DWORD)transparent))
        continue;
      DWORD p = rop == 0xcc ? 0 : w32_dib_encode(d, solid);
      if (pattern && rop != 0xcc) {
        int px = (int)(((int64_t)x - org.x) % (int)pattern->width),
            py = (int)(((int64_t)y - org.y) % (int)pattern->height);
        if (px < 0)
          px += (int)pattern->width;
        if (py < 0)
          py += (int)pattern->height;
        p = same_pattern ? w32_dib_raw(pattern, px, py)
                         : w32_dib_encode(d, w32_dib_rgb(pattern, px, py));
      }
      DWORD converted =
          same_index ? w32_dib_raw(s, source_coord(sx, sw, i * abs_i(sw) / w),
                                   source_coord(sy, sh, j * abs_i(sh) / h))
          : used     ? w32_dib_encode(d, c)
                     : 0;
      DWORD out = rop == 0xcc
                      ? converted
                      : w32_gdi_rop3(rop, p, converted, w32_dib_raw(d, x, y));
      w32_dib_put(d, x, y, out);
    }
  if (snapshot)
    gdi_free(snapshot, s->size);
  if (pattern_snapshot)
    gdi_free(pattern_snapshot, pattern->size);
  return 1;
}
