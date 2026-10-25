/* Value-only USER/kernel boundaries; present.c itself runs its production path.
 * SPDX-License-Identifier: MIT */
#include <w32/opengl32.h>
#include <w32/shims.h>
#include <ciuki/present_abi.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>
struct gl_lease { DWORD active, token, generation, width, height, bpp, pitch; };
struct ciuki_w32_compat gl_test_compat;
HDC gl_test_dc;
unsigned gl_test_width, gl_test_height, gl_test_popup, gl_test_dead;
unsigned gl_test_lease, gl_test_releases, gl_test_damage, gl_test_failure;
unsigned gl_test_generation=12, gl_test_token=11;
unsigned gl_test_gamma_presenters, gl_test_gamma_updates, gl_test_gamma_serial;
unsigned gl_test_gamma_failure, gl_test_frames;
unsigned *gl_test_frame_pixel;
uint16_t gl_test_gamma_ramp[768];
#ifndef GL_TEST_NO_GAMMA
DWORD W32API w32_gdi_gamma_presenter(BOOL on) {
  if (on) gl_test_gamma_presenters++;
  else { assert(gl_test_gamma_presenters); gl_test_gamma_presenters--; }
  return gl_test_gamma_presenters;
}
DWORD W32API w32_gdi_gamma(uint16_t *out) {
  if (out) memcpy(out, gl_test_gamma_ramp, sizeof(gl_test_gamma_ramp));
  return gl_test_gamma_serial;
}
#endif
pthread_t gl_test_owner;
const struct ciuki_w32_compat *w32_shim_current(void) { return &gl_test_compat; }
BOOL w32_user_gl_target(DWORD dc, DWORD *v) {
  if (dc != gl_test_dc) return 0;
  if (gl_test_dead) return -1;
  v[0] = gl_test_width; v[1] = gl_test_height; v[2] = 7; v[3] = gl_test_popup;
  return pthread_equal(pthread_self(), gl_test_owner) ? 1 : -1;
}
BOOL w32_user_gl_damage(DWORD dc) {
  assert(dc == gl_test_dc && !gl_test_lease);
  gl_test_damage++;
  return !gl_test_failure && !gl_test_dead;
}
int32_t w32_user_display_lease(uintptr_t w, DWORD x, DWORD y, DWORD bpp, struct gl_lease *l) {
  assert(w == 7);
  *l = (struct gl_lease){1, gl_test_token, gl_test_generation, x, y, bpp, x * (bpp / 8)};
  gl_test_lease = 1;
  return 0;
}
void w32_user_display_release(uintptr_t w) {
  assert(w == 7 && gl_test_lease);
  gl_test_lease = 0; gl_test_releases++;
}
int32_t gl_host_display_frame(struct ciuki_w32_display_args *a, const void *p) {
  assert(gl_test_lease && p && a->token == gl_test_token && a->generation == gl_test_generation);
  assert(a->header.operation == W32_DISPLAY && a->header.size == sizeof(*a));
  if (a->action == W32_DISPLAY_GAMMA) {
    assert(!memcmp(p, gl_test_gamma_ramp, sizeof(gl_test_gamma_ramp)));
    gl_test_gamma_updates++;
    if (gl_test_gamma_failure == 1) return -1;
    if (gl_test_gamma_failure == 2) { a->error=5; return 0; }
    if (gl_test_gamma_failure == 3) return 0;
    a->result=1;
    return 0;
  }
  assert(a->action == W32_DISPLAY_FRAME);
  if (gl_test_frame_pixel) memcpy(gl_test_frame_pixel, p, sizeof(*gl_test_frame_pixel));
  gl_test_frames++;
  a->result = !gl_test_failure;
  return gl_test_failure ? -1 : 0;
}
