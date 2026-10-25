/* System OpenGL driver formats and an acyclic swap dispatch into the GL owner.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/pixelformat.h>
#ifndef W32_HOST_MODEL
extern void *W32API GetModuleHandleA(const char *);
extern void *W32API GetProcAddress(void *, const char *);
#else
extern BOOL W32API w32_gl_swap(HDC) __attribute__((weak));
extern BOOL W32API w32_user_display_mode(DWORD *) __attribute__((weak));
#endif
/* Keep the original four indices stable. XRGB uses 32 storage bits and
 * RGB 8/8/8; its unused high byte is not a destination-alpha attachment.
 * This is CiukiOS's system driver, not the generic GDI implementation. */
static const struct {
  unsigned char color, depth, stencil, doublebuffer;
} formats[] = {{32, 16, 0, 1}, {16, 16, 0, 1},
               {32, 16, 0, 0}, {16, 16, 0, 0},
               {32, 24, 0, 1}, {16, 24, 0, 1},
               {32, 16, 8, 1}, {16, 16, 8, 1},
               {32, 24, 8, 1}, {16, 24, 8, 1}};
#define FORMAT_COUNT ((int)(sizeof(formats) / sizeof(formats[0])))
static PIXELFORMATDESCRIPTOR describe(int n) {
  PIXELFORMATDESCRIPTOR p = {0};
  p.nSize = 40;
  p.nVersion = 1;
  p.dwFlags = PFD_DRAW_TO_WINDOW | PFD_DRAW_TO_BITMAP | PFD_SUPPORT_OPENGL;
  if (formats[n - 1].doublebuffer)
    p.dwFlags |= PFD_DOUBLEBUFFER | PFD_SWAP_COPY;
  p.cColorBits = formats[n - 1].color;
  p.cDepthBits = formats[n - 1].depth;
  p.cStencilBits = formats[n - 1].stencil;
  p.cRedBits = p.cBlueBits = (n & 1) ? 8 : 5;
  p.cGreenBits = (n & 1) ? 8 : 6;
  p.cRedShift = (n & 1) ? 16 : 11;
  p.cGreenShift = (n & 1) ? 8 : 5;
  return p;
}
int W32API DescribePixelFormat(HDC h, int n, unsigned size,
                               PIXELFORMATDESCRIPTOR *out) {
  GDI_GUARD;
  if (!gdi_dc(h) || n < 1 || n > FORMAT_COUNT)
    return 0;
  if (out) {
    if (size < 40)
      return 0;
    *out = describe(n);
  }
  return FORMAT_COUNT;
}
/* Prefer sufficient precision, then the smallest excess, then a lower index.
 * Zero color/depth requests keep the original XRGB/D16 default. */
