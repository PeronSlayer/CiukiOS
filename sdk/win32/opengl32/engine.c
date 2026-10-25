/* GL 1.1 ABI personality over TinyGL and the scalar fragment backend.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include "../../libgl/software/profile.h"
static int clampi(int x, int a, int b) { return x < a ? a : x > b ? b : x; }
static float compat_clamp(float x, float a, float b) {
  return !(x >= a) ? a : x > b ? b : x;
}
static unsigned rgba(const float *v) {
  return (unsigned)(compat_clamp(v[3], 0, 1) * 255) << 24 |
         (unsigned)(compat_clamp(v[0], 0, 1) * 255) << 16 |
         (unsigned)(compat_clamp(v[1], 0, 1) * 255) << 8 |
         (unsigned)(compat_clamp(v[2], 0, 1) * 255);
}
static int convert(struct gl_context *c, GLVertex *v, struct sw_vertex *out) {
  if (!(v->pc.W > 0.00001f) || v->pc.W > 1000000)
    return 0;
  float iw = 1 / v->pc.W;
  if (iw > 64)
    return 0;
  float x = v->pc.X * iw, y = v->pc.Y * iw, z = v->pc.Z * iw;
  out->x =
      clampi(gl_window_coord(c, (c->viewport[0] + (x + 1) * c->viewport[2] * 0.5f) * 16, 0),
             -32768, 65536);
  out->y =
      clampi(gl_window_coord(c, (c->viewport[1] + (y + 1) * c->viewport[3] * 0.5f) * 16, 1),
             -32768, 65536);
  out->z = (int)(compat_clamp(c->range[0] +
                                  (z + 1) * 0.5f * (c->range[1] - c->range[0]),
                              0, 1) *
                 (c->depth_bits == 24 ? 16777215 : 65535));
  out->iw = (int)(iw * 1048576);
  for (int k = 0; k < 4; k++)
    out->color[k] = (int)(compat_clamp(v->color.v[k], 0, 1) * 255);
  for (int t = 0; t < 2; t++) {
    V4 *uv = t ? &v->tex_coord1 : &v->tex_coord;
    out->qw[t] = (int)(compat_clamp(uv->v[3] * iw, -128, 128) * 1048576);
    for (int k = 0; k < 2; k++)
      out->uv[t][k] =
          (int)(compat_clamp(uv->v[k] * iw, -2048, 2048) * 65536);
  }
  float dist = v->ec.Z;
  if (dist < 0)
    dist = -dist;
  float f = 1;
  if (c->sw.fog) {
    if (c->fog_mode == GL_LINEAR)
      f = c->fog_end == c->fog_start
              ? 0
              : (c->fog_end - dist) / (c->fog_end - c->fog_start);
    else {
      float d = c->fog_density * dist;
      if (c->fog_mode == GL_EXP2)
        d *= d;
      f = (float)exp(-d);
    }
  }
  out->fog = (int)(compat_clamp(f, 0, 1) * 255);
  return 1;
}
void gl_triangle(GLContext *tiny, GLVertex *a, GLVertex *b, GLVertex *d) {
  struct gl_context *c = tiny->software_state;
  struct sw_vertex v[3];
  if (!convert(c, a, v) || !convert(c, b, v + 1) || !convert(c, d, v + 2))
    return;
  if (tiny->current_shade_model == GL_FLAT)
    for (int i = 0; i < 2; i++)
      for (int k = 0; k < 4; k++)
        v[i].color[k] = v[2].color[k] = (int)(compat_clamp(tiny->software_flat_color.v[k], 0, 1) * 255);
  c->sw.offset = 0;
  if (tiny->offset_states & TGL_OFFSET_FILL) {
    int64_t area = (int64_t)(v[1].x - v[0].x) * (v[2].y - v[0].y) -
                   (int64_t)(v[1].y - v[0].y) * (v[2].x - v[0].x);
    if (area) {
      double dzdx = ((double)(v[1].z - v[0].z) * (v[2].y - v[0].y) -
                     (double)(v[2].z - v[0].z) * (v[1].y - v[0].y)) *
                    16 / area;
      double dzdy = ((double)(v[2].z - v[0].z) * (v[1].x - v[0].x) -
                     (double)(v[1].z - v[0].z) * (v[2].x - v[0].x)) *
                    16 / area;
      if (dzdx < 0)
        dzdx = -dzdx;
      if (dzdy < 0)
        dzdy = -dzdy;
      c->sw.offset = (int)compat_clamp(
          (float)(c->offset_units +
                  c->offset_factor * (dzdx > dzdy ? dzdx : dzdy)),
          c->depth_bits == 24 ? -16777215 : -65535,
          c->depth_bits == 24 ? 16777215 : 65535);
    }
  }
  sw_triangle(&c->sw, v, v + 1, v + 2);
  c->sw.offset = 0;
}
void gl_compat_line(GLContext *tiny, GLVertex *a, GLVertex *b) {
  struct gl_context *c = tiny->software_state;
  c->sw.offset = 0;
  int point = a == b;
  GLVertex ca = *a, cb = *b;
  if (tiny->current_shade_model == GL_FLAT)
    ca.color = cb.color = tiny->software_flat_color;
  if (!gl_clip_line(c, &ca, &cb))
    return;
  a = &ca;
  b = &cb;
  struct sw_vertex x, y;
  if (convert(c, a, &x) && convert(c, b, &y)) {
    if (point) {
      int n = (int)(c->zb->pointsize + 0.5f);
      if (n < 1)
        n = 1;
      if (n > 64)
        n = 64;
      int px = (x.x >> 4) - n / 2, py = (x.y >> 4) - n / 2;
      for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++)
          sw_fragment(&c->sw, px + i, py + j, x.z, x.color);
    } else {
      int width = (int)compat_clamp(c->line_width + 0.5f, 1, 64);
      int dx = y.x - x.x, dy = y.y - x.y;
      int horizontal = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
      for (int i = 0; i < width; i++) {
        struct sw_vertex a = x, b = y;
        int offset = (i - (width - 1) / 2) * 16;
        if (horizontal) { a.y += offset; b.y += offset; }
        else { a.x += offset; b.x += offset; }
        sw_line(&c->sw, &a, &b);
      }
    }
  }
}
void gl_free_list(struct gl_list *l) {
  struct gl_command *p = l->head;
  while (p) {
    struct gl_command *n = p->next;
    gl_dealloc(p);
    p = n;
  }
  *l = (struct gl_list){0};
}
/* Sizes of fixed input vectors, generated from the retained ABI, never outputs.
 */
