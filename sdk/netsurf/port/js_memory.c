/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "docmem_query.h"
#include "js_memory.h"
#if defined(__i386__) && !defined(CIUKI_HOST)
#include <ciuki/abi.h>
#include "logline.h"
/* Diagnostic (QEMU 2026-10-25: the main 1 MiB stack overflowed at exit with
 * duk__mark_heaphdr on top; Duktape's own mark recursion is capped at 256
 * levels, so something else had used the stack). The first JavaScript
 * allocation made with less than 128 KiB of main stack left logs the frame
 * chain (the browser is built with frame pointers): innermost and outermost
 * return addresses and the most frequent ones, which name the recursion.
 * Resolve them with llvm-symbolizer against Programs/netsurf. */
static int stack_reported;
static void stack_probe(void)
{
    uintptr_t sp=(uintptr_t)__builtin_frame_address(0);
    if(stack_reported || sp<CIUKI_MAIN_STACK_BASE || sp>=CIUKI_MAIN_STACK_BASE+128u*1024u)return;
    stack_reported=1;
    uintptr_t kind[48],inner[10],outer[10];unsigned count[48],kinds=0,frames=0;
    for(uintptr_t *fp=(uintptr_t *)sp;(uintptr_t)fp>=sp && (uintptr_t)fp<CIUKI_MAIN_STACK_LIMIT-8;frames++) {
        uintptr_t ret=fp[1];
        if(frames<10)inner[frames]=ret;
        outer[frames%10]=ret;
        unsigned k=0;while(k<kinds && kind[k]!=ret)k++;
        if(k<kinds)count[k]++; else if(kinds<48) { kind[kinds]=ret;count[kinds++]=1; }
        uintptr_t *next=(uintptr_t *)fp[0];
        if(next<=fp)break;
        fp=next;
    }
    char text[CIUKI_LOG_LINE_MAX-64];size_t used=0;
    for(unsigned i=0;i<frames && i<10 && used+12<sizeof(text);i++)used+=(size_t)snprintf(text+used,sizeof(text)-used," %#x",(unsigned)inner[i]);
    ciuki_log_line("[netsurf] deep stack sp=%#x frames=%u inner:%s",(unsigned)sp,frames,text);
    used=0;text[0]=0;
    for(unsigned i=frames>10?frames-10:0;i<frames && used+12<sizeof(text);i++)used+=(size_t)snprintf(text+used,sizeof(text)-used," %#x",(unsigned)outer[i%10]);
    ciuki_log_line("[netsurf] deep stack outer:%s",text);
    used=0;text[0]=0;
    for(unsigned shown=0;shown<16 && used+24<sizeof(text);shown++) {
        unsigned best=0;for(unsigned k=1;k<kinds;k++)if(count[k]>count[best])best=k;
        if(!kinds || !count[best])break;
        used+=(size_t)snprintf(text+used,sizeof(text)-used," %#x*%u",(unsigned)kind[best],count[best]);count[best]=0;
    }
    ciuki_log_line("[netsurf] deep stack top:%s",text);
}
#else
static void stack_probe(void) {}
#endif
union js_header { size_t size;long double alignment;void *pointer; };
bool ciuki_js_exiting;
static size_t charged,budget;
static unsigned long refusals;
size_t ciuki_js_charged(void) { return charged; }
size_t ciuki_js_available(void)
{
    if(!charged)budget=ciuki_docmem_query()/8;
    return budget>charged?budget-charged:0;
}
unsigned long ciuki_js_refusals(void) { return refusals; }
void *ciuki_js_alloc(size_t bytes)
{
    stack_probe();
    if(!bytes || bytes>SIZE_MAX-sizeof(union js_header))return NULL;
    size_t total=bytes+sizeof(union js_header);
    if(!charged)budget=ciuki_docmem_query()/8;
    if(total>budget || charged>budget-total) { refusals++;return NULL; }
    union js_header *h=malloc(total);if(!h) { refusals++;return NULL; }
    h->size=total;charged+=total;return h+1;
}
void ciuki_js_free(void *p)
{ if(p) { union js_header *h=(union js_header *)p-1;charged-=h->size;free(h); } }
void *ciuki_js_realloc(void *p,size_t bytes)
{
    stack_probe();
    if(!p)return ciuki_js_alloc(bytes);
    if(!bytes) { ciuki_js_free(p);return NULL; }
    if(bytes>SIZE_MAX-sizeof(union js_header))return NULL;
    union js_header *h=(union js_header *)p-1;size_t previous=h->size,total=bytes+sizeof(*h);
    if(total>previous && (total-previous>budget || charged>budget-(total-previous))) { refusals++;return NULL; }
    union js_header *next=realloc(h,total);if(!next) { refusals++;return NULL; }
    next->size=total;charged=charged-previous+total;return next+1;
}
