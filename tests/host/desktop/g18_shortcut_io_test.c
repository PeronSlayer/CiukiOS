/* SPDX-License-Identifier: MIT */
#include "shortcut_io.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
static void ico_header(uint8_t *b,unsigned bytes,unsigned depth)
{
    memset(b,0,256);b[2]=1;b[4]=1;b[6]=2;b[7]=2;b[10]=1;b[12]=(uint8_t)depth;
    desk_put32(b+14,bytes);desk_put32(b+18,22);
    desk_put32(b+22,40);desk_put32(b+26,2);desk_put32(b+30,4);b[34]=1;b[36]=(uint8_t)depth;
}
int main(int argc,char **argv)
{
    assert(argc==2);uint8_t b[256];uint32_t pixels[DESK_SHORTCUT_PIXELS];
    ico_header(b,64,24);
    /* Bottom row red/green; top row blue/white. */
    b[62+2]=255;b[65+1]=255;b[70]=255;b[73]=255;b[74]=255;b[75]=255;
    b[78+4]=0x40; /* top-right AND transparency */
    assert(desk_ico_decode(b,86,pixels,16));
    assert(pixels[0]==0xff0000ff && pixels[15]==0 && pixels[15*16]==0xffff0000 && pixels[255]==0xff00ff00);
    for(unsigned n=0;n<86;n++)assert(!desk_ico_decode(b,n,pixels,16));
    desk_put32(b+18,UINT32_MAX);assert(!desk_ico_decode(b,86,pixels,16));
    ico_header(b,64,32);b[62]=0x11;b[63]=0x22;b[64]=0x33;b[65]=0x80;
    b[70]=0x44;b[71]=0x55;b[72]=0x66;b[73]=0xff;
    assert(desk_ico_decode(b,86,pixels,32));assert(pixels[0]==0xff665544 && pixels[31*32]==0x80332211);
    ico_header(b,64,1);b[62+4]=255;b[62+5]=255;b[62+6]=255; /* white palette entry */
    b[70]=0x80;b[74]=0x40;
    assert(desk_ico_decode(b,86,pixels,16));assert(pixels[0]==0xff000000 && pixels[15]==0xffffffff && pixels[240]==0xffffffff);
    ico_header(b,64,8);desk_put32(b+54,2);b[66]=255;b[67]=255;b[68]=255;
    b[70]=1;b[75]=1;
    assert(desk_ico_decode(b,86,pixels,16));assert(pixels[0]==0xff000000 && pixels[15]==0xffffffff && pixels[240]==0xffffffff);
    b[74]=2;assert(!desk_ico_decode(b,86,pixels,16)); /* palette index beyond colors-used */
    ico_header(b,64,4);desk_put32(b+54,2);b[66]=255;b[67]=255;b[68]=255;
    b[70]=0x10;b[74]=0x01;
    assert(desk_ico_decode(b,86,pixels,16));assert(pixels[0]==0xff000000 && pixels[15]==0xffffffff && pixels[240]==0xffffffff);
    desk_put32(b+54,17);assert(!desk_ico_decode(b,86,pixels,16));
    ico_header(b,64,32);memcpy(b+22,"\211PNG",4);assert(!desk_ico_decode(b,86,pixels,16));
    assert(!desk_ico_decode(b,86,pixels,48));
    char native[260];assert(desk_shortcut_native_path("C:\\TEST\\app.ico",native) && !strcmp(native,"/TEST/app.ico"));
    assert(!desk_shortcut_native_path("D:\\TEST\\app.ico",native));
    struct desk_launch_record r={.show=7};strcpy(r.label,"Repair fixture");strcpy(r.target,"C:\\Old\\missing.exe");
    strcpy(r.arguments,"/preserve \"quoted argument\"");strcpy(r.directory,"C:\\Work");strcpy(r.icon,"C:\\Icons\\app.ico");
    char path[260];assert(snprintf(path,sizeof(path),"%s/repair.lnk",argv[1])<(int)sizeof(path));
    assert(!desk_shortcut_write(path,&r));
    strcpy(r.target,"C:\\New\\repaired.exe");r.show=3;
    assert(!desk_shortcut_write(path,&r));
    FILE *f=fopen(path,"rb");assert(f);uint8_t bytes[DESK_LINK_BYTES];size_t n=fread(bytes,1,sizeof(bytes),f);assert(!fclose(f));
    struct desk_launch_record saved;assert(desk_link_decode(bytes,(unsigned)n,&saved));assert(!memcmp(&r,&saved,sizeof(r)));
    r.show=10;assert(desk_shortcut_write(path,&r)==-EINVAL);
    f=fopen(path,"rb");assert(f);uint8_t again[DESK_LINK_BYTES];assert(fread(again,1,sizeof(again),f)==n && !memcmp(bytes,again,n) && !fclose(f));
    assert(!unlink(path));
    puts("PASS G18 classic ICO orientation/mask/alpha/palette/bounds, durable shortcut repair and metadata preservation");
    return 0;
}
