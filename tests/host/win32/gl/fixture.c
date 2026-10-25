/* Execute the production guest fixture on real host GL/present paths.
 * USER startup, font success and process exit are deterministic boundaries;
 * font conversion is qualified separately by engine.c. SPDX-License-Identifier: MIT */
#include "../gdi32/support.h"
#include "../../../../src/probes/win32_gl/controller.h"
#include <w32/opengl32.h>
#include <assert.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
extern HDC gl_test_dc;
extern unsigned gl_test_width,gl_test_height,gl_test_dead,gl_test_failure;
extern pthread_t gl_test_owner;
static struct w32_gl_result result;
static unsigned pixels[320*240], code;
static jmp_buf exit_point;
void gl_fixture_entry(void);
void *VirtualAlloc(void *p,DWORD bytes,DWORD type,DWORD protect) {
  assert((uintptr_t)p==W32_GL_RESULT_VA && bytes==4096 && type==0x3000 && protect==4);
  memset(&result,0,sizeof(result));result.release=1;return &result;
}
void ExitProcess(DWORD c) { code=c;longjmp(exit_point,1); }
void Sleep(DWORD ms) { (void)ms;assert(0); }
intptr_t DefWindowProcA(uintptr_t w,unsigned m,uintptr_t a,intptr_t b) {
  (void)w;(void)m;(void)a;(void)b;return 0;
}
unsigned short RegisterClassA(const void *c) { assert(c);return 1; }
BOOL AdjustWindowRect(RECT *r,DWORD style,BOOL menu) {
  assert(r && style==0x00cf0000 && !menu);
  r->left-=4;r->right+=4;r->top-=24;r->bottom+=4;return 1;
}
uintptr_t CreateWindowExA(DWORD ex,const char *c,const char *t,DWORD style,
  int x,int y,int w,int h,uintptr_t parent,void *menu,void *instance,void *data) {
  (void)ex;(void)c;(void)t;(void)style;(void)x;(void)y;
  (void)parent;(void)menu;(void)instance;(void)data;
  assert(w==328 && h==268);return 7;
}
HDC GetDC(uintptr_t w) {
  assert(w==7);struct w32_dib dib=test_dib(pixels,320,240,32,0,1);
  gl_test_dc=w32_gdi_surface_dc(&dib,(POINT){0},0,0);
  gl_test_width=320;gl_test_height=240;gl_test_owner=pthread_self();
  return gl_test_dc;
}
int ReleaseDC(uintptr_t w,HDC dc) { assert(w==7 && dc==gl_test_dc);return DeleteDC(dc); }
BOOL DestroyWindow(uintptr_t w) { assert(w==7);gl_test_dc=0;return 1; }
BOOL gl_fixture_font(HDC d,DWORD first,DWORD count,DWORD base) {
  assert(d==gl_test_dc && first==32 && count==96 && base);return 1;
}
void gl_fixture_tests(void) {
  for(int failed=0;failed<2;failed++) {
    gl_test_failure=failed;
    if(!setjmp(exit_point))gl_fixture_entry();
    assert(result.done && result.render_width==320 && result.render_height==240 &&
           result.drawable_width==320 && result.drawable_height==240 && result.display_width==320);
    for(int i=0;i<7;i++)assert(result.checksum[i]==w32_gl_expected[i]);
    if(failed)assert(result.frames==0 && result.errors==7 && code==1 && !w32_gl_valid(&result));
    else {
      assert(code==0 && !result.errors && w32_gl_valid(&result));
      result.pixel_formats=4;assert(!w32_gl_valid(&result));result.pixel_formats=10;
      result.checksum[0]^=1;assert(!w32_gl_valid(&result));result.checksum[0]^=1;
      result.render_width=1;assert(!w32_gl_valid(&result));result.render_width=320;
    }
  }
  gl_test_failure=gl_test_dead=0;
  puts("PASS production fixture actual sizes, exact scene content, completed frame counting and failed-swap rejection");
}
