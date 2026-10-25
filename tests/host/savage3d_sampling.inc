/* Independent interpreter of Mesa's register image and texture address rules.
 * Host-only long double arithmetic; no production sampling/packing helpers.
 * SPDX-License-Identifier: GPL-2.0-only */
static bool compare3d(unsigned a,unsigned b,unsigned cf)
{
    switch (cf) {
    case 0: return false; case 1: return a<b; case 2: return a==b;
    case 3: return a<=b; case 4: return a>b; case 5: return a!=b;
    case 6: return a>=b; default: return true;
    }
}
static uint32_t texture_address3d(unsigned stage,unsigned level,unsigned x,unsigned y)
{
    uint32_t td=m.reg[0x485a8/4]>>(stage*12),addr=m.reg[(0x48594+stage*4)/4]&~7u;
    unsigned w=1u<<(td&15),h=1u<<(td>>4&15),format=td>>8&15,bpp=format==3 ? 4 : 2;
    CHECK(format==3 || format==5 || format==6);
    for (unsigned i=0;i<level;i++) {
        if (w>4 || h>4) addr+=(w<8 ? 8 : w)*(h<8 ? 8 : h)*bpp;
        w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
    }
    CHECK(x<w && y<h);
    if (w<=4 && h<=4) {
        unsigned side=w>h ? w : h;
        /* Mesa tinyOffset[0] is size ONE; tinyOffset[1] is size TWO. */
        return addr+(side==1 ? 36 : side==2 ? 4 : 0)*bpp+(y*8+x)*bpp;
    }
    unsigned tilew=bpp==4 ? 32 : 64,pw=w<8 ? 8 : w,ph=h<8 ? 8 : h;
    unsigned cols=pw<tilew ? pw/8 : tilew/8,rows=ph<16 ? ph/8 : 2;
    unsigned tile=(y/(rows*8))*(pw/(cols*8))+x/(cols*8);
    unsigned sub=(y/8%rows)*cols+x/8%cols;
    return addr+(tile*cols*rows*64+sub*64+y%8*8+x%8)*bpp;
}
static uint32_t texel3d(unsigned stage,unsigned level,int x,int y)
{
    uint32_t td=m.reg[0x485a8/4]>>(stage*12),tc=m.reg[(0x4858c+stage*4)/4];
    unsigned w=(1u<<(td&15))>>level,h=(1u<<(td>>4&15))>>level,fmt=td>>8&15;
    if (!w) w=1;
    if (!h) h=1;
    if (tc&0x10000) x=x<0 ? 0 : (unsigned)x>=w ? (int)w-1 : x;
    else x=(int)((unsigned)x&(w-1));
    if (tc&0x40000) y=y<0 ? 0 : (unsigned)y>=h ? (int)h-1 : y;
    else y=(int)((unsigned)y&(h-1));
    uint32_t t=load3d(texture_address3d(stage,level,(unsigned)x,(unsigned)y),fmt==3 ? 4 : 2);
    if (fmt==6) {
        unsigned r=t>>11,g=t>>5&63,b=t&31;
        return 0xff000000u|((r*8+r/4)<<16)|((g*4+g/16)<<8)|b*8+b/4;
    }
    if (fmt==5) return ((t>>12)*17<<24)|((t>>8&15)*17<<16)|((t>>4&15)*17<<8)|(t&15)*17;
    return t;
}
static long double sample_channel3d(unsigned stage,unsigned level,long double u,long double v,unsigned shift,bool linear)
{
    uint32_t td=m.reg[0x485a8/4]>>(stage*12);
    unsigned w=(1u<<(td&15))>>level,h=(1u<<(td>>4&15))>>level;
    if (!w) w=1;
    if (!h) h=1;
    long double x=u*w-(linear ? .5L : 0),y=v*h-(linear ? .5L : 0);
    int ix=(int)floorl(x),iy=(int)floorl(y);
    unsigned a=(texel3d(stage,level,ix,iy)>>shift)&255;
    if (!linear) return a;
    long double fx=x-ix,fy=y-iy;
    unsigned b=(texel3d(stage,level,ix+1,iy)>>shift)&255;
    unsigned c=(texel3d(stage,level,ix,iy+1)>>shift)&255;
    unsigned d=(texel3d(stage,level,ix+1,iy+1)>>shift)&255;
    return (a*(1-fx)+b*fx)*(1-fy)+(c*(1-fx)+d*fx)*fy;
}
static uint32_t sample3d(unsigned stage,long double u,long double v,long double rho)
{
    uint32_t tc=m.reg[(0x4858c+stage*4)/4];
    unsigned filter=tc&3,max=tc>>12&15;
    CHECK(filter!=2 && ((tc>>3)&511)==((tc&4) ? 10 : 8)); /* pinned Mesa mip LOD offset */
    long double l=(tc&4) && rho>1 ? log2l(rho) : 0;
    if (l>max) l=max;
    unsigned level=filter==3 ? (unsigned)l : (unsigned)(l+.5L);
    bool linear=filter!=0;
    uint32_t out=0;
    for (unsigned shift=0;shift<32;shift+=8) {
        long double c=sample_channel3d(stage,level,u,v,shift,linear);
        if (filter==3 && level<max) c=c*(1-(l-level))+sample_channel3d(stage,level+1,u,v,shift,true)*(l-level);
        out|=(uint32_t)(c+.5L)<<shift;
    }
    return out;
}
static uint32_t combine3d(uint32_t src,uint32_t tex,unsigned stage)
{
    uint32_t bc=m.reg[(0x4859c+stage*4)/4],out=0;
    for (unsigned shift=0;shift<32;shift+=8) {
        unsigned a=src>>shift&255,b=tex>>shift&255,c=0;
        switch (bc) {
        case 0x00110011: case 0x00130013: c=(a*b+127)/255; break;
        case 0x00840410: c=b; break;
        case 0x00850410: case 0x00870410: c=shift==24 ? a : b; break;
        case 0x00900405: case 0x00900400: c=shift==24 ? b : a; break;
        case 0x00852a04: case 0x00832a00: c=shift==24 ? a : (a*(255-(tex>>24))+b*(tex>>24)+127)/255; break;
        case 0x18110c11: case 0x18130c13: c=shift==24 ? (a*b+127)/255 : a+b>255 ? 255 : a+b; break;
        default: CHECK(false);
        }
        out|=c<<shift;
    }
    return out;
}
static uint32_t blend3d(uint32_t src,uint32_t dst,uint32_t local)
{
    unsigned sf=local>>4&7,df=local&7,alpha=src>>24;
    unsigned factors[8]={0,255,0,0,alpha,255-alpha,0,0};
    CHECK((sf==0 || sf==1 || sf==4 || sf==5) && (df==0 || df==1 || df==4 || df==5));
    uint32_t out=0;
    for (unsigned shift=0;shift<32;shift+=8) {
        unsigned c=(((src>>shift)&255)*factors[sf]+((dst>>shift)&255)*factors[df]+127)/255;
        out|=(c>255 ? 255 : c)<<shift;
    }
    return out;
}
