/* SPDX-License-Identifier: MIT
 * Link wrapping fails exactly one allocation, including calls in archives. */
#include <stddef.h>
static unsigned allocation_count,fail_allocation;
void *__real_malloc(size_t);
void *__real_calloc(size_t,size_t);
void *__real_realloc(void *,size_t);
void *__wrap_malloc(size_t n)
{ if(++allocation_count==fail_allocation)return NULL;return __real_malloc(n); }
void *__wrap_calloc(size_t n,size_t s)
{ if(++allocation_count==fail_allocation)return NULL;return __real_calloc(n,s); }
void *__wrap_realloc(void *p,size_t n)
{ if(++allocation_count==fail_allocation)return NULL;return __real_realloc(p,n); }
