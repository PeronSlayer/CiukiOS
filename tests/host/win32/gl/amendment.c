/* Amendment 1 observable rendering and lifetime regressions.
 * SPDX-License-Identifier: MIT */
#include "../../../../sdk/libgl/software/extensions.h"
#include "aliases.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <w32/opengl32.h>
static unsigned image[16 * 16];
static void clean(void) {
  for (int u = 0; u < 2; u++) {
    glActiveTextureARB(GL_TEXTURE0_ARB + u);
    glDisable(GL_TEXTURE_1D);
    glDisable(GL_TEXTURE_2D);
    for (int k = 0; k < 4; k++)
      glDisable(GL_TEXTURE_GEN_S + k);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
  }
  glActiveTextureARB(GL_TEXTURE0_ARB);
  for (int i = 0; i < 6; i++)
    glDisable(GL_CLIP_PLANE0 + i);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_ALPHA_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_FOG);
  glDisable(GL_LIGHTING);
  glDisable(GL_CULL_FACE);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glColorMask(1, 1, 1, 1);
  glDepthMask(1);
  glShadeModel(GL_SMOOTH);
  glViewport(0, 0, 16, 16);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glColor4f(1, 1, 1, 1);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_PACK_SKIP_ROWS, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  assert(!glGetError());
}
static void capture(const char *name) {
  glReadPixels(0, 0, 16, 16, GL_BGRA_EXT, GL_UNSIGNED_BYTE, image);
  assert(!glGetError());
  unsigned hash = 2166136261u;
  for (unsigned i = 0; i < sizeof(image); i++)
    hash = (hash ^ ((unsigned char *)image)[i]) * 16777619u;
  printf("GL_AMENDMENT name=%s checksum=%u\n", name, hash);
}
static unsigned center(void) {
  unsigned c = 0;
  glReadPixels(8, 8, 1, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &c);
  assert(!glGetError());
  return c;
}
static void quad(void) {
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0);
  glVertex3f(-1, -1, 0);
  glTexCoord2f(1, 0);
  glVertex3f(1, -1, 0);
  glTexCoord2f(1, 1);
  glVertex3f(1, 1, 0);
  glTexCoord2f(0, 1);
  glVertex3f(-1, 1, 0);
  glEnd();
}
static void upload(int target, unsigned name, unsigned color) {
  glBindTexture(target, name);
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  if (target == GL_TEXTURE_1D)
    glTexImage1D(target, 0, GL_RGBA, 1, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE,
                 &color);
  else
    glTexImage2D(target, 0, GL_RGBA, 1, 1, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE,
                 &color);
}
static void clipping(void) {
  clean();
  double equation[4] = {1, 0, 0, 0}, got[4];
  glTranslatef(0.25f, 0, 0);
  glClipPlane(GL_CLIP_PLANE0, equation);
  glGetClipPlane(GL_CLIP_PLANE0, got);
  assert(got[0] == 1 && got[3] == -0.25);
  glLoadIdentity();
  glEnable(GL_CLIP_PLANE0);
  quad();
  capture("clip-eye-transform");
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++)
      assert(image[y * 16 + x] == (x >= 10 ? 0xffffffffu : 0xff000000u));
  equation[0] = 0;
  equation[1] = 1;
  equation[3] = 0;
  glClipPlane(GL_CLIP_PLANE0 + 1, equation);
  glEnable(GL_CLIP_PLANE0 + 1);
  glClear(GL_COLOR_BUFFER_BIT);
  quad();
  capture("clip-two-planes");
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++)
      assert(image[y * 16 + x] ==
             (x >= 10 && y >= 8 ? 0xffffffffu : 0xff000000u));
  glColor3f(1, 0, 0);
  glPointSize(1);
  glBegin(GL_POINTS);
  glVertex2f(-0.5f, 0.5f);
  glEnd();
  assert(center() == 0xff000000u);
  glBegin(GL_LINES);
  glVertex2f(-1, 0.5f);
  glVertex2f(1, 0.5f);
  glEnd();
  glRasterPos2f(-0.5f, 0.5f);
  unsigned white = 0xffffffffu;
  glDrawPixels(1, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &white);
  capture("clip-points-lines-raster");
  glPushAttrib(GL_TRANSFORM_BIT | GL_ENABLE_BIT);
  glDisable(GL_CLIP_PLANE0);
  glPopAttrib();
  assert(glIsEnabled(GL_CLIP_PLANE0));
  int n = 0;
  glGetIntegerv(GL_MAX_CLIP_PLANES, &n);
  assert(n == 6);
  glClipPlane(GL_CLIP_PLANE0 + 6, equation);
  assert(glGetError() == GL_INVALID_ENUM);
}
static void texgen(void) {
  clean();
  unsigned texels[4] = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
  glBindTexture(GL_TEXTURE_2D, 20);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_BGRA_EXT,
               GL_UNSIGNED_BYTE, texels);
  glEnable(GL_TEXTURE_2D);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  float s[4] = {0.5f, 0, 0, 0.5f}, t[4] = {0, 0.5f, 0, 0.5f};
  glTexGenfv(GL_S, GL_OBJECT_PLANE, s);
  glTexGenfv(GL_T, GL_OBJECT_PLANE, t);
  glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
  glTexGend(GL_T, GL_TEXTURE_GEN_MODE, GL_OBJECT_LINEAR);
  glEnable(GL_TEXTURE_GEN_S);
  glEnable(GL_TEXTURE_GEN_T);
  quad();
  capture("texgen-object");
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++)
      assert(image[y * 16 + x] == texels[(y >= 8) * 2 + (x >= 8)]);
  glClear(GL_COLOR_BUFFER_BIT);
  glTranslatef(0.5f, 0, 0);
  glTexGenfv(GL_S, GL_EYE_PLANE, s);
  glTexGenfv(GL_T, GL_EYE_PLANE, t);
  glTexGenf(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
  glTexGenf(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
  glLoadIdentity();
  quad();
  capture("texgen-eye-capture");
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++)
      assert(image[y * 16 + x] == texels[(y >= 8) * 2 + (x < 4 || x >= 12)]);
  glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
  glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
  glEnable(GL_NORMALIZE);
  glNormal3f(1, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  for (int i = 0; i < 4; i++)
    glVertex3f(i == 1 || i == 2 ? 1 : -1, i >= 2 ? 1 : -1, -0.4f);
  glEnd();
  capture("texgen-sphere");
  unsigned sphere[256];
  memcpy(sphere, image, sizeof(sphere));
  glDisable(GL_TEXTURE_GEN_S);
  glDisable(GL_TEXTURE_GEN_T);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_QUADS);
  for (int i = 0; i < 4; i++) {
    double x = i == 1 || i == 2 ? 1 : -1, y = i >= 2 ? 1 : -1, z = -0.4;
    double length = sqrt(x * x + y * y + z * z), nx = 1 / sqrt(2), nz = nx;
    double ux = x / length, uy = y / length, uz = z / length,
           d = nx * ux + nz * uz;
    double rx = ux - 2 * nx * d, ry = uy, rz = uz - 2 * nz * d;
    double m = 2 * sqrt(rx * rx + ry * ry + (rz + 1) * (rz + 1));
    glTexCoord2d(rx / m + 0.5, ry / m + 0.5);
    glVertex3f(x, y, z);
  }
  glEnd();
  glReadPixels(0, 0, 16, 16, GL_BGRA_EXT, GL_UNSIGNED_BYTE, image);
  assert(!memcmp(image, sphere, sizeof(sphere)));
  glTexGeni(GL_R, GL_TEXTURE_GEN_MODE, GL_SPHERE_MAP);
  assert(glGetError() == GL_INVALID_ENUM);
  double plane[4];
  glGetTexGendv(GL_S, GL_EYE_PLANE, plane);
  assert(plane[0] == 0.5 && plane[3] == 0.25);
  glActiveTextureARB(GL_TEXTURE0_ARB + 1);
  int mode = 0;
  glGetTexGeniv(GL_S, GL_TEXTURE_GEN_MODE, &mode);
  assert(mode == GL_EYE_LINEAR);
  glActiveTextureARB(GL_TEXTURE0_ARB);
}
static void targets_and_sharing(HDC dc) {
  clean();
  glGenTextures(0, 0);
  glDeleteTextures(0, 0);
  assert(glAreTexturesResident(0, 0, 0));
  assert(!glGetError());
  unsigned n;
  glGenTextures(1, &n);
  assert(!glIsTexture(n));
  upload(GL_TEXTURE_1D, n, 0xffff0000);
  assert(glIsTexture(n));
  GLboolean residence = 0x55;
  assert(glAreTexturesResident(1, &n, &residence) && residence == 0x55);
  glBindTexture(GL_TEXTURE_2D, n);
  assert(glGetError() == GL_INVALID_OPERATION);
  upload(GL_TEXTURE_2D, n + 1, 0xff00ff00);
  glEnable(GL_TEXTURE_1D);
  glEnable(GL_TEXTURE_2D);
  quad();
  assert(center() == 0xff00ff00);
  glDisable(GL_TEXTURE_2D);
  quad();
  assert(center() == 0xffff0000);
  capture("texture-1d-precedence");
  int bound;
  glGetIntegerv(GL_TEXTURE_BINDING_1D, &bound);
  assert(bound == (int)n);
  glPushAttrib(GL_TEXTURE_BIT);
  glBindTexture(GL_TEXTURE_1D, 0);
  glPopAttrib();
  glGetIntegerv(GL_TEXTURE_BINDING_1D, &bound);
  assert(bound == (int)n);
  HGLRC first = wglGetCurrentContext(), second = wglCreateContext(dc);
  assert(second && wglShareLists(first, second) && wglMakeCurrent(dc, second));
  glBindTexture(GL_TEXTURE_1D, n);
  unsigned original = 0;
  glGetTexImage(GL_TEXTURE_1D, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &original);
  assert(original == 0xffff0000);
  assert(wglMakeCurrent(dc, first));
  glDeleteTextures(1, &n);
  assert(!glIsTexture(n));
  glGetIntegerv(GL_TEXTURE_BINDING_1D, &bound);
  assert(!bound);
  upload(GL_TEXTURE_1D, n, 0xff0000ff);
  assert(wglMakeCurrent(dc, second));
  original = 0;
  glGetTexImage(GL_TEXTURE_1D, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &original);
  assert(original == 0xffff0000);
  glBindTexture(GL_TEXTURE_1D, n);
  glGetTexImage(GL_TEXTURE_1D, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &original);
  assert(original == 0xff0000ff);
  assert(wglMakeCurrent(dc, first) && wglDeleteContext(second));
  /* Attribute references survive deletion and are freed at teardown. */
  glPushAttrib(GL_TEXTURE_BIT);
  glDeleteTextures(1, &n);
  glPopAttrib();
  glGetTexImage(GL_TEXTURE_1D, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &original);
  assert(original == 0xff0000ff);
  glBindTexture(GL_TEXTURE_1D, 0);
  assert(!glGetError());
  puts("PASS independent texture targets, immutable dimensionality, retained "
       "shared deletion and reused names");
}
static void pixel_bytes(void) {
  const unsigned formats[] = {GL_RED,     GL_GREEN,     GL_BLUE,
                              GL_ALPHA,   GL_LUMINANCE, GL_LUMINANCE_ALPHA,
                              GL_RGB,     GL_RGBA,      GL_BGR_EXT,
                              GL_BGRA_EXT};
  const int bytes[] = {1, 1, 1, 1, 1, 2, 3, 4, 3, 4};
  const unsigned expected[] = {0xff110000, 0xff001100, 0xff000011, 0xff000000,
                               0xff111111, 0xff111111, 0xff112233, 0xff112233,
                               0xff332211, 0xff332211};
  for (unsigned f = 0; f < sizeof(formats) / sizeof(*formats); f++) {
    clean();
    unsigned char src[64], out[64];
    memset(src, 0xcc, sizeof(src));
    memset(out, 0xa5, sizeof(out));
    int b = bytes[f], stride = (3 * b + 7) & ~7, offset = stride + b;
    const unsigned char color[] = {0x11, 0x22, 0x33, 0x44};
    memcpy(src + offset, color, b);
    memcpy(src + offset + b, color, b);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 3);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    glRasterPos2f(0, 0);
    glDrawPixels(2, 1, formats[f], GL_UNSIGNED_BYTE, src);
    assert(center() == expected[f]);
    glPixelStorei(GL_PACK_ALIGNMENT, 8);
    glPixelStorei(GL_PACK_ROW_LENGTH, 3);
    glPixelStorei(GL_PACK_SKIP_ROWS, 1);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 1);
    glReadPixels(8, 8, 2, 1, formats[f], GL_UNSIGNED_BYTE, out);
    assert(!glGetError());
    unsigned char want[4] = {0};
    if (formats[f] == GL_ALPHA)
      want[0] = 255;
    else if (formats[f] == GL_LUMINANCE || formats[f] == GL_LUMINANCE_ALPHA) {
      want[0] = 0x33;
      want[1] = 255;
    } else {
      memcpy(want, color, b);
      if (b == 4)
        want[3] = 255;
    }
    assert(!memcmp(out + offset, want, b) &&
           !memcmp(out + offset + b, want, b));
    for (int i = 0; i < 64; i++)
      if (i < offset || i >= offset + 2 * b)
        assert(out[i] == 0xa5);
  }
  clean();
  glReadBuffer(GL_FRONT);
  assert(glGetError() == GL_INVALID_OPERATION);
  glDrawBuffer(GL_FRONT_AND_BACK);
  assert(glGetError() == GL_INVALID_OPERATION);
  glReadBuffer(GL_BACK);
  glDrawBuffer(GL_BACK);
  assert(!glGetError());
  puts("PASS ten byte pixel formats, independent PACK/UNPACK alignment, row "
       "length, skips and guard bytes");
}
static void lists(void) {
  clean();
  unsigned list = glGenLists(3);
  assert(list && glIsList(list));
  double equation[4] = {1, 0, 0, 0};
  float p[4] = {0.5f, 0, 0, 0.5f};
  unsigned color = 0xff123456, texel = 0xfffefdfc, sub = 0xff446688;
  unsigned char indices[2] = {(unsigned char)list, 0};
  glNewList(list, GL_COMPILE);
  glColor3f(1, 0, 0);
  glEndList();
  glNewList(list + 1, GL_COMPILE);
  glClipPlane(GL_CLIP_PLANE0, equation);
  glEnable(GL_CLIP_PLANE0);
  glTexGenfv(GL_S, GL_OBJECT_PLANE, p);
  glBindTexture(GL_TEXTURE_1D, 99);
  glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA, 1, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE,
               &texel);
  glTexImage1D(GL_TEXTURE_1D, 1, GL_RGBA, 1, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE,
               &texel);
  glTexSubImage1D(GL_TEXTURE_1D, 1, 0, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &sub);
  glCallLists(1, GL_UNSIGNED_BYTE, indices);
  glBegin(GL_QUADS);
  glVertex2f(-1, -1);
  glVertex2f(1, -1);
  glVertex2f(1, 1);
  glVertex2f(-1, 1);
  glEnd();
  glRasterPos2f(0.5f, 0);
  glDrawPixels(1, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &color);
  glEndList();
  equation[0] = -1;
  p[0] = 7;
  color = texel = sub = 0;
  indices[0] = 0;
  glCallList(list + 1);
  capture("list-owned-inputs");
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++)
      assert(image[y * 16 + x] == (x == 12 && y == 8 ? 0xff123456u
                                   : x >= 8          ? 0xffff0000u
                                                     : 0xff000000u));
  glGetTexImage(GL_TEXTURE_1D, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &texel);
  glGetTexImage(GL_TEXTURE_1D, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &sub);
  assert(texel == 0xfffefdfc && sub == 0xff446688);
  float got[4];
  glGetTexGenfv(GL_S, GL_OBJECT_PLANE, got);
  assert(got[0] == 0.5f);
  glNewList(list + 2, GL_COMPILE);
  glBegin(0xffff);
  assert(glGetError() == GL_INVALID_ENUM);
  glEnd();
  assert(glGetError() == GL_INVALID_OPERATION);
  glTexImage2D(GL_TEXTURE_1D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE,
               &color);
  assert(glGetError() == GL_INVALID_ENUM);
  glTexGenfv(GL_S, GL_TEXTURE_GEN_MODE, 0);
  assert(glGetError() == GL_INVALID_VALUE);
  glMatrixMode(0xffff);
  assert(glGetError() == GL_INVALID_ENUM);
  glStencilFunc(GL_ALWAYS, 0, 0);
  assert(glGetError() == GL_INVALID_OPERATION);
  glBegin(GL_TRIANGLES);
  glClear(0); /* GetError itself is illegal here. */
  glEnd();
  assert(glGetError() == GL_INVALID_OPERATION);
  glEndList();
  glCallList(list + 2);
  assert(!glGetError());
  /* A replacement is not visible before EndList. */
  glNewList(list, GL_COMPILE);
  glColor3f(0, 1, 0);
  glCallList(list);
  glEndList();
  glCallList(list);
  assert(glGetError() == GL_STACK_OVERFLOW);
  glNewList(list, GL_COMPILE_AND_EXECUTE);
  glColor3f(0, 1, 0);
  glEndList();
  glGetFloatv(GL_CURRENT_COLOR, got);
  assert(got[1] == 1);
  glDeleteLists(list, 3);
  puts("PASS display-list input ownership, compile errors, Begin/End "
       "validation, recursion and compile-and-execute");
}

