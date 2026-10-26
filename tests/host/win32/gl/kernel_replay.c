/* Quake-like ordered lightmap/HUD passes, real GL/provider/wire/fake Savage.
 * SPDX-License-Identifier: MIT */
#include "../../../../sdk/win32/opengl32/backend.h"
#include "../gdi32/support.h"
#include "aliases.h"
#include <w32/opengl32.h>
#include <ciuki/abi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
void gl_hardware_native(int);
void gl_kernel_setup(int);
void gl_kernel_teardown(void);
void gl_kernel_revoke(void);
void gl_kernel_process_exit(void);
unsigned gl_kernel_draws(void);
unsigned gl_kernel_copies(void);
unsigned gl_kernel_writes(void);
unsigned gl_kernel_presents(void);
const uint32_t *gl_kernel_pixels(void);
extern const struct gl_hw_driver *w32_gdi_render3d_native(unsigned);
static unsigned replay_width=64,replay_height=64;
static HDC make_kernel_context(HGLRC *rc) {
  HDC dc=CreateCompatibleDC(0);
  BITMAPINFO bi={.bmiHeader={.biSize=40,.biWidth=replay_width,.biHeight=-(int)replay_height,.biPlanes=1,.biBitCount=32}};
  void *bits=0;
  HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&bits,0,0);
  assert(bitmap && SelectObject(dc,bitmap));
  PIXELFORMATDESCRIPTOR f={.nSize=40,.nVersion=1,.dwFlags=PFD_DRAW_TO_BITMAP|PFD_SUPPORT_OPENGL|PFD_DOUBLEBUFFER,
    .cColorBits=32,.cDepthBits=16};
  int pf=ChoosePixelFormat(dc,&f); assert(pf && SetPixelFormat(dc,pf,&f));
  *rc=wglCreateContext(dc); assert(*rc && wglMakeCurrent(dc,*rc));
  /* GDI retains the selected bitmap; teardown restores and deletes it. */
  return dc;
}
static void triangle_kernel(unsigned color) {
  glColor4ub((color>>16)&255,(color>>8)&255,color&255,color>>24);
  const GLfloat positions[]={-.75f,-.75f,0,.75f,-.75f,0,-.75f,.75f,0};
  const GLushort indices[]={0,1,2};
  glTexCoord2f(.375f,.375f);
  glEnableClientState(GL_VERTEX_ARRAY);glVertexPointer(3,GL_FLOAT,0,positions);
  glDrawElements(GL_TRIANGLES,3,GL_UNSIGNED_SHORT,indices);glDisableClientState(GL_VERTEX_ARRAY);
}
static void replay(uint32_t *pixels,int hardware) {
  HGLRC rc; HDC dc=make_kernel_context(&rc);
  glViewport(0,0,replay_width,replay_height); glClearColor(.1f,.2f,.3f,1); glClearDepth(1);
  glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
  unsigned char texels[16]={160,200,240,255,240,80,120,255,40,160,80,255,200,120,40,255};
  for (unsigned u=0;u<2;u++) {
    glActiveTextureARB(GL_TEXTURE0_ARB+u); glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D,11+u);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,texels);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
    glMultiTexCoord2fARB(GL_TEXTURE0_ARB+u,.375f,.375f);
  }
  glActiveTextureARB(GL_TEXTURE0_ARB);
  unsigned capture=0;
  for (unsigned i=0;i<(replay_width==64 ? 12u : 4u);i++) triangle_kernel(0xff806040u);
  glReadPixels(0,0,replay_width,replay_height,GL_RGBA,GL_UNSIGNED_BYTE,pixels+replay_width*replay_height*capture++);
  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER,.1f); triangle_kernel(0x8080c040u);
  glReadPixels(0,0,replay_width,replay_height,GL_RGBA,GL_UNSIGNED_BYTE,pixels+replay_width*replay_height*capture++);
  /* DST_COLOR and polygon offset deliberately exercise whole-draw fallback. */
  glBlendFunc(GL_DST_COLOR,GL_ZERO); triangle_kernel(0xff80ffffu);
  glReadPixels(0,0,replay_width,replay_height,GL_RGBA,GL_UNSIGNED_BYTE,pixels+replay_width*replay_height*capture++);
  glDisable(GL_BLEND); glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(1,1); triangle_kernel(0xff4080c0u);
  glReadPixels(0,0,replay_width,replay_height,GL_RGBA,GL_UNSIGNED_BYTE,pixels+replay_width*replay_height*capture++);
  glDisable(GL_POLYGON_OFFSET_FILL);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
  triangle_kernel(0xff804020u);
  glFinish(); glReadPixels(0,0,replay_width,replay_height,GL_RGBA,GL_UNSIGNED_BYTE,pixels+replay_width*replay_height*capture++);
  assert(capture==5);
  assert(glGetError()==GL_NO_ERROR);
  if (hardware) assert(gl_kernel_draws()>0);
  HBITMAP bitmap=GetCurrentObject(dc,OBJ_BITMAP);
  assert(wglMakeCurrent(0,0) && wglDeleteContext(rc));
  assert(DeleteDC(dc) && DeleteObject(bitmap));
}
static void decoded_texture_replay(const struct gl_hw_driver *d) {
  struct gl_hw_format f={64,64,16,0,0};
  unsigned pixels[16],output[4096]; uint16_t depth[4096];
  for (unsigned i=0;i<16;i++) pixels[i]=0x00ff0000;
  for (unsigned format=0;format<2;format++) {
    unsigned id,handle; assert(!d->open(0,&f,&id));
    struct sw_texture t={.wrap_s=GL_REPEAT,.wrap_t=GL_REPEAT,.min=GL_NEAREST,.mag=GL_NEAREST};
    t.image[0]=(struct sw_image){.w=4,.h=4,.pixels=pixels,.internal=format ? 0x83f1 : 0x83f0};
    assert(!d->texture(id,&t,&handle));
    struct gl_hw_command g={.state={.width=64,.height=64,.color=output,.depth=depth,
      .color_mask=~0u,.depth_test=1,.depth_write=1,.depth_func=GL_LESS,
      .alpha_test=1,.alpha_func=GL_GEQUAL,.alpha_ref=128},.count=3,
      .clear_mask=GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT,.clear_color=0xff0000ff,.clear_depth=65535};
    g.texture[0]=handle; g.state.unit[0]=(struct sw_unit){.enabled=1,.mode=GL_REPLACE,.texture=&t};
    for (unsigned i=0;i<3;i++) g.vertices[i]=(struct sw_vertex){.x=i==1 ? 1024 : 0,
      .y=i==2 ? 1024 : 0,.z=1000,.qw={1048576,1048576},.uv={{32768,32768}},.color={255,255,255,128}};
    int submitted=d->submit(id,&g),finished=submitted ? 0 : d->finish(id);
    int read=(submitted || finished) ? 0 : d->read(id,&g.state);
    if (submitted || finished || read) fprintf(stderr,"decoded texture format=%u submit=%d finish=%d read=%d\n",format,submitted,finished,read);
    assert(!submitted && !finished && !read);
    unsigned at=56*64+8;
    assert(output[at]==(format ? 0xff0000ffu : 0x80ff0000u));
    assert(depth[at]==(format ? 65535 : 1000));
    d->free_texture(id,handle); d->close(id);
  }
  puts("PASS decoded S3TC RGB/RGBA wire upload: opaque RGB alpha, RGB REPLACE preserves vertex alpha, sampled-alpha rejection and depth");
}
void gl_kernel_replay(void) {
  uint32_t golden[4096*5],actual[4096*5];
  gl_hardware_native(1);
  gl_kernel_setup(0); replay(golden,0); assert(!gl_kernel_writes()); gl_kernel_teardown();
  gl_kernel_setup(1); replay(actual,1);
  unsigned different=0;
  for (unsigned i=0;i<4096*5;i++) for (unsigned c=0;c<4;c++) {
    int d=(int)(actual[i]>>(c*8)&255)-(int)(golden[i]>>(c*8)&255);
    assert(d>=-2 && d<=2); different+=d!=0;
  }
  printf("PASS real GL/GDI/copied wire/validator/fake Savage replay: %u channel differences <=2; %u GPU triangles\n",different,gl_kernel_draws());
  const struct gl_hw_driver *d=w32_gdi_render3d_native(GL_HW_VERSION);
  struct gl_hw_format f={64,64,16,0,0}; struct gl_hw_caps caps; unsigned id;
  assert(d->query(0,&f,&caps) && caps.texture_units==2 && caps.max_vertices==3);
  f.depth_bits=24; assert(!d->query(0,&f,&caps)); f.depth_bits=16;
  f.stencil_bits=8; assert(!d->query(0,&f,&caps)); f.stencil_bits=0;
  f.width=65; assert(!d->query(0,&f,&caps)); f.width=64;
  assert(!d->open(0,&f,&id));
  struct gl_hw_command q={.state={.width=64,.height=64,.color_mask=~0u},.clear_mask=GL_COLOR_BUFFER_BIT,.clear_color=0xff123456};
  assert(!d->submit(id,&q) && !d->finish(id));
  assert(d->present(id,1234,42,64,64,32,256)==1);
  assert(gl_kernel_copies()==1 && gl_kernel_presents()==1 && gl_kernel_pixels()[0]==0xff123456);
  gl_kernel_revoke(); assert(d->present(id,1234,42,64,64,32,256)<0 && gl_kernel_presents()==1);
  decoded_texture_replay(d);
  assert(!d->submit(id,&q)); gl_kernel_process_exit();
  assert(d->finish(id)<0); d->close(id); gl_kernel_teardown();
  gl_hardware_native(0);
  puts("PASS format/mode refusal, completed GPU linear copy, revoked lease and process death mid-frame; zero retained allocations");
  static uint32_t full_golden[640*480*5],full_actual[640*480*5];
  replay_width=640; replay_height=480; gl_hardware_native(1);
  gl_kernel_setup(0); replay(full_golden,0); assert(!gl_kernel_writes()); gl_kernel_teardown();
  gl_kernel_setup(2); unsigned before=gl_kernel_draws(); replay(full_actual,1);
  assert(gl_kernel_draws()>before);
  for (unsigned i=0;i<640*480*5;i++) for (unsigned c=0;c<4;c++) {
    int delta=(int)(full_actual[i]>>(c*8)&255)-(int)(full_golden[i]>>(c*8)&255);
    assert(delta>=-2 && delta<=2);
  }
  gl_kernel_teardown(); gl_hardware_native(0); replay_width=replay_height=64;
  puts("PASS 640x480 real GL/GDI/copied-wire/fake Savage: full-size boot qualification, lightmap/depth/blend/HUD replay equals software golden within two levels; unqualified zero writes");
}