static unsigned precision_cost(unsigned have, unsigned want) {
  return have >= want ? have - want : 256 + want - have;
}
int W32API ChoosePixelFormat(HDC h, const PIXELFORMATDESCRIPTOR *p) {
  GDI_GUARD;
  if (!gdi_dc(h) || !p || p->nSize != 40 || p->nVersion != 1 ||
      p->iPixelType != PFD_TYPE_RGBA ||
      ((p->dwFlags & PFD_STEREO) && !(p->dwFlags & 0x80000000u)) ||
      p->iLayerType)
    return 0;
  unsigned required = p->dwFlags & (PFD_DRAW_TO_WINDOW | PFD_DRAW_TO_BITMAP |
                                     PFD_SUPPORT_OPENGL | PFD_SUPPORT_GDI);
  unsigned best_cost = ~0u;
  int best = 0;
  for (int n = 1; n <= FORMAT_COUNT; n++) {
    PIXELFORMATDESCRIPTOR candidate = describe(n);
    if ((candidate.dwFlags & required) != required ||
        (!(p->dwFlags & 0x40000000u) &&
         !!(p->dwFlags & PFD_DOUBLEBUFFER) !=
             !!(candidate.dwFlags & PFD_DOUBLEBUFFER)))
      continue;
    unsigned color = precision_cost(candidate.cColorBits,
                                     p->cColorBits ? p->cColorBits : 32);
    unsigned depth = (p->dwFlags & 0x20000000u)
                         ? 0
                         : precision_cost(candidate.cDepthBits,
                                          p->cDepthBits ? p->cDepthBits : 16);
    unsigned stencil = precision_cost(candidate.cStencilBits, p->cStencilBits);
    unsigned cost = (color * 512 + depth) * 512 + stencil;
    if (cost < best_cost) {
      best_cost = cost;
      best = n;
    }
  }
  return best;
}
int W32API GetPixelFormat(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  return d ? (d->window_pixel_format ? *d->window_pixel_format
                                     : d->pixel_format)
           : 0;
}
BOOL W32API SetPixelFormat(HDC h, int n, const PIXELFORMATDESCRIPTOR *p) {
  GDI_GUARD;
  (void)p;
  struct dc *d = gdi_dc(h);
  if (!d || n < 1 || n > FORMAT_COUNT)
    return 0;
  int *slot =
      d->window_pixel_format ? d->window_pixel_format : &d->pixel_format;
  if (*slot)
    return 0;
  *slot = n;
  return 1;
}
/* Bind once while USER holds the owning window alive with its DC count. */
BOOL W32API w32_gdi_gl_window_format(HDC h, int *slot) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  if (!d || d->memory || !slot || d->window_pixel_format || d->pixel_format)
    return 0;
  d->window_pixel_format = slot;
  return 1;
}
BOOL W32API w32_gdi_gl_size(HDC h, DWORD *w, DWORD *height) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct w32_dib *b = d ? gdi_target(d) : 0;
  if (!b || !w || !height)
    return 0;
  *w = b->width;
  *height = b->height;
  return 1;
}
BOOL W32API SwapBuffers(HDC h) {
#ifdef W32_HOST_MODEL
  return w32_gl_swap ? w32_gl_swap(h) : 0;
#else
  void *m = GetModuleHandleA("opengl32.dll");
  BOOL(W32API * swap)(HDC) = m ? GetProcAddress(m, "w32_gl_swap") : 0;
  return swap ? swap(h) : 0;
#endif
}

/* f7: device gamma ramps (GetDeviceGammaRamp/SetDeviceGammaRamp). On
 * Windows 98 the ramp belongs to the display device and works "only on
 * display devices that support gamma ramps in hardware"; otherwise both
 * functions fail (return FALSE). On CiukiOS a process owns the display only
 * in full screen: after ChangeDisplaySettings(CDS_FULLSCREEN) its frames go
 * to a display lease, where the kernel presenter applies a 3x256 ramp
 * (W32_DISPLAY_GAMMA). The composed desktop of a windowed program has no
 * device ramp. So the ramp exists while USER reports a full-screen display
 * mode for this process and a presenter (opengl32/ddraw) has registered that
 * it applies the ramp on its lease (w32_gdi_gamma_presenter); otherwise both
 * calls fail with ERROR_NOT_SUPPORTED and the program uses its own software
 * gamma (Quake III: "GAMMA: software w/ 0 overbright bits"). The presenter
 * reads the ramp and its serial with w32_gdi_gamma and sends it again after
 * every grant (the kernel resets the lease ramp to identity). */
