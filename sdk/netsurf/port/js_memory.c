/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "docmem_query.h"
#include "js_memory.h"
union js_header { size_t size;long double alignment;void *pointer; };
static size_t charged,budget;
size_t ciuki_js_charged(void) { return charged; }
void *ciuki_js_alloc(size_t bytes)
{
    if(!bytes || bytes>SIZE_MAX-sizeof(union js_header))return NULL;
    size_t total=bytes+sizeof(union js_header);
    if(!charged)budget=ciuki_docmem_query()/8;
    if(total>budget || charged>budget-total)return NULL;
    union js_header *h=malloc(total);if(!h)return NULL;
    h->size=total;charged+=total;return h+1;
}
void ciuki_js_free(void *p)
{ if(p) { union js_header *h=(union js_header *)p-1;charged-=h->size;free(h); } }
void *ciuki_js_realloc(void *p,size_t bytes)
{
    if(!p)return ciuki_js_alloc(bytes);
    if(!bytes) { ciuki_js_free(p);return NULL; }
    if(bytes>SIZE_MAX-sizeof(union js_header))return NULL;
    union js_header *h=(union js_header *)p-1;size_t previous=h->size,total=bytes+sizeof(*h);
    if(total>previous && (total-previous>budget || charged>budget-(total-previous)))return NULL;
    union js_header *next=realloc(h,total);if(!next)return NULL;
    next->size=total;charged=charged-previous+total;return next+1;
}
