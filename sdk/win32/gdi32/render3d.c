/* GDI's optional process-local OpenGL provider. Copied native wire only.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "internal.h"
#include "../opengl32/encode.h"
#include "../opengl32/admission.h"
static int exchange(struct ciuki_w32_render3d_args *a, void *payload) {
  a->header=(struct ciuki_w32_header){1,sizeof(*a),W32_RENDER3D,0};
  a->result=a->error=0;
#ifdef W32_HOST_MODEL
  extern int w32_render3d_host_exchange(struct ciuki_w32_render3d_args *,void *) __attribute__((weak));
  if (!w32_render3d_host_exchange) return -ENOSYS;
  return w32_render3d_host_exchange(a,payload);
#else
  extern DWORD w32r_gate(DWORD,DWORD,DWORD,DWORD);
  a->address=(DWORD)(uintptr_t)payload;
  int e=(int)w32r_gate(CIUKI_SYS_W32,(DWORD)(uintptr_t)a,sizeof(*a),0);
  return e<0 ? e : a->error ? -(int)a->error : a->result ? 0 : -EIO;
#endif
}
static int gl_provider_exchange(struct ciuki_w32_render3d_args *a,void *payload) { return exchange(a,payload); }
#define GL_PROVIDER_ALLOC(n) gdi_alloc(n)
#define GL_PROVIDER_FREE(p,n) gdi_free(p,n)
#define GL_PROVIDER_COPY(d,s,n) w32_copy(d,s,n)
#define GL_PROVIDER_ZERO(p,n) w32_fill(p,0,n)
#include "../opengl32/provider.inc"
#ifdef W32_HOST_MODEL
const struct gl_hw_driver *w32_gdi_render3d_native(unsigned version) {
  return version==GL_HW_VERSION ? &provider : 0;
}
__attribute__((weak))
#endif
const struct gl_hw_driver *W32API w32_gdi_render3d(unsigned version) {
  return version==GL_HW_VERSION ? &provider : 0;
}
