/* SPDX-License-Identifier: MIT -- private little-endian codec helpers */
#ifndef ACCT_WIRE_H
#define ACCT_WIRE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static inline uint32_t a32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
static inline uint64_t a64(const uint8_t *p)
{ return a32(p) | (uint64_t)a32(p+4)<<32; }
static inline void w32(uint8_t *p, uint32_t x)
{ for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(x>>(8*i)); }
static inline void w64(uint8_t *p, uint64_t x)
{ w32(p,(uint32_t)x); w32(p+4,(uint32_t)(x>>32)); }
static inline int azero(const void *v, size_t n)
{ const uint8_t *p=v; unsigned b=0; while(n--) b|=*p++; return !b; }
static inline int apadded(const void *v, size_t n)
{ const char *p=v, *e=memchr(p,0,n); return e && azero(e,n-(size_t)(e-p)); }
#endif
