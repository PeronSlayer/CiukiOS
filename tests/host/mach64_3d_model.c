/* Independent Mach64 setup-engine and 2D-blit model. Register offsets are
 * spelled out here (BAR2 page = Mesa mach64_reg.h offset + 0x800), not taken
 * from the production headers. The expected-pixel path uses host long double;
 * it never calls the scalar renderer, the production encoder or layout.
 * Hypotheses that the sources leave open (sample centre, tie rule, depth and
 * colour rounding, alpha multiplexer) are explicit and checked on silicon by
 * the boot qualification, not proven here. SPDX-License-Identifier: GPL-2.0-only */
#include "mach64_3d_model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct mach64_3d_model m64m;
#define M m64m
enum {
    OOA=0xa9c, V1=0xa40, V2=0xa60, V3=0xa80, SEC1=0xb28, SEC2=0xb34, SEC3=0xaa0, SETUP=0xb04, OOA_UC=0xb00,
    BUS=0xca0, CHIP=0xce0, CHIP_LOW=0x4e0,
    DST_OP=0xd00, DST_YX=0xd0c, DST_HW=0xd18, DST_CN=0xd30, Z_OP=0xd48, Z_CN=0xd4c, ALPHA=0xd50,
    SRC_OP=0xd80, SRC_YX=0xd8c, SRC_HW=0xd98, TEX0=0xdc0, SCALE=0xdfc,
    SC_L=0xea0, SC_R=0xea4, SC_LR=0xea8, SC_T=0xeac, SC_B=0xeb0, SC_TB=0xeb4,
    FRGD=0xec4, WMASK=0xec8, PIX=0xed0, MIX=0xed4, SRC=0xed8, CMP=0xf08, FIFO=0xf10, GUI=0xf38,
    TSIZE=0xf70, TCNTL=0xf74, SECOFF=0xf78,
    GEN_TEST=0xcd0 /* 2D quarantine hides the cursor (engine.c hide) */
};
static uint32_t reg(unsigned a) { assert(a<4096 && !(a&3)); return M.regs[a/4]; }
static bool writable(unsigned a)
{
    if ((a>=V1 && a<=V1+0x18) || (a>=V2 && a<=V2+0x18) || (a>=V3 && a<=V3+0x18) || a==OOA ||
        (a>=SEC3 && a<=SEC3+8) || (a>=SEC1 && a<=SEC2+8) || (a>=TEX0 && a<=TEX0+40)) return true;
    static const unsigned list[]={SETUP,DST_OP,DST_YX,DST_HW,DST_CN,Z_OP,Z_CN,ALPHA,SRC_OP,SRC_YX,SRC_HW,
        SCALE,SC_LR,SC_TB,FRGD,WMASK,PIX,MIX,SRC,CMP,TSIZE,TCNTL,SECOFF};
    for (unsigned i=0;i<sizeof(list)/sizeof(*list);i++) if (a==list[i]) return true;
    return a==GEN_TEST && M.engine.quarantined;
}
static bool readable(unsigned a)
{
    if (a>=TEX0 && a<=TEX0+40) return true;
    static const unsigned list[]={FIFO,GUI,BUS,CHIP,CHIP_LOW,DST_OP,DST_CN,SRC_OP,SCALE,SC_L,SC_R,SC_T,SC_B,
        FRGD,WMASK,PIX,MIX,SRC,CMP,TSIZE,TCNTL,GEN_TEST};
    for (unsigned i=0;i<sizeof(list)/sizeof(*list);i++) if (a==list[i]) return true;
    return false;
}
static uint8_t *physical(uint32_t a,unsigned n)
{
    /* Every engine and CPU access stays inside the claimed heap. */
    assert(n && a>=M.device.heap.first && (uint64_t)a+n<=(uint64_t)M.device.heap.first+M.device.heap.bytes);
    return M.vram+a;
}
static uint32_t load(uint32_t a,unsigned n) { uint32_t v=0; memcpy(&v,physical(a,n),n); return v; }
static void store(uint32_t a,uint32_t v,unsigned n) { memcpy(physical(a,n),&v,n); }
static long double flt(uint32_t bits) { float f; memcpy(&f,&bits,4); assert(isfinite(f)); return f; }
static bool cmp(unsigned a,unsigned b,unsigned f)
{
    switch (f&7) { case 0:return false; case 1:return a<b; case 2:return a<=b; case 3:return a==b;
    case 4:return a>=b; case 5:return a>b; case 6:return a!=b; default:return true; }
}
struct surface { uint32_t base, pitch; };
static struct surface decode(uint32_t op,unsigned bytes) { return (struct surface){(op&0xfffffu)*8,(op>>22)*8*bytes}; }
static unsigned clampi(int a,unsigned n,bool clamp) { return clamp ? (a<0 ? 0 : (unsigned)a>=n ? n-1 : (unsigned)a) : (unsigned)a&(n-1); }
static uint32_t mix(uint32_t a,uint32_t b,unsigned f)
{
    uint32_t c=0;
    for (unsigned s=0;s<32;s+=8) c|=((((a>>s)&255)*(256-f)+((b>>s)&255)*f+128)>>8)<<s;
    return c;
}
struct unit { uint32_t base; unsigned w, h; bool linear, clamp_s, clamp_t; };
static uint32_t sample(const struct unit *t,long double u,long double v)
{
    long double x=u*t->w-(t->linear ? .5L : 0),y=v*t->h-(t->linear ? .5L : 0);
    int ix=(int)floorl(x),iy=(int)floorl(y);
    unsigned xx=clampi(ix,t->w,t->clamp_s),yy=clampi(iy,t->h,t->clamp_t);
    uint32_t a=load(t->base+yy*t->w*4+xx*4,4);
    if (!t->linear) return a;
    /* Eight filter-fraction bits: a model choice, measured at boot. */
    unsigned fx=(unsigned)floorl((x-ix)*256),fy=(unsigned)floorl((y-iy)*256);
    unsigned x1=clampi(ix+1,t->w,t->clamp_s),y1=clampi(iy+1,t->h,t->clamp_t);
    return mix(mix(a,load(t->base+yy*t->w*4+x1*4,4),fx),
               mix(load(t->base+y1*t->w*4+xx*4,4),load(t->base+y1*t->w*4+x1*4,4),fx),fy);
}
static unsigned alpha_mux(unsigned sel,unsigned src,unsigned dst)
{
    switch (sel) { case 0:return 0; case 1:return 255; case 4:return src; case 5:return 255-src;
    case 6:return dst; case 7:return 255-dst; default: assert(0); return 0; }
}
static unsigned factor(unsigned f,bool source,uint32_t s,uint32_t d,unsigned shift)
{
    switch (f) {
    case 0: return 0; case 1: return 255;
    case 2: return source ? (d>>shift)&255 : (s>>shift)&255;
    case 3: return 255-(source ? (d>>shift)&255 : (s>>shift)&255);
    case 4: return s>>24; case 5: return 255-(s>>24);
    case 6: return d>>24; default: return 255-(d>>24);
    }
}
static void triangle(void)
{
    M.draws++;
    /* Mesa/DRM 3D state: source 3D, mix source, no colour compare, Gouraud
     * setup, ARGB8888 destination, scale and texture formats. */
    assert(reg(SRC)==0x505 && reg(MIX)==0x70007 && !reg(CMP) && reg(DST_CN)==3 && !reg(SETUP));
    uint32_t pix=reg(PIX),scale=reg(SCALE),zc=reg(Z_CN),al=reg(ALPHA),tc=reg(TCNTL),ts=reg(TSIZE);
    assert((pix&15)==6 && ((pix>>4)&15)==6 && ((pix>>8)&15)==6 && ((pix>>16)&15)==6 && (pix>>28)==6);
    assert((scale&0x1fu)==0x1bu && (scale&(1u<<24)) && reg(WMASK)==UINT32_MAX);
    unsigned fcn=(scale>>6)&3; assert(fcn==2 || fcn==3);
    bool textured=fcn==2,composite=textured && (tc&0x100u);
    assert(!(al&(1u<<12)));
    if (textured) assert(!(tc&(1u<<19)) && !(tc&(1u<<20)) && (tc&(1u<<9)) && (tc&(1u<<30)));
    long double x[3],y[3],z[3],w[3],s0[3],t0[3],s1[3],t1[3]; uint32_t c[3];
    static const unsigned vb[3]={V1,V2,V3},sb[3]={SEC1,SEC2,SEC3};
    for (unsigned i=0;i<3;i++) {
        uint32_t xy=reg(vb[i]+0x18);
        x[i]=(int16_t)(xy>>16)/4.0L; y[i]=(int16_t)(xy&0xffff)/4.0L;
        z[i]=reg(vb[i]+0x10)/65536.0L; c[i]=reg(vb[i]+0x14); assert(!reg(vb[i]+0xc));
        w[i]=flt(reg(vb[i]+8)); s0[i]=flt(reg(vb[i])); t0[i]=flt(reg(vb[i]+4));
        s1[i]=composite ? flt(reg(sb[i])) : 0; t1[i]=composite ? flt(reg(sb[i]+4)) : 0;
        if (composite) assert(reg(sb[i]+8)==reg(vb[i]+8)); /* shared reciprocal W */
    }
    long double cross=(x[0]-x[2])*(y[1]-y[2])-(y[0]-y[2])*(x[1]-x[2]);
    assert(cross!=0);
    long double ooa=flt(reg(OOA));
    assert(fabsl(ooa-1/cross)<=fabsl(1/cross)*0x1p-24L);
    if (M.drop) return;
    if (cross<0) {
        long double *arrays[]={x,y,z,w,s0,t0,s1,t1};
        for (unsigned i=0;i<8;i++) { long double tmp=arrays[i][1]; arrays[i][1]=arrays[i][2]; arrays[i][2]=tmp; }
        uint32_t tmp=c[1]; c[1]=c[2]; c[2]=tmp; cross=-cross;
    }
    unsigned left=reg(SC_L),right=reg(SC_R),top=reg(SC_T),bottom=reg(SC_B);
    struct surface dst=decode(reg(DST_OP),4),zs=decode(reg(Z_OP),2);
    assert(right<1024 && bottom<1024 && dst.pitch>=(right+1)*4);
    struct unit u0={0},u1={0};
    if (textured) {
        unsigned lw=ts&15,lmax=(ts>>4)&15,lh=(ts>>8)&15; assert(lmax==(lw>lh ? lw : lh) && lmax<=10);
        bool linear=(scale>>26&3)!=0,mag=(scale&(1u<<25))!=0;
        assert(linear==mag && ((scale>>26&3)==3)==composite && ((scale&(1u<<9))!=0)==composite);
        u0=(struct unit){reg(TEX0+lmax*4),1u<<lw,1u<<lh,linear,(tc&(1u<<17))!=0,(tc&(1u<<18))!=0};
        if (composite) {
            unsigned l1=(ts>>16)&15,m1=(ts>>20)&15,h1=(ts>>24)&15; assert(m1==(l1>h1 ? l1 : h1));
            assert(((tc>>11)&1)==((tc>>12)&1) && (tc&(1u<<31)));
            u1=(struct unit){reg(SECOFF),1u<<l1,1u<<h1,(tc&(1u<<11))!=0,(tc&(1u<<24))!=0,(tc&(1u<<25))!=0};
        } else assert(!(ts>>16));
    }
    for (int yy=(int)fmaxl(top,floorl(fminl(y[0],fminl(y[1],y[2]))));yy<=(int)bottom && yy<ceill(fmaxl(y[0],fmaxl(y[1],y[2])));yy++)
    for (int xx=(int)fmaxl(left,floorl(fminl(x[0],fminl(x[1],x[2]))));xx<=(int)right && xx<ceill(fmaxl(x[0],fmaxl(x[1],x[2])));xx++) {
        /* Hypothesis H-COV: samples at pixel centres; ties belong to top and
         * left edges (the Ciuki reference rule). wrong_tie flips it. */
        long double px=xx+.5L,py=yy+.5L,e[3];
        bool inside=true;
        for (unsigned i=0;i<3;i++) {
            unsigned a=(i+1)%3,b=(i+2)%3;
            e[i]=(x[b]-x[a])*(py-y[a])-(y[b]-y[a])*(px-x[a]);
            bool topleft=y[b]<y[a] || (y[b]==y[a] && x[b]>x[a]);
            if (M.wrong_tie) topleft=!topleft;
            if (e[i]<0 || (e[i]==0 && !topleft)) inside=false;
        }
        if (!inside) continue;
        long double b0=e[0]/cross,b1=e[1]/cross,b2=e[2]/cross;
        long double zf=b0*z[0]+b1*z[1]+b2*z[2];
        unsigned depth=M.depth_truncate ? (unsigned)floorl(zf) : (unsigned)floorl(zf+.5L);
        if (depth>65535) depth=65535;
        uint32_t za=zs.base+(unsigned)yy*zs.pitch+(unsigned)xx*2;
        if ((zc&1) && !cmp(depth,load(za,2),zc>>4)) continue;
        uint32_t frag=0;
        for (unsigned sh=0;sh<32;sh+=8)
            frag|=(uint32_t)floorl(b0*(c[0]>>sh&255)+b1*(c[1]>>sh&255)+b2*(c[2]>>sh&255)+.5L)<<sh;
        unsigned texel_alpha=255;
        if (textured) {
            long double den=b0*w[0]+b1*w[1]+b2*w[2];
            long double uu=(b0*w[0]*s0[0]+b1*w[1]*s0[1]+b2*w[2]*s0[2])/den;
            long double vv=(b0*w[0]*t0[0]+b1*w[1]*t0[1]+b2*w[2]*t0[2])/den;
            uint32_t t=sample(&u0,uu,vv),out=0;
            bool aen=(scale&(1u<<30))!=0;
            unsigned fn=(scale>>22)&3,ta=t>>24;
            texel_alpha=ta;
            for (unsigned sh=0;sh<32;sh+=8) {
                unsigned f=frag>>sh&255,tv=t>>sh&255,v;
                if (sh==24) v=fn==2 ? f : aen ? (fn==0 ? tv : (f*tv+127)/255) : f;
                else v=fn==0 ? tv : fn==1 ? (f*tv+127)/255 : (f*(255-ta)+tv*ta+127)/255;
                out|=v<<sh;
            }
            frag=out;
            if (composite) {
                long double u2=(b0*w[0]*s1[0]+b1*w[1]*s1[1]+b2*w[2]*s1[2])/den;
                long double v2=(b0*w[0]*t1[0]+b1*w[1]*t1[1]+b2*w[2]*t1[2])/den;
                uint32_t t2=sample(&u1,u2,v2); out=0;
                for (unsigned sh=0;sh<32;sh+=8) {
                    unsigned f=frag>>sh&255,tv=t2>>sh&255;
                    out|=(sh==24 && !(tc&(1u<<13)) ? f : (f*tv+127)/255)<<sh;
                }
                frag=out;
            }
        }
        /* ALPHA_TST_SRC_TEXEL: the unit-0 texel alpha. */
        if ((al&1) && !cmp(texel_alpha,(al>>16)&255,al>>4)) continue;
        uint32_t ca=dst.base+(unsigned)yy*dst.pitch+(unsigned)xx*4,old=load(ca,4),result=frag&0xffffffu;
        if (((scale>>11)&3)==1) {
            result=0;
            for (unsigned sh=0;sh<24;sh+=8) {
                unsigned v=((frag>>sh&255)*factor(scale>>16&7,true,frag,old,sh)+
                            (old>>sh&255)*factor(scale>>19&7,false,frag,old,sh)+127)/255;
                result|=(v>255 ? 255u : v)<<sh;
            }
        } else assert(!((scale>>11)&3) && ((scale>>16)&63)==1);
        result|=alpha_mux((al>>8)&7,frag>>24,old>>24)<<24;
        store(ca,result,4);
        if ((zc&1) && (zc&0x100)) store(za,depth,2);
        M.fragments++;
    }
    if (M.hang_after_draw) M.stuck=true;
}
static void blit2d(void)
{
    /* DRM clear/swap preconditions: no Z or 3D function during 2D. */
    assert(!reg(Z_CN) && !reg(SCALE) && !reg(CMP) && reg(DST_CN)==0x23 && reg(WMASK)==UINT32_MAX);
    uint32_t pix=reg(PIX),type=pix&15; assert(type==4 || type==6);
    bool owner2d=pix==(0x01000000u|type|type<<8|type<<16);
    assert(owner2d || pix==(type|type<<4|type<<8|type<<16|type<<28));
    unsigned bytes=type==4 ? 2 : 4,width=reg(DST_HW)>>16,height=reg(DST_HW)&0xffff;
    unsigned dx=reg(DST_YX)>>16,dy=reg(DST_YX)&0xffff;
    struct surface dst=decode(reg(DST_OP),bytes);
    assert((reg(MIX)==0x70003 || (owner2d && reg(MIX)==0x70000)) && width && height);
    bool blit=reg(SRC)==0x300;
    assert(blit || reg(SRC)==0x101 || (owner2d && reg(SRC)==0x100));
    struct surface src=decode(reg(SRC_OP),bytes);
    if (blit) assert(reg(SRC_HW)==reg(DST_HW) && (type==6 || owner2d));
    for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++) {
        unsigned px=dx+x,py=dy+y;
        if (px<reg(SC_L) || px>reg(SC_R) || py<reg(SC_T) || py>reg(SC_B)) continue;
        unsigned sx=(reg(SRC_YX)>>16)+x,sy=(reg(SRC_YX)&0xffff)+y;
        uint32_t v=blit ? load(src.base+sy*src.pitch+sx*bytes,bytes) : reg(FRGD)&(bytes==2 ? 0xffffu : UINT32_MAX);
        if (!(owner2d && M.bad_selfcheck)) store(dst.base+py*dst.pitch+px*bytes,v,bytes);
    }
    if (blit) M.blits++; else M.fills++;
    if (M.corrupt_guard) store(dst.base-4,0,4);
    if (M.hang_after_draw) M.stuck=true;
}
static uint32_t read_io(void *ctx,unsigned a)
{
    (void)ctx; M.reads++;
    assert(readable(a));
    if (M.dead) return UINT32_MAX;
    if (a==FIFO) {
        bool full=M.full || (M.full_after && M.writes>=M.full_after);
        M.credits=full ? 0 : 16;
        return (M.fifo_error ? 0x80000000u : 0)|(full ? 0xffffu : 0);
    }
    if (a==GUI) {
        if (M.stuck) return 1;
        if (M.busy) { M.busy--; return 1; }
        return 0;
    }
    if (a==BUS) return M.block1_off ? 0x7333a040u : 0x7b33a040u;
    if (a==CHIP_LOW) return M.mirror_off ? 0 : reg(CHIP);
    uint32_t v=reg(a);
    if (M.bad_restore && a==PIX && M.writes) v^=1;
    if (M.bad_texture_restore && a==TEX0+5*4 && M.writes) v^=1;
    return v;
}
static void write_io(void *ctx,unsigned a,uint32_t v)
{
    (void)ctx;
    assert(writable(a));
    if (a==GEN_TEST) { assert(M.locks); M.regs[a/4]=v; return; }
    assert(M.locks);
    /* FIFO discipline: never more writes than the last read proved free. */
    assert(M.credits>0); M.credits--; M.writes++;
    if (a==OOA_UC) assert(0);
    M.regs[a/4]=v;
    if (a==TCNTL) M.regs[a/4]&=~(1u<<23); /* cache flush self-clears */
    if (a==SC_LR) { M.regs[SC_L/4]=v&0xffff; M.regs[SC_R/4]=v>>16; }
    if (a==SC_TB) { M.regs[SC_T/4]=v&0xffff; M.regs[SC_B/4]=v>>16; }
    if (a==OOA) { triangle(); M.busy=2; }
    if (a==DST_HW) { blit2d(); M.busy=2; }
}
static void drain(void *ctx) { (void)ctx; }
uint64_t m64m_now(void *ctx) { (void)ctx; return M.frozen ? M.clock : M.clock++; }
static uint32_t read_mem(void *ctx,uint32_t a,unsigned n) { (void)ctx; assert(M.locks); M.memory_reads++; return load(a,n); }
static void write_mem(void *ctx,uint32_t a,uint32_t v,unsigned n) { (void)ctx; assert(M.locks); store(a,v,n); M.memory_writes++; }
static int lock(void *ctx,uint32_t generation) { (void)ctx; assert(generation==42 && !M.locks); M.locks++; return 0; }
static void unlock(void *ctx) { (void)ctx; assert(M.locks==1); M.locks--; }
/* Allocations are tracked so that a quarantined (retained) boot can still be
 * torn down like a reboot without leaking under ASan/LSan. */