static unsigned vector_size(unsigned id) {
  switch (id) {
#include "vectors.inc"
  default:
    return 0;
  }
}
int gl_record(struct gl_context *c, unsigned id, const union gl_arg *a) {
  if (!c->compiling || c->replaying)
    return 0;
  unsigned bytes = vector_size(id), pointer = 0;
  int rectangle = id == ID_glRectdv || id == ID_glRectfv || id == ID_glRectiv ||
                  id == ID_glRectsv;
  unsigned second = rectangle ? bytes : 0;
  if (id >= ID_glMultiTexCoord1dvARB && id <= ID_glMultiTexCoord4svARB && bytes)
    pointer = 1;
  switch (id) {
  case ID_glBitmap: {
    int w = (int)a[0].i, h = (int)a[1].i;
    if (w < 0 || h < 0 || w > 2048 || h > 2048) {
      gl_error(c, GL_INVALID_VALUE);
      return 1;
    }
    struct gl_bitmap_layout layout;
    unsigned error = gl_bitmap_layout(w, h, c->unpack, &layout);
    if (error) {
      gl_error(c, error);
      c->compile_failed = 1;
      return 1;
    }
    bytes = a[6].p ? layout.bytes : 0;
    pointer = 6;
    break;
  }
  case ID_glLightfv:
  case ID_glMaterialfv:
  case ID_glLightiv:
  case ID_glMaterialiv:
    bytes =
        (a[1].i == GL_SHININESS || a[1].i == GL_SPOT_CUTOFF ||
         a[1].i == GL_SPOT_EXPONENT || a[1].i == GL_CONSTANT_ATTENUATION ||
         a[1].i == GL_LINEAR_ATTENUATION || a[1].i == GL_QUADRATIC_ATTENUATION)
            ? 4
        : a[1].i == GL_SPOT_DIRECTION ? 12
                                      : 16;
    pointer = 2;
    break;
  case ID_glLightModelfv:
  case ID_glLightModeliv:
  case ID_glFogfv:
  case ID_glFogiv:
    bytes =
        (a[0].i == GL_FOG_COLOR || a[0].i == GL_LIGHT_MODEL_AMBIENT) ? 16 : 4;
    pointer = 1;
    break;
  case ID_glTexEnviv:
  case ID_glTexEnvfv:
    bytes = a[1].i == GL_TEXTURE_ENV_COLOR ? 16 : 4;
    pointer = 2;
    break;
  case ID_glTexParameteriv:
  case ID_glTexParameterfv:
    bytes = a[1].i == GL_TEXTURE_BORDER_COLOR ? 16 : 4;
    pointer = 2;
    break;
  case ID_glCallLists:
    pointer = 2;
    bytes =
        (unsigned)a[0].i *
        (a[1].i >= GL_2_BYTES && a[1].i <= GL_4_BYTES ? a[1].i - GL_2_BYTES + 2
         : a[1].i == GL_BYTE || a[1].i == GL_UNSIGNED_BYTE   ? 1
         : a[1].i == GL_SHORT || a[1].i == GL_UNSIGNED_SHORT ? 2
                                                             : 4);
    break;
  case ID_glPrioritizeTextures:
    pointer = 1;
    bytes = (unsigned)a[0].i * 4;
    second = bytes;
    break;
  case ID_glEdgeFlagv:
    bytes = 1;
    break;
  case ID_glTexGendv:
  case ID_glTexGenfv:
  case ID_glTexGeniv:
    pointer = 2;
    bytes =
        (a[1].i == GL_TEXTURE_GEN_MODE ? 1 : 4) * (id == ID_glTexGendv ? 8 : 4);
    break;
  case ID_glClipPlane:
    bytes = 32;
    pointer = 1;
    break;
  case ID_glDrawPixels:
  case ID_glTexImage2D:
  case ID_glTexImage1D:
  case ID_glTexSubImage2D:
  case ID_glTexSubImage1D: {
    int w, h, f, type;
    if (id == ID_glDrawPixels) {
      w = a[0].i;
      h = a[1].i;
      f = a[2].i;
      type = a[3].i;
      pointer = 4;
    } else if (id == ID_glTexImage2D) {
      w = a[3].i;
      h = a[4].i;
      f = a[6].i;
      type = a[7].i;
      pointer = 8;
    } else if (id == ID_glTexImage1D) {
      w = a[3].i;
      h = 1;
      f = a[5].i;
      type = a[6].i;
      pointer = 7;
    } else if (id == ID_glTexSubImage2D) {
      w = a[4].i;
      h = a[5].i;
      f = a[6].i;
      type = a[7].i;
      pointer = 8;
    } else {
      w = a[3].i;
      h = 1;
      f = a[4].i;
      type = a[5].i;
      pointer = 6;
    }
    int b = gl_pixel_bytes(f, type);
    if (w < 0 || h < 0 || w > 2048 || h > 2048 || b <= 0) {
      gl_error(c, GL_INVALID_VALUE);
      return 1;
    }
    uint64_t n = (uint64_t)gl_pixel_stride(c->unpack[1] ? c->unpack[1] : w, f,
                                           type, c->unpack[0]) *
                     (h ? h - 1 + c->unpack[2] : 0) +
                 (uint64_t)(c->unpack[3] + w) * b;
    if (n > 16 * 1024 * 1024) {
      gl_error(c, GL_OUT_OF_MEMORY);
      return 1;
    }
    bytes = w && h && a[pointer].p ? (unsigned)n : 0;
    break;
  }
  case ID_glCompressedTexImage2DARB:
    pointer = 7;
    bytes = (unsigned)a[6].i;
    break;
  case ID_glCompressedTexSubImage2DARB:
    pointer = 8;
    bytes = (unsigned)a[7].i;
    break;
  case ID_glLoadMatrixf:
  case ID_glMultMatrixf:
    bytes = 64;
    break;
  case ID_glLoadMatrixd:
  case ID_glMultMatrixd:
    bytes = 128;
    break;
  default:
    break;
  }
  if (bytes > 16 * 1024 * 1024) {
    gl_error(c, GL_OUT_OF_MEMORY);
    c->compile_failed = 1;
    return 1;
  }
  struct gl_command *p = gl_alloc(sizeof(*p) + bytes + second);
  if (!p) {
    gl_error(c, GL_OUT_OF_MEMORY);
    c->compile_failed = 1;
    return 1;
  }
  p->id = id;
  p->bytes = bytes;
  w32_copy(p->unpack, c->unpack, sizeof(p->unpack));
  w32_copy(p->arg, a, sizeof(p->arg));
  if (bytes) {
    if (!a[pointer].p) {
      gl_dealloc(p);
      gl_error(c, GL_INVALID_VALUE);
      return 1;
    }
    w32_copy(p->data, a[pointer].p, bytes);
    p->arg[pointer].p = p->data;
    if (second) {
      unsigned second_pointer = rectangle ? 1 : 2;
      if (!a[second_pointer].p) {
        gl_dealloc(p);
        gl_error(c, GL_INVALID_VALUE);
        return 1;
      }
      w32_copy(p->data + bytes, a[second_pointer].p, second);
      p->arg[second_pointer].p = p->data + bytes;
    }
  }
  struct gl_list *l = &c->pending_list;
  if (l->tail)
    l->tail->next = p;
  else
    l->head = p;
  l->tail = p;
  return c->compile_mode == GL_COMPILE;
}
void gl_execute_list(struct gl_context *c, unsigned id) {
  if (id >= GL_LISTS)
    return;
  if (c->call_depth == 64) {
    gl_error(c, GL_STACK_OVERFLOW);
    return;
  }
  c->call_depth++;
  c->replaying++;
  for (struct gl_command *p = c->share->lists[id].head; p; p = p->next) {
    int unpack[6];
    w32_copy(unpack, c->unpack, sizeof(unpack));
    w32_copy(c->unpack, p->unpack, sizeof(unpack));
    w32_gl_dispatch(p->id, p->arg);
    w32_copy(c->unpack, unpack, sizeof(unpack));
  }
  c->replaying--;
  c->call_depth--;
}
static int size(int t) {
  switch (t) {
  case GL_BYTE:
  case GL_UNSIGNED_BYTE:
    return 1;
  case GL_SHORT:
  case GL_UNSIGNED_SHORT:
    return 2;
  case GL_INT:
  case GL_UNSIGNED_INT:
  case GL_FLOAT:
    return 4;
  case GL_DOUBLE:
    return 8;
  default:
    return 0;
  }
}
static double read_component(const void *p, int t, int i, int normalize) {
  double v = 0;
  switch (t) {
  case GL_BYTE:
    v = ((const GLbyte *)p)[i];
    if (normalize)
      v = v < -127 ? -1 : v / 127;
    break;
  case GL_UNSIGNED_BYTE:
    v = ((const GLubyte *)p)[i];
    if (normalize)
      v /= 255;
    break;
  case GL_SHORT:
    v = ((const GLshort *)p)[i];
    if (normalize)
      v = v < -32767 ? -1 : v / 32767;
    break;
  case GL_UNSIGNED_SHORT:
    v = ((const GLushort *)p)[i];
    if (normalize)
      v /= 65535;
    break;
  case GL_INT:
    v = ((const GLint *)p)[i];
    if (normalize)
      v = v < -2147483647 ? -1 : v / 2147483647;
    break;
  case GL_UNSIGNED_INT:
    v = ((const GLuint *)p)[i];
    if (normalize)
      v /= 4294967295.0;
    break;
  case GL_FLOAT:
    v = ((const GLfloat *)p)[i];
    break;
  case GL_DOUBLE:
    v = ((const GLdouble *)p)[i];
    break;
  }
  return v;
}
static void element(struct gl_context *c, int index) {
  SW_COUNT(SW_VERTEX, 1);
  if (index < 0) {
    gl_error(c, GL_INVALID_VALUE);
    return;
  }
  for (int k = 6; k >= 0; k--) {
    struct gl_array *v = &c->array[k];
    if (!v->enabled || !v->p)
      continue;
    unsigned stride = v->stride ? v->stride : v->size * size(v->type);
    if ((uint64_t)(unsigned)index * stride > 0x7fffffff) {
      gl_error(c, GL_INVALID_VALUE);
      return;
    }
    const uint8_t *p = v->p + (size_t)index * stride;
    float a[4] = {0, 0, 0, 1};
    for (int i = 0; i < v->size; i++)
      a[i] = (float)read_component(p, v->type, i, k == 1 || k == 2);
    /* DrawElements already owns the context lock and has validated the array
     * descriptors. Keep list compilation on the recording dispatcher. */
    if (c->tiny.in_begin && !c->compiling) {
      switch (k) {
      case 0: glVertex4f(a[0], a[1], a[2], a[3]); break;
      case 1: glColor4f(a[0], a[1], a[2], a[3]); break;
      case 2: glNormal3f(a[0], a[1], a[2]); break;
      case 3:
      case 4:
        for (int j = 0; j < 4; j++) {
          c->texcoord[k-3][j] = a[j];
          if (k == 3) c->tiny.current_tex_coord.v[j] = a[j];
          else c->tiny.current_tex_coord1.v[j] = a[j];
        }
        break;
      case 5: glEdgeFlag(*p); break;
      default: break;
      }
      continue;
    }
    switch (k) {
    case 0:
      w32_glVertex4f(a[0], a[1], a[2], a[3]);
      break;
    case 1:
      w32_glColor4f(a[0], a[1], a[2], a[3]);
      break;
    case 2:
      w32_glNormal3f(a[0], a[1], a[2]);
      break;
    case 3:
    case 4: {
      union gl_arg args[12] = {{.i = GL_TEXTURE0_ARB + k - 3},
                               {.f = a[0]},
                               {.f = a[1]},
                               {.f = a[2]},
                               {.f = a[3]}};
      w32_gl_dispatch(ID_glMultiTexCoord4fARB, args);
      break;
    }
    case 5:
      glEdgeFlag(*p);
      break;
    default:
      break;
    }
  }
}
static int array_index(struct gl_context *c, int name) {
  switch (name) {
  case GL_VERTEX_ARRAY:
    return 0;
  case GL_COLOR_ARRAY:
    return 1;
  case GL_NORMAL_ARRAY:
    return 2;
  case GL_TEXTURE_COORD_ARRAY:
    return 3 + c->client;
  case GL_EDGE_FLAG_ARRAY:
    return 5;
  case GL_INDEX_ARRAY:
    return 6;
  default:
    return -1;
  }
}
static int family(struct gl_context *c, unsigned id, const union gl_arg *a) {
  (void)c;
  (void)a;
  switch (id) {
#include "families.inc"
  default:
    return 0;
  }
  return 1;
}
static int enabled(struct gl_context *c, int n) {
  switch (n) {
  case GL_DEPTH_TEST:
    return c->sw.depth_test;
  case GL_STENCIL_TEST:
    return c->sw.stencil_test;
  case GL_ALPHA_TEST:
    return c->sw.alpha_test;
  case GL_BLEND:
    return c->sw.blend;
  case GL_SCISSOR_TEST:
    return c->sw.scissor;
  case GL_FOG:
    return c->sw.fog;
  case GL_TEXTURE_1D:
  case GL_TEXTURE_2D:
    return c->texture_enabled[c->active][n == GL_TEXTURE_2D];
  case GL_LIGHTING:
    return c->tiny.lighting_enabled;
  case GL_CULL_FACE:
    return c->tiny.cull_face_enabled;
  case GL_NORMALIZE:
    return c->tiny.normalize_enabled;
  default:
    if (n >= GL_CLIP_PLANE0 && n < GL_CLIP_PLANE0 + 6)
      return c->clip_enabled[n - GL_CLIP_PLANE0];
    if (n >= GL_TEXTURE_GEN_S && n <= GL_TEXTURE_GEN_Q)
      return c->texgen[c->active][n - GL_TEXTURE_GEN_S].enabled;
    if (n >= GL_LIGHT0 && n < GL_LIGHT0 + MAX_LIGHTS)
      return c->tiny.lights[n - GL_LIGHT0].enabled;
    return -1;
  }
}
static int get(struct gl_context *c, int name, double *v) {
  switch (name) {
  case GL_VIEWPORT:
    for (int k = 0; k < 4; k++)
      v[k] = c->viewport[k];
    return 4;
  case GL_SCISSOR_BOX:
    for (int k = 0; k < 4; k++)
      v[k] = c->scissor[k];
    return 4;
  case GL_CURRENT_COLOR:
    for (int k = 0; k < 4; k++)
      v[k] = c->tiny.current_color.v[k];
    return 4;
  case GL_COLOR_CLEAR_VALUE:
    for (int k = 0; k < 4; k++)
      v[k] = c->clear[k];
    return 4;
  case GL_COLOR_WRITEMASK:
    v[0] = (c->sw.color_mask & 0xff0000) != 0;
    v[1] = (c->sw.color_mask & 0xff00) != 0;
    v[2] = (c->sw.color_mask & 0xff) != 0;
    v[3] = (c->sw.color_mask & 0xff000000) != 0;
    return 4;
  case GL_MAX_VIEWPORT_DIMS:
    v[0] = v[1] = 2048;
    return 2;
  case GL_TEXTURE_COMPRESSION_HINT_ARB:
    *v = c->compression_hint;
    break;
  case GL_CURRENT_NORMAL:
    for (int k = 0; k < 3; k++)
      v[k] = c->tiny.current_normal.v[k];
    return 3;
  case GL_CURRENT_TEXTURE_COORDS:
    for (int k = 0; k < 4; k++)
      v[k] = c->texcoord[c->active][k];
    return 4;
  case GL_MODELVIEW_MATRIX:
  case GL_PROJECTION_MATRIX:
  case GL_TEXTURE_MATRIX: {
    int m = name == GL_MODELVIEW_MATRIX    ? 0
            : name == GL_PROJECTION_MATRIX ? 1
                                           : 2;
    for (int k = 0; k < 16; k++)
      v[k] = c->tiny.matrix_stack_ptr[m]->m[k % 4][k / 4];
    return 16;
  }
  case 0x81a8:
    *v = c->locked_first;
    break;
  case 0x81a9:
    *v = c->locked_count;
    break;
  case GL_MAX_ATTRIB_STACK_DEPTH:
  case GL_MAX_CLIENT_ATTRIB_STACK_DEPTH:
    *v = 16;
    break;
  case GL_ATTRIB_STACK_DEPTH:
    *v = c->attribute_depth;
    break;
  case GL_CLIENT_ATTRIB_STACK_DEPTH:
    *v = c->client_depth;
    break;
  case GL_LINE_WIDTH:
    *v = c->line_width;
    break;
  case GL_LINE_WIDTH_RANGE:
    v[0] = 1; v[1] = 64;
    return 2;
  case GL_LINE_WIDTH_GRANULARITY:
    *v = 1;
    break;
  case GL_POINT_SIZE:
    *v = c->zb->pointsize;
    break;
  case GL_MAX_TEXTURE_SIZE:
    *v = 2048;
    break;
  case GL_MAX_TEXTURE_UNITS_ARB:
    *v = 2;
    break;
  case GL_ACTIVE_TEXTURE_ARB:
    *v = GL_TEXTURE0_ARB + c->active;
    break;
  case GL_CLIENT_ACTIVE_TEXTURE_ARB:
    *v = GL_TEXTURE0_ARB + c->client;
    break;
  case GL_MAX_CLIP_PLANES:
    *v = 6;
    break;
  case GL_TEXTURE_BINDING_1D:
  case GL_TEXTURE_BINDING_2D:
    *v = c->binding[c->active][name == GL_TEXTURE_BINDING_2D]->name;
    break;
  case GL_DEPTH_BITS:
    *v = c->depth_bits;
    break;
  case GL_RED_BITS:
  case GL_BLUE_BITS:
    *v = c->sw.rgb565 ? 5 : 8;
    break;
  case GL_GREEN_BITS:
    *v = c->sw.rgb565 ? 6 : 8;
    break;
  case GL_ALPHA_BITS:
  case GL_ACCUM_RED_BITS:
  case GL_ACCUM_GREEN_BITS:
  case GL_ACCUM_BLUE_BITS:
  case GL_ACCUM_ALPHA_BITS:
  case GL_AUX_BUFFERS:
    *v = 0;
    break;
  case GL_STENCIL_BITS: *v = c->stencil_bits; break;
  case GL_STENCIL_FUNC: *v = c->sw.stencil_func; break;
  case GL_STENCIL_REF: *v = c->sw.stencil_ref; break;
  case GL_STENCIL_VALUE_MASK: *v = c->sw.stencil_mask; break;
  case GL_STENCIL_WRITEMASK: *v = c->sw.stencil_write_mask; break;
  case GL_STENCIL_FAIL: *v = c->sw.stencil_fail; break;
  case GL_STENCIL_PASS_DEPTH_FAIL: *v = c->sw.stencil_zfail; break;
  case GL_STENCIL_PASS_DEPTH_PASS: *v = c->sw.stencil_zpass; break;
  case GL_STENCIL_CLEAR_VALUE: *v = c->clear_stencil; break;
  case GL_DRAW_BUFFER: *v = c->draw_buffer; break;
  case GL_READ_BUFFER: *v = c->read_buffer; break;
  case GL_DOUBLEBUFFER:
    *v = c->doublebuffer;
    break;
  case GL_RGBA_MODE:
    *v = 1;
    break;
  case GL_STEREO:
    *v = 0;
    break;
  case GL_DEPTH_FUNC:
    *v = c->sw.depth_func;
    break;
  case GL_DEPTH_WRITEMASK:
    *v = c->sw.depth_write;
    break;
  case GL_DEPTH_CLEAR_VALUE:
    *v = c->clear_depth;
    break;
  case GL_DEPTH_RANGE:
    v[0] = c->range[0];
    v[1] = c->range[1];
    return 2;
  case GL_ALPHA_TEST_FUNC:
    *v = c->sw.alpha_func;
    break;
  case GL_ALPHA_TEST_REF:
    *v = c->alpha_ref;
    break;
  case GL_BLEND_SRC:
    *v = c->sw.sfactor;
    break;
  case GL_BLEND_DST:
    *v = c->sw.dfactor;
    break;
  case GL_MATRIX_MODE:
    *v = c->tiny.matrix_mode == 0   ? GL_MODELVIEW
         : c->tiny.matrix_mode == 1 ? GL_PROJECTION
                                    : GL_TEXTURE;
    break;
  case GL_MODELVIEW_STACK_DEPTH:
    *v = c->tiny.matrix_stack_ptr[0] - c->tiny.matrix_stack[0] + 1;
    break;
  case GL_PROJECTION_STACK_DEPTH:
    *v = c->tiny.matrix_stack_ptr[1] - c->tiny.matrix_stack[1] + 1;
    break;
  case GL_TEXTURE_STACK_DEPTH:
    *v = c->tiny.matrix_stack_ptr[2] - c->tiny.matrix_stack[2] + 1;
    break;
  case GL_MAX_MODELVIEW_STACK_DEPTH:
    *v = MAX_MODELVIEW_STACK_DEPTH;
    break;
  case GL_MAX_PROJECTION_STACK_DEPTH:
    *v = MAX_PROJECTION_STACK_DEPTH;
    break;
  case GL_MAX_TEXTURE_STACK_DEPTH:
    *v = MAX_TEXTURE_STACK_DEPTH;
    break;
  case GL_MAX_LIST_NESTING:
    *v = 64;
    break;
  case GL_MAX_LIGHTS:
    *v = MAX_LIGHTS;
    break;
  case GL_PACK_ALIGNMENT:
    *v = c->pack[0];
    break;
  case GL_UNPACK_ALIGNMENT:
    *v = c->unpack[0];
    break;
  case GL_PACK_ROW_LENGTH:
    *v = c->pack[1];
    break;
  case GL_UNPACK_ROW_LENGTH:
    *v = c->unpack[1];
    break;
  case GL_PACK_SKIP_ROWS:
    *v = c->pack[2];
    break;
  case GL_UNPACK_SKIP_ROWS:
    *v = c->unpack[2];
    break;
  case GL_PACK_SKIP_PIXELS:
    *v = c->pack[3];
    break;
  case GL_UNPACK_SKIP_PIXELS:
    *v = c->unpack[3];
    break;
  case GL_FOG_MODE:
    *v = c->fog_mode;
    break;
  case GL_FOG_DENSITY:
    *v = c->fog_density;
    break;
  case GL_FOG_START:
    *v = c->fog_start;
    break;
  case GL_FOG_END:
    *v = c->fog_end;
    break;
  case GL_NUM_COMPRESSED_TEXTURE_FORMATS_ARB:
    *v = 4;
    break;
  case GL_COMPRESSED_TEXTURE_FORMATS_ARB:
    for (int k = 0; k < 4; k++)
      v[k] = 0x83f0 + k;
    return 4;
  default: {
    int e = enabled(c, name);
    if (e < 0) {
      gl_error(c, GL_INVALID_ENUM);
      return 0;
    }
    *v = e;
    break;
  }
  }
  return 1;
}
static void selected_texture_done(struct gl_context **selected) {
  struct gl_context *c = *selected;
  c->texture_ptr[c->active] = c->tiny.matrix_stack_ptr[2];
  c->tiny.software_texture_matrix[c->active] = c->texture_ptr[c->active];
}
static void active_texture(struct gl_context *c, unsigned unit) {
  c->texture_ptr[c->active] = c->tiny.matrix_stack_ptr[2];
  c->active = unit;
  c->tiny.matrix_stack[2] = c->texture_base[unit];
  c->tiny.matrix_stack_ptr[2] = c->texture_ptr[unit];
}
union gl_arg w32_gl_dispatch(unsigned id, const union gl_arg *a) {
  GL_GUARD;
  union gl_arg result = {.i = 0};
  struct gl_context *c = gl_current();
  if (!c)
    return result;
  if (!c->tiny.in_begin && id != ID_glGetError &&
      !gl_refresh_target(c, c->dc)) {
    gl_error(c, GL_INVALID_OPERATION);
    return result;
  }
  gl_selected_context = &c->tiny;
  struct gl_context *selected __attribute__((cleanup(selected_texture_done))) =
      c;
#define I(n) ((int)a[n].i)
#define F(n) ((float)a[n].f)
#define P(n) (a[n].p)
  if ((c->compiling && !c->replaying) ? c->compile_begin : c->tiny.in_begin) {
    int legal = 0;
    switch (id) {
#include "begin.inc"
    default:
      break;
    }
    if (!legal) {
      gl_error(c, GL_INVALID_OPERATION);
      return result;
    }
  }
  /* Queries and client state are never compiled into display lists. */
  switch (id) {
  case ID_glGetError:
    result.i = c->error ? c->error : c->tiny.error_flag;
    c->error = 0;
    c->tiny.error_flag = 0;
    return result;
  case ID_glGetString:
    switch (I(0)) {
    case GL_VENDOR:
      result.p = "CiukiOS; TinyGL by Fabrice Bellard, Gek, C-Chads";
      break;
    case GL_RENDERER:
      result.p = "CiukiOS scalar software OpenGL";
      break;
    case GL_VERSION:
      result.p = "1.1 CiukiOS software subset";
      break;
    case GL_EXTENSIONS:
      result.p = "GL_ARB_multitexture GL_EXT_texture_env_add "
                 "GL_ARB_texture_env_combine GL_EXT_compiled_vertex_array "
                 "GL_EXT_bgra GL_EXT_texture_edge_clamp "
                 "GL_ARB_texture_compression GL_EXT_texture_compression_s3tc";
      break;
    default:
      gl_error(c, GL_INVALID_ENUM);
    }
    return result;
  case ID_glGetBooleanv:
  case ID_glGetIntegerv:
  case ID_glGetFloatv:
  case ID_glGetDoublev: {
    double v[16] = {0};
    int n = get(c, I(0), v);
    if (!P(1)) {
      gl_error(c, GL_INVALID_VALUE);
      return result;
    }
    for (int k = 0; k < n; k++)
      if (id == ID_glGetBooleanv)
        ((GLboolean *)P(1))[k] = v[k] != 0;
      else if (id == ID_glGetIntegerv)
        ((GLint *)P(1))[k] = (GLint)v[k];
      else if (id == ID_glGetFloatv)
        ((GLfloat *)P(1))[k] = (GLfloat)v[k];
      else
        ((GLdouble *)P(1))[k] = v[k];
    return result;
  }
  case ID_glIsEnabled: {
    int e = enabled(c, I(0));
    if (e < 0)
      gl_error(c, GL_INVALID_ENUM);
    else
      result.i = e;
    return result;
  }
  case ID_glGenTextures:
  case ID_glDeleteTextures:
  case ID_glIsTexture:
  case ID_glAreTexturesResident:
  case ID_glGetTexImage:
  case ID_glGetTexLevelParameteriv:
  case ID_glGetTexLevelParameterfv:
  case ID_glGetTexParameteriv:
  case ID_glGetTexParameterfv:
  case ID_glGetTexEnviv:
  case ID_glGetTexEnvfv:
  case ID_glGetCompressedTexImageARB:
    gl_textures(c, id, a, &result);
    return result;
  case ID_glFlush:
  case ID_glFinish:
    if ((c->draw_buffer == GL_FRONT || c->draw_buffer == GL_FRONT_LEFT || c->draw_buffer == GL_FRONT_AND_BACK) && !gl_present(c, 1))
      gl_error(c, GL_INVALID_OPERATION);
    return result;
  case ID_glRenderMode:
    gl_state(c, id, a, &result);
    return result;
  case ID_glPushClientAttrib:
  case ID_glPopClientAttrib:
    gl_state(c, id, a, &result);
    return result;
  case ID_glGetLightfv:
  case ID_glGetLightiv:
  case ID_glGetMaterialfv:
  case ID_glGetMaterialiv:
    gl_state(c, id, a, &result);
    return result;
  case ID_glReadPixels:
    gl_pixels(c, id, a, &result);
    return result;
  case ID_glGetClipPlane:
  case ID_glGetTexGendv:
  case ID_glGetTexGenfv:
  case ID_glGetTexGeniv:
    gl_geometry(c, id, a, &result);
    return result;
  case ID_glIsList:
    result.i = I(0) > 0 && I(0) < GL_LISTS && c->share->lists[I(0)].reserved;
    return result;
  case ID_glGenLists: {
    int n = I(0);
    if (n < 0) {
      gl_error(c, GL_INVALID_VALUE);
      return result;
    }
    if (n >= GL_LISTS)
      return result;
    for (int i = 1; n && i + n <= GL_LISTS; i++) {
      int k;
      for (k = 0; k < n && !c->share->lists[i + k].reserved; k++) {
      }
      if (k == n) {
        for (k = 0; k < n; k++)
          c->share->lists[i + k].reserved = 1;
        result.i = i;
        break;
      }
    }
    return result;
  }
  case ID_glDeleteLists:
    if (I(1) < 0) {
      gl_error(c, GL_INVALID_VALUE);
      return result;
    }
    for (int k = 0; k < I(1) && a[0].i + k < GL_LISTS; k++)
      if (a[0].i + k > 0)
        gl_free_list(&c->share->lists[a[0].i + k]);
    return result;
  case ID_glNewList:
    if (c->compiling || c->tiny.in_begin) {
      gl_error(c, GL_INVALID_OPERATION);
      return result;
    }
    if (I(0) <= 0 || I(0) >= GL_LISTS) {
      gl_error(c, GL_INVALID_VALUE);
      return result;
    }
    if (I(1) != GL_COMPILE && I(1) != GL_COMPILE_AND_EXECUTE) {
      gl_error(c, GL_INVALID_ENUM);
      return result;
    }
    c->pending_list = (struct gl_list){.reserved = 1};
    c->compile_failed = c->compile_begin = 0;
    c->compiling = I(0);
    c->compile_mode = I(1);
    return result;
  case ID_glEndList:
    if (!c->compiling) {
      gl_error(c, GL_INVALID_OPERATION);
      return result;
    }
    if (c->compile_failed)
      gl_free_list(&c->pending_list);
    else {
      gl_free_list(&c->share->lists[c->compiling]);
      c->share->lists[c->compiling] = c->pending_list;
      c->pending_list = (struct gl_list){0};
    }
    c->compiling = 0;
    return result;
  case ID_glPixelStorei:
  case ID_glPixelStoref: {
    int v = id == ID_glPixelStoref ? (int)F(1) : I(1), index = -1, *state = 0;
    switch (I(0)) {
    case GL_PACK_ALIGNMENT:
      state = c->pack;
      index = 0;
      break;
    case GL_UNPACK_ALIGNMENT:
      state = c->unpack;
      index = 0;
      break;
    case GL_PACK_ROW_LENGTH:
      state = c->pack;
      index = 1;
      break;
    case GL_UNPACK_ROW_LENGTH:
      state = c->unpack;
      index = 1;
      break;
    case GL_PACK_SKIP_ROWS:
      state = c->pack;
      index = 2;
      break;
    case GL_UNPACK_SKIP_ROWS:
      state = c->unpack;
      index = 2;
      break;
    case GL_PACK_SKIP_PIXELS:
      state = c->pack;
      index = 3;
      break;
    case GL_UNPACK_SKIP_PIXELS:
      state = c->unpack;
      index = 3;
      break;
    default:
      gl_error(c, GL_INVALID_ENUM);
      return result;
    }
    if (v < 0 || v > 65536 ||
        (index == 0 && v != 1 && v != 2 && v != 4 && v != 8)) {
      gl_error(c, GL_INVALID_VALUE);
      return result;
    }
    state[index] = v;
    return result;
  }
  case ID_glClientActiveTextureARB:
    if (I(0) < GL_TEXTURE0_ARB || I(0) > GL_TEXTURE0_ARB + 1)
      gl_error(c, GL_INVALID_ENUM);
    else
      c->client = I(0) - GL_TEXTURE0_ARB;
    return result;
  case ID_glEnableClientState:
  case ID_glDisableClientState: {
    int k = array_index(c, I(0));
    if (k < 0)
      gl_error(c, GL_INVALID_ENUM);
    else
      c->array[k].enabled = id == ID_glEnableClientState;
    return result;
  }
  case ID_glInterleavedArrays: {
    int format = I(0), stride = I(1), vsize = 3, tsize = 0, csize = 0,
        normal = 0, ctype = GL_FLOAT, toff = 0, coff = 0, noff = 0, voff = 0,
        total = 0;
    switch (format) {
    case GL_V2F:
      vsize = 2;
      total = 8;
      break;
    case GL_V3F:
      total = 12;
      break;
    case GL_C4UB_V2F:
      csize = 4;
      ctype = GL_UNSIGNED_BYTE;
      voff = 4;
      vsize = 2;
      total = 12;
      break;
    case GL_C4UB_V3F:
      csize = 4;
      ctype = GL_UNSIGNED_BYTE;
      voff = 4;
      total = 16;
      break;
    case GL_C3F_V3F:
      csize = 3;
      voff = 12;
      total = 24;
      break;
    case GL_N3F_V3F:
      normal = 1;
      voff = 12;
      total = 24;
      break;
    case GL_C4F_N3F_V3F:
      csize = 4;
      normal = 1;
      noff = 16;
      voff = 28;
      total = 40;
      break;
    case GL_T2F_V3F:
      tsize = 2;
      voff = 8;
      total = 20;
      break;
    case GL_T4F_V4F:
      tsize = 4;
      vsize = 4;
      voff = 16;
      total = 32;
      break;
    case GL_T2F_C4UB_V3F:
      tsize = 2;
      csize = 4;
      ctype = GL_UNSIGNED_BYTE;
      coff = 8;
      voff = 12;
      total = 24;
      break;
    case GL_T2F_C3F_V3F:
      tsize = 2;
      csize = 3;
      coff = 8;
      voff = 20;
      total = 32;
      break;
    case GL_T2F_N3F_V3F:
      tsize = 2;
      normal = 1;
      noff = 8;
      voff = 20;
      total = 32;
      break;
    case GL_T2F_C4F_N3F_V3F:
      tsize = 2;
      csize = 4;
      normal = 1;
      coff = 8;
      noff = 24;
      voff = 36;
      total = 48;
      break;
    case GL_T4F_C4F_N3F_V4F:
      tsize = 4;
      csize = 4;
      normal = 1;
      vsize = 4;
      coff = 16;
      noff = 32;
      voff = 44;
      total = 60;
      break;
    default:
      gl_error(c, GL_INVALID_ENUM);
      return result;
    }
    if (stride < 0) {
      gl_error(c, GL_INVALID_VALUE);
      return result;
    }
    if (!stride)
      stride = total;
    uintptr_t p = (uintptr_t)P(2);
    c->array[0] = (struct gl_array){(const uint8_t *)(p + voff), vsize,
                                    GL_FLOAT, stride, 1};
    c->array[1] = (struct gl_array){(const uint8_t *)(p + coff), csize, ctype,
                                    stride, csize != 0};
    c->array[2] = (struct gl_array){(const uint8_t *)(p + noff), 3, GL_FLOAT,
                                    stride, normal};
    c->array[3 + c->client] = (struct gl_array){
        (const uint8_t *)(p + toff), tsize, GL_FLOAT, stride, tsize != 0};
    c->array[5].enabled = c->array[6].enabled = 0;
    return result;
  }
  case ID_glVertexPointer:
  case ID_glColorPointer:
  case ID_glNormalPointer:
  case ID_glTexCoordPointer:
  case ID_glIndexPointer:
  case ID_glEdgeFlagPointer: {
    int k = id == ID_glVertexPointer     ? 0
            : id == ID_glColorPointer    ? 1
            : id == ID_glNormalPointer   ? 2
            : id == ID_glTexCoordPointer ? 3 + c->client
            : id == ID_glIndexPointer    ? 6
                                         : 5;
    int n = k == 2 ? 3 : k >= 5 ? 1 : I(0);
    int type = k == 5 ? GL_UNSIGNED_BYTE : I(k == 2 || k == 6 ? 0 : 1),
        stride = I(k == 5             ? 0
                   : k == 2 || k == 6 ? 1
                                      : 2);
    const void *p = P(k == 5 ? 1 : k == 2 || k == 6 ? 2 : 3);
    if (!size(type) || n < 1 || n > 4 || stride < 0 || (k == 0 && n < 2)) {
      gl_error(c, GL_INVALID_VALUE);
      return result;
    }
    c->array[k] = (struct gl_array){p, n, type, stride, c->array[k].enabled};
    return result;
  }
  case ID_glGetPointerv: {
    int k = I(0) == GL_VERTEX_ARRAY_POINTER          ? 0
            : I(0) == GL_COLOR_ARRAY_POINTER         ? 1
            : I(0) == GL_NORMAL_ARRAY_POINTER        ? 2
            : I(0) == GL_TEXTURE_COORD_ARRAY_POINTER ? 3 + c->client
            : I(0) == GL_EDGE_FLAG_ARRAY_POINTER     ? 5
            : I(0) == GL_INDEX_ARRAY_POINTER         ? 6
                                                     : -1;
    if (k < 0)
      gl_error(c, GL_INVALID_ENUM);
    else if (P(1))
      *(const void **)P(1) = c->array[k].p;
    return result;
  }
  case ID_glArrayElement:
    element(c, I(0));
    return result;
  case ID_glDrawArrays:
  case ID_glDrawElements: {
    if (I(0) < GL_POINTS || I(0) > GL_POLYGON) {
      gl_error(c, GL_INVALID_ENUM);
      return result;
    }
    int count = I(2), first = I(1);
    if (id == ID_glDrawElements) {
      count = I(1);
      first = 0;
    }
    if (count < 0 || count > 1048576 || first < 0 ||
        (int64_t)first + count > 0x7fffffff) {
      gl_error(c, GL_INVALID_VALUE);
      return result;
    }
    if (id == ID_glDrawElements &&
        ((I(2) != GL_UNSIGNED_BYTE && I(2) != GL_UNSIGNED_SHORT &&
          I(2) != GL_UNSIGNED_INT) ||
         !P(3))) {
      gl_error(c, GL_INVALID_ENUM);
      return result;
    }
    w32_glBegin(I(0));
    for (int k = 0; k < count; k++) {
      unsigned index = id == ID_glDrawElements
                           ? (unsigned)read_component(P(3), I(2), k, 0)
                           : (unsigned)(first + k);
      element(c, index);
    }
    w32_glEnd();
    return result;
  }
  case ID_glLockArraysEXT:
    if (I(0) < 0 || I(1) <= 0 || a[0].i + a[1].i > 0x7fffffff)
      gl_error(c, GL_INVALID_VALUE);
    else if (c->locked_count)
      gl_error(c, GL_INVALID_OPERATION);
    else {
      c->locked_first = I(0);
      c->locked_count = I(1);
    }
    return result;
  case ID_glUnlockArraysEXT:
    if (!c->locked_count)
      gl_error(c, GL_INVALID_OPERATION);
    else
      c->locked_first = c->locked_count = 0;
    return result;
  default:
    break;
  }
  unsigned validation = gl_validate(c, id, a);
  if (validation) {
    gl_error(c, validation);
    return result;
  }
  if (c->compiling && !c->replaying) {
    if (id == ID_glEnd && !c->compile_begin) {
      gl_error(c, GL_INVALID_OPERATION);
      return result;
    }
    if (id == ID_glBegin)
      c->compile_begin = 1;
    if (id == ID_glEnd)
      c->compile_begin = 0;
  }
  if (gl_record(c, id, a))
    return result;
  if (gl_geometry(c, id, a, &result))
    return result;
  if (family(c, id, a))
    return result;
  switch (id) {
  case ID_glEdgeFlag:
    glEdgeFlag(!!I(0));
    break;
  case ID_glEdgeFlagv:
    glEdgeFlag(!!*(const GLboolean *)P(0));
    break;
  case ID_glBegin:
    if (I(0) < GL_POINTS || I(0) > GL_POLYGON)
      gl_error(c, GL_INVALID_ENUM);
    else if (c->tiny.in_begin)
      gl_error(c, GL_INVALID_OPERATION);
    else {
      c->primitive = I(0);
      glBegin(I(0) == GL_POLYGON     ? GL_TRIANGLE_FAN
              : I(0) == GL_LINE_LOOP ? GL_LINE_STRIP
                                     : I(0));
    }
    break;
  case ID_glEnd:
    if (!c->tiny.in_begin)
      gl_error(c, GL_INVALID_OPERATION);
    else {
      if (c->primitive == GL_LINE_LOOP && c->tiny.vertex_cnt >= 2)
        gl_draw_line(&c->tiny.vertex[0], &c->tiny.vertex[2]);
      glEnd();
    }
    break;
  case ID_glCallList:
    gl_execute_list(c, (unsigned)I(0));
    break;
  case ID_glListBase:
    c->list_base = I(0);
    break;
  case ID_glCallLists:
    if (I(0) < 0 || (!size(I(1)) && (I(1) < GL_2_BYTES || I(1) > GL_4_BYTES)) ||
        (I(0) && !P(2))) {
      gl_error(c, GL_INVALID_VALUE);
      break;
    }
    for (int i = 0; i < I(0); i++) {
      unsigned index = 0;
      if (I(1) >= GL_2_BYTES && I(1) <= GL_4_BYTES) {
        unsigned n = I(1) - GL_2_BYTES + 2;
        for (unsigned j = 0; j < n; j++)
          index = (index << 8) | ((const uint8_t *)P(2))[i * n + j];
      } else
        index = (unsigned)read_component(P(2), I(1), i, 0);
      gl_execute_list(c, c->list_base + index);
    }
    break;
  case ID_glActiveTextureARB:
    if (I(0) < GL_TEXTURE0_ARB || I(0) > GL_TEXTURE0_ARB + 1)
      gl_error(c, GL_INVALID_ENUM);
    else
      active_texture(c, I(0) - GL_TEXTURE0_ARB);
    break;
  case ID_glClearColor:
    for (int k = 0; k < 4; k++)
      c->clear[k] = compat_clamp(F(k), 0, 1);
    break;
  case ID_glClearDepth:
    c->clear_depth = compat_clamp(F(0), 0, 1);
    break;
  case ID_glClear: {
    if (I(0) & ~(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT |
                 GL_STENCIL_BUFFER_BIT | GL_ACCUM_BUFFER_BIT)) {
      gl_error(c, GL_INVALID_VALUE);
      break;
    }
    unsigned color = sw_color_format(&c->sw, rgba(c->clear));
    int z = (int)(c->clear_depth * (c->depth_bits == 24 ? 16777215 : 65535));
    for (unsigned y = 0; y < c->sw.height; y++)
      for (unsigned x = 0; x < c->sw.width; x++) {
        if (c->sw.scissor && ((int)x < c->sw.box[0] || (int)y < c->sw.box[1] ||
                              (int)x >= c->sw.box[0] + c->sw.box[2] ||
                              (int)y >= c->sw.box[1] + c->sw.box[3]))
          continue;
        unsigned p = (c->sw.height - 1 - y) * c->sw.width + x;
        if ((I(0) & GL_COLOR_BUFFER_BIT) && !c->sw.color_disabled) {
          c->sw.color[p] = (c->sw.color[p] & ~c->sw.color_mask) | (color & c->sw.color_mask);
          if (c->sw.color2) c->sw.color2[p] = (c->sw.color2[p] & ~c->sw.color_mask) | (color & c->sw.color_mask);
        }
        if ((I(0) & GL_DEPTH_BUFFER_BIT) && c->sw.depth_write)
          { if (c->sw.depth24) c->sw.depth24[p] = z;
            else c->sw.depth[p] = z; }
        if ((I(0) & GL_STENCIL_BUFFER_BIT) && c->sw.stencil)
          c->sw.stencil[p] = (c->sw.stencil[p] & ~c->sw.stencil_write_mask) |
                            (c->clear_stencil & c->sw.stencil_write_mask);
      }
    break;
  }
  case ID_glViewport:
    if (I(2) < 0 || I(3) < 0 || I(2) > 2048 || I(3) > 2048 || I(0) < -2048 ||
        I(0) > 2048 || I(1) < -2048 || I(1) > 2048)
      gl_error(c, GL_INVALID_VALUE);
    else {
      for (int k = 0; k < 4; k++)
        c->viewport[k] = I(k);
      if (I(2) && I(3))
        glViewport(I(0), I(1), I(2), I(3));
    }
    break;
  case ID_glScissor:
    if (I(2) < 0 || I(3) < 0 || I(2) > 65536 || I(3) > 65536)
      gl_error(c, GL_INVALID_VALUE);
    else {
      for (int k = 0; k < 4; k++)
        c->scissor[k] = clampi(I(k), -65536, 65536);
      gl_window_state(c);
    }
    break;
  case ID_glEnable:
  case ID_glDisable: {
    int e = id == ID_glEnable;
    switch (I(0)) {
    case GL_STENCIL_TEST:
      c->sw.stencil_test = e;
      break;
    case GL_DEPTH_TEST:
      c->sw.depth_test = e;
      break;
    case GL_ALPHA_TEST:
      c->sw.alpha_test = e;
      break;
    case GL_BLEND:
      c->sw.blend = e;
      break;
    case GL_SCISSOR_TEST:
      c->sw.scissor = e;
      break;
    case GL_FOG:
      c->sw.fog = e;
      break;
    case GL_TEXTURE_1D:
    case GL_TEXTURE_2D:
      c->texture_enabled[c->active][I(0) == GL_TEXTURE_2D] = e;
      gl_sync_textures(c);
      break;
    case GL_LIGHTING:
    case GL_NORMALIZE:
    case GL_CULL_FACE:
    case GL_COLOR_MATERIAL:
    case GL_POLYGON_OFFSET_FILL:
    case GL_POLYGON_OFFSET_LINE:
    case GL_POLYGON_OFFSET_POINT:
      if (e)
        glEnable(I(0));
      else
        glDisable(I(0));
      break;
    default:
      if (I(0) >= GL_CLIP_PLANE0 && I(0) < GL_CLIP_PLANE0 + 6)
        c->clip_enabled[I(0) - GL_CLIP_PLANE0] = e;
      else if (I(0) >= GL_TEXTURE_GEN_S && I(0) <= GL_TEXTURE_GEN_Q)
        c->texgen[c->active][I(0) - GL_TEXTURE_GEN_S].enabled = e;
      else if (I(0) >= GL_LIGHT0 && I(0) < GL_LIGHT0 + MAX_LIGHTS) {
        if (e)
          glEnable(I(0));
        else
          glDisable(I(0));
      } else
        gl_error(c, GL_INVALID_ENUM);
      break;
    }
    break;
  }
  case ID_glDepthFunc:
    if (I(0) < GL_NEVER || I(0) > GL_ALWAYS)
      gl_error(c, GL_INVALID_ENUM);
    else
      c->sw.depth_func = I(0);
    break;
  case ID_glDepthMask:
    c->sw.depth_write = !!I(0);
    break;
  case ID_glDepthRange:
    c->range[0] = compat_clamp(F(0), 0, 1);
    c->range[1] = compat_clamp(F(1), 0, 1);
    break;
  case ID_glAlphaFunc:
    if (I(0) < GL_NEVER || I(0) > GL_ALWAYS)
      gl_error(c, GL_INVALID_ENUM);
    else {
      c->sw.alpha_func = I(0);
      c->alpha_ref = compat_clamp(F(1), 0, 1);
      c->sw.alpha_ref = (int)(c->alpha_ref * 255 + 0.5f);
    }
    break;
  case ID_glBlendFunc:
    if ((I(0) != GL_ZERO && I(0) != GL_ONE &&
         (I(0) < GL_SRC_COLOR || I(0) > GL_SRC_ALPHA_SATURATE)) ||
        (I(1) != GL_ZERO && I(1) != GL_ONE &&
         (I(1) < GL_SRC_COLOR || I(1) > GL_ONE_MINUS_DST_COLOR)))
      gl_error(c, GL_INVALID_ENUM);
    else {
      c->sw.sfactor = I(0);
      c->sw.dfactor = I(1);
    }
    break;
  case ID_glColorMask:
    c->sw.color_mask = (I(0) ? 0xff0000u : 0) | (I(1) ? 0xff00u : 0) |
                       (I(2) ? 0xffu : 0) | (I(3) ? 0xff000000u : 0);
    break;
  case ID_glPolygonOffset:
    c->offset_factor = compat_clamp(F(0), -65535, 65535);
    c->offset_units = compat_clamp(F(1), -65535, 65535);
    break;
  case ID_glMatrixMode:
    if (I(0) != GL_MODELVIEW && I(0) != GL_PROJECTION && I(0) != GL_TEXTURE)
      gl_error(c, GL_INVALID_ENUM);
    else
      glMatrixMode(I(0));
    break;
  case ID_glLoadIdentity:
    glLoadIdentity();
    break;
  case ID_glLoadMatrixf:
  case ID_glMultMatrixf:
  case ID_glLoadMatrixd:
  case ID_glMultMatrixd:
    if (P(0)) {
      float m[16];
      for (int i = 0; i < 16; i++)
        m[i] = (id == ID_glLoadMatrixd || id == ID_glMultMatrixd)
                   ? (float)((const double *)P(0))[i]
                   : ((const float *)P(0))[i];
      if (id == ID_glLoadMatrixf || id == ID_glLoadMatrixd)
        glLoadMatrixf(m);
      else
        glMultMatrixf(m);
    } else
      gl_error(c, GL_INVALID_VALUE);
    break;
  case ID_glPushMatrix:
    glPushMatrix();
    break;
  case ID_glPopMatrix:
    glPopMatrix();
    break;
  case ID_glTranslatef:
  case ID_glTranslated:
    glTranslatef(F(0), F(1), F(2));
    break;
  case ID_glScalef:
  case ID_glScaled:
    glScalef(F(0), F(1), F(2));
    break;
  case ID_glRotatef:
  case ID_glRotated:
    glRotatef(F(0), F(1), F(2), F(3));
    break;
  case ID_glFrustum:
    if (F(0) == F(1) || F(2) == F(3) || F(4) <= 0 || F(5) <= 0 || F(4) == F(5))
      gl_error(c, GL_INVALID_VALUE);
    else
      glFrustum(F(0), F(1), F(2), F(3), F(4), F(5));
    break;
  case ID_glOrtho:
    if (F(0) == F(1) || F(2) == F(3) || F(4) == F(5))
      gl_error(c, GL_INVALID_VALUE);
    else {
      float m[16] = {0};
      m[0] = 2 / (F(1) - F(0));
      m[5] = 2 / (F(3) - F(2));
      m[10] = -2 / (F(5) - F(4));
      m[12] = -(F(1) + F(0)) / (F(1) - F(0));
      m[13] = -(F(3) + F(2)) / (F(3) - F(2));
      m[14] = -(F(5) + F(4)) / (F(5) - F(4));
      m[15] = 1;
      glMultMatrixf(m);
    }
    break;
  case ID_glLineWidth:
    c->line_width = F(0);
    break;
  case ID_glShadeModel:
    glShadeModel(I(0));
    break;
  case ID_glCullFace:
    glCullFace(I(0));
    break;
  case ID_glFrontFace:
    glFrontFace(I(0));
    break;
  case ID_glPolygonMode:
    glPolygonMode(I(0), I(1));
    break;
  case ID_glColorMaterial:
    glColorMaterial(I(0), I(1));
    break;
  case ID_glLightfv:
  case ID_glLightiv:
  case ID_glMaterialfv:
  case ID_glMaterialiv: {
    float v[4] = {0};
    int n = I(1) == GL_SHININESS || I(1) == GL_SPOT_EXPONENT ||
                    I(1) == GL_SPOT_CUTOFF || I(1) == GL_CONSTANT_ATTENUATION ||
                    I(1) == GL_LINEAR_ATTENUATION ||
                    I(1) == GL_QUADRATIC_ATTENUATION
                ? 1
            : I(1) == GL_SPOT_DIRECTION ? 3
                                        : 4;
    if (!P(2)) {
      gl_error(c, GL_INVALID_VALUE);
      break;
    }
    for (int k = 0; k < n; k++)
      v[k] = (id == ID_glLightiv || id == ID_glMaterialiv)
                 ? (float)((const int *)P(2))[k]
                 : ((const float *)P(2))[k];
    if (id == ID_glLightfv || id == ID_glLightiv) {
      if (I(0) < GL_LIGHT0 || I(0) >= GL_LIGHT0 + MAX_LIGHTS)
        gl_error(c, GL_INVALID_ENUM);
      else
        glLightfv(I(0), I(1), v);
    } else
      glMaterialfv(I(0), I(1), v);
    break;
  }
  case ID_glLightf:
  case ID_glLighti:
    glLightf(I(0), I(1), id == ID_glLightf ? F(2) : (float)I(2));
    break;
  case ID_glMaterialf:
  case ID_glMateriali:
    glMaterialf(I(0), I(1), id == ID_glMaterialf ? F(2) : (float)I(2));
    break;
  case ID_glLightModelf:
  case ID_glLightModeli:
    glLightModeli(I(0), id == ID_glLightModelf ? (int)F(1) : I(1));
    break;
  case ID_glLightModelfv:
  case ID_glLightModeliv: {
    if (!P(1)) {
      gl_error(c, GL_INVALID_VALUE);
      break;
    }
    float v[4];
    for (int k = 0; k < (I(0) == GL_LIGHT_MODEL_AMBIENT ? 4 : 1); k++)
      v[k] = id == ID_glLightModeliv ? (float)((const int *)P(1))[k]
                                     : ((const float *)P(1))[k];
    glLightModelfv(I(0), v);
    break;
  }
  case ID_glFogf:
  case ID_glFogi:
  case ID_glFogfv:
  case ID_glFogiv: {
    float v[4] = {0};
    if (id == ID_glFogf)
      v[0] = F(1);
    else if (id == ID_glFogi)
      v[0] = I(1);
    else {
      if (!P(1)) {
        gl_error(c, GL_INVALID_VALUE);
        break;
      }
      for (int k = 0; k < (I(0) == GL_FOG_COLOR ? 4 : 1); k++)
        v[k] = id == ID_glFogiv ? (float)((const int *)P(1))[k]
                                : ((const float *)P(1))[k];
    }
    switch (I(0)) {
    case GL_FOG_MODE:
      if ((int)v[0] != GL_LINEAR && (int)v[0] != GL_EXP && (int)v[0] != GL_EXP2)
        gl_error(c, GL_INVALID_ENUM);
      else
        c->fog_mode = (int)v[0];
      break;
    case GL_FOG_DENSITY:
      if (v[0] < 0)
        gl_error(c, GL_INVALID_VALUE);
      else
        c->fog_density = v[0];
      break;
    case GL_FOG_START:
      c->fog_start = v[0];
      break;
    case GL_FOG_END:
      c->fog_end = v[0];
      break;
    case GL_FOG_COLOR:
      c->sw.fog_color = (int)rgba(v);
      break;
    default:
      gl_error(c, GL_INVALID_ENUM);
    }
    break;
  }
  case ID_glHint:
    if (I(0) == GL_TEXTURE_COMPRESSION_HINT_ARB)
      c->compression_hint = I(1);
    break;
  case ID_glClearStencil: c->clear_stencil = (unsigned)I(0) & (c->stencil_bits ? 255 : 0); break;
  case ID_glStencilFunc:
    c->sw.stencil_func = I(0); c->sw.stencil_ref = clampi(I(1), 0, c->stencil_bits ? 255 : 0);
    c->sw.stencil_mask = (unsigned)I(2) & 255; break;
  case ID_glStencilMask: c->sw.stencil_write_mask = (unsigned)I(0) & 255; break;
  case ID_glStencilOp:
    c->sw.stencil_fail = I(0); c->sw.stencil_zfail = I(1);
    c->sw.stencil_zpass = I(2); break;
  case ID_glDrawBuffer:
  case ID_glReadBuffer: {
    int buffer = I(0);
    if ((buffer == GL_BACK || buffer == GL_BACK_LEFT) && !c->doublebuffer) gl_error(c, GL_INVALID_OPERATION);
    else if (id == ID_glReadBuffer) c->read_buffer = buffer;
    else { c->draw_buffer = buffer; gl_select_buffer(c); }
    break;
  }
  default:
    if (id == ID_glBitmap || id == ID_glDrawPixels || id == ID_glReadPixels ||
        id == ID_glCopyPixels || id == ID_glPixelZoom) {
      gl_pixels(c, id, a, &result);
      break;
    }
    if (!gl_state(c, id, a, &result))
      gl_textures(c, id, a, &result);
    break;
  }
  return result;
}
