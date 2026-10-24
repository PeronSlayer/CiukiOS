#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sdk/netsurf/port/js_memory.h"
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);assert(!ciuki_js_charged() && !ciuki_js_alloc(0));
    unsigned char *p=ciuki_js_alloc(900);assert(p && !((uintptr_t)p%_Alignof(long double)));
    memset(p,0x52,900);size_t charged=ciuki_js_charged();assert(charged>=900 && charged<1024);
    assert(!ciuki_js_alloc(200) && !ciuki_js_realloc(p,2048) && ciuki_js_charged()==charged && p[899]==0x52);
    p=ciuki_js_realloc(p,100);assert(p && p[99]==0x52 && ciuki_js_charged()<charged);
    void *other_heap=ciuki_js_alloc(400);assert(other_heap && ciuki_js_charged()>500);
    ciuki_js_free(other_heap);assert(!ciuki_js_realloc(p,0) && !ciuki_js_charged());
    assert(!ciuki_js_alloc(SIZE_MAX));
    puts("PASS G26B shared JavaScript heap admission, aligned headers, charged realloc/shrink/OOM preservation/free/overflow");return 0;
}
