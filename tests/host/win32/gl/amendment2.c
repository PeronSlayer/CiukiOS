/* Independent regressions for the f7-08 lead review. SPDX-License-Identifier: MIT */
#include "aliases.h"
#include "../gdi32/support.h"
#include "../../../../sdk/libgl/software/renderer.h"
#include <w32/opengl32.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
void raster_review_reciprocal_tests(void);

static void line_depth(void) {
  static uint32_t color[640], depth[640];
  struct sw_state s = {.width=640, .height=1, .color=color, .depth24=depth,
    .depth_test=1, .depth_write=1, .depth_func=GL_ALWAYS, .color_mask=~0u};
  struct sw_vertex a = {.color={255,255,255,255}}, b = a;
  b.x=639*16; b.z=16777215;
  for (int direction=0; direction<2; direction++) {
    sw_line(&s,direction?&b:&a,direction?&a:&b);
    for (unsigned x=0; x<640; x++) {
      unsigned expected=direction ? 16777215u-(unsigned)(16777215ULL*(639-x)/639)
                                  : (unsigned)(16777215ULL*x/639);
      assert(depth[x]==expected && color[x]==0xffffffff);
    }
  }
}
static void arrays(void) {
  const unsigned caps[]={GL_VERTEX_ARRAY,GL_COLOR_ARRAY,GL_NORMAL_ARRAY,
    GL_EDGE_FLAG_ARRAY,GL_INDEX_ARRAY,GL_TEXTURE_COORD_ARRAY};
  for (unsigned k=0;k<sizeof(caps)/sizeof(*caps);k++)
    for (unsigned unit=0;unit<2;unit++) {
      glClientActiveTextureARB(GL_TEXTURE0_ARB+unit);
      glDisableClientState(caps[k]);
    }
  for (unsigned k=0;k<sizeof(caps)/sizeof(*caps);k++)
    for (unsigned unit=0;unit<2;unit++) {
      glClientActiveTextureARB(GL_TEXTURE0_ARB+unit);
      glActiveTextureARB(GL_TEXTURE0_ARB+1-unit);
      for (int on=0;on<2;on++) {
        if(on) glEnableClientState(caps[k]); else glDisableClientState(caps[k]);
        GLboolean b=2; GLint i=-1; GLfloat f=-1; GLdouble d=-1;
        assert(glIsEnabled(caps[k])==on && !glGetError());
        glGetBooleanv(caps[k],&b); glGetIntegerv(caps[k],&i);
        glGetFloatv(caps[k],&f); glGetDoublev(caps[k],&d);
        assert(!glGetError() && b==on && i==on && f==on && d==on);
        if(caps[k]==GL_TEXTURE_COORD_ARRAY) {
          glClientActiveTextureARB(GL_TEXTURE0_ARB+1-unit);
          assert(glIsEnabled(caps[k])==(unit==0?0:1) && !glGetError());
          glClientActiveTextureARB(GL_TEXTURE0_ARB+unit);
        }
      }
    }
}
static void viewport(void) {
  GLint max[2],v[4]; glGetIntegerv(GL_MAX_VIEWPORT_DIMS,max);
  glViewport(17,-29,INT_MAX,INT_MAX); glGetIntegerv(GL_VIEWPORT,v);
  assert(!glGetError() && v[0]==17 && v[1]==-29 && v[2]==max[0] && v[3]==max[1]);
  glViewport(INT_MAX,INT_MIN,INT_MAX,0); glGetIntegerv(GL_VIEWPORT,v);
  assert(!glGetError() && v[0]==INT_MAX && v[1]==INT_MIN && v[2]==max[0] && !v[3]);
  glViewport(INT_MIN,INT_MAX,INT_MAX,INT_MAX);
  glBegin(GL_TRIANGLES); glVertex2f(-1,-1); glVertex2f(1,-1); glVertex2f(0,1); glEnd();
  assert(!glGetError());
  glViewport(0,0,-1,1); assert(glGetError()==GL_INVALID_VALUE);
  glGetIntegerv(GL_VIEWPORT,v); assert(v[0]==INT_MIN && v[1]==INT_MAX);
  glPushAttrib(GL_VIEWPORT_BIT); glViewport(0,0,16,16); glPopAttrib();
  glGetIntegerv(GL_VIEWPORT,v);
  assert(!glGetError() && v[0]==INT_MIN && v[1]==INT_MAX && v[2]==max[0] && v[3]==max[1]);
  unsigned list=glGenLists(1); glNewList(list,GL_COMPILE);
  glViewport(-35,42,INT_MAX,123); glEndList(); glCallList(list);
  glGetIntegerv(GL_VIEWPORT,v); glDeleteLists(list,1);
  assert(!glGetError() && v[0]==-35 && v[1]==42 && v[2]==max[0] && v[3]==123);
}
static void device_gamma(HDC dc, HGLRC rc) {
  extern HDC gl_test_dc;
  extern pthread_t gl_test_owner;
  extern unsigned gl_test_width, gl_test_height, gl_test_popup;
  extern unsigned gl_test_gamma_presenters, gl_test_gamma_updates, gl_test_gamma_serial;
  extern unsigned gl_test_gamma_failure, gl_test_frames, gl_test_generation, gl_test_token;
  extern unsigned gl_test_lease;
  extern unsigned gl_test_damage, gl_test_releases;
  extern uint16_t gl_test_gamma_ramp[768];
  gl_test_dc=dc; gl_test_owner=pthread_self();
  gl_test_width=gl_test_height=16; gl_test_popup=1;
  for(unsigned c=0;c<3;c++) for(unsigned i=0;i<256;i++)
    gl_test_gamma_ramp[c*256+i]=i*257;
  assert(wglMakeCurrent(dc,rc));
#ifdef GL_TEST_NO_GAMMA
  assert(!gl_test_gamma_presenters && SwapBuffers(dc) && !gl_test_gamma_updates);
#else
  assert(gl_test_gamma_presenters==1); /* before first SwapBuffers */
  assert(wglMakeCurrent(dc,rc) && gl_test_gamma_presenters==1);
  unsigned updates=gl_test_gamma_updates;
  assert(SwapBuffers(dc) && gl_test_gamma_updates==++updates);
  assert(SwapBuffers(dc) && gl_test_gamma_updates==updates);
  for(unsigned c=0;c<3;c++) for(unsigned i=0;i<256;i++)
    gl_test_gamma_ramp[c*256+i]=(uint16_t)((i*(511-c*73))&65535);
  gl_test_gamma_serial++;
  assert(SwapBuffers(dc) && gl_test_gamma_updates==++updates);
  gl_test_generation++;
  assert(SwapBuffers(dc) && gl_test_gamma_updates==++updates);
  gl_test_token++;
  assert(SwapBuffers(dc) && gl_test_gamma_updates==++updates);
  for(unsigned failure=1;failure<=3;failure++) {
    gl_test_gamma_serial++; gl_test_gamma_failure=failure;
    unsigned frames=gl_test_frames;
    assert(!SwapBuffers(dc) && gl_test_frames==frames && gl_test_gamma_updates==++updates);
    gl_test_gamma_failure=0;
    assert(SwapBuffers(dc) && gl_test_gamma_updates==++updates);
  }
  gl_test_popup=0;
  assert(SwapBuffers(dc) && !gl_test_gamma_presenters && !gl_test_lease);
  gl_test_popup=1;
  assert(SwapBuffers(dc) && gl_test_gamma_presenters==1 && gl_test_gamma_updates==++updates);
#endif
  assert(wglMakeCurrent(0,0) && wglDeleteContext(rc));
  assert(!gl_test_gamma_presenters && !gl_test_lease);
  gl_test_dc=0; gl_test_popup=0; gl_test_generation=12; gl_test_token=11;
  gl_test_damage=gl_test_releases=0;
  puts("PASS amendment2 gamma registration, serial/lease updates, payload, retry and release");
}
void gl_amendment2_tests(const char *which) {
  if(!which || !strcmp(which,"reciprocal")) raster_review_reciprocal_tests();
  if(!which || !strcmp(which,"line")) { line_depth(); puts("PASS amendment2 D24 line depth"); }
  if(!which || !strcmp(which,"border") || !strcmp(which,"lod")) {
    extern void raster_review_sampling_case(const char *);
    raster_review_sampling_case(which);
  }
  if(which && strcmp(which,"arrays") && strcmp(which,"viewport") && strcmp(which,"gamma")) return;
  uint32_t pixels[16*16]; struct w32_dib dib=test_dib(pixels,16,16,32,0,1);
  HDC dc=w32_gdi_surface_dc(&dib,(POINT){0},0,0);
  assert(dc && SetPixelFormat(dc,1,0));
  HGLRC rc=wglCreateContext(dc); assert(rc && wglMakeCurrent(dc,rc));
  if(!which || !strcmp(which,"arrays")) { arrays(); puts("PASS amendment2 client-array enable queries"); }
  if(!which || !strcmp(which,"viewport")) { viewport(); puts("PASS amendment2 clamped viewport and extreme origins"); }
  if(!which || !strcmp(which,"gamma")) {
    device_gamma(dc,rc); assert(DeleteDC(dc)); return;
  }
  assert(wglMakeCurrent(0,0) && wglDeleteContext(rc) && DeleteDC(dc));
}
