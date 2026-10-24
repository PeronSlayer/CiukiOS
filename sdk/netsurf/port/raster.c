/* Reuse pinned libpng and stb_image JPEG (MIT alternative). All decoder and
 * output allocations share NetSurf's existing image ledger.
 * SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include "image_memory.h"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_MAX_DIMENSIONS 8192
#define STBI_MALLOC ciuki_image_malloc
#define STBI_REALLOC ciuki_image_realloc
#define STBI_FREE ciuki_image_free
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "third_party/stb/stb_image.h"
#pragma clang diagnostic pop
#include "raster.h"
struct png_source { const unsigned char *data; size_t bytes,offset; unsigned char *pixels; png_bytep *rows; };
static png_voidp raster_png_alloc(png_structp png,png_alloc_size_t n)
{ (void)png;return ciuki_image_malloc(n); }
static void raster_png_free(png_structp png,png_voidp p)
{ (void)png;ciuki_image_free(p); }
static void raster_png_read(png_structp png,png_bytep out,png_size_t n)
{
    struct png_source *s=png_get_io_ptr(png);
    if(n>s->bytes-s->offset)png_error(png,"Truncated PNG");
    memcpy(out,s->data+s->offset,n);s->offset+=n;
}
static unsigned char *raster_png(const unsigned char *data,size_t bytes,int *width,int *height)
{
    struct png_source *s=ciuki_image_calloc(1,sizeof(*s));if(!s)return NULL;
    s->data=data;s->bytes=bytes;
    png_structp png=png_create_read_struct_2(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL,NULL,raster_png_alloc,raster_png_free);
    if(!png) { ciuki_image_free(s);return NULL; }
    png_infop info=png_create_info_struct(png);
    if(!info) { png_destroy_read_struct(&png,NULL,NULL);ciuki_image_free(s);return NULL; }
    if(setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png,&info,NULL);ciuki_image_free(s->rows);ciuki_image_free(s->pixels);ciuki_image_free(s);return NULL;
    }
    png_set_read_fn(png,s,raster_png_read);png_set_user_limits(png,8192,8192);
    png_set_chunk_malloc_max(png,CIUKI_IMAGE_BYTES);png_set_chunk_cache_max(png,128);
    png_read_info(png,info);
    unsigned w=png_get_image_width(png,info),h=png_get_image_height(png,info);
    if(!w || !h || (size_t)w*h>CIUKI_IMAGE_BYTES/4)png_error(png,"Image exceeds available memory");
    int colour=png_get_color_type(png,info),depth=png_get_bit_depth(png,info);
    if(depth==16)png_set_strip_16(png);
    if(colour==PNG_COLOR_TYPE_PALETTE)png_set_palette_to_rgb(png);
    if(colour==PNG_COLOR_TYPE_GRAY && depth<8)png_set_expand_gray_1_2_4_to_8(png);
    int transparent=png_get_valid(png,info,PNG_INFO_tRNS);
    if(transparent)png_set_tRNS_to_alpha(png);
    if(colour==PNG_COLOR_TYPE_GRAY || colour==PNG_COLOR_TYPE_GRAY_ALPHA)png_set_gray_to_rgb(png);
    if(!(colour&PNG_COLOR_MASK_ALPHA) && !transparent)png_set_add_alpha(png,255,PNG_FILLER_AFTER);
    png_set_interlace_handling(png);png_read_update_info(png,info);
    if(png_get_rowbytes(png,info)!=(size_t)w*4)png_error(png,"Unexpected PNG row size");
    s->pixels=ciuki_image_malloc((size_t)w*h*4);s->rows=ciuki_image_malloc((size_t)h*sizeof(*s->rows));
    if(!s->pixels || !s->rows)png_error(png,"Image exceeds available memory");
    for(unsigned y=0;y<h;y++)s->rows[y]=s->pixels+(size_t)y*w*4;
    png_read_image(png,s->rows);png_read_end(png,info);
    png_destroy_read_struct(&png,&info,NULL);ciuki_image_free(s->rows);
    unsigned char *pixels=s->pixels;ciuki_image_free(s);*width=(int)w;*height=(int)h;return pixels;
}
unsigned char *ciuki_raster_decode(const unsigned char *data,size_t bytes,int *width,int *height)
{
    int w,h,channels;
    if(bytes>8u*1024u*1024u)return NULL;
    if(bytes>=8 && !png_sig_cmp(data,0,8))return raster_png(data,bytes,width,height);
    if(!stbi_info_from_memory(data,(int)bytes,&w,&h,&channels) ||
       w<=0 || h<=0 || w>8192 || h>8192 || (size_t)w*(size_t)h>CIUKI_IMAGE_BYTES/4)return NULL;
    unsigned char *pixels=stbi_load_from_memory(data,(int)bytes,&w,&h,&channels,4);
    if(pixels) { *width=w;*height=h; }
    return pixels;
}
