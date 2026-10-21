/* Eye-space clipping and per-unit texture coordinate generation.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
static double dot(const double p[4], const V4 *v) {
  double d = 0;
  for (int k = 0; k < 4; k++)
    d += p[k] * v->v[k];
  return d;
}
static void eye_plane(struct gl_context *c, double out[4], const double in[4]) {
  M4 inverse;
  gl_M4_Inv(&inverse, c->tiny.matrix_stack_ptr[0]);
  for (int k = 0; k < 4; k++) {
    out[k] = 0;
    for (int j = 0; j < 4; j++)
      out[k] += in[j] * inverse.m[j][k];
  }
}
int gl_user_visible(struct gl_context *c, const V4 *eye) {
  for (int i = 0; i < 6; i++)
    if (c->clip_enabled[i] && dot(c->clip_plane[i], eye) < 0)
      return 0;
  return 1;
}
void gl_vertex_generated(GLContext *tiny, GLVertex *v) {
  struct gl_context *c = tiny->software_state;
  gl_M4_MulV4(&v->ec, tiny->matrix_stack_ptr[0], &v->coord);
  M4 inverse;
  int sphere = 0;
  for (int u = 0; u < 2; u++)
    for (int k = 0; k < 2; k++)
      sphere |=
          c->texgen[u][k].enabled && c->texgen[u][k].mode == GL_SPHERE_MAP;
  float reflection[3] = {0}, divisor = 0;
  if (sphere) {
    gl_M4_Inv(&inverse, tiny->matrix_stack_ptr[0]);
    float n[3] = {0}, eye[3], length = 0;
    for (int k = 0; k < 3; k++) {
      for (int j = 0; j < 3; j++)
        n[k] += inverse.m[j][k] * tiny->current_normal.v[j];
      eye[k] = v->ec.v[k];
      length += eye[k] * eye[k];
    }
    float nl = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
    if (tiny->normalize_enabled && nl > 0)
      for (int k = 0; k < 3; k++)
        n[k] /= (float)sqrt(nl);
    if (length > 0)
      for (int k = 0; k < 3; k++)
        eye[k] /= (float)sqrt(length);
    float d = n[0] * eye[0] + n[1] * eye[1] + n[2] * eye[2];
    for (int k = 0; k < 3; k++)
      reflection[k] = eye[k] - 2 * n[k] * d;
    divisor = 2 * (float)sqrt(reflection[0] * reflection[0] +
                              reflection[1] * reflection[1] +
                              (reflection[2] + 1) * (reflection[2] + 1));
  }
  for (int u = 0; u < 2; u++) {
    V4 coord = u ? tiny->current_tex_coord1 : tiny->current_tex_coord;
    for (int k = 0; k < 4; k++) {
      struct gl_texgen *g = &c->texgen[u][k];
      if (g->enabled)
        coord.v[k] = g->mode == GL_OBJECT_LINEAR
                         ? (float)dot(g->object, &v->coord)
                     : g->mode == GL_EYE_LINEAR ? (float)dot(g->eye, &v->ec)
                     : divisor > 0 ? reflection[k < 2 ? k : 0] / divisor + 0.5f
                                   : 0.5f;
    }
    gl_M4_MulV4(u ? &v->tex_coord1 : &v->tex_coord,
                tiny->software_texture_matrix[u], &coord);
  }
}
static GLVertex interpolate(const GLVertex *a, const GLVertex *b, double t) {
  GLVertex v = *a;
  for (int k = 0; k < 4; k++) {
#define LERP(field)                                                            \
  v.field.v[k] = (float)(a->field.v[k] + (b->field.v[k] - a->field.v[k]) * t)
    LERP(pc);
    LERP(ec);
    LERP(color);
    LERP(tex_coord);
    LERP(tex_coord1);
#undef LERP
  }
  return v;
}
static void clip_triangle(GLContext *tiny, GLVertex *a, GLVertex *b,
                          GLVertex *d, int plane) {
  struct gl_context *c = tiny->software_state;
  while (plane < 6 && !c->clip_enabled[plane])
    plane++;
  if (plane == 6) {
    gl_triangle(tiny, a, b, d);
    return;
  }
  GLVertex *input[3] = {a, b, d}, output[4];
  unsigned n = 0;
  for (int i = 0; i < 3; i++) {
    GLVertex *x = input[i], *y = input[(i + 1) % 3];
    double dx = dot(c->clip_plane[plane], &x->ec),
           dy = dot(c->clip_plane[plane], &y->ec);
    if (dx >= 0)
      output[n++] = *x;
    if ((dx >= 0) != (dy >= 0))
      output[n++] = interpolate(x, y, dx / (dx - dy));
  }
  for (unsigned i = 2; i < n; i++)
    clip_triangle(tiny, output, output + i - 1, output + i, plane + 1);
}
void gl_clip_triangle(GLContext *tiny, GLVertex *a, GLVertex *b, GLVertex *d) {
  GLVertex x = *a, y = *b;
  if (tiny->current_shade_model == GL_FLAT)
    x.color = y.color = d->color;
  clip_triangle(tiny, &x, &y, d, 0);
}
int gl_clip_line(struct gl_context *c, GLVertex *a, GLVertex *b) {
  for (int i = 0; i < 6; i++)
    if (c->clip_enabled[i]) {
      double da = dot(c->clip_plane[i], &a->ec),
             db = dot(c->clip_plane[i], &b->ec);
      if (da < 0 && db < 0)
        return 0;
      if ((da < 0) != (db < 0)) {
        GLVertex v = interpolate(a, b, da / (da - db));
        if (da < 0)
          *a = v;
        else
          *b = v;
      }
    }
  return 1;
}
int gl_geometry(struct gl_context *c, unsigned id, const union gl_arg *a,
                union gl_arg *r) {
  (void)r;
  if (id == ID_glClipPlane || id == ID_glGetClipPlane) {
    int p = (int)a[0].i - GL_CLIP_PLANE0;
    if (p < 0 || p >= 6)
      gl_error(c, GL_INVALID_ENUM);
    else if (!a[1].p)
      gl_error(c, GL_INVALID_VALUE);
    else if (id == ID_glClipPlane)
      eye_plane(c, c->clip_plane[p], a[1].p);
    else
      w32_copy((void *)a[1].p, c->clip_plane[p], 4 * sizeof(double));
    return 1;
  }
  if ((id >= ID_glTexGend && id <= ID_glTexGeniv) ||
      (id >= ID_glGetTexGendv && id <= ID_glGetTexGeniv)) {
    int k = (int)a[0].i - GL_S, pname = a[1].i;
    if (k < 0 || k >= 4 ||
        (pname != GL_TEXTURE_GEN_MODE && pname != GL_OBJECT_PLANE &&
         pname != GL_EYE_PLANE)) {
      gl_error(c, GL_INVALID_ENUM);
      return 1;
    }
    struct gl_texgen *g = &c->texgen[c->active][k];
    int query = id >= ID_glGetTexGendv && id <= ID_glGetTexGeniv;
    int vector = query || id == ID_glTexGendv || id == ID_glTexGenfv ||
                 id == ID_glTexGeniv;
    int dbl = id == ID_glTexGendv || id == ID_glGetTexGendv;
    int integer = id == ID_glTexGeniv || id == ID_glGetTexGeniv;
    if (vector && !a[2].p) {
      gl_error(c, GL_INVALID_VALUE);
      return 1;
    }
    int n = pname == GL_TEXTURE_GEN_MODE ? 1 : 4;
    if (!vector && n != 1) {
      gl_error(c, GL_INVALID_ENUM);
      return 1;
    }
    double value[4];
    for (int j = 0; j < n; j++) {
      if (query) {
        double v = pname == GL_TEXTURE_GEN_MODE ? g->mode
                   : pname == GL_OBJECT_PLANE   ? g->object[j]
                                                : g->eye[j];
        if (dbl)
          ((double *)a[2].p)[j] = v;
        else if (integer)
          ((int *)a[2].p)[j] = (int)v;
        else
          ((float *)a[2].p)[j] = (float)v;
      } else
        value[j] = vector               ? dbl       ? ((const double *)a[2].p)[j]
                                          : integer ? ((const int *)a[2].p)[j]
                                                    : ((const float *)a[2].p)[j]
                   : id == ID_glTexGeni ? a[2].i
                                        : a[2].f;
    }
    if (!query) {
      if (pname == GL_TEXTURE_GEN_MODE) {
        if (value[0] != GL_OBJECT_LINEAR && value[0] != GL_EYE_LINEAR &&
            (value[0] != GL_SPHERE_MAP || k >= 2))
          gl_error(c, GL_INVALID_ENUM);
        else
          g->mode = (int)value[0];
      } else if (pname == GL_OBJECT_PLANE)
        w32_copy(g->object, value, sizeof(value));
      else
        eye_plane(c, g->eye, value);
    }
    return 1;
  }
  return 0;
}
