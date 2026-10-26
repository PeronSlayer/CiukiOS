/* Process-local fake transport: production GL batching/recovery, scalar engine.
 * It qualifies no kernel transport or hardware capability. SPDX-License-Identifier: MIT */
#include "../../../../sdk/win32/opengl32/backend.h"
#include "../../../../sdk/libgl/software/extensions.h"
#include "../gdi32/support.h"
#include "aliases.h"
#include <w32/opengl32.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned enabled, generation = 1, reject_blend, fail_submit, fail_finish, fail_read;
static unsigned submissions, vertices, uploads, reads, loads, closes, presents, live, texture_live;
static unsigned max_vertices = GL_HW_VERTICES, budget = 4 * 1024 * 1024;
static uint64_t submitted_bytes, upload_bytes, checkpoint_bytes, load_bytes;
struct model {
  struct sw_state target;
  struct sw_texture textures[GL_HW_TEXTURES + 1];
  unsigned active;
};
static struct model models[16];
static struct model *model(unsigned id) {
  assert(id && id <= 16 && models[id-1].active);
  return &models[id-1];
}
static void free_image(struct sw_texture *t) {
  for (unsigned l = 0; l < SW_LEVELS; l++) free(t->image[l].pixels);
  memset(t, 0, sizeof(*t));
}
static int query(unsigned dc, const struct gl_hw_format *f, struct gl_hw_caps *out) {
  (void)dc;
  if (!enabled || f->width > 1024 || f->height > 1024) return 0;
  *out = (struct gl_hw_caps){GL_HW_VERSION, generation, 1024, max_vertices, 2, budget};
  return 1;
}
static int open_model(unsigned dc, const struct gl_hw_format *f, unsigned *id) {
  (void)dc;
  for (unsigned i = 0; i < 16; i++) if (!models[i].active) {
    struct model *m = &models[i];
    m->active = 1;
    m->target.width = f->width; m->target.height = f->height;
    unsigned n = f->width * f->height;
    m->target.color = calloc(n, 4); m->target.color2 = calloc(n, 4);
    if (f->depth_bits == 24) m->target.depth24 = calloc(n, 4);
    else m->target.depth = calloc(n, 2);
    if (f->stencil_bits) m->target.stencil = calloc(n, 1);
    assert(m->target.color && m->target.color2 && (m->target.depth || m->target.depth24));
    *id = i+1; live++; return 0;
  }
  return -1;
}
static void close_model(unsigned id) {
  struct model *m = model(id);
  for (unsigned i = 1; i <= GL_HW_TEXTURES; i++) if (m->textures[i].used) {
    free_image(&m->textures[i]); texture_live--;
  }
  free(m->target.color); free(m->target.color2); free(m->target.depth);
  free(m->target.depth24); free(m->target.stencil);
  memset(m, 0, sizeof(*m)); live--; closes++;
}
static int accept(unsigned id, const struct sw_state *s, unsigned mask,
                   const struct sw_vertex *v, unsigned count) {
  (void)model(id); (void)mask; (void)v; (void)count;
  return !s->fog && !(reject_blend && s->blend);
}
static int load(unsigned id, const struct sw_state *s) {
  struct sw_state *t = &model(id)->target;
  unsigned n = s->width * s->height;
  load_bytes += (uint64_t)n * (4 + (s->color2 ? 4 : 0) + (s->depth24 ? 4 : 2) + (s->stencil ? 1 : 0));
  memcpy(t->color, s->color, n*4);
  if (s->color2) memcpy(t->color2, s->color2, n*4);
  if (s->depth24) memcpy(t->depth24, s->depth24, n*4);
  else memcpy(t->depth, s->depth, n*2);
  if (s->stencil) memcpy(t->stencil, s->stencil, n);
  loads++; return 0;
}
static int read_model(unsigned id, struct sw_state *s) {
  const struct sw_state *t = &model(id)->target;
  unsigned n = s->width * s->height;
  reads++;
  checkpoint_bytes += (uint64_t)n * (4 + (s->color2 ? 4 : 0) + (s->depth24 ? 4 : 2) + (s->stencil ? 1 : 0));
  if (fail_read) { s->color[0] = 0xdeadbeef; return -1; }
  memcpy(s->color, t->color, n*4);
  if (s->color2) memcpy(s->color2, t->color2, n*4);
  if (s->depth24) memcpy(s->depth24, t->depth24, n*4);
  else memcpy(s->depth, t->depth, n*2);
  if (s->stencil) memcpy(s->stencil, t->stencil, n);
  return 0;
}
static int upload(unsigned id, const struct sw_texture *s, unsigned *handle) {
  struct model *m = model(id);
  for (unsigned i = 1; i <= GL_HW_TEXTURES; i++) if (!m->textures[i].used) {
    struct sw_texture *t = &m->textures[i];
    *t = *s; t->used = 1;
    for (unsigned l = 0; l < SW_LEVELS; l++) {
      t->image[l].pixels = 0; t->image[l].compressed = 0;
      if (s->image[l].pixels) {
        unsigned n = s->image[l].w * s->image[l].h * 4;
        t->image[l].pixels = malloc(n); assert(t->image[l].pixels);
        memcpy(t->image[l].pixels, s->image[l].pixels, n);
        upload_bytes += n;
      }
    }
    *handle = i; uploads++; texture_live++; return 0;
  }
  return -1;
}
static void drop(unsigned id, unsigned handle) {
  struct model *m = model(id);
  assert(handle && handle <= GL_HW_TEXTURES && m->textures[handle].used);
  free_image(&m->textures[handle]); texture_live--;
}
static int submit(unsigned id, const struct gl_hw_command *q) {
  struct model *m = model(id);
  submissions++;
  submitted_bytes += sizeof(*q) - sizeof(q->vertices) + q->count * sizeof(q->vertices[0]);
  if (fail_submit && submissions == fail_submit) return -1;
  struct sw_state s = q->state;
  s.color = m->target.color;
  s.color2 = s.color2 ? m->target.color2 : 0;
  s.depth = m->target.depth; s.depth24 = m->target.depth24;
  s.stencil = m->target.stencil;
  for (unsigned u = 0; u < 2; u++) if (s.unit[u].enabled && q->count) {
    assert(q->texture[u] && m->textures[q->texture[u]].used);
    s.unit[u].texture = &m->textures[q->texture[u]];
  }
  if (q->clear_mask) for (unsigned y = 0; y < s.height; y++)
    for (unsigned x = 0; x < s.width; x++) {
      if (s.scissor && ((int)x < s.box[0] || (int)y < s.box[1] ||
          (int)x >= s.box[0]+s.box[2] || (int)y >= s.box[1]+s.box[3])) continue;
      unsigned p = (s.height-1-y)*s.width+x;
      if ((q->clear_mask & GL_COLOR_BUFFER_BIT) && !s.color_disabled) {
        s.color[p] = (s.color[p] & ~s.color_mask) | (q->clear_color & s.color_mask);
        if (s.color2) s.color2[p] = (s.color2[p] & ~s.color_mask) | (q->clear_color & s.color_mask);
      }
      if ((q->clear_mask & GL_DEPTH_BUFFER_BIT) && s.depth_write) {
        if (s.depth24) s.depth24[p] = q->clear_depth;
        else s.depth[p] = q->clear_depth;
      }
      if ((q->clear_mask & GL_STENCIL_BUFFER_BIT) && s.stencil)
        s.stencil[p] = (s.stencil[p] & ~s.stencil_write_mask) | (q->clear_stencil & s.stencil_write_mask);
    }
  assert(q->count <= max_vertices && !(q->count % 3));
  for (unsigned i = 0; i < q->count; i += 3)
    sw_triangle(&s, q->vertices+i, q->vertices+i+1, q->vertices+i+2);
  vertices += q->count;
  return 0;
}
static int finish(unsigned id) { (void)model(id); return fail_finish ? -1 : 0; }
static int present(unsigned id, unsigned token, unsigned gen, unsigned w,
                    unsigned h, unsigned bpp, unsigned pitch) {
  const struct model *m = model(id);
  extern unsigned gl_test_token, gl_test_generation, gl_test_lease;
  assert(gl_test_lease && token == gl_test_token && gen == gl_test_generation);
  assert(w == m->target.width && h == m->target.height && pitch >= w*(bpp/8));
  presents++; return 1;
}
static const struct gl_hw_driver fake = {GL_HW_VERSION, sizeof(fake), query,
  open_model, close_model, accept, load, read_model, upload, drop, submit, finish, present};
