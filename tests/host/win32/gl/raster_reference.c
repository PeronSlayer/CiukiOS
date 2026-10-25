/* Independent bounding-box/barycentric traversal, retained from the pre-f7-08
 * scalar backend. Sampling and fragment semantics share the production helpers;
 * this oracle checks span coverage, interpolation and early-reject ordering.
 * All included external definitions are renamed; nm is checked by test_gl.py.
 * SPDX-License-Identifier: MIT */
#include "../../../../sdk/libgl/software/renderer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define sw_triangle reference_unused_triangle
#define sw_fragment reference_fragment
#define sw_line reference_line
#define sw_s3tc reference_s3tc
#define sw_color_format reference_color_format
#define sw_base_format reference_base_format
unsigned reference_base_format(unsigned);
#define mip_lod reference_unused_mip_lod
#include "../../../../sdk/libgl/software/renderer.c"
#undef mip_lod
/* Retain the division-based footprint/LOD oracle from before amendment 1. */
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
      while (size > 1) {
        size >>= 1;
        shift++;
      }
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
    return !lod && t->mag == GL_LINEAR && t->min == GL_NEAREST_MIPMAP_NEAREST
               ? (rho >= 363 ? 129 : 0) : lod + (rho >= 363 ? 128 : 1);
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

#undef sw_triangle
#undef sw_fragment
#undef sw_line
#undef sw_s3tc
#undef sw_color_format
#undef sw_base_format
/* These expected components/filters are independent of the shared sampler. */
void raster_review_sampling_case(const char *which) {
  if (!which || !strcmp(which, "border")) {
    const unsigned formats[] = {GL_RGBA, GL_RGB, GL_ALPHA, GL_LUMINANCE,
                                 GL_LUMINANCE_ALPHA, GL_INTENSITY};
    const int expected[][4] = {{100,20,40,30}, {100,20,40,255}, {0,0,0,30},
                               {100,100,100,255}, {100,100,100,30}, {100,100,100,100}};
    for (unsigned f=0; f<6; f++) {
      uint32_t black = f==1 || f==3 ? 0xff000000 : 0;
      struct sw_texture t={.target=GL_TEXTURE_2D, .wrap_s=GL_CLAMP,
        .wrap_t=GL_CLAMP, .min=GL_LINEAR, .mag=GL_LINEAR,
        .border_color=0x3cc82850};
      t.image[0]=(struct sw_image){.w=1,.h=1,.pixels=&black,.internal=formats[f]};
      int out[4]; sample(&t,0,32768,0,out);
      for(int k=0;k<4;k++) assert(out[k]==expected[f][k]);
      sample(&t,32768,65536,0,out);
      for(int k=0;k<4;k++) assert(out[k]==expected[f][k]);
      t.target=GL_TEXTURE_1D; sample(&t,0,0,0,out);
      for(int k=0;k<4;k++) assert(out[k]==expected[f][k]);
      if(formats[f]==GL_RGB) {
        struct sw_unit u={.mode=GL_DECAL,.texture=&t};
        int primary[4]={11,22,33,44}, previous[4]={11,22,33,44};
        environment(&u,out,primary,previous);
        assert(previous[0]==100 && previous[1]==20 && previous[2]==40 && previous[3]==44);
      }
    }
    puts("PASS amendment2 internal-format border expansion (six formats, 1D/2D, DECAL)");
  }
  if (!which || !strcmp(which, "lod")) {
    uint32_t texels[4]={0xffff0000,0xff0000ff,0xff00ff00,0xffffffff};
    struct sw_texture t={.target=GL_TEXTURE_1D,.wrap_s=GL_REPEAT,
      .wrap_t=GL_REPEAT,.mag=GL_LINEAR};
    t.image[0]=(struct sw_image){.w=4,.h=1,.pixels=texels,.internal=GL_RGBA};
    t.image[1]=(struct sw_image){.w=2,.h=1,.pixels=texels+2,.internal=GL_RGBA};
    t.image[2]=(struct sw_image){.w=1,.h=1,.pixels=texels+3,.internal=GL_RGBA};
    const int filters[]={GL_NEAREST_MIPMAP_NEAREST,GL_NEAREST_MIPMAP_LINEAR};
    for(unsigned f=0;f<2;f++) {
      t.min=filters[f];
      const int lods[]={0,1,64,127,128};
      for(unsigned j=0;j<5;j++) {
        int out[4]; sample(&t,10240,0,lods[j],out);
        assert(out[0]==223 && out[1]==0 && out[2]==32 && out[3]==255);
      }
      int out[4]; sample(&t,10240,0,129,out);
      assert(out[2]==0 && out[0]<255);
      t.mag=GL_NEAREST; sample(&t,10240,0,1,out);
      assert(out[0]>=254 && out[2]==0);
      t.mag=GL_LINEAR;
      /* 1.25 texels/pixel must still magnify through actual plane/LOD setup. */
      uint32_t pixels[16*16]={0};
      struct sw_state s={.width=16,.height=16,.color=pixels,.color_mask=~0u};
      s.unit[0]=(struct sw_unit){.enabled=1,.mode=GL_REPLACE,.texture=&t};
      struct sw_vertex v[3]={{.qw={1048576}}, {.x=256,.uv={{5*65536}},.qw={1048576}},
                             {.y=256,.qw={1048576}}};
      sw_triangle(&s,v,v+1,v+2);
      assert(pixels[15*16]==0xffdf0020);
    }
    puts("PASS amendment2 LINEAR/NEAREST_MIPMAP min-mag switch at LOD 0.5");
  }
}
void raster_review_reciprocal_tests(void) {
  uint32_t rng = 0x243f6a88;
  for (unsigned i=0;i<1000000;i++) {
    rng ^= rng<<13; rng ^= rng>>17; rng ^= rng<<5;
    unsigned d = i<65536 ? 4096+i : 4096+rng%((1u<<27)-4095);
    uint64_t r = reciprocal48(d);
    assert(r == (1ULL<<48)/d);
    for (int sign=-1;sign<=1;sign+=2) {
      int uv=(int)(rng%((1u<<28)+1))-(1<<27), q=sign*(int)d;
      assert(project(uv,q,r) == (int64_t)uv*1048576/q);
    }
  }
  const int64_t bounds[]={INT64_MIN,INT64_MAX,-65537,-65536,-1,0,1,65535,65536};
  for (unsigned i=0;i<sizeof(bounds)/sizeof(*bounds);i++) {
    int64_t old = bounds[i]%65536; if(old<0) old+=65536;
    assert(coordinate(bounds[i],GL_REPEAT)==old);
  }
  puts("PASS amendment2 1000000 exact perspective reciprocals/projections and signed repeat extrema");
}
static int interpolate(int64_t a, int64_t b, int64_t c, int x, int y, int z,
                       int64_t area) {
  return (int)((a * x + b * y + c * z) / area);
}
void raster_reference_triangle(struct sw_state *s, const struct sw_vertex *a,
                               const struct sw_vertex *b,
                               const struct sw_vertex *c) {
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
      gradients[t][0].q =
          gradient(a->qw[t], b->qw[t], c->qw[t], a->y, b->y, c->y, area);
      gradients[t][1].q =
          -gradient(a->qw[t], b->qw[t], c->qw[t], a->x, b->x, c->x, area);
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
            int q = interpolate(e[0], e[1], e[2], a->qw[t], b->qw[t], c->qw[t],
                                area);
            int uv[2], projected[2], tex[4];
            for (int k = 0; k < 2; k++) {
              uv[k] = interpolate(e[0], e[1], e[2], a->uv[t][k], b->uv[t][k],
                                  c->uv[t][k], area);
              projected[k] = coordinate(q ? (int64_t)uv[k] * 1048576 / q : 0,
                                        k ? s->unit[t].texture->wrap_t
                                          : s->unit[t].texture->wrap_s);
            }
            const struct sw_texture *texture = s->unit[t].texture;
            int lod = texture->min == texture->mag
                          ? 0
                          : mip_lod(texture, gradients[t], uv, q);
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

/* Exercise threshold boundaries independently of the triangle traversal and
 * sampler shared above, including large-Q products and sliver fallbacks. */
void raster_reference_lod_tests(void) {
  static const int filters[] = {GL_NEAREST, GL_LINEAR,
      GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
      GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR};
  const unsigned thresholds[] = {257, 363, 512};
  for (unsigned octave = 0; octave <= 11; octave++)
    for (unsigned k = 0; k < 3; k++)
      for (unsigned shift = 0; shift < 55; shift++) {
        uint64_t d = 1ULL << shift;
        uint64_t limit = (uint64_t)thresholds[k] << octave;
        if (d <= UINT64_MAX / limit) {
          uint64_t boundary = d * limit;
          assert(!footprint_ge(boundary - 1, d, 0, octave, thresholds[k]));
          assert(footprint_ge(boundary, d, 0, octave, thresholds[k]));
        } else
          assert(!footprint_ge(UINT64_MAX, d, 0, octave, thresholds[k]));
      }
  uint32_t rng = 0x243f6a88;
  for (unsigned trial = 0; trial < 20000; trial++) {
    int values[8];
    for (unsigned k = 0; k < 8; k++) {
      rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
      values[k] = (int)(rng % (1u << 28)) - (1 << 27);
    }
    int uv[2] = {values[0], values[1]}, q = values[2];
    if (trial % 17 == 0) q = 0;
    if (trial % 19 == 0) q = 1;
    if (trial % 23 == 0) q = -4096;
    struct gradient g[2];
    for (unsigned axis = 0; axis < 2; axis++) {
      int64_t scale = trial % 5 == 0 ? 1LL << 18 : 1;
      g[axis].q = values[3 + axis] * scale;
      for (unsigned k = 0; k < 2; k++)
        g[axis].u[k] = values[4 + axis + k] * scale;
    }
    struct sw_texture t = {.target = trial % 2 ? GL_TEXTURE_1D : GL_TEXTURE_2D,
        .min = filters[trial % 6]};
    t.image[0].w = 1u << (trial % 12);
    t.image[0].h = 1u << ((trial / 12) % 12);
    assert(reference_unused_mip_lod(&t, g, uv, q) == mip_lod(&t, g, uv, q));
  }
}
