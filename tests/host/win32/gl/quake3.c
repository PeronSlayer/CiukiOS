/* Synthesized, deterministic Q3-style command stream; no game assets/code.
 * SPDX-License-Identifier: MIT */
#include "../../../../sdk/libgl/software/profile.h"
#include "../gdi32/support.h"
#include "aliases.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <w32/opengl32.h>

#define QUADS 800
#ifdef SW_PROFILE
static uint64_t work[SW_WORK_COUNT];
void sw_profile_count(enum sw_work key, uint64_t count) { work[key] += count; }
static void model(unsigned w, unsigned h, unsigned passes, unsigned frames,
                   unsigned depth_bits) {
  /* Explicit analytic assumptions, not measured PIII instruction latencies.
   * 64-bit division uses the i686 scalar helper, not a host x86-64 DIV. */
  static const unsigned cycles[SW_WORK_COUNT] = {
      700, 60, 120, 12, 16, 0, 25, 160, 120, 160, 12, 40, 60, 8, 220};
  static const char *names[SW_WORK_COUNT] = {
      "triangle", "span",         "attribute_setup", "attribute_step",
      "covered",  "early_reject", "shaded",          "perspective_div",
      "lod",      "wide_div",     "texel",           "stage",
      "fragment", "write",        "vertex"};
  double cpu = 0;
  for (unsigned i = 0; i < SW_WORK_COUNT; i++)
    cpu += (double)work[i] * cycles[i] / frames;
  /* A 300 MiB/s streaming-memory budget; texture working set fits 256 KiB L2.
   * Count clear, covered depth reads and destination read/write conservatively.
   */
  unsigned depth_bytes = depth_bits == 24 ? 4 : 2;
  double bytes = (double)w * h * (4 + depth_bytes) +
                 ((double)work[SW_COVERED] * depth_bytes +
                  work[SW_WRITE] * 8) / frames;
  double memory = bytes * 933000000 / (300 * 1024 * 1024);
  double total = cpu + memory;
  printf("GL_Q3_MODEL width=%u height=%u passes=%u cycles=%.0f fps=%.3f "
         "low_fps=%.3f high_fps=%.3f",
         w, h, passes, total, 933000000 / total,
         933000000 / (cpu * 2 + memory * 2),
         933000000 / (cpu * 0.5 + memory * 0.5));
  for (unsigned i = 0; i < SW_WORK_COUNT; i++)
    printf(" %s=%llu", names[i], (unsigned long long)(work[i] / frames));
  printf(" depth=%u stencil=%u", depth_bits, depth_bits == 24 ? 8 : 0);
  putchar('\n');
}
#endif
static float xyz[QUADS * 4][4], uv[QUADS * 4][4];
static unsigned char colors[QUADS * 4][4];
static unsigned indices[QUADS * 6];
int W32API wglChoosePixelFormat(HDC, const PIXELFORMATDESCRIPTOR *);
int W32API wglDescribePixelFormat(HDC, int, unsigned, PIXELFORMATDESCRIPTOR *);
int W32API wglGetPixelFormat(HDC);
BOOL W32API wglSetPixelFormat(HDC, int, const PIXELFORMATDESCRIPTOR *);
BOOL W32API wglSwapBuffers(HDC);
static uint64_t clock_ns(void) {
  struct timespec t;
  assert(!clock_gettime(CLOCK_MONOTONIC, &t));
  return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int savage_frames;
static void mesh(unsigned count, unsigned columns, float z, float opacity,
                 int perspective) {
  unsigned rows = (count + columns - 1) / columns;
  for (unsigned i = 0; i < count; i++) {
    float x = -1 + 2.0f * (i % columns) / columns;
    float y = -1 + 2.0f * (i / columns) / rows;
    for (unsigned k = 0; k < 4; k++) {
      unsigned at = i * 4 + k;
      float w = perspective ? 1 + 0.25f * (float)(k / 2) : 1;
      xyz[at][0] = (x + ((k == 1 || k == 2) ? 2.0f / columns : 0)) * w;
      xyz[at][1] = (y + ((k >= 2) ? 2.0f / rows : 0)) * w;
      /* Leave sky and world visible; the HUD occupies only the bottom band. */
      if (count == 800)
        xyz[at][1] = (xyz[at][1] / w * 0.8f - 0.2f) * w;
      if (count == 300) {
        xyz[at][0] *= 0.6f;
        xyz[at][1] = (xyz[at][1] / w * 0.3f - 0.2f) * w;
      }
      if (count == 100) {
        xyz[at][0] *= 0.8f;
        xyz[at][1] = (xyz[at][1] / w * 0.4f - 0.2f) * w;
      }
      if (count == 199)
        xyz[at][1] = (xyz[at][1] / w * 0.12f - 0.88f) * w;
      xyz[at][2] = z * w;
      xyz[at][3] = w;
      uv[at][0] = (k == 1 || k == 2) ? 2 : 0;
      uv[at][1] = k >= 2 ? 2 : 0;
      uv[at][2] = uv[at][0] * 0.5f;
      uv[at][3] = uv[at][1] * 0.5f;
      if (savage_frames) {
        uv[at][2]=(0.5f+uv[at][2]*127)/128;
        uv[at][3]=(0.5f+uv[at][3]*127)/128;
      }
      colors[at][0] = 160 + i % 96;
      colors[at][1] = 192 + i % 64;
      colors[at][2] = 224 + i % 32;
      colors[at][3] = (unsigned char)(opacity * 255);
    }
    static const unsigned order[6] = {0, 1, 2, 0, 2, 3};
    for (unsigned k = 0; k < 6; k++)
      indices[i * 6 + k] = i * 4 + order[k];
  }
  glVertexPointer(4, GL_FLOAT, 16, xyz);
  glColorPointer(4, GL_UNSIGNED_BYTE, 4, colors);
  glClientActiveTextureARB(GL_TEXTURE0_ARB);
  glTexCoordPointer(2, GL_FLOAT, 16, uv);
  glClientActiveTextureARB((GL_TEXTURE0_ARB + 1));
  glTexCoordPointer(2, GL_FLOAT, 16, &uv[0][2]);
  glClientActiveTextureARB(GL_TEXTURE0_ARB);
}
void gl_savage_class(unsigned);
static void draw(unsigned n) {
  gl_savage_class(n==800 ? 1 : n==300 ? 2 : 0);
  glLockArraysEXT(0, n * 4);
  glDrawElements(GL_TRIANGLES, n * 6, GL_UNSIGNED_INT, indices);
  glUnlockArraysEXT();
}
static void texture(unsigned name, unsigned size) {
  unsigned char data[256 * 256 * 4];
  glBindTexture(GL_TEXTURE_2D, name);
  for (unsigned level = 0;; level++, size /= 2) {
    for (unsigned i = 0; i < size * size; i++) {
      data[i * 4] = 64 + (i * 13 + name) % 192;
      data[i * 4 + 1] = 64 + (i * 7 + name) % 192;
      data[i * 4 + 2] = 64 + (i * 3 + name) % 192;
      data[i * 4 + 3] = (i % 5) ? 255 : 0;
    }
    glTexImage2D(GL_TEXTURE_2D, level, savage_frames && name==1025 ? GL_RGB8 : GL_RGBA8, size, size, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, data);
    if (size == 1)
      break;
  }
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_LINEAR_MIPMAP_NEAREST);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  if (savage_frames && name==1025) {
    glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP);
    glTexParameterf(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP);
  }
}
static void frame(int multipass) {
  glDepthMask(GL_TRUE);
  glDepthFunc(GL_LEQUAL);
  glDisable(GL_BLEND);
  glDisable(GL_ALPHA_TEST);
  glEnable(GL_DEPTH_TEST);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glActiveTextureARB((GL_TEXTURE0_ARB + 1));
  glDisable(GL_TEXTURE_2D);
  glActiveTextureARB(GL_TEXTURE0_ARB);
  glEnable(GL_TEXTURE_2D);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  mesh(1, 1, 0.95f, 1, 0);
  draw(1); /* sky: 2 triangles */
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glActiveTextureARB((GL_TEXTURE0_ARB + 1));
  if (multipass<=0)
    glEnable(GL_TEXTURE_2D);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
  glActiveTextureARB(GL_TEXTURE0_ARB);
  mesh(800, 40, 0.5f, 1, 1);
  draw(800); /* lightmapped world: 1600 */
  if (multipass>0) {
    glBindTexture(GL_TEXTURE_2D, 1025);
    glEnable(GL_BLEND);
    glBlendFunc(GL_DST_COLOR, GL_ZERO);
    glDepthFunc(GL_EQUAL);
    if (savage_frames) glTexCoordPointer(2,GL_FLOAT,16,&uv[0][2]);
    draw(800);
    if (savage_frames) glTexCoordPointer(2,GL_FLOAT,16,uv);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 1024);
  }
  glActiveTextureARB((GL_TEXTURE0_ARB + 1));
  glDisable(GL_TEXTURE_2D);
  glActiveTextureARB(GL_TEXTURE0_ARB);
  mesh(300, 30, 0.25f, 1, 1);
  draw(300); /* models: 600 */
  if (multipass<0) return; /* Qualified H1 prefix, reported separately. */
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GEQUAL, 128.0f / 255);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  mesh(100, 20, 0, 0.75f, 1);
  draw(100); /* cutouts: 200 */
  glAlphaFunc(GL_GREATER, 0);
  glBlendFunc(GL_ONE, GL_ONE);
  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(-1, -1);
  mesh(100, 20, 0, 1, 1);
  draw(100); /* additive effects: 200 */
  glDisable(GL_POLYGON_OFFSET_FILL);
  glDisable(GL_ALPHA_TEST);
  glDisable(GL_DEPTH_TEST);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  mesh(199, 40, 0, 0.5f, 0);
  draw(199); /* 2D HUD: 398 */
  assert(glGetError() == GL_NO_ERROR);
}
static unsigned checksum(unsigned w, unsigned h) {
  unsigned char *p = malloc(w * h * 4);
  assert(p);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, p);
  unsigned hash = 2166136261u;
  for (unsigned i = 0; i < w * h * 4; i++)
    hash = (hash ^ p[i]) * 16777619u;
  free(p);
  return hash;
}
static void quad(void) {
  glBegin(GL_QUADS);
  glVertex2f(-1, -1);
  glVertex2f(1, -1);
  glVertex2f(1, 1);
  glVertex2f(-1, 1);
  glEnd();
}
static unsigned read_pixel(unsigned buffer) {
  unsigned char p[4];
  glReadBuffer(buffer);
  glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
  return (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2];
}
static unsigned read_stencil(void) {
  unsigned char p[4] = {0};
  glReadPixels(8, 8, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, p);
  assert(glGetError() == GL_NO_ERROR);
  return p[0];
}
static void extended_coverage(void) {
  unsigned pixels[16 * 16] = {0};
  struct w32_dib dib = test_dib(pixels, 16, 16, 32, 0, 1);
  HDC dc = w32_gdi_surface_dc(&dib, (POINT){0}, 0, 0);
  PIXELFORMATDESCRIPTOR p = {.nSize = 40, .nVersion = 1,
      .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
      .cColorBits = 32, .cDepthBits = 24, .cStencilBits = 8};
  int selected = ChoosePixelFormat(dc, &p);
  assert(selected == 9 && SetPixelFormat(dc, selected, &p));
  assert(DescribePixelFormat(dc, selected, sizeof(p), &p) == 10);
  assert(!(p.dwFlags & PFD_GENERIC_FORMAT));
  long allocations = test_allocations, bytes = test_bytes;
  for (int fail = 0; fail < 12; fail++) {
    test_fail_after = fail;
    HGLRC attempt = wglCreateContext(dc);
    test_fail_after = -1;
    if (attempt)
      assert(wglDeleteContext(attempt));
    assert(test_allocations == allocations && test_bytes == bytes);
  }
  HGLRC rc = wglCreateContext(dc);
  assert(rc && wglMakeCurrent(dc, rc));
  int bits = 0;
  glGetIntegerv(GL_DEPTH_BITS, &bits);
  assert(bits == 24);
  glGetIntegerv(GL_STENCIL_BITS, &bits);
  assert(bits == 8);
  glViewport(0, 0, 16, 16);
  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, 16, 16);
  glStencilMask(15);
  glClearStencil(0x37);
  glClear(GL_STENCIL_BUFFER_BIT | GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  assert(read_stencil() == 7);
  glStencilMask(255);
  glEnable(GL_STENCIL_TEST);
  glStencilFunc(GL_ALWAYS, 0xa5, 255);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  quad();
  assert(read_stencil() == 0xa5);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GREATER, 1);
  glStencilOp(GL_INCR, GL_INCR, GL_INCR);
  quad();
  assert(read_stencil() == 0xa5);
  glDisable(GL_ALPHA_TEST);
  glStencilFunc(GL_NEVER, 0, 255);
  glStencilOp(GL_INCR, GL_KEEP, GL_KEEP);
  quad();
  assert(read_stencil() == 0xa6);
  glStencilFunc(GL_ALWAYS, 42, 255);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_NEVER);
  glStencilOp(GL_KEEP, GL_DECR, GL_REPLACE);
  quad();
  assert(read_stencil() == 0xa5);
  glDepthFunc(GL_ALWAYS);
  quad();
  assert(read_stencil() == 42);
  glDepthFunc(GL_EQUAL);
  glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
  quad();
  assert(read_stencil() == (42 ^ 255));
  glDisable(GL_DEPTH_TEST);
  glClearStencil(255);
  glClear(GL_STENCIL_BUFFER_BIT);
  glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
  quad();
  assert(read_stencil() == 255);
  glClearStencil(0);
  glClear(GL_STENCIL_BUFFER_BIT);
  glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
  quad();
  assert(read_stencil() == 0);
  unsigned list = glGenLists(1);
  glNewList(list, GL_COMPILE);
  glStencilFunc(GL_ALWAYS, 19, 255);
  glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
  glClearStencil(11);
  glEndList();
  glCallList(list);
  glClear(GL_STENCIL_BUFFER_BIT);
  assert(read_stencil() == 11);
  quad();
  assert(read_stencil() == 19);
  unsigned char packed[24];
  memset(packed, 0xa5, sizeof(packed));
  glPixelStorei(GL_PACK_ALIGNMENT, 8);
  glPixelStorei(GL_PACK_ROW_LENGTH, 4);
  glPixelStorei(GL_PACK_SKIP_ROWS, 1);
  glPixelStorei(GL_PACK_SKIP_PIXELS, 1);
  glReadPixels(8, 8, 2, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, packed);
  for (unsigned i = 0; i < sizeof(packed); i++)
    assert(packed[i] == (i == 9 || i == 10 ? 19 : 0xa5));
  glReadPixels(0x7fffffff, 8, 2, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, packed);
  assert(glGetError() == GL_NO_ERROR);
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glPixelStorei(GL_PACK_SKIP_ROWS, 0);
  glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
  glDisable(GL_STENCIL_TEST);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glClear(GL_DEPTH_BUFFER_BIT);
  glColor3f(0, 1, 0);
  glBegin(GL_QUADS);
  glVertex3f(-1, -1, -0.5f);
  glVertex3f(1, -1, -0.5f);
  glVertex3f(1, 1, -0.5f);
  glVertex3f(-1, 1, -0.5f);
  glEnd();
  const unsigned char red[4] = {255, 0, 0, 255};
  glRasterPos3f(0, 0, 0);
  glDrawPixels(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, red);
  assert(read_pixel(GL_BACK) == 0xff00); /* Raster pixels also use D24. */
  assert(wglMakeCurrent(0, 0) && wglDeleteContext(rc));
  assert(DeleteDC(dc));
  puts("PASS production GDI 32/D24/S8: context storage, masked/scissored "
       "clear, stencil readback, alpha/stencil/depth ordering, saturation, "
       "invert and lists");
}
void gl_quake3_coverage(HDC dc) {
  HGLRC old = wglGetCurrentContext(), rc = wglCreateContext(dc);
  assert(rc && wglMakeCurrent(dc, rc));
  PIXELFORMATDESCRIPTOR p = {.nSize = 40,
                             .nVersion = 1,
                             .dwFlags = PFD_DOUBLEBUFFER | PFD_SUPPORT_OPENGL,
                             .cColorBits = 24};
  assert(wglChoosePixelFormat(dc, &p) == GetPixelFormat(dc));
  assert(wglGetPixelFormat(dc) == GetPixelFormat(dc));
  assert(wglDescribePixelFormat(dc, 1, sizeof(p), &p) ==
         DescribePixelFormat(dc, 1, sizeof(p), 0));
  assert(!wglSetPixelFormat(dc, 1, &p));
  glViewport(0, 0, 16, 16);
  glClearDepth(1);
  glCullFace(GL_FRONT);
  glColor4f(1, 1, 1, 1);
  glShadeModel(GL_SMOOTH);
  glDepthFunc(GL_LEQUAL);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glDepthMask(GL_TRUE);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_SCISSOR_TEST);
  glScissor(0, 0, 16, 16);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glEnableClientState(GL_VERTEX_ARRAY);
  assert(glGetError() == GL_NO_ERROR);
  assert(!wglGetProcAddress("wglSwapIntervalEXT"));
  assert(!wglGetProcAddress("wglGetSwapIntervalEXT"));
  assert(!strstr((const char *)glGetString(GL_EXTENSIONS), "GL_S3_s3tc"));
  assert(glGetString(GL_VENDOR) && glGetString(GL_RENDERER) &&
         glGetString(GL_VERSION));
  int stencil_bits = 0;
  glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
  int units = 0;
  glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, &units);
  assert(units >= 2);
  float width = 0;
  glLineWidth(3);
  glGetFloatv(GL_LINE_WIDTH, &width);
  assert(width == 3);
  glPushAttrib(GL_LINE_BIT);
  glLineWidth(1);
  glPopAttrib();
  glGetFloatv(GL_LINE_WIDTH, &width);
  assert(width == 3);
  glLineWidth(0);
  assert(glGetError() == GL_INVALID_VALUE);
  glLineWidth(1);
  glEnable(GL_STENCIL_TEST);
  assert(glIsEnabled(GL_STENCIL_TEST));
  glStencilFunc(GL_ALWAYS, 300, 0xff);
  glStencilMask(0x0f);
  glStencilOp(GL_KEEP, GL_INCR, GL_DECR);
  glClearStencil(0x37);
  glPushAttrib(GL_STENCIL_BUFFER_BIT | GL_ENABLE_BIT);
  glDisable(GL_STENCIL_TEST);
  glStencilFunc(GL_NEVER, 0, 0);
  glStencilMask(0);
  glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO);
  glClearStencil(0);
  glPopAttrib();
  int value = 0;
  glGetIntegerv(GL_STENCIL_REF, &value);
  assert(value == (stencil_bits ? 255 : 0));
  glGetIntegerv(GL_STENCIL_WRITEMASK, &value);
  assert(value == 15);
  glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &value);
  assert(value == GL_DECR);
  glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &value);
  assert(value == (stencil_bits ? 0x37 : 0));
  assert(glIsEnabled(GL_STENCIL_TEST));
  glStencilFunc(0xffff, 0, 0);
  assert(glGetError() == GL_INVALID_ENUM);
  glStencilOp(GL_KEEP, 0xffff, GL_KEEP);
  assert(glGetError() == GL_INVALID_ENUM);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  assert(glGetError() ==
         GL_NO_ERROR); /* Clearing an absent stencil attachment is legal. */
  glDisable(GL_STENCIL_TEST);
  unsigned char no_stencil[4];
  glReadPixels(0, 0, 1, 1, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, no_stencil);
  assert(glGetError() == GL_INVALID_OPERATION);
  glDrawBuffer(GL_FRONT);
  glClearColor(1, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  assert(read_pixel(GL_FRONT) == 0xff0000 && read_pixel(GL_BACK) == 0);
  glDrawBuffer(GL_BACK);
  glClearColor(0, 0, 1, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  assert(read_pixel(GL_FRONT) == 0xff0000 && read_pixel(GL_BACK) == 0xff);
  assert(wglSwapBuffers(dc));
  assert(read_pixel(GL_FRONT) == 0xff && read_pixel(GL_BACK) == 0xff);
  glDrawBuffer(GL_FRONT_AND_BACK);
  glClearColor(0, 1, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  assert(read_pixel(GL_FRONT) == 0xff00 && read_pixel(GL_BACK) == 0xff00);
  glDrawBuffer(GL_NONE);
  glClearColor(1, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  assert(read_pixel(GL_FRONT) == 0xff00 && read_pixel(GL_BACK) == 0xff00);
  glDrawBuffer(GL_BACK);
  glReadBuffer(GL_BACK);
  /* GL 1.1 rounds the reference: Quake's GE 0.5 means byte alpha >=128. */
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisableClientState(GL_VERTEX_ARRAY);
  glEnable(GL_ALPHA_TEST);
  glAlphaFunc(GL_GEQUAL, 0.5f);
  glColor4ub(255, 0, 0, 127);
  quad();
  assert(read_pixel(GL_BACK) == 0);
  glColor4ub(0, 255, 0, 128);
  quad();
  assert(read_pixel(GL_BACK) == 0xff00);
  glDisable(GL_ALPHA_TEST);
  glColor4f(1, 1, 1, 1);
  unsigned pixel = 0xffffffff;
  for (unsigned i = 0; i < 2048; i++) {
    glBindTexture(GL_TEXTURE_2D, 1024 + i);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 &pixel);
  }
  assert(glIsTexture(1024) && glIsTexture(3071));
  assert(glGetError() == GL_NO_ERROR);
  unsigned list = glGenLists(1);
  assert(list);
  float border[4] = {1, 0, 0, 1}, got[4];
  glNewList(list, GL_COMPILE);
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
  glEndList();
  border[0] = 0;
  glCallList(list);
  glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, got);
  assert(got[0] == 1 && got[1] == 0 && got[2] == 0 && got[3] == 1);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glEnable(GL_TEXTURE_2D);
  glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0.5f);
  glVertex2f(-1, -1);
  glVertex2f(1, -1);
  glVertex2f(1, 1);
  glVertex2f(-1, 1);
  glEnd();
  unsigned char rgba[4];
  glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  assert(rgba[0] == 255 && rgba[1] == 128 &&
         rgba[2] == 128); /* half border + half white */
  test_fail_after = 0;
  glBindTexture(GL_TEXTURE_2D, 5000);
  test_fail_after = -1;
  assert(glGetError() == GL_OUT_OF_MEMORY);
  glBindTexture(GL_TEXTURE_2D, 3071);
  assert(glGetError() == GL_NO_ERROR);
  assert(wglMakeCurrent(0, 0) && wglDeleteContext(rc));
  extended_coverage();
  assert(wglMakeCurrent(dc, old));
  puts("PASS Quake defaults, strings/probes, line width, stencil state/list "
       "validation, 2048 live named textures, owned border color and "
       "allocation recovery");
}
void gl_quake3_benchmark(unsigned iterations, int default_format) {
  extern void gl_hardware_sample(unsigned, unsigned, unsigned, unsigned);
  for (unsigned size = 0; size < 2; size++) {
    unsigned w = size ? 640 : 320, h = size ? 480 : 240;
    unsigned *pixels = calloc(w * h, 4);
    assert(pixels);
    struct w32_dib dib = test_dib(pixels, w, h, 32, 0, 1);
    HDC dc = w32_gdi_surface_dc(&dib, (POINT){0}, 0, 0);
    assert(dc);
    PIXELFORMATDESCRIPTOR p = {.nSize = 40,
                               .nVersion = 1,
                               .dwFlags = PFD_DOUBLEBUFFER | PFD_SUPPORT_OPENGL,
                               .cColorBits = 32,
                               .cDepthBits = default_format ? 24 : 16,
                               .cStencilBits = default_format ? 8 : 0};
    assert(SetPixelFormat(dc, ChoosePixelFormat(dc, &p), &p));
    HGLRC rc = wglCreateContext(dc);
    assert(rc && wglMakeCurrent(dc, rc));
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    for (unsigned unit = 0; unit < 2; unit++) {
      glActiveTextureARB(GL_TEXTURE0_ARB + unit);
      texture(1024 + unit, 64 >> unit);
      glClientActiveTextureARB(GL_TEXTURE0_ARB + unit);
      glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glActiveTextureARB(GL_TEXTURE0_ARB);
    glClientActiveTextureARB(GL_TEXTURE0_ARB);
    for (int pass = 0; pass < 2; pass++) {
      frame(pass);
      unsigned expected = checksum(w, h);
#ifdef SW_PROFILE
      memset(work, 0, sizeof(work));
#endif
      uint64_t start = clock_ns();
      for (unsigned i = 0; i < iterations; i++)
        frame(pass);
      uint64_t elapsed = clock_ns() - start;
      assert(checksum(w, h) == expected && glGetError() == GL_NO_ERROR);
      printf("GL_Q3 width=%u height=%u passes=%u triangles=%u frames=%u "
             "checksum=%u ns_frame=%llu ns_pixel=%.3f fps=%.3f depth=%u stencil=%u\n",
             w, h, pass + 1, pass ? 4600 : 3000, iterations, expected,
             (unsigned long long)(elapsed / iterations),
             (double)elapsed / iterations / (w * h),
             1e9 * iterations / (double)elapsed, p.cDepthBits, p.cStencilBits);
#ifdef SW_PROFILE
      model(w, h, pass + 1, iterations, p.cDepthBits);
#endif
      gl_hardware_sample(w,h,pass+1,iterations+1);
    }
    assert(wglMakeCurrent(0, 0) && wglDeleteContext(rc));
    assert(DeleteDC(dc));
    free(pixels);
  }
}

/* Complete H1 frames through production GL/GDI/wire and fake Savage packets.
 * No game assets; both one-pass and two-pass lightmapped worlds. */
void gl_quake3_savage(int default_format) {
  extern void gl_hardware_native(int),gl_kernel_setup(int),gl_kernel_teardown(void);
  extern void gl_kernel_mode(unsigned,unsigned),gl_savage_begin(void);
  extern unsigned gl_kernel_draws(void);
  extern uint64_t gl_kernel_fragments(void);
  extern void gl_savage_end(unsigned,unsigned,unsigned,unsigned,uint64_t,uint64_t,uint64_t);
  gl_hardware_native(1); savage_frames=1;
  for (unsigned size=0;size<2;size++) {
    unsigned w=size ? 1024 : 640,h=size ? 768 : 480;
    gl_kernel_mode(w,h);
#ifdef GL_KERNEL_FAMILY_MACH64
    gl_kernel_setup(2);
#else
    extern void gl_kernel_fullscreen_setup(unsigned,unsigned); gl_kernel_fullscreen_setup(w,h);
#endif
    for (unsigned scenario=0;scenario<2;scenario++) {
      unsigned *pixels=calloc(w*h,4); assert(pixels);
      struct w32_dib dib=test_dib(pixels,w,h,32,0,1);
      HDC dc=w32_gdi_surface_dc(&dib,(POINT){0},0,0); assert(dc);
      PIXELFORMATDESCRIPTOR p={.nSize=40,.nVersion=1,.dwFlags=PFD_DOUBLEBUFFER|PFD_SUPPORT_OPENGL,
        .cColorBits=32,.cDepthBits=default_format ? 24 : 16,.cStencilBits=default_format ? 8 : 0};
      int chosen=ChoosePixelFormat(dc,&p);
      if (default_format) {
        extern int gl_quake3_enumerate_format(HDC,unsigned);
        int strict=gl_quake3_enumerate_format(dc,8);
        if (!strict) strict=gl_quake3_enumerate_format(dc,0);
        assert(strict==chosen);
      }
      PIXELFORMATDESCRIPTOR actual; assert(DescribePixelFormat(dc,chosen,40,&actual));
#ifndef GL_KERNEL_FAMILY_MACH64
      assert(!(actual.dwFlags&PFD_GENERIC_FORMAT));
#endif
      printf("GL_SAVAGE_FORMAT width=%u height=%u request=%u/%u selected=%d depth=%u stencil=%u flags=%08x\n",
          w,h,p.cDepthBits,p.cStencilBits,chosen,actual.cDepthBits,actual.cStencilBits,actual.dwFlags);
      assert(SetPixelFormat(dc,chosen,&p));
      HGLRC rc=wglCreateContext(dc); assert(rc && wglMakeCurrent(dc,rc));
      glViewport(0,0,w,h); glEnable(GL_SCISSOR_TEST); glScissor(0,0,w,h);
      glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY);
      for (unsigned u=0;u<2;u++) {
        glActiveTextureARB(GL_TEXTURE0_ARB+u); texture(1024+u,256>>u);
        glClientActiveTextureARB(GL_TEXTURE0_ARB+u); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
      }
      glActiveTextureARB(GL_TEXTURE0_ARB); glClientActiveTextureARB(GL_TEXTURE0_ARB);
      for (unsigned f=0;f<3;f++) {
        unsigned before=gl_kernel_draws(); uint64_t fragments=gl_kernel_fragments();
#ifdef SW_PROFILE
        memset(work,0,sizeof(work));
#endif
        extern void gl_savage_scope(unsigned);
        gl_savage_scope(scenario); gl_savage_begin(); frame(scenario); glFinish();
#ifndef GL_KERNEL_FAMILY_MACH64
        extern void gl_kernel_present_lease(const uint32_t *,unsigned,unsigned); gl_kernel_present_lease(pixels,w,h);
#endif
#ifdef SW_PROFILE
        gl_savage_end(w,h,f,gl_kernel_draws()-before,gl_kernel_fragments()-fragments,work[SW_TRIANGLE],work[SW_FRAGMENT]);
#else
        gl_savage_end(w,h,f,gl_kernel_draws()-before,gl_kernel_fragments()-fragments,0,0);
#endif
        assert(glGetError()==GL_NO_ERROR);
      }
      assert(wglMakeCurrent(0,0) && wglDeleteContext(rc)); assert(DeleteDC(dc)); free(pixels);
    }
    gl_kernel_teardown();
  }
#ifndef GL_KERNEL_FAMILY_MACH64
  extern void gl_quake3_lease_flow(int); gl_quake3_lease_flow(default_format);
#endif
  gl_kernel_mode(640,480); gl_hardware_native(0); savage_frames=0;
}
#ifndef GL_KERNEL_FAMILY_MACH64
/* f7-15 review P1 (real Quake III order): wglCreateContext and the first
 * frame run in the 1024x768 desktop generation; the first SwapBuffers takes
 * the full-screen lease, whose 640x480 mode switch revokes that generation.
 * The context must regain hardware without a failure mark, and later frames
 * must stay GPU-resident: no READ, no UPLOAD, direct native scanout. */
