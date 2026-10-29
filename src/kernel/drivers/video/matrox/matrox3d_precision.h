/* Matrox-only fixed-point sample envelope derived from the f7-25 baseline; no
 * allocation or engine access. The caller includes render3d.h first.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MATROX3D_PRECISION_H
#define CIUKI_MATROX3D_PRECISION_H
struct precision_arithmetic { unsigned bits; bool rounded,separable; };
static int32_t sample_floor(int32_t a)
{ return a>=0 ? a/65536 : -1-(-1-a)/65536; }
static unsigned sample_address(int a,unsigned n,bool repeat)
{ return repeat ? (unsigned)a&(n-1) : a<0 ? 0 : (unsigned)a>=n ? n-1 : (unsigned)a; }
static unsigned sample_value(const struct render3d_resource *r,const struct render3d_sampler *s,
                              int32_t x,int32_t y,unsigned shift,unsigned level,const struct precision_arithmetic *arithmetic)
{
    unsigned width=r->width>>level,height=r->height>>level;
    if (!width) width=1;
    if (!height) height=1;
    int ix=sample_floor(x),iy=sample_floor(y);
    unsigned fx=x-ix*65536,fy=y-iy*65536;
    if (arithmetic->bits) {
        unsigned step=65536>>arithmetic->bits,bias=arithmetic->rounded ? step/2 : 0;
        fx=((fx+bias)/step)*step; fy=((fy+bias)/step)*step;
    }
    const uint32_t *p=(const void *)((const uint8_t *)r->shadow+r->level_offset[level]);
    uint64_t sum=0; unsigned row[2]={0};
    for (unsigned j=0;j<2;j++) for (unsigned i=0;i<2;i++) {
        unsigned at=sample_address(iy+(int)j,height,s->repeat_v)*width+
                    sample_address(ix+(int)i,width,s->repeat_u);
        unsigned c=p[at]>>shift&255;
        row[j]+=c*(i ? fx : 65536-fx);
        sum+=(uint64_t)c*(i ? fx : 65536-fx)*(j ? fy : 65536-fy);
    }
    /* Q16 colour keeps bilinear accumulation until after trilinear mixing.
     * The separable scalar rule rounds horizontal rows and each mip first. */
    if (arithmetic->separable) return ((((row[0]+32768)/65536)*(65536-fy)+((row[1]+32768)/65536)*fy+32768)/65536)*65536;
    return (unsigned)((sum+32768)>>16);
}
/* Bilinear extrema occur at the corners of each intersected texel cell.
 * Weight rounding/truncation by N bits is covered by one weight quantum. */
