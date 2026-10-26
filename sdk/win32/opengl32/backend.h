/* Private process-local render3d transport adapter. NOT a kernel wire/SDK ABI.
 * The provider owns marshalling to the serialized display worker. No callback
 * grants raw device/scanout access. SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef W32_GL_BACKEND_H
#define W32_GL_BACKEND_H
#include "../../libgl/software/renderer.h"
#define GL_HW_VERSION 1u
#define GL_HW_VERTICES 768u
#define GL_HW_TEXTURES 64u
#define GL_HW_BATCHES 64u
struct gl_hw_format {
  unsigned width, height, depth_bits, stencil_bits, rgb565;
};
struct gl_hw_caps {
  unsigned version, generation, max_size, max_vertices, texture_units, texture_bytes;
};
/* GL bottom-left coordinates and the exact sw_vertex fixed-point scales.
 * State contains process-local RAM pointers for checkpoint/replay only.
 * Providers MUST copy/marshal values, never send these pointers to the engine.
 * texture[] names immutable provider-owned texture versions, including mips. */
struct gl_hw_command {
  struct sw_state state;
  unsigned texture[2], clear_mask, clear_color, clear_depth, clear_stencil, count;
  struct sw_vertex vertices[GL_HW_VERTICES];
};
struct gl_hw_driver {
  unsigned version, bytes;
  /* query succeeds only for qualified HARDWARE in this exact mode/format.
   * Software/unqualified/unimplemented/out-of-heap must return zero. */
  int (*query)(unsigned dc, const struct gl_hw_format *, struct gl_hw_caps *);
  int (*open)(unsigned dc, const struct gl_hw_format *, unsigned *context);
  /* close cancels/drains; on unproven idle the kernel quarantines resources. */
  void (*close)(unsigned context);
  /* Check ALL state, sampler, precision, origin and coverage semantics.
   * A NULL vertex list denotes clear; mask contains GL buffer bits.
   * 1=accept, 0=temporary refusal, negative=keep this context in software;
   * the caller drains/closes and replays its pending journal before fallback. */
  int (*accept)(unsigned context, const struct sw_state *, unsigned mask,
                const struct sw_vertex *, unsigned count);
  /* Upload/read full canonical top-down color and D16/D24/S8 attachments.
   * Uploads snapshot before return. read may fail after partial staging writes. */
  int (*load)(unsigned context, const struct sw_state *);
  int (*read)(unsigned context, struct sw_state *);
  int (*texture)(unsigned context, const struct sw_texture *, unsigned *handle);
  void (*free_texture)(unsigned context, unsigned handle);
  /* submit copies before return; zero means accepted, not completed.
   * finish is bounded and returns zero only on proven completion. */
  int (*submit)(unsigned context, const struct gl_hw_command *);
  int (*finish)(unsigned context);
  /* Optional completed GPU copy under the supplied existing present lease.
   * 1=completed, 0=unsupported (no effect), negative=failure. */
  int (*present)(unsigned context, unsigned token, unsigned generation,
                 unsigned width, unsigned height, unsigned bpp, unsigned pitch);
};
struct gl_context;
void gl_hw_probe(struct gl_context *);
void gl_hw_close(struct gl_context *);
void gl_hw_flush(struct gl_context *);
void gl_hw_sync(struct gl_context *);
void gl_hw_software(struct gl_context *);
int gl_hw_triangle(struct gl_context *, const struct sw_vertex *);
int gl_hw_clear(struct gl_context *, unsigned, unsigned, unsigned);
int gl_hw_present(struct gl_context *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
void gl_hw_texture_changed(struct sw_texture *);
int gl_hw_active(const struct gl_context *);
#endif