static int native_provider;
void gl_hardware_native(int on) { native_provider=on; }
const struct gl_hw_driver *w32_gdi_render3d(unsigned version) {
  extern const struct gl_hw_driver *w32_gdi_render3d_native(unsigned);
  if (native_provider) return w32_gdi_render3d_native(version);
  assert(version == GL_HW_VERSION); return &fake;
}
void gl_hardware_enable(void) { enabled = 1; }
void gl_hardware_sample(unsigned w, unsigned h, unsigned passes, unsigned frames) {
  if (!enabled) return;
  static uint64_t previous[6];
  uint64_t values[6]={submissions,vertices,submitted_bytes,upload_bytes,checkpoint_bytes,load_bytes};
  uint64_t delta[6];
  for (unsigned i=0;i<6;i++) { delta[i]=values[i]-previous[i]; previous[i]=values[i]; }
  double cycles=((double)delta[0]*1200 + (double)delta[1]*220 + delta[2])/frames;
  double traffic=(double)(delta[3]+delta[4]+delta[5])/frames;
  printf("GL_HW_REPLAY width=%u height=%u passes=%u frames=%u submissions_frame=%.2f vertices_frame=%.0f command_bytes_frame=%.0f transfer_bytes_frame=%.0f submission_cycles_frame=%.0f submission_ms_frame=%.3f transfer_ms_frame=%.3f\n",
    w,h,passes,frames,(double)delta[0]/frames,(double)delta[1]/frames,
    (double)delta[2]/frames,traffic,cycles,cycles/933000,traffic/(300*1024*1024)*1000);
}
void gl_hardware_stats(void) {
  assert(!live && !texture_live);
  /* Assumptions: 1200 cycles/submit, 220/vertex, 1/command byte.
   * Engineering assumptions at PIII 933 MHz, not GPU timing. */
  unsigned long long bytes = (unsigned long long)vertices * sizeof(struct sw_vertex);
  printf("GL_HW_MODEL submissions=%u vertices=%u vertex_bytes=%llu uploads=%u reads=%u loads=%u closes=%u presents=%u submission_cycles=%llu command_bytes=%llu upload_bytes=%llu readback_bytes=%llu\n",
    submissions, vertices, bytes, uploads, reads, loads, closes, presents,
    (unsigned long long)submissions*1200 + (unsigned long long)vertices*220 +
      (unsigned long long)submitted_bytes, (unsigned long long)submitted_bytes,
      (unsigned long long)upload_bytes, (unsigned long long)checkpoint_bytes);
}
static unsigned result[32*32];
static unsigned checksum(void) {
  unsigned value = 2166136261u;
  for (unsigned i = 0; i < 32*32; i++) value = (value ^ result[i]) * 16777619u;
  return value;
}
static void triangle(float z) {
  glBegin(GL_TRIANGLES); glVertex3f(-1,-1,z); glVertex3f(1,-1,z); glVertex3f(0,1,z); glEnd();
}
static HDC make(HGLRC *rc) {
  struct w32_dib dib = test_dib(result, 32, 32, 32, 0, 1);
  HDC dc = w32_gdi_surface_dc(&dib, (POINT){0}, 0, 0); assert(dc);
  PIXELFORMATDESCRIPTOR p = {.nSize=40,.nVersion=1,.dwFlags=PFD_DOUBLEBUFFER|PFD_SUPPORT_OPENGL,
                             .cColorBits=32,.cDepthBits=16};
  assert(SetPixelFormat(dc, ChoosePixelFormat(dc,&p), &p));
  *rc = wglCreateContext(dc); assert(*rc && wglMakeCurrent(dc,*rc));
  return dc;
}
static void scene(void) {
  glClearColor(.1f,.2f,.3f,1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST); glColor4f(1,0,0,1); triangle(.5f);
  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(0,1,0,.5f); triangle(0);
  glDisable(GL_BLEND); glColor4f(0,0,1,1);
  glPushMatrix(); glScalef(.5f,.5f,1); triangle(-.5f); glPopMatrix();
  glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,result);
  assert(glGetError()==GL_NO_ERROR);
}
static void end(HDC dc, HGLRC rc) {
  assert(wglMakeCurrent(0,0) && wglDeleteContext(rc) && DeleteDC(dc));
  assert(!live && !texture_live);
}
static void texture_scene(void) {
  unsigned char tex[16]={255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255};
  glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  glBindTexture(GL_TEXTURE_2D,2);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,tex);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  glEnable(GL_TEXTURE_2D); glColor3f(1,1,1); triangle(0);
  memset(tex,255,sizeof(tex));
  glTexSubImage2D(GL_TEXTURE_2D,0,0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,tex);
  glPushMatrix(); glScalef(.5f,.5f,1); triangle(0); glPopMatrix();
  unsigned name=2; glDeleteTextures(1,&name);
  glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,result);
  assert(glGetError()==GL_NO_ERROR);
}
static void swap_front_tests(unsigned popup) {
  HGLRC rc; HDC dc=make(&rc);
  extern HDC gl_test_dc;
  extern unsigned gl_test_width,gl_test_height,gl_test_popup,gl_test_frames,*gl_test_frame_pixel;
  extern pthread_t gl_test_owner;
  gl_test_dc=dc; gl_test_owner=pthread_self();
  gl_test_width=gl_test_height=32; gl_test_popup=popup;
  unsigned frame_pixel=0;
  gl_test_frame_pixel=&frame_pixel;
  glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
  glDrawBuffer(GL_FRONT);
  glClearColor(0,0,1,1); glClear(GL_COLOR_BUFFER_BIT);
  glColor3f(0,0,1); triangle(0);
  unsigned presented=presents, frames=gl_test_frames;
  assert(SwapBuffers(dc));
  /* Swap always presents back, even while the GPU attachment is front. */
  assert(presents==presented);
  if (popup) assert(gl_test_frames==frames+1 && frame_pixel==0xffff0000);
  else assert(result[0]==0xffff0000);
  /* No draw-buffer transition: the next front draw must reload swapped RAM. */
  unsigned before=submissions;
  glColor3f(0,1,0);
  glPushMatrix(); glScalef(.5f,.5f,1); triangle(0); glPopMatrix();
  glReadBuffer(GL_FRONT);
  unsigned char pixels[32*32*4];
  glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
  assert(submissions>before && glGetError()==GL_NO_ERROR);
  assert(pixels[0]==255 && pixels[1]==0 && pixels[2]==0);
  unsigned center=(16*32+16)*4;
  assert(pixels[center]==0 && pixels[center+1]==255 && pixels[center+2]==0);
  end(dc,rc); gl_test_dc=0; gl_test_popup=0; gl_test_frame_pixel=0;
  puts(popup ? "PASS front-selected fullscreen swap: back presentation and coherent front redraw" :
               "PASS front-selected windowed swap: back presentation and coherent front redraw");
}
void gl_hardware_tests(void) {
  HGLRC rc; HDC dc = make(&rc);
  scene(); unsigned golden = checksum(); end(dc,rc);
  dc=make(&rc); texture_scene(); unsigned texture_golden=checksum(); end(dc,rc);
  enabled=1;
  swap_front_tests(0);
  swap_front_tests(1);
  for (unsigned variant=0; variant<5; variant++) {
    dc=make(&rc);
    unsigned before = submissions;
    reject_blend = variant==1;
    fail_submit = variant==2 ? submissions+2 : 0;
    fail_finish = variant==3; fail_read = variant==4;
    scene(); assert(checksum()==golden);
    assert(submissions>before);
    end(dc,rc);
    reject_blend=fail_submit=fail_finish=fail_read=0;
  }
  /* Allocation failure in context, batch and readback staging must preserve
   * the same framebuffer and reclaim the entire journal. */
  for (long allocation=0;allocation<7;allocation++) {
    dc=make(&rc); test_fail_after=allocation;
    scene(); assert(checksum()==golden);
    test_fail_after=-1; end(dc,rc);
  }
  dc=make(&rc);
  glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); glColor3f(1,0,0); triangle(0);
  unsigned n=submissions, r=reads;
  glFlush(); assert(submissions>n && reads==r); /* Flush is not Finish. */
  generation++;
  glFinish(); /* old session is checkpointed before opening the new one. */
  assert(closes && reads>r);
  /* CopyTexImage uses GL read selection, ignores UNPACK skips, owns its data. */
  glBindTexture(GL_TEXTURE_2D, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH,100); glPixelStorei(GL_UNPACK_SKIP_ROWS,2);
  glCopyTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,0,0,32,32,0);
  unsigned char copied[32*32*4]; glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,copied);
  glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,result);
  assert(!memcmp(copied,result,sizeof(copied)) && glGetError()==GL_NO_ERROR);
  int unpack; glGetIntegerv(GL_UNPACK_ROW_LENGTH,&unpack); assert(unpack==100);
  glClearColor(0,1,0,1); glClear(GL_COLOR_BUFFER_BIT);
  glCopyTexSubImage2D(GL_TEXTURE_2D,0,4,5,0,0,1,1);
  glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,copied);
  assert(copied[(5*32+4)*4]==0 && copied[(5*32+4)*4+1]==255);
  glCopyTexImage2D(GL_TEXTURE_1D,0,GL_RGBA,0,0,32,32,0);
  assert(glGetError()==GL_INVALID_ENUM);
  glPixelStorei(GL_UNPACK_ROW_LENGTH,0); glPixelStorei(GL_UNPACK_SKIP_ROWS,0);
  end(dc,rc);
  /* Immutable versions survive subimage/name deletion before delayed submit. */
  dc=make(&rc);
  unsigned u=uploads; texture_scene();
  assert(checksum()==texture_golden && uploads==u+2);
  end(dc,rc);
  /* LRU pressure checkpoints pinned versions, respects provider budget. */
  budget=16; generation++; dc=make(&rc);
  unsigned char tex[16]; memset(tex,255,sizeof(tex));
  glEnable(GL_TEXTURE_2D);
  for (unsigned i=1;i<8;i++) {
    glBindTexture(GL_TEXTURE_2D,i); tex[0]=(unsigned char)i;
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,tex);
    triangle(0);
  }
  glFinish(); assert(glGetError()==GL_NO_ERROR); end(dc,rc); budget=4*1024*1024;
  /* Process detach collects every pending context and immutable resource. */
  dc=make(&rc); triangle(0);
  extern BOOL W32API GlDllMain(void *,DWORD,void *);
  assert(GlDllMain(0,0,0) && !live && !texture_live && DeleteDC(dc));
  /* Existing lease identity reaches only the optional completed GPU copy. */
  dc=make(&rc);
  extern HDC gl_test_dc;
  extern unsigned gl_test_width,gl_test_height,gl_test_popup,gl_test_generation,gl_test_token;
  extern pthread_t gl_test_owner;
  gl_test_dc=dc; gl_test_owner=pthread_self(); gl_test_width=gl_test_height=32; gl_test_popup=1;
  glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); triangle(0);
  unsigned presented=presents; assert(SwapBuffers(dc) && presents==presented+1);
  /* Resize drains old targets before GDI attachment replacement. */
  gl_test_width=gl_test_height=16;
  glViewport(0,0,16,16); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); triangle(0);
  assert(SwapBuffers(dc) && presents==presented+2);
  end(dc,rc); gl_test_dc=0; gl_test_popup=0; gl_test_generation=12; gl_test_token=11;
  enabled=0;
  gl_hardware_stats();
  puts("PASS render3d adapter: mixed draws, delayed submit, partial read failure, replay recovery, mode generation, CopyTexImage, texture versions/LRU, process detach");
}
