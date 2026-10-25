/* Shared scalar Win32 compatibility fragment backend. SPDX-License-Identifier:
 * MIT */
#ifndef CIUKI_GL_SOFTWARE_H
#define CIUKI_GL_SOFTWARE_H
#include <stdint.h>
#define SW_LEVELS 12
#define SW_MAX_SIZE 2048
struct sw_image {
  unsigned w, h;
  uint32_t *pixels;
  void *compressed;
  unsigned bytes, format, internal;
};
struct sw_texture {
  uint64_t revision; /* Private immutable-upload identity, not a public ABI. */
  unsigned name, used, refs, deleted, target;
  unsigned border_color;
  int wrap_s, wrap_t, min, mag;
  struct sw_image image[SW_LEVELS];
};
struct sw_unit {
  int enabled, mode, combine[2], source[2][3], operand[2][3], scale[2];
  unsigned constant;
  struct sw_texture *texture;
};
struct sw_vertex {
  int x, y, z, iw, uv[2][2], qw[2], color[4], fog;
};
struct sw_state {
  unsigned width, height;
  uint32_t *color;
  uint32_t *color2;
  int color_disabled;
  uint16_t *depth;
  int rgb565, depth_test, depth_write, depth_func, alpha_test, alpha_func,
      alpha_ref, blend, sfactor, dfactor;
  unsigned color_mask;
  int scissor, box[4], fog, fog_color, offset;
  struct sw_unit unit[2];
  /* Optional D24/S8 attachments; absent attachments preserve native D16. */
  uint32_t *depth24;
  uint8_t *stencil;
  int stencil_test, stencil_func, stencil_ref;
  unsigned stencil_mask, stencil_write_mask;
  int stencil_fail, stencil_zfail, stencil_zpass;
};
/* Coordinates: 4 fractional bits, S/W,T/W 16, Q/W and reciprocal-W 20.
 * Color 0..255. Clipped before entry; dimensions <=2048; stored UV/Q <=2^27.
 * Edge*interpolant accumulation fits signed 64 bits at these bounds. */
unsigned sw_base_format(unsigned);
uint32_t sw_color_format(const struct sw_state *, uint32_t);
void sw_triangle(struct sw_state *, const struct sw_vertex *,
                 const struct sw_vertex *, const struct sw_vertex *);
void sw_fragment(struct sw_state *, int, int, int, const int[4]);
void sw_line(struct sw_state *, const struct sw_vertex *,
             const struct sw_vertex *);
int sw_s3tc(uint32_t *, unsigned, unsigned, unsigned, const uint8_t *,
            unsigned);
#endif
