/* Amendment 3 observable regression oracles. SPDX-License-Identifier: MIT */
#include "aliases.h"
#include "../../../../sdk/libgl/software/extensions.h"
#include "../../../../sdk/libgl/software/renderer.h"
#include "../gdi32/support.h"
#include <w32/opengl32.h>
#include <w32/shims.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
extern struct ciuki_w32_compat gl_test_compat;
extern HDC gl_test_dc;
extern unsigned gl_test_width, gl_test_height, gl_test_popup, gl_test_dead;
extern unsigned gl_test_lease, gl_test_releases, gl_test_damage, gl_test_failure;
extern pthread_t gl_test_owner;
static unsigned pixel(int x, int y) {
  unsigned p = 0;
  glReadPixels(x, y, 1, 1, GL_BGRA_EXT, GL_UNSIGNED_BYTE, &p);
  assert(!glGetError()); return p & 0xffffff;
}
static void bitmap_layout(void) {
  for (int skips=0;skips<2;skips++) {
  glViewport(0, 0, 16, 16); glDisable(GL_DEPTH_TEST);
  glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(1, 0, 0); glRasterPos2f(-1, -1);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 64);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, skips);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, skips*3);
  unsigned char bits[18] = {0}; bits[skips*8] = 0x80>> (skips*3); bits[(skips+1)*8] = 0x40>> (skips*3);
  unsigned list = glGenLists(1);
  glNewList(list, GL_COMPILE); glBitmap(8, 2, 0, 0, 0, 0, bits); glEndList();
  memset(bits, 0xff, sizeof(bits));
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0); glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glCallList(list); glDeleteLists(list, 1);
  assert(pixel(0, 0) == 0xff0000 && pixel(1, 1) == 0xff0000);
  assert(pixel(1, 0) == 0 && pixel(0, 1) == 0);
  assert(!glGetError());
  }
}
static void *worker(void *p) {
  HGLRC rc = *(HGLRC *)p;
  assert(!wglMakeCurrent(gl_test_dc, rc));
  assert(!wglGetCurrentContext() && !SwapBuffers(gl_test_dc));
  return 0;
}
struct memory_worker { HDC dc; HGLRC rc; };
static void *memory_worker(void *p) {
  struct memory_worker *target=p;
  assert(wglMakeCurrent(target->dc,target->rc));
  glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
  assert(SwapBuffers(target->dc));assert(wglMakeCurrent(0,0));
  return 0;
}
static void scaled_drawable(HDC dc) {
  HGLRC old = wglGetCurrentContext();
  gl_test_dc = dc; gl_test_owner = pthread_self();
  gl_test_width = 640; gl_test_height = 480;
  gl_test_compat.mask = W32_SHIM_BIT(W32_SH_DDRAW_SCALE);
  gl_test_compat.width = 320; gl_test_compat.height = 240;
  HGLRC rc = wglCreateContext(dc); assert(rc && wglMakeCurrent(dc, rc));
  DWORD sizes[6]; assert(CiukiGLGetSizes(dc, sizes));
  assert(sizes[0] == 320 && sizes[1] == 240 && sizes[2] == 640 && sizes[3] == 480);
  glViewport(0, 0, 640, 480); glScissor(320, 0, 320, 480);
  glEnable(GL_SCISSOR_TEST); glClearColor(0, 0, 0, 1);
  glDisable(GL_SCISSOR_TEST); glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_SCISSOR_TEST); glColor3f(0, 1, 0);
  glBegin(GL_QUADS); glVertex2f(-1,-1); glVertex2f(1,-1); glVertex2f(1,1); glVertex2f(-1,1); glEnd();
  assert(pixel(100, 100) == 0 && pixel(500, 400) == 0x00ff00);
  int box[4]; glGetIntegerv(GL_SCISSOR_BOX, box); assert(box[0] == 320 && box[2] == 320);
  glDisable(GL_SCISSOR_TEST); glRasterPos2f(0,0);
  unsigned blue[4] = {0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff};
  glDrawPixels(2,2,GL_BGRA_EXT,GL_UNSIGNED_BYTE,blue);
  assert(pixel(320,240) == 255 && pixel(321,241) == 255);
  assert(SwapBuffers(dc) && gl_test_damage == 1);
  gl_test_popup = 1; assert(SwapBuffers(dc) && gl_test_lease);
  gl_test_popup = 0; assert(SwapBuffers(dc) && !gl_test_lease && gl_test_releases == 1);
  gl_test_failure = 1; assert(!SwapBuffers(dc)); gl_test_failure = 0;
  gl_test_dead = 1; assert(!SwapBuffers(dc)); gl_test_dead = 0;
  assert(wglMakeCurrent(0,0));
  pthread_t t; assert(!pthread_create(&t,0,worker,&rc)); pthread_join(t,0);
  assert(wglMakeCurrent(dc,rc));
  gl_test_compat.mask = 0; gl_test_width = 128; gl_test_height = 96;
  glViewport(0,0,128,96); assert(CiukiGLGetSizes(dc,sizes));
  assert(sizes[0] == 128 && sizes[1] == 96);
  glClearColor(1,0,1,1); glClear(GL_COLOR_BUFFER_BIT); assert(pixel(127,95) == 0xff00ff);
  unsigned target[32 * 24]; struct w32_dib dib = test_dib(target,32,24,32,0,1);
  HDC second = w32_gdi_surface_dc(&dib,(POINT){0},0,0);
  assert(second && SetPixelFormat(second,1,0));
  assert(wglMakeCurrent(second,rc) && CiukiGLGetSizes(second,sizes));
  assert(sizes[0] == 32 && sizes[1] == 24 && sizes[2] == 32 && sizes[3] == 24);
  assert(wglMakeCurrent(dc,rc)); assert(DeleteDC(second));
  assert(wglDeleteContext(rc)); gl_test_dc = 0;
  assert(wglMakeCurrent(dc,old));
}
static void reset(void) {
  for(int u=0;u<2;u++) {
    glActiveTextureARB(GL_TEXTURE0_ARB+u); glDisable(GL_TEXTURE_1D); glDisable(GL_TEXTURE_2D);
    for(int k=0;k<4;k++) glDisable(GL_TEXTURE_GEN_S+k);
    glMatrixMode(GL_TEXTURE); glLoadIdentity();
  }
  glActiveTextureARB(GL_TEXTURE0_ARB);
  glMatrixMode(GL_PROJECTION); glLoadIdentity(); glMatrixMode(GL_MODELVIEW); glLoadIdentity();
  for(int k=0;k<6;k++) glDisable(GL_CLIP_PLANE0+k);
  glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_ALPHA_TEST);
  glDisable(GL_FOG); glDisable(GL_LIGHTING); glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST); glDisable(GL_POLYGON_OFFSET_FILL);
  glShadeModel(GL_SMOOTH); glViewport(0,0,16,16); glColor4f(1,1,1,1);
  glClearColor(0,0,0,1); glClearDepth(1); glDepthMask(1); glColorMask(1,1,1,1);
  glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); glPixelZoom(1,1);
  glPixelStorei(GL_UNPACK_ALIGNMENT,1); glPixelStorei(GL_PACK_ALIGNMENT,1);
  assert(!glGetError());
}
static void projective(void) {
  for(int unit=0;unit<2;unit++) {
    reset(); glActiveTextureARB(GL_TEXTURE0_ARB+unit);
    unsigned tex, data[16]; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_1D,tex);
    for(int i=0;i<16;i++) data[i]=0xff000000u+i*0x101010u;
    glTexImage1D(GL_TEXTURE_1D,0,GL_RGBA,16,0,GL_BGRA_EXT,GL_UNSIGNED_BYTE,data);
    glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_1D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_REPLACE); glEnable(GL_TEXTURE_1D);
    const float w[3]={1,2,4}, s[3]={0.125f,0.75f,2}, q[3]={1,2,3};
    glBegin(GL_TRIANGLES);
    for(int i=0;i<3;i++) {
      glMultiTexCoord4fARB(GL_TEXTURE0_ARB+unit,s[i],0,0,q[i]);
      glVertex4f((i==1?1:-1)*w[i],(i==2?1:-1)*w[i],0,w[i]);
    }
    glEnd();
    unsigned checked=0;
    for(int y=0;y<15;y++) for(int x=0;x<15-y;x++) {
      double b=(x+0.5)/16, c=(y+0.5)/16, a=1-b-c;
      double u=(a*s[0]/w[0]+b*s[1]/w[1]+c*s[2]/w[2])/
               (a*q[0]/w[0]+b*q[1]/w[1]+c*q[2]/w[2]);
      double value=u*16;
      if(fabs(value-round(value))<0.002) continue;
      assert(pixel(x,y)==(data[(int)value]&0xffffff)); checked++;
    }
    assert(checked>80); glDeleteTextures(1,&tex);
  }
  puts("PASS projective varying-Q and varying-W barycentric oracle on both units");
}
static void wide_and_mips(void) {
  unsigned color[16*16], baseline[16*16], texels[8]; uint16_t depth[16*16];
  struct sw_texture tex={.target=GL_TEXTURE_1D,.wrap_s=GL_CLAMP_TO_EDGE,.wrap_t=GL_CLAMP_TO_EDGE,
    .min=GL_NEAREST,.mag=GL_NEAREST};
  texels[0]=0xffff0000;texels[1]=0xff0000ff;
  tex.image[0]=(struct sw_image){.w=2,.h=1,.pixels=texels,.internal=GL_RGBA};
  struct sw_state state={.width=16,.height=16,.color=color,.depth=depth,.color_mask=0xffffffff};
  state.unit[0]=(struct sw_unit){.enabled=1,.mode=GL_REPLACE,.texture=&tex};
  struct sw_vertex v[3]={{.x=0,.y=0,.qw={10485,10485},.color={255,255,255,255}},
                         {.x=256,.y=0,.qw={10485,10485},.color={255,255,255,255}},
                         {.x=0,.y=256,.qw={10485,10485},.color={255,255,255,255}}};
  const int uv[]={400*65536,134217728,-134217728};
  const int qs[]={10485,1,134217728,-1};
  for(unsigned j=0;j<sizeof(uv)/sizeof(*uv);j++) for(unsigned k=0;k<sizeof(qs)/sizeof(*qs);k++)
    for(int repeat=0;repeat<2;repeat++) {
      for(int i=0;i<3;i++){v[i].uv[0][0]=uv[j];v[i].qw[0]=qs[k];}
      tex.wrap_s=repeat?GL_REPEAT:GL_CLAMP_TO_EDGE;
      memset(color,0,sizeof(color)); sw_triangle(&state,v,v+1,v+2);
      int64_t u=(int64_t)uv[j]*1048576/qs[k];
      int index;
      if(repeat){u%=65536;if(u<0)u+=65536;index=(int)(u*2/65536);}
      else index=u>=32768;
      assert(color[(15-2)*16+2]==texels[index]);
    }
  unsigned levels[4][8]={{0}};
  const unsigned colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
  for(int l=0;l<4;l++) {
    for(int i=0;i<(8>>l);i++)levels[l][i]=colors[l];
    tex.image[l]=(struct sw_image){.w=8u>>l,.h=1,.pixels=levels[l],.internal=GL_RGBA};
  }
  tex.min=GL_NEAREST_MIPMAP_LINEAR;
  for(int perspective=0;perspective<2;perspective++) {
    for(int i=0;i<3;i++){v[i].qw[0]=perspective?(1048576>>(i*2)):1048576;v[i].uv[0][0]=i==1?3*65536/(perspective?4:1):0;}
    for(int rotation=0;rotation<3;rotation++) {
      memset(color,0,sizeof(color));sw_triangle(&state,v+rotation,v+(rotation+1)%3,v+(rotation+2)%3);
      if(!rotation)memcpy(baseline,color,sizeof(color));else assert(!memcmp(baseline,color,sizeof(color)));
    }
    double bx=2.5/16,cy=3.5/16, u=bx*(perspective?0.75:3),
      q=1-bx-cy+bx*(perspective?0.25:1)+cy*(perspective?0.0625:1);
    double dudx=(perspective?0.75:3)/16, dqdx=(perspective?-0.75:0)/16,
      dqdy=(perspective?-0.9375:0)/16;
    double dx=fabs((dudx*q-u*dqdx)/(q*q))*8,dy=fabs(-u*dqdy/(q*q))*8;
    double lod=log2(fmax(dx,dy));if(lod<0)lod=0;if(lod>3)lod=3;
    int level=(int)lod, next=level<3?level+1:level;double f=lod-level;
    unsigned got=color[(15-3)*16+2];
    for(int channel=0;channel<3;channel++) {
      int shift=channel*8;
      int expected=(int)(((colors[level]>>shift)&255)*(1-f)+((colors[next]>>shift)&255)*f+0.5);
      assert(abs((int)((got>>shift)&255)-expected)<=3);
    }
    if(!perspective)assert((got&0xff0000) && (got&0xff00));
  }
  puts("PASS wide UV wrap/clamp extrema, cyclic mip invariance, fractional LOD and strong-perspective derivative oracle");
}
static void flat_and_offset(void) {
  reset();glShadeModel(GL_FLAT);
  glBegin(GL_QUADS);
  glColor3f(1,0,0);glVertex2f(-1,-1);glColor3f(0,1,0);glVertex2f(1,-1);
  glColor3f(1,1,0);glVertex2f(1,1);glColor3f(0,0,1);glVertex2f(-1,1);glEnd();
  for(int y=1;y<15;y++)for(int x=1;x<15;x++)assert(pixel(x,y)==255);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_TRIANGLES);
  glColor3f(1,0,0);glVertex2f(-1,-1);glColor3f(0,1,0);glVertex2f(0,1);
  glColor3f(0,0,1);glVertex2f(3,-1);glEnd();
  assert(pixel(8,4)==255);
  double plane[4]={1,0,0,0};glClipPlane(GL_CLIP_PLANE0,plane);glEnable(GL_CLIP_PLANE0);
  glClear(GL_COLOR_BUFFER_BIT);
  glBegin(GL_TRIANGLES);glColor3f(1,0,0);glVertex2f(-1,-1);
  glColor3f(0,1,0);glVertex2f(-1,1);glColor3f(0,0,1);glVertex2f(1,0);glEnd();
  assert(pixel(10,8)==255);
  for(int disabled=0;disabled<2;disabled++) {
    reset();glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);
    glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(0,1000);glColor3f(1,0,0);
    glBegin(GL_QUADS);glVertex2f(-1,-1);glVertex2f(1,-1);glVertex2f(1,1);glVertex2f(-1,1);glEnd();
    if(disabled)glDisable(GL_POLYGON_OFFSET_FILL);
    glColor3f(0,1,0);glBegin(GL_LINES);glVertex2f(-1,0);glVertex2f(1,0);glEnd();
    assert(pixel(8,8)==0x00ff00);
    unsigned blue=0xff0000ff;glRasterPos2f(0.25f,0.25f);glDrawPixels(1,1,GL_BGRA_EXT,GL_UNSIGNED_BYTE,&blue);
    assert(pixel(10,10)==255);
    unsigned char bit=128;glRasterPos2f(-0.25f,0.25f);glBitmap(1,1,0,0,0,0,&bit);
    assert(pixel(6,10)==0x00ff00);
    glBegin(GL_POINTS);glVertex2f(0,0.5f);glEnd();assert(pixel(8,12)==0x00ff00);
  }
  puts("PASS original flat quad/homogeneous/user-clipped color and polygon offset isolation for line/point/bitmap/pixels");
}
static void formats_and_hint(void) {
  reset();
  const unsigned formats[]={GL_RGB,GL_RGBA,GL_ALPHA,GL_LUMINANCE,GL_LUMINANCE_ALPHA,GL_INTENSITY};
  const unsigned expected[]={0xff3d7ae9,0x113d7ae9,0x11000000,0xff3d3d3d,0x113d3d3d,0x3d3d3d3d};
  unsigned tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
  const unsigned char source[]={61,122,233,17};
  for(unsigned i=0;i<6;i++) {
    unsigned a=0,b=0;glTexImage2D(GL_TEXTURE_2D,0,formats[i],1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,source);
    glGetTexImage(GL_TEXTURE_2D,0,GL_BGRA_EXT,GL_UNSIGNED_BYTE,&a);
    glTexImage2D(GL_TEXTURE_2D,0,formats[i],1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,source);
    glGetTexImage(GL_TEXTURE_2D,0,GL_BGRA_EXT,GL_UNSIGNED_BYTE,&b);
    assert(a==expected[i] && b==a);
  }
  glDeleteTextures(1,&tex);
  int hint;glGetIntegerv(GL_TEXTURE_COMPRESSION_HINT_ARB,&hint);assert(hint==GL_DONT_CARE);
  glPushAttrib(GL_HINT_BIT);glHint(GL_TEXTURE_COMPRESSION_HINT_ARB,GL_NICEST);
  double d;float f;GLboolean b;
  glGetDoublev(GL_TEXTURE_COMPRESSION_HINT_ARB,&d);glGetFloatv(GL_TEXTURE_COMPRESSION_HINT_ARB,&f);
  glGetBooleanv(GL_TEXTURE_COMPRESSION_HINT_ARB,&b);assert(d==GL_NICEST && f==GL_NICEST && b);
  glHint(GL_TEXTURE_COMPRESSION_HINT_ARB,0);assert(glGetError()==GL_INVALID_ENUM);
  glPopAttrib();glGetIntegerv(GL_TEXTURE_COMPRESSION_HINT_ARB,&hint);assert(hint==GL_DONT_CARE);
  unsigned list=glGenLists(1);glNewList(list,GL_COMPILE);glHint(GL_TEXTURE_COMPRESSION_HINT_ARB,GL_FASTEST);glEndList();
  glCallList(list);glGetIntegerv(GL_TEXTURE_COMPRESSION_HINT_ARB,&hint);assert(hint==GL_FASTEST);
  glDeleteLists(list,1);assert(!glGetError());
  puts("PASS all six internal-format subimage equivalences and compression hint validation/queries/stack/list");
}
void gl_review_tests(HDC dc) {
  bitmap_layout(); scaled_drawable(dc);
  projective(); wide_and_mips(); flat_and_offset(); formats_and_hint(); reset();
  struct memory_worker memory={dc,wglGetCurrentContext()};
  assert(wglMakeCurrent(0,0));pthread_t t;
  assert(!pthread_create(&t,0,memory_worker,&memory));pthread_join(t,0);
  assert(wglMakeCurrent(dc,memory.rc));
  puts("PASS Amendment 3 bitmap capture, scaled viewport/scissor/pixels/resize, lease transition, failed completion/death, worker binding refusal");
}
