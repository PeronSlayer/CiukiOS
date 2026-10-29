/* Independent MMIO interpreter: consumes register writes and claimed VRAM,
 * never a render3d request or scalar result. Host arithmetic is deliberately
 * floating point, unlike production's fixed-point setup. GPL-2.0-only. */
#ifndef MATROX3D_MODEL_H
#define MATROX3D_MODEL_H
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include "../../src/kernel/drivers/video/matrox/matrox3d.h"
#include <stdlib.h>
struct model_texture { uint32_t reg[MATROX_MMIO_BYTES/4]; };
struct model_edge { double x,y,dx,dy; };
struct model_plane { double start,dx,dy,x,y; };
struct matrox_model {
    uint8_t *ram;
    uint32_t bytes, first, reg[MATROX_MMIO_BYTES/4];
    struct model_texture tex[2];
    struct model_edge left,right;
    struct model_plane color[4],z,uv[2][3];
    unsigned y, bank, credits, capacity, writes, launches, triangles, clears, copies, locks;
    unsigned dac_index;
    unsigned reject_encoding;
    unsigned read_delay;
    uint8_t dac[256];
    uint64_t time, step, fragments;
    bool full, busy, wedge_launch, dead, no_draw, corrupt, bad_restore;
};
static struct matrox_model model;
static uint32_t model_load(uint32_t at,unsigned bytes)
{
    assert((bytes==2 || bytes==4) && at>=model.first && (uint64_t)at+bytes<=model.first+model.bytes);
    uint32_t value=0; memcpy(&value,model.ram+at-model.first,bytes); return value;
}
static void model_store(uint32_t at,uint32_t value,unsigned bytes)
{
    assert((bytes==2 || bytes==4) && at>=model.first && (uint64_t)at+bytes<=model.first+model.bytes);
    memcpy(model.ram+at-model.first,&value,bytes);
}
static uint32_t model_read_memory(void *ctx,uint32_t at,unsigned bytes)
{ assert(ctx==&model && model.locks); model.time+=model.read_delay; return model_load(at,bytes); }
static void model_write_memory(void *ctx,uint32_t at,uint32_t value,unsigned bytes)
{ assert(ctx==&model && model.locks); model_store(at,value,bytes); }
static int model_enter(void *ctx,uint32_t generation)
{ assert(ctx==&model && generation==42 && !model.locks); model.locks++; return 0; }
static void model_leave(void *ctx) { assert(ctx==&model && model.locks==1); model.locks--; }
static uint64_t model_now(void *ctx) { assert(ctx==&model); model.time+=model.step; return model.time; }
static void model_drain(void *ctx) { assert(ctx==&model); }
static int32_t signed24(uint32_t v) { return (int32_t)(v<<8)>>8; }
static double plane_at(const struct model_plane *p,double x,double y)
{ return p->start+p->dx*(x-p->x)+p->dy*(y-p->y); }
static unsigned channel(double x)
{ return x<0 ? 0 : x>255 ? 255 : (unsigned)floor(x+0.5); }
static bool model_compare(unsigned a,unsigned b,unsigned mode)
{
    switch (mode) { case 0:return true; case 2:return a==b; case 3:return a!=b;
    case 4:return a<b; case 5:return a<=b; case 6:return a>b; case 7:return a>=b; }
    assert(0); return false;
}
static unsigned model_factor(unsigned mode,uint32_t src,uint32_t dst,unsigned shift,bool destination)
{
    switch (mode) { case 0:return 0; case 1:return 255;
    case 2:return (destination ? src : dst)>>shift&255;
    case 3:return 255-((destination ? src : dst)>>shift&255);
    case 4:return src>>24; case 5:return 255-(src>>24);
    case 6:return dst>>24; case 7:return 255-(dst>>24); }
    assert(0); return 0;
}
static unsigned tex_address(int x,unsigned size,bool clamp)
{ return clamp ? x<0 ? 0 : (unsigned)x>=size ? size-1 : (unsigned)x : (unsigned)x&(size-1); }
static uint32_t model_texel(unsigned stage,unsigned level,int x,int y)
{
    const uint32_t *t=model.tex[stage].reg;
    unsigned w=(t[M3D_TEXWIDTH/4]>>18&2047)+1,h=(t[M3D_TEXHEIGHT/4]>>18&2047)+1;
    uint32_t origin=t[M3D_TEXORG/4]&~31u;
    if (t[M3D_TEXORG/4]&4) {
        unsigned bytes=t[M3D_TEXORG1/4];
        for (unsigned i=0;i<level;i++) { origin+=bytes; bytes/=4; if (bytes<32) bytes=32; }
    } else if (level) origin=t[(M3D_TEXORG1+(level-1)*4)/4];
    w>>=level; h>>=level; assert(w && h);
    unsigned xx=tex_address(x,w,t[M3D_TEXCTL/4]&(1u<<28));
    unsigned yy=tex_address(y,h,t[M3D_TEXCTL/4]&(1u<<27));
    return model_load(origin+(yy*w+xx)*4,4);
}
static uint32_t model_sample_level(unsigned stage,unsigned level,double u,double v,bool linear)
{
    const uint32_t *t=model.tex[stage].reg;
    unsigned w=((t[M3D_TEXWIDTH/4]>>18&2047)+1)>>level;
    unsigned h=((t[M3D_TEXHEIGHT/4]>>18&2047)+1)>>level;
    double xx=u*w-(linear ? 0.5 : 0),yy=v*h-(linear ? 0.5 : 0);
    int x=(int)floor(xx),y=(int)floor(yy);
    if (!linear) return model_texel(stage,level,x,y);
    double fx=xx-x,fy=yy-y;
    uint32_t p[4]={model_texel(stage,level,x,y),model_texel(stage,level,x+1,y),
        model_texel(stage,level,x,y+1),model_texel(stage,level,x+1,y+1)},result=0;
    for (unsigned s=0;s<32;s+=8) {
        double a=(p[0]>>s&255)*(1-fx)+(p[1]>>s&255)*fx;
        double b=(p[2]>>s&255)*(1-fx)+(p[3]>>s&255)*fx;
        result|=channel(a*(1-fy)+b*fy)<<s;
    }
    return result;
}
static void model_coordinates(unsigned stage,double x,double y,double *u,double *v)
{
    double q=plane_at(&model.uv[stage][2],x,y); assert(q>0);
    *u=plane_at(&model.uv[stage][0],x,y)/q*4;
    *v=plane_at(&model.uv[stage][1],x,y)/q*4;
}
static uint32_t model_sample(unsigned stage,double x,double y)
{
    const uint32_t *t=model.tex[stage].reg;
    unsigned filter=t[M3D_TEXFILTER/4]&15;
    double u,v; model_coordinates(stage,x,y,&u,&v);
    double lod=0;
    if (filter>=8) {
        unsigned w=(t[M3D_TEXWIDTH/4]>>18&2047)+1,h=(t[M3D_TEXHEIGHT/4]>>18&2047)+1;
        double rho=1;
        for (unsigned axis=0;axis<2;axis++) {
            double un,vn; model_coordinates(stage,x+!axis,y+axis,&un,&vn);
            rho=fmax(rho,fmax(fabs(un-u)*w,fabs(vn-v)*h));
        }
        unsigned max=(t[M3D_TEXFILTER/4]>>29)|((t[M3D_TEXFILTER/4]>>18&1)<<3);
        lod=fmin(log2(rho),max);
    }
    unsigned level=(unsigned)floor(lod+(filter==12 ? 0 : 0.5));
    uint32_t a=model_sample_level(stage,level,u,v,filter==2 || filter==10 || filter==12);
    if (filter==12 && lod>level) {
        uint32_t b=model_sample_level(stage,level+1,u,v,true),out=0;
        for (unsigned s=0;s<32;s+=8) out|=channel((a>>s&255)*(1-(lod-level))+(b>>s&255)*(lod-level))<<s;
        a=out;
    }
    return a;
}
static uint32_t model_combine(uint32_t src,uint32_t tex,uint32_t td)
{
    uint32_t out=0; unsigned cm=td>>21&3,am=td>>30;
    for (unsigned s=0;s<32;s+=8) {
        unsigned a=src>>s&255,b=tex>>s&255,c;
        if (s==24) c=am==0 ? b : am==1 ? a : (a*b+127)/255;
        else if (td&(1u<<20)) c=(a*(255-(tex>>24))+b*(tex>>24)+127)/255;
        else c=cm==0 ? b : cm==1 ? a : cm==2 ? (a+b>255 ? 255 : a+b) : (a*b+127)/255;
        out|=c<<s;
    }
    return out;
}
static struct model_plane model_plane(unsigned start,unsigned dx,unsigned dy,bool short_value,double x,double y,unsigned scale)
{
    double a=short_value ? signed24(start) : (int32_t)start;
    double b=short_value ? signed24(dx) : (int32_t)dx;
    double c=short_value ? signed24(dy) : (int32_t)dy;
    return (struct model_plane){a*scale,b*scale,c*scale,x,y};
}
static void model_setup(uint32_t sgn)
{
    double y=(int32_t)model.reg[M3D_YDST/4]/16.;
    if ((sgn&M3D_SGN_SSE) || (sgn&M3D_SGN_SBE && sgn&M3D_SGN_BL))
        model.left=(struct model_edge){(int32_t)model.reg[M3D_FXLEFT/4]/16.,y,
            (int32_t)model.reg[M3D_AR2/4]/16.,(int32_t)model.reg[M3D_AR0/4]/16.};
    if ((sgn&M3D_SGN_SSE) || (sgn&M3D_SGN_SBE && !(sgn&M3D_SGN_BL)))
        model.right=(struct model_edge){(int32_t)model.reg[M3D_FXRIGHT/4]/16.,y,
            (int32_t)model.reg[M3D_AR5/4]/16.,(int32_t)model.reg[M3D_AR6/4]/16.};
    model.y=(unsigned)ceil(y-.5);
    if (!(sgn&M3D_SGN_SSE)) return;
    double x=model.left.x;
    unsigned scale=sgn&M3D_SGN_US ? 16 : 1;
    if (! (sgn&M3D_SGN_US)) {
        y=model.y+.5; x=ceil(model.left.x+model.left.dx*(y-model.left.y)/model.left.dy-.5)+.5;
    }
    model.z=model_plane(model.reg[M3D_DR0/4],model.reg[M3D_DR2/4],model.reg[M3D_DR3/4],false,x,y,scale);
    for (unsigned c=0;c<4;c++) {
        unsigned reg=c==3 ? M3D_ALPHASTART : M3D_DR4+c*16;
        unsigned dx=c==3 ? reg+4 : reg+8,dy=c==3 ? reg+8 : reg+12;
        model.color[c]=model_plane(model.reg[reg/4],model.reg[dx/4],model.reg[dy/4],true,x,y,scale);
    }
    for (unsigned stage=0;stage<2;stage++) for (unsigned c=0;c<3;c++) {
        const uint32_t *t=model.tex[stage].reg;
        model.uv[stage][c]=model_plane(t[(M3D_TMR0+24+c*4)/4],t[(M3D_TMR0+c*8)/4],t[(M3D_TMR0+c*8+4)/4],false,x,y,scale);
    }
}
static void model_triangle(unsigned lines)
{
    unsigned pitch=model.reg[M_PITCH/4]*4,ctl=model.reg[M_DWGCTL/4],ac=model.reg[M3D_ALPHACTRL/4];
    unsigned width=(model.reg[M_CXBNDRY/4]>>16)+1;
    for (unsigned row=0;row<lines;row++,model.y++) {
        double y=model.y+.5;
        double left=model.left.x+model.left.dx*(y-model.left.y)/model.left.dy;
        double right=model.right.x+model.right.dx*(y-model.right.y)/model.right.dy;
        int first=(int)ceil(left-.5),end=(int)ceil(right-.5);
        for (int x=first;x<end;x++) {
            assert(x>=0 && (unsigned)x<width);
            uint32_t at=model.reg[M_DSTORG/4]+model.y*pitch+x*4;
            uint32_t za=model.reg[M3D_ZORG/4]+model.y*pitch+x*2;
            unsigned z=(unsigned)fmax(0,fmin(65535,floor(plane_at(&model.z,x+.5,y)/32768.+.5)));
            unsigned zm=ctl>>8&7;
            if (zm && !model_compare(z,model_load(za,2),zm)) continue;
            uint32_t src=0;
            for (unsigned c=0;c<4;c++) src|=channel(plane_at(&model.color[c],x+.5,y)/32768.)<<((c<3 ? 2-c : 3)*8);
            if ((ctl&15)==6) {
                unsigned units=model.reg[M3D_TEXCTL2/4]&(1u<<7) ? 2 : 1;
                if (!(model.reg[M3D_TEXCTL2/4]&(1u<<15))) {
                    /* G200 has no TDUALSTAGE registers. Decode TEXCTL's
                     * modulation/decal controls and ALPHACTRL's selection. */
                    uint32_t tex=model_sample(0,x+.5,y),colour=0;
                    for (unsigned shift=0;shift<24;shift+=8) {
                        unsigned a=src>>shift&255,b=tex>>shift&255,c;
                        if (model.tex[0].reg[M3D_TEXCTL/4]&(1u<<29)) c=(a*b+127)/255;
                        else if (model.reg[M3D_TEXCTL2/4]&1) c=(a*(255-(tex>>24))+b*(tex>>24)+127)/255;
                        else c=b;
                        colour|=c<<shift;
                    }
                    unsigned select=ac>>24&3;
                    unsigned alpha=select==0 ? tex>>24 : select==1 ? src>>24 : ((src>>24)*(tex>>24)+127)/255;
                    src=colour|(alpha<<24);
                } else for (unsigned stage=0;stage<units;stage++) {
                    uint32_t td=model.reg[(stage ? M3D_TDUALSTAGE1 : M3D_TDUALSTAGE0)/4];
                    src=model_combine(src,model_sample(stage,x+.5,y),td);
                }
            }
            if (!model_compare(src>>24,ac>>16&255,ac>>13&7)) continue;
            uint32_t dst=model_load(at,4),out=0;
            for (unsigned shift=0;shift<32;shift+=8) {
                unsigned a=model_factor(ac&15,src,dst,shift,false),b=model_factor(ac>>4&15,src,dst,shift,true);
                unsigned c=((src>>shift&255)*a+(dst>>shift&255)*b+127)/255;
                out|=(c>255 ? 255 : c)<<shift;
            }
            model_store(at,out,4); model.fragments++;
            if ((ctl>>4&7)==3) model_store(za,z,2);
        }
    }
    model.triangles++;
}
static void model_launch(unsigned reg,uint32_t value)
{
    model.launches++;
    if (model.wedge_launch) { model.busy=true; return; }
    if (model.no_draw && reg==M3D_LEN) return;
    if (reg==M3D_LEN && model.reject_encoding &&
        model.reject_encoding==((model.reg[M3D_SGN/4]&M3D_SGN_US) ? 2u : 1u)) return;
    if (reg==M3D_LEN) { model_triangle(value); return; }
    assert(reg==M_YDSTLEN);
    unsigned ctl=model.reg[M_DWGCTL/4],pitch=model.reg[M_PITCH/4]*4;
    unsigned x0=model.reg[M_FXBNDRY/4]&65535,x1=model.reg[M_FXBNDRY/4]>>16;
    if ((ctl&15)==4) { model.clears++; assert(ctl&M_DWG_SOLID); }
    else { assert((ctl&15)==8); x1++; model.copies++; }
    for (unsigned y=value>>16;y<(value>>16)+(value&65535);y++) for (unsigned x=x0;x<x1;x++) {
        uint32_t at=model.reg[M_DSTORG/4]+y*pitch+x*4;
        uint32_t data=ctl&M_DWG_SOLID ? model.reg[M_FCOL/4] : model_load(model.reg[M_SRCORG/4]+y*pitch+x*4,4);
        model_store(at,data,4);
    }
    if (model.corrupt) model_store(model.first+508,0,4);
}
static uint32_t model_read32(void *ctx,unsigned reg)
{
    assert(ctx==&model && !(reg&3) && reg<MATROX_MMIO_BYTES);
    if (model.dead) return UINT32_MAX;
    if (reg==M_FIFOSTATUS) { model.credits=model.full ? 0 : model.capacity; return model.credits|(model.full ? 0 : M_FIFO_EMPTY); }
    if (reg==M_STATUS) return model.busy ? M_BUSY : 0;
    return model.reg[reg/4];
}
static void model_write32(void *ctx,unsigned reg,uint32_t value)
{
    assert(ctx==&model && reg<MATROX_MMIO_BYTES && !(reg&3)); model.writes++;
    bool fifo=(reg>=0x1c00 && reg<0x1e00) || (reg>=0x2c00 && reg<0x2d00);
    if (fifo) { assert(model.credits && !model.full); model.credits--; }
    bool exec=(reg>=0x1d00 && reg<0x1e00);
    if (exec) reg-=M_EXEC;
    model.reg[reg/4]=value;
    if (reg==M3D_TEXCTL2) model.bank=value>>31;
    if ((reg>=M3D_TMR0 && reg<=M3D_TEXBORDERCOL) || (reg>=M3D_TEXORG1 && reg<=M3D_TEXORG4))
        for (unsigned bank=model.bank;bank<2;bank++) model.tex[bank].reg[reg/4]=value;
    if (reg==M3D_SGN && value&M3D_SGN_SUBPIXEN) model_setup(value);
    if (model.bad_restore && reg==M3D_ALPHACTRL && value==1) model.busy=true;
    if (exec) model_launch(reg,value);
}
static uint8_t model_read8(void *ctx,unsigned reg)
{ assert(ctx==&model); return reg==M_DAC_INDEX ? model.dac_index : model.dac[model.dac_index]; }
static void model_write8(void *ctx,unsigned reg,uint8_t value)
{ assert(ctx==&model); if (reg==M_DAC_INDEX) model.dac_index=value; else model.dac[model.dac_index]=value; }
static void model_reset(void)
{
    free(model.ram); memset(&model,0,sizeof(model));
    model.first=0x100000; model.bytes=12*1024*1024;
    model.ram=malloc(model.bytes); assert(model.ram); memset(model.ram,0xba,model.bytes);
    model.capacity=model.credits=16;
}
static struct matrox_engine model_engine(void)
{
    return (struct matrox_engine){.active=true,.cpu_safe=true,.touched=true,.pitch=1024,.maccess=2,
        .io={&model,model_read32,model_write32,model_read8,model_write8,model_now,model_drain}};
}
#endif
