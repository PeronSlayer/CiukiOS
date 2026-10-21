/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32_WGL_H
#define W32_WGL_H
#include <w32/pixelformat.h>
HGLRC W32API wglCreateContext(HDC);
BOOL W32API wglDeleteContext(HGLRC);
BOOL W32API wglMakeCurrent(HDC, HGLRC);
HGLRC W32API wglGetCurrentContext(void);
HDC W32API wglGetCurrentDC(void);
void *W32API wglGetProcAddress(const char *);
BOOL W32API wglShareLists(HGLRC, HGLRC);
BOOL W32API wglUseFontBitmapsA(HDC, DWORD, DWORD, DWORD);
BOOL W32API wglUseFontBitmapsW(HDC, DWORD, DWORD, DWORD);
BOOL W32API wglUseFontBitmaps(HDC, DWORD, DWORD, DWORD);
BOOL W32API w32_gl_swap(HDC);
#endif
