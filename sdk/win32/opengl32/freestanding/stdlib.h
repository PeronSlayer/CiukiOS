#ifndef GL_FREESTANDING_STDLIB_H
#define GL_FREESTANDING_STDLIB_H
#include <stddef.h>
void *malloc(size_t);
void *calloc(size_t, size_t);
void free(void *);
__attribute__((noreturn)) void exit(int);
static inline int abs(int v) { return v < 0 ? -v : v; }
#endif
