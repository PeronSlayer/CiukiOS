/* SPDX-License-Identifier: MIT
 * Shared ordinary entry ordering for Files and desktop Arrange Icons.
 * Callers handle folders/system icons and special namespace columns. */
#ifndef CIUKI_FILES_ENTRY_ORDER_H
#define CIUKI_FILES_ENTRY_ORDER_H
#include "model.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
/* Sort entries or placement indices without changing the caller's identities.
 * Allocation failure leaves the array untouched. Equal keys retain their order. */
static inline int fm_stable_sort(void *values, size_t count, size_t size,
    int (*order)(const void *, const void *, const void *), const void *context)
{
    if (count<2) return 1;
    if (!size || count>SIZE_MAX/size) return 0;
    unsigned char *items=values, *temp=malloc(count*size);
    if (!temp) return 0;
    for (size_t span=1;span<count;) {
        for (size_t start=0;start<count;) {
            size_t middle=start+(span<count-start ? span : count-start);
            size_t end=middle+(span<count-middle ? span : count-middle);
            size_t left=start,right=middle;
            for (size_t i=start;i<end;i++) {
                size_t pick=left<middle && (right==end ||
                    order(items+left*size,items+right*size,context)<=0) ? left++ : right++;
                memcpy(temp+i*size,items+pick*size,size);
            }
            memcpy(items+start*size,temp+start*size,(end-start)*size);
            start=end;
        }
        if (span>count/2) break;
        span*=2;
    }
    free(temp);
    return 1;
}
static inline int fm_entry_order(const struct fm_entry *a, const struct fm_entry *b, unsigned column)
{
    int result=0;
    if (column==1) result=a->meta.size<b->meta.size ? -1 : a->meta.size>b->meta.size;
    else if (column==2) {
        char x[32],y[32];
        result=strcasecmp(fm_type_name(a,x,sizeof(x)),fm_type_name(b,y,sizeof(y)));
    }
    else if (column==3) result=a->meta.modified<b->meta.modified ? -1 : a->meta.modified>b->meta.modified;
    if (!result) result=strcasecmp(a->name,b->name);
    if (!result) result=strcmp(a->name,b->name);
    return result;
}
#endif
