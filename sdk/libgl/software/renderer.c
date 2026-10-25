/* Bounded integer rasterization, two texture stages, D16 and XRGB8888.
 * SPDX-License-Identifier: MIT */
#include "renderer.h"
#include "../vendor/include/GL/gl.h"
#include "extensions.h"
#include "profile.h"
static int clamp(int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}
static int ab(int x) { return x < 0 ? -x : x; }
static unsigned pack(const int c[4]) {
  return (unsigned)c[3] << 24 | (unsigned)c[0] << 16 | (unsigned)c[1] << 8 |
         (unsigned)c[2];
}
/* Stored pixel order ARGB; semantic channel order RGBA. */
static void unpack(unsigned p, int c[4]) {
  c[0] = (p >> 16) & 255;
  c[1] = (p >> 8) & 255;
  c[2] = p & 255;
  c[3] = p >> 24;
}
static int compare(int fn, int a, int b) {
  switch (fn) {
  case GL_NEVER:
    return 0;
  case GL_LESS:
    return a < b;
  case GL_EQUAL:
    return a == b;
  case GL_LEQUAL:
    return a <= b;
  case GL_GREATER:
    return a > b;
  case GL_NOTEQUAL:
    return a != b;
  case GL_GEQUAL:
    return a >= b;
  case GL_ALWAYS:
    return 1;
  default:
    return 0;
  }
}
static int factor(int f, int k, const int s[4], const int d[4]) {
  switch (f) {
  case GL_ZERO:
    return 0;
  case GL_ONE:
    return 255;
  case GL_SRC_COLOR:
    return s[k];
  case GL_ONE_MINUS_SRC_COLOR:
    return 255 - s[k];
  case GL_DST_COLOR:
    return d[k];
  case GL_ONE_MINUS_DST_COLOR:
    return 255 - d[k];
  case GL_SRC_ALPHA:
    return s[3];
  case GL_ONE_MINUS_SRC_ALPHA:
    return 255 - s[3];
  case GL_DST_ALPHA:
    return 255;
  case GL_ONE_MINUS_DST_ALPHA:
    return 0;
  case GL_SRC_ALPHA_SATURATE:
    return k == 3 ? 255 : 0;
  default:
    return 0;
  }
}
uint32_t sw_color_format(const struct sw_state *s, uint32_t p) {
  if (!s->rgb565)
    return p;
  unsigned r = (p >> 19) & 31, g = (p >> 10) & 63, b = (p >> 3) & 31;
  return (p & 0xff000000) | ((r << 3 | r >> 2) << 16) |
         ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}
void sw_fragment(struct sw_state *s, int x, int y, int z, const int rgba[4]) {
  SW_COUNT(SW_FRAGMENT, 1);
  if (x < 0 || y < 0 || (unsigned)x >= s->width || (unsigned)y >= s->height)
    return;
  if (s->scissor && (x < s->box[0] || y < s->box[1] ||
                     x >= s->box[0] + s->box[2] || y >= s->box[1] + s->box[3]))
    return;
  if (s->alpha_test && !compare(s->alpha_func, rgba[3], s->alpha_ref))
    return;
  unsigned at = (s->height - 1 - (unsigned)y) * s->width + (unsigned)x;
  int maximum = s->depth24 ? 16777215 : 65535;
  z = clamp(z + s->offset, 0, maximum);
  int stencil = s->stencil && s->stencil_test;
  int pass = !stencil ||
             compare(s->stencil_func, s->stencil_ref & (int)s->stencil_mask,
                     s->stencil[at] & (int)s->stencil_mask);
  int depth_pass = !s->depth_test ||
                   compare(s->depth_func, z,
                           s->depth24 ? (int)s->depth24[at] : s->depth[at]);
  if (stencil) {
    int old = s->stencil[at], value = old;
    int op = !pass         ? s->stencil_fail
             : !depth_pass ? s->stencil_zfail
                           : s->stencil_zpass;
    switch (op) {
    case GL_ZERO:
      value = 0;
      break;
    case GL_REPLACE:
      value = s->stencil_ref;
      break;
    case GL_INCR:
      value = old < 255 ? old + 1 : 255;
      break;
    case GL_DECR:
      value = old > 0 ? old - 1 : 0;
      break;
    case GL_INVERT:
      value = old ^ 255;
      break;
    default:
      break;
    }
    s->stencil[at] =
        (old & ~s->stencil_write_mask) | (value & s->stencil_write_mask);
  }
  if (!pass || !depth_pass)
    return;
  SW_COUNT(SW_WRITE, 1);
  if (s->depth_test && s->depth_write) {
    if (s->depth24)
      s->depth24[at] = (uint32_t)z;
    else
      s->depth[at] = (uint16_t)z;
  }
  if (s->color_disabled)
    return;
  uint32_t *buffers[2] = {s->color, s->color2};
  for (unsigned buffer = 0; buffer < (s->color2 ? 2u : 1u); buffer++) {
    uint32_t *target = buffers[buffer];
    int c[4], d[4];
    unpack(target[at], d);
    if (!s->blend) {
      for (int k = 0; k < 4; k++)
        c[k] = rgba[k];
    } else if (s->sfactor == GL_ONE && s->dfactor == GL_ONE) {
      for (int k = 0; k < 4; k++)
        c[k] = clamp(rgba[k] + d[k], 0, 255);
    } else if (s->sfactor == GL_SRC_ALPHA &&
               s->dfactor == GL_ONE_MINUS_SRC_ALPHA) {
      for (int k = 0; k < 4; k++)
        c[k] = (rgba[k] * rgba[3] + d[k] * (255 - rgba[3]) + 127) / 255;
    } else if (s->sfactor == GL_DST_COLOR && s->dfactor == GL_ZERO) {
      for (int k = 0; k < 4; k++)
        c[k] = (rgba[k] * d[k] + 127) / 255;
    } else {
      for (int k = 0; k < 4; k++)
        c[k] = clamp((rgba[k] * factor(s->sfactor, k, rgba, d) +
                      d[k] * factor(s->dfactor, k, rgba, d) + 127) /
                         255,
                     0, 255);
    }
    unsigned p = pack(c);
    target[at] =
        sw_color_format(s, (target[at] & ~s->color_mask) | (p & s->color_mask));
  }
}
static int wrap(int x, int n, int mode) {
  if (mode == GL_REPEAT) {
    if (!(n & (n - 1)))
      return (int)((unsigned)x & (unsigned)(n - 1));
    x %= n;
    if (x < 0)
      x += n;
    return x;
  }
  return clamp(x, 0, n - 1);
}
static unsigned texel(const struct sw_texture *t, const struct sw_image *m,
                      int x, int y) {
  return m->pixels[wrap(y, (int)m->h, t->wrap_t) * m->w +
                   wrap(x, (int)m->w, t->wrap_s)];
}
/* Border colors have the same base-format component expansion as image data
 * (GL 1.1 Table 3.7). Do this only on the clamp-border path. */
