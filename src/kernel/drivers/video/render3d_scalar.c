/* Scalar fixed function: top-left coverage, D16, two samplers and fragment state.
 * All intermediates bounded by render3d.c admission. No floating point/SIMD.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "render3d.h"
static int64_t edge(const struct render3d_vertex *a, const struct render3d_vertex *b, int64_t x, int64_t y)
{ return ((int64_t)b->x-a->x)*(y-a->y)-((int64_t)b->y-a->y)*(x-a->x); }
static bool top_left(const struct render3d_vertex *a, const struct render3d_vertex *b)
{ return b->y<a->y || (b->y==a->y && b->x>a->x); }
/* floor(a/b * 65536) without overflowing the 52-bit edge product. */
static uint32_t fraction(uint64_t a, uint64_t b)
{
    if (a==b) return 65536;
    uint32_t q=0;
    for (unsigned i=0;i<16;i++) { a*=2; q<<=1; if (a>=b) { a-=b; q++; } }
    return q;
}
static uint32_t interpolate(uint32_t a, uint32_t b, uint32_t c, const uint32_t w[3])
{ return (uint32_t)(((uint64_t)a*w[0]+(uint64_t)b*w[1]+(uint64_t)c*w[2]+32768)>>16); }
static bool compare(uint32_t a,uint32_t b,unsigned func)
{
    switch (func) {
    case R3D_LESS: return a<b;
    case R3D_NEVER: return false;
    case R3D_ALWAYS: return true;
    case R3D_EQUAL: return a==b;
    case R3D_LEQUAL: return a<=b;
    case R3D_GREATER: return a>b;
    case R3D_NOTEQUAL: return a!=b;
    default: return a>=b;
    }
}
static int32_t coordinate(const struct render3d_vertex v[3],const int32_t uv[3],const int64_t w[3])
{
    int64_t denom=0,numer=0,tail=0;
    for (unsigned i=0;i<3;i++) {
        int64_t product=(int64_t)uv[i]*v[i].rhw;
        denom+=(int64_t)v[i].rhw*w[i];
        /* Keep all Q32 product bits. Signed mip-neighbour weights can make
         * the unsplit weighted sum exceed int64 even though UV is bounded.
         * Here |numer| < 2^51, |tail| < 2^39, |denom| <= 2^46+2^40. */
        numer+=(product/65536)*w[i];
        tail+=(product%65536)*w[i];
    }
    /* Neighbour samples behind W=0 select a maximally coarse mip. */
    if (denom<=0) return 32*65536;
    numer+=tail/65536; tail%=65536;
    int64_t whole=numer/denom;
    if (whole>32) return 32*65536;
    if (whole < -32) return -32*65536;
    /* Divide only after recovery. Remainder scaling stays below 2^63;
     * carry a fractional remainder of opposite sign toward zero as well. */
    int64_t remainder=(numer%denom)*65536+tail;
    int64_t value=whole*65536+remainder/denom;
    if (value>0 && remainder%denom<0) value--;
    if (value<0 && remainder%denom>0) value++;
    if (value>32*65536) value=32*65536;
    if (value < -32*65536) value=-32*65536;
    return (int32_t)value;
}
static unsigned address(int32_t x,unsigned size,bool repeat)
{
    if (repeat) return (unsigned)x&(size-1);
    return x<0 ? 0 : (unsigned)x>=size ? size-1 : (unsigned)x;
}
static uint32_t mix(uint32_t a,uint32_t b,unsigned fraction)
{
    uint32_t result=0;
    for (unsigned shift=0;shift<32;shift+=8)
        result|=(((((a>>shift)&255)*(256-fraction)+((b>>shift)&255)*fraction)+128)>>8)<<shift;
    return result;
}
static uint32_t sample_level(const struct render3d_resource *r,const struct render3d_sampler *s,
                             unsigned level,int32_t u,int32_t v,bool linear)
{
    unsigned w=r->width>>level,h=r->height>>level;
    if (!w) w=1;
    if (!h) h=1;
    const uint32_t *data=(const void *)((const uint8_t *)r->shadow+r->level_offset[level]);
    int32_t x=u*(int32_t)w-(linear ? 32768 : 0),y=v*(int32_t)h-(linear ? 32768 : 0);
    /* Explicit floor for negative repeat coordinates, no signed shifts. */
    int32_t ix=x>=0 ? x/65536 : -1-(-1-x)/65536,iy=y>=0 ? y/65536 : -1-(-1-y)/65536;
    unsigned xx=address(ix,w,s->repeat_u),yy=address(iy,h,s->repeat_v);
    if (!linear) return data[yy*w+xx];
    unsigned fx=(unsigned)(x-ix*65536)>>8,fy=(unsigned)(y-iy*65536)>>8;
    unsigned x1=address(ix+1,w,s->repeat_u),y1=address(iy+1,h,s->repeat_v);
    return mix(mix(data[yy*w+xx],data[yy*w+x1],fx),mix(data[y1*w+xx],data[y1*w+x1],fx),fy);
}
static uint32_t combine(uint32_t src,uint32_t tex,unsigned mode,unsigned format)
{
    bool alpha_only=format==R3D_A8,rgb_only=format==R3D_RGB565 || format==R3D_L8;
    uint32_t out=0; unsigned ta=tex>>24;
    for (unsigned shift=0;shift<32;shift+=8) {
        unsigned a=(src>>shift)&255,b=(tex>>shift)&255,c;
        if (mode==R3D_REPLACE) c=(alpha_only && shift!=24) || (rgb_only && shift==24) ? a : b;
        else if (mode==R3D_DECAL) c=shift==24 || alpha_only ? a : rgb_only ? b : (a*(255-ta)+b*ta+127)/255;
        else if (mode==R3D_ADD && shift!=24 && !alpha_only) c=a+b>255 ? 255 : a+b;
        else c=(a*b+127)/255;
        out|=c<<shift;
    }
    return out;
}
static unsigned lod(uint32_t rho,unsigned levels)
{
    if (rho<=65536 || levels<=1) return 0;
    unsigned whole=0,frac=0;
    while (rho>=131072) { rho>>=1; whole++; }
    for (unsigned i=0;i<8;i++) {
        rho=(uint32_t)(((uint64_t)rho*rho)>>16); frac<<=1;
        if (rho>=131072) { rho>>=1; frac++; }
    }
    unsigned value=whole*256+frac,max=(levels-1)*256;
    return value>max ? max : value;
}
static unsigned factor(unsigned mode,unsigned alpha)
{ return mode==R3D_ONE ? 255 : mode==R3D_ZERO ? 0 : mode==R3D_SRC_ALPHA ? alpha : 255-alpha; }
static uint32_t blend(uint32_t src,uint32_t dst,const struct render3d_state *s)
{
    if (!s->blend) return src;
    unsigned sf=factor(s->source,src>>24),df=factor(s->destination,src>>24);
    uint32_t out=0;
    for (unsigned shift=0;shift<32;shift+=8) {
        unsigned c=(((src>>shift)&255)*sf+((dst>>shift)&255)*df+127)/255;
        out|=(c>255 ? 255 : c)<<shift;
    }
    return out;
}
static void triangle(const struct render3d_request *q, struct render3d_resource *const r[R3D_REFS],
                     struct render3d_job *j, const struct render3d_host *host, uint64_t start)
{
    unsigned indices[3]={j->triangle*3,j->triangle*3+1,j->triangle*3+2};
    struct render3d_vertex v[3]; memcpy(v,&q->vertices[j->triangle*3],sizeof(v));
    int64_t area=edge(&v[0],&v[1],v[2].x,v[2].y);
    if (!area) { j->triangle++; j->row=0; return; }
    if (area<0) { struct render3d_vertex tmp=v[1]; v[1]=v[2]; v[2]=tmp; unsigned ti=indices[1]; indices[1]=indices[2]; indices[2]=ti; area=-area; }
    int32_t minx=v[0].x,maxx=v[0].x,miny=v[0].y,maxy=v[0].y;
    for (unsigned i=1;i<3;i++) {
        if (v[i].x<minx) minx=v[i].x;
        if (v[i].x>maxx) maxx=v[i].x;
        if (v[i].y<miny) miny=v[i].y;
        if (v[i].y>maxy) maxy=v[i].y;
    }
    unsigned first=(unsigned)miny>>16, end=(unsigned)(maxy+65535)>>16;
    unsigned row=j->row ? j->row : first, stop=row+4;
    if (stop>end) stop=end;
    for (unsigned y=row;y<stop;y++) {
      if (y>row && host && host->now_us(host->ctx)-start>=1000) { stop=y; break; }
      for (unsigned x=(unsigned)minx>>16;x<(unsigned)(maxx+65535)>>16;x++) {
        int64_t px=((int64_t)x<<16)+32768, py=((int64_t)y<<16)+32768;
        int64_t a=edge(&v[1],&v[2],px,py), b=edge(&v[2],&v[0],px,py), c=edge(&v[0],&v[1],px,py);
        if (a<0 || b<0 || c<0 || (!a && !top_left(&v[1],&v[2])) ||
            (!b && !top_left(&v[2],&v[0])) || (!c && !top_left(&v[0],&v[1]))) continue;
        uint32_t w[3]={fraction((uint64_t)a,(uint64_t)area),fraction((uint64_t)b,(uint64_t)area),0};
        w[2]=65536-w[0]-w[1];
        uint32_t z=interpolate(v[0].z,v[1].z,v[2].z,w);
        uint16_t *depth=r[1] ? (uint16_t *)r[1]->shadow+y*r[1]->width+x : 0;
        if ((q->flags&R3D_DEPTH_TEST) && !compare(z,*depth,q->state.depth_func)) continue;
        uint32_t shaded=q->flat_color[j->triangle];
        if (!q->state.flat) {
            shaded=0;
            for (unsigned shift=0;shift<32;shift+=8)
                shaded|=interpolate((v[0].color>>shift)&255,(v[1].color>>shift)&255,(v[2].color>>shift)&255,w)<<shift;
        }
        for (unsigned stage=0;stage<2;stage++) {
            if (!(q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED))) continue;
            const struct render3d_resource *tex=r[stage ? 4 : 2];
            const struct render3d_sampler *s=&q->state.sampler[stage];
            int32_t uv[2]; int64_t weights[3]={w[0],w[1],w[2]};
            uint32_t rho=0;
            for (unsigned t=0;t<2;t++) {
                int32_t attr[3];
                for (unsigned i=0;i<3;i++) attr[i]=stage ? q->uv1[indices[i]][t] : t ? v[i].v : v[i].u;
                uv[t]=coordinate(v,attr,weights);
                if (s->filter>=R3D_NEAREST_MIP) for (unsigned axis=0;axis<2;axis++) {
                    int64_t dw[3],neighbour[3];
                    dw[0]=(axis ? (int64_t)v[2].x-v[1].x : (int64_t)v[1].y-v[2].y)*4294967296ll/area;
                    dw[1]=(axis ? (int64_t)v[0].x-v[2].x : (int64_t)v[2].y-v[0].y)*4294967296ll/area;
                    /* Tiny triangles saturate the mip choice without large products. */
                    for (unsigned i=0;i<2;i++) {
                        if (dw[i]>1048576) dw[i]=1048576;
                        if (dw[i]<-1048576) dw[i]=-1048576;
                    }
                    dw[2]=-dw[0]-dw[1];
                    for (unsigned i=0;i<3;i++) neighbour[i]=weights[i]+dw[i];
                    int64_t delta=(int64_t)coordinate(v,attr,neighbour)-uv[t];
                    uint32_t footprint=(uint32_t)(delta<0 ? -delta : delta)*(t ? tex->height : tex->width);
                    if (footprint>rho) rho=footprint;
                }
            }
            unsigned l=s->filter>=R3D_NEAREST_MIP ? lod(rho,tex->levels) : 0;
            bool linear=s->filter==R3D_BILINEAR || s->filter==R3D_BILINEAR_MIP || s->filter==R3D_TRILINEAR;
            unsigned level=s->filter==R3D_TRILINEAR ? l/256 : (l+128)/256;
            uint32_t texel=sample_level(tex,s,level,uv[0],uv[1],linear);
            if (s->filter==R3D_TRILINEAR && level+1<tex->levels)
                texel=mix(texel,sample_level(tex,s,level+1,uv[0],uv[1],true),l&255);
            shaded=combine(shaded,texel,s->combine,tex->format);
        }
        if (q->state.alpha_test && !compare(shaded>>24,q->state.alpha_ref,q->state.alpha_func)) continue;
        uint32_t *destination=(uint32_t *)r[0]->shadow+y*r[0]->width+x;
        *destination=blend(shaded,*destination,&q->state);
        if (q->flags&R3D_DEPTH_WRITE) *depth=(uint16_t)z;
      }
    }
    j->row=stop;
    if (stop==end) { j->row=0; j->triangle++; }
}
int render3d_scalar_slice(const struct render3d_request *q, struct render3d_resource *const r[R3D_REFS],
                          struct render3d_job *j, const struct render3d_host *host)
{
    uint64_t start=host ? host->now_us(host->ctx) : 0;
    if (!j->phase) {
        if (q->flags&(R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH)) {
            unsigned end=j->row+16; if (end>r[0]->height) end=r[0]->height;
            for (unsigned y=j->row;y<end;y++) {
              if (y>j->row && host && host->now_us(host->ctx)-start>=1000) { end=y; break; }
              for (unsigned x=0;x<r[0]->width;x++) {
                if (q->flags&R3D_CLEAR_COLOR) ((uint32_t *)r[0]->shadow)[y*r[0]->width+x]=q->clear_color;
                if (q->flags&R3D_CLEAR_DEPTH) ((uint16_t *)r[1]->shadow)[y*r[1]->width+x]=(uint16_t)q->clear_depth;
              }
            }
            j->row=end; if (end<r[0]->height) return 0;
        }
        j->row=0; j->phase=1;
        if ((q->flags&(R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH)) && host && host->now_us(host->ctx)-start>=1000) return 0;
    }
    if (j->phase==1) {
        if (j->triangle<q->count/3) { triangle(q,r,j,host,start); return 0; }
        j->row=0; j->phase=2;
    }
    if (q->flags&R3D_COPY) {
        unsigned end=j->row+16; if (end>r[0]->height) end=r[0]->height;
        for (unsigned y=j->row;y<end;y++) {
            if (y>j->row && host && host->now_us(host->ctx)-start>=1000) { end=y; break; }
            memcpy((uint8_t *)r[3]->shadow+y*r[3]->pitch,
                   (uint8_t *)r[0]->shadow+y*r[0]->pitch,r[0]->pitch);
        }
        j->row=end; if (end<r[0]->height) return 0;
    }
    j->phase=3; return 1;
}
