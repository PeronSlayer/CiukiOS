/* Independent patterned-image oracle for the production surface presenter.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/kernel/vm/vga_presenter.h"
static uint32_t digest(const uint8_t *p,size_t n)
{ uint32_t h=2166136261u;while(n--)h=(h^*p++)*16777619u;return h; }
static uint32_t oracle(unsigned format,const uint8_t *p,const uint32_t *pal,
                       const struct ciuki_present_gamma *g,const cvp_format *f)
{
    unsigned c[3];
    if(format==CIUKI_PRESENT_INDEX8){uint32_t v=pal[*p];c[0]=(v>>16)&255;c[1]=(v>>8)&255;c[2]=v&255;}
    else if(format==CIUKI_PRESENT_XRGB8888){c[0]=p[2];c[1]=p[1];c[2]=p[0];}
    else {
        unsigned v=p[0]+256u*p[1],bits=format==CIUKI_PRESENT_RGB565?6:5;
        unsigned raw[3]={v>>(5+bits)&31,v>>5&((1u<<bits)-1),v&31};
        for(unsigned i=0;i<3;i++){
            unsigned n=i==1?bits:5;
            /* Exact bit replication expressed independently as a ratio. */
            c[i]=raw[i]*(256u/(1u<<n))+raw[i]/(1u<<(2*n-8));
        }
    }
    if(g){c[0]=g->red[c[0]]/256;c[1]=g->green[c[1]]/256;c[2]=g->blue[c[2]]/256;}
    return (c[0]/(1u<<(8-f->red_size))<<f->red_pos)|
           (c[1]/(1u<<(8-f->green_size))<<f->green_pos)|
           (c[2]/(1u<<(8-f->blue_size))<<f->blue_pos);
}
static void vectors(void)
{
    uint32_t pal[256];struct ciuki_present_gamma gamma;
    for(unsigned i=0;i<256;i++){
        pal[i]=((i*17&255)<<16)|((i*7&255)<<8)|(i*3&255);
        gamma.red[i]=(uint16_t)((255-i)*257);gamma.green[i]=(uint16_t)((i/2)*257);gamma.blue[i]=(uint16_t)((i^85)*257);
    }
    static const uint32_t pinned[24]={0x53ccd989u,0xaadca561u,0xad3125d4u,0xbf3ab614u,0xa518430fu,0x3a56078fu,0x1220696cu,0x233ec39bu,0x3256addcu,0xb09dc4bcu,0x001f30b8u,0xabb164d1u,0xdd49ba39u,0xc18f1909u,0x53a62934u,0xd28119dcu,0x1825c12fu,0x0a5c5f9bu,0x129e3301u,0x848dac7bu,0x237bc244u,0x6de807fcu,0x461450ffu,0xdab1ee59u};
    unsigned vector=0;
    for(unsigned format=1;format<=4;format++)for(unsigned scale=1;scale<=3;scale++)for(unsigned depth=0;depth<2;depth++){
        unsigned bytes=cvp_surface_bytes(format);
        struct ciuki_present_surface s={17,11,17*bytes+5,(17*bytes+5)*11,format};
        cvp_format f={.width=17*scale+7,.height=11*scale+9,.bytes=depth?4:2,
            .red_size=depth?8:5,.red_pos=depth?16:11,.green_size=depth?8:6,.green_pos=depth?8:5,.blue_size=depth?8:5};
        f.pitch=f.width*f.bytes+13;
        size_t cap=f.pitch*f.height;
        uint8_t *source=malloc(s.size+16),*target=malloc(cap+32),*expected=malloc(cap);
        assert(source&&target&&expected);
        for(unsigned y=0;y<s.height;y++)for(unsigned x=0;x<s.pitch;x++)source[y*s.pitch+x]=(uint8_t)(x*37+y*19);
        for(unsigned phase=0;phase<3;phase++){
            memset(target,0xa7,cap+32);memset(expected,0xa7,cap);
            const struct ciuki_present_gamma *g=phase==1?&gamma:NULL;
            cvp_rect clip={-3,2,(int16_t)(f.width-2),(int16_t)(f.height+4)};
            if(phase==2){uint32_t entries[3]={0xabcdef,0xfedcba,0x123456};assert(!cvp_palette_update(pal,100,3,entries));}
            assert(!cvp_surface_blit(&s,source,pal,g,&f,scale,phase==1?&clip:NULL,target+16,cap));
            unsigned ox=(f.width-s.width*scale)/2,oy=(f.height-s.height*scale)/2;
            for(unsigned y=0;y<f.height;y++)for(unsigned x=0;x<f.width;x++){
                if(phase==1&&(y<2||x>=f.width-2))continue;
                uint32_t c=0;
                if(x>=ox&&x<ox+s.width*scale&&y>=oy&&y<oy+s.height*scale)
                    c=oracle(format,source+(y-oy)/scale*s.pitch+(x-ox)/scale*bytes,pal,g,&f);
                for(unsigned b=0;b<f.bytes;b++)expected[y*f.pitch+x*f.bytes+b]=(uint8_t)(c>>(b*8));
            }
            assert(!memcmp(target+16,expected,cap));
            for(unsigned i=0;i<16;i++)assert(target[i]==0xa7&&target[cap+16+i]==0xa7);
            if(!phase)assert(digest(target+16,cap)==pinned[vector]);
            if(!phase)printf("DIGEST format=%u scale=%u bpp=%u fnv=%08x\n",format,scale,f.bytes*8,digest(target+16,cap));
        }
        struct ciuki_present_surface bad=s;bad.size=1;
        memset(target,0x33,cap+32);
        assert(cvp_surface_blit(&bad,source,pal,NULL,&f,scale,NULL,target,cap));
        assert(cvp_surface_blit(&s,source,pal,NULL,&f,4,NULL,target,cap));
        assert(cvp_surface_blit(&s,source,pal,NULL,&f,scale,NULL,target,cap-1));
        cvp_rect invalid={3,4,-1,8};assert(cvp_surface_blit(&s,source,pal,NULL,&f,scale,&invalid,target,cap));
        cvp_format overlap=f;overlap.blue_pos=overlap.red_pos;
        assert(cvp_surface_blit(&s,source,pal,NULL,&overlap,scale,NULL,target,cap));
        for(size_t i=0;i<cap+32;i++)assert(target[i]==0x33);
        free(source);free(target);free(expected);vector++;
    }
    assert(vector==24);
    uint32_t before=pal[255];assert(cvp_palette_update(pal,255,2,pal));assert(pal[255]==before);
    assert(cvp_present_deadline(0)==16666667&&cvp_present_deadline(16666667)==33333334);
    assert(cvp_present_deadline(999999999)==1000000000&&cvp_present_deadline(1000000000)==1016666667);
    puts("PASS surface oracle: formats/scales/pitches/gamma/palette/clipping/canaries/invalid extents/pacing");
}
static uint64_t ns(void){struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static void bench(void)
{
    struct ciuki_present_gamma ramp;
    for(unsigned i=0;i<256;i++)ramp.red[i]=ramp.green[i]=ramp.blue[i]=(uint16_t)(i*257);
    const unsigned sizes[][2]={{320,200},{640,480},{800,600}};
    for(unsigned format=1;format<=4;format++)for(unsigned size=0;size<3;size++)for(unsigned scale=1;scale<=3;scale++)for(unsigned depth=0;depth<2;depth++){
        unsigned w=sizes[size][0],h=sizes[size][1];
        if(w*scale>2048||h*scale>2048)continue;
        struct ciuki_present_surface s={w,h,w*cvp_surface_bytes(format),0,format};s.size=s.pitch*h;
        cvp_format f={.width=w*scale,.height=h*scale,.bytes=depth?4:2,.red_size=depth?8:5,
            .red_pos=depth?16:11,.green_size=depth?8:6,.green_pos=depth?8:5,.blue_size=depth?8:5};f.pitch=f.width*f.bytes;
        uint8_t *source=calloc(1,s.size),*target=malloc(f.pitch*f.height);uint32_t pal[256]={0};assert(source&&target);
        uint64_t start=ns();
        for(unsigned i=0;i<12;i++)assert(!cvp_surface_blit(&s,source,pal,&ramp,&f,scale,NULL,target,f.pitch*f.height));
        uint64_t elapsed=ns()-start;
        printf("COST format=%u source=%ux%u scale=%u bpp=%u ns_frame=%llu digest=%08x gamma=identity\n",format,w,h,scale,f.bytes*8,(unsigned long long)(elapsed/12),digest(target,f.pitch*f.height));
        free(source);free(target);
    }
}
int main(int argc,char **argv){if(argc>1&&!strcmp(argv[1],"--bench"))bench();else vectors();return 0;}