static void border_color(const struct sw_texture *t, unsigned internal, int c[4]) {
  unpack(t->border_color, c);
  unsigned base = sw_base_format(internal);
  if (base == GL_ALPHA)
    c[0] = c[1] = c[2] = 0;
  else if (base == GL_LUMINANCE || base == GL_LUMINANCE_ALPHA || base == GL_INTENSITY)
    c[1] = c[2] = c[0];
  if (base == GL_RGB || base == GL_LUMINANCE)
    c[3] = 255;
  else if (base == GL_INTENSITY)
    c[3] = c[0];
}
static void sample_level(const struct sw_texture *t, int level, int u, int v,
                         int linear, int out[4]) {
  const struct sw_image *m = &t->image[level];
  if (!m->pixels || !m->w || !m->h) {
    out[0] = out[1] = out[2] = 0;
    out[3] = 255;
    return;
  }
  int64_t sx = (int64_t)u * m->w, sy = (int64_t)v * m->h;
  if (!linear) {
    SW_COUNT(SW_TEXEL, 1);
    unpack(texel(t, m, (int)(sx >> 16), (int)(sy >> 16)), out);
    return;
  }
  sx -= 32768;
  SW_COUNT(SW_TEXEL, 4);
  sy -= 32768;
  int x = (int)(sx >> 16), y = (int)(sy >> 16), fx = (int)(sx & 65535) >> 8,
      fy = (int)(sy & 65535) >> 8;
  int a[4], b[4], c[4], d[4];
  int xs[2] = {wrap(x, (int)m->w, t->wrap_s),
               wrap(x + 1, (int)m->w, t->wrap_s)};
  int ys[2] = {wrap(y, (int)m->h, t->wrap_t),
               wrap(y + 1, (int)m->h, t->wrap_t)};
  if (t->wrap_s != GL_CLAMP && t->wrap_t != GL_CLAMP) {
    unsigned row0 = ys[0] * m->w, row1 = ys[1] * m->w;
    unpack(m->pixels[row0 + xs[0]], a);
    unpack(m->pixels[row0 + xs[1]], b);
    unpack(m->pixels[row1 + xs[0]], c);
    unpack(m->pixels[row1 + xs[1]], d);
  } else {
    int *out_color[4] = {a, b, c, d};
    for (int j = 0; j < 4; j++) {
      int xx = x + (j & 1), yy = y + (j >> 1);
      int border = (t->wrap_s == GL_CLAMP && (xx < 0 || (unsigned)xx >= m->w)) ||
                   (t->target != GL_TEXTURE_1D && t->wrap_t == GL_CLAMP &&
                    (yy < 0 || (unsigned)yy >= m->h));
      if (border)
        border_color(t, m->internal, out_color[j]);
      else
        unpack(m->pixels[ys[j >> 1] * m->w + xs[j & 1]], out_color[j]);
    }
  }
  for (int k = 0; k < 4; k++) {
    /* Algebraically identical bilinear weights, with three multiplies per
     * channel instead of six. No intermediate rounding; all values fit int. */
    int upper = (a[k] << 8) + (b[k] - a[k]) * fx;
    int lower = (c[k] << 8) + (d[k] - c[k]) * fx;
    out[k] = ((upper << 8) + (lower - upper) * fy + 32768) >> 16;
  }
}
static void sample(const struct sw_texture *t, int u, int v, int lod,
                   int out[4]) {
  int level = 0, linear = 0, last = 0;
  if (!t) {
    out[0] = out[1] = out[2] = 0;
    out[3] = 255;
    return;
  }
  if (t->target == GL_TEXTURE_1D)
    v = 0;
  int crossover = t->mag == GL_LINEAR &&
      (t->min == GL_NEAREST_MIPMAP_NEAREST || t->min == GL_NEAREST_MIPMAP_LINEAR)
          ? 128 : 0;
  if (lod <= crossover) {
    sample_level(t, 0, u, v, t->mag == GL_LINEAR, out);
    return;
  }
  linear = t->min == GL_LINEAR || t->min == GL_LINEAR_MIPMAP_NEAREST ||
             t->min == GL_LINEAR_MIPMAP_LINEAR;
  if (t->min != GL_NEAREST && t->min != GL_LINEAR) {
    /* Only mip minification needs the chain limit. One scalar BSR replaces
     * the size loop, and magnified/nonmip samples need neither. */
    unsigned size = t->image[0].w > t->image[0].h ? t->image[0].w : t->image[0].h;
    last = size ? clamp(31 - (int)__builtin_clz(size), 0, SW_LEVELS - 1) : 0;
    level = clamp((lod + 128) >> 8, 0, last);
  }
  if (t->min == GL_NEAREST_MIPMAP_LINEAR || t->min == GL_LINEAR_MIPMAP_LINEAR) {
    level = clamp(lod >> 8, 0, last);
    int next = level + 1;
    if (next > last)
      next = level;
    int a[4], b[4], f = lod > 0 ? lod & 255 : 0;
    sample_level(t, level, u, v, linear, a);
    sample_level(t, next, u, v, linear, b);
    for (int k = 0; k < 4; k++)
      out[k] = (a[k] * (256 - f) + b[k] * f + 128) >> 8;
  } else
    sample_level(t, level, u, v, linear, out);
}
unsigned sw_base_format(unsigned f) {
  if (f == 1 || f == GL_LUMINANCE || f == 0x84ea ||
      (f >= 0x803f && f <= 0x8042))
    return GL_LUMINANCE;
  if (f == 2 || f == GL_LUMINANCE_ALPHA || f == 0x84eb ||
      (f >= 0x8043 && f <= 0x8048))
    return GL_LUMINANCE_ALPHA;
  if (f == GL_ALPHA || f == 0x84e9 || (f >= 0x803b && f <= 0x803e))
    return GL_ALPHA;
  if (f == GL_INTENSITY || f == 0x84ec || (f >= 0x804a && f <= 0x804d))
    return GL_INTENSITY;
  if (f == 3 || f == GL_RGB || f == 0x84ed || f == 0x83f0 ||
      (f >= 0x804f && f <= 0x8054))
    return GL_RGB;
  return GL_RGBA;
}
static void environment(const struct sw_unit *u, const int texture[4],
                        const int primary[4], int previous[4]) {
  SW_COUNT(SW_STAGE, 1);
  int constant[4];
  unsigned base = sw_base_format(u->texture->image[0].internal);
  if (base == GL_RGB || base == GL_RGBA) {
    if (u->mode == GL_REPLACE) {
      for (int k = 0; k < 3; k++)
        previous[k] = texture[k];
      if (base == GL_RGBA)
        previous[3] = texture[3];
      return;
    }
    if (u->mode == GL_MODULATE || u->mode == GL_ADD) {
      if (u->mode == GL_MODULATE)
        for (int k = 0; k < 3; k++)
          previous[k] = (previous[k] * texture[k] + 127) / 255;
      else
        for (int k = 0; k < 3; k++)
          previous[k] = clamp(previous[k] + texture[k], 0, 255);
      if (base == GL_RGBA)
        previous[3] = (previous[3] * texture[3] + 127) / 255;
      return;
    }
  }
  unpack(u->constant, constant);
  int out[4];
  for (int k = 0; k < 4; k++) {
    int a = previous[k], b = texture[k], v = a;
    switch (u->mode) {
    case GL_REPLACE:
      v = b;
      break;
    case GL_DECAL:
      v = k == 3 ? a : (a * (255 - texture[3]) + b * texture[3] + 127) / 255;
      break;
    case GL_MODULATE:
      v = (a * b + 127) / 255;
      break;
    case GL_ADD:
      v = k == 3 ? (a * b + 127) / 255 : clamp(a + b, 0, 255);
      break;
    case GL_BLEND:
      v = k == 3 ? (a * b + 127) / 255
                 : (a * (255 - b) + constant[k] * b + 127) / 255;
      break;
    case GL_COMBINE_ARB: {
      int group = k == 3, args[3];
      for (int j = 0; j < 3; j++) {
        const int *p = u->source[group][j] == GL_TEXTURE             ? texture
                       : u->source[group][j] == GL_PRIMARY_COLOR_ARB ? primary
                       : u->source[group][j] == GL_CONSTANT_ARB      ? constant
                                                                     : previous;
        int op = u->operand[group][j];
        args[j] =
            p[(op == GL_SRC_ALPHA || op == GL_ONE_MINUS_SRC_ALPHA) ? 3 : k];
        if (op == GL_ONE_MINUS_SRC_ALPHA || op == GL_ONE_MINUS_SRC_COLOR)
          args[j] = 255 - args[j];
      }
      switch (u->combine[group]) {
      case GL_REPLACE:
        v = args[0];
        break;
      case GL_ADD:
        v = args[0] + args[1];
        break;
      case GL_ADD_SIGNED_ARB:
        v = args[0] + args[1] - 128;
        break;
      case GL_SUBTRACT_ARB:
        v = args[0] - args[1];
        break;
      case GL_INTERPOLATE_ARB:
        v = (args[0] * args[2] + args[1] * (255 - args[2]) + 127) / 255;
        break;
      default:
        v = (args[0] * args[1] + 127) / 255;
        break;
      }
      v = clamp(v * u->scale[group], 0, 255);
      break;
    }
    default:
      break;
    }
    if (u->mode != GL_COMBINE_ARB) {
      if ((base == GL_ALPHA && k < 3) ||
          ((base == GL_RGB || base == GL_LUMINANCE) && k == 3))
        v = a;
      else if (base == GL_INTENSITY && k == 3 && u->mode == GL_ADD)
        v = clamp(a + b, 0, 255);
      else if (base == GL_INTENSITY && k == 3 && u->mode == GL_BLEND)
        v = (a * (255 - b) + constant[k] * b + 127) / 255;
    }
    out[k] = v;
  }
  for (int k = 0; k < 4; k++)
    previous[k] = out[k];
}
static int complete(const struct sw_texture *t) {
  if (!t || !t->image[0].pixels)
    return 0;
  if (t->min == GL_NEAREST || t->min == GL_LINEAR)
    return 1;
  unsigned w = t->image[0].w, h = t->image[0].h;
  for (int level = 0; level < SW_LEVELS; level++) {
    const struct sw_image *m = &t->image[level];
    if (!m->pixels || m->w != w || m->h != h ||
        m->internal != t->image[0].internal)
      return 0;
    if (w == 1 && h == 1)
      return 1;
    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }
  return 0;
}
static int64_t edge(const struct sw_vertex *a, const struct sw_vertex *b, int x,
                    int y) {
  return (int64_t)(b->x - a->x) * (y - a->y) -
         (int64_t)(b->y - a->y) * (x - a->x);
}
static int top(const struct sw_vertex *a, const struct sw_vertex *b) {
  return b->y < a->y || (b->y == a->y && b->x < a->x);
}
/* Reduce reconstructed 16.16 coordinates before narrowing. In particular a
 * positive coordinate beyond INT_MAX must still clamp to the positive edge. */
