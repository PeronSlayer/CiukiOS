/* Boot-only fixed-point sample envelope; no allocation or engine access.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SAVAGE3D_PRECISION_H
#define CIUKI_SAVAGE3D_PRECISION_H
static int32_t sample_floor(int32_t a)
{ return a>=0 ? a/65536 : -1-(-1-a)/65536; }
static unsigned sample_address(int a,unsigned n,bool repeat)
{ return repeat ? (unsigned)a&(n-1) : a<0 ? 0 : (unsigned)a>=n ? n-1 : (unsigned)a; }
static unsigned sample_value(const struct render3d_resource *r,const struct render3d_sampler *s,
                              int32_t x,int32_t y,unsigned shift,unsigned level)
{
    unsigned width=r->width>>level,height=r->height>>level;
    if (!width) width=1;
    if (!height) height=1;
    int ix=sample_floor(x),iy=sample_floor(y);
    unsigned fx=x-ix*65536,fy=y-iy*65536;
    const uint32_t *p=(const void *)((const uint8_t *)r->shadow+r->level_offset[level]);
    uint64_t sum=0;
    for (unsigned j=0;j<2;j++) for (unsigned i=0;i<2;i++) {
        unsigned at=sample_address(iy+(int)j,height,s->repeat_v)*width+
                    sample_address(ix+(int)i,width,s->repeat_u);
        sum+=(uint64_t)(p[at]>>shift&255)*(i ? fx : 65536-fx)*(j ? fy : 65536-fy);
    }
    return (unsigned)((sum+2147483648u)>>32);
}
/* Bilinear extrema occur at the corners of each intersected texel cell.
 * Weight rounding/truncation by N bits is covered by one weight quantum. */
