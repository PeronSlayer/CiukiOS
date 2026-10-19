/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/base.h>
void *w32_copy(void *d,const void *s,uint32_t n){uint8_t *p=d;const uint8_t *q=s;for(uint32_t i=0;i<n;i++)p[i]=q[i];return d;}
void *w32_fill(void *d,uint8_t c,uint32_t n){uint8_t *p=d;while(n--)*p++=c;return d;}
uint32_t w32_strlen(const char *s){uint32_t n=0;if(s)while(s[n])n++;return n;}
uint16_t w32_u16(const void *s){const uint8_t *p=s;return (uint16_t)(p[0]|(uint16_t)p[1]<<8);}
uint32_t w32_u32(const void *s){const uint8_t *p=s;return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
void w32_put16(void *s,uint16_t n){uint8_t *p=s;p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
void w32_put32(void *s,uint32_t n){uint8_t *p=s;for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