static int coordinate(int64_t uv, int wrap_mode) {
  if (wrap_mode == GL_REPEAT) {
    /* Unsigned conversion is defined modulo 2^64, including negative UVs.
     * The low 16 bits are exactly the nonnegative fractional coordinate. */
    return (int)((uint64_t)uv & 65535u);
  }
  return uv < 0 ? 0 : uv > 65536 ? 65536 : (int)uv;
}
struct gradient {
  int64_t u[2], q;
};
static int64_t gradient(int a, int b, int c, int x, int y, int z,
                        int64_t area) {
  return ((int64_t)(b - a) * (z - x) - (int64_t)(c - a) * (y - x)) * 16 / area;
}
/* Saturating n*2^shift/d, without a wide product. d <= 2^54; doubling
 * remainders is safe. The saturation is the largest useful texture footprint.
 */
static unsigned ratio(uint64_t n, uint64_t d, unsigned shift) {
  SW_COUNT(SW_WIDE_DIV, 1);
  const unsigned limit = 2048 * 256;
  /* The normal derivative fits one 64-bit division. Retain the bounded
   * bit-at-a-time quotient for slivers that would overflow the shift. */
  if (shift < 64 && n <= (UINT64_MAX >> shift)) {
    uint64_t value = (n << shift) / d;
    return value >= limit ? limit : (unsigned)value;
  }
  uint64_t whole = n / d, rem = n % d;
  if (whole >= limit)
    return limit;
  while (shift--) {
    SW_COUNT(SW_WIDE_DIV, 1);
    whole = whole * 2 + (rem * 2 >= d);
    rem = rem * 2 % d;
    if (whole >= limit)
      return limit;
  }
  return (unsigned)whole;
}
/* Compare floor(n/d), combined with a separately saturated footprint, with
 * threshold*2^octave. Constant thresholds let i686 use multiplies/shifts,
 * not the 64-bit division helper. Shifting n first is exact for this integer
 * inequality. Guard the product instead of overflowing for very large Q. */
