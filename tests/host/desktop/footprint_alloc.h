/* SPDX-License-Identifier: MIT
 * Call-site allocation accounting for production units only. The host libc,
 * fake surfaces and harness setup are outside the desktop's heap ledger. */
#ifndef DESK_FOOTPRINT_ALLOC_H
#define DESK_FOOTPRINT_ALLOC_H
#include <stdlib.h>
void *footprint_malloc(size_t, const char *, unsigned, const char *);
void *footprint_calloc(size_t, size_t, const char *, unsigned, const char *);
void *footprint_realloc(void *, size_t, const char *, unsigned, const char *);
char *footprint_strdup(const char *, const char *, unsigned, const char *);
void footprint_begin(const char *);
unsigned footprint_end(void);
void footprint_fail_once(const char *);
#ifndef FOOTPRINT_ALLOC_IMPLEMENTATION
#define malloc(n) footprint_malloc(n,__FILE__,__LINE__,__func__)
#define calloc(n,s) footprint_calloc(n,s,__FILE__,__LINE__,__func__)
#define realloc(p,n) footprint_realloc(p,n,__FILE__,__LINE__,__func__)
#define strdup(s) footprint_strdup(s,__FILE__,__LINE__,__func__)
#endif
#endif
