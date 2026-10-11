/* SPDX-License-Identifier: MIT */
#include "deskui.h"
#include <limits.h>
/* The compositor is the single glyph source. */
extern uint8_t desk_glyph_row(unsigned,unsigned);
int du_contains(struct du_rect r,int x,int y)
{ return r.w>0 && r.h>0 && x>=r.x && y>=r.y && (int64_t)x< (int64_t)r.x+r.w && (int64_t)y<(int64_t)r.y+r.h; }
struct du_rect du_intersection(struct du_rect a,struct du_rect b)
{
    int64_t x=a.x>b.x?a.x:b.x,y=a.y>b.y?a.y:b.y;
    int64_t right=(int64_t)a.x+a.w,bottom=(int64_t)a.y+a.h;
    if ((int64_t)b.x+b.w<right) right=(int64_t)b.x+b.w;
    if ((int64_t)b.y+b.h<bottom) bottom=(int64_t)b.y+b.h;
    if (a.w<=0 || a.h<=0 || b.w<=0 || b.h<=0 || right<=x || bottom<=y) return (struct du_rect){0};
    return (struct du_rect){(int)x,(int)y,(int)(right-x),(int)(bottom-y)};
}
void du_fill(struct du_canvas *c,struct du_rect r,uint32_t colour)
{
    r=du_intersection(du_intersection(r,c->clip),(struct du_rect){0,0,(int)c->width,(int)c->height});
    for (int y=r.y;y<r.y+r.h;y++) for (int x=r.x;x<r.x+r.w;x++) c->pixels[(size_t)y*c->width+x]=colour;
}
void du_text(struct du_canvas *c,int x,int y,const char *s,uint32_t colour)
{
    int64_t at=x;
    for (;*s && at<(int64_t)c->clip.x+c->clip.w;s++,at+=8) {
        unsigned ch=(unsigned char)*s;
        if ((ch&0xc0)==0x80) { at-=8; continue; }
        if (ch>=128) ch='?';
        if (at<INT_MIN || at>INT_MAX-8 || y>INT_MAX-16) continue;
        for (unsigned row=0;row<7;row++) for (unsigned col=0;col<5;col++)
            if (desk_glyph_row(ch,row)&(1u<<(4-col))) du_fill(c,(struct du_rect){(int)at+1+(int)col,y+1+(int)row*2,1,2},colour);
    }
}
void du_icon(struct du_canvas *c,int x,int y,const uint32_t *icon,unsigned size)
{
    if (!icon || size>64) return;
    for (unsigned row=0;row<size;row++) for (unsigned col=0;col<size;col++) {
        unsigned v=icon[(row*16/size)*16+col*16/size],a=v>>24;
        int64_t px=(int64_t)x+col,py=(int64_t)y+row;
        if (px<0 || py<0 || px>=c->width || py>=c->height || !du_contains(c->clip,(int)px,(int)py)) continue;
        uint32_t *p=c->pixels+(size_t)py*c->width+(size_t)px,old=*p,result=0;
        for (unsigned shift=0;shift<24;shift+=8) result|=((((v>>shift)&255)*a+((old>>shift)&255)*(255-a)+127)/255)<<shift;
        *p=result;
    }
}
void du_bevel(struct du_canvas *c,struct du_rect r,int pressed)
{
    uint32_t a=pressed?0x879399u:DU_WHITE,b=pressed?DU_WHITE:0x879399u;
    du_fill(c,r,DU_BG); du_fill(c,(struct du_rect){r.x,r.y,r.w,1},a);
    du_fill(c,(struct du_rect){r.x,r.y,1,r.h},a);
    du_fill(c,(struct du_rect){r.x,r.y+r.h-1,r.w,1},b);
    du_fill(c,(struct du_rect){r.x+r.w-1,r.y,1,r.h},b);
}
