/* Context helpers shared by the Win32 personality and the native library:
 * errors, defaults, window/scissor mapping and draw-buffer selection.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
void gl_error(struct gl_context *c, unsigned e) {
  if (c && !c->error)
    c->error = e;
}
void gl_defaults(struct gl_context *c) {
  c->sw.stencil_func = GL_ALWAYS;
  c->sw.stencil_mask = c->sw.stencil_write_mask = 255;
  c->sw.stencil_fail = c->sw.stencil_zfail = c->sw.stencil_zpass = GL_KEEP;
  c->sw.depth_write = 1;
  c->sw.depth_func = GL_LESS;
  c->sw.alpha_func = GL_ALWAYS;
  c->sw.sfactor = GL_ONE;
  c->sw.dfactor = GL_ZERO;
  c->sw.color_mask = 0xffffffff;
  c->clear_depth = 1;
  c->line_width = 1;
  c->draw_buffer = c->read_buffer = c->doublebuffer ? GL_BACK : GL_FRONT;
  gl_select_buffer(c);
  c->range[1] = 1;
  c->fog_density = 1;
  c->fog_end = 1;
  c->fog_mode = GL_EXP;
  c->compression_hint = GL_DONT_CARE;
  c->zoom[0] = c->zoom[1] = 1;
  c->pack[0] = c->unpack[0] = 4;
  for (int i = 0; i < 7; i++) {
    c->array[i].type = GL_FLOAT;
    c->array[i].size = i == 1 ? 4 : 3;
  }
  for (int i = 0; i < 2; i++) {
    struct sw_unit *u = &c->sw.unit[i];
    u->mode = GL_MODULATE;
    u->combine[0] = u->combine[1] = GL_MODULATE;
    u->scale[0] = u->scale[1] = 1;
    for (int j = 0; j < 3; j++) {
      u->source[0][j] = u->source[1][j] = j == 0   ? GL_TEXTURE
                                          : j == 1 ? GL_PREVIOUS_ARB
                                                   : GL_CONSTANT_ARB;
      u->operand[0][j] = j == 2 ? GL_SRC_ALPHA : GL_SRC_COLOR;
      u->operand[1][j] = GL_SRC_ALPHA;
    }
    c->texcoord[i][3] = 1;
    for (int j = 0; j < 2; j++) {
      struct sw_texture *t = &c->defaults[i][j];
      *t = (struct sw_texture){.used = 1,
                               .target = j ? GL_TEXTURE_2D : GL_TEXTURE_1D,
                               .wrap_s = GL_REPEAT,
                               .wrap_t = GL_REPEAT,
                               .min = GL_NEAREST_MIPMAP_LINEAR,
                               .mag = GL_LINEAR};
      c->binding[i][j] = t;
    }
    for (int j = 0; j < 4; j++) {
      c->texgen[i][j].mode = GL_EYE_LINEAR;
      if (j < 2)
        c->texgen[i][j].object[j] = c->texgen[i][j].eye[j] = 1;
    }
    u->texture = c->binding[i][1];
  }
}
/* Application window coordinates survive compatibility scaling and resizing.
 * Edge mapping is floor, including negative coordinates. */
int gl_window_coord(struct gl_context *c, double value, int axis) {
  unsigned render = axis ? c->sw.height : c->sw.width;
  unsigned drawable = axis ? c->drawable_height : c->drawable_width;
  double scaled = value * render / drawable;
  if (scaled < -1048576) scaled = -1048576;
  if (scaled > 1048576) scaled = 1048576;
  int n = (int)scaled;
  return scaled < n ? n - 1 : n;
}
void gl_sync_viewport(struct gl_context *c) {
  /* TinyGL adds signed origin+size and converts raster positions to int.
   * Software mapping uses the original application viewport through the
   * bounded gl_window_coord path. Keep only TinyGL's internal copy bounded. */
  int x = c->viewport[0], y = c->viewport[1];
  x = x < -2048 ? -2048 : x > 2048 ? 2048 : x;
  y = y < -2048 ? -2048 : y > 2048 ? 2048 : y;
  glViewport(x, y, c->viewport[2] ? c->viewport[2] : 1,
              c->viewport[3] ? c->viewport[3] : 1);
}
void gl_window_state(struct gl_context *c) {
  int x = gl_window_coord(c, c->scissor[0], 0),
      y = gl_window_coord(c, c->scissor[1], 1);
  c->sw.box[0] = x; c->sw.box[1] = y;
  c->sw.box[2] = gl_window_coord(c, (double)c->scissor[0] + c->scissor[2], 0) - x;
  c->sw.box[3] = gl_window_coord(c, (double)c->scissor[1] + c->scissor[3], 1) - y;
}
unsigned gl_read_color(struct gl_context *c, int x, int y) {
  if (x < 0 || y < 0 || (unsigned)x >= c->drawable_width ||
      (unsigned)y >= c->drawable_height)
    return 0xff000000;
  x = gl_window_coord(c, x, 0); y = gl_window_coord(c, y, 1);
  uint32_t *source = (c->read_buffer == GL_FRONT || c->read_buffer == GL_FRONT_LEFT) ? c->front_color : c->back_color;
  return source[(c->sw.height - 1 - y) * c->sw.width + x] | 0xff000000;
}
void gl_select_buffer(struct gl_context *c) {
  uint32_t *color = (c->draw_buffer == GL_FRONT || c->draw_buffer == GL_FRONT_LEFT) ? c->front_color : c->back_color;
  uint32_t *color2 = c->doublebuffer && c->draw_buffer == GL_FRONT_AND_BACK ? c->front_color : 0;
  int disabled = c->draw_buffer == GL_NONE;
  /* Quake III selects GL_BACK every frame: an unchanged selection keeps the
   * GPU-resident attachments (no readback and no reload, f7-15). */
  if (color == c->sw.color && color2 == c->sw.color2 && disabled == c->sw.color_disabled) return;
  gl_hw_software(c);
  c->sw.color = color; c->sw.color2 = color2; c->sw.color_disabled = disabled;
}