static int footprint_ge(uint64_t n, uint64_t d, unsigned rho,
                         unsigned octave, unsigned threshold) {
  return (rho >> octave) >= threshold ||
         (d <= UINT64_MAX / threshold && (n >> octave) >= d * threshold);
}
static int mip_lod(const struct sw_texture *t, const struct gradient g[2],
                   const int uv[2], int q) {
  SW_COUNT(SW_LOD, 1);
  if (!q)
    return 11 * 256;
  unsigned rho = 0;
  uint64_t largest = 0;
  uint64_t den = (uint64_t)((int64_t)q * q);
  for (int axis = 0; axis < 2; axis++)
    for (int k = 0; k < (t->target == GL_TEXTURE_1D ? 1 : 2); k++) {
      int64_t du = g[axis].u[k], dq = g[axis].q;
      unsigned shift = 12, size = k ? t->image[0].h : t->image[0].w;
      /* Sliver triangles may have enormous per-pixel gradients. Scale both
       * gradients together before the determinant, restore scale in ratio. */
      while (du > (1LL << 30) || du < -(1LL << 30) || dq > (1LL << 30) ||
             dq < -(1LL << 30)) {
        du /= 2;
        dq /= 2;
        shift++;
      }
      if (size > 1)
        shift += 31u - (unsigned)__builtin_clz(size);
      int64_t det = du * q - (int64_t)uv[k] * dq;
      uint64_t n = det < 0 ? (uint64_t)-det : (uint64_t)det;
      /* All four derivatives share Q^2. Max and floor-division commute,
       * so ordinary footprints need one division rather than four. */
      if (shift < 64 && n <= (UINT64_MAX >> shift)) {
        n <<= shift;
        if (n > largest)
          largest = n;
      } else {
        unsigned r = ratio(n, den, shift);
        if (r > rho)
          rho = r;
      }
    }
  /* Nonmip and nearest-mip filters need only threshold decisions. Preserve
   * exactly the old floor, half-octave threshold (363) and saturation. The
   * fractional trilinear path below still computes the original quotient. */
  if (t->min == GL_NEAREST || t->min == GL_LINEAR)
    return footprint_ge(largest, den, rho, 0, 257) ? 1 : 0;
  if (t->min == GL_NEAREST_MIPMAP_NEAREST || t->min == GL_LINEAR_MIPMAP_NEAREST) {
    if (!footprint_ge(largest, den, rho, 0, 257))
      return 0;
    unsigned octave = 0;
    while (octave < 11 && footprint_ge(largest, den, rho, octave, 512))
      octave++;
    int half = octave < 11 && footprint_ge(largest, den, rho, octave, 363);
    /* 128 previously encoded only mip rounding. With a 0.5 crossover it
     * must distinguish a footprint above sqrt(2) from magnification. */
    if (!octave && t->mag == GL_LINEAR && t->min == GL_NEAREST_MIPMAP_NEAREST)
      return half ? 129 : 0;
    return (int)octave * 256 + (half ? 128 : 1);
  }
  if (largest) {
    unsigned r = ratio(largest, den, 0);
    if (r > rho)
      rho = r;
  }
  if (rho <= 256)
    return 0;
  int lod = 0;
  while (rho >= 512) {
    rho >>= 1;
    lod += 256;
  }
  /* Nearest-mip filters consume only rounding at half an octave; nonmip
   * filters consume only the min/mag decision. Skip unused fractional bits. */
  if (t->min == GL_NEAREST || t->min == GL_LINEAR)
    return 1;
  if (t->min == GL_NEAREST_MIPMAP_NEAREST || t->min == GL_LINEAR_MIPMAP_NEAREST)
    return lod + (rho >= 363 ? 128 : 1);
  /* Binary log in 8 fractional bits, integer only. */
  uint64_t n = (uint64_t)rho << 8;
  for (int bit = 128; bit; bit >>= 1) {
    n = n * n >> 16;
    if (n >= 131072) {
      n >>= 1;
      lod += bit;
    }
  }
  return lod;
}
/* Exact rational planes. Each span starts with one quotient/remainder per
 * varying attribute, then advances with additions. Floor-normalized residues
 * reproduce C's signed division (toward zero) at every pixel, including UVs
 * crossing zero. There is no affine approximation of perspective coordinates.
 */
