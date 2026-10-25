/* Legacy WGL entry names delegate to the same GDI format namespace. The normal
 * Quake ICD path imports GDI directly; mini-drivers resolve these names.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
int W32API wglChoosePixelFormat(HDC dc, const PIXELFORMATDESCRIPTOR *p) {
  return ChoosePixelFormat(dc, p);
}
int W32API wglDescribePixelFormat(HDC dc, int format, unsigned size,
                                  PIXELFORMATDESCRIPTOR *p) {
  return DescribePixelFormat(dc, format, size, p);
}
int W32API wglGetPixelFormat(HDC dc) { return GetPixelFormat(dc); }
BOOL W32API wglSetPixelFormat(HDC dc, int format,
                              const PIXELFORMATDESCRIPTOR *p) {
  return SetPixelFormat(dc, format, p);
}
BOOL W32API wglSwapBuffers(HDC dc) { return w32_gl_swap(dc); }
