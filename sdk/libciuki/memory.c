/* SPDX-License-Identifier: MIT */
#include <ciuki/runtime.h>
#include <malloc.h>
#include <errno.h>
#include <stdlib.h>
int posix_memalign(void **out,size_t alignment,size_t size) {
    if(alignment<sizeof(void *)||(alignment&(alignment-1)))return EINVAL;
    int saved=errno;void *p=_memalign_r(__getreent(),alignment,size);errno=saved;
    if(!p)return ENOMEM;*out=p;return 0;
}
void *aligned_alloc(size_t alignment,size_t size) {
    if(!alignment||(alignment&(alignment-1))||size%alignment) { errno=EINVAL;return NULL; }
    return _memalign_r(__getreent(),alignment,size);
}
