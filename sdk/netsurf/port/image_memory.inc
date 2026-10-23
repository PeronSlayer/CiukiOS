/* SPDX-License-Identifier: MIT
 * Compiled once in libnsgif. A failed resize preserves both allocation and
 * charge. Headers are aligned and charged too; bitmap reservations include
 * a conservative 4096 bytes for nsfb's small object/routines/plotter tables. */
#include <stdint.h>
#include <string.h>
#include "sdk/netsurf/port/image_memory.h"
#include "sdk/netsurf/port/docmem_query.h"
static size_t image_bytes,image_limit;
size_t ciuki_image_limit(void)
{
    if(!image_limit) {
        size_t share=ciuki_docmem_query()/8;
        image_limit=share<CIUKI_IMAGE_MIN_BYTES?CIUKI_IMAGE_MIN_BYTES:share>CIUKI_IMAGE_MAX_BYTES?CIUKI_IMAGE_MAX_BYTES:share;
    }
    return image_limit;
}
union image_header { size_t bytes; long double align; void *pointer; };
int ciuki_image_reserve(size_t n)
{
    if(n>CIUKI_IMAGE_BYTES-image_bytes)return -1;
    image_bytes+=n;return 0;
}
void ciuki_image_release(size_t n) { image_bytes-=n; }
size_t ciuki_image_used(void) { return image_bytes; }
void *ciuki_image_malloc(size_t n)
{
    if(n>SIZE_MAX-sizeof(union image_header))return NULL;
    size_t total=n+sizeof(union image_header);
    if(ciuki_image_reserve(total))return NULL;
    union image_header *h=malloc(total);
    if(!h) { ciuki_image_release(total);return NULL; }
    h->bytes=total;return h+1;
}
void ciuki_image_free(void *p)
{
    if(!p)return;
    union image_header *h=(union image_header *)p-1;
    ciuki_image_release(h->bytes);free(h);
}
void *ciuki_image_calloc(size_t n,size_t size)
{
    if(size && n>SIZE_MAX/size)return NULL;
    void *p=ciuki_image_malloc(n*size);
    if(p)memset(p,0,n*size);
    return p;
}
void *ciuki_image_realloc(void *p,size_t n)
{
    if(!p)return ciuki_image_malloc(n);
    if(!n) { ciuki_image_free(p);return NULL; }
    if(n>SIZE_MAX-sizeof(union image_header))return NULL;
    union image_header *h=(union image_header *)p-1;
    size_t old=h->bytes,total=n+sizeof(*h);
    if(total>old && ciuki_image_reserve(total-old))return NULL;
    union image_header *next=realloc(h,total);
    if(!next) { if(total>old)ciuki_image_release(total-old);return NULL; }
    if(total<old)ciuki_image_release(old-total);
    next->bytes=total;return next+1;
}
