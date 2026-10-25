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
/* The retained command and execution use the same highest addressed byte,
 * including partial-byte skips and rows shorter than the bitmap itself. */
int gl_bitmap_layout(int w, int h, const int state[6], struct gl_bitmap_layout *l) {
  if (w < 0 || h < 0 || w > 2048 || h > 2048)
    return GL_INVALID_VALUE;
  l->pitch = gl_pixel_stride(state[1] ? state[1] : w, GL_COLOR_INDEX,
                             GL_BITMAP, state[0]);
  uint64_t n = w && h ? ((uint64_t)h - 1 + state[2]) * l->pitch +
                            ((uint64_t)state[3] + w + 7) / 8 : 0;
  if (n > 16 * 1024 * 1024)
    return GL_OUT_OF_MEMORY;
  l->bytes = (unsigned)n;
  return 0;
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
static void pixel_rect(struct gl_context *c, double x0, double y0, double x1,
                       double y1, const int color[4]) {
  int left = gl_window_coord(c, x0, 0), right = gl_window_coord(c, x1, 0),
      bottom = gl_window_coord(c, y0, 1), top = gl_window_coord(c, y1, 1);
  if (right < left) { int n = right; right = left; left = n; }
  if (top < bottom) { int n = top; top = bottom; bottom = n; }
  if (left < 0) left = 0;
  if (bottom < 0) bottom = 0;
  if (right > (int)c->sw.width) right = c->sw.width;
  if (top > (int)c->sw.height) top = c->sw.height;
  for (int y = bottom; y < top; y++)
    for (int x = left; x < right; x++)
      sw_fragment(&c->sw, x, y, (int)c->raster[2], color);
}
void gl_pixels(struct gl_context *c, unsigned id, const union gl_arg *a,
               union gl_arg *r) {
  gl_hw_software(c);
  (void)r;
  c->sw.offset = 0;
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
    struct gl_bitmap_layout layout;
    unsigned error = gl_bitmap_layout(w, h, c->unpack, &layout);
    if (error) { gl_error(c, error); return; }
    unsigned pitch = layout.pitch;
    const uint8_t *p = a[6].p;
    if (p)
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          if (p[(y + c->unpack[2]) * pitch + (x + c->unpack[3]) / 8] &
              (128 >> ((x + c->unpack[3]) & 7)))
            pixel_rect(c, c->raster[0] - a[2].f + x,
                       c->raster[1] - a[3].f + y,
                       c->raster[0] - a[2].f + x + 1,
                       c->raster[1] - a[3].f + y + 1, col);
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
        p[j * w + i] = gl_read_color(c, x + i, y + j);
    for (int j = 0; j < h; j++)
      for (int i = 0; i < w; i++) {
        unsigned pix = p[j * w + i];
        int color[4] = {(pix >> 16) & 255, (pix >> 8) & 255, pix & 255, 255};
        pixel_rect(c, c->raster[0] + i * c->zoom[0], c->raster[1] + j * c->zoom[1],
                   c->raster[0] + (i + 1) * c->zoom[0],
                   c->raster[1] + (j + 1) * c->zoom[1], color);
      }
    gl_dealloc(p);
    return;
  }
  int read = id == ID_glReadPixels, base = read ? 2 : 0;
  int w = a[base].i, h = a[base + 1].i, f = a[base + 2].i, t = a[base + 3].i;
  /* Stencil readback is used by Quake's overdraw diagnostic. Pixel transfer
   * defaults are identity; stencil is not a texture/color upload format. */
  if (read && f == GL_STENCIL_INDEX && t == GL_UNSIGNED_BYTE) {
    if (!c->sw.stencil) { gl_error(c, GL_INVALID_OPERATION); return; }
    if (w < 0 || h < 0 || w > 2048 || h > 2048) { gl_error(c, GL_INVALID_VALUE); return; }
    if (!w || !h) return;
    uint8_t *out = (uint8_t *)a[6].p;
    if (!out) { gl_error(c, GL_INVALID_VALUE); return; }
    unsigned pitch = ((unsigned)(c->pack[1] ? c->pack[1] : w) + c->pack[0] - 1) & ~(unsigned)(c->pack[0]-1);
    out += (unsigned)c->pack[2] * pitch + (unsigned)c->pack[3];
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
      int64_t px = a[0].i + x, py = a[1].i + y;
      unsigned value = 0;
      if (c->sw.stencil && px >= 0 && py >= 0 && (unsigned)px < c->drawable_width &&
          (unsigned)py < c->drawable_height) {
        px = gl_window_coord(c, px, 0); py = gl_window_coord(c, py, 1);
        value = c->sw.stencil[(c->sw.height-1-(unsigned)py)*c->sw.width+(unsigned)px];
      }
      out[(unsigned)y * pitch + (unsigned)x] = value;
    }
    return;
  }
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
        unsigned color = gl_read_color(c, px, py);
        gl_pixel_put(p + y * pitch + x * bytes, f, t, color);
      } else if (c->raster_valid) {
        unsigned col = gl_pixel_get(p + y * pitch + x * bytes, f, t);
        int color[4] = {(col >> 16) & 255, (col >> 8) & 255, col & 255,
                        col >> 24};
        pixel_rect(c, c->raster[0] + x * c->zoom[0], c->raster[1] + y * c->zoom[1],
                   c->raster[0] + (x + 1) * c->zoom[0],
                   c->raster[1] + (y + 1) * c->zoom[1], color);
      }
    }
}
