/* Portable XRGB8888 target for TinyGL. SPDX-License-Identifier: MIT */
#ifndef CIUKI_GL_H
#define CIUKI_GL_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One context per process, one calling thread. Stride is in pixels.
 * Pixels remain caller-owned and must be writable until finish/rebind/close.
 * Invalid targets return -EINVAL; allocation failure returns -ENOMEM.
 * Initial creation resets GL state; later binds retain it, resetting viewport.
 * Do not call glInit/glClose/ZB_* directly with this adapter. */
int ciuki_gl_bind(uint32_t *pixels, unsigned width, unsigned height, unsigned stride);
void ciuki_gl_finish(void);
void ciuki_gl_close(void);
unsigned ciuki_gl_generation(void);
#ifdef __cplusplus
}
#endif
#endif
