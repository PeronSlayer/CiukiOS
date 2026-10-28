/* Native OpenGL target binding, backend policy, completion accounting and
 * full-screen presentation (f7-01, f7-14). SPDX-License-Identifier: MIT */
#ifndef CIUKI_GL_H
#define CIUKI_GL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One context per process, one calling thread. Stride is in pixels.
 * Pixels remain caller-owned and must be writable until finish/rebind/close.
 * Invalid targets return -EINVAL; allocation failure returns -ENOMEM.
 * Initial creation resets GL state; later binds retain it, resetting the
 * viewport and scissor to the new target. The target is XRGB8888, top-down,
 * with a 16-bit depth attachment owned by the library. A packed target
 * (stride == width) is rendered in place; other strides use private rows
 * copied out by ciuki_gl_finish. Do not call the engine's TinyGL internals. */
int ciuki_gl_bind(uint32_t *pixels, unsigned width, unsigned height, unsigned stride);
/* Ends the frame: completes every queued hardware batch, reads the finished
 * target back into the caller's pixels and records the frame's backend. */
void ciuki_gl_finish(void);
void ciuki_gl_close(void);
unsigned ciuki_gl_generation(void);
/* Backend policy (contract 7.1): automatic uses the qualified hardware path
 * when the kernel advertises it for the bound target, software never opens
 * it, hardware refuses (-ENODEV) when it is unavailable and keeps rendering
 * in software. The policy applies from the next GL call. */
#define CIUKI_GL_BACKEND_AUTO 0u
#define CIUKI_GL_BACKEND_SOFTWARE 1u
#define CIUKI_GL_BACKEND_HARDWARE 2u
int ciuki_gl_set_backend(unsigned policy);
/* Completion accounting. A frame is a hardware frame when every draw and
 * clear of the frame executed on the provider; a frame with any scalar
 * draw is a software frame; a frame without draws is counted only in
 * frames. Frames end at ciuki_gl_finish or ciuki_gl_present.
 * refusals are temporary provider refusals
 * (software for that draw); fallbacks are permanent whole-context
 * fallbacks (contract 7.2). Counters are cumulative since bind. */
struct ciuki_gl_status {
    uint32_t size;             /* in: sizeof(struct ciuki_gl_status) */
    uint32_t policy;           /* CIUKI_GL_BACKEND_* requested */
    uint32_t backend;          /* CIUKI_GL_BACKEND_HARDWARE or _SOFTWARE now active */
    uint32_t hardware_failed;  /* permanent software fallback for this context */
    uint32_t generation;       /* provider capability generation, 0 without hardware */
    uint32_t reserved;
    uint64_t frames, hardware_frames, software_frames;
    uint64_t hardware_draws, software_draws, refusals, fallbacks;
    uint64_t presents, hardware_presents;
};
int ciuki_gl_status(struct ciuki_gl_status *out);
/* Full-screen presentation through the desktop's present lease
 * (DESK_LEASE / DESK_LEASE_EVENT). The bound target must match the lease
 * surface (width, height, XRGB8888). A qualified provider copies the
 * completed frame on the GPU and the kernel writes it to the scanout
 * (returns 1); the frame then stays resident on the provider and is not
 * read back into the caller's pixels: ciuki_gl_finish or a pixel read
 * makes them current, while a rebind or close before the next draw
 * releases them without writing (they may already be freed). Otherwise
 * the finished pixels are presented by the CPU frame path (returns 0).
 * Negative errno: the lease is no longer valid (-EACCES), the layout does
 * not match (-EINVAL) or the kernel refused. */
struct ciuki_gl_lease {
    uint32_t token, generation, width, height, bpp, pitch;
};
int ciuki_gl_present(const struct ciuki_gl_lease *lease);
/* Address of an advertised extension entry point by name (the
 * wglGetProcAddress set of opengl32), NULL for core or unknown names. */
void *ciuki_gl_get_proc_address(const char *name);
#ifdef __cplusplus
}
#endif
#endif