static void *live[8192];
static unsigned live_count;
void *m64m_alloc(void *ctx,size_t n)
{
    (void)ctx; assert(!M.locks);
    void *p=calloc(1,n);
    if (p) { assert(live_count<8192); live[live_count++]=p; }
    return p;
}
void m64m_free(void *ctx,void *p)
{
    (void)ctx; assert(!M.locks);
    if (!p) return;
    for (unsigned i=0;i<live_count;i++) if (live[i]==p) { live[i]=live[--live_count]; free(p); return; }
    assert(0);
}
void m64m_init(unsigned w,unsigned h)
{
    free(M.vram);
    memset(&M,0,sizeof(M)); M.vram=calloc(1,M64M_VRAM); assert(M.vram);
    struct m64_io io={0,read_io,write_io,m64m_now,drain};
    struct fb_device fb={.present=true,.mapped=M.vram,.generation=42,.width=w,.height=h,.bpp=32,
        .pitch=w*4,.size=w*h*4,.red_size=8,.red_pos=16,.green_size=8,.green_pos=8,.blue_size=8,.phys=0xe0000000};
    m64_attach(&M.engine,&fb,&io,0,0);
    M.engine.active=M.engine.cpu_safe=M.engine.touched=true; M.engine.reason="active";
    M.engine.vram_bytes=M64M_VRAM;
    /* Registers as the 2D driver's m64_configure leaves them (32 bpp). */
    M.regs[CHIP/4]=0x00004c4du;
    M.regs[DST_OP/4]=M.regs[SRC_OP/4]=(w/8)<<22;
    M.regs[DST_CN/4]=0x23; M.regs[PIX/4]=0x01060606u; M.regs[FRGD/4]=UINT32_MAX; M.regs[WMASK/4]=UINT32_MAX;
    M.regs[MIX/4]=0x70003; M.regs[SRC/4]=0x100;
    M.regs[SC_LR/4]=(w-1)<<16; M.regs[SC_R/4]=w-1; M.regs[SC_TB/4]=(h-1)<<16; M.regs[SC_B/4]=h-1;
    uint32_t first=(w*h*4+4095)&~4095u;
    M.device=(struct mach64_3d_device){.engine=&M.engine,.heap={first,0x7ff000u-first},.generation=42,
        .mode_width=w,.mode_height=h,.memory={0,read_mem,write_mem,lock,unlock}};
    M.device.texture_bytes=2u*1024*1024;
}
void m64m_destroy(void)
{
    assert(!M.locks); free(M.vram); M.vram=0;
    while (live_count) free(live[--live_count]);
}
static const struct render3d_host host={0,m64m_alloc,m64m_free,m64m_now,0};
void m64m_resources(struct render3d_service *s,unsigned w,unsigned h,bool hw,bool full)
{
    render3d_init(s,&host,42,0,0,(struct render3d_extent){0});
    uint32_t c,id;
    assert(!render3d_create_context(s,7,R3D_SOFTWARE,&c));
    const unsigned kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR,full ? R3D_TEXTURE : R3D_COLOR};
    uint32_t at=M.device.heap.first,ta=M.device.heap.first+M.device.heap.bytes-M.device.texture_bytes;
    for (unsigned i=0;i<5;i++) {
        if (kinds[i]==R3D_TEXTURE) {
            unsigned n=full ? (i==2 ? 256 : 128) : 32,hh=full ? n : 16;
            uint32_t *pixels=calloc(1,n*hh*4); assert(pixels);
            for (unsigned y=0;y<hh;y++) for (unsigned x=0;x<n;x++)
                pixels[y*n+x]=full ? 0xff000000u|((96+x*32/n)<<16)|((112+y*32/hh)<<8)|(128+i) :
                    x<16 ? 0xff4080c0u : 0xffc04080u;
            struct render3d_texture_desc t={.format=R3D_ARGB8888,.width=n,.height=hh,.levels=1,.image={{pixels,n*hh*4,n*4}}};
            assert(!render3d_create_texture(s,7,c,&t,&id)); free(pixels);
        } else assert(!render3d_create_resource(s,7,c,kinds[i],w,h,0,0,&id));
        struct render3d_resource *r=&s->contexts[0].resources[i];
        if (hw && (full || i<4)) {
            r->hardware=true; r->offset=kinds[i]==R3D_TEXTURE ? ta : at;
            assert(!mach64_3d_backend()->layout(&M.device,r));
            if (kinds[i]==R3D_TEXTURE) ta+=r->reserved; else at+=r->reserved;
        }
    }
}
static bool volatile_register(unsigned a)
{
    return (a>=0xa40 && a<=0xaa8) || (a>=SEC1 && a<=SEC2+8) || a==DST_YX || a==DST_HW || a==SRC_YX ||
        a==SRC_HW || a==Z_OP || a==SECOFF || a==FIFO || a==GUI;
}
void m64m_snapshot(uint32_t out[1024]) { memcpy(out,M.regs,sizeof(M.regs)); }
bool m64m_restored(const uint32_t before[1024])
{
    for (unsigned i=0;i<1024;i++) if (!volatile_register(i*4) && M.regs[i]!=before[i]) {
        fprintf(stderr,"register %03x changed %08x -> %08x\n",i*4,before[i],M.regs[i]);
        return false;
    }
    return true;
}
