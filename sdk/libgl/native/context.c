/* Native context of the shared GL engine: one process-local context bound to
 * caller-owned XRGB8888 pixels, the backend policy, completion accounting and
 * presentation through the desktop's present lease. The engine, scalar
 * backend, hardware journal and encoder are the same sources the Win32
 * personality builds (sdk/win32/opengl32). SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef CIUKI_GL_HOST_MODEL
/* The kernel ABI header spells these for the SDK; the host libc had them. */
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#endif
#include "../../win32/opengl32/internal.h"
#include "../include/ciuki/gl.h"
#include <ciuki/present_abi.h>
#ifndef CIUKI_GL_HOST_MODEL
#include <ciuki/runtime.h>
#endif

static struct gl_context *current;
static unsigned generation, policy;
static uint32_t *target;            /* caller pixels */
static unsigned target_stride;       /* in pixels */
static uint32_t *private_rows;       /* padded/strided targets render here */
static uint64_t frames, hardware_frames, software_frames, presents, hardware_presents;
/* The last frame ended with a GPU present (f7-15 resident targets): the
 * caller's pixels were not written back. A rebind or close before the next
 * draw releases them without a readback (gfxdemo frees its full-screen
 * buffer when the lease ends). */
static int gpu_presented;
static int released(const struct gl_context *c) {
  return gpu_presented && !c->frame_hardware_draws && !c->frame_software_draws;
}

/* Engine services the Win32 context module provides from the loader. */
void *gl_alloc(unsigned n) {
  if (!n || n > 64u * 1024u * 1024u) return 0;
  return calloc(1, n);
}
void gl_dealloc(void *p) { free(p); }
void gl_lock(void) {}
void gl_unlock(int *unused) { (void)unused; }
struct gl_context *gl_current(void) { return current; }
int gl_target(HDC dc, unsigned *w, unsigned *h, unsigned *window) {
  (void)dc;
  if (!current) return 0;
  *w = current->sw.width; *h = current->sw.height; *window = 0;
  return 1;
}
void gl_render_size(unsigned *w, unsigned *h) { (void)w; (void)h; }
int gl_refresh_target(struct gl_context *c, HDC dc) {
  (void)dc;
  return c->sw.color != 0;
}
void gl_release_present(struct gl_context *c) { (void)c; }
void gl_prepare_present(struct gl_context *c, HDC dc) { (void)c; (void)dc; }
/* Single-buffered native targets draw to the front buffer; glFlush and
 * glFinish complete the engine's work and the application presents. */
BOOL gl_present(struct gl_context *c, int front) { (void)c; (void)front; return 1; }
const char *ciuki_gl_native_option(const char *name) { return getenv(name); }
void ciuki_gl_native_log(const char *line, unsigned bytes) {
  (void)bytes;
  fprintf(stderr, "%s\n", line);
}
void *ciuki_gl_get_proc_address(const char *name) { return name ? gl_extension(name) : 0; }

