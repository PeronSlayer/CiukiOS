/* Deterministic differential checks against the old triangle traversal.
 * SPDX-License-Identifier: MIT */
#include "../../../../sdk/libgl/software/extensions.h"
#include "../../../../sdk/libgl/software/renderer.h"
#include "../../../../sdk/libgl/vendor/include/GL/gl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
void raster_reference_triangle(struct sw_state *, const struct sw_vertex *,
                               const struct sw_vertex *,
                               const struct sw_vertex *);
void raster_reference_lod_tests(void);
static unsigned rng = 0xb7e15162;
static unsigned random_value(void) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return rng;
}
static uint32_t colors[2][32 * 32], depth24[2][32 * 32], texels[2][8 * 8];
static uint16_t depth16[2][32 * 32];
static uint8_t stencil[2][32 * 32];
static struct sw_texture textures[2];
static uint64_t now(void) {
  struct timespec t;
  assert(!clock_gettime(CLOCK_MONOTONIC, &t));
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
void gl_raster_tests(void) {
  raster_reference_lod_tests();
  static const int filters[6] = {GL_NEAREST,
                                 GL_LINEAR,
                                 GL_NEAREST_MIPMAP_NEAREST,
                                 GL_LINEAR_MIPMAP_NEAREST,
                                 GL_NEAREST_MIPMAP_LINEAR,
                                 GL_LINEAR_MIPMAP_LINEAR};
  static const int modes[4] = {GL_REPLACE, GL_MODULATE, GL_ADD, GL_BLEND};
  static const int wraps[3] = {GL_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP};
  for (unsigned t = 0; t < 2; t++) {
    textures[t] = (struct sw_texture){.target = GL_TEXTURE_2D};
    for (unsigned i = 0; i < 64; i++)
      texels[t][i] = random_value();
    for (unsigned l = 0; l < 4; l++)
      textures[t].image[l] = (struct sw_image){
          .w = 8 >> l, .h = 8 >> l, .pixels = texels[t], .internal = GL_RGBA};
  }
  unsigned hash = 2166136261u;
  for (unsigned trial = 0; trial < 1500; trial++) {
    struct sw_vertex v[3] = {0};
    for (unsigned j = 0; j < 3; j++) {
      v[j].x = (int)(random_value() % 1200) - 300;
      v[j].y = (int)(random_value() % 1200) - 300;
      v[j].z = random_value() % 65536;
      v[j].fog = random_value() % 256;
      for (unsigned k = 0; k < 4; k++)
        v[j].color[k] = random_value() % 256;
      for (unsigned t = 0; t < 2; t++) {
        v[j].qw[t] = (int)(random_value() % (1 << 25)) - (1 << 24);
        for (unsigned k = 0; k < 2; k++)
          v[j].uv[t][k] = (int)(random_value() % (1 << 28)) - (1 << 27);
      }
    }
    if (trial % 11 == 0)
      for (unsigned j = 0; j < 3; j++)
        v[j].qw[0] = v[j].qw[1] = 1048576;
    if (trial % 13 == 0) {
      int q[2] = {(int)(random_value() % (1 << 27)) + 4096,
                  -(int)(random_value() % (1 << 27)) - 4096};
      if (trial % 26 == 0) q[1] = q[0];
      for (unsigned j = 0; j < 3; j++)
        for (unsigned t = 0; t < 2; t++) v[j].qw[t] = q[t];
    }
    struct sw_state s = {.width = 32,
                         .height = 32,
                         .depth_test = trial % 2,
                         .depth_write = trial % 3 != 0,
                         .depth_func = GL_NEVER + trial % 8,
                         .color_mask = trial % 5 ? 0xffffffff : 0x00ff00ff,
                         .alpha_test = trial % 4 == 0,
                         .alpha_func = GL_NEVER + trial % 8,
                         .alpha_ref = trial % 256,
                         .blend = trial % 3 == 0,
                         .sfactor = trial % 2 ? GL_SRC_ALPHA : GL_ONE,
                         .dfactor = trial % 2 ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE,
                         .offset = (int)(trial % 17) - 8,
                         .scissor = trial % 7 == 0,
                         .box = {3, 4, 25, 20},
                         .fog = trial % 5 == 0,
                         .fog_color = 0xff336699,
                         .rgb565 = trial % 3 == 1,
                         .stencil_test = trial % 6 == 0,
                         .stencil_func = GL_NEVER + trial % 8,
                         .stencil_ref = trial % 256,
                         .stencil_mask = trial % 256,
                         .stencil_write_mask = trial * 7 % 256,
                         .stencil_fail = GL_INCR,
                         .stencil_zfail = GL_DECR,
                         .stencil_zpass = GL_REPLACE};
    for (unsigned t = 0; t < 2; t++) {
      textures[t].min = filters[(trial + t) % 6];
      textures[t].mag = trial % 2 ? GL_NEAREST : GL_LINEAR;
      textures[t].wrap_s = wraps[trial % 3];
      textures[t].wrap_t = wraps[(trial + 1) % 3];
      textures[t].border_color = 0x7f123456;
      s.unit[t] = (struct sw_unit){.enabled = trial % (t + 2) != 0,
                                   .mode = modes[(trial + t) % 4],
                                   .texture = &textures[t],
                                   .constant = 0x7f996633};
    }
    for (unsigned i = 0; i < 32 * 32; i++) {
      colors[0][i] = colors[1][i] = random_value();
      depth16[0][i] = depth16[1][i] = random_value() % 65536;
      depth24[0][i] = depth24[1][i] = random_value() % 65536;
      stencil[0][i] = stencil[1][i] = random_value() % 256;
    }
    for (unsigned copy = 0; copy < 2; copy++) {
      s.color = colors[copy];
      s.depth = depth16[copy];
      s.depth24 = trial % 3 == 0 ? depth24[copy] : 0;
      s.stencil = stencil[copy];
      if (copy)
        raster_reference_triangle(&s, v, v + 1, v + 2);
      else
        sw_triangle(&s, v, v + 1, v + 2);
    }
    assert(!memcmp(colors[0], colors[1], sizeof(colors[0])));
    assert(!memcmp(depth16[0], depth16[1], sizeof(depth16[0])));
    assert(!memcmp(depth24[0], depth24[1], sizeof(depth24[0])));
    assert(!memcmp(stencil[0], stencil[1], sizeof(stencil[0])));
    for (unsigned i = 0; i < 32 * 32; i++)
      hash = (hash ^ colors[0][i]) * 16777619u;
  }
  /* Adjacent D24 values must remain distinct above the D16 range. */
  struct sw_state precise = {.width = 1, .height = 1, .color = colors[0],
      .depth24 = depth24[0], .depth_test = 1, .depth_write = 1,
      .depth_func = GL_LESS, .color_mask = 0xffffffff};
  int red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
  depth24[0][0] = 1000000;
  sw_fragment(&precise, 0, 0, 999999, red);
  assert(depth24[0][0] == 999999 && colors[0][0] == 0xffff0000);
  precise.depth_func = GL_EQUAL;
  sw_fragment(&precise, 0, 0, 1000000, green);
  assert(colors[0][0] == 0xffff0000);
  sw_fragment(&precise, 0, 0, 999999, green);
  assert(colors[0][0] == 0xff00ff00);
  /* A same-process bound tolerates scheduling jitter, but catches loss of the
   * early depth path that dominates occluded Quake world surfaces. */
  struct sw_vertex a = {.x = 0,
                        .y = 0,
                        .z = 50000,
                        .color = {255, 255, 255, 255},
                        .qw = {1048576, 1048576}},
                   b = a, c = a;
  b.x = 32 * 16;
  c.y = 32 * 16;
  b.uv[0][0] = 65536;
  c.uv[0][1] = 65536;
  struct sw_state s = {.width = 32,
                       .height = 32,
                       .color = colors[0],
                       .depth = depth16[0],
                       .depth_test = 1,
                       .depth_func = GL_LESS,
                       .color_mask = 0xffffffff};
  textures[0].min = textures[0].mag = GL_LINEAR;
  s.unit[0] = (struct sw_unit){
      .enabled = 1, .mode = GL_MODULATE, .texture = &textures[0]};
  memset(depth16[0], 0, sizeof(depth16[0]));
  uint64_t start = now();
  for (unsigned i = 0; i < 400; i++)
    sw_triangle(&s, &a, &b, &c);
  uint64_t fast = now() - start;
  start = now();
  for (unsigned i = 0; i < 400; i++)
    raster_reference_triangle(&s, &a, &b, &c);
  uint64_t reference = now() - start;
  assert(fast <= reference * 2 + 10000000);
  printf("PASS 20000 independent LOD cases and 1500 raster differential cases: coverage, signed UV/Q, six "
         "filters, fog, D16/D24/S8, masks; hash=%u early_ns=%llu "
         "reference_ns=%llu\n",
         hash, (unsigned long long)fast, (unsigned long long)reference);
}