static void sample_bounds(const struct render3d_resource *r,const struct render3d_sampler *s,
                          int32_t u,int32_t v,unsigned shift,const int32_t box[6][2],unsigned stage,
                          const struct precision_arithmetic *arithmetic,unsigned level,unsigned *lo,unsigned *hi)
{
    unsigned width=r->width>>level,height=r->height>>level;
    if (!width) width=1;
    if (!height) height=1;
    /* Coordinates retain Q24 until conversion into texel Q16. */
    int32_t x=(int32_t)((int64_t)u*width/256)-32768;
    int32_t y=(int32_t)((int64_t)v*height/256)-32768;
    /* A common normalized UV feeds all mip levels; only its per-level
     * filter fractions are quantized. Offsets are base-level texels. */
    int32_t xs[4]={x+(int64_t)box[stage*3][0]*width/r->width,x+(int64_t)box[stage*3][1]*width/r->width};
    int32_t ys[4]={y+(int64_t)box[stage*3+1][0]*height/r->height,y+(int64_t)box[stage*3+1][1]*height/r->height};
    unsigned nx=xs[0]==xs[1] ? 1 : 2,ny=ys[0]==ys[1] ? 1 : 2;
    int32_t boundary=(sample_floor(xs[0])+1)*65536;
    if (boundary<xs[1]) xs[nx++]=boundary;
    boundary=(sample_floor(ys[0])+1)*65536;
    if (boundary<ys[1]) ys[ny++]=boundary;
    *lo=255*65536; *hi=0;
    for (unsigned j=0;j<ny;j++) for (unsigned i=0;i<nx;i++) {
        unsigned c=sample_value(r,s,xs[i],ys[j],shift,level,arithmetic);
        if (c<*lo) *lo=c;
        if (c>*hi) *hi=c;
    }
    if (arithmetic->separable && (xs[0]!=xs[1] || ys[0]!=ys[1])) {
        *lo=*lo<2*65536 ? 0 : *lo-2*65536;
        *hi=*hi>253*65536 ? 255*65536 : *hi+2*65536;
    }
}
static int32_t precision_coordinate(const struct render3d_request *q,unsigned first,
    const int64_t w[3],unsigned stage,unsigned axis)
{
    int64_t numer=0,tail=0,denom=0;
    for (unsigned i=0;i<3;i++) {
        const struct render3d_vertex *v=&q->vertices[first+i];
        int32_t a=stage ? q->uv1[first+i][axis] : axis ? v->v : v->u;
        int64_t product=(int64_t)a*v->rhw;
        numer+=(product/65536)*w[i]; tail+=(product%65536)*w[i];
        denom+=(int64_t)v->rhw*w[i];
    }
    int64_t part=(numer%denom)*65536+tail;
    int64_t whole=(numer/denom)*65536+part/denom;
    return (int32_t)(whole*256+(part%denom)*256/denom);
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
    int32_t u,int32_t v,unsigned shift,const int32_t box[6][2],unsigned stage,const struct precision_arithmetic *arithmetic,unsigned lod,unsigned *lo,unsigned *hi)
{
    if (s->filter==R3D_BILINEAR) {
        sample_bounds(r,s,u,v,shift,box,stage,arithmetic,0,lo,hi);
        *lo=(*lo+32768)/65536; *hi=(*hi+32768)/65536; return;
    }
    int begin=(int)lod+box[stage*3+2][0],end=(int)lod+box[stage*3+2][1];
    unsigned max=(r->levels-1)*256;
    if (begin<0) begin=0;
    if (begin>(int)max) begin=(int)max;
    if (end<0) end=0;
    if (end>(int)max) end=(int)max;
    *lo=255; *hi=0;
    unsigned points[3]={begin,end,((begin/128)+1)*128};
    for (unsigned i=0;i<3;i++) {
        unsigned l=points[i];
        if (l>(unsigned)end) continue;
        unsigned level=s->filter==R3D_TRILINEAR ? l/256 : (l+128)/256;
        unsigned a,b; sample_bounds(r,s,u,v,shift,box,stage,arithmetic,level,&a,&b);
        if (s->filter==R3D_TRILINEAR && level+1<r->levels) {
            unsigned c,d; sample_bounds(r,s,u,v,shift,box,stage,arithmetic,level+1,&c,&d);
            a=(a*(256-l%256)+c*(l%256)+128)/256;
            b=(b*(256-l%256)+d*(l%256)+128)/256;
        }
        a=(a+32768)/65536; b=(b+32768)/65536;
        if (a<*lo) *lo=a;
        if (b>*hi) *hi=b;
    }
}
static bool precision_compare(unsigned a,unsigned b,unsigned f)
{
    switch (f) {
    case R3D_NEVER: return false; case R3D_ALWAYS: return true;
    case R3D_LESS: return a<b; case R3D_LEQUAL: return a<=b;
    case R3D_EQUAL: return a==b; case R3D_NOTEQUAL: return a!=b;
    case R3D_GREATER: return a>b; default: return a>=b;
    }
}
static unsigned precision_factor(unsigned f,unsigned c,unsigned a,unsigned d,unsigned da)
{
    switch (f) {
    case R3D_ONE: return 255; case R3D_ZERO: return 0;
    case R3D_SRC_ALPHA: return a; case R3D_INV_SRC_ALPHA: return 255-a;
    case R3D_DST_COLOR: return d; case R3D_INV_DST_COLOR: return 255-d;
    case R3D_SRC_COLOR: return c; case R3D_INV_SRC_COLOR: return 255-c;
    case R3D_DST_ALPHA: return da; default: return 255-da;
    }
}
static void precision_blend(const struct render3d_state *s,unsigned lo,unsigned hi,unsigned al,unsigned ah,
                             unsigned d,unsigned da,unsigned *low,unsigned *high)
{
    /* Interval arithmetic is conservative even for alpha*alpha and inverse
     * factors: every possible sampled source remains represented. */
    unsigned sl=255,sh=0,dl=255,dh=0;
    for (unsigned i=0;i<4;i++) {
        unsigned c=i&1 ? hi : lo,a=i&2 ? ah : al;
        unsigned sf=precision_factor(s->source,c,a,d,da),df=precision_factor(s->destination,c,a,d,da);
        if (sf<sl) sl=sf;
        if (sf>sh) sh=sf;
        if (df<dl) dl=df;
        if (df>dh) dh=df;
    }
    *low=(lo*sl+d*dl+127)/255; *high=(hi*sh+d*dh+127)/255;
    if (*low>255) *low=255;
    if (*high>255) *high=255;
}
struct precision_position { int32_t uv[2][2]; unsigned lod[2]; struct precision_arithmetic arithmetic; };
static void precision_source(const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS],
    const struct precision_position *p,const int32_t box[6][2],unsigned lower[4],unsigned upper[4])
{
    for (unsigned shift=0;shift<32;shift+=8) {
        unsigned lo=255,hi=255;
        for (unsigned stage=0;stage<2;stage++) {
            if (!(q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED))) continue;
            unsigned a,b;
            precision_mip_bounds(r[stage ? 4 : 2],&q->state.sampler[stage],p->uv[stage][0],p->uv[stage][1],
                                  shift,box,stage,&p->arithmetic,p->lod[stage],&a,&b);
            lo=(lo*a+127)/255; hi=(hi*b+127)/255;
        }
        lower[shift/8]=lo; upper[shift/8]=hi;
    }
}
static bool precision_outcome(const struct render3d_request *q,const unsigned lower[4],const unsigned upper[4],
                              uint32_t actual,bool rejected)
{
    bool possible=!q->state.alpha_test;
    if (q->state.alpha_test) for (unsigned a=lower[3];a<=upper[3];a++)
        possible|=precision_compare(a,q->state.alpha_ref,q->state.alpha_func)!=rejected;
    if (!possible) return false;
    if (rejected) return q->state.alpha_test;
    for (unsigned shift=0;shift<32;shift+=8) {
        unsigned lo=lower[shift/8],hi=upper[shift/8];
        if (q->state.blend) precision_blend(&q->state,lo,hi,lower[3],upper[3],
            q->clear_color>>shift&255,q->clear_color>>24,&lo,&hi);
        unsigned c=actual>>shift&255;
        if (c+1<lo || c>hi+1) return false;
    }
    return true;
}
/* Try exact filter-grid witnesses before subdivision. Half-channel rounding
 * planes can have zero measure and must not depend on hitting a random point. */
