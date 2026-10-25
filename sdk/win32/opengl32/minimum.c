/* Capability-neutral translation to the f7-02 minimum; no silent state loss.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "minimum.h"
#include "../../libgl/vendor/include/GL/gl.h"
#include "../../libgl/software/extensions.h"
int gl_hw_minimum_encode(const struct gl_hw_command *g, unsigned target,
    unsigned depth, unsigned copy, struct render3d_request *out) {
  const struct sw_state *s = &g->state;
  *out = (struct render3d_request){0};
  if (!s->width || !s->height || s->width > R3D_MAX_SIZE || s->height > R3D_MAX_SIZE ||
      g->count > R3D_VERTICES || g->count % 3 || s->depth24 || s->rgb565 ||
      s->color_disabled || s->color2 || s->color_mask != UINT32_MAX || s->scissor ||
      (s->stencil_test && s->stencil) || s->blend || s->alpha_test || s->fog ||
      s->offset || s->unit[1].enabled ||
      (s->depth_test && s->depth_func != GL_LESS) ||
      (g->clear_mask & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT))) return -1;
  out->target = target;
  if (g->clear_mask & GL_COLOR_BUFFER_BIT) out->flags |= R3D_CLEAR_COLOR;
  if ((g->clear_mask & GL_DEPTH_BUFFER_BIT) && s->depth_write) out->flags |= R3D_CLEAR_DEPTH;
  if (g->count && s->depth_test) {
    out->flags |= R3D_DEPTH_TEST;
    if (s->depth_write) out->flags |= R3D_DEPTH_WRITE;
  }
  if (out->flags & (R3D_CLEAR_DEPTH | R3D_DEPTH_TEST | R3D_DEPTH_WRITE)) out->depth = depth;
  out->clear_color = g->clear_color;
  if (g->clear_depth > 65535) return -1;
  out->clear_depth = g->clear_depth;
  if (g->count && s->unit[0].enabled) {
    const struct sw_texture *t = s->unit[0].texture;
    if (!t || s->unit[0].mode != GL_MODULATE || !t->image[0].pixels ||
        (sw_base_format(t->image[0].internal) != GL_RGBA &&
         sw_base_format(t->image[0].internal) != GL_RGB) ||
        t->wrap_s != GL_CLAMP_TO_EDGE || t->wrap_t != GL_CLAMP_TO_EDGE ||
        t->min != GL_NEAREST || t->mag != GL_NEAREST) return -1;
    out->flags |= R3D_TEXTURED; out->texture = g->texture[0];
  }
  if (copy) { out->flags |= R3D_COPY; out->copy_target = copy; }
  if (!out->flags) return -1; /* The minimum has no independent DRAW flag. */
  for (unsigned i = 0; i < g->count; i++) {
    const struct sw_vertex *v = &g->vertices[i];
    struct render3d_vertex *r = &out->vertices[i];
    if (v->x < 0 || v->y < 0 || (unsigned)v->x > s->width * 16 ||
        (unsigned)v->y > s->height * 16 || v->z < 0 || v->z > 65535) return -1;
    r->x = v->x * 4096;
    r->y = ((int)s->height * 16 - v->y) * 4096;
    r->z = (unsigned)v->z; r->rhw = 65536;
    for (unsigned k = 0; k < 4; k++) if (v->color[k] < 0 || v->color[k] > 255) return -1;
    r->color = (unsigned)v->color[3] << 24 | (unsigned)v->color[0] << 16 |
               (unsigned)v->color[1] << 8 | (unsigned)v->color[2];
    if (out->flags & R3D_TEXTURED) {
      if (v->qw[0] < 4096 || v->qw[0] > 268435456 || (v->qw[0] & 15)) return -1;
      /* Native interpolation rounds; software planes truncate. Even a sub-Q16
       * difference can choose a different nearest texel. Until equivalence is
       * qualified, admit only constant, exactly representable sampling. */
      const struct sw_vertex *first = &g->vertices[i - i % 3];
      if (v->qw[0] != first->qw[0] || v->uv[0][0] != first->uv[0][0] ||
          v->uv[0][1] != first->uv[0][1]) return -1;
      int64_t uq = (int64_t)v->uv[0][0] * 1048576;
      int64_t tq = (int64_t)v->uv[0][1] * 1048576;
      if (uq % v->qw[0] || tq % v->qw[0]) return -1;
      int64_t u = uq / v->qw[0], t = tq / v->qw[0];
      if (u < 0 || u > 65536 || t < 0 || t > 65536) return -1;
      r->u = (int32_t)u; r->v = (int32_t)t;
      r->rhw = (unsigned)v->qw[0] / 16;
      if ((u * r->rhw) % 65536 || (t * r->rhw) % 65536) return -1;
    }
  }
  out->count = g->count;
  return 0;
}
