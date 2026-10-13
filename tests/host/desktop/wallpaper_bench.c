/* SPDX-License-Identifier: MIT
 * -O2 benchmark and deterministic raw output using the production renderer. */
#define _POSIX_C_SOURCE 200809L
#include "wallpaper.h"
#include "settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double ms(void)
{
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t));
    return t.tv_sec*1000.0+t.tv_nsec/1000000.0;
}
int main(int argc, char **argv)
{
    assert(argc==3);
    FILE *f=fopen(argv[1],"rb"); assert(f);
    assert(!fseek(f,0,SEEK_END)); long n=ftell(f); assert(n>0); rewind(f);
    unsigned char *data=malloc((size_t)n); assert(data);
    assert(fread(data,1,(size_t)n,f)==(size_t)n); fclose(f);
    uint32_t *pixels=malloc(1024u*768u*4u); assert(pixels);
    double decode=0,scale=0;
    for (unsigned i=0;i<10;i++) {
        struct desk_image image;
        double start=ms(); assert(!desk_image_decode(data,(size_t)n,&image)); decode+=ms()-start;
        start=ms(); desk_image_scale(&image,pixels,1024,704,CFG_WALL_FILL,CFG_DESKTOP_COLOR); scale+=ms()-start;
        if (!i) {
            for (int mode=0;mode<CFG_WALL_MODES;mode++) {
                assert(!desk_wallpaper_render(pixels,1024,768,32,32,"/system/wallpapers/Ciuk2.png",mode,CFG_DESKTOP_COLOR,&image,NULL));
                char path[512]; snprintf(path,sizeof(path),"%s/%s.raw",argv[2],cfg_mode_names[mode]);
                f=fopen(path,"wb"); assert(f); assert(fwrite(pixels,4,1024u*768u,f)==1024u*768u); assert(!fclose(f));
            }
        }
        desk_image_free(&image);
    }
    printf("Ciuk2 decode_mean_ms=%.3f scale_fill_1024x704_mean_ms=%.3f iterations=10\n",decode/10,scale/10);
    free(data); free(pixels); return 0;
}
