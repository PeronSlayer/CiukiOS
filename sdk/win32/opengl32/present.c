/* Existing GDI and delegated-present boundaries. No device access.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <ciuki/present_abi.h>
#include "internal.h"
#include <w32/shims.h>
#ifndef W32_HOST_MODEL
extern void *W32API GetModuleHandleA(const char *);
extern void *W32API GetProcAddress(void *, const char *);
extern DWORD w32r_gate(DWORD, DWORD, DWORD, DWORD);
#endif
struct gl_lease {
  DWORD active, token, generation, width, height, bpp, pitch;
};
static struct {
  BOOL(W32API *target)(DWORD, DWORD *);
  BOOL(W32API *damage)(DWORD);
  int32_t(W32API *lease)(uintptr_t, DWORD, DWORD, DWORD, struct gl_lease *);
  void(W32API *release)(uintptr_t);
} user;
static void bind_user(void) {
  if (user.target)
    return;
#ifdef W32_HOST_MODEL
  extern BOOL w32_user_gl_target(DWORD, DWORD *);
  extern BOOL w32_user_gl_damage(DWORD);
  extern int32_t w32_user_display_lease(uintptr_t, DWORD, DWORD, DWORD, struct gl_lease *);
  extern void w32_user_display_release(uintptr_t);
  user.target = w32_user_gl_target;
  user.damage = w32_user_gl_damage;
  user.lease = w32_user_display_lease;
  user.release = w32_user_display_release;
#else
  void *m = GetModuleHandleA("user32.dll");
  if (!m)
    return;
  user.target = GetProcAddress(m, "w32_user_gl_target");
  user.damage = GetProcAddress(m, "w32_user_gl_damage");
  user.lease = GetProcAddress(m, "w32_user_display_lease");
  user.release = GetProcAddress(m, "w32_user_display_release");
#endif
}
int gl_target(HDC dc, unsigned *w, unsigned *h, unsigned *window) {
  DWORD dw = 0, dh = 0;
  *window = 0;
  bind_user();
  DWORD values[4];
  int result = user.target ? user.target(dc, values) : 0;
  if (result < 0)
    return 0;
  if (result) {
    *w = values[0];
    *h = values[1];
    *window = values[2];
    return 1;
  }
  if (!w32_gdi_gl_size(dc, &dw, &dh))
    return 0;
  *w = dw;
  *h = dh;
  return 1;
}
void gl_render_size(unsigned *w, unsigned *h) {
  const struct ciuki_w32_compat *p = w32_shim_current();
  if (!p || !(p->mask & W32_SHIM_BIT(W32_SH_DDRAW_SCALE)))
    return;
  unsigned width = p->width, height = p->height;
  if (!width && p->scale > 1) {
    width = *w / p->scale;
    height = *h / p->scale;
  }
  static const unsigned sizes[][2] = {
      {320, 240}, {400, 300}, {512, 384}, {640, 480}};
  for (unsigned i = 0; i < 4; i++)
    if (width == sizes[i][0] && height == sizes[i][1]) {
      *w = width;
      *h = height;
      return;
    }
}
void gl_release_present(struct gl_context *c) {
  bind_user();
  if (c->leased_window && user.release)
    user.release(c->leased_window);
  c->leased_window = 0;
}
static void display_size(unsigned *dw, unsigned *dh) {
#ifndef W32_HOST_MODEL
  struct ciuki_display_info info = {.version = CIUKI_DISPLAY_VERSION,
                                    .size = sizeof(info)};
  if ((int32_t)w32r_gate(CIUKI_SYS_DISPLAY, CIUKI_DISPLAY_QUERY,
                         (DWORD)(uintptr_t)&info, 0) >= 0) {
    *dw = info.width;
    *dh = info.height;
  } else {
    *dw = 0;
    *dh = 0;
  }
#else
  (void)dw;
  (void)dh;
#endif
}
BOOL W32API CiukiGLGetSizes(HDC dc, DWORD *values) {
  GL_GUARD;
  struct gl_context *c = gl_current();
  if (!c || c->dc != dc || !values || !gl_refresh_target(c, dc))
    return 0;
  values[0] = c->sw.width; values[1] = c->sw.height;
  values[2] = c->drawable_width; values[3] = c->drawable_height;
  unsigned dw = values[2], dh = values[3];
  display_size(&dw, &dh);
  values[4] = dw; values[5] = dh;
  return 1;
}
static void size_log(struct gl_context *c, unsigned dw, unsigned dh) {
  display_size(&dw, &dh);
  const struct w32_services *s = w32_get_services();
  if (!s->diagnostic || c->frames)
    return;
  char line[100] = "software render=";
  unsigned used = w32_strlen(line),
           values[4] = {c->sw.width, c->sw.height, dw, dh};
  for (unsigned k = 0; k < 4; k++) {
    char n[12];
    unsigned len = 0, v = values[k];
    do {
      n[len++] = (char)('0' + v % 10);
      v /= 10;
    } while (v);
    while (len)
      line[used++] = n[--len];
    if (k == 0 || k == 2)
      line[used++] = 'x';
    else if (k == 1) {
      const char *text = " display=";
      while (*text)
        line[used++] = *text++;
    }
  }
  line[used] = 0;
  s->diagnostic(s->opaque, "info", "opengl32.dll", line);
}
BOOL gl_present(struct gl_context *c, int front) {
  unsigned w, h, window;
  if (!gl_refresh_target(c, c->dc) || !gl_target(c->dc, &w, &h, &window))
    return 0;
  const uint32_t *source = front ? c->front_color : c->back_color;
  bind_user();
  DWORD values[4];
  if (user.target && user.target(c->dc, values) > 0 && values[3]) {
    if (c->leased_window && c->leased_window != window)
      gl_release_present(c);
    if (!user.lease)
      return 0;
    struct gl_lease lease = {0};
    if (user.lease(window, c->sw.width, c->sw.height, c->sw.rgb565 ? 16 : 32,
                   &lease) ||
        !lease.active)
      return 0;
    c->leased_window = window;
    if (lease.width != c->sw.width || lease.height != c->sw.height ||
        (lease.bpp != 16 && lease.bpp != 32) ||
        lease.pitch < c->sw.width * (lease.bpp / 8) || lease.pitch > 65536)
      return 0;
    uint8_t *copy = gl_alloc(lease.pitch * lease.height);
    if (!copy)
      return 0;
    for (unsigned y = 0; y < lease.height; y++)
      for (unsigned x = 0; x < lease.width; x++) {
        unsigned p = source[y * c->sw.width + x];
        if (lease.bpp == 32)
          w32_put32(copy + y * lease.pitch + x * 4, p);
        else
          w32_put16(copy + y * lease.pitch + x * 2,
                    ((p >> 8) & 0xf800) | ((p >> 5) & 0x7e0) | ((p >> 3) & 31));
      }
    struct ciuki_w32_display_args a = {.header = {1, sizeof(a), W32_DISPLAY, 0},
                                       .action = W32_DISPLAY_FRAME,
                                       .token = lease.token,
                                       .generation = lease.generation,
                                       .address = (DWORD)(uintptr_t)copy};
    int32_t result;
#ifdef W32_HOST_MODEL
    extern int32_t gl_host_display_frame(struct ciuki_w32_display_args *, const void *);
    result = gl_host_display_frame(&a, copy);
#else
    result =
        (int32_t)w32r_gate(CIUKI_SYS_W32, (DWORD)(uintptr_t)&a, sizeof(a), 0);
#endif
    gl_dealloc(copy);
    if (result < 0 || a.error || !a.result)
      return 0;
    c->leased_window = window;
    size_log(c, w, h);
    return 1;
  }
  /* LEASE(0) is synchronous: composition must be restored before PRESENT. */
  gl_release_present(c);
  BITMAPINFO info = {.bmiHeader = {.biSize = 40,
                                   .biWidth = c->sw.width,
                                   .biHeight = -(LONG)c->sw.height,
                                   .biPlanes = 1,
                                   .biBitCount = 32}};
  if (!StretchDIBits(c->dc, 0, 0, w, h, 0, 0, c->sw.width, c->sw.height,
                     source, &info, DIB_RGB_COLORS, SRCCOPY))
    return 0;
  if (window && (!user.damage || !user.damage(c->dc)))
    return 0;
  size_log(c, w, h);
  return 1;
}