static bool precision_fit(const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS],
    const struct precision_position *p,const int32_t box[6][2],uint32_t actual,bool rejected,unsigned phase)
{
    int32_t point[6][2]={{0}}; unsigned lower[4],upper[4];
    precision_source(q,r,p,point,lower,upper);
    if (precision_outcome(q,lower,upper,actual,rejected)) return true;
    if (!phase && (box[2][1] || box[5][1])) {
        for (int step=1;step<=14;step++) for (unsigned mask=1;mask<4;mask++)
        for (int sign=-1;sign<=1;sign+=2) {
            for (unsigned stage=0;stage<2;stage++) {
                int a=(mask&(1u<<stage)) ? sign*step : 0;
                if (a<box[stage*3+2][0]) a=box[stage*3+2][0];
                if (a>box[stage*3+2][1]) a=box[stage*3+2][1];
                point[stage*3+2][0]=point[stage*3+2][1]=a;
            }
            precision_source(q,r,p,point,lower,upper);
            if (precision_outcome(q,lower,upper,actual,rejected)) return true;
        }
    }
    precision_source(q,r,p,box,lower,upper);
    if (!precision_outcome(q,lower,upper,actual,rejected)) return false;
    if (!phase) return false;
    for (unsigned scale=0;scale<4;scale++) for (unsigned code=1;code<81;code++) {
        unsigned c=code;
        for (unsigned stage=0;stage<2;stage++) for (unsigned axis=0;axis<2;axis++) {
            unsigned digit=c%3; c/=3;
            int32_t offset=digit==1 ? -(128<<scale) : digit==2 ? 128<<scale : 0;
            point[stage*3+axis][0]=point[stage*3+axis][1]=offset;
        }
        precision_source(q,r,p,point,lower,upper);
        if (precision_outcome(q,lower,upper,actual,rejected)) return true;
    }
    if (phase==1) return false;
    uint32_t seed=0x12345678;
    for (unsigned trial=0;trial<512;trial++) {
        for (unsigned i=0;i<6;i++) {
            seed=seed*1664525u+1013904223u;
            int32_t radius=box[i][1];
            if (trial<256 && radius>(i%3==2 ? 4 : 512)) radius=i%3==2 ? 4 : 512;
            int32_t offset=(int32_t)((int64_t)(seed>>8)*(2*radius)/16777215)-radius;
            point[i][0]=point[i][1]=offset;
        }
        precision_source(q,r,p,point,lower,upper);
        if (precision_outcome(q,lower,upper,actual,rejected)) return true;
    }
    return false;
}
/* Bounds only prune. A positive result requires one common (u,v,LOD) per
 * stage for all RGBA channels, the alpha decision and the blended colour. */
