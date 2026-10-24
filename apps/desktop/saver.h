/* SPDX-License-Identifier: MIT
 * Geometry shared by the full-screen saver and Display Properties miniature.
 * The approved portrait is sampled by the caller, never reinterpreted. */
#ifndef CIUKI_DESK_SAVER_H
#define CIUKI_DESK_SAVER_H
#include <stdint.h>
struct desk_saver_sprite { int x,y; unsigned size,colour; };
static inline struct desk_saver_sprite desk_saver_logo(unsigned w,unsigned h,uint64_t frame)
{
    unsigned size=w<256 ? w : 256;
    if (size>h) size=h;
    unsigned wx=w-size,hy=h-size;
    unsigned x=wx ? (unsigned)(frame%((uint64_t)wx*2)) : 0;
    unsigned y=hy ? (unsigned)(frame%((uint64_t)hy*2)) : 0;
    if (x>wx) x=wx*2-x;
    if (y>hy) y=hy*2-y;
    return (struct desk_saver_sprite){(int)x,(int)y,size,0};
}
static inline struct desk_saver_sprite desk_saver_star(unsigned w,unsigned h,uint64_t frame,unsigned i)
{
    unsigned z=1+(unsigned)((i*53u+255u-(unsigned)(frame%255))%255);
    int x=(int)w/2+((int)(i*7919u%2049)-1024)*128/(int)z;
    int y=(int)h/2+((int)(i*3571u%1537)-768)*128/(int)z;
    return (struct desk_saver_sprite){x,y,z<64 ? 2u : 1u,(255-z/2)*0x010101u};
}
#endif
