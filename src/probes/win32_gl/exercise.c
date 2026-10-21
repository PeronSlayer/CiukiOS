/* Authored Win32 GL exercise, result page reserved before window creation.
 * SPDX-License-Identifier: MIT */
#include <w32/gl_client.h>
typedef uintptr_t HWND;
typedef struct {
  unsigned style;
  intptr_t(W32API *lpfnWndProc)(HWND, unsigned, uintptr_t, intptr_t);
  int cbClsExtra, cbWndExtra;
  void *hInstance, *hIcon, *hCursor;
  HBRUSH hbrBackground;
  const char *lpszMenuName, *lpszClassName;
} WNDCLASSA;
extern intptr_t W32API DefWindowProcA(HWND, unsigned, uintptr_t, intptr_t);
extern unsigned short W32API RegisterClassA(const WNDCLASSA *);
extern HWND W32API CreateWindowExA(DWORD, const char *, const char *, DWORD,
                                   int, int, int, int, HWND, void *, void *,
                                   void *);
extern HDC W32API GetDC(HWND);
extern int W32API ReleaseDC(HWND, HDC);
extern BOOL W32API DestroyWindow(HWND);
extern BOOL W32API AdjustWindowRect(RECT *, DWORD, BOOL);
#define WS_VISIBLE 0x10000000u
#define WS_OVERLAPPEDWINDOW 0x00cf0000u
#include "controller.h"
#include "scenes.h"
extern void *W32API VirtualAlloc(void *, DWORD, DWORD, DWORD);
extern void W32API Sleep(DWORD);
extern void W32API ExitProcess(DWORD);
int _fltused = 1;
static volatile struct w32_gl_result *result;
#define CHECK(v)                                                               \
  do {                                                                         \
    if (!(v)) {                                                                \
      result->errors++;                                                        \
      if (!result->first_error)                                                \
        result->first_error = __LINE__;                                        \
    }                                                                          \
  } while (0)
void W32API entry(void) {
  result = VirtualAlloc((void *)W32_GL_RESULT_VA, 4096, 0x3000, 4);
  if (!result)
    ExitProcess(8);
  result->magic = W32_GL_MAGIC;
  result->version = 2;
  WNDCLASSA klass = {.lpfnWndProc = DefWindowProcA,
                     .lpszClassName = "CiukiGLExercise"};
  CHECK(RegisterClassA(&klass));
  RECT rect = {0,0,W32_GL_WIDTH,W32_GL_HEIGHT};
  CHECK(AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, 0));
  HWND window = CreateWindowExA(0, "CiukiGLExercise", "CiukiOS software OpenGL",
                                WS_VISIBLE | WS_OVERLAPPEDWINDOW, 24, 24,
                                rect.right-rect.left, rect.bottom-rect.top, 0, 0, 0, 0);
  CHECK(window);
  HDC dc = GetDC(window);
  CHECK(dc);
  PIXELFORMATDESCRIPTOR p = {.nSize = 40,
                             .nVersion = 1,
                             .dwFlags = PFD_DRAW_TO_WINDOW |
                                        PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                             .cColorBits = 24,
                             .cDepthBits = 16};
  int format = ChoosePixelFormat(dc, &p);
  CHECK(format);
  result->pixel_formats = DescribePixelFormat(dc, format, sizeof(p), &p);
  CHECK(SetPixelFormat(dc, format, &p));
  HGLRC context = wglCreateContext(dc);
  CHECK(context);
  CHECK(wglMakeCurrent(dc, context));
  DWORD sizes[6] = {0};
  CHECK(CiukiGLGetSizes(dc, sizes));
  result->render_width = sizes[0]; result->render_height = sizes[1];
  result->drawable_width = sizes[2]; result->drawable_height = sizes[3];
  result->display_width = sizes[4]; result->display_height = sizes[5];
  const unsigned char *extensions = glGetString(GL_EXTENSIONS);
  if (extensions && *extensions) {
    result->extensions = 1;
    for (const unsigned char *s = extensions; *s; s++)
      if (*s == ' ')
        result->extensions++;
  }
  CHECK(result->extensions == 8);
  for (unsigned i = 0; i < 7; i++) {
    w32_gl_scene(i);
    CHECK(glGetError() == GL_NO_ERROR);
    result->checksum[i] = w32_gl_checksum();
    CHECK(result->checksum[i] == w32_gl_expected[i]);
    BOOL completed = SwapBuffers(dc);
    CHECK(completed);
    if (completed) result->frames++;
  }
  GLuint base = glGenLists(96);
  if (base) {
    result->fonts = wglUseFontBitmapsA(dc, 32, 96, base);
    CHECK(result->fonts);
    glDeleteLists(base, 96);
  } else
    CHECK(0);
  CHECK(wglMakeCurrent(0, 0));
  CHECK(wglDeleteContext(context));
  ReleaseDC(window, dc);
  DestroyWindow(window);
  result->done = 1;
  for (unsigned i = 0; i < 30000 && !result->release; i++)
    Sleep(1);
  ExitProcess(result->errors ? 1 : 0);
}
