/* Pixel/region assertions run through the production PE facades. LGPL-2.1-or-later */
#include <w32/gdi32.h>
#include "../win32_core/fixture.h"
struct bitmap_info { BITMAPINFOHEADER header; RGBQUAD colors[256]; };
static HBITMAP bitmap(HDC dc,unsigned depth)
{
    struct bitmap_info info={.header={.biSize=40,.biWidth=4,.biHeight=-4,
        .biPlanes=1,.biBitCount=(uint16_t)depth}};
    if(depth<=8) {
        unsigned count=1u<<depth;
        for(unsigned i=0;i<count;i++) {
            uint8_t v=(uint8_t)(i*255/(count-1));
            info.colors[i]=(RGBQUAD){v,v,v,0};
        }
    }
    void *bits=0;
    return CreateDIBSection(dc,(BITMAPINFO *)&info,0,&bits,0,0);
}
void W32API entry(void)
{
    fixture_begin(W32_FIXTURE_GDI32);
    HDC src=CreateCompatibleDC(0),dst=CreateCompatibleDC(0);
    CHECK(0,src && dst);
    unsigned depths[]={1,4,8,16,24,32};
    for(unsigned i=0;i<6;i++) {
        HBITMAP b=bitmap(dst,depths[i]);CHECK(0,b);
        if(!b)continue;
        HGDIOBJ old=SelectObject(dst,b);
        CHECK(0,old && SetPixel(dst,1,1,RGB(255,255,255))!=CLR_INVALID);
        CHECK(0,GetPixel(dst,1,1)==RGB(255,255,255));
        if(GetPixel(dst,1,1)==RGB(255,255,255))result->row[0].value[0]++;
        CHECK(0,SelectObject(dst,old));CHECK(0,DeleteObject(b));
    }
    HBITMAP a=bitmap(src,32),b=bitmap(dst,32);
    HGDIOBJ olda=SelectObject(src,a),oldb=SelectObject(dst,b);
    CHECK(0,a && b && olda && oldb);
    for(unsigned rop=0;rop<256;rop++) {
        unsigned errors=result->row[0].errors;
        for(unsigned input=0;input<8;input++) {
            COLORREF s=input&2 ? 0xffffffu : 0,d=input&1 ? 0xffffffu : 0;
            HGDIOBJ brush=GetStockObject(input&4 ? WHITE_BRUSH : BLACK_BRUSH);
            HGDIOBJ prior=SelectObject(dst,brush);
            CHECK(0,prior && SetPixel(src,0,0,s)!=CLR_INVALID && SetPixel(dst,0,0,d)!=CLR_INVALID);
            CHECK(0,BitBlt(dst,0,0,1,1,src,0,0,rop<<16));
            CHECK(0,GetPixel(dst,0,0)==((rop>>input)&1 ? 0xffffffu : 0));
            CHECK(0,SelectObject(dst,prior));
        }
        if(errors==result->row[0].errors)result->row[0].value[1]++;
    }
    for(unsigned mode=1;mode<=4;mode++) {
        unsigned errors=result->row[0].errors;
        CHECK(0,SetStretchBltMode(dst,(int)mode));
        CHECK(0,SetPixel(src,0,0,0xffffffu)!=CLR_INVALID);
        CHECK(0,StretchBlt(dst,0,0,2,2,src,0,0,1,1,0x00cc0020));
        CHECK(0,GetPixel(dst,1,1)==0xffffffu);
        if(errors==result->row[0].errors)result->row[0].value[2]++;
    }
    HRGN left=CreateRectRgn(0,0,2,2),right=CreateRectRgn(1,0,3,2),out=CreateRectRgn(0,0,0,0);
    CHECK(1,left && right && out);
    for(int mode=1;mode<=5;mode++) {
        unsigned errors=result->row[1].errors;
        CHECK(1,CombineRgn(out,left,right,mode)>0);
        for(int x=0;x<4;x++) {
            int l=x<2,r=x>=1 && x<3;
            int expected=mode==1 ? l&&r : mode==2 ? l||r : mode==3 ? l!=r : mode==4 ? l&&!r : l;
            CHECK(1,!!PtInRegion(out,x,0)==expected);
        }
        if(errors==result->row[1].errors)result->row[1].value[0]++;
    }
    int reserved=GetDeviceCaps(dst,NUMRESERVED);
    CHECK(1,reserved==20);result->row[1].value[1]=(unsigned)reserved;
    unsigned alignments[]={0,2,6,8,10,14,24,26,30,1,3,7};
    for(unsigned i=0;i<sizeof(alignments)/sizeof(*alignments);i++) {
        unsigned errors=result->row[1].errors;
        CHECK(1,SetTextAlign(dst,alignments[i])!=0xffffffffu);
        CHECK(1,GetTextAlign(dst)==alignments[i]);
        SIZE size;CHECK(1,GetTextExtentPoint32A(dst,"Ciuki",5,&size) && size.cx>0 && size.cy>0);
        CHECK(1,TextOutA(dst,0,0,"C",1));
        if(errors==result->row[1].errors)result->row[1].value[2]++;
    }
    CHECK(1,DeleteObject(left));CHECK(1,DeleteObject(right));CHECK(1,DeleteObject(out));
    CHECK(0,SelectObject(src,olda));CHECK(0,SelectObject(dst,oldb));
    CHECK(0,DeleteObject(a));CHECK(0,DeleteObject(b));
    CHECK(2,DeleteDC(src));CHECK(2,DeleteDC(dst));
    result->row[2].value[0]=w32_gdi_live_objects();
    result->row[2].value[1]=w32_gdi_surface_bytes();
    result->row[2].value[2]=!result->row[2].value[0] && !result->row[2].value[1];
    CHECK(2,result->row[2].value[2]);
    fixture_finish();
}