static void free_attachments(struct gl_context *c) {
  free(private_rows); private_rows = 0;
  gl_dealloc(c->sw.depth);
  c->sw.depth = 0; c->front_color = c->back_color = 0; c->sw.color = 0;
}
static int attach(struct gl_context *c, uint32_t *pixels, unsigned width, unsigned height, unsigned stride) {
  int direct = stride == width;
  uint32_t *color = pixels, *rows = 0;
  if (c->sw.depth && width == c->sw.width && height == c->sw.height &&
      pixels == target && stride == target_stride) {
    /* The same target again (one bind per frame): the provider's copy of
     * the target stays loaded; only the window mapping is refreshed. */
    gl_window_state(c);
    gl_selected_context = &c->tiny;
    gl_sync_viewport(c);
    gl_selected_context = 0;
    return 0;
  }
  if (!direct) {
    rows = calloc((size_t)width * height, 4);
    if (!rows) return -ENOMEM;
    color = rows;
  }
  if (width != c->sw.width || height != c->sw.height || !c->sw.depth) {
    uint16_t *depth = gl_alloc(width * height * 2);
    if (!depth) { free(rows); return -ENOMEM; }
    /* Contents after a target change are undefined; no engine may keep
     * the old storage. */
    if (released(c)) gl_hw_release(c);
    gl_hw_close(c);
    free_attachments(c);
    c->sw.depth = depth;
    c->sw.width = width; c->sw.height = height;
    c->viewport[0] = c->viewport[1] = 0; c->viewport[2] = (int)width; c->viewport[3] = (int)height;
    c->scissor[0] = c->scissor[1] = 0; c->scissor[2] = (int)width; c->scissor[3] = (int)height;
  } else if (color != c->back_color) {
    /* Same geometry, different storage: the journal's checkpoint names the
     * old pixels, so complete and release it before re-pointing (a frame
     * the GPU presented is released without writing them). */
    if (released(c)) gl_hw_release(c);
    else gl_hw_software(c);
    free(private_rows); private_rows = 0;
  } else { free(rows); rows = 0; }
  if (rows) private_rows = rows;
  c->front_color = c->back_color = color;
  c->drawable_width = width; c->drawable_height = height;
  gl_select_buffer(c);
  gl_window_state(c);
  gl_selected_context = &c->tiny;
  gl_sync_viewport(c);
  gl_selected_context = 0;
  target = pixels; target_stride = stride;
  gpu_presented = 0;
  return 0;
}
static int create(uint32_t *pixels, unsigned width, unsigned height, unsigned stride) {
  struct gl_context *c = gl_alloc(sizeof(*c));
  if (!c) return -ENOMEM;
  c->depth_bits = 16;
  c->share = gl_alloc(sizeof(*c->share));
  c->zb = ZB_open(4, 4, ZB_MODE_RGBA, 0);
  if (!c->share || !c->zb) {
    if (c->zb) ZB_close(c->zb);
    gl_dealloc(c->share); gl_dealloc(c);
    return -ENOMEM;
  }
  c->share->refs = 1;
  c->handle = 1;
  gl_selected_context = &c->tiny;
  glInit(c->zb);
  if (c->tiny.error_flag) {
    glClose(); gl_selected_context = 0;
    ZB_close(c->zb); gl_dealloc(c->share); gl_dealloc(c);
    return -ENOMEM;
  }
  c->tiny.software_triangle = gl_clip_triangle;
  c->tiny.software_vertex = gl_vertex_generated;
  c->tiny.software_line = gl_compat_line;
  c->tiny.software_state = c;
  c->texture_base[0] = c->texture_ptr[0] = c->tiny.matrix_stack[2];
  c->texture_base[1] = c->texture_ptr[1] = c->texture1_stack;
  gl_M4_Id(c->texture1_stack);
  for (int unit = 0; unit < 2; unit++) c->tiny.software_texture_matrix[unit] = c->texture_ptr[unit];
  c->tiny.current_tex_coord1.W = 1;
  gl_selected_context = 0;
  current = c;
  int e = attach(c, pixels, width, height, stride);
  if (e) { ciuki_gl_close(); return e; }
  gl_selected_context = &c->tiny;
  gl_defaults(c);
  gl_selected_context = 0;
  gl_census_open(c);
  generation++;
  frames = hardware_frames = software_frames = presents = hardware_presents = 0;
  return 0;
}
int ciuki_gl_bind(uint32_t *pixels, unsigned width, unsigned height, unsigned stride) {
  if (!pixels || !width || !height || width > 1600 || height > 1200 ||
      stride < width || stride > 16384) return -EINVAL;
  if (!current) return create(pixels, width, height, stride);
  if (current->tiny.in_begin) return -EINVAL;
  return attach(current, pixels, width, height, stride);
}
static void account(struct gl_context *c) {
  gpu_presented = 0;
  frames++;
  /* A frame without any draw or clear is neither (frames counts it). */
  if (c->frame_hardware_draws && !c->frame_software_draws) hardware_frames++;
  else if (c->frame_software_draws) software_frames++;
  c->frame_hardware_draws = c->frame_software_draws = 0;
  c->frames++;
  if (c->census) gl_census_frame(c);
  extern void ciuki_gl_native_frame(void);
  ciuki_gl_native_frame();
}
static void copy_out(struct gl_context *c) {
  if (!private_rows || !target) return;
  for (unsigned y = 0; y < c->sw.height; y++)
    memcpy(target + (size_t)y * target_stride, private_rows + (size_t)y * c->sw.width, c->sw.width * 4u);
}
void ciuki_gl_finish(void) {
  struct gl_context *c = current;
  if (!c || c->tiny.in_begin) return;
  gl_hw_sync(c);
  copy_out(c);
  account(c);
}
void ciuki_gl_close(void) {
  struct gl_context *c = current;
  if (!c) return;
  if (released(c)) gl_hw_release(c);
  gpu_presented = 0;
  gl_hw_close(c);
  gl_census_close(c);
  current = 0;
  gl_selected_context = &c->tiny;
  c->tiny.matrix_stack[2] = c->texture_base[0];
  glClose();
  gl_selected_context = 0;
  ZB_close(c->zb);
  gl_drop_state(c);
  gl_free_list(&c->pending_list);
  gl_texture_cleanup(c);
  if (!--c->share->refs) {
    gl_free_textures(c->share);
    for (unsigned i = 0; i < GL_LISTS; i++) gl_free_list(&c->share->lists[i]);
    gl_dealloc(c->share);
  }
  free_attachments(c);
  gl_dealloc(c);
  target = 0;
  /* Accounting belongs to the closed context. */
  frames = hardware_frames = software_frames = presents = hardware_presents = 0;
}
unsigned ciuki_gl_generation(void) { return generation; }
unsigned ciuki_gl_native_policy(void) { return policy; }
int ciuki_gl_set_backend(unsigned requested) {
  if (requested > CIUKI_GL_BACKEND_HARDWARE) return -EINVAL;
  policy = requested;
  extern void ciuki_gl_native_frame(void);
  ciuki_gl_native_frame(); /* re-query the provider at the next GL call */
  if (requested == CIUKI_GL_BACKEND_HARDWARE) {
    /* Without a context the qualification of the common 640x480 target is
     * reported; a bound context asks for its own size. A context that fell
     * back permanently (contract 7.2) never reopens hardware. */
    extern int ciuki_gl_native_available(const struct gl_hw_format *);
    struct gl_context *c = current;
    struct gl_hw_format f = {c ? c->sw.width : 640, c ? c->sw.height : 480, 16, 0, 0};
    if ((c && c->hardware_failed) || !ciuki_gl_native_available(&f)) return -ENODEV;
  }
  return 0;
}
int ciuki_gl_status(struct ciuki_gl_status *out) {
  if (!out || out->size != sizeof(*out)) return -EINVAL;
  struct gl_context *c = current;
  memset(out, 0, sizeof(*out));
  out->size = sizeof(*out);
  out->policy = policy;
  out->backend = c && gl_hw_active(c) ? CIUKI_GL_BACKEND_HARDWARE : CIUKI_GL_BACKEND_SOFTWARE;
  out->hardware_failed = c ? c->hardware_failed : 0;
  extern unsigned ciuki_gl_native_generation(void);
  out->generation = c && gl_hw_active(c) ? ciuki_gl_native_generation() : 0;
  out->frames = frames; out->hardware_frames = hardware_frames; out->software_frames = software_frames;
  if (c) {
    out->hardware_draws = c->hardware_draws; out->software_draws = c->software_draws;
    out->refusals = c->refusals; out->fallbacks = c->fallbacks;
  }
  out->presents = presents; out->hardware_presents = hardware_presents;
  return 0;
}
/* The present lease frame path: the same W32_DISPLAY record and kernel
 * authority checks the Win32 personality uses (token, generation, holder). */
