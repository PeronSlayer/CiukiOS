/* Identical authored scenes in the PE fixture and Win32 host model.
 * SPDX-License-Identifier: MIT */
#ifdef W32_HOST_MODEL
#include "../../../tests/host/win32/gl/aliases.h"
#include <w32/opengl32.h>
#else
#include <w32/gl_client.h>
#endif
#include "../../../sdk/libgl/software/extensions.h"
#include "scenes.h"
static void triangle(void) {
  glBegin(GL_TRIANGLES);
  glColor3f(1, 0.1f, 0.2f);
  glVertex3f(-0.8f, -0.7f, 0);
  glColor3f(0.1f, 1, 0.2f);
  glVertex3f(0.8f, -0.7f, 0);
  glColor3f(0.1f, 0.2f, 1);
  glVertex3f(0, 0.8f, 0);
  glEnd();
}
static void quad(int multi) {
  glColor4f(1, 1, 1, 1);
  glBegin(GL_QUADS);
  for (int i = 0; i < 4; i++) {
    float x = i == 1 || i == 2 ? 0.85f : -0.85f, y = i >= 2 ? 0.8f : -0.8f,
          s = i == 1 || i == 2 ? 1 : 0, t = i >= 2 ? 1 : 0;
    glTexCoord2f(s, t);
    if (multi)
      glMultiTexCoord2fARB(GL_TEXTURE0_ARB + 1, s, t);
    glVertex3f(x, y, 0);
  }
  glEnd();
}
static void texture(unsigned name, int other) {
  unsigned pixels[16];
  for (unsigned y = 0; y < 4; y++)
    for (unsigned x = 0; x < 4; x++)
      pixels[y * 4 + x] = other ? 0xff808080u + ((x ^ y) & 1) * 0x00404040u
                          : ((x + y) & 1) ? 0xffffcc20u
                                          : 0xff2050e0u;
  glBindTexture(GL_TEXTURE_2D, name);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_BGRA_EXT,
               GL_UNSIGNED_BYTE, pixels);
}
void w32_gl_scene(unsigned scene) {
  glActiveTextureARB(GL_TEXTURE0_ARB + 1);
  glDisable(GL_TEXTURE_2D);
  glActiveTextureARB(GL_TEXTURE0_ARB);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_LIGHTING);
  glDisable(GL_BLEND);
  glDisable(GL_FOG);
  glDisable(GL_ALPHA_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(1);
  glColorMask(1, 1, 1, 1);
  glViewport(0, 0, W32_GL_WIDTH, W32_GL_HEIGHT);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0.04f, 0.06f, 0.09f, 1);
  glClearDepth(1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (scene == 0)
    triangle();
  else if (scene == 1) {
    texture(1, 0);
    glEnable(GL_TEXTURE_2D);
    quad(0);
  } else if (scene == 2) {
    const float light[4] = {0, 0, 2, 1}, material[4] = {0.3f, 0.65f, 0.95f, 1};
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_NORMALIZE);
    glLightfv(GL_LIGHT0, GL_POSITION, light);
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, material);
    glRotatef(28, 1, 0, 0);
    glRotatef(32, 0, 1, 0);
    static const signed char face[6][4][3] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
        {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
        {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}}};
    static const signed char normal[6][3] = {
        {0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    glBegin(GL_QUADS);
    for (int f = 0; f < 6; f++) {
      glNormal3f(normal[f][0], normal[f][1], normal[f][2]);
      for (int v = 0; v < 4; v++)
        glVertex3f(face[f][v][0] * 0.5f, face[f][v][1] * 0.5f,
                   face[f][v][2] * 0.5f);
    }
    glEnd();
  } else if (scene == 3) {
    texture(1, 0);
    glEnable(GL_TEXTURE_2D);
    glActiveTextureARB(GL_TEXTURE0_ARB + 1);
    texture(2, 1);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glActiveTextureARB(GL_TEXTURE0_ARB);
    quad(1);
  } else if (scene == 4) {
    unsigned list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    triangle();
    glEndList();
    glCallList(list);
    glDeleteLists(list, 1);
  } else if (scene == 5) {
    static const unsigned char text[16] = {0x7e, 0x42, 0x40, 0x40, 0x40, 0x42,
                                           0x7e, 0,    0x3c, 0x18, 0x18, 0x18,
                                           0x18, 0x18, 0x3c, 0};
    glDisable(GL_DEPTH_TEST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glColor3f(1, 0.9f, 0.5f);
    glRasterPos2f(-0.3f, 0);
    glBitmap(8, 8, 0, 0, 10, 0, text);
    glBitmap(8, 8, 0, 0, 10, 0, text + 8);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  } else {
    static const unsigned char image[12] = {255, 0, 0,   0,   255, 0,
                                            0,   0, 255, 255, 255, 255};
    glDisable(GL_DEPTH_TEST);
    glRasterPos2f(-0.5f, -0.5f);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelZoom(32, 32);
    glDrawPixels(2, 2, GL_RGB, GL_UNSIGNED_BYTE, image);
    glPixelZoom(1, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  }
  glFinish();
}
uint32_t w32_gl_checksum(void) {
  static unsigned char row[W32_GL_WIDTH * 4];
  uint32_t checksum = 2166136261u;
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  for (unsigned y = 0; y < W32_GL_HEIGHT; y++) {
    glReadPixels(0, y, W32_GL_WIDTH, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
    for (unsigned x = 0; x < sizeof(row); x++)
      checksum = (checksum ^ row[x]) * 16777619u;
  }
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  return checksum;
}