void gl_quake3_lease_flow(int default_format) {
  extern void gl_kernel_desktop_setup(unsigned,unsigned),gl_kernel_teardown(void),gl_kernel_mode(unsigned,unsigned);
  extern void gl_kernel_counters(unsigned *); extern unsigned gl_kernel_draws(void);
  extern const uint8_t *gl_kernel_scanout(void); extern unsigned gl_kernel_scanout_pitch(void);
  extern HDC gl_test_dc; extern unsigned gl_test_width,gl_test_height,gl_test_popup,gl_test_generation,gl_test_token;
  extern pthread_t gl_test_owner;
  const unsigned w=640,h=480;
  gl_kernel_mode(w,h); gl_kernel_desktop_setup(w,h);
  unsigned *pixels=calloc(w*h,4); assert(pixels);
  struct w32_dib dib=test_dib(pixels,w,h,32,0,1);
  HDC dc=w32_gdi_surface_dc(&dib,(POINT){0},0,0); assert(dc);
  gl_test_dc=dc; gl_test_owner=pthread_self(); gl_test_width=w; gl_test_height=h; gl_test_popup=1;
  PIXELFORMATDESCRIPTOR p={.nSize=40,.nVersion=1,.dwFlags=PFD_DOUBLEBUFFER|PFD_SUPPORT_OPENGL|PFD_DRAW_TO_WINDOW,
    .cColorBits=32,.cDepthBits=default_format ? 24 : 16,.cStencilBits=default_format ? 8 : 0};
  int chosen=ChoosePixelFormat(dc,&p); assert(chosen && SetPixelFormat(dc,chosen,&p));
  HGLRC rc=wglCreateContext(dc); assert(rc && wglMakeCurrent(dc,rc));
  glViewport(0,0,w,h); glEnable(GL_SCISSOR_TEST); glScissor(0,0,w,h);
  glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_COLOR_ARRAY);
  for (unsigned u=0;u<2;u++) {
    glActiveTextureARB(GL_TEXTURE0_ARB+u); texture(1024+u,256>>u);
    glClientActiveTextureARB(GL_TEXTURE0_ARB+u); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  }
  glActiveTextureARB(GL_TEXTURE0_ARB); glClientActiveTextureARB(GL_TEXTURE0_ARB);
  for (unsigned f=0;f<4;f++) {
    unsigned before[5],after[5],draws=gl_kernel_draws();
    gl_kernel_counters(before);
    glDrawBuffer(GL_BACK); /* Quake III's per-frame RB_DrawBuffer: no sync */
    frame(0);
    assert(SwapBuffers(dc) && glGetError()==GL_NO_ERROR);
    gl_kernel_counters(after);
    unsigned gpu=gl_kernel_draws()-draws,reads=after[0]-before[0],uploads=after[1]-before[1];
    unsigned direct=after[2]-before[2],vram_accesses=after[3]-before[3];
    printf("GL_T23_SWAP depth=%u frame=%u generation=%u gpu_draws=%u reads=%u uploads=%u direct=%u vram_accesses=%u\n",
      p.cDepthBits,f,after[4],gpu,reads,uploads,direct,vram_accesses);
    /* Frame 0 also counts the 3-triangle minimum recheck that the grant's
     * mode switch runs on the fake engine. */
    assert(gpu==(f ? 3000u : 3003u));
    if (!f) {
      /* The first clear loads RAM unless it clears every attachment (D16);
       * one coherent readback (colour + depth) before the grant that
       * switches the mode, then a CPU present of that frame. */
      assert(after[4]==43 && gl_test_generation==43 && reads==2 && uploads==(default_format ? 2u : 0u) && !direct);
    } else {
      assert(after[4]==43 && !reads && direct==1);
      /* D16: the full clear needs no upload; D24/S8 (stencil not cleared by
       * this frame) uploads its RAM copy once, after the mode switch. */
      assert(uploads==(f==1 && default_format ? 2u : 0u));
      if (f>1) assert(vram_accesses<4096); /* CPU VRAM accesses: sampled + scanout guards only */
    }
  }
  const char *renderer=(const char *)glGetString(GL_RENDERER);
  assert(renderer && strstr(renderer,"render3d"));
  /* The front buffer of a GPU swap is the presented copy, read on demand. */
  unsigned before[5],after[5]; gl_kernel_counters(before);
  glReadBuffer(GL_FRONT);
  uint8_t *front=malloc(w*h*4); assert(front);
  glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,front);
  gl_kernel_counters(after);
  assert(glGetError()==GL_NO_ERROR && after[0]-before[0]>=1);
  const uint8_t *scanout=gl_kernel_scanout(); unsigned pitch=gl_kernel_scanout_pitch(),mismatches=0;
  for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
    const uint8_t *q=front+((h-1-y)*w+x)*4;
    uint32_t a=(uint32_t)q[0]<<16|(uint32_t)q[1]<<8|q[2],b; memcpy(&b,scanout+y*pitch+x*4,4);
    if (a!=(b&0xffffffu)) mismatches++;
  }
  assert(!mismatches);
  free(front);
  assert(wglMakeCurrent(0,0) && wglDeleteContext(rc)); assert(DeleteDC(dc)); free(pixels);
  gl_test_dc=0; gl_test_popup=0; gl_test_generation=12; gl_test_token=11;
  gl_kernel_teardown();
  printf("GL_T23_LEASE_FLOW depth=%u context_before_lease=1 regained_hardware=1 gpu_resident_frames=3 front_on_demand=1 PASS\n",
    p.cDepthBits);
}

#endif /* GL_KERNEL_FAMILY_MACH64 */
