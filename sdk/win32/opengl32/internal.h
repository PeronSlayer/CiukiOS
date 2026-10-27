/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32_GL_INTERNAL_H
#define W32_GL_INTERNAL_H
#include "../../../sdk/libgl/vendor/src/zgl.h"
#define W32_GL_TYPES_H
typedef unsigned GLbitfield;
#include "../../../sdk/libgl/software/extensions.h"
#include "../../../sdk/libgl/software/renderer.h"
#ifdef CIUKI_GL_NATIVE
/* f7-14: the native library builds the same engine over libc and the native
 * render3d provider; Win32 types and byte helpers come from this shim. */
#include "../../libgl/native/platform.h"
#else
#include <w32/opengl32.h>
#include <w32/services.h>
#endif
#include "backend.h"
union gl_arg {
  int64_t i;
  double f;
  const void *p;
  void *result;
};
#include "ids.h"
union gl_arg w32_gl_dispatch(unsigned, const union gl_arg *);
void gl_lock(void);
void gl_unlock(int *);
#define GL_GUARD int guard __attribute__((cleanup(gl_unlock))) = (gl_lock(), 0)
struct gl_array {
  const uint8_t *p;
  int size, type, stride, enabled;
};
struct gl_command {
  struct gl_command *next;
  unsigned id, bytes;
  int unpack[6];
  union gl_arg arg[12];
  uint8_t data[];
};
struct gl_list {
  struct gl_command *head, *tail;
  unsigned reserved;
};
struct gl_texture {
  struct gl_texture *next;
  struct sw_texture value;
};
#define GL_LISTS 2048
struct gl_share {
  unsigned refs;
  struct gl_texture *textures;
  struct gl_list lists[GL_LISTS];
};
struct gl_snapshot;
struct gl_census;
struct gl_texgen {
  int enabled, mode;
  double object[4], eye[4];
};
struct gl_context {
  struct gl_census *census;
  struct gl_hw_context *hardware;
  unsigned hardware_failed; /* Permanent for this HGLRC, including resize/rebind. */
  struct gl_snapshot *attributes, *client_attributes;
  unsigned attribute_depth, client_depth;
  HGLRC handle;
  HDC dc;
  unsigned tid, format;
  unsigned depth_bits, stencil_bits, doublebuffer, clear_stencil;
  unsigned draw_buffer, read_buffer;
  uint32_t *back_color, *front_color;
  GLContext tiny;
  ZBuffer *zb;
  struct sw_state sw;
  struct gl_share *share;
  unsigned active, client;
  struct sw_texture defaults[2][2], *binding[2][2];
  int texture_enabled[2][2];
  struct gl_texgen texgen[2][4];
  double clip_plane[6][4];
  int clip_enabled[6];
  struct gl_list pending_list;
  unsigned compile_begin, compile_failed;
  unsigned error, compiling, compile_mode, replaying, list_base, call_depth;
  int viewport[4], scissor[4], pack[6], unpack[6];
  unsigned drawable_width, drawable_height;
  struct gl_array array[7];
  int fog_mode;
  int compression_hint;
  float fog_density, fog_start, fog_end;
  float range[2], clear[4], clear_depth, alpha_ref, offset_factor, offset_units;
  float line_width;
  float texcoord[2][4];
  float raster[4], raster_color[4];
  int raster_valid;
  float zoom[2];
  M4 texture1_stack[MAX_TEXTURE_STACK_DEPTH];
  M4 *texture_base[2], *texture_ptr[2];
  int primitive, locked_first, locked_count;
  unsigned frames, leased_window;
  unsigned gamma_registered, gamma_valid, gamma_serial, gamma_token, gamma_generation;
  HBITMAP back;
  /* f7-14 completion accounting shared by SwapBuffers and the native frame
   * boundary: draws/clears executed by the provider versus the scalar path,
   * provider refusals (temporary) and whole-context fallbacks (permanent).
   * frame_* counts reset at each frame boundary. */
  uint64_t hardware_draws, software_draws, refusals, fallbacks;
  uint64_t frame_hardware_draws, frame_software_draws;
};
int gl_target(HDC, unsigned *, unsigned *, unsigned *);
void gl_census_open(struct gl_context *);
void gl_census_draw(struct gl_context *, const struct sw_vertex *);
void gl_census_clear(struct gl_context *, unsigned);
void gl_census_frame(struct gl_context *);
void gl_census_other(struct gl_context *);
void gl_census_close(struct gl_context *);
void gl_prepare_present(struct gl_context *, HDC);
int gl_refresh_target(struct gl_context *, HDC);
void gl_select_buffer(struct gl_context *);
void gl_window_state(struct gl_context *);
void gl_sync_viewport(struct gl_context *);
int gl_window_coord(struct gl_context *, double, int);
unsigned gl_read_color(struct gl_context *, int, int);
void gl_render_size(unsigned *, unsigned *);
void gl_release_present(struct gl_context *);
BOOL gl_present(struct gl_context *, int);
struct gl_context *gl_current(void);
void gl_error(struct gl_context *, unsigned);
void *gl_alloc(unsigned);
void gl_dealloc(void *);
int gl_record(struct gl_context *, unsigned, const union gl_arg *);
void gl_execute_list(struct gl_context *, unsigned);
void gl_free_list(struct gl_list *);
void gl_compat_line(GLContext *, GLVertex *, GLVertex *);
void gl_vertex_generated(GLContext *, GLVertex *);
int gl_user_visible(struct gl_context *, const V4 *);
int gl_clip_line(struct gl_context *, GLVertex *, GLVertex *);
void gl_clip_triangle(GLContext *, GLVertex *, GLVertex *, GLVertex *);
int gl_geometry(struct gl_context *, unsigned, const union gl_arg *,
                union gl_arg *);
void gl_sync_textures(struct gl_context *);
void gl_texture_ref(struct sw_texture *, int);
void gl_texture_bindings(struct gl_context *, int);
void gl_texture_cleanup(struct gl_context *);
void gl_free_textures(struct gl_share *);
unsigned gl_validate(struct gl_context *, unsigned, const union gl_arg *);
void gl_triangle(GLContext *, GLVertex *, GLVertex *, GLVertex *);
void gl_drop_state(struct gl_context *);
int gl_state(struct gl_context *, unsigned, const union gl_arg *,
             union gl_arg *);
void gl_defaults(struct gl_context *);
void gl_pixels(struct gl_context *, unsigned, const union gl_arg *,
               union gl_arg *);
void gl_textures(struct gl_context *, unsigned, const union gl_arg *,
                 union gl_arg *);
int gl_pixel_bytes(int, int);
unsigned gl_pixel_get(const uint8_t *, int, int);
void gl_pixel_put(uint8_t *, int, int, unsigned);
unsigned gl_pixel_stride(int, int, int, int);
struct gl_bitmap_layout { unsigned pitch, bytes; };
int gl_bitmap_layout(int, int, const int[6], struct gl_bitmap_layout *);
void *gl_extension(const char *);
#endif
