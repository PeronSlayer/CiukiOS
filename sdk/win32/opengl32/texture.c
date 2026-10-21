/* Texture objects, mip storage and software extension upload.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
static struct sw_texture *texture(struct gl_context *c, unsigned name,
                                  int create) {
  for (unsigned i = 0; i < GL_TEXTURES; i++)
    if (c->share->textures[i].used && !c->share->textures[i].deleted &&
        c->share->textures[i].name == name)
      return &c->share->textures[i];
  if (create)
    for (unsigned i = 1; i < GL_TEXTURES; i++)
      if (!c->share->textures[i].used) {
        struct sw_texture *t = &c->share->textures[i];
        *t = (struct sw_texture){.name = name,
                                 .used = 1,
                                 .wrap_s = GL_REPEAT,
                                 .wrap_t = GL_REPEAT,
                                 .min = GL_NEAREST_MIPMAP_LINEAR,
                                 .mag = GL_LINEAR};
        return t;
      }
  return 0;
}
static void free_texture(struct sw_texture *t) {
  for (int l = 0; l < SW_LEVELS; l++) {
    gl_dealloc(t->image[l].pixels);
    gl_dealloc(t->image[l].compressed);
  }
  *t = (struct sw_texture){0};
}
void gl_texture_ref(struct sw_texture *t, int delta) {
  if (!t || !t->name)
    return;
  if (delta > 0)
    t->refs++;
  else if (t->refs)
    t->refs--;
  if (t->deleted && !t->refs)
    free_texture(t);
}
void gl_sync_textures(struct gl_context *c) {
  for (int u = 0; u < 2; u++) {
    int target = c->texture_enabled[u][1] ? 1 : 0;
    c->sw.unit[u].texture = c->binding[u][target];
    c->sw.unit[u].enabled =
        c->texture_enabled[u][0] || c->texture_enabled[u][1];
  }
}
void gl_texture_bindings(struct gl_context *c, int delta) {
  for (int u = 0; u < 2; u++)
    for (int t = 0; t < 2; t++)
      gl_texture_ref(c->binding[u][t], delta);
}
void gl_texture_cleanup(struct gl_context *c) {
  gl_texture_bindings(c, -1);
  for (int u = 0; u < 2; u++)
    for (int t = 0; t < 2; t++)
      free_texture(&c->defaults[u][t]);
}
static int dimension(int n) { return n > 0 && n <= 2048 && !(n & (n - 1)); }
static int target(int t) { return t == GL_TEXTURE_2D || t == GL_TEXTURE_1D; }
static void image(struct gl_context *c, struct sw_texture *t, int level,
                  int internal, int w, int h, int border, int f, int type,
                  const void *data, int compressed, unsigned n) {
  if (level < 0 || level >= SW_LEVELS || w < 0 || h < 0 || w > 2048 ||
      h > 2048 || border || (!dimension(w) && w) || (!dimension(h) && h)) {
    gl_error(c, GL_INVALID_VALUE);
    return;
  }
  int bytes = gl_pixel_bytes(f, type);
  if (!compressed && bytes <= 0) {
    gl_error(c, GL_INVALID_ENUM);
    return;
  }
  if (internal != 1 && internal != 2 && internal != 3 && internal != 4 &&
      internal != GL_RGB && internal != GL_RGBA && internal != GL_LUMINANCE &&
      internal != GL_LUMINANCE_ALPHA && internal != GL_ALPHA &&
      !(internal >= 0x803b && internal <= 0x805b) &&
      !(internal >= 0x83f0 && internal <= 0x83f3) &&
      !(internal >= 0x84e9 && internal <= 0x84ee)) {
    gl_error(c, GL_INVALID_VALUE);
    return;
  }
  struct sw_image next = {.w = w, .h = h, .internal = internal};
  if (w && h) {
    next.pixels = gl_alloc((unsigned)w * h * 4);
    if (!next.pixels) {
      gl_error(c, GL_OUT_OF_MEMORY);
      return;
    }
  }
  if (compressed) {
    if (!sw_s3tc(next.pixels, w, h, internal, data, n)) {
      gl_dealloc(next.pixels);
      gl_error(c, GL_INVALID_VALUE);
      return;
    }
    next.compressed = gl_alloc(n);
    if (!next.compressed) {
      gl_dealloc(next.pixels);
      gl_error(c, GL_OUT_OF_MEMORY);
      return;
    }
    w32_copy(next.compressed, data, n);
    next.bytes = n;
    next.format = internal;
  } else if (data) {
    unsigned pitch =
        gl_pixel_stride(c->unpack[1] ? c->unpack[1] : w, f, type, c->unpack[0]);
    const uint8_t *p =
        (const uint8_t *)data + c->unpack[2] * pitch + c->unpack[3] * bytes;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        unsigned pixel = gl_pixel_get(p + y * pitch + x * bytes, f, type);
        unsigned base = sw_base_format(internal), lum = (pixel >> 16) & 255;
        if (base == GL_RGB)
          pixel |= 0xff000000;
        else if (base == GL_ALPHA)
          pixel &= 0xff000000;
        else if (base == GL_LUMINANCE || base == GL_LUMINANCE_ALPHA ||
                 base == GL_INTENSITY)
          pixel =
              lum * 0x010101 | (base == GL_LUMINANCE   ? 0xff000000
                                : base == GL_INTENSITY ? lum << 24
                                                       : pixel & 0xff000000);
        next.pixels[y * w + x] = pixel;
      }
  }
  gl_dealloc(t->image[level].pixels);
  gl_dealloc(t->image[level].compressed);
  t->image[level] = next;
}
static void subimage(struct gl_context *c, struct sw_texture *t, int level,
                     int x, int y, int w, int h, int f, int type,
                     const void *data) {
  if (level < 0 || level >= SW_LEVELS || x < 0 || y < 0 || w < 0 || h < 0) {
    gl_error(c, GL_INVALID_VALUE);
    return;
  }
  struct sw_image *m = &t->image[level];
  int bytes = gl_pixel_bytes(f, type);
  if (bytes <= 0) {
    gl_error(c, GL_INVALID_ENUM);
    return;
  }
  if (!m->pixels || (unsigned)x > m->w || (unsigned)y > m->h ||
      (unsigned)w > m->w - x || (unsigned)h > m->h - y || !data) {
    gl_error(c, GL_INVALID_VALUE);
    return;
  }
  unsigned pitch =
      gl_pixel_stride(c->unpack[1] ? c->unpack[1] : w, f, type, c->unpack[0]);
  const uint8_t *p =
      (const uint8_t *)data + c->unpack[2] * pitch + c->unpack[3] * bytes;
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++)
      m->pixels[(y + j) * m->w + x + i] =
          gl_pixel_get(p + j * pitch + i * bytes, f, type);
  gl_dealloc(m->compressed);
  m->compressed = 0;
  m->bytes = m->format = 0;
}
void gl_textures(struct gl_context *c, unsigned id, const union gl_arg *a,
                 union gl_arg *r) {
#define I(n) ((int)a[n].i)
#define P(n) (a[n].p)
  struct sw_unit *u = &c->sw.unit[c->active];
  struct sw_texture *t = c->binding[c->active][I(0) == GL_TEXTURE_2D];
  switch (id) {
  case ID_glBindTexture:
    if (!target(I(0))) {
      gl_error(c, GL_INVALID_ENUM);
      break;
    }
    t = I(1) ? texture(c, (unsigned)I(1), 1)
             : &c->defaults[c->active][I(0) == GL_TEXTURE_2D];
    if (!t)
      gl_error(c, GL_OUT_OF_MEMORY);
    else if (t->target && t->target != (unsigned)I(0))
      gl_error(c, GL_INVALID_OPERATION);
    else {
      t->target = I(0);
      gl_texture_ref(t, 1);
      gl_texture_ref(c->binding[c->active][I(0) == GL_TEXTURE_2D], -1);
      c->binding[c->active][I(0) == GL_TEXTURE_2D] = t;
      gl_sync_textures(c);
    }
    break;
  case ID_glGenTextures: {
    if (I(0) < 0 || (I(0) && !P(1))) {
      gl_error(c, GL_INVALID_VALUE);
      break;
    }
    for (int i = 0; i < I(0); i++) {
      unsigned n = 1;
      while (texture(c, n, 0))
        n++;
      if (!texture(c, n, 1)) {
        gl_error(c, GL_OUT_OF_MEMORY);
        break;
      }
      ((unsigned *)P(1))[i] = n;
    }
    break;
  }
  case ID_glIsTexture:
    t = texture(c, (unsigned)I(0), 0);
    r->i = I(0) != 0 && t && t->target;
    break;
  case ID_glDeleteTextures:
    if (I(0) < 0 || (I(0) && !P(1))) {
      gl_error(c, GL_INVALID_VALUE);
      break;
    }
    for (int i = 0; i < I(0); i++) {
      unsigned n = ((const unsigned *)P(1))[i];
      if (!n)
        continue;
      t = texture(c, n, 0);
      if (!t)
        continue;
      /* Unbind only this context. Other contexts/attribute stacks retain
       * the deleted object, and may coexist with a reused numeric name. */
      t->deleted = 1;
      for (int j = 0; j < 2; j++)
        for (int k = 0; k < 2; k++)
          if (c->binding[j][k] == t) {
            c->binding[j][k] = &c->defaults[j][k];
            gl_texture_ref(t, -1);
          }
      if (t->used && !t->refs)
        free_texture(t);
      gl_sync_textures(c);
    }
    break;
  case ID_glAreTexturesResident:
    if (I(0) < 0 || (I(0) && !P(1))) {
      gl_error(c, GL_INVALID_VALUE);
      break;
    }
    r->i = 1;
    /* Every valid software object is resident. When all are resident the
     * specification leaves the caller's residence array untouched. */
    for (int i = 0; i < I(0); i++) {
      unsigned name = ((const unsigned *)P(1))[i];
      t = name ? texture(c, name, 0) : 0;
      if (!t || !t->target) {
        gl_error(c, GL_INVALID_VALUE);
        r->i = 0;
        break;
      }
    }
    break;
  case ID_glPrioritizeTextures:
    if (I(0) < 0)
      gl_error(c, GL_INVALID_VALUE);
    break;
  case ID_glTexImage2D:
  case ID_glTexImage1D: {
    int one = id == ID_glTexImage1D;
    if (I(0) != (one ? GL_TEXTURE_1D : GL_TEXTURE_2D)) {
      gl_error(c, GL_INVALID_ENUM);
      break;
    }
    image(c, t, I(1), I(2), I(3), one ? 1 : I(4), I(one ? 4 : 5),
          I(one ? 5 : 6), I(one ? 6 : 7), P(one ? 7 : 8), 0, 0);
    break;
  }
  case ID_glTexSubImage2D:
  case ID_glTexSubImage1D: {
    int one = id == ID_glTexSubImage1D;
    if (I(0) != (one ? GL_TEXTURE_1D : GL_TEXTURE_2D)) {
      gl_error(c, GL_INVALID_ENUM);
      break;
    }
    subimage(c, t, I(1), I(2), one ? 0 : I(3), I(one ? 3 : 4), one ? 1 : I(5),
             I(one ? 4 : 6), I(one ? 5 : 7), P(one ? 6 : 8));
    break;
  }
  case ID_glCompressedTexImage2DARB:
    if (I(0) != GL_TEXTURE_2D) {
      gl_error(c, GL_INVALID_ENUM);
      break;
    }
    image(c, t, I(1), I(2), I(3), I(4), I(5), 0, 0, P(7), 1, (unsigned)I(6));
    break;
  case ID_glCompressedTexSubImage2DARB: {
    int level = I(1), x = I(2), y = I(3), w = I(4), h = I(5);
    if (I(0) != GL_TEXTURE_2D || level < 0 || level >= SW_LEVELS || x < 0 ||
        y < 0 || w < 0 || h < 0 || w > 2048 || h > 2048) {
      gl_error(c, GL_INVALID_VALUE);
      break;
    }
    struct sw_image *m = &t->image[level];
    if ((x & 3) || (y & 3) || !m->pixels || I(6) != (int)m->format ||
        (unsigned)x > m->w || (unsigned)y > m->h || (unsigned)w > m->w - x ||
        (unsigned)h > m->h - y || ((w & 3) && (unsigned)(x + w) != m->w) ||
        ((h & 3) && (unsigned)(y + h) != m->h)) {
      gl_error(c, GL_INVALID_OPERATION);
      break;
    }
    uint32_t *decoded = gl_alloc((unsigned)w * h * 4);
    if (!decoded) {
      gl_error(c, GL_OUT_OF_MEMORY);
      break;
    }
    if (!sw_s3tc(decoded, w, h, I(6), P(8), I(7)))
      gl_error(c, GL_INVALID_VALUE);
    else {
      for (int j = 0; j < h; j++)
        w32_copy(m->pixels + (y + j) * m->w + x, decoded + j * w, w * 4);
      unsigned block = I(6) < 0x83f2 ? 8 : 16, srcpitch = ((w + 3) / 4) * block,
               dstpitch = ((m->w + 3) / 4) * block;
      for (int j = 0; j < (h + 3) / 4; j++)
        w32_copy((uint8_t *)m->compressed + (y / 4 + j) * dstpitch +
                     x / 4 * block,
                 (const uint8_t *)P(8) + j * srcpitch, srcpitch);
    }
    gl_dealloc(decoded);
    break;
  }
  case ID_glGetCompressedTexImageARB:
    if (!target(I(0)) || I(1) < 0 || I(1) >= SW_LEVELS || !P(2)) {
      gl_error(c, GL_INVALID_VALUE);
      break;
    }
    if (!t->image[I(1)].compressed)
      gl_error(c, GL_INVALID_OPERATION);
    else
      w32_copy((void *)P(2), t->image[I(1)].compressed, t->image[I(1)].bytes);
    break;
  case ID_glTexParameteri:
  case ID_glTexParameterf:
  case ID_glTexParameteriv:
  case ID_glTexParameterfv: {
    int v = id == ID_glTexParameteri   ? I(2)
            : id == ID_glTexParameterf ? (int)a[2].f
            : P(2) ? (id == ID_glTexParameteriv ? *(const int *)P(2)
                                                : (int)*(const float *)P(2))
                   : 0;
    if (!target(I(0))) {
      gl_error(c, GL_INVALID_ENUM);
      break;
    }
    switch (I(1)) {
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
      if (v != GL_REPEAT && v != GL_CLAMP && v != GL_CLAMP_TO_EDGE)
        gl_error(c, GL_INVALID_ENUM);
      else if (I(1) == GL_TEXTURE_WRAP_S)
        t->wrap_s = v;
      else
        t->wrap_t = v;
      break;
    case GL_TEXTURE_MIN_FILTER:
      if (v != GL_NEAREST && v != GL_LINEAR &&
          (v < GL_NEAREST_MIPMAP_NEAREST || v > GL_LINEAR_MIPMAP_LINEAR))
        gl_error(c, GL_INVALID_ENUM);
      else
        t->min = v;
      break;
    case GL_TEXTURE_MAG_FILTER:
      if (v != GL_NEAREST && v != GL_LINEAR)
        gl_error(c, GL_INVALID_ENUM);
      else
        t->mag = v;
      break;
    default:
      gl_error(c, GL_INVALID_ENUM);
    }
    break;
  }
  case ID_glTexEnvi:
  case ID_glTexEnvf:
  case ID_glTexEnviv:
  case ID_glTexEnvfv: {
    if (I(0) != GL_TEXTURE_ENV) {
      gl_error(c, GL_INVALID_ENUM);
      break;
    }
    int v = id == ID_glTexEnvi   ? I(2)
            : id == ID_glTexEnvf ? (int)a[2].f
            : P(2) ? (id == ID_glTexEnviv ? *(const int *)P(2)
                                          : (int)*(const float *)P(2))
                   : 0;
    if (I(1) == GL_TEXTURE_ENV_COLOR) {
      if (!P(2)) {
        gl_error(c, GL_INVALID_VALUE);
        break;
      }
      float color[4];
      for (int k = 0; k < 4; k++)
        color[k] = id == ID_glTexEnviv
                       ? (float)((const int *)P(2))[k] / 2147483647.0f
                       : ((const float *)P(2))[k];
      unsigned p = 0;
      for (int k = 0; k < 4; k++) {
        float q = color[k] < 0 ? 0 : color[k] > 1 ? 1 : color[k];
        p |= (unsigned)(q * 255) << (k == 3 ? 24 : (2 - k) * 8);
      }
      u->constant = p;
      break;
    }
    if (I(1) == GL_TEXTURE_ENV_MODE) {
      if (v != GL_MODULATE && v != GL_DECAL && v != GL_BLEND &&
          v != GL_REPLACE && v != GL_ADD && v != GL_COMBINE_ARB)
        gl_error(c, GL_INVALID_ENUM);
      else
        u->mode = v;
      break;
    }
    if (I(1) == GL_COMBINE_RGB_ARB || I(1) == GL_COMBINE_ALPHA_ARB) {
      if (v != GL_REPLACE && v != GL_MODULATE && v != GL_ADD &&
          v != GL_ADD_SIGNED_ARB && v != GL_INTERPOLATE_ARB &&
          v != GL_SUBTRACT_ARB)
        gl_error(c, GL_INVALID_ENUM);
      else
        u->combine[I(1) == GL_COMBINE_ALPHA_ARB] = v;
      break;
    }
    if (I(1) == GL_RGB_SCALE_ARB || I(1) == GL_ALPHA_SCALE) {
      if (v != 1 && v != 2 && v != 4)
        gl_error(c, GL_INVALID_VALUE);
      else
        u->scale[I(1) == GL_ALPHA_SCALE] = v;
      break;
    }
    if ((I(1) >= GL_SOURCE0_RGB_ARB && I(1) < GL_SOURCE0_RGB_ARB + 3) ||
        (I(1) >= GL_SOURCE0_ALPHA_ARB && I(1) < GL_SOURCE0_ALPHA_ARB + 3)) {
      int group = I(1) >= GL_SOURCE0_ALPHA_ARB,
          index = I(1) - (group ? GL_SOURCE0_ALPHA_ARB : GL_SOURCE0_RGB_ARB);
      if (v != GL_TEXTURE && v != GL_CONSTANT_ARB &&
          v != GL_PRIMARY_COLOR_ARB && v != GL_PREVIOUS_ARB)
        gl_error(c, GL_INVALID_ENUM);
      else
        u->source[group][index] = v;
      break;
    }
    if ((I(1) >= GL_OPERAND0_RGB_ARB && I(1) < GL_OPERAND0_RGB_ARB + 3) ||
        (I(1) >= GL_OPERAND0_ALPHA_ARB && I(1) < GL_OPERAND0_ALPHA_ARB + 3)) {
      int group = I(1) >= GL_OPERAND0_ALPHA_ARB,
          index = I(1) - (group ? GL_OPERAND0_ALPHA_ARB : GL_OPERAND0_RGB_ARB);
      if ((group && (v != GL_SRC_ALPHA && v != GL_ONE_MINUS_SRC_ALPHA)) ||
          (!group && v != GL_SRC_ALPHA && v != GL_ONE_MINUS_SRC_ALPHA &&
           v != GL_SRC_COLOR && v != GL_ONE_MINUS_SRC_COLOR))
        gl_error(c, GL_INVALID_ENUM);
      else
        u->operand[group][index] = v;
      break;
    }
    gl_error(c, GL_INVALID_ENUM);
    break;
  }
  case ID_glGetTexLevelParameteriv:
  case ID_glGetTexLevelParameterfv: {
    if (!target(I(0)) || I(1) < 0 || I(1) >= SW_LEVELS || !P(3)) {
      gl_error(c, GL_INVALID_VALUE);
      break;
    }
    struct sw_image *m = &t->image[I(1)];
    int v;
    switch (I(2)) {
    case GL_TEXTURE_WIDTH:
      v = m->w;
      break;
    case GL_TEXTURE_HEIGHT:
      v = m->h;
      break;
    case GL_TEXTURE_COMPONENTS:
      v = m->internal;
      break;
    case GL_TEXTURE_BORDER:
      v = 0;
      break;
    case GL_TEXTURE_COMPRESSED_ARB:
      v = m->compressed != 0;
      break;
    case GL_TEXTURE_COMPRESSED_IMAGE_SIZE_ARB:
      v = m->bytes;
      break;
    default:
      gl_error(c, GL_INVALID_ENUM);
      return;
    }
    if (id == ID_glGetTexLevelParameteriv)
      *(int *)P(3) = v;
    else
      *(float *)P(3) = (float)v;
    break;
  }
  case ID_glGetTexParameteriv:
  case ID_glGetTexParameterfv: {
    int v = I(1) == GL_TEXTURE_MIN_FILTER   ? t->min
            : I(1) == GL_TEXTURE_MAG_FILTER ? t->mag
            : I(1) == GL_TEXTURE_WRAP_S     ? t->wrap_s
            : I(1) == GL_TEXTURE_WRAP_T     ? t->wrap_t
                                            : 0;
    if (!v || !target(I(0)))
      gl_error(c, GL_INVALID_ENUM);
    else if (P(2)) {
      if (id == ID_glGetTexParameteriv)
        *(int *)P(2) = v;
      else
        *(float *)P(2) = (float)v;
    }
    break;
  }
  case ID_glGetTexEnviv:
  case ID_glGetTexEnvfv: {
    if (I(0) != GL_TEXTURE_ENV || !P(2)) {
      gl_error(c, GL_INVALID_ENUM);
      break;
    }
    float v[4] = {0};
    int n = 1, name = I(1);
    if (name == GL_TEXTURE_ENV_MODE)
      v[0] = u->mode;
    else if (name == GL_COMBINE_RGB_ARB || name == GL_COMBINE_ALPHA_ARB)
      v[0] = u->combine[name == GL_COMBINE_ALPHA_ARB];
    else if (name == GL_RGB_SCALE_ARB || name == GL_ALPHA_SCALE)
      v[0] = u->scale[name == GL_ALPHA_SCALE];
    else if (name == GL_TEXTURE_ENV_COLOR) {
      n = 4;
      for (int k = 0; k < 4; k++)
        v[k] = ((u->constant >> (k == 3 ? 24 : (2 - k) * 8)) & 255) / 255.0f;
    } else if ((name >= GL_SOURCE0_RGB_ARB && name < GL_SOURCE0_RGB_ARB + 3) ||
               (name >= GL_SOURCE0_ALPHA_ARB &&
                name < GL_SOURCE0_ALPHA_ARB + 3)) {
      int group = name >= GL_SOURCE0_ALPHA_ARB;
      v[0] = u->source[group][name - (group ? GL_SOURCE0_ALPHA_ARB
                                            : GL_SOURCE0_RGB_ARB)];
    } else if ((name >= GL_OPERAND0_RGB_ARB &&
                name < GL_OPERAND0_RGB_ARB + 3) ||
               (name >= GL_OPERAND0_ALPHA_ARB &&
                name < GL_OPERAND0_ALPHA_ARB + 3)) {
      int group = name >= GL_OPERAND0_ALPHA_ARB;
      v[0] = u->operand[group][name - (group ? GL_OPERAND0_ALPHA_ARB
                                             : GL_OPERAND0_RGB_ARB)];
    } else {
      gl_error(c, GL_INVALID_ENUM);
      break;
    }
    for (int k = 0; k < n; k++)
      if (id == ID_glGetTexEnviv)
        ((int *)P(2))[k] = name == GL_TEXTURE_ENV_COLOR
                               ? (int)((double)v[k] * 2147483647.0)
                               : (int)v[k];
      else
        ((float *)P(2))[k] = v[k];
    break;
  }

  case ID_glGetTexImage: {
    if (!target(I(0)) || I(1) < 0 || I(1) >= SW_LEVELS || !P(4)) {
      gl_error(c, GL_INVALID_VALUE);
      break;
    }
    struct sw_image *m = &t->image[I(1)];
    int bytes = gl_pixel_bytes(I(2), I(3));
    if (bytes <= 0) {
      gl_error(c, GL_INVALID_ENUM);
      break;
    }
    unsigned pitch =
        gl_pixel_stride(c->pack[1] ? c->pack[1] : m->w, I(2), I(3), c->pack[0]);
    uint8_t *p = (uint8_t *)P(4) + c->pack[2] * pitch + c->pack[3] * bytes;
    for (unsigned y = 0; y < m->h; y++)
      for (unsigned x = 0; x < m->w; x++)
        gl_pixel_put(p + y * pitch + x * bytes, I(2), I(3),
                     m->pixels[y * m->w + x]);
    break;
  }
  default:
    gl_error(c, GL_INVALID_OPERATION);
    break;
  }
}
