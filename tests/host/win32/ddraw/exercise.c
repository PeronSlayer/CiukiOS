/* Authored PE exercise driver for the lead's fullscreen/DirectDraw probes.
 * Runtime evidence is collected by the kernel controller, never fabricated
 * here. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/ddraw.h>
#define IMPORT __declspec(dllimport)
#define API W32API
typedef intptr_t(API *PROC)(uintptr_t, DWORD, uintptr_t, intptr_t);
struct window_class {
  DWORD style;
  PROC procedure;
  int class_bytes, window_bytes;
  void *instance, *icon, *cursor, *brush;
  const char *menu, *name;
};
struct message {
  void *hwnd;
  DWORD message;
  uintptr_t wparam;
  intptr_t lparam;
  DWORD time;
  POINT point;
};
IMPORT uint16_t API RegisterClassA(const struct window_class *);
IMPORT uintptr_t API CreateWindowExA(DWORD, const char *, const char *, DWORD,
                                     int, int, int, int, uintptr_t, void *,
                                     void *, void *);
IMPORT intptr_t API DefWindowProcA(uintptr_t, DWORD, uintptr_t, intptr_t);
IMPORT BOOL API DestroyWindow(uintptr_t);
IMPORT BOOL API PeekMessageA(struct message *, uintptr_t, DWORD, DWORD, DWORD);
IMPORT intptr_t API DispatchMessageA(const struct message *);
IMPORT void API Sleep(DWORD);
IMPORT void API ExitProcess(DWORD);
static DWORD deactivations, activations;
static intptr_t API procedure(uintptr_t hwnd, DWORD message, uintptr_t wparam,
                              intptr_t lparam) {
  if (message == 28) {
    if (wparam)
      activations++;
    else
      deactivations++;
  }
  return DefWindowProcA(hwnd, message, wparam, lparam);
}
#define METHOD(o, n, type) ((type)((o)->vt[n]))
#define CHECK(e)                                                               \
  do {                                                                         \
    if (!(e))                                                                  \
      ExitProcess(__LINE__);                                                   \
  } while (0)
static void release(DD *o) {
  if (o)
    METHOD(o, 2, DWORD(API *)(DD *))(o);
}
static void mode(DD *d, uintptr_t hwnd, DWORD width, DWORD height, DWORD bpp) {
  CHECK(METHOD(d, 20, DDRESULT(API *)(DD *, uintptr_t, DWORD))(
            d, hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN) == 0);
  CHECK(METHOD(d, 21, DDRESULT(API *)(DD *, DWORD, DWORD, DWORD))(
            d, width, height, bpp) == 0);
  DDDESC desc = {
      .size = sizeof(desc), .flags = DDSD_CAPS, .caps = DDSCAPS_PRIMARYSURFACE};
  DD *primary = 0;
  CHECK(METHOD(d, 6, DDRESULT(API *)(DD *, const DDDESC *, DD **, void *))(
            d, &desc, &primary, 0) == 0);
  DD *palette = 0;
  if (bpp == 8) {
    PALETTEENTRY colors[256];
    for (DWORD i = 0; i < 256; i++)
      colors[i] = (PALETTEENTRY){i, i, 255 - i, 0};
    CHECK(METHOD(d, 5,
                 DDRESULT(API *)(DD *, DWORD, PALETTEENTRY *, DD **, void *))(
              d, DDPCAPS_8BIT | DDPCAPS_ALLOW256, colors, &palette, 0) == 0);
    CHECK(METHOD(primary, 31, DDRESULT(API *)(DD *, DD *))(primary, palette) ==
          0);
  }
  DDDESC locked = {.size = sizeof(locked)};
  CHECK(METHOD(primary, 25,
               DDRESULT(API *)(DD *, RECT *, DDDESC *, DWORD, void *))(
            primary, 0, &locked, DDLOCK_WAIT, 0) == 0);
  for (DWORD y = 0; y < height; y++)
    for (DWORD x = 0; x < width; x++)
      for (DWORD k = 0; k < bpp / 8; k++)
        ((uint8_t *)locked.pixels)[y * locked.pitch + x * (bpp / 8) + k] =
            (uint8_t)(x + y + k);
  CHECK(METHOD(primary, 32, DDRESULT(API *)(DD *, void *))(primary, 0) == 0);
  HDC dc = 0;
  CHECK(METHOD(primary, 17, DDRESULT(API *)(DD *, HDC *))(primary, &dc) == 0);
  CHECK(SetPixel(dc, 4, 5, RGB(255, 255, 255)) != CLR_INVALID);
  CHECK(METHOD(primary, 26, DDRESULT(API *)(DD *, HDC))(primary, dc) == 0);
  /* The lead drives ten Alt+Tab cycles during the first 640x480x8 scene.
   * Polling executes USER activation and waits for a regrant before Restore. */
  if (width == 640 && bpp == 8) {
    DWORD restored = 0, spins = 0;
    while (restored < 10 && spins++ < 60000) {
      struct message m;
      while (PeekMessageA(&m, 0, 0, 0, 1))
        DispatchMessageA(&m);
      DDRESULT lost = METHOD(primary, 24, DDRESULT(API *)(DD *))(primary);
      if (lost == DDERR_SURFACELOST &&
          METHOD(primary, 27, DDRESULT(API *)(DD *))(primary) == 0)
        restored++;
      Sleep(1);
    }
    CHECK(restored == 10 && deactivations >= 10 && activations >= 10);
  }
  release(palette);
  release(primary);
  CHECK(METHOD(d, 19, DDRESULT(API *)(DD *))(d) == 0);
}
void API entry(void) {
  struct window_class c = {.procedure = procedure,
                           .name = "CiukiDDrawExercise"};
  CHECK(RegisterClassA(&c));
  uintptr_t hwnd = CreateWindowExA(0, c.name, "CiukiOS DirectDraw exercise",
                                   0x10cf0000, 32, 32, 640, 480, 0, 0, 0, 0);
  CHECK(hwnd);
  DD *d = 0;
  CHECK(DirectDrawCreate(0, &d, 0) == 0);
  mode(d, hwnd, 640, 480, 8);
  mode(d, hwnd, 320, 200, 8);
  mode(d, hwnd, 640, 480, 16);
  mode(d, hwnd, 640, 480, 32);
  CHECK(METHOD(d, 20, DDRESULT(API *)(DD *, uintptr_t, DWORD))(
            d, hwnd, DDSCL_NORMAL) == 0);
  DD *primary = 0, *clipper = 0;
  DDDESC desc = {
      .size = sizeof(desc), .flags = DDSD_CAPS, .caps = DDSCAPS_PRIMARYSURFACE};
  CHECK(METHOD(d, 6, DDRESULT(API *)(DD *, const DDDESC *, DD **, void *))(
            d, &desc, &primary, 0) == 0);
  CHECK(DirectDrawCreateClipper(0, &clipper, 0) == 0);
  CHECK(METHOD(clipper, 8, DDRESULT(API *)(DD *, DWORD, uintptr_t))(clipper, 0,
                                                                    hwnd) == 0);
  CHECK(METHOD(primary, 28, DDRESULT(API *)(DD *, DD *))(primary, clipper) ==
        0);
  DDFX fill = {.size = sizeof(fill), .fill = 0x375564};
  CHECK(METHOD(primary, 5,
               DDRESULT(API *)(DD *, RECT *, DD *, RECT *, DWORD, DDFX *))(
            primary, 0, 0, 0, DDBLT_COLORFILL | DDBLT_WAIT, &fill) == 0);
  release(clipper);
  release(primary);
  release(d);
  DestroyWindow(hwnd);
  ExitProcess(0);
}
