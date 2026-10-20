/* Freestanding compiler helpers. SPDX-License-Identifier: LGPL-2.1-or-later */
#include <w32/base.h>
int w32_strcmp(const char *a,const char *b){while(*a && *a==*b){a++;b++;}return (uint8_t)*a-(uint8_t)*b;}
static unsigned lower(unsigned c){return c>='A' && c<='Z'?c+32:c;}
int w32_stricmp(const char *a,const char *b){while(*a && lower((uint8_t)*a)==lower((uint8_t)*b)){a++;b++;}return (int)lower((uint8_t)*a)-(int)lower((uint8_t)*b);}
int w32_copy_string(char *out,uint32_t cap,const char *s){uint32_t n=0;while(n<cap && s[n])n++;if(n==cap)return 0;for(uint32_t i=0;i<=n;i++)out[i]=s[i];return 1;}
#ifndef W32_HOST_MODEL
void *memset(void *p,int value,size_t n){uint8_t *b=p;for(size_t i=0;i<n;i++)b[i]=(uint8_t)value;return p;}
void *memcpy(void *p,const void *s,size_t n){uint8_t *b=p;const uint8_t *a=s;for(size_t i=0;i<n;i++)b[i]=a[i];return p;}
#endif