static uint16_t gamma_ramp[3][256];
static DWORD gamma_serial, gamma_presenters;
static int gamma_ready;
static int display_full_screen(void) {
  DWORD mode[3] = {0, 0, 0};
#ifdef W32_HOST_MODEL
  return w32_user_display_mode && w32_user_display_mode(mode);
#else
  void *m = GetModuleHandleA("user32.dll");
  BOOL(W32API * query)(DWORD *) = m ? GetProcAddress(m, "w32_user_display_mode") : 0;
  return query && query(mode);
#endif
}
static void gamma_identity(void) {
  if (gamma_ready)
    return;
  for (unsigned c = 0; c < 3; c++)
    for (unsigned i = 0; i < 256; i++)
      gamma_ramp[c][i] = (uint16_t)(i * 257u);
  gamma_ready = 1;
}
static int gamma_device(HDC h, const void *ramp) {
  struct dc *d = gdi_dc(h);
  if (!d || d->printer || !ramp) {
    SetLastError(87);
    return 0;
  }
  if (!gamma_presenters || !display_full_screen()) {
    SetLastError(50); /* ERROR_NOT_SUPPORTED */
    return 0;
  }
  gamma_identity();
  return 1;
}
BOOL W32API GetDeviceGammaRamp(HDC h, void *ramp) {
  GDI_GUARD;
  if (!gamma_device(h, ramp))
    return 0;
  w32_copy(ramp, gamma_ramp, sizeof(gamma_ramp));
  return 1;
}
BOOL W32API SetDeviceGammaRamp(HDC h, const void *ramp) {
  GDI_GUARD;
  if (!gamma_device(h, ramp))
    return 0;
  w32_copy(gamma_ramp, ramp, sizeof(gamma_ramp));
  if (!++gamma_serial)
    gamma_serial = 1;
  return 1;
}
/* In the host model the opengl32 tests link GDI's pixelformat.c together
 * with their own counting stand-ins of these two seams (tests/host/win32/gl/
 * transport.c): the stand-ins win, GDI's own tests get these, and the
 * "no gamma seam" build of those tests (GL_TEST_NO_GAMMA) gets none, as a
 * GDI without the device ramp. */
#if defined(W32_HOST_MODEL) && defined(GL_TEST_NO_GAMMA)
#define GDI_GAMMA_SEAM_ABSENT 1
#elif defined(W32_HOST_MODEL)
#define GDI_GAMMA_SEAM __attribute__((weak))
#else
#define GDI_GAMMA_SEAM
#endif
#ifndef GDI_GAMMA_SEAM_ABSENT
/* Presenter seam: a presenter that applies the ramp on its full-screen
 * lease registers (TRUE) and unregisters (FALSE). Returns the count. */
DWORD W32API GDI_GAMMA_SEAM w32_gdi_gamma_presenter(BOOL on) {
  GDI_GUARD;
  if (on)
    gamma_presenters++;
  else if (gamma_presenters)
    gamma_presenters--;
  return gamma_presenters;
}
/* Copies the current ramp (identity until SetDeviceGammaRamp) into out
 * (768 WORDs, red, green, blue) and returns its serial (0: never set). */
DWORD W32API GDI_GAMMA_SEAM w32_gdi_gamma(uint16_t *out) {
  GDI_GUARD;
  gamma_identity();
  if (out)
    w32_copy(out, gamma_ramp, sizeof(gamma_ramp));
  return gamma_serial;
}
#endif

/* Selected-font raster boundary, shared by bridge-backed and native/provider
 * fonts. Coverage ownership stays in GDI; the caller receives packed bits. */
DWORD W32API w32_gdi_gl_glyph(HDC h, DWORD cp, BOOL wide, GLYPHMETRICS *m,
                              DWORD size, void *bits) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  struct object *o = d ? gdi_object(d->s.font, GDI_FONT) : 0;
  if (!o || !m || !o->u.font.provider.raster)
    return GDI_ERROR;
  if (!wide && cp < 256)
    cp = gdi_cp1252_unicode(cp);
  struct w32_gdi_glyph g = {0};
  struct font *f = &o->u.font;
  if (!f->provider.raster(f->face, f->em, cp, 0, &g))
    return GDI_ERROR;
  DWORD pitch = (g.width + 31) / 32 * 4, need = pitch * g.height;
  *m = (GLYPHMETRICS){
      g.width, g.height, {g.left, g.top}, (int16_t)((g.advance + 32) / 64), 0};
  DWORD result = need;
  if (bits && size) {
    if (size < need)
      result = GDI_ERROR;
    else {
      w32_fill(bits, 0, need);
      for (DWORD y = 0; y < g.height; y++)
        for (DWORD x = 0; x < g.width; x++)
          if (g.coverage[y * g.pitch + x] >= 128)
            ((uint8_t *)bits)[y * pitch + x / 8] |= (uint8_t)(128 >> (x & 7));
    }
  }
  if (f->provider.free_glyph)
    f->provider.free_glyph(f->face, &g);
  return result;
}
