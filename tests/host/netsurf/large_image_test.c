/* SPDX-License-Identifier: MIT
 * Actual content decoders, with only the bitmap/llcache boundary modelled.
 * Fixtures are authored 1600x1200 quadrants and partial GIF animation frames. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "allocation_fail.h"
#include "sdk/netsurf/port/image_memory.h"
#if defined(TEST_GIF)
#include "src/gif.c"
#undef malloc
#undef calloc
#undef realloc
#undef free
#else
#ifdef TEST_PNG
#include "image/png.c"
#else
#include "image/webp.c"
#endif
#endif
struct test_bitmap { unsigned width,height; bool opaque; _Alignas(uint32_t) uint8_t pixels[]; };
static void *create(int w,int h)
{
    if(w<=0 || h<=0 || w>4096 || h>4096 || (size_t)w*h>1024u*1024u)return NULL;
    struct test_bitmap *b=ciuki_image_calloc(1,sizeof(*b)+(size_t)w*h*4);
    if(b) { b->width=w;b->height=h; }return b;
}
static void destroy(void *b) { ciuki_image_free(b); }
static unsigned char *buffer(void *b) { return ((struct test_bitmap *)b)->pixels; }
#ifndef TEST_GIF
static size_t stride(void *b) { return ((struct test_bitmap *)b)->width*4; }
#endif
static void opaque_set(void *b,bool value) { ((struct test_bitmap *)b)->opaque=value; }
static bool opaque_get(void *b) { return ((struct test_bitmap *)b)->opaque; }
static void modified(void *b) { assert(b); }
#ifndef TEST_GIF
static int width(void *b) { return ((struct test_bitmap *)b)->width; }
static int height(void *b) { return ((struct test_bitmap *)b)->height; }
#endif
static uint8_t *source,*samples;
static size_t source_bytes;
static unsigned frames;
static size_t peak;
#ifdef TEST_GIF
static const unsigned edges[][2]={
    {16,32},{20,28},{20,32},{1300,900},{1304,900},{1300,904},
    {900,604},{904,600},{904,604},{1500,1100},{1504,1100},{1500,1104},
    {300,204},{304,200},{304,204},{500,400},{504,400},{500,404}};
enum { sample_count=16*12+sizeof(edges)/sizeof(*edges) };
#else
enum { sample_count=16*12 };
#endif
static void check_pixel(struct test_bitmap *b,unsigned x,unsigned y,unsigned *pos)
{
    unsigned scale=1600/b->width;
    const uint8_t *pixel=b->pixels+((size_t)(y/scale)*b->width+x/scale)*4;
    const uint8_t *ref=samples+*pos;*pos+=4;
    assert(pixel[3]==ref[3]);
    if(ref[3])for(unsigned c=0;c<3;c++) {
        int delta=(int)pixel[c]-ref[c];assert(delta>=-8 && delta<=8);
    }
}
static void check(struct test_bitmap *b,unsigned frame)
{
    assert(b && b->width<1600 && b->height<1200);
    assert(b->width*1200==b->height*1600);
    unsigned scale=1600/b->width;
    assert(scale>1);
    unsigned pos=frame*sample_count*4;
    for(unsigned y=40;y<1200;y+=100)for(unsigned x=40;x<1600;x+=100)check_pixel(b,x,y,&pos);
#ifdef TEST_GIF
    for(unsigned i=0;i<sizeof(edges)/sizeof(*edges);i++)check_pixel(b,edges[i][0],edges[i][1],&pos);
#endif
    if(ciuki_image_used()>peak)peak=ciuki_image_used();
    assert(ciuki_image_used()<=ciuki_image_limit());
}
#ifndef TEST_GIF
static void *bitmap_create(int w,int h,enum gui_bitmap_flags flags) { (void)flags;return create(w,h); }
static struct gui_bitmap_table bitmap_ops={.create=bitmap_create,.destroy=destroy,.get_buffer=buffer,
    .get_rowstride=stride,.get_width=width,.get_height=height,.set_opaque=opaque_set,.get_opaque=opaque_get,.modified=modified};
static struct netsurf_table gui={.bitmap=&bitmap_ops};
struct netsurf_table *guit=&gui;
bool verbose_log;
const uint8_t *content__get_source_data(struct content *c,size_t *n) { (void)c;*n=source_bytes;return source; }
void content_broadcast_error(struct content *c,nserror err,const char *msg) { (void)c;(void)err;(void)msg; }
void nslog_log(const char *file,const char *fn,int line,const char *fmt,...) { (void)file;(void)fn;(void)line;(void)fmt; }
bool image_cache_speculate(struct content *c) { (void)c;return true; }
#endif
static unsigned decode(bool verify)
{
    size_t initial=ciuki_image_used();allocation_count=0;
#ifdef TEST_GIF
    const nsgif_bitmap_cb_vt ops={.create=create,.destroy=destroy,.get_buffer=buffer,
        .set_opaque=opaque_set,.test_opaque=opaque_get,.modified=modified};
    nsgif_t *gif=NULL;
    nsgif_error error=nsgif_create(&ops,NSGIF_BITMAP_FMT_R8G8B8A8,&gif);
    if(error==NSGIF_OK) {
        error=nsgif_data_scan(gif,source_bytes,source);
        if(error==NSGIF_OK) {
            nsgif_data_complete(gif);assert(gif->info.width==1600 && gif->info.height==1200);
            assert(gif->info.frame_count==frames);
            for(unsigned i=0;i<frames;i++) {
                nsgif_bitmap_t *b=NULL;error=nsgif_frame_decode(gif,i,&b);
                if(error!=NSGIF_OK)break;
                if(verify)check(b,i);
            }
        }
    }
    if(verify)assert(error==NSGIF_OK);
    nsgif_destroy(gif);
#else
    bitmap_set_format(&(bitmap_fmt_t){.layout=BITMAP_LAYOUT_R8G8B8A8,.pma=false});
    struct content c={0};
#ifdef TEST_PNG
    (void)nspng_create;(void)nspng_process_data;(void)nspng_convert;(void)nspng_clone;
    struct test_bitmap *b=(void *)png_cache_convert(&c);
#else
    (void)webp_create;(void)webp_convert;(void)webp_clone;
    struct test_bitmap *b=(void *)webp_cache_convert(&c);
#endif
    if(verify)check(b,0);
    if(b)destroy(b);
#endif
    assert(ciuki_image_used()==initial);return allocation_count;
}
int main(int argc,char **argv)
{
    assert(argc==4);frames=(unsigned)atoi(argv[3]);
    FILE *f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));
    source_bytes=ftell(f);rewind(f);source=malloc(source_bytes);assert(source);
    assert(fread(source,1,source_bytes,f)==source_bytes);fclose(f);
    samples=malloc(frames*sample_count*4);assert(samples);f=fopen(argv[2],"rb");assert(f);
    assert(fread(samples,1,frames*sample_count*4,f)==frames*sample_count*4);fclose(f);
    unsigned count=decode(true);assert(count);
    /* Decoder/bitmap allocation failures leave the ledger reusable. */
    for(unsigned i=1;i<=count;i++) { fail_allocation=i;decode(false);fail_allocation=0; }
    decode(true);
    uint32_t w,h;
    assert(!ciuki_image_scale(0,1,2,&w,&h));
    assert(!ciuki_image_scale(65536,1,2,&w,&h));
    assert(!ciuki_image_reserve(ciuki_image_limit()));
    decode(false);ciuki_image_release(ciuki_image_limit());
    decode(true);
    printf("PASS FX-24B %s: %u frames, %u pixel samples/frame, %u allocation failures, post-decode ledger max %zu / %zu\n",argv[1],frames,sample_count,count,peak,ciuki_image_limit());
    free(source);free(samples);fflush(stdout);return 0;
}