static int32_t display_frame(struct ciuki_w32_display_args *a, const void *payload) {
#ifdef CIUKI_GL_HOST_MODEL
  extern int32_t ciuki_gl_host_display_frame(struct ciuki_w32_display_args *, const void *) __attribute__((weak));
  if (!ciuki_gl_host_display_frame) return -ENOSYS;
  return ciuki_gl_host_display_frame(a, payload);
#else
  (void)payload;
  return (int32_t)ciuki_syscall(CIUKI_SYS_W32, (uint32_t)(uintptr_t)a, sizeof(*a), 0, 0, 0, 0);
#endif
}
int ciuki_gl_present(const struct ciuki_gl_lease *l) {
  struct gl_context *c = current;
  if (!c || c->tiny.in_begin) return -EINVAL;
  if (!l || !l->token || !l->generation || l->width != c->sw.width || l->height != c->sw.height ||
      (l->bpp != 32 && l->bpp != 16) || l->pitch < l->width * (l->bpp / 8) || l->pitch > 65536) {
    /* The frame still completes into the caller's pixels; only the
     * presentation is refused. */
    ciuki_gl_finish();
    return -EINVAL;
  }
  int gpu = gl_hw_present(c, l->token, l->generation, l->width, l->height, l->bpp, l->pitch);
  if (gpu < 0) { gl_hw_sync(c); copy_out(c); account(c); return gpu; }
  if (gpu > 0) {
    /* GPU frame (as opengl32's GPU swap, f7-15): the attachments stay
     * resident on the provider, nothing is read back into the caller's
     * pixels; ciuki_gl_finish or a pixel read makes them current. */
    account(c); presents++; hardware_presents++; gpu_presented = 1;
    return 1;
  }
  gl_hw_sync(c);
  copy_out(c);
  const uint32_t *source = c->back_color;
  const void *bytes = source;
  uint8_t *copy = 0;
  if (l->bpp != 32 || l->pitch != l->width * 4u) {
    copy = malloc((size_t)l->pitch * l->height);
    if (!copy) { account(c); return -ENOMEM; }
    for (unsigned y = 0; y < l->height; y++)
      for (unsigned x = 0; x < l->width; x++) {
        uint32_t p = source[y * c->sw.width + x];
        if (l->bpp == 32) memcpy(copy + y * l->pitch + x * 4, &p, 4);
        else {
          uint16_t q = (uint16_t)(((p >> 8) & 0xf800) | ((p >> 5) & 0x7e0) | ((p >> 3) & 31));
          memcpy(copy + y * l->pitch + x * 2, &q, 2);
        }
      }
    bytes = copy;
  }
  struct ciuki_w32_display_args a = {.header = {1, sizeof(a), W32_DISPLAY, 0},
    .action = W32_DISPLAY_FRAME, .token = l->token, .generation = l->generation,
    .address = (uint32_t)(uintptr_t)bytes};
  int32_t r = display_frame(&a, bytes);
  free(copy);
  account(c);
  if (r < 0) return r;
  if (a.error) return -(int)a.error;
  if (!a.result) return -EIO;
  presents++;
  return 0;
}
