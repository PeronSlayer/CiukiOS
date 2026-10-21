/* Public WGL/GDI pixel-format ABI. SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef W32_PIXELFORMAT_H
#define W32_PIXELFORMAT_H
#include <w32/gdi32.h>
typedef uint32_t HGLRC;
typedef struct {
  uint16_t nSize, nVersion;
  DWORD dwFlags;
  uint8_t iPixelType, cColorBits, cRedBits, cRedShift, cGreenBits, cGreenShift,
      cBlueBits, cBlueShift, cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits,
      cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits, cDepthBits,
      cStencilBits, cAuxBuffers, iLayerType, bReserved;
  DWORD dwLayerMask, dwVisibleMask, dwDamageMask;
} PIXELFORMATDESCRIPTOR;
_Static_assert(sizeof(PIXELFORMATDESCRIPTOR) == 40, "public pixel format ABI");
#define PFD_DOUBLEBUFFER 1
#define PFD_STEREO 2
#define PFD_DRAW_TO_WINDOW 4
#define PFD_DRAW_TO_BITMAP 8
#define PFD_SUPPORT_GDI 16
#define PFD_SUPPORT_OPENGL 32
#define PFD_GENERIC_FORMAT 64
#define PFD_SWAP_COPY 1024
#define PFD_TYPE_RGBA 0
#define PFD_TYPE_COLORINDEX 1
#define PFD_MAIN_PLANE 0
int W32API ChoosePixelFormat(HDC, const PIXELFORMATDESCRIPTOR *);
int W32API DescribePixelFormat(HDC, int, unsigned, PIXELFORMATDESCRIPTOR *);
int W32API GetPixelFormat(HDC);
BOOL W32API SetPixelFormat(HDC, int, const PIXELFORMATDESCRIPTOR *);
BOOL W32API SwapBuffers(HDC);
DWORD W32API w32_gdi_gl_glyph(HDC, DWORD, BOOL, GLYPHMETRICS *, DWORD, void *);
/* Value-only surface query; no backing pointer escapes GDI's lock. */
BOOL W32API w32_gdi_gl_size(HDC, DWORD *, DWORD *);
#endif