struct plane {
  int64_t row, dx, dy, step, step_rem, value, rem;
};
/* Exact floor(2^48/d), 4096 <= d <= 2^27. Normalize to [2^31,2^32),
 * seed with one 32-bit DIV, then refine floor(2^63/normalized) using integer
 * Newton correction. The 16-bit seed leaves only a few quotient corrections.
 * No wide division/remainder helper is called by ordinary perspective spans.
 * Downscaling a floored quotient is exact; upscaling appends <=4 remainder bits.
 */
static uint64_t reciprocal48(unsigned d) {
  unsigned shift = (unsigned)__builtin_clz(d), n = d << shift;
  uint64_t r = (uint64_t)(UINT32_MAX / (n >> 16)) << 15;
  const uint64_t numerator = 1ULL << 63;
  uint64_t product = r * n;
  int64_t error = product > numerator ? -(int64_t)(product - numerator)
                                      : (int64_t)(numerator - product);
  r = (uint64_t)((int64_t)r + (((int64_t)r * (error >> 32)) >> 31));
  product = r * n;
  while (product > numerator) { r--; product -= n; }
  uint64_t remainder = numerator - product;
  while (remainder >= n) { r++; remainder -= n; }
  if (shift <= 15)
    return r >> (15 - shift);
  for (unsigned bit = 15; bit < shift; bit++) {
    r <<= 1;
    remainder <<= 1;
    if (remainder >= n) { r++; remainder -= n; }
  }
  return r;
}
/* A shared reciprocal reconstructs S and T exactly. For |Q| >= 4096,
 * |UV| <= 2^27 makes UV*floor(2^48/|Q|) fit uint64_t. The quotient estimate
 * is at most one low; a remainder comparison corrects it. Small Q retains
 * signed 64-bit division. Equal Q on both texture stages shares the reciprocal.
 * No span approximation and no dependence on host floating-point precision. */
