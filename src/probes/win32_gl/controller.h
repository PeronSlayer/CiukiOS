/* Copied result page; application stdout is not evidence.
 * SPDX-License-Identifier: MIT */
#ifndef W32_GL_CONTROLLER_H
#define W32_GL_CONTROLLER_H
#include <stdint.h>
#define W32_GL_RESULT_VA 0x10040000u
#define W32_GL_MAGIC 0x4c473357u
#define W32_GL_PATH "/System/Win32/Fixtures/gl-exercise.exe"
struct w32_gl_result {
  uint32_t magic, version, done, release, errors, first_error;
  uint32_t checksum[7], frames, extensions, pixel_formats, fonts;
  uint32_t render_width, render_height, reserved[17];
};
_Static_assert(sizeof(struct w32_gl_result) == 144,
               "common fixture result size");
static inline int w32_gl_valid(const struct w32_gl_result *r) {
  return r->magic == W32_GL_MAGIC && r->version == 1 && r->done == 1 &&
         r->frames == 7 && r->pixel_formats == 4 && r->extensions == 8;
}
static inline void
w32_gl_publish(const struct w32_gl_result *r, uint32_t exit_code,
               void (*emit)(const char *, const char *, const char *, ...)) {
  for (unsigned i = 0; i < 7; i++)
    emit("w32-gl", "DATA", "case=scene scene=%u checksum=%u frames=%u", i,
         r->checksum[i], r->frames);
  emit("w32-gl", "DATA",
       "case=capabilities extensions=%u pixel_formats=%u fonts=%u "
       "render_width=%u render_height=%u",
       r->extensions, r->pixel_formats, r->fonts, r->render_width,
       r->render_height);
  emit("w32-gl", "DATA", "case=fixture errors=%u first_error=%u exit=%u",
       r->errors, r->first_error, exit_code);
}
#endif
