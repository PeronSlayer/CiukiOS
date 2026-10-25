/* Serialized WGL contexts; the current mapping is per thread. TinyGL globals
 * are selected only inside the common lock. SPDX-License-Identifier:
 * LGPL-2.1-or-later */
#include "internal.h"
static struct gl_context *contexts[16];
static unsigned serial, owner, depth;
static volatile uint32_t mutex;
static unsigned tid(void) {
  const struct w32_services *s = w32_get_services();
  return s->thread_id(s->opaque);
}
void gl_lock(void) {
  unsigned t = tid();
  if (__atomic_load_n(&owner, __ATOMIC_ACQUIRE) == t) {
    depth++;
    return;
  }
  while (__atomic_exchange_n(&mutex, 1, __ATOMIC_ACQUIRE)) {
    const struct w32_services *s = w32_get_services();
    s->wait_word(s->opaque, &mutex, 1);
  }
  depth = 1;
  __atomic_store_n(&owner, t, __ATOMIC_RELEASE);
}
void gl_unlock(int *unused) {
  (void)unused;
  if (--depth)
    return;
  __atomic_store_n(&owner, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&mutex, 0, __ATOMIC_RELEASE);
  const struct w32_services *s = w32_get_services();
  s->wake_word(s->opaque, &mutex);
}
void *gl_alloc(unsigned n) {
  if (!n || n > 64 * 1024 * 1024)
    return 0;
  const struct w32_services *s = w32_get_services();
  unsigned *p = s->alloc(s->opaque, n + 16);
  if (!p)
    return 0;
  *p = n + 16;
  w32_fill(p + 4, 0, n);
  return p + 4;
}
void gl_dealloc(void *v) {
  if (v) {
    unsigned *p = (unsigned *)v - 4;
    const struct w32_services *s = w32_get_services();
    s->free(s->opaque, p, *p);
  }
}
struct gl_context *gl_current(void) {
  unsigned t = tid();
  for (unsigned i = 0; i < 16; i++)
    if (contexts[i] && contexts[i]->tid == t)
      return contexts[i];
  return 0;
}
static struct gl_context *find(HGLRC h) {
  for (unsigned i = 0; i < 16; i++)
    if (contexts[i] && contexts[i]->handle == h)
      return contexts[i];
  return 0;
}
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
  gl_hw_software(c);
  c->sw.color = (c->draw_buffer == GL_FRONT || c->draw_buffer == GL_FRONT_LEFT) ? c->front_color : c->back_color;
  c->sw.color2 = c->doublebuffer && c->draw_buffer == GL_FRONT_AND_BACK ? c->front_color : 0;
  c->sw.color_disabled = c->draw_buffer == GL_NONE;
}
static void free_attachments(struct gl_context *c) {
  if (c->doublebuffer) gl_dealloc(c->front_color);
  c->front_color = c->back_color = 0;
  gl_dealloc(c->sw.depth);
  gl_dealloc(c->sw.depth24);
  gl_dealloc(c->sw.stencil);
  c->sw.depth = 0; c->sw.depth24 = 0; c->sw.stencil = 0;
}
int gl_refresh_target(struct gl_context *c, HDC dc) {
  unsigned dw, dh, window;
  if (!gl_target(dc, &dw, &dh, &window) || !dw || !dh || dw > 2048 || dh > 2048)
    return 0;
  unsigned w = dw, h = dh;
  gl_render_size(&w, &h);
  if (w != c->sw.width || h != c->sw.height) {
    BITMAPINFO info = {.bmiHeader = {.biSize = 40, .biWidth = w,
        .biHeight = -(LONG)h, .biPlanes = 1, .biBitCount = 32}};
    void *bits = 0;
    HBITMAP back = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, 0, 0);
    void *depth = gl_alloc(w * h * (c->depth_bits == 24 ? 4 : 2));
    uint8_t *stencil = c->stencil_bits ? gl_alloc(w * h) : 0;
    uint32_t *front = c->doublebuffer ? gl_alloc(w * h * 4) : bits;
    if (!back || !bits || !depth || (c->stencil_bits && !stencil) || !front) {
      if (back) DeleteObject(back);
      gl_dealloc(depth);
      gl_dealloc(stencil);
      if (c->doublebuffer) gl_dealloc(front);
      return 0;
    }
    /* Contents after drawable resize are undefined; never expose old storage. */
    gl_hw_close(c);
    gl_release_present(c);
    if (c->back) DeleteObject(c->back);
    free_attachments(c);
    c->back = back; c->back_color = bits; c->front_color = front;
    gl_select_buffer(c);
    if (c->depth_bits == 24) c->sw.depth24 = depth;
    else c->sw.depth = depth;
    c->sw.stencil = stencil;
    c->sw.width = w; c->sw.height = h;
  }
  c->drawable_width = dw; c->drawable_height = dh;
  gl_window_state(c);
  return 1;
}
HGLRC W32API wglCreateContext(HDC dc) {
  GL_GUARD;
  int format = GetPixelFormat(dc);
  unsigned w = 0, h = 0, window = 0;
  if (!format || !gl_target(dc, &w, &h, &window) || !w || !h || w > 2048 ||
      h > 2048)
    return 0;
  unsigned slot;
  for (slot = 0; slot < 16 && contexts[slot]; slot++) {
  }
  if (slot == 16)
    return 0;
  struct gl_context *c = gl_alloc(sizeof(*c));
  if (!c)
    return 0;
  PIXELFORMATDESCRIPTOR pfd;
  if (!DescribePixelFormat(dc, format, sizeof(pfd), &pfd) ||
      (pfd.cDepthBits != 16 && pfd.cDepthBits != 24) ||
      (pfd.cStencilBits && pfd.cStencilBits != 8)) {
    gl_dealloc(c); return 0;
  }
  c->format = format;
  c->depth_bits = pfd.cDepthBits;
  c->stencil_bits = pfd.cStencilBits;
  c->doublebuffer = !!(pfd.dwFlags & PFD_DOUBLEBUFFER);
  c->sw.rgb565 = pfd.cColorBits <= 16;
  c->share = gl_alloc(sizeof(*c->share));
  if (!c->share) {
    gl_dealloc(c);
    return 0;
  }
  c->share->refs = 1;
  c->zb = ZB_open(4, 4, ZB_MODE_RGBA, 0);
  if (!gl_refresh_target(c, dc) || !c->zb) {
    if (c->back)
      DeleteObject(c->back);
    free_attachments(c);
    gl_dealloc(c->share);
    if (c->zb)
      ZB_close(c->zb);
    gl_dealloc(c);
    return 0;
  }
  c->handle = (++serial << 4) | slot;
  c->dc = dc;
  c->format = format;
  gl_selected_context = &c->tiny;
  glInit(c->zb);
  if (c->tiny.error_flag) {
    glClose();
    gl_selected_context = 0;
    ZB_close(c->zb);
    DeleteObject(c->back);
    free_attachments(c);
    gl_dealloc(c->share);
    gl_dealloc(c);
    return 0;
  }
  c->tiny.software_triangle = gl_clip_triangle;
  c->tiny.software_vertex = gl_vertex_generated;
  c->tiny.software_line = gl_compat_line;
  c->tiny.software_state = c;
  c->texture_base[0] = c->texture_ptr[0] = c->tiny.matrix_stack[2];
  c->texture_base[1] = c->texture_ptr[1] = c->texture1_stack;
  gl_M4_Id(c->texture1_stack);
  for (int unit = 0; unit < 2; unit++)
    c->tiny.software_texture_matrix[unit] = c->texture_ptr[unit];
  c->tiny.current_tex_coord1.W = 1;
  c->viewport[2] = w;
  c->viewport[3] = h;
  c->scissor[2] = w; c->scissor[3] = h;
  gl_window_state(c);
  glViewport(0, 0, w, h);
  gl_defaults(c);
  contexts[slot] = c;
  gl_selected_context = 0;
  return c->handle;
}
static void release_share(struct gl_share *s) {
  if (--s->refs)
    return;
  gl_free_textures(s);
  for (unsigned i = 0; i < GL_LISTS; i++)
    gl_free_list(&s->lists[i]);
  gl_dealloc(s);
}
BOOL W32API wglDeleteContext(HGLRC h) {
  GL_GUARD;
  struct gl_context *c = find(h);
  if (!c || (c->tid && c->tid != tid()))
    return 0;
  gl_hw_close(c);
  for (unsigned i = 0; i < 16; i++)
    if (contexts[i] == c)
      contexts[i] = 0;
  gl_selected_context = &c->tiny;
  c->tiny.matrix_stack[2] = c->texture_base[0];
  glClose();
  gl_selected_context = 0;
  ZB_close(c->zb);
  gl_drop_state(c);
  gl_free_list(&c->pending_list);
  gl_texture_cleanup(c);
  gl_release_present(c);
  release_share(c->share);
  if (c->back)
    DeleteObject(c->back);
  free_attachments(c);
  gl_dealloc(c);
  return 1;
}
BOOL W32API wglMakeCurrent(HDC dc, HGLRC h) {
  GL_GUARD;
  struct gl_context *old = gl_current(), *c = h ? find(h) : 0;
  if (old) {
    gl_hw_sync(old);
    old->tid = 0;
  }
  if (!h)
    return 1;
  if (!c || (c->tid && c->tid != tid()) || GetPixelFormat(dc) != (int)c->format)
    return 0;
  if (!gl_refresh_target(c, dc))
    return 0;
  c->tid = tid();
  if (c->dc != dc) {
    gl_hw_close(c);
    gl_release_present(c);
  }
  c->dc = dc;
  /* Quake probes device gamma immediately after MakeCurrent, before swapping. */
  gl_prepare_present(c, dc);
  return 1;
}
HGLRC W32API wglGetCurrentContext(void) {
  GL_GUARD;
  struct gl_context *c = gl_current();
  return c ? c->handle : 0;
}
HDC W32API wglGetCurrentDC(void) {
  GL_GUARD;
  struct gl_context *c = gl_current();
  return c ? c->dc : 0;
}
void *W32API wglGetProcAddress(const char *n) {
  GL_GUARD;
  return gl_current() && n ? gl_extension(n) : 0;
}
BOOL W32API wglShareLists(HGLRC a, HGLRC b) {
  GL_GUARD;
  struct gl_context *s = find(a), *d = find(b);
  if (!s || !d || s == d || d->tid || s->format != d->format)
    return 0;
  for (struct gl_texture *n = d->share->textures; n; n = n->next)
    if (n->value.used)
      return 0;
  for (unsigned i = 1; i < GL_LISTS; i++)
    if (d->share->lists[i].reserved)
      return 0;
  release_share(d->share);
  d->share = s->share;
  d->share->refs++;
  gl_sync_textures(d);
  return 1;
}
/* Font bridge GGO_BITMAP rows are DWORD-aligned, MSB first, top-down;
 * GL bitmap rows run bottom-up. Commands own a deep copy of their bitmap. */
