/* Independent host MMIO decoder; no production state/packing/sampling helpers.
 * Arithmetic widths are model parameters, not claims about undocumented silicon.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ciuki/kernel.h>
/* The copied kernel wire uses its own wait-status macros. */
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include "../../src/kernel/drivers/video/radeon2d/radeon3d.h"
#include "../../src/kernel/drivers/video/radeon2d/radeon3d_regs.h"
#include "../../sdk/win32/opengl32/encode.h"
static unsigned failures,locks;
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c); failures++; exit(1); } } while (0)
static struct radeon_engine test_engine;
static struct radeon3d_device gpu3d;
static struct render3d_service scratch3d,reference3d;
static uint8_t ram3d[0xbff000];
static struct { uint32_t reg[0x4000/4]; uint64_t us; bool exact_clock; } m;
static uint32_t batch3d[28],count3d,need3d;
static unsigned draw3d,copy3d,writes3d,memory3d,allocations3d;
static uint64_t fragments3d;
static unsigned corruption3d,read_delay3d,write_delay3d,delay_calls3d;
static bool frozen3d;
static bool dropping3d,bad_restore3d,fifo_stuck3d,cache_stuck3d;
struct sampling_model3d { unsigned interpolant_bits,address_bits,filter_bits; bool round_weights,affine; long double bias; unsigned address_integer_bits; };
static struct sampling_model3d sampling3d={24,12,8,false,false,0,14};
static unsigned probe_filter_bits3d,probe_address_bits3d;
static long double probe_lod_bias3d;
static uint32_t physical3d(uint32_t address,unsigned bytes)
{
    CHECK((bytes==2 || bytes==4) && !(address&(bytes-1)));
    CHECK(address>=gpu3d.heap.first && (uint64_t)address+bytes<=(uint64_t)gpu3d.heap.first+gpu3d.heap.bytes);
    return address-gpu3d.heap.first;
}
static uint32_t load3d(uint32_t address,unsigned bytes)
{ uint32_t v=0; memcpy(&v,ram3d+physical3d(address,bytes),bytes); return v; }
static void store3d(uint32_t address,uint32_t value,unsigned bytes)
{ memcpy(ram3d+physical3d(address,bytes),&value,bytes); }
static long double ieee3d(uint32_t word)
{ float f; memcpy(&f,&word,4); CHECK(isfinite(f)); return f; }
static bool compare3d(unsigned a,unsigned b,unsigned cf)
{
    switch(cf) { case 0:return false;case 1:return a<b;case 2:return a<=b;case 3:return a==b;
        case 4:return a>=b;case 5:return a>b;case 6:return a!=b;default:return true; }
}
static uint32_t texreg3d(unsigned stage,unsigned reg100,unsigned reg200)
{ return m.reg[(gpu3d.r200 ? reg200+stage*0x20 : reg100+stage*0x18)/4]; }
static uint32_t texel3d(unsigned stage,unsigned level,int x,int y)
{
    unsigned format=texreg3d(stage,0x1c58,0x2c04),filter=texreg3d(stage,0x1c54,0x2c00);
    unsigned w=1u<<(format>>8&15),h=1u<<(format>>12&15),fmt=format&31,bpp=fmt==6 ? 4 : 2;
    CHECK(fmt==4 || fmt==5 || fmt==6);
    CHECK((format>>24&7)==stage);
    CHECK(gpu3d.r200 || (format&0x80000000u));
    CHECK(!!(format&0x40)==(fmt!=4));
    CHECK((filter>>23&7)==0 || (filter>>23&7)==2);
    CHECK((filter>>27&7)==0 || (filter>>27&7)==2);
    if(gpu3d.r200) CHECK(m.reg[(0x2c08+stage*0x20)/4]==0x00600000u);
    uint32_t addr=m.reg[(gpu3d.r200 ? 0x2d00+stage*0x18 : 0x1c5c+stage*0x18)/4]-test_engine.local_base;
    CHECK(!(addr&31));
    for (unsigned i=0;i<level;i++) {
        unsigned stride=(w*bpp+31)&~31u; if (stride<32) stride=32;
        addr+=stride*h; w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
    }
    if ((filter>>23&7)==2) x=x<0 ? 0 : x>=(int)w ? (int)w-1 : x; else x=(unsigned)x&(w-1);
    if ((filter>>27&7)==2) y=y<0 ? 0 : y>=(int)h ? (int)h-1 : y; else y=(unsigned)y&(h-1);
    unsigned stride=(w*bpp+31)&~31u; if (stride<32) stride=32;
    uint32_t t=load3d(addr+y*stride+x*bpp,bpp);
    if (fmt==4) { unsigned r=t>>11,g=t>>5&63,b=t&31; return 0xff000000u|((r*8+r/4)<<16)|((g*4+g/16)<<8)|b*8+b/4; }
    if (fmt==5) return ((t>>12)*17<<24)|((t>>8&15)*17<<16)|((t>>4&15)*17<<8)|(t&15)*17;
    return t;
}
static long double interpolant3d(long double v)
{
    if (!v || !sampling3d.interpolant_bits) return v;
    int exponent;
    long double a=frexpl(v,&exponent),scale=ldexpl(1,sampling3d.interpolant_bits);
    return ldexpl(roundl(a*scale)/scale,exponent);
}
static long double sample_channel3d(unsigned stage,unsigned level,long double u,long double v,unsigned shift,bool linear)
{
    uint32_t td=texreg3d(stage,0x1c58,0x2c04);
    unsigned w=(1u<<(td>>8&15))>>level,h=(1u<<(td>>12&15))>>level;
    if (!w) w=1;
    if (!h) h=1;
    long double x=u*w-(linear ? .5L : 0),y=v*h-(linear ? .5L : 0);
    x+=sampling3d.bias; y+=sampling3d.bias;
    unsigned integer_bits=gpu3d.fullsize_qualified && gpu3d.fullsize_testing && probe_address_bits3d ?
        probe_address_bits3d : sampling3d.address_integer_bits;
    if (integer_bits) {
        /* Signed fixed-point address accumulator, before repeat/clamp.
         * Width is a model/probe parameter, not an S3 specification claim. */
        long double period=ldexpl(1,integer_bits),half=period/2;
        x-=floorl((x+half)/period)*period; y-=floorl((y+half)/period)*period;
    }
    if (sampling3d.address_bits) {
        long double scale=ldexpl(1,sampling3d.address_bits);
        x=floorl(x*scale)/scale; y=floorl(y*scale)/scale;
    }
    int ix=(int)floorl(x),iy=(int)floorl(y);
    unsigned a=(texel3d(stage,level,ix,iy)>>shift)&255;
    if (!linear) return a;
    long double fx=x-ix,fy=y-iy;
    unsigned bits=gpu3d.fullsize_qualified && gpu3d.fullsize_testing && probe_filter_bits3d ? probe_filter_bits3d : sampling3d.filter_bits;
    if (bits) {
        long double scale=ldexpl(1,bits);
        fx=(sampling3d.round_weights ? roundl(fx*scale) : floorl(fx*scale))/scale;
        fy=(sampling3d.round_weights ? roundl(fy*scale) : floorl(fy*scale))/scale;
    }
    unsigned b=(texel3d(stage,level,ix+1,iy)>>shift)&255;
    unsigned c=(texel3d(stage,level,ix,iy+1)>>shift)&255;
    unsigned d=(texel3d(stage,level,ix+1,iy+1)>>shift)&255;
    return (a*(1-fx)+b*fx)*(1-fy)+(c*(1-fx)+d*fx)*fy;
}
static uint32_t sample3d(unsigned stage,long double u,long double v,long double rho)
{
    unsigned tc=texreg3d(stage,0x1c54,0x2c00),filter=tc>>1&15,max=tc>>16&15;
    CHECK(filter==0 || filter==1 || filter==2 || filter==6 || filter==7);
    long double l=filter>=2 && rho>1 ? log2l(rho) : 0;
    if (gpu3d.fullsize_qualified && gpu3d.fullsize_testing) l+=probe_lod_bias3d;
    l=fmaxl(0,fminl(max,l));
    unsigned level=filter==7 ? (unsigned)l : (unsigned)(l+.5L);
    bool linear=l<=0 ? (tc&1) : filter==1 || filter==6 || filter==7;
    uint32_t out=0;
    for(unsigned shift=0;shift<32;shift+=8) {
        long double c=sample_channel3d(stage,level,u,v,shift,linear);
        if(filter==7 && level<max) c=c*(1-(l-level))+sample_channel3d(stage,level+1,u,v,shift,true)*(l-level);
        out|=(uint32_t)(c+.5L)<<shift;
    }
    return out;
}
static unsigned argument3d(unsigned selector,bool alpha,uint32_t diffuse,uint32_t current,const uint32_t tex[2],unsigned shift)
{
    if(gpu3d.r200) {
        if(alpha) { if(selector==0) return 0; if(selector==4) return diffuse>>24;
            if(selector==10) return current>>24; if(selector==12) return tex[1]>>24; }
        else { if(selector==0) return 0; if(selector==4) return diffuse>>shift&255; if(selector==5) return diffuse>>24;
            if(selector==10) return current>>shift&255; if(selector==11) return current>>24;
            if(selector==12) return tex[1]>>shift&255; if(selector==13) return tex[1]>>24; }
    } else if(alpha) {
        if(selector==0) return 0; if(selector==1) return current>>24;
        if(selector==5) return tex[0]>>24; if(selector==6) return tex[1]>>24;
    } else {
        if(selector==0) return 0; if(selector==2) return current>>shift&255; if(selector==3) return current>>24;
        if(selector==10) return tex[0]>>shift&255; if(selector==11) return tex[0]>>24;
        if(selector==12) return tex[1]>>shift&255; if(selector==13) return tex[1]>>24;
    }
    CHECK(false); return 0;
}
static uint32_t combine3d(uint32_t diffuse,uint32_t current,const uint32_t tex[2],unsigned stage)
{
    uint32_t out=0;
    if(gpu3d.r200) {
        CHECK(m.reg[(0x2f04+stage*0x10)/4]==0x11000); /* clamp and output R0 */
        CHECK(m.reg[(0x2f0c+stage*0x10)/4]==0x11000);
    }
    for(unsigned shift=0;shift<32;shift+=8) {
        bool alpha=shift==24;
        unsigned ctl=m.reg[(gpu3d.r200 ? 0x2f00+stage*0x10+(alpha ? 8 : 0) : 0x1c60+stage*0x18+(alpha ? 4 : 0))/4];
        unsigned bits=alpha && !gpu3d.r200 ? 4 : 5,mask=(1u<<bits)-1;
        unsigned a=argument3d(ctl&mask,alpha,diffuse,current,tex,shift);
        unsigned b=argument3d(ctl>>bits&mask,alpha,diffuse,current,tex,shift);
        unsigned c=argument3d(ctl>>(2*bits)&mask,alpha,diffuse,current,tex,shift);
        if(ctl&(gpu3d.r200 ? 1u<<20 : 1u<<16)) b=255-b;
        unsigned op=ctl>>(gpu3d.r200 ? 28 : 18)&7;
        CHECK(op==0 || op==3);
        unsigned v=op==3 ? (a*(255-c)+b*c+127)/255 : (a*b+c*255+127)/255;
        out|=(v>255 ? 255 : v)<<shift;
    }
    return out;
}
static unsigned factor3d(unsigned f,unsigned s,unsigned d,unsigned sa,unsigned da)
{
    switch(f) { case 32:return 0;case 33:return 255;case 34:return s;case 35:return 255-s;
        case 36:return d;case 37:return 255-d;case 38:return sa;case 39:return 255-sa;
        case 40:return da;case 41:return 255-da;default:CHECK(false);return 0; }
}
static uint32_t blend3d(uint32_t src,uint32_t dst,uint32_t ctl)
{
    if(!(m.reg[0x1c3c/4]&1)) return src;
    uint32_t out=0;
    for(unsigned shift=0;shift<32;shift+=8) {
        unsigned s=src>>shift&255,d=dst>>shift&255;
        unsigned v=(s*factor3d(ctl>>16&63,s,d,src>>24,dst>>24)+d*factor3d(ctl>>24&63,s,d,src>>24,dst>>24)+127)/255;
        out|=(v>255 ? 255 : v)<<shift;
    }
    return out;
}
static uint32_t destination3d(uint32_t addr,unsigned pitch,unsigned bytes,unsigned x,unsigned y)
{ return addr+pitch*y+bytes*x; }
static void raster3d(void)
{
    unsigned stride=9;
    if(dropping3d) { draw3d++;return; }
    CHECK(batch3d[0]==(gpu3d.r200 ? 0x30034u : 0x30134u));
    unsigned pp=m.reg[0x1c38/4],zctrl=m.reg[0x1c2c/4];
    bool depth_enabled=m.reg[0x1c3c/4]&0x100,depth_write=zctrl>>30&1;
    CHECK((m.reg[0x1c3c/4]&~0x101u)==0x1802); /* ARGB8888 and plane mask */
    CHECK((zctrl&~0x40000070u)==0x7000); /* integer D16, stencil ALWAYS/KEEP */
    CHECK(m.reg[0x1d84/4]==0xffffffffu && m.reg[0x1d80/4]==0xc00);
    unsigned comparison=zctrl>>4&7;
    bool textured=pp&16,second=pp&32;
    unsigned cb=m.reg[0x1c40/4]-test_engine.local_base,zb=m.reg[0x1c24/4]-test_engine.local_base;
    unsigned cp=(m.reg[0x1c48/4]&0x1ff8)*4,zp=(m.reg[0x1c28/4]&0x1ff8)*2;
    unsigned width=m.reg[0x1c44/4]&65535,height=m.reg[0x1c44/4]>>16;
    CHECK(width<=1024 && height<=1024 && cp>=width*4);
    CHECK(m.reg[0x1c4c/4]==0x18000a1eu); /* both faces solid; OGL center, 1/16 pixel rounding */
    if(gpu3d.r200) { CHECK(m.reg[0x1c50/4]==0xa && m.reg[0x2080/4]==0x240000); CHECK(m.reg[0x2088/4]==0x803 && m.reg[0x208c/4]==18 && m.reg[0x20b0/4]==0x300); }
    else CHECK(m.reg[0x2080/4]==0x80000189 && m.reg[0x1c50/4]==3);
    long double x[3],y[3],z[3],u[3],v[3],u1[3],v1[3],rw[3]; uint32_t colors[3];
    for (unsigned i=0;i<3;i++) {
        const uint32_t *p=batch3d+1+i*stride;
        x[i]=ieee3d(p[0]); y[i]=ieee3d(p[1]);
        z[i]=ieee3d(p[2]); rw[i]=ieee3d(p[3]); colors[i]=p[4]; u[i]=ieee3d(p[5]); v[i]=ieee3d(p[6]);
        u1[i]=second ? ieee3d(p[7]) : 0; v1[i]=second ? ieee3d(p[8]) : 0;
    }
    long double area=(x[1]-x[0])*(y[2]-y[0])-(y[1]-y[0])*(x[2]-x[0]);
    if (!area) { draw3d++; return; }
    if (area<0) {
        /* Culling is disabled. Normalize winding together with attributes. */
        long double *attributes[]={x,y,z,u,v,rw,u1,v1};
        for (unsigned i=0;i<8;i++) {
            long double tmp=attributes[i][1]; attributes[i][1]=attributes[i][2]; attributes[i][2]=tmp;
        }
        uint32_t tmp=colors[1]; colors[1]=colors[2]; colors[2]=tmp; area=-area;
    }
    bool inclusive[3];
    for (unsigned i=0;i<3;i++) {
        unsigned from=(i+1)%3,to=(i+2)%3;
        inclusive[i]=y[to]<y[from] || (y[to]==y[from] && x[to]>x[from]);
    }
    unsigned left=(unsigned)fmaxl(0,floorl(fminl(x[0],fminl(x[1],x[2]))));
    unsigned right=(unsigned)fminl(width,ceill(fmaxl(x[0],fmaxl(x[1],x[2]))));
    unsigned top=(unsigned)fmaxl(0,floorl(fminl(y[0],fminl(y[1],y[2]))));
    unsigned bottom=(unsigned)fminl(height,ceill(fmaxl(y[0],fmaxl(y[1],y[2]))));
    for (unsigned yy=top;yy<bottom;yy++) for (unsigned xx=left;xx<right;xx++) {
        long double px=xx+0.5L,py=yy+0.5L;
        /* Keep exact edge numerators until the last division. Normalizing
         * each weight first can move an exact nearest-sampling boundary. */
        long double a=(x[1]-px)*(y[2]-py)-(y[1]-py)*(x[2]-px);
        long double b=(x[2]-px)*(y[0]-py)-(y[2]-py)*(x[0]-px);
        long double c=(x[0]-px)*(y[1]-py)-(y[0]-py)*(x[1]-px);
        if (a<0 || b<0 || c<0 || (!a && !inclusive[0]) || (!b && !inclusive[1]) || (!c && !inclusive[2])) continue;
        unsigned depth=(unsigned)((a*z[0]+b*z[1]+c*z[2])*65535/area+0.5L);
        uint32_t da=depth_enabled ? destination3d(zb,zp,2,xx,yy) : 0;
        if (!dropping3d && depth_enabled && !compare3d(depth,load3d(da,2),comparison)) continue;
        long double rhw=a*rw[0]+b*rw[1]+c*rw[2];
        uint32_t color=0;
        for (unsigned shift=0;shift<32;shift+=8) {
            long double diffuse=(a*((colors[0]>>shift)&255)+b*((colors[1]>>shift)&255)+c*((colors[2]>>shift)&255))/area;
            color|=(uint32_t)(diffuse+.5L)<<shift;
        }
        uint32_t diffuse=color,tex[2]={0};

        for (unsigned stage=0;stage<2;stage++) {
            if (!(stage ? second : textured)) continue;
            long double *us=stage ? u1 : u,*vs=stage ? v1 : v;
            long double denom=interpolant3d(rhw/area);
            long double uu=interpolant3d((a*interpolant3d(us[0]*rw[0])+b*interpolant3d(us[1]*rw[1])+c*interpolant3d(us[2]*rw[2]))/area)/denom;
            long double vv=interpolant3d((a*interpolant3d(vs[0]*rw[0])+b*interpolant3d(vs[1]*rw[1])+c*interpolant3d(vs[2]*rw[2]))/area)/denom;
            if (sampling3d.affine) { uu=(a*us[0]+b*us[1]+c*us[2])/area; vv=(a*vs[0]+b*vs[1]+c*vs[2])/area; }
            uu=interpolant3d(uu); vv=interpolant3d(vv);
            long double rho=0;
            for (unsigned axis=0;axis<2;axis++) {
                long double aa=a+(axis ? x[2]-x[1] : y[1]-y[2]);
                long double bb=b+(axis ? x[0]-x[2] : y[2]-y[0]);
                long double cc=area-aa-bb,den=aa*rw[0]+bb*rw[1]+cc*rw[2];
                long double un=(aa*us[0]*rw[0]+bb*us[1]*rw[1]+cc*us[2]*rw[2])/den;
                long double vn=(aa*vs[0]*rw[0]+bb*vs[1]*rw[1]+cc*vs[2]*rw[2])/den;
                unsigned desc=texreg3d(stage,0x1c58,0x2c04),tw=1u<<(desc>>8&15),th=1u<<(desc>>12&15);
                long double du=fabsl(un-uu)*tw,dv=fabsl(vn-vv)*th;
                if (du>rho) rho=du;
                if (dv>rho) rho=dv;
            }
            tex[stage]=sample3d(stage,uu,vv,rho);
            if(gpu3d.r200 && !stage) color=tex[0];
            color=combine3d(diffuse,color,tex,stage);
        }
        fragments3d++;
        unsigned alpha=m.reg[0x1c14/4];
        if((pp&(1u<<23)) && !compare3d(color>>24,alpha&255,alpha>>8&7)) continue;
        uint32_t ca=destination3d(cb,cp,4,xx,yy);
        store3d(ca,blend3d(color,load3d(ca,4),m.reg[0x1c20/4]),4);
        if (depth_write) store3d(da,depth,2);
    }
    draw3d++;
}
static void blit3d(unsigned w,unsigned h,bool copy)
{
    unsigned master=m.reg[0x146c/4],bytes=(master>>8&15)==4 ? 2 : 4;
    /* GMC selectors choose DEFAULT_PITCH_OFFSET unless explicitly enabled.
     * Modeling that default catches accidental writes to the scanout lease. */
    unsigned dst=m.reg[(master&2 ? 0x142c : 0x16e0)/4],src=m.reg[(master&1 ? 0x1428 : 0x16e0)/4];
    CHECK((master&0x3000)==0x3000);
    unsigned dp=(dst>>22)*64,sp=(src>>22)*64;
    unsigned db=(dst&0x3fffff)*1024-test_engine.local_base,sb=(src&0x3fffff)*1024-test_engine.local_base;
    CHECK(w<=1024 && h<=1024 && dp>=w*bytes);
    for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) {
        uint32_t v=copy ? load3d(sb+y*sp+x*bytes,bytes) : m.reg[0x147c/4];
        store3d(db+y*dp+x*bytes,v^(copy && corruption3d==2 ? 1u : 0u),bytes);
    }
    if(copy) copy3d++;
}
static uint32_t read_engine3d(void *ctx,uint32_t reg)
{
    (void)ctx; CHECK(reg<0x4000 && !(reg&3));
    if(reg==0x0e40) return fifo_stuck3d ? 0 : 64;
    if(reg==0x325c && cache_stuck3d) return 1u<<31;
    return m.reg[reg/4];
}
static void write_engine3d(void *ctx,uint32_t off,uint32_t value)
{
    (void)ctx; CHECK(locks==1 && off<0x4000 && !(off&3)); writes3d++;m.us+=write_delay3d;
    if(off==0x2000) {
        CHECK(count3d && count3d<28);
        batch3d[count3d++]=value;
        if(count3d==need3d) { raster3d();count3d=0; if(corruption3d==1) ram3d[0]=0; }
        return;
    }
    if(off==0x2084) { CHECK(!count3d); batch3d[0]=value;count3d=1;need3d=28;return; }
    if(off==0x1714 || off==0x325c || off==0x3254) return;
    if(bad_restore3d && draw3d && off==0x1c3c && value==0) return;
    m.reg[off/4]=value;
    if(off==0x1598) blit3d(value>>16,value&65535,false);
    if(off==0x143c) blit3d(value&65535,value>>16,true);
}
static uint32_t memory_read3d(void *ctx,uint32_t offset,unsigned bytes)
{ (void)ctx; CHECK(locks); memory3d++; m.us+=read_delay3d; return load3d(offset,bytes); }
static void memory_write3d(void *ctx,uint32_t offset,uint32_t value,unsigned bytes)
{ (void)ctx; CHECK(locks); memory3d++; store3d(offset,value,bytes); }
static int lock3d(void *ctx,uint32_t generation)
{ (void)ctx; CHECK(!locks && generation==42); locks=1;return 0; }
static void unlock3d(void *ctx) { (void)ctx;CHECK(locks);locks=0; }
static void drain3d(void *ctx) { (void)ctx; }
static int delay3d(void *ctx,uint32_t us) { (void)ctx;delay_calls3d++;if(!frozen3d) m.us+=us;return 0; }
static uint64_t clock3d(void *ctx) { (void)ctx;return m.us; }
static void *allocate3d(void *ctx,size_t bytes)
{ (void)ctx;CHECK(!locks);void *p=malloc(bytes);if(p) allocations3d++;return p; }
static void free3d(void *ctx,void *p)
{ (void)ctx;CHECK(!locks && p && allocations3d);allocations3d--;free(p); }
static const struct radeon_group_info group3d={"fixture",RADEON_RECT,0};
static void prepare(unsigned bpp)
{
    CHECK(!locks && !allocations3d && bpp==32); memset(&m,0,sizeof(m));
    memset(ram3d,0x7a,sizeof(ram3d)); draw3d=copy3d=writes3d=memory3d=count3d=0;
    corruption3d=read_delay3d=write_delay3d=delay_calls3d=0;frozen3d=false; dropping3d=bad_restore3d=fifo_stuck3d=cache_stuck3d=false;
    test_engine=(struct radeon_engine){.group=&group3d,.group_id=RADEON_R100,.active=true,.cpu_safe=true,
        .render3d_qualified=true,.local_bytes=16*1024*1024,
        .io={0,read_engine3d,write_engine3d,clock3d,delay3d,drain3d}};
}
static void finish(void) { CHECK(!locks && !count3d && !allocations3d); }
static uint32_t texture_pixels3d[256*256*2];
static void texture_fixture3d(struct render3d_texture_desc *d,unsigned format,unsigned size,unsigned levels)
{
    *d=(struct render3d_texture_desc){.format=format,.width=size,.height=size,.levels=levels};
    unsigned offset=0;
    for(unsigned level=0,side=size;level<levels;level++,side=side>1 ? side/2 : 1) {
        for(unsigned y=0;y<side;y++) for(unsigned x=0;x<side;x++)
            texture_pixels3d[offset+y*side+x]=0xff000000u|((96+x*32/side+level*4)<<16)|((112+y*32/side+level*4)<<8)|(128+level*4);
        d->image[level]=(struct render3d_texture_image){texture_pixels3d+offset,side*side*4,side*4};offset+=side*side;
    }
}
static void fullsize_resources3d(struct render3d_service *s,const struct render3d_host *host,unsigned w,unsigned h,bool hardware)
{
    render3d_init(s,host,42,0,0,(struct render3d_extent){0});
    uint32_t c,handle; CHECK(!render3d_create_context(s,1,R3D_SOFTWARE,&c));
    const unsigned kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR,R3D_TEXTURE};
    unsigned at=gpu3d.heap.first,tex=gpu3d.heap.first+gpu3d.heap.bytes-gpu3d.texture_bytes;
    for(unsigned i=0;i<5;i++) {
        if(kinds[i]==R3D_TEXTURE) {
            struct render3d_texture_desc t;texture_fixture3d(&t,R3D_ARGB8888,i==2 ? 256 : 128,i==2 ? 9 : 8);
            CHECK(!render3d_create_texture(s,1,c,&t,&handle));
        } else CHECK(!render3d_create_resource(s,1,c,kinds[i],w,h,0,0,&handle));
        if(hardware) {
            struct render3d_resource *r=&s->contexts[0].resources[i];
            r->hardware=true;r->offset=kinds[i]==R3D_TEXTURE ? tex : at;
            CHECK(!radeon3d_backend()->layout(&gpu3d,r));
            if(kinds[i]==R3D_TEXTURE) tex+=r->reserved;else at+=r->reserved;
        }
    }
}