static void sample_bounds(const struct render3d_resource *r,const struct render3d_sampler *s,
                          int32_t u,int32_t v,unsigned shift,unsigned delta,unsigned bits,
                          unsigned level,unsigned *lo,unsigned *hi)
{
    int32_t radius=(int32_t)delta+(65536>>bits);
    unsigned width=r->width>>level,height=r->height>>level;
    if (!width) width=1;
    if (!height) height=1;
    int32_t x=u*(int32_t)width-32768,y=v*(int32_t)height-32768;
    int32_t xs[4]={x-radius,x+radius},ys[4]={y-radius,y+radius};
    unsigned nx=2,ny=2;
    int32_t boundary=(sample_floor(xs[0])+1)*65536;
    if (boundary<xs[1]) xs[nx++]=boundary;
    boundary=(sample_floor(ys[0])+1)*65536;
    if (boundary<ys[1]) ys[ny++]=boundary;
    *lo=255; *hi=0;
    for (unsigned j=0;j<ny;j++) for (unsigned i=0;i<nx;i++) {
        unsigned c=sample_value(r,s,xs[i],ys[j],shift,level);
        if (c<*lo) *lo=c;
        if (c>*hi) *hi=c;
    }
}
static int32_t precision_coordinate(const struct render3d_request *q,unsigned first,
    const int64_t w[3],unsigned stage,unsigned axis)
{
    int64_t numer=0,denom=0;
    for (unsigned i=0;i<3;i++) {
        const struct render3d_vertex *v=&q->vertices[first+i];
        int32_t a=stage ? q->uv1[first+i][axis] : axis ? v->v : v->u;
        numer+=(int64_t)a*v->rhw*w[i]; denom+=(int64_t)v->rhw*w[i];
    }
    return (int32_t)(numer/denom);
}
static unsigned precision_lod(uint32_t rho,unsigned levels)
{
    if (rho<=65536 || levels<=1) return 0;
    unsigned whole=0,frac=0;
    while (rho>=131072) { rho>>=1; whole++; }
    for (unsigned i=0;i<8;i++) {
        rho=(uint32_t)(((uint64_t)rho*rho)>>16); frac*=2;
        if (rho>=131072) { rho>>=1; frac++; }
    }
    unsigned l=whole*256+frac,max=(levels-1)*256;
    return l>max ? max : l;
}
static void precision_mip_bounds(const struct render3d_resource *r,const struct render3d_sampler *s,
    int32_t u,int32_t v,unsigned shift,unsigned delta,unsigned bits,unsigned lod,unsigned *lo,unsigned *hi)
{
    if (s->filter==R3D_BILINEAR) { sample_bounds(r,s,u,v,shift,delta,bits,0,lo,hi); return; }
    unsigned begin=lod>CIUKI_R3D_LOD_DELTA_Q8 ? lod-CIUKI_R3D_LOD_DELTA_Q8 : 0;
    unsigned end=lod+CIUKI_R3D_LOD_DELTA_Q8,max=(r->levels-1)*256;
    if (end>max) end=max;
    *lo=255; *hi=0;
    unsigned points[3]={begin,end,((begin/128)+1)*128};
    for (unsigned i=0;i<3;i++) {
        unsigned l=points[i];
        if (l>end) continue;
        unsigned level=s->filter==R3D_TRILINEAR ? l/256 : (l+128)/256;
        unsigned a,b; sample_bounds(r,s,u,v,shift,delta,bits,level,&a,&b);
        if (s->filter==R3D_TRILINEAR && level+1<r->levels) {
            unsigned c,d; sample_bounds(r,s,u,v,shift,delta,bits,level+1,&c,&d);
            a=(a*(256-l%256)+c*(l%256)+128)/256;
            b=(b*(256-l%256)+d*(l%256)+128)/256;
        }
        if (a<*lo) *lo=a;
        if (b>*hi) *hi=b;
    }
}
static bool precision_pixel(const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS],
                             unsigned x,unsigned y,uint32_t actual,unsigned delta,unsigned bits)
{
    /* Calibration geometry has integer-pixel corners and shared W. Products
     * are <2^61 at 1024x1024, |UV|<=16, W<=2; no wide-runtime helper. */
    unsigned width=r[0]->width,height=r[0]->height,first=0;
    int64_t w[3],xx=2*x+1,yy=2*y+1,area=(int64_t)4*width*height;
    if (xx*height+yy*width<=2*(int64_t)width*height) {
        w[1]=xx*2*height; w[2]=yy*2*width; w[0]=area-w[1]-w[2];
    } else {
        first=3; w[0]=(2*(int64_t)height-yy)*2*width;
        w[2]=(2*(int64_t)width-xx)*2*height; w[1]=area-w[0]-w[2];
    }
    int32_t uv[2][2]; unsigned lod[2]={0};
    for (unsigned stage=0;stage<2;stage++) {
        for (unsigned axis=0;axis<2;axis++) uv[stage][axis]=precision_coordinate(q,first,w,stage,axis);
        const struct render3d_resource *tex=r[stage ? 4 : 2];
        if (q->state.sampler[stage].filter!=R3D_BILINEAR) {
            uint32_t rho=0;
            for (unsigned axis=0;axis<2;axis++) {
                int64_t neighbour[3];
                for (unsigned i=0;i<3;i++) {
                    const struct render3d_vertex *a=&q->vertices[first+(i+1)%3],*b=&q->vertices[first+(i+2)%3];
                    int64_t grad=axis ? (b->x-a->x)/65536 : (a->y-b->y)/65536;
                    neighbour[i]=w[i]+4*grad;
                }
                for (unsigned k=0;k<2;k++) {
                    int32_t diff=precision_coordinate(q,first,neighbour,stage,k)-uv[stage][k];
                    uint32_t footprint=(uint32_t)(diff<0 ? -diff : diff)*(k ? tex->height : tex->width);
                    if (footprint>rho) rho=footprint;
                }
            }
            lod[stage]=precision_lod(rho,tex->levels);
        }
    }
    for (unsigned shift=0;shift<32;shift+=8) {
        unsigned lo=255,hi=255;
        for (unsigned stage=0;stage<2;stage++) {
            unsigned a,b;
            precision_mip_bounds(r[stage ? 4 : 2],&q->state.sampler[stage],uv[stage][0],uv[stage][1],
                          shift,delta+64,bits,lod[stage],&a,&b); /* Q16 recovery <1/1024 texel */
            lo=(lo*a+127)/255; hi=(hi*b+127)/255;
        }
        unsigned c=actual>>shift&255;
        if (c+1<lo || c>hi+1) return false;
    }
    return true;
}
#endif