static int64_t project(int uv, int q, uint64_t reciprocal) {
  if (!q)
    return 0;
  if (q == 1048576)
    return uv;
  if (q == -1048576)
    return -(int64_t)uv;
  unsigned den = q < 0 ? (unsigned)-q : (unsigned)q;
  if (den < 4096) {
    SW_COUNT(SW_PERSPECTIVE_DIV, 1);
    return (int64_t)uv * 1048576 / q;
  }
  unsigned n = uv < 0 ? (unsigned)-uv : (unsigned)uv;
  uint64_t value = ((uint64_t)n * reciprocal) >> 28;
  uint64_t remainder = (uint64_t)n * 1048576 - value * den;
  if (remainder >= den)
    value++;
  return (uv < 0) != (q < 0) ? -(int64_t)value : (int64_t)value;
}
static int plane_value(const struct plane *p) {
  return (int)(p->value + (p->value < 0 && p->rem != 0));
}
static void plane_start(struct plane *p, int x, int64_t area) {
  int64_t n = p->row + p->dx * x;
  p->value = n / area;
  p->rem = n % area;
  if (p->rem < 0) {
    p->value--;
    p->rem += area;
  }
}
static void plane_step(struct plane *p, int64_t area) {
  p->value += p->step;
  p->rem += p->step_rem;
  if (p->rem >= area) {
    p->rem -= area;
    p->value++;
  }
}
void sw_triangle(struct sw_state *s, const struct sw_vertex *a,
                 const struct sw_vertex *b, const struct sw_vertex *c) {
  int64_t area = edge(a, b, c->x, c->y);
  if (!area || !s->width || !s->height)
    return;
  if (area < 0) {
    const struct sw_vertex *t = b;
    b = c;
    c = t;
    area = -area;
  }
  int x0 = clamp((a->x < b->x ? (a->x < c->x ? a->x : c->x)
                              : (b->x < c->x ? b->x : c->x)) >>
                     4,
                 0, (int)s->width - 1);
  int y0 = clamp((a->y < b->y ? (a->y < c->y ? a->y : c->y)
                              : (b->y < c->y ? b->y : c->y)) >>
                     4,
                 0, (int)s->height - 1);
  int x1 = clamp((a->x > b->x ? (a->x > c->x ? a->x : c->x)
                              : (b->x > c->x ? b->x : c->x)) >>
                     4,
                 0, (int)s->width - 1);
  int y1 = clamp((a->y > b->y ? (a->y > c->y ? a->y : c->y)
                              : (b->y > c->y ? b->y : c->y)) >>
                     4,
                 0, (int)s->height - 1);
  if (s->scissor) {
    if (x0 < s->box[0])
      x0 = s->box[0];
    if (y0 < s->box[1])
      y0 = s->box[1];
    if (x1 >= s->box[0] + s->box[2])
      x1 = s->box[0] + s->box[2] - 1;
    if (y1 >= s->box[1] + s->box[3])
      y1 = s->box[1] + s->box[3] - 1;
  }
  if (x0 > x1 || y0 > y1)
    return;
  SW_COUNT(SW_TRIANGLE, 1);
  int textured[2], fixed_lod[2] = {-1, -1};
  struct gradient gradients[2][2] = {0};
  for (int t = 0; t < 2; t++) {
    textured[t] = s->unit[t].enabled && complete(s->unit[t].texture);
    if (!textured[t])
      continue;
    for (int k = 0; k < 2; k++) {
      gradients[t][0].u[k] = gradient(a->uv[t][k], b->uv[t][k], c->uv[t][k],
                                      a->y, b->y, c->y, area);
      gradients[t][1].u[k] = -gradient(a->uv[t][k], b->uv[t][k], c->uv[t][k],
                                       a->x, b->x, c->x, area);
    }
    gradients[t][0].q =
        gradient(a->qw[t], b->qw[t], c->qw[t], a->y, b->y, c->y, area);
    gradients[t][1].q =
        -gradient(a->qw[t], b->qw[t], c->qw[t], a->x, b->x, c->x, area);
    const struct sw_texture *tex = s->unit[t].texture;
    if (tex->min == tex->mag)
      fixed_lod[t] = 0;
    else if (a->qw[t] == b->qw[t] && a->qw[t] == c->qw[t])
      fixed_lod[t] = mip_lod(tex, gradients[t], a->uv[t], a->qw[t]);
  }
  int64_t row[3] = {edge(b, c, x0 * 16 + 8, y0 * 16 + 8),
                    edge(c, a, x0 * 16 + 8, y0 * 16 + 8),
                    edge(a, b, x0 * 16 + 8, y0 * 16 + 8)};
  int64_t dx[3] = {-(c->y - b->y) * 16, -(a->y - c->y) * 16,
                   -(b->y - a->y) * 16};
  int64_t dy[3] = {(c->x - b->x) * 16, (a->x - c->x) * 16, (b->x - a->x) * 16};
  int bias[3] = {!top(b, c), !top(c, a), !top(a, b)};
  struct plane planes[12] = {0};
  int values[3][12];
  const struct sw_vertex *vertices[3] = {a, b, c};
  int varying[12], varying_count = 0;
  for (int j = 0; j < 3; j++) {
    const struct sw_vertex *v = vertices[j];
    values[j][0] = v->z;
    for (int k = 0; k < 4; k++)
      values[j][1 + k] = v->color[k];
    values[j][5] = s->fog ? v->fog : 0;
    for (int t = 0; t < 2; t++) {
      values[j][6 + t * 3] = textured[t] ? v->uv[t][0] : 0;
      values[j][7 + t * 3] = textured[t] ? v->uv[t][1] : 0;
      values[j][8 + t * 3] = textured[t] ? v->qw[t] : 0;
    }
  }
  for (int k = 0; k < 12; k++) {
    struct plane *p = &planes[k];
    if (values[0][k] == values[1][k] && values[0][k] == values[2][k]) {
      p->value = values[0][k];
      continue;
    }
    varying[varying_count++] = k;
    for (int j = 0; j < 3; j++) {
      p->row += row[j] * values[j][k];
      p->dx += dx[j] * values[j][k];
      p->dy += dy[j] * values[j][k];
    }
    p->step = p->dx / area;
    p->step_rem = p->dx % area;
    if (p->step_rem < 0) {
      p->step--;
      p->step_rem += area;
    }
  }
  int early_depth = s->depth_test && !(s->stencil && s->stencil_test);
  /* Reuse only for an identical denominator. Horizontal spans commonly have
   * constant Q, including the two lightmap units; no approximation is made. */
  unsigned reciprocal_q[2] = {0, 0};
  uint64_t reciprocal[2] = {0, 0};
  for (int y = y0; y <= y1; y++) {
    int first = 0, last = x1 - x0;
    /* Intersect the three half-open edge inequalities to visit the span only.
     */
    for (int j = 0; j < 3; j++) {
      int64_t e = row[j] - bias[j];
      if (dx[j] > 0 && e < 0) {
        int64_t lo = (-e + dx[j] - 1) / dx[j];
        if (lo > first)
          first = lo > last ? last + 1 : (int)lo;
      } else if (dx[j] < 0) {
        if (e < 0) {
          last = -1;
          break;
        }
        int64_t hi = e / -dx[j];
        if (hi < last)
          last = (int)hi;
      } else if (!dx[j] && e < 0) {
        last = -1;
        break;
      }
    }
    if (first <= last) {
      SW_COUNT(SW_SPAN, 1);
      SW_COUNT(SW_ATTRIBUTE_SETUP, varying_count);
      for (int j = 0; j < varying_count; j++)
        plane_start(&planes[varying[j]], first, area);
      for (int x = x0 + first; x <= x0 + last; x++) {
        SW_COUNT(SW_COVERED, 1);
        int z = plane_value(&planes[0]);
        unsigned at = (s->height - 1 - (unsigned)y) * s->width + (unsigned)x;
        if (early_depth &&
            !compare(s->depth_func,
                     clamp(z + s->offset, 0, s->depth24 ? 16777215 : 65535),
                     s->depth24 ? (int)s->depth24[at] : s->depth[at])) {
          SW_COUNT(SW_EARLY_REJECT, 1);
          goto next_pixel;
        }
        SW_COUNT(SW_SHADED, 1);
        int primary[4], col[4];
        for (int k = 0; k < 4; k++)
          primary[k] = col[k] = plane_value(&planes[1 + k]);
        for (int t = 0; t < 2; t++)
          if (textured[t]) {
            const struct sw_texture *tex = s->unit[t].texture;
            int q = plane_value(&planes[8 + t * 3]), uv[2], projected[2],
                pixel[4];
            unsigned den = q < 0 ? (unsigned)-q : (unsigned)q;
            if (den >= 4096 && den != 1048576 &&
                den != reciprocal_q[t]) {
              reciprocal_q[t] = den;
              if (den == reciprocal_q[1 - t])
                reciprocal[t] = reciprocal[1 - t];
              else {
                SW_COUNT(SW_PERSPECTIVE_DIV, 1);
                reciprocal[t] = reciprocal48(den);
              }
            }
            for (int k = 0; k < 2; k++) {
              uv[k] = plane_value(&planes[6 + t * 3 + k]);
              projected[k] = coordinate(project(uv[k], q, reciprocal[t]),
                                        k ? tex->wrap_t : tex->wrap_s);
            }
            int lod = fixed_lod[t] >= 0 ? fixed_lod[t]
                                        : mip_lod(tex, gradients[t], uv, q);
            sample(tex, projected[0], projected[1], lod, pixel);
            if (sw_base_format(tex->image[0].internal) == GL_ALPHA)
              pixel[0] = pixel[1] = pixel[2] = 255;
            environment(&s->unit[t], pixel, primary, col);
          }
        if (s->fog) {
          int f = clamp(plane_value(&planes[5]), 0, 255), fog[4];
          unpack((unsigned)s->fog_color, fog);
          for (int k = 0; k < 3; k++)
            col[k] = (col[k] * f + fog[k] * (255 - f) + 127) / 255;
        }
        sw_fragment(s, x, y, z, col);
      next_pixel:
        SW_COUNT(SW_ATTRIBUTE_STEP, varying_count);
        for (int j = 0; j < varying_count; j++)
          plane_step(&planes[varying[j]], area);
      }
    }
    for (int j = 0; j < varying_count; j++)
      planes[varying[j]].row += planes[varying[j]].dy;
    for (int j = 0; j < 3; j++)
      row[j] += dy[j];
  }
}
void sw_line(struct sw_state *s, const struct sw_vertex *a,
             const struct sw_vertex *b) {
  int dx = ab(b->x - a->x), dy = ab(b->y - a->y), n = (dx > dy ? dx : dy) / 16;
  if (!n)
    n = 1;
  for (int i = 0; i <= n; i++) {
    int c[4];
    for (int k = 0; k < 4; k++)
      c[k] = a->color[k] + (b->color[k] - a->color[k]) * i / n;
    sw_fragment(s, (a->x + (b->x - a->x) * i / n) >> 4,
                (a->y + (b->y - a->y) * i / n) >> 4,
                a->z + (int)((int64_t)(b->z - a->z) * i / n), c);
  }
}
static unsigned u16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned rgb565(unsigned x) {
  unsigned r = x >> 11, g = (x >> 5) & 63, b = x & 31;
  return 0xff000000 | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) |
         (b << 3 | b >> 2);
}
int sw_s3tc(uint32_t *out, unsigned w, unsigned h, unsigned fmt,
            const uint8_t *src, unsigned size) {
  unsigned block = fmt < GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ? 8 : 16;
  if (!w || !h || w > 2048 || h > 2048 || fmt < 0x83f0 || fmt > 0x83f3 ||
      size != ((w + 3) / 4) * ((h + 3) / 4) * block || !out || !src)
    return 0;
  for (unsigned by = 0; by < h; by += 4)
    for (unsigned bx = 0; bx < w; bx += 4, src += block) {
      const uint8_t *p = src + (block == 16 ? 8 : 0);
      unsigned colors[4] = {rgb565(u16(p)), rgb565(u16(p + 2)), 0, 0};
      int c[2][4];
      unpack(colors[0], c[0]);
      unpack(colors[1], c[1]);
      for (int j = 2; j < 4; j++) {
        int v[4] = {0, 0, 0, 255};
        if (u16(p) > u16(p + 2) || block == 16) {
          for (int k = 0; k < 3; k++)
            v[k] = (c[0][k] * (4 - j) + c[1][k] * (j - 1)) / 3;
        } else if (j == 2) {
          for (int k = 0; k < 3; k++)
            v[k] = (c[0][k] + c[1][k]) / 2;
        } else
          v[3] = fmt == 0x83f0 ? 255 : 0;
        colors[j] = pack(v);
      }
      unsigned alpha[8] = {src[0], src[1], 0, 0, 0, 0, 0, 255};
      if (fmt == 0x83f3) {
        for (int i = 2; i < (src[0] > src[1] ? 8 : 6); i++)
          alpha[i] =
              ((src[0] > src[1] ? 8 - i : 6 - i) * src[0] + (i - 1) * src[1]) /
              (src[0] > src[1] ? 7 : 5);
      }
      uint64_t bits = 0;
      if (fmt == 0x83f3)
        for (int i = 0; i < 6; i++)
          bits |= (uint64_t)src[i + 2] << (8 * i);
      for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 4; x++) {
          unsigned i = y * 4 + x, pix = colors[(p[4 + y] >> (2 * x)) & 3];
          if (fmt == 0x83f2)
            pix = (pix & 0xffffff) |
                  ((unsigned)((src[i / 2] >> (4 * (i % 2))) & 15) * 17 << 24);
          if (fmt == 0x83f3)
            pix = (pix & 0xffffff) | (alpha[(bits >> (3 * i)) & 7] << 24);
          if (bx + x < w && by + y < h)
            out[(by + y) * w + bx + x] = pix;
        }
    }
  return 1;
}