static bool precision_search(const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS],
    const struct precision_position *p,int32_t box[6][2],uint32_t actual,bool rejected,unsigned *budget,unsigned depth)
{
    if (!*budget) return false;
    --*budget;
    int32_t point[6][2]; unsigned lower[4],upper[4];
    for (unsigned i=0;i<6;i++) point[i][0]=point[i][1]=box[i][0]+(box[i][1]-box[i][0])/2;
    precision_source(q,r,p,point,lower,upper);
    if (precision_outcome(q,lower,upper,actual,rejected)) return true;
    precision_source(q,r,p,box,lower,upper);
    if (!precision_outcome(q,lower,upper,actual,rejected) || depth==42) return false;
    unsigned axis=depth%6;
    for (unsigned i=0;i<6 && box[axis][0]==box[axis][1];i++) axis=(axis+1)%6;
    int32_t a=box[axis][0],b=box[axis][1],mid=a+(b-a)/2;
    if (a==b) return false;
    box[axis][1]=mid;
    bool found=precision_search(q,r,p,box,actual,rejected,budget,depth+1);
    box[axis][0]=mid+1; box[axis][1]=b;
    if (!found) found=precision_search(q,r,p,box,actual,rejected,budget,depth+1);
    box[axis][0]=a;
    return found;
}
static bool precision_pixel(const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS],
                             unsigned x,unsigned y,uint32_t actual,unsigned z,unsigned delta,unsigned bits)
{
    if (bits<8 || bits>16 || delta<2) return false;
    /* Calibration geometry has integer-pixel corners and shared W. Products
     * are split before weighting at 1024x1024, |UV|<=16, W<=256. */
    unsigned width=r[0]->width,height=r[0]->height,first=0;
    int64_t w[3],xx=2*x+1,yy=2*y+1,area=(int64_t)4*width*height;
    if (xx*height+yy*width<=2*(int64_t)width*height) {
        w[1]=xx*2*height; w[2]=yy*2*width; w[0]=area-w[1]-w[2];
    } else {
        first=3; w[0]=(2*(int64_t)height-yy)*2*width;
        w[2]=(2*(int64_t)width-xx)*2*height; w[1]=area-w[0]-w[2];
    }
    struct precision_position p={0};
    for (unsigned stage=0;stage<2;stage++) {
        if (!(q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED))) continue;
        for (unsigned axis=0;axis<2;axis++) p.uv[stage][axis]=precision_coordinate(q,first,w,stage,axis);
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
                    int32_t diff=precision_coordinate(q,first,neighbour,stage,k)-p.uv[stage][k];
                    uint32_t footprint=(uint32_t)(((uint64_t)(diff<0 ? -diff : diff)*
                        (k ? tex->height : tex->width)+255)/256);
                    if (footprint>rho) rho=footprint;
                }
            }
            p.lod[stage]=precision_lod(rho,tex->levels);
        }
    }
    int32_t box[6][2];
    for (unsigned stage=0;stage<2;stage++) for (unsigned axis=0;axis<3;axis++) {
        /* Reserve two Q16/Q8 steps for rational recovery and log error. */
        int32_t radius=axis==2 ? CIUKI_R3D_LOD_DELTA_Q8-2 : delta-2;
        if (axis==2 && q->state.sampler[stage].filter==R3D_BILINEAR) radius=0;
        box[stage*3+axis][0]=-radius; box[stage*3+axis][1]=radius;
    }
    for (unsigned phase=0;phase<3;phase++) for (unsigned mode=0;mode<10;mode++) {
        p.arithmetic=(struct precision_arithmetic){.bits=mode<4 ? bits : mode<8 ? (bits<=12 ? bits+4 : 16) : 0,
            .rounded=(mode&2)!=0,.separable=(mode&1)!=0};
        unsigned budget=1024;
        /* Each phase clears first; a rejected fragment preserves both attachments. */
        if (actual==q->clear_color && z==q->clear_depth && q->state.alpha_test &&
            (precision_fit(q,r,&p,box,actual,true,phase) || (phase==2 && precision_search(q,r,&p,box,actual,true,&budget,0)))) return true;
        if (z+1<q->vertices[0].z || z>q->vertices[0].z+1) continue;
        budget=1024;
        if (precision_fit(q,r,&p,box,actual,false,phase) || (phase==2 && precision_search(q,r,&p,box,actual,false,&budget,0))) return true;
    }
    return false;
}
#endif
