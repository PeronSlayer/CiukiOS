#ifndef U32_COMPAT_STRING_H
#define U32_COMPAT_STRING_H
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
#endif
