/* Pixel packing, bitmap and readback. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include "internal.h"
int gl_pixel_bytes(int format, int type) {
  if (type == GL_BITMAP)
    return format == GL_COLOR_INDEX ? 0 : -1;
  if (type == GL_UNSIGNED_SHORT_5_6_5)
    return format == GL_RGB ? 2 : -1;
  if (type != GL_UNSIGNED_BYTE)
    return -1;
  switch (format) {
  case GL_RGBA:
  case GL_BGRA_EXT:
    return 4;
  case GL_RGB:
  case GL_BGR_EXT:
    return 3;
  case GL_LUMINANCE_ALPHA:
    return 2;
  case GL_LUMINANCE:
  case GL_RED:
  case GL_GREEN:
  case GL_BLUE:
  case GL_ALPHA:
    return 1;
  default:
    return -1;
  }
}
unsigned gl_pixel_stride(int w, int f, int t, int align) {
  int b = gl_pixel_bytes(f, t);
  if (b < 0 || w < 0 || w > 65536)
    return 0;
  unsigned n = t == GL_BITMAP ? (w + 7) / 8 : w * b;
  return (n + align - 1) & ~(unsigned)(align - 1);
}
unsigned gl_pixel_get(const uint8_t *p, int f, int t) {
  unsigned r = 0, g = 0, b = 0, a = 255;
  if (t == GL_UNSIGNED_SHORT_5_6_5) {
    unsigned v = p[0] | (unsigned)p[1] << 8;
    r = (v >> 11) * 255 / 31;
    g = ((v >> 5) & 63) * 255 / 63;
    b = (v & 31) * 255 / 31;
  } else
    switch (f) {
    case GL_RGBA:
      a = p[3]; /* fallthrough */
    case GL_RGB:
      r = p[0];
      g = p[1];
      b = p[2];
      break;
    case GL_BGRA_EXT:
      a = p[3]; /* fallthrough */
    case GL_BGR_EXT:
      b = p[0];
      g = p[1];
      r = p[2];
      break;
    case GL_RED:
      r = p[0];
      break;
    case GL_GREEN:
      g = p[0];
      break;
    case GL_BLUE:
      b = p[0];
      break;
    case GL_ALPHA:
      a = p[0];
      break;
    case GL_LUMINANCE_ALPHA:
      a = p[1]; /* fallthrough */
    case GL_LUMINANCE:
      r = g = b = p[0];
      break;
    }
  return a << 24 | r << 16 | g << 8 | b;
}
void gl_pixel_put(uint8_t *p, int f, int t, unsigned c) {
  unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255, a = c >> 24;
  if (t == GL_UNSIGNED_SHORT_5_6_5) {
    unsigned v = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    p[0] = v;
    p[1] = v >> 8;
    return;
  }
  switch (f) {
  case GL_RGBA:
    p[3] = a; /* fallthrough */
  case GL_RGB:
    p[0] = r;
    p[1] = g;
    p[2] = b;
    break;
  case GL_BGRA_EXT:
    p[3] = a; /* fallthrough */
  case GL_BGR_EXT:
    p[0] = b;
    p[1] = g;
    p[2] = r;
    break;
  case GL_RED:
    p[0] = r;
    break;
  case GL_GREEN:
    p[0] = g;
    break;
  case GL_BLUE:
    p[0] = b;
    break;
  case GL_ALPHA:
    p[0] = a;
    break;
  case GL_LUMINANCE_ALPHA:
    p[1] = a; /* fallthrough */
  case GL_LUMINANCE:
    p[0] = r + g + b > 255 ? 255 : r + g + b;
    break;
  }
}
void gl_pixels(struct gl_context *c, unsigned id, const union gl_arg *a,
               union gl_arg *r) {
  (void)r;
  if (id == ID_glPixelZoom) {
    c->zoom[0] = (float)a[0].f;
    c->zoom[1] = (float)a[1].f;
    return;
  }
  if (id == ID_glBitmap) {
    int w = (int)a[0].i, h = (int)a[1].i;
    if (w < 0 || h < 0 || w > 2048 || h > 2048) {
      gl_error(c, GL_INVALID_VALUE);
      return;
    }
    if (!c->raster_valid)
      return;
    int col[4];
    for (int k = 0; k < 4; k++)
      col[k] = (int)(c->raster_color[k] * 255);
    unsigned pitch = gl_pixel_stride(c->unpack[1] ? c->unpack[1] : w,
                                     GL_COLOR_INDEX, GL_BITMAP, c->unpack[0]);
    const uint8_t *p = a[6].p;
    if (p)
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          if (p[(y + c->unpack[2]) * pitch + (x + c->unpack[3]) / 8] &
              (128 >> ((x + c->unpack[3]) & 7)))
            sw_fragment(&c->sw, (int)(c->raster[0] - a[2].f) + x,
                        (int)(c->raster[1] - a[3].f) + y, (int)c->raster[2],
                        col);
    c->raster[0] += (float)a[4].f;
    c->raster[1] += (float)a[5].f;
    return;
  }
  if (id == ID_glCopyPixels) {
    int x = a[0].i, y = a[1].i, w = a[2].i, h = a[3].i;
    if (a[4].i != GL_COLOR) {
      gl_error(c, GL_INVALID_ENUM);
      return;
    }
    if (w < 0 || h < 0 || w > 2048 || h > 2048) {
      gl_error(c, GL_INVALID_VALUE);
      return;
    }
    if (!w || !h || !c->raster_valid)
      return;
    unsigned *p = gl_alloc(w * h * 4);
    if (!p) {
      gl_error(c, GL_OUT_OF_MEMORY);
      return;
    }
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++)
        p[j * w + i] =
            x + i >= 0 && y + j >= 0 && (unsigned)(x + i) < c->sw.width &&
                    (unsigned)(y + j) < c->sw.height
                ? c->sw
                      .color[(c->sw.height - 1 - (y + j)) * c->sw.width + x + i]
                : 0;
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) {
        unsigned pix = p[j * w + i];
        int color[4] = {(pix >> 16) & 255, (pix >> 8) & 255, pix & 255, 255};
        sw_fragment(&c->sw, (int)c->raster[0] + i, (int)c->raster[1] + j,
                    (int)c->raster[2], color);
      }
    gl_dealloc(p);
    return;
  }
  int read = id == ID_glReadPixels, base = read ? 2 : 0;
  int w = a[base].i, h = a[base + 1].i, f = a[base + 2].i, t = a[base + 3].i;
  int bytes = gl_pixel_bytes(f, t);
  if (w < 0 || h < 0 || w > 2048 || h > 2048) {
    gl_error(c, GL_INVALID_VALUE);
    return;
  }
  if (bytes <= 0) {
    gl_error(c, GL_INVALID_ENUM);
    return;
  }
  if (!w || !h)
    return;
  uint8_t *p = (uint8_t *)a[base + 4].p;
  if (!p) {
    gl_error(c, GL_INVALID_VALUE);
    return;
  }
  int *state = read ? c->pack : c->unpack;
  unsigned pitch = gl_pixel_stride(state[1] ? state[1] : w, f, t, state[0]);
  p += state[2] * pitch + state[3] * bytes;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      if (read) {
        int px = (int)a[0].i + x, py = (int)a[1].i + y;
        unsigned color = 0xff000000;
        if (px >= 0 && py >= 0 && (unsigned)px < c->sw.width &&
            (unsigned)py < c->sw.height)
          color = c->sw.color[(c->sw.height - 1 - py) * c->sw.width + px] |
                  0xff000000;
        gl_pixel_put(p + y * pitch + x * bytes, f, t, color);
      } else if (c->raster_valid) {
        unsigned col = gl_pixel_get(p + y * pitch + x * bytes, f, t);
        int color[4] = {(col >> 16) & 255, (col >> 8) & 255, col & 255,
                        col >> 24};
        int x0 = (int)(c->raster[0] + x * c->zoom[0]),
            x1 = (int)(c->raster[0] + (x + 1) * c->zoom[0]),
            y0 = (int)(c->raster[1] + y * c->zoom[1]),
            y1 = (int)(c->raster[1] + (y + 1) * c->zoom[1]);
        if (x1 < x0) {
          int q = x0;
          x0 = x1;
          x1 = q;
        }
        if (y1 < y0) {
          int q = y0;
          y0 = y1;
          y1 = q;
        }
        if (x0 < -2048)
          x0 = -2048;
        if (y0 < -2048)
          y0 = -2048;
        if (x1 > 4096)
          x1 = 4096;
        if (y1 > 4096)
          y1 = 4096;
        for (int yy = y0; yy < y1; yy++)
          for (int xx = x0; xx < x1; xx++)
            sw_fragment(&c->sw, xx, yy, (int)c->raster[2], color);
      }
    }
}
