/* Native build shim for the shared GL engine (sdk/win32/opengl32): the few
 * Win32 handle types and byte helpers the engine sources name, over libc.
 * No Win32 service, loader or module lookup exists in a native program.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_GL_PLATFORM_H
#define CIUKI_GL_PLATFORM_H
#include <stdint.h>
#include <string.h>
#define W32API
typedef uint32_t DWORD, HDC, HGLRC, HBITMAP;
typedef int BOOL;
typedef int32_t LONG;
static inline void *w32_copy(void *d, const void *s, uint32_t n) { return memcpy(d, s, n); }
static inline void *w32_fill(void *d, int c, uint32_t n) { return memset(d, c, n); }
static inline uint32_t w32_strlen(const char *s) { return (uint32_t)strlen(s); }
static inline int w32_strcmp(const char *a, const char *b) { return strcmp(a, b); }
/* Array/list replay entries of the engine, defined by the generated api.c. */
void w32_glBegin(GLenum);
void w32_glEnd(void);
void w32_glVertex4f(GLfloat, GLfloat, GLfloat, GLfloat);
void w32_glColor4f(GLfloat, GLfloat, GLfloat, GLfloat);
void w32_glNormal3f(GLfloat, GLfloat, GLfloat);
/* Provided by sdk/libgl/native/context.c and provider.c. */
const char *ciuki_gl_native_option(const char *name);
void ciuki_gl_native_log(const char *line, unsigned bytes);
#endif
