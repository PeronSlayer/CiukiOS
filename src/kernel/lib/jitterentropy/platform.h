/* Ciuki replaces only upstream's OS-specific header. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_JENT_PLATFORM_H
#define CIUKI_JENT_PLATFORM_H
#define _JITTERENTROPY_BASE_USER_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef intptr_t ssize_t;
#ifndef EAGAIN
#define EAGAIN 11
#endif
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
void ciuki_jent_clock(uint64_t *);
void *ciuki_jent_alloc(size_t);
void ciuki_jent_free(void *, size_t);
void ciuki_jent_erase(void *, size_t);
struct rand_data;
bool ciuki_jent_round(struct rand_data *);
void ciuki_jent_raw(struct rand_data *, uint64_t);
#define jent_get_nstime ciuki_jent_clock
#define jent_zalloc ciuki_jent_alloc
#define jent_zfree ciuki_jent_free
#define jent_memset_secure ciuki_jent_erase
static inline int jent_fips_enabled(void) { return 1; }
/* No cache enumeration at the boot guard safe point: upstream's 256 KiB default. */
static inline uint32_t jent_cache_size_roundup(int all) { (void)all; return 0; }
static inline void jent_yield(void) { }
static inline uint64_t rol64(uint64_t x, int n)
{ return (x << (n & 63)) | (x >> ((64 - n) & 63)); }
#endif
