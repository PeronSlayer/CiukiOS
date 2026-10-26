/* Native render3d provider: the shared provider body (sdk/win32/opengl32/
 * provider.inc) over the copied W32_RENDER3D record of CIUKI_SYS_W32, which
 * the kernel accepts from native processes since f7-14. The capability
 * query is cached per frame so an unqualified machine pays one copied query
 * per frame, not per GL call. SPDX-License-Identifier: MIT */
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
#include "../../win32/opengl32/encode.h"
#include "../../win32/opengl32/admission.h"
#include "../include/ciuki/gl.h"
#ifndef CIUKI_GL_HOST_MODEL
#include <ciuki/runtime.h>
#endif
unsigned ciuki_gl_native_policy(void);
static int exchange(struct ciuki_w32_render3d_args *a, void *payload) {
  a->header = (struct ciuki_w32_header){1, sizeof(*a), W32_RENDER3D, 0};
  a->result = a->error = 0;
#ifdef CIUKI_GL_HOST_MODEL
  extern int ciuki_gl_host_exchange(struct ciuki_w32_render3d_args *, void *) __attribute__((weak));
  if (!ciuki_gl_host_exchange) return -ENOSYS;
  return ciuki_gl_host_exchange(a, payload);
#else
  a->address = (uint32_t)(uintptr_t)payload;
  int e = (int)ciuki_syscall(CIUKI_SYS_W32, (uint32_t)(uintptr_t)a, sizeof(*a), 0, 0, 0, 0);
  return e < 0 ? e : a->error ? -(int)a->error : a->result ? 0 : -EIO;
#endif
}
static int gl_provider_exchange(struct ciuki_w32_render3d_args *a, void *payload) { return exchange(a, payload); }
static void *provider_alloc(unsigned n) { return calloc(1, n); }
static void provider_free(void *p, unsigned n) { (void)n; free(p); }
#define GL_PROVIDER_ALLOC(n) provider_alloc(n)
#define GL_PROVIDER_FREE(p,n) provider_free(p,n)
#define GL_PROVIDER_COPY(d,s,n) memcpy(d,s,n)
#define GL_PROVIDER_ZERO(p,n) memset(p,0,n)
#include "../../win32/opengl32/provider.inc"

/* Per-frame capability cache: the engine probes at every call. */
static struct { int valid, result; struct gl_hw_format format; struct gl_hw_caps caps; } cache;
void ciuki_gl_native_frame(void) { cache.valid = 0; }
/* The last answered capability generation (the cache is dropped per frame). */
unsigned ciuki_gl_native_generation(void) { return cache.result ? cache.caps.generation : 0; }
static int cached_query(unsigned dc, const struct gl_hw_format *f, struct gl_hw_caps *out) {
  if (ciuki_gl_native_policy() == CIUKI_GL_BACKEND_SOFTWARE) return 0;
  if (!cache.valid || memcmp(&cache.format, f, sizeof(*f))) {
    cache.format = *f;
    cache.result = query(dc, f, &cache.caps);
    cache.valid = 1;
  }
  if (cache.result) *out = cache.caps;
  return cache.result;
}
int ciuki_gl_native_available(const struct gl_hw_format *f) {
  struct gl_hw_caps caps;
  cache.valid = 0;
  return cached_query(0, f, &caps);
}
static const struct gl_hw_driver native_provider = {GL_HW_VERSION, sizeof(native_provider), cached_query,
  open_provider, close_provider, accept, load, read_provider, texture, free_texture, submit, finish_provider, present};
const struct gl_hw_driver *ciuki_gl_native_render3d(unsigned version) {
  return version == GL_HW_VERSION ? &native_provider : 0;
}
