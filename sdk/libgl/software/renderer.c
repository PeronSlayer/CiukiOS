/* Bounded integer rasterization, two texture stages, D16 and XRGB8888.
 * SPDX-License-Identifier: MIT */
#include "renderer.h"
#include "../vendor/include/GL/gl.h"
#include "extensions.h"
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
  if (x < 0 || y < 0 || (unsigned)x >= s->width || (unsigned)y >= s->height)
    return;
  if (s->scissor && (x < s->box[0] || y < s->box[1] ||
                     x >= s->box[0] + s->box[2] || y >= s->box[1] + s->box[3]))
    return;
  if (s->alpha_test && !compare(s->alpha_func, rgba[3], s->alpha_ref))
    return;
  unsigned at = (s->height - 1 - (unsigned)y) * s->width + (unsigned)x;
  z = clamp(z + s->offset, 0, 65535);
  if (s->depth_test && !compare(s->depth_func, z, s->depth[at]))
    return;
  if (s->depth_test && s->depth_write)
    s->depth[at] = (uint16_t)z;
  int c[4], d[4];
  unpack(s->color[at], d);
  for (int k = 0; k < 4; k++)
    c[k] = s->blend ? clamp((rgba[k] * factor(s->sfactor, k, rgba, d) +
                             d[k] * factor(s->dfactor, k, rgba, d) + 127) /
                                255,
                            0, 255)
                    : rgba[k];
  unsigned p = pack(c);
  s->color[at] =
      sw_color_format(s, (s->color[at] & ~s->color_mask) | (p & s->color_mask));
}
static int wrap(int x, int n, int mode) {
  if (mode == GL_REPEAT) {
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
    unpack(texel(t, m, (int)(sx >> 16), (int)(sy >> 16)), out);
    return;
  }
  sx -= 32768;
  sy -= 32768;
  int x = (int)(sx >> 16), y = (int)(sy >> 16), fx = (int)(sx & 65535) >> 8,
      fy = (int)(sy & 65535) >> 8;
  int a[4], b[4], c[4], d[4];
  unpack(texel(t, m, x, y), a);
  unpack(texel(t, m, x + 1, y), b);
  unpack(texel(t, m, x, y + 1), c);
  unpack(texel(t, m, x + 1, y + 1), d);
  for (int k = 0; k < 4; k++)
    out[k] = ((a[k] * (256 - fx) + b[k] * fx) * (256 - fy) +
              (c[k] * (256 - fx) + d[k] * fx) * fy + 32768) >>
             16;
}
static void sample(const struct sw_texture *t, int u, int v, int lod,
                   int out[4]) {
  int level = 0, linear = 0, last = 0;
  if (t) {
    unsigned size =
        t->image[0].w > t->image[0].h ? t->image[0].w : t->image[0].h;
    while (size > 1 && last < SW_LEVELS - 1) {
      size >>= 1;
      last++;
    }
    if (t->target == GL_TEXTURE_1D)
      v = 0;
  }
  if (!t) {
    out[0] = out[1] = out[2] = 0;
    out[3] = 255;
    return;
  }
  if (lod > 0) {
    linear = t->min == GL_LINEAR || t->min == GL_LINEAR_MIPMAP_NEAREST ||
             t->min == GL_LINEAR_MIPMAP_LINEAR;
    if (t->min != GL_NEAREST && t->min != GL_LINEAR)
      level = clamp((lod + 128) >> 8, 0, last);
  } else
    linear = t->mag == GL_LINEAR;
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
  int constant[4];
  unsigned base = sw_base_format(u->texture->image[0].internal);
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
static int interpolate(int64_t a, int64_t b, int64_t c, int x, int y, int z,
                       int64_t area) {
  return (int)((a * x + b * y + c * z) / area);
}
/* Reduce reconstructed 16.16 coordinates before narrowing. In particular a
 * positive coordinate beyond INT_MAX must still clamp to the positive edge. */
static int coordinate(int64_t uv, int wrap_mode) {
  if (wrap_mode == GL_REPEAT) {
    uv %= 65536;
    if (uv < 0) uv += 65536;
    return (int)uv;
  }
  return uv < 0 ? 0 : uv > 65536 ? 65536 : (int)uv;
}
struct gradient { int64_t u[2], q; };
static int64_t gradient(int a, int b, int c, int x, int y, int z, int64_t area) {
  return ((int64_t)(b - a) * (z - x) - (int64_t)(c - a) * (y - x)) * 16 / area;
}
/* Saturating n*2^shift/d, without a wide product. d <= 2^54; doubling
 * remainders is safe. The saturation is the largest useful texture footprint. */
static unsigned ratio(uint64_t n, uint64_t d, unsigned shift) {
  const unsigned limit = 2048 * 256;
  uint64_t whole = n / d, rem = n % d;
  if (whole >= limit) return limit;
  while (shift--) {
    whole = whole * 2 + (rem * 2 >= d);
    rem = rem * 2 % d;
    if (whole >= limit) return limit;
  }
  return (unsigned)whole;
}
static int mip_lod(const struct sw_texture *t, const struct gradient g[2],
                   const int uv[2], int q) {
  if (!q) return 11 * 256;
  unsigned rho = 0;
  uint64_t den = (uint64_t)((int64_t)q * q);
  for (int axis = 0; axis < 2; axis++)
    for (int k = 0; k < (t->target == GL_TEXTURE_1D ? 1 : 2); k++) {
      int64_t du = g[axis].u[k], dq = g[axis].q;
      unsigned shift = 12, size = k ? t->image[0].h : t->image[0].w;
      /* Sliver triangles may have enormous per-pixel gradients. Scale both
       * gradients together before the determinant, restore scale in ratio. */
      while (du > (1LL << 30) || du < -(1LL << 30) ||
             dq > (1LL << 30) || dq < -(1LL << 30)) {
        du /= 2; dq /= 2; shift++;
      }
      while (size > 1) { size >>= 1; shift++; }
      int64_t det = du * q - (int64_t)uv[k] * dq;
      unsigned r = ratio(det < 0 ? (uint64_t)-det : (uint64_t)det, den, shift);
      if (r > rho) rho = r;
    }
  if (rho <= 256) return 0;
  int lod = 0;
  while (rho >= 512) { rho >>= 1; lod += 256; }
  /* Binary log in 8 fractional bits, integer only. */
  uint64_t n = (uint64_t)rho << 8;
  for (int bit = 128; bit; bit >>= 1) {
    n = n * n >> 16;
    if (n >= 131072) { n >>= 1; lod += bit; }
  }
  return lod;
}
void sw_triangle(struct sw_state *s, const struct sw_vertex *a,
                 const struct sw_vertex *b, const struct sw_vertex *c) {
  int64_t area = edge(a, b, c->x, c->y);
  if (!area)
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
  int textured[2];
  struct gradient gradients[2][2] = {0};
  for (int t = 0; t < 2; t++)
    textured[t] = s->unit[t].enabled && complete(s->unit[t].texture);
  for (int t = 0; t < 2; t++)
    if (textured[t]) {
      for (int k = 0; k < 2; k++) {
        gradients[t][0].u[k] = gradient(a->uv[t][k], b->uv[t][k], c->uv[t][k],
                                       a->y, b->y, c->y, area);
        gradients[t][1].u[k] = -gradient(a->uv[t][k], b->uv[t][k], c->uv[t][k],
                                        a->x, b->x, c->x, area);
      }
      gradients[t][0].q = gradient(a->qw[t], b->qw[t], c->qw[t], a->y, b->y, c->y, area);
      gradients[t][1].q = -gradient(a->qw[t], b->qw[t], c->qw[t], a->x, b->x, c->x, area);
    }
  /* Incremental edges, pixel centers, half-open shared edges. */
  int64_t row[3] = {edge(b, c, x0 * 16 + 8, y0 * 16 + 8),
                    edge(c, a, x0 * 16 + 8, y0 * 16 + 8),
                    edge(a, b, x0 * 16 + 8, y0 * 16 + 8)};
  int tl[3] = {top(b, c), top(c, a), top(a, b)};
  for (int y = y0; y <= y1; y++) {
    int64_t e[3] = {row[0], row[1], row[2]};
    for (int x = x0; x <= x1; x++) {
      if ((e[0] > 0 || (e[0] == 0 && tl[0])) &&
          (e[1] > 0 || (e[1] == 0 && tl[1])) &&
          (e[2] > 0 || (e[2] == 0 && tl[2]))) {
        int p[4], col[4];
        for (int k = 0; k < 4; k++)
          p[k] = col[k] = interpolate(e[0], e[1], e[2], a->color[k],
                                      b->color[k], c->color[k], area);
        for (int t = 0; t < 2; t++)
          if (textured[t]) {
            int q = interpolate(e[0], e[1], e[2], a->qw[t], b->qw[t], c->qw[t], area);
            int uv[2], projected[2], tex[4];
            for (int k = 0; k < 2; k++) {
              uv[k] = interpolate(e[0], e[1], e[2], a->uv[t][k], b->uv[t][k],
                                  c->uv[t][k], area);
              projected[k] = coordinate(q ? (int64_t)uv[k] * 1048576 / q : 0,
                  k ? s->unit[t].texture->wrap_t : s->unit[t].texture->wrap_s);
            }
            const struct sw_texture *texture = s->unit[t].texture;
            int lod = texture->min == texture->mag ? 0 : mip_lod(texture, gradients[t], uv, q);
            sample(s->unit[t].texture, projected[0], projected[1], lod, tex);
            if (sw_base_format(s->unit[t].texture->image[0].internal) ==
                GL_ALPHA)
              tex[0] = tex[1] = tex[2] = 255;
            environment(&s->unit[t], tex, p, col);
          }
        if (s->fog) {
          int f = clamp(
                  interpolate(e[0], e[1], e[2], a->fog, b->fog, c->fog, area),
                  0, 255),
              fog[4];
          unpack((unsigned)s->fog_color, fog);
          for (int k = 0; k < 3; k++)
            col[k] = (col[k] * f + fog[k] * (255 - f) + 127) / 255;
        }
        sw_fragment(s, x, y,
                    interpolate(e[0], e[1], e[2], a->z, b->z, c->z, area), col);
      }
      e[0] -= (c->y - b->y) * 16;
      e[1] -= (a->y - c->y) * 16;
      e[2] -= (b->y - a->y) * 16;
    }
    row[0] += (c->x - b->x) * 16;
    row[1] += (a->x - c->x) * 16;
    row[2] += (b->x - a->x) * 16;
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
                a->z + (b->z - a->z) * i / n, c);
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
