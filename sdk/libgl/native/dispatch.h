/* Dispatch boundary of the generated native entry points (api.c): the
 * engine's argument union and function ids, without its private headers.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_GL_DISPATCH_H
#define CIUKI_GL_DISPATCH_H
#include <stdint.h>
#include <string.h>
union gl_arg {
  int64_t i;
  double f;
  const void *p;
  void *result;
};
#include "../../win32/opengl32/ids.h"
union gl_arg w32_gl_dispatch(unsigned, const union gl_arg *);
static inline int ciuki_gl_strcmp(const char *a, const char *b) { return strcmp(a, b); }
#endif
