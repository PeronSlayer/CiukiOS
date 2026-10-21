/* Value-only USER/kernel boundaries; present.c itself runs its production path.
 * SPDX-License-Identifier: MIT */
#include <w32/opengl32.h>
#include <w32/shims.h>
#include <ciuki/present_abi.h>
#include <assert.h>
#include <pthread.h>
struct gl_lease { DWORD active, token, generation, width, height, bpp, pitch; };
struct ciuki_w32_compat gl_test_compat;
HDC gl_test_dc;
unsigned gl_test_width, gl_test_height, gl_test_popup, gl_test_dead;
unsigned gl_test_lease, gl_test_releases, gl_test_damage, gl_test_failure;
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
  *l = (struct gl_lease){1, 11, 12, x, y, bpp, x * (bpp / 8)};
  gl_test_lease = 1;
  return 0;
}
void w32_user_display_release(uintptr_t w) {
  assert(w == 7 && gl_test_lease);
  gl_test_lease = 0; gl_test_releases++;
}
int32_t gl_host_display_frame(struct ciuki_w32_display_args *a, const void *p) {
  assert(gl_test_lease && p && a->token == 11 && a->generation == 12);
  a->result = !gl_test_failure;
  return gl_test_failure ? -1 : 0;
}
