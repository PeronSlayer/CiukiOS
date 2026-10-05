#include "webstore.h"

static u16 xseg,xoff;
u8 webstore_pixels[6146];
#define row webstore_pixels
#pragma pack(push,1)
static struct {u32 length;u16 source;u32 source_offset;u16 dest;u32 dest_offset;} move;
struct band {u16 seg,top,bottom,stride,width;u8 bytes,rs,rp,gs,gp,bs,bp;u16 left;};
#pragma pack(pop)

static int find_xms(void)
{
    struct regs r;
    if(xseg) return 1;
    mem_set(&r,0,sizeof r);r.ax=0x4300;intr(0x2F,&r);
    if((r.ax&255)!=0x80) return 0;
    mem_set(&r,0,sizeof r);r.ax=0x4310;intr(0x2F,&r);
    xseg=r.es;xoff=r.bx;return xseg!=0;
}
static void xcall(struct regs *r)
{
    r->ds=r->es=app_seg();r->si=(u16)&move;
    far_regs(xseg,xoff,r);
}
u16 webstore_available(void)
{
    struct regs r;
    if(!find_xms()) return 0;
    mem_set(&r,0,sizeof r);r.ax=0x0800;xcall(&r);return r.ax;
}
u16 webstore_alloc(u32 bytes)
{
    struct regs r;
    if(!bytes || bytes>16UL*1024*1024 || !find_xms()) return 0;
    mem_set(&r,0,sizeof r);r.ax=0x0900;r.dx=(u16)((bytes+1023)>>10);
    xcall(&r);return r.ax==1?r.dx:0;
}
void webstore_free(u16 handle)
{
    struct regs r;
    if(!handle || !xseg) return;
    mem_set(&r,0,sizeof r);r.ax=0x0A00;r.dx=handle;xcall(&r);
}
u32 webstore_lock(u16 handle)
{
    struct regs r;if(!handle||!find_xms())return 0;
    mem_set(&r,0,sizeof r);r.ax=0x0C00;r.dx=handle;xcall(&r);
    return r.ax==1?((u32)r.dx<<16)|r.bx:0;
}
void webstore_unlock(u16 handle)
{
    struct regs r;if(!handle||!xseg)return;
    mem_set(&r,0,sizeof r);r.ax=0x0D00;r.dx=handle;xcall(&r);
}
static int transfer(u16 handle,u32 offset,void *buffer,u16 count,int write)
{
    struct regs r;
    u32 address=((u32)app_seg()<<16)|(u16)buffer;
    if(!handle || !xseg || (offset&1) || (count&1)) return 0;
    move.length=count;
    move.source=write?0:handle;move.source_offset=write?address:offset;
    move.dest=write?handle:0;move.dest_offset=write?offset:address;
    mem_set(&r,0,sizeof r);r.ax=0x0B00;xcall(&r);return r.ax==1;
}
int webstore_read(u16 handle,u32 offset,void *buffer,u16 count)
{return transfer(handle,offset,buffer,count,0);}
int webstore_write(u16 handle,u32 offset,const void *buffer,u16 count)
{return transfer(handle,offset,(void *)buffer,count,1);}

void webstore_image(u16 handle,u16 width,u16 height,int x,int y,int w,int h,
                    int clip_x,int clip_y,int clip_w,int clip_h)
{
    struct band band;
    int left=x,top=y,right=x+w,bottom=y+h,dx,dy,previous=-1;
    u16 stride=(width*3u+1u)&~1u,sy,sx,off,xstep,xrem,xerr,xfirst,firsterr;
    int direct;
    u8 __far *dest;
    u32 value;
    if(!handle || !width || width>2048 || !height || w<=0 || h<=0) return;
    if(left<clip_x) left=clip_x;if(top<clip_y) top=clip_y;
    if(right>clip_x+clip_w) right=clip_x+clip_w;
    if(bottom>clip_y+clip_h) bottom=clip_y+clip_h;
    if(!app_band_info(&band)) {
        /* VGA16 fallback uses the existing desktop palette, with no palette
         * writes that could recolour other windows. */
        for(dy=top;dy<bottom;++dy) {
            sy=(u16)(((u32)(dy-y)*height)/h);
            if(previous!=sy && !webstore_read(handle,(u32)sy*stride,row,stride)) return;
            previous=sy;
            for(dx=left;dx<right;++dx) {
                int color;
                sx=(u16)(((u32)(dx-x)*width)/w)*3u;
                color=(row[sx]>127?4:0)|(row[sx+1]>127?2:0)|(row[sx+2]>127?1:0);
                if(row[sx]>200 || row[sx+1]>200 || row[sx+2]>200) color|=8;
                ui_rect(dx,dy,1,1,color);
            }
        }
        return;
    }
    if(left<band.left) left=band.left;
    if(right>band.left+band.width) right=band.left+band.width;
    if(top<band.top) top=band.top;if(bottom>band.bottom) bottom=band.bottom;
    if(band.bytes<2 || band.bytes>4 || band.rs>8 || band.gs>8 || band.bs>8) return;
    if(left>=right||top>=bottom)return;
    /* Divide once per span, not once per pixel. The common VBE RGB888 path
       also avoids the 16-bit compiler's variable 32-bit shift helpers. */
    value=(u32)(left-x)*width;
    xfirst=(u16)(value/w)*3u;firsterr=(u16)(value%w);
    xstep=(width/w)*3u;xrem=width%w;
    direct=band.rs==8&&band.gs==8&&band.bs==8&&band.rp==16&&band.gp==8&&band.bp==0;
    for(dy=top;dy<bottom;++dy) {
        sy=(u16)(((u32)(dy-y)*height)/h);
        if(previous!=sy && !webstore_read(handle,(u32)sy*stride,row,stride)) return;
        previous=sy;
        off=(dy-band.top)*band.stride+(left-band.left)*band.bytes;
        dest=(u8 __far *)(((u32)band.seg<<16)|off);
        sx=xfirst;xerr=firsterr;
        for(dx=left;dx<right;++dx) {
            if(direct){
                *dest++=row[sx+2];*dest++=row[sx+1];*dest++=row[sx];
                if(band.bytes==4)*dest++=0;
            }else{
                value=((u32)(row[sx]>>(8-band.rs))<<band.rp)|
                  ((u32)(row[sx+1]>>(8-band.gs))<<band.gp)|
                  ((u32)(row[sx+2]>>(8-band.bs))<<band.bp);
                *dest++=(u8)value;*dest++=(u8)(value>>8);
                if(band.bytes>=3) *dest++=(u8)(value>>16);
                if(band.bytes==4) *dest++=0;
            }
            sx+=xstep;xerr+=xrem;if(xerr>=w){xerr-=w;sx+=3;}
        }
    }
}
