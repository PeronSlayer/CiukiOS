/* Production icon parser: rectangular XOR/AND images and complete bounds.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <assert.h>
#include <stdio.h>
#include "../../../../sdk/win32/user32/icons.c"
static DWORD error;
BOOL u32_fail(DWORD e) { error=e; return 0; }
int main(void)
{
    uint8_t image[456]={0}; /* 16x8, 24 bpp: 40 + 48*8 + 4*8 */
    w32_put32(image,40); w32_put32(image+4,16); w32_put32(image+8,16);
    image[12]=1; image[14]=24;
    for (unsigned y=0;y<8;y++) for (unsigned x=0;x<16;x++) {
        image[40+y*48+x*3+(y==7 ? 1 : 2)]=255;
    }
    image[40+48*8]=0x80; /* bottom-left AND bit */
    struct icon *i=from_bits(image,sizeof(image),0);
    assert(i && !error && i->pixels[0]==0xff00ff00u);
    assert(i->pixels[31*32+31]==0xffff0000u && i->mask[31*32]);
    for (unsigned n=0;n<sizeof(image);n++) assert(!from_bits(image,n,0));
    assert(error==13);
    w32_put32(image+8,15); assert(!from_bits(image,sizeof(image),0));
    w32_put32(image+8,0); assert(!from_bits(image,sizeof(image),0));
    w32_put32(image+8,514); assert(!from_bits(image,sizeof(image),0));
    puts("PASS rectangular ICO: independent axes, XOR/AND resampling, every truncation and height bounds");
    return 0;
}
