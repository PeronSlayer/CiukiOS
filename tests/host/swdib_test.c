/* Byte goldens independent of the software implementation. Padding and
 * allocation guards are part of every comparison. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/swdib.h>
static uint8_t actual[4096], golden[4096], snapshot[4096];
static unsigned checks;
static uint32_t colour(const struct fb_device *d,uint32_t rgb)
{
    return (((rgb>>16)&255)>>(8-d->red_size))<<d->red_pos |
           (((rgb>>8)&255)>>(8-d->green_size))<<d->green_pos |
           ((rgb&255)>>(8-d->blue_size))<<d->blue_pos;
}
static void put(const struct fb_device *d,unsigned x,unsigned y,uint32_t value)
{
    for (unsigned b=0;b<d->bpp/8;b++) golden[13+y*d->pitch+x*(d->bpp/8)+b]=(uint8_t)(value>>(8*b));
}
static void check(void) { assert(!memcmp(actual,golden,sizeof(actual))); checks++; }
static void reset(void)
{
    for (unsigned i=0;i<sizeof(actual);i++) actual[i]=golden[i]=(uint8_t)(i*71+19);
}
static void run(unsigned bpp)
{
    struct fb_device d={.width=19,.height=17,.pitch=19*(bpp/8)+7,.size=17*(19*(bpp/8)+7),
        .mapped=actual+13,.bpp=bpp,.red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8};
    struct fb_rect r={3,2,13,11};
    reset(); swdib_fill(&d,&r,swdib_pack(&d,0xfe37a59b));
    for (int y=r.y;y<r.y+r.height;y++) for (int x=r.x;x<r.x+r.width;x++) put(&d,x,y,colour(&d,0x37a59b));
    check();
    const uint8_t pattern[8]={0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01};
    swdib_pattern(&d,&r,pattern,colour(&d,0x123456),colour(&d,0xcba987));
    for (int y=r.y;y<r.y+r.height;y++) for (int x=r.x;x<r.x+r.width;x++)
        put(&d,x,y,colour(&d,pattern[y%8]&(128>>(x%8)) ? 0x123456 : 0xcba987));
    check();
    for (int dy=-2;dy<=2;dy++) for (int dx=-2;dx<=2;dx++) {
        reset(); memcpy(snapshot,golden,sizeof(golden)); swdib_copy(&d,&r,r.x+dx,r.y+dy);
        for (int y=0;y<r.height;y++) for (int x=0;x<r.width;x++) for (unsigned b=0;b<bpp/8;b++)
            golden[13+(r.y+dy+y)*d.pitch+(r.x+dx+x)*(bpp/8)+b]=snapshot[13+(r.y+y)*d.pitch+(r.x+x)*(bpp/8)+b];
        check();
    }
    uint32_t input[17][21];
    for (unsigned y=0;y<17;y++) for (unsigned x=0;x<21;x++) input[y][x]=0xa5000000|(x*34571+y*713);
    struct fb_surface surface={input,19,17,sizeof(input[0]),sizeof(input)};
    struct fb_rect destination={-2,-3,19,17}, clip={0,0,17,14};
    reset(); swdib_present(&d,&surface,&destination,&clip);
    for (unsigned y=0;y<14;y++) for (unsigned x=0;x<17;x++) put(&d,x,y,colour(&d,input[y+3][x+2]));
    check();
    uint32_t rows[17][21]; memcpy(rows,input,sizeof(rows));
    destination=(struct fb_rect){0,0,19,17}; clip=(struct fb_rect){2,3,13,11};
    reset(); swdib_convert(&d,&destination,rows,sizeof(rows[0]));
    swdib_write(&d,&destination,&clip,rows,sizeof(rows[0]));
    for (int y=clip.y;y<clip.y+clip.height;y++) for (int x=clip.x;x<clip.x+clip.width;x++) put(&d,x,y,colour(&d,input[y][x]));
    check();
    printf("swdib %u bpp: FILL, COPY (25 overlap directions), PATTERN (screen origin/MSB), WRITE (in-place conversion/clipping), X ignored, unaligned pitch and guards PASS\n",bpp);
}
int main(void)
{ run(16); run(24); run(32); printf("swdib: %u byte goldens PASS\n",checks); return 0; }