static void extension_rendering(void) {
  clean();
  upload(GL_TEXTURE_2D, 70, 0xff203040);
  glEnable(GL_TEXTURE_2D);
  glColor4ub(64, 96, 128, 255);
  const int modes[] = {GL_REPLACE,        GL_MODULATE,        GL_ADD,
                       GL_ADD_SIGNED_ARB, GL_INTERPOLATE_ARB, GL_SUBTRACT_ARB};
  const unsigned expected[] = {0xff203040, 0xff081220, 0xff6090c0,
                               0xff001040, 0xff304860, 0xff000000};
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE_ARB);
  const float constant[4] = {0, 0, 0, 0.5f};
  glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, constant);
  for (int i = 0; i < 6; i++) {
    glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, modes[i]);
    quad();
    assert(center() == expected[i]);
  }
  glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB_ARB, GL_MODULATE);
  glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE_ARB, 4);
  quad();
  assert(center() == 0xff204880);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
  quad();
  assert(center() == 0xff6090c0);
  capture("extension-add-combine");
  const unsigned bases[] = {GL_ALPHA,     GL_LUMINANCE, GL_LUMINANCE_ALPHA,
                            GL_INTENSITY, GL_RGB,       GL_RGBA};
  const unsigned add_expected[] = {0xff406080, 0xff6080a0, 0xff6080a0,
                                   0xff6080a0, 0xff6090c0, 0xff6090c0};
  unsigned source = 0x80203040;
  for (unsigned i = 0; i < 6; i++) {
    glTexImage2D(GL_TEXTURE_2D, 0, bases[i], 1, 1, 0, GL_BGRA_EXT,
                 GL_UNSIGNED_BYTE, &source);
    quad();
    assert(center() == add_expected[i]);
  }
  glActiveTextureARB(GL_TEXTURE0_ARB + 1);
  glTexCoord2f(0.25f, 0.75f);
  float current[4];
  glGetFloatv(GL_CURRENT_TEXTURE_COORDS, current);
  assert(current[0] == 0 && current[3] == 1);
  glMultiTexCoord2fARB(GL_TEXTURE0_ARB + 1, 0.125f, 0.5f);
  glGetFloatv(GL_CURRENT_TEXTURE_COORDS, current);
  assert(current[0] == 0.125f);
  glActiveTextureARB(GL_TEXTURE0_ARB);
  glGetFloatv(GL_CURRENT_TEXTURE_COORDS, current);
  assert(current[0] == 0.25f);
  /* Edge clamp with linear filtering at both ends cannot wrap to opposite
   * texels. */
  unsigned texels[2] = {0xffff0000, 0xff0000ff};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 1, 0, GL_BGRA_EXT,
               GL_UNSIGNED_BYTE, texels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
  quad();
  capture("extension-edge-clamp");
  assert(image[0] == 0xffff0000 && image[15] == 0xff0000ff);
  /* Incomplete mip chains act as disabled texturing, then become complete. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  GL_NEAREST_MIPMAP_NEAREST);
  quad();
  assert(center() == 0xff406080);
  unsigned mip = 0xff00ff00;
  glTexImage2D(GL_TEXTURE_2D, 1, GL_RGBA, 1, 1, 0, GL_BGRA_EXT,
               GL_UNSIGNED_BYTE, &mip);
  quad();
  assert(center() != 0xff406080);
  /* A 1x1 chain clamps LOD to zero, even with large texture coordinates. */
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_BGRA_EXT,
               GL_UNSIGNED_BYTE, &mip);
  glBegin(GL_QUADS);
  glTexCoord2f(0, 0);
  glVertex2f(-1, -1);
  glTexCoord2f(1024, 0);
  glVertex2f(1, -1);
  glTexCoord2f(1024, 1024);
  glVertex2f(1, 1);
  glTexCoord2f(0, 1024);
  glVertex2f(-1, 1);
  glEnd();
  assert(center() == mip);
  clean();
  float vertices[] = {-1, -1, 1, -1, 1, 1, -1, 1};
  unsigned char indices[] = {0, 1, 2, 3};
  glVertexPointer(2, GL_FLOAT, 0, vertices);
  glEnableClientState(GL_VERTEX_ARRAY);
  glLockArraysEXT(0, 4);
  glDrawElements(GL_QUADS, 4, GL_UNSIGNED_BYTE, indices);
  glUnlockArraysEXT();
  capture("extension-compiled-arrays");
  glLockArraysEXT(-1, 2);
  assert(glGetError() == GL_INVALID_VALUE);
  glLockArraysEXT(0, 0);
  assert(glGetError() == GL_INVALID_VALUE);
  glUnlockArraysEXT();
  assert(glGetError() == GL_INVALID_OPERATION);
  glLockArraysEXT(0, 4);
  glLockArraysEXT(0, 4);
  assert(glGetError() == GL_INVALID_OPERATION);
  glUnlockArraysEXT();
  glDisableClientState(GL_VERTEX_ARRAY);
  for (unsigned format = 0x83f0; format <= 0x83f3; format++) {
    unsigned char block[16] = {0}, copy[16];
    int n = format < 0x83f2 ? 8 : 16, offset = n == 8 ? 0 : 8;
    memset(block, 255, offset);
    block[offset] = 0;
    block[offset + 1] = 0xf8;
    block[offset + 2] = 0xe0;
    block[offset + 3] = 7;
    glBindTexture(GL_TEXTURE_2D, 80);
    glCompressedTexImage2DARB(GL_TEXTURE_2D, 0, format, 4, 4, 0, n, block);
    glCompressedTexSubImage2DARB(GL_TEXTURE_2D, 0, 0, 0, 4, 4, format, n,
                                 block);
    glGetCompressedTexImageARB(GL_TEXTURE_2D, 0, copy);
    assert(!memcmp(block, copy, n));
    unsigned color[16];
    glGetTexImage(GL_TEXTURE_2D, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, color);
    assert(color[0] == 0xffff0000);
    glCompressedTexImage1DARB(GL_TEXTURE_1D, 0, format, 4, 0, n, block);
    assert(glGetError() == GL_INVALID_ENUM);
    glCompressedTexSubImage1DARB(GL_TEXTURE_1D, 0, 0, 4, format, n, block);
    assert(glGetError() == GL_INVALID_ENUM);
    glCompressedTexImage3DARB(0x806f, 0, format, 4, 4, 1, 0, n, block);
    assert(glGetError() == GL_INVALID_ENUM);
    glCompressedTexSubImage3DARB(0x806f, 0, 0, 0, 0, 4, 4, 1, format, n, block);
    assert(glGetError() == GL_INVALID_ENUM);
  }
  assert(!glGetError());
  puts("PASS advertised add/combine, edge-clamp, mip completeness/LOD, array "
       "locks, S3TC upload/subimage/readback and unsupported dimensions");
}
void gl_amendment_tests(HDC dc) {
  HGLRC old = wglGetCurrentContext(), c = wglCreateContext(dc);
  assert(c && wglMakeCurrent(dc, c));
  clipping();
  texgen();
  targets_and_sharing(dc);
  pixel_bytes();
  lists();
  extension_rendering();
  assert(wglMakeCurrent(dc, old) && wglDeleteContext(c));
}
