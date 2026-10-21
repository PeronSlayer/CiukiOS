/* Software formats and an acyclic swap dispatch into the GL owner.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include <w32/pixelformat.h>
#ifndef W32_HOST_MODEL
extern void *W32API GetModuleHandleA(const char *);
extern void *W32API GetProcAddress(void *, const char *);
#else
extern BOOL W32API w32_gl_swap(HDC) __attribute__((weak));
#endif
static PIXELFORMATDESCRIPTOR describe(int n) {
  PIXELFORMATDESCRIPTOR p = {0};
  p.nSize = 40;
  p.nVersion = 1;
  p.dwFlags = PFD_DRAW_TO_WINDOW | PFD_DRAW_TO_BITMAP | PFD_SUPPORT_OPENGL |
              PFD_GENERIC_FORMAT | PFD_SWAP_COPY;
  if (n <= 2)
    p.dwFlags |= PFD_DOUBLEBUFFER;
  p.cColorBits = (n & 1) ? 24 : 16;
  p.cDepthBits = 16;
  p.cRedBits = p.cBlueBits = (n & 1) ? 8 : 5;
  p.cGreenBits = (n & 1) ? 8 : 6;
  p.cRedShift = (n & 1) ? 16 : 11;
  p.cGreenShift = (n & 1) ? 8 : 5;
  return p;
}
int W32API DescribePixelFormat(HDC h, int n, unsigned size,
                               PIXELFORMATDESCRIPTOR *out) {
  GDI_GUARD;
  if (!gdi_dc(h) || n < 1 || n > 4)
    return 0;
  if (out) {
    if (size < 40)
      return 0;
    *out = describe(n);
  }
  return 4;
}
int W32API ChoosePixelFormat(HDC h, const PIXELFORMATDESCRIPTOR *p) {
  GDI_GUARD;
  if (!gdi_dc(h) || !p || p->nSize != 40 || p->nVersion != 1 ||
      p->iPixelType != PFD_TYPE_RGBA || (p->dwFlags & PFD_STEREO) ||
      p->iLayerType)
    return 0;
  return (p->dwFlags & PFD_DOUBLEBUFFER ? 0 : 2) +
         (p->cColorBits && p->cColorBits <= 16 ? 2 : 1);
}
int W32API GetPixelFormat(HDC h) {
  GDI_GUARD;
  struct dc *d = gdi_dc(h);
  return d ? d->pixel_format : 0;
}
BOOL W32API SetPixelFormat(HDC h, int n, const PIXELFORMATDESCRIPTOR *p) {
  GDI_GUARD;
  (void)p;
  struct dc *d = gdi_dc(h);
  if (!d || d->pixel_format || n < 1 || n > 4)
    return 0;
  d->pixel_format = n;
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
