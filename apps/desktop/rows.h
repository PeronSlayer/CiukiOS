/* SPDX-License-Identifier: MIT
 * Integer-only XRGB row operations shared with the demo's host preview. */
#ifndef CIUKI_DESKTOP_ROWS_H
#define CIUKI_DESKTOP_ROWS_H
#include <stdint.h>
#include <stddef.h>
static inline void desk_row_copy(uint32_t *dst,const uint32_t *src,size_t words)
{
    __asm__ volatile("cld\n\trep movsl" : "+D"(dst), "+S"(src), "+c"(words) : : "memory", "cc");
}
static inline void desk_row_xrgb(uint32_t *dst,const uint32_t *src,unsigned words)
{
    uint32_t used=words ? src[0]|src[words-1] : 0;
    if (!(used&0xff000000u)) for (unsigned k=1;k+1<words;k++) used|=src[k];
    if (!(used&0xff000000u)) desk_row_copy(dst,src,words);
    else for (unsigned k=0;k<words;k++) dst[k]=src[k]&0xffffffu;
}
/* clip coordinates are relative to the destination region. Both surfaces
 * are bounded by the protocol; at most 2048 source/destination columns. */
static inline void desk_scale_rows(uint32_t *dst,unsigned stride,const uint32_t *src,unsigned src_stride,
    unsigned sw,unsigned sh,unsigned dw,unsigned dh,unsigned x,unsigned y,unsigned w,unsigned h)
{
    unsigned ratio=dw%sw==0 ? dw/sw : 0;
    unsigned first_x=x*sw/dw,first_rem=x*sw%dw,step=sw/dw,delta=sw%dw;
    unsigned sy=y*sh/dh,yrem=y*sh%dh,ystep=sh/dh,ydelta=sh%dh;
    unsigned last=UINT32_MAX;
    for (unsigned row=0;row<h;row++) {
        uint32_t *out=dst+(size_t)row*stride;
        if (sy==last) { desk_row_copy(out,out-stride,w); }
        else {
            const uint32_t *in=src+(size_t)sy*src_stride;
            if (ratio==1) {
                desk_row_xrgb(out,in+x,w);
            } else if (ratio) {
                unsigned k=0,sx=x/ratio,offset=x%ratio;
                while (k<w) {
                    uint32_t colour=in[sx++]&0xffffffu;
                    unsigned n=ratio-offset; if (n>w-k) n=w-k;
                    offset=0;
                    if (n==2) { out[k]=out[k+1]=colour; k+=2; }
                    else if (n==3) { out[k]=out[k+1]=out[k+2]=colour; k+=3; }
                    else while (n--) out[k++]=colour;
                }
            } else {
                unsigned sx=first_x,remainder=first_rem;
                for (unsigned k=0;k<w;k++) {
                    out[k]=in[sx]&0xffffffu;sx+=step;remainder+=delta;
                    if (remainder>=dw) { remainder-=dw;sx++; }
                }
            }
        }
        last=sy;
        sy+=ystep;yrem+=ydelta;if (yrem>=dh) { yrem-=dh;sy++; }
    }
}
#endif
