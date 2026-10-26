/* Production GL/GDI scenes, lifetime and independent fragment oracles.
 * SPDX-License-Identifier: MIT */
#include "../../../../sdk/libgl/software/extensions.h"
#include "../../../../sdk/libgl/software/renderer.h"
#include "../../../../src/probes/win32_gl/scenes.h"
#include "../gdi32/support.h"
#include "aliases.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <w32/opengl32.h>
int w32_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static const char *const extensions[] = {
#include "extensions.inc"
};
void gl_amendment_tests(HDC);
void gl_review_tests(HDC);
void gl_fixture_tests(void);
void gl_quake3_benchmark(unsigned, int);
void gl_quake3_coverage(HDC);
void gl_raster_tests(void);
void gl_amendment2_tests(const char *);
void gl_hardware_enable(void);
void gl_hardware_tests(void);
void gl_hardware_stats(void);
static HDC dc;
static HGLRC rc;
static unsigned pixels[W32_GL_WIDTH * W32_GL_HEIGHT];
static void fragment_oracles(void) {
  uint32_t color[16] = {0};
  uint16_t depth[16];
  for (int i = 0; i < 16; i++)
    depth[i] = 65535;
  struct sw_state s = {.width = 4,
                       .height = 4,
                       .color = color,
                       .depth = depth,
                       .depth_write = 1,
                       .depth_test = 1,
                       .depth_func = GL_LESS,
                       .color_mask = 0xffffffff};
  struct sw_vertex a = {.x = 0,
                        .y = 0,
                        .z = 100,
                        .iw = 1048576,
                        .color = {255, 0, 0, 255}},
                   b = a, c = a, d = a;
  b.x = 64;
  c.x = c.y = 64;
  d.y = 64;
  sw_triangle(&s, &a, &b, &c);
  for (int k = 0; k < 4; k++)
    a.color[k] = b.color[k] = c.color[k] = d.color[k] =
        k == 1 || k == 3 ? 255 : 0;
  sw_triangle(&s, &a, &c, &d);
  for (int i = 0; i < 16; i++)
    assert((color[i] & 0xffffff) == 0xff0000 ||
           (color[i] & 0xffffff) == 0x00ff00);
  unsigned covered = 0;
  for (int i = 0; i < 16; i++)
    if (depth[i] == 100)
      covered++;
  assert(covered == 16);
  int blue[4] = {0, 0, 255, 255};
  sw_fragment(&s, 0, 0, 200, blue);
  assert((color[12] & 0xffffff) != 255);
  s.depth_write = 0;
  sw_fragment(&s, 0, 0, 50, blue);
  assert(depth[12] == 100);
  assert((color[12] & 0xffffff) == 255);
  s.alpha_test = 1;
  s.alpha_func = GL_GREATER;
  s.alpha_ref = 127;
  int transparent[4] = {255, 255, 255, 0};
  sw_fragment(&s, 0, 0, 0, transparent);
  assert((color[12] & 0xffffff) == 255);
  s.alpha_test = 0;
  s.scissor = 1;
  s.box[0] = 2;
  s.box[1] = 2;
  s.box[2] = s.box[3] = 1;
  sw_fragment(&s, 0, 0, 0, transparent);
  assert((color[12] & 0xffffff) == 255);
  s.scissor = 0;
  s.depth_test = 0;
  s.blend = 1;
  s.sfactor = GL_SRC_ALPHA;
  s.dfactor = GL_ONE_MINUS_SRC_ALPHA;
  int red[4] = {255, 0, 0, 128};
  sw_fragment(&s, 0, 0, 0, red);
  assert((color[12] & 0xffffff) == 0x80007f);
  uint8_t dxt1[8] = {0x00, 0xf8, 0xe0, 0x07, 0, 0, 0, 0};
  uint32_t decoded[16];
  assert(sw_s3tc(decoded, 4, 4, 0x83f0, dxt1, 8));
  for (int i = 0; i < 16; i++)
    assert(decoded[i] == 0xffff0000);
  assert(!sw_s3tc(decoded, 4, 4, 0x83f0, dxt1, 7));
  uint8_t dxt3[16] = {0};
  dxt3[8] = 0x1f;
  dxt3[9] = 0;
  dxt3[10] = 0xff;
  dxt3[11] = 0xff;
  assert(sw_s3tc(decoded, 4, 4, 0x83f2, dxt3, 16));
  assert(decoded[0] == 0x000000ff);
  uint8_t dxt5[16] = {255, 0, 0, 0, 0, 0, 0, 0, 0x1f, 0, 0xff, 0xff};
  assert(sw_s3tc(decoded, 4, 4, 0x83f3, dxt5, 16));
  assert(decoded[0] == 0xff0000ff);
  puts("PASS integer fragment oracles: shared edges, D16 order/mask, alpha, "
       "scissor, blend, DXT1/3/5");
}
static void pixel_formats(void) {
  PIXELFORMATDESCRIPTOR p;
  assert(sizeof(p) == 40);
  static const int depths[] = {16,16,16,16,24,24,16,16,24,24};
  for (int n = 1; n <= 10; n++) {
    assert(DescribePixelFormat(dc, n, 40, &p) == 10);
    assert(!(p.dwFlags & PFD_GENERIC_FORMAT));
    assert(p.cDepthBits == depths[n-1] && p.cStencilBits == (n >= 7 ? 8 : 0));
    assert(!p.cAccumBits && !p.cAlphaBits);
    assert(p.cColorBits == (n & 1 ? 32 : 16));
    assert(p.cRedBits == (n & 1 ? 8 : 5));
    assert(p.cGreenBits == (n & 1 ? 8 : 6));
    assert(p.cBlueBits == (n & 1 ? 8 : 5));
    assert(p.cRedShift == (n & 1 ? 16 : 11));
    assert(p.cGreenShift == (n & 1 ? 8 : 5) && !p.cBlueShift);
    int doublebuffer = n != 3 && n != 4;
    assert(!!(p.dwFlags & PFD_DOUBLEBUFFER) == doublebuffer);
    assert(!!(p.dwFlags & PFD_SWAP_COPY) == doublebuffer);
    assert(ChoosePixelFormat(dc, &p) == n);
    unsigned pixels[4] = {0};
    struct w32_dib dib = test_dib(pixels, 2, 2, 32, 0, 1);
    HDC target = w32_gdi_surface_dc(&dib, (POINT){0}, 0, 0);
    assert(target && SetPixelFormat(target, n, &p));
    assert(GetPixelFormat(target) == n && !SetPixelFormat(target, n, &p));
    HGLRC context = wglCreateContext(target);
    assert(context && wglMakeCurrent(target, context));
    int value;
    glGetIntegerv(GL_DEPTH_BITS, &value);
    assert(value == p.cDepthBits);
    glGetIntegerv(GL_STENCIL_BITS, &value);
    assert(value == p.cStencilBits);
    glGetIntegerv(GL_DOUBLEBUFFER, &value);
    assert(value == doublebuffer);
    glGetIntegerv(GL_ALPHA_BITS, &value);
    assert(value == 0);
    glClearColor(0.3f, 0.5f, 0.7f, 0.2f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    unsigned char rgba[4];
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    assert(rgba[0] == (n & 1 ? 76 : 74));
    assert(rgba[1] == (n & 1 ? 127 : 125));
    assert(rgba[2] == (n & 1 ? 178 : 181) && rgba[3] == 255);
    assert(glGetError() == GL_NO_ERROR);
    assert(wglMakeCurrent(0, 0) && wglDeleteContext(context));
    assert(DeleteDC(target));
  }
  assert(!DescribePixelFormat(dc, 0, 40, &p));
  assert(!DescribePixelFormat(dc, 11, 40, &p));
  assert(!DescribePixelFormat(dc, 1, 39, &p));
  p = (PIXELFORMATDESCRIPTOR){.nSize = 40,
                              .nVersion = 1,
                              .dwFlags = PFD_DOUBLEBUFFER | PFD_SUPPORT_OPENGL,
                              .cColorBits = 24};
  assert(ChoosePixelFormat(dc, &p) == 1);
  p.cColorBits = 32; p.cDepthBits = 24; p.cStencilBits = 8;
  assert(ChoosePixelFormat(dc, &p) == 9);
  p.cColorBits = 16;
  assert(ChoosePixelFormat(dc, &p) == 10);
  p.cDepthBits = 16;
  assert(ChoosePixelFormat(dc, &p) == 8);
  p.cStencilBits = 0;
  assert(ChoosePixelFormat(dc, &p) == 2);
  p.dwFlags = PFD_STEREO;
  assert(!ChoosePixelFormat(dc, &p));
  p.dwFlags = PFD_SUPPORT_GDI;
  assert(!ChoosePixelFormat(dc, &p));
  p.dwFlags = PFD_DOUBLEBUFFER | PFD_SUPPORT_OPENGL;
  assert(SetPixelFormat(dc, 1, &p));
  assert(!SetPixelFormat(dc, 2, &p));
  assert(GetPixelFormat(dc) == 1);
}
static void *other_thread(void *unused) {
  (void)unused;
  assert(!wglGetCurrentContext());
  assert(!wglGetProcAddress("glActiveTextureARB"));
  assert(!wglMakeCurrent(dc, rc));
  assert(!wglDeleteContext(rc));
  return 0;
}
static void lifecycle(void) {
  rc = wglCreateContext(dc);
  assert(rc);
  assert(wglMakeCurrent(dc, rc));
  assert(wglGetCurrentContext() == rc && wglGetCurrentDC() == dc);
  pthread_t t;
  assert(!pthread_create(&t, 0, other_thread, 0));
  pthread_join(t, 0);
  for (unsigned i = 0; i < sizeof(extensions) / sizeof(*extensions); i++)
    assert(wglGetProcAddress(extensions[i]));
  assert(!wglGetProcAddress("glBogus"));
  assert(!wglGetProcAddress("glBegin"));
  const char *s = (const char *)glGetString(GL_EXTENSIONS);
  assert(!strcmp(
      s,
      "GL_ARB_multitexture GL_EXT_texture_env_add GL_ARB_texture_env_combine "
      "GL_EXT_compiled_vertex_array GL_EXT_bgra GL_EXT_texture_edge_clamp "
      "GL_ARB_texture_compression GL_EXT_texture_compression_s3tc"));
  unsigned list = glGenLists(1);
  assert(list);
  glNewList(list, GL_COMPILE);
  glColor3f(0.2f, 0.4f, 0.6f);
  glEndList();
  HGLRC shared = wglCreateContext(dc);
  assert(shared && wglShareLists(rc, shared));
  assert(wglMakeCurrent(dc, shared));
  assert(glIsList(list));
  glCallList(list);
  float current[4];
  glGetFloatv(GL_CURRENT_COLOR, current);
  assert(current[0] == 0.2f && current[2] == 0.6f);
  glDeleteLists(list, 1);
  assert(wglMakeCurrent(dc, rc));
  assert(!glIsList(list));
  assert(wglDeleteContext(shared));
  assert(!wglMakeCurrent(dc, 0xdeadbeef));
  assert(!wglGetCurrentContext());
  assert(wglMakeCurrent(dc, rc));
  puts("PASS pixel formats, context ownership, invalid/stale handles, list "
       "sharing, complete extension lookup");
}
static int font_metrics(void *opaque, unsigned em,
                        struct w32_gdi_font_metrics *m) {
  (void)opaque;
  (void)em;
  *m = (struct w32_gdi_font_metrics){8 * 64, 0, 0, 8 * 64};
  return 1;
}
static int font_raster(void *opaque, unsigned em, DWORD cp, int aa,
                       struct w32_gdi_glyph *g) {
  (void)opaque;
  (void)em;
  (void)cp;
  (void)aa;
  uint8_t *p = malloc(64);
  assert(p);
  memset(p, 255, 64);
  *g = (struct w32_gdi_glyph){8, 8, 8, 0, 8, 8 * 64, p};
  return 1;
}
static void font_free(void *opaque, struct w32_gdi_glyph *g) {
  (void)opaque;
  free(g->coverage);
}
static void fonts(void) {
  struct w32_gdi_font_provider p = {
      .metrics = font_metrics, .raster = font_raster, .free_glyph = font_free};
  HFONT font = w32_gdi_create_font(&p, 0, 8 * 64);
  assert(font);
  HGDIOBJ old = SelectObject(dc, font);
  unsigned base = glGenLists(2);
  assert(base);
  assert(wglUseFontBitmapsA(dc, 65, 2, base));
  assert(glIsList(base));
  glDisable(GL_DEPTH_TEST);
  glRasterPos2f(0, 0);
  glCallList(base);
  assert(!glGetError());
  glDeleteLists(base, 2);
  SelectObject(dc, old);
  DeleteObject(font);
  puts("PASS font-provider glyphs converted to owned bitmap display lists");
}
static void upload(void) {
  unsigned t;
  glGenTextures(1, &t);
  assert(t);
  glBindTexture(GL_TEXTURE_2D, t);
  uint8_t block[8] = {0x00, 0xf8, 0xe0, 0x07, 0, 0, 0, 0}, copy[8] = {0};
  glCompressedTexImage2DARB(GL_TEXTURE_2D, 0, 0x83f0, 4, 4, 0, 8, block);
  assert(!glGetError());
  glGetCompressedTexImageARB(GL_TEXTURE_2D, 0, copy);
  assert(!memcmp(block, copy, 8));
  int n;
  glGetTexLevelParameteriv(GL_TEXTURE_2D, 0,
                           GL_TEXTURE_COMPRESSED_IMAGE_SIZE_ARB, &n);
  assert(n == 8);
  unsigned texels[16];
  glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, texels);
  assert(texels[0] == 0xffff0000);
  glDeleteTextures(1, &t);
  assert(!glIsTexture(t));
  assert(glGetError() == 0);
  puts("PASS compressed upload/readback, extension query and deletion");
}
static uint64_t now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
int main(int argc, char **argv) {
  setvbuf(stdout,0,_IONBF,0); /* Keep evidence even if a sanitizer exit scan fails. */
  if (argc>1 && !strcmp(argv[1],"--census")) {
    extern void gl_census_tests(void); gl_census_tests(); test_reset(); return 0;
  }
  extern void gl_kernel_replay(void);
  if (argc > 1 && !strcmp(argv[1], "--kernel-transport")) { gl_kernel_replay(); test_reset(); return 0; }
  if (argc > 1 && !strcmp(argv[1], "--hardware")) {
    gl_hardware_tests(); test_reset(); return 0;
  }
  if (argc > 1 && (!strcmp(argv[1], "--hardware-quake3") ||
                   !strcmp(argv[1], "--hardware-quake3-default"))) {
    gl_hardware_enable(); gl_quake3_benchmark(3,!strcmp(argv[1], "--hardware-quake3-default"));
    gl_hardware_stats();
    if (!strcmp(argv[1],"--hardware-quake3")) { extern void gl_quake3_savage(void); gl_quake3_savage(); }
    test_reset(); return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "--amendment2")) {
    gl_amendment2_tests(argc > 2 ? argv[2] : 0);
    test_reset();
    return 0;
  }
  int benchmark = argc > 1 && !strcmp(argv[1], "--benchmark");
  if (argc > 1 && (!strcmp(argv[1], "--quake3") ||
                  !strcmp(argv[1], "--quake3-default"))) {
    gl_quake3_benchmark(3, !strcmp(argv[1], "--quake3-default"));
    test_reset();
    return 0;
  }
  gl_amendment2_tests(0);
  fragment_oracles();
  gl_raster_tests();
  struct w32_dib dib = test_dib(pixels, W32_GL_WIDTH, W32_GL_HEIGHT, 32, 0, 1);
  dc = w32_gdi_surface_dc(&dib, (POINT){0}, 0, 0);
  assert(dc);
  pixel_formats();
  lifecycle();
  gl_quake3_coverage(dc);
  upload();
  gl_amendment_tests(dc);
  gl_review_tests(dc);
  for (unsigned scene = 0; scene < W32_GL_SCENES; scene++) {
    uint64_t start = now();
    unsigned count = benchmark ? 10 : 1;
    for (unsigned i = 0; i < count; i++)
      w32_gl_scene(scene);
    uint64_t elapsed = now() - start;
    assert(glGetError() == 0);
    uint32_t checksum = w32_gl_checksum();
    assert(checksum);
    printf("GL_SCENE scene=%u checksum=%u frame_us=%llu frames=%u\n", scene,
           checksum, (unsigned long long)(elapsed / count / 1000), count);
    assert(SwapBuffers(dc));
  }
  fonts();
  gl_fixture_tests();
  assert(wglMakeCurrent(dc,rc));
  assert(wglMakeCurrent(0, 0));
  assert(wglDeleteContext(rc));
  assert(!wglDeleteContext(rc));
  assert(DeleteDC(dc));
  test_reset();
  puts("PASS GL scenes and zero tracked allocations at teardown");
  return 0;
}
