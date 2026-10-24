/* Authored RGB/RGBA fixtures and fail-every-allocation decoding.
 * SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "third_party/stb/stb_image_write.h"
#pragma clang diagnostic pop
#include "sdk/netsurf/port/raster.h"
#include "sdk/netsurf/port/image_memory.inc"
#include "allocation_fail.h"
static uint8_t encoded[8192];static size_t length;
static void encode(void *context,void *data,int bytes)
{ (void)context;assert(bytes>=0 && (size_t)bytes<=sizeof(encoded)-length);memcpy(encoded+length,data,(size_t)bytes);length+=(size_t)bytes; }
static void decode(int jpeg)
{
    allocation_count=0;int w=0,h=0;unsigned char *pixels=ciuki_raster_decode(encoded,length,&w,&h);assert(pixels && w==16 && h==16);
    for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++) {
        const uint8_t *p=pixels+(y*16+x)*4;assert(p[3]==255);
        unsigned channel=x<8?0:2;
        assert(p[channel]>=250 && p[1]<=5 && p[2-channel]<=5);
    }
    ciuki_image_free(pixels);assert(!ciuki_image_used());unsigned allocations=allocation_count;assert(allocations>2);
    for(unsigned i=1;i<=allocations;i++) {
        allocation_count=0;fail_allocation=i;pixels=ciuki_raster_decode(encoded,length,&w,&h);
        fail_allocation=0;ciuki_image_free(pixels);assert(!ciuki_image_used());
    }
    for(size_t n=0;n<(jpeg?20:length);n++) { pixels=ciuki_raster_decode(encoded,n,&w,&h);assert(!pixels);assert(!ciuki_image_used()); }
    assert(!ciuki_image_reserve(CIUKI_IMAGE_BYTES-16));pixels=ciuki_raster_decode(encoded,length,&w,&h);assert(!pixels);
    assert(ciuki_image_used()==CIUKI_IMAGE_BYTES-16);ciuki_image_release(CIUKI_IMAGE_BYTES-16);
    printf("PASS G26 %s: exact authored colours, malformed/truncated input, %u allocation failures, shared budget/cleanup\n",jpeg?"JPEG":"PNG",allocations);
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);assert(argc==4);uint8_t pixels[16*16*3];
    for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++) { uint8_t *p=pixels+(y*16+x)*3;p[0]=x<8?255:0;p[1]=0;p[2]=x<8?0:255; }
    for(int jpeg=0;jpeg<3;jpeg++) {
        length=0;
        if(jpeg) {
            FILE *fixture=fopen(argv[jpeg+1],"rb");assert(fixture);
            length=fread(encoded,1,sizeof(encoded),fixture);assert(length && feof(fixture));fclose(fixture);
        } else assert(stbi_write_png_to_func(encode,NULL,16,16,3,pixels,16*3));
        char path[600];snprintf(path,sizeof(path),"%s/%s",argv[1],jpeg==2?"progressive.jpg":jpeg?"colours.jpg":"colours.png");
        FILE *f=fopen(path,"wb");assert(f && fwrite(encoded,1,length,f)==length && !fclose(f));decode(jpeg);
    }
    return 0;
}