static BOOL fonts(HDC dc, DWORD first, DWORD count, DWORD base, int wide) {
  GL_GUARD;
  struct gl_context *c = gl_current();
  if (!c || !base || base >= GL_LISTS || count > GL_LISTS - base ||
      first > 65535 || count > 65536 - first)
    return 0;

  int old[6];
  w32_copy(old, c->unpack, sizeof(old));
  w32_fill(c->unpack, 0, sizeof(c->unpack));
  c->unpack[0] = 1;
  for (DWORD i = 0; i < count; i++) {
    GLYPHMETRICS m;
    DWORD n = w32_gdi_gl_glyph(dc, first + i, wide, &m, 0, 0);
    if (n == 0xffffffffu) {
      w32_copy(c->unpack, old, sizeof(old));
      return 0;
    }
    uint8_t *bits = n ? gl_alloc(n) : 0, *flipped = n ? gl_alloc(n) : 0;
    if (n && (!bits || !flipped)) {
      gl_dealloc(bits);
      gl_dealloc(flipped);
      w32_copy(c->unpack, old, sizeof(old));
      return 0;
    }
    if (n) {
      DWORD r = w32_gdi_gl_glyph(dc, first + i, wide, &m, n, bits);
      if (r != n) {
        gl_dealloc(bits);
        gl_dealloc(flipped);
        w32_copy(c->unpack, old, sizeof(old));
        return 0;
      }
      unsigned pitch = (m.gmBlackBoxX + 31) / 32 * 4,
               row = (m.gmBlackBoxX + 7) / 8;
      for (unsigned y = 0; y < m.gmBlackBoxY; y++)
        w32_copy(flipped + y * row, bits + (m.gmBlackBoxY - 1 - y) * pitch,
                 row);
    }
    w32_glNewList(base + i, GL_COMPILE);
    w32_glBitmap(m.gmBlackBoxX, m.gmBlackBoxY, -m.gmptGlyphOrigin.x,
                 (float)m.gmBlackBoxY - m.gmptGlyphOrigin.y, m.gmCellIncX,
                 m.gmCellIncY, flipped);
    w32_glEndList();
    gl_dealloc(bits);
    gl_dealloc(flipped);
  }
  w32_copy(c->unpack, old, sizeof(old));
  return !c->error;
}
BOOL W32API wglUseFontBitmapsA(HDC d, DWORD f, DWORD n, DWORD b) {
  return fonts(d, f, n, b, 0);
}
BOOL W32API wglUseFontBitmapsW(HDC d, DWORD f, DWORD n, DWORD b) {
  return fonts(d, f, n, b, 1);
}
BOOL W32API wglUseFontBitmaps(HDC d, DWORD f, DWORD n, DWORD b) {
  return fonts(d, f, n, b, 0);
}
BOOL W32API w32_gl_swap(HDC dc) {
  GL_GUARD;
  struct gl_context *c = gl_current();
  if (!c || c->dc != dc || c->tiny.in_begin)
    return 0;
  if (!gl_present(c, 0))
    return 0;
  if (c->doublebuffer) w32_copy(c->front_color, c->back_color, c->sw.width * c->sw.height * 4);
  c->frames++;
  return 1;
}
BOOL W32API GlDllMain(void *m, DWORD why, void *reserved) {
  (void)m;
  (void)reserved;
  if (why == 3) {
    GL_GUARD;
    struct gl_context *c = gl_current();
    if (c)
      c->tid = 0;
  }
  if (why == 0) {
    GL_GUARD;
    for (unsigned i = 0; i < 16; i++)
      if (contexts[i]) {
        contexts[i]->tid = 0;
        wglDeleteContext(contexts[i]->handle);
      }
  }
  return 1;
}
