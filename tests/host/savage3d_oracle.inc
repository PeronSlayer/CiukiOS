/* Independent exact rational GL sample envelope. Host-only long double.
 * Never calls packet sampling, scalar sampling, or boot-envelope helpers.
 * SPDX-License-Identifier: GPL-2.0-only */
static unsigned oracle_texel3d(const struct sw_texture *t,int x,int y,unsigned shift,unsigned level)
{
    const struct sw_image *im=&t->image[level];
    if (shift==24 && gl_hw_texture_rgb(im->internal)) return 255;
    if ((t->wrap_s==GL_CLAMP && (x<0 || (unsigned)x>=im->w)) ||
        (t->wrap_t==GL_CLAMP && (y<0 || (unsigned)y>=im->h))) return t->border_color>>shift&255;
    if (t->wrap_s==GL_REPEAT) x=(unsigned)x&(im->w-1);
    else x=x<0 ? 0 : (unsigned)x>=im->w ? (int)im->w-1 : x;
    if (t->wrap_t==GL_REPEAT) y=(unsigned)y&(im->h-1);
    else y=y<0 ? 0 : (unsigned)y>=im->h ? (int)im->h-1 : y;
    return im->pixels[y*im->w+x]>>shift&255;
}
static void oracle_sample3d(const struct sw_texture *t,long double u,long double v,unsigned shift,unsigned level,
                            long double *low,long double *high)
{
    /* The union of N>=8-bit rounded/truncated fractions is contained in
     * this box. Split at all integer boundaries so extrema cannot hide. */
    long double radius=(long double)CIUKI_R3D_SAMPLE_DELTA_Q16/65536+1.0L/256;
    long double x=u*t->image[level].w-.5L,y=v*t->image[level].h-.5L;
    long double xx[4]={x-radius,x+radius},yy[4]={y-radius,y+radius};
    unsigned nx=2,ny=2;
    if (floorl(xx[0])+1<xx[1]) xx[nx++]=floorl(xx[0])+1;
    if (floorl(yy[0])+1<yy[1]) yy[ny++]=floorl(yy[0])+1;
    *low=255; *high=0;
    for (unsigned j=0;j<ny;j++) for (unsigned i=0;i<nx;i++) {
        int ix=floorl(xx[i]),iy=floorl(yy[j]);
        long double fx=xx[i]-ix,fy=yy[j]-iy;
        long double c=(oracle_texel3d(t,ix,iy,shift,level)*(1-fx)+oracle_texel3d(t,ix+1,iy,shift,level)*fx)*(1-fy)+
            (oracle_texel3d(t,ix,iy+1,shift,level)*(1-fx)+oracle_texel3d(t,ix+1,iy+1,shift,level)*fx)*fy;
        if (c<*low) *low=c;
        if (c>*high) *high=c;
    }
}
static void oracle_mip3d(const struct sw_texture *t,long double u,long double v,long double lambda,
    unsigned shift,long double *low,long double *high)
{
    if (t->min==GL_LINEAR || lambda<=0) { oracle_sample3d(t,u,v,shift,0,low,high); return; }
    unsigned max=0;
    while (max+1<SW_LEVELS && t->image[max+1].pixels) max++;
    if (lambda>max) lambda=max;
    long double begin=fmaxl(0,lambda-1.0L/16),end=fminl(max,lambda+1.0L/16);
    *low=255; *high=0;
    /* Piecewise linear LOD extrema: endpoints and the one possible
     * integer/half-integer boundary. No random neighbourhood sampling. */
    for (unsigned i=0;i<3;i++) {
        long double l=i==0 ? begin : i==1 ? end : (floorl(begin*2)+1)/2;
        if (l>end) continue;
        unsigned level=t->min==GL_LINEAR_MIPMAP_LINEAR ? floorl(l) : floorl(l+.5L);
        if (level>max) level=max;
        long double a,b; oracle_sample3d(t,u,v,shift,level,&a,&b);
        if (t->min==GL_LINEAR_MIPMAP_LINEAR && level<max) {
            long double c,d; oracle_sample3d(t,u,v,shift,level+1,&c,&d);
            long double f=l-level;
            a=a*(1-f)+c*f; b=b*(1-f)+d*f;
        }
        if (a<*low) *low=a;
        if (b>*high) *high=b;
    }
}
static void oracle_source3d(const struct gl_hw_command *g,unsigned x,unsigned y,long double lower[4],long double upper[4])
{
    const struct sw_vertex *v=g->vertices;
    long double xx=x*16+8,yy=(g->state.height-y)*16-8;
    long double w[3];
    for (unsigned i=0;i<3;i++) {
        const struct sw_vertex *a=v+(i+1)%3,*b=v+(i+2)%3;
        w[i]=((long double)a->x-xx)*(b->y-yy)-((long double)a->y-yy)*(b->x-xx);
    }
    long double area=w[0]+w[1]+w[2];
    if (!area) { for (unsigned i=0;i<4;i++) lower[i]=upper[i]=0; return; }
    for (unsigned shift=0;shift<32;shift+=8) {
        unsigned component=shift==24 ? 3 : 2-shift/8;
        long double low=0;
        for (unsigned i=0;i<3;i++) low+=w[i]*v[i].color[component]/area;
        long double high=low;
        for (unsigned stage=0;stage<2;stage++) if (g->state.unit[stage].enabled) {
            long double q=0,u=0,t=0;
            for (unsigned i=0;i<3;i++) { q+=w[i]*v[i].qw[stage]; u+=w[i]*v[i].uv[stage][0]; t+=w[i]*v[i].uv[stage][1]; }
            const struct sw_texture *texture=g->state.unit[stage].texture;
            long double rho=0;
            if (texture->min!=GL_LINEAR) for (unsigned axis=0;axis<2;axis++) {
                long double dq=0,du=0,dt=0;
                for (unsigned i=0;i<3;i++) {
                    const struct sw_vertex *a=v+(i+1)%3,*b=v+(i+2)%3;
                    long double dw=(axis ? b->x-a->x : a->y-b->y)*16;
                    dq+=dw*v[i].qw[stage]; du+=dw*v[i].uv[stage][0]; dt+=dw*v[i].uv[stage][1];
                }
                long double ru=fabsl((du*q-u*dq)*16/(q*q))*texture->image[0].w;
                long double rt=fabsl((dt*q-t*dq)*16/(q*q))*texture->image[0].h;
                rho=fmaxl(rho,fmaxl(ru,rt));
            }
            long double a,b;
            oracle_mip3d(texture,u*16/q,t*16/q,rho>1 ? log2l(rho) : 0,shift,&a,&b);
            if (shift==24 && gl_hw_texture_rgb(texture->image[0].internal)) continue;
            if (g->state.unit[stage].mode==GL_REPLACE) { low=roundl(a); high=roundl(b); }
            else if (g->state.unit[stage].mode==GL_ADD && shift!=24) {
                low=fminl(255,low+roundl(a)); high=fminl(255,high+roundl(b));
            } else if (g->state.unit[stage].mode==GL_DECAL) {
                if (shift!=24) {
                    long double al,ah;
                    oracle_mip3d(texture,u*16/q,t*16/q,rho>1 ? log2l(rho) : 0,24,&al,&ah);
                    long double first=(low*(255-al)+roundl(a)*al)/255;
                    long double second=(low*(255-ah)+roundl(a)*ah)/255;
                    long double third=(high*(255-al)+roundl(b)*al)/255;
                    long double fourth=(high*(255-ah)+roundl(b)*ah)/255;
                    low=roundl(fminl(first,second)); high=roundl(fmaxl(third,fourth));
                }
            } else { low=roundl(low*roundl(a)/255); high=roundl(high*roundl(b)/255); }
        }
        lower[shift/8]=low; upper[shift/8]=high;
    }
}

/* The source and alpha may be correlated. RGB extrema of the blend
 * polynomial are at rectangle corners; alpha has one possible quadratic
 * extremum. Evaluate it too, rather than assuming monotonic alpha blending. */
static long double oracle_factor3d(int f,long double c,long double a,unsigned d,unsigned da) {
    (void)da; /* WGL XRGB has no destination-alpha attachment. */
    switch (f) {
    case GL_ZERO: return 0; case GL_ONE: return 255;
    case GL_SRC_ALPHA: return a; case GL_ONE_MINUS_SRC_ALPHA: return 255-a;
    case GL_SRC_COLOR: return c; case GL_ONE_MINUS_SRC_COLOR: return 255-c;
    case GL_DST_COLOR: return d; case GL_ONE_MINUS_DST_COLOR: return 255-d;
    case GL_DST_ALPHA: return 255; case GL_ONE_MINUS_DST_ALPHA: return 0;
    default: return -1000000;
    }
}
static void oracle_blend3d(const struct sw_state *s,long double lo,long double hi,long double al,long double ah,
                           unsigned d,unsigned da,bool alpha,long double *low,long double *high) {
    *low=255; *high=0;
    long double points[3]={al,ah,al};
    if (alpha) {
        long double sa=s->sfactor==GL_SRC_ALPHA ? 1 : s->sfactor==GL_ONE_MINUS_SRC_ALPHA ? -1 : 0;
        long double sc=oracle_factor3d(s->sfactor,0,0,d,da);
        long double ds=(s->dfactor==GL_SRC_ALPHA || s->dfactor==GL_SRC_COLOR) ? 1 :
            (s->dfactor==GL_ONE_MINUS_SRC_ALPHA || s->dfactor==GL_ONE_MINUS_SRC_COLOR) ? -1 : 0;
        if (sa) points[2]=fmaxl(al,fminl(ah,-(sc+d*ds)/(2*sa)));
    }
    for (unsigned i=0;i<(alpha ? 3u : 2u);i++) for (unsigned j=0;j<2;j++) {
        long double a=points[i],c=alpha ? a : j ? hi : lo;
        long double value=(c*oracle_factor3d(s->sfactor,c,a,d,da)+d*oracle_factor3d(s->dfactor,c,a,d,da))/255;
        value=fmaxl(0,fminl(255,roundl(value)));
        if (value<*low) *low=value;
        if (value>*high) *high=value;
    }
}
static bool oracle_colour3d(const struct gl_hw_command *g,unsigned x,unsigned y,uint32_t actual,uint32_t destination) {
    long double lo[4],hi[4]; oracle_source3d(g,x,y,lo,hi);
    for (unsigned k=0;k<4;k++) {
        long double a=lo[k],b=hi[k];
        if (g->state.blend) oracle_blend3d(&g->state,a,b,lo[3],hi[3],destination>>(k*8)&255,destination>>24,k==3,&a,&b);
        unsigned c=actual>>(k*8)&255;
        if (c+1<a || c>b+1) return false;
    }
    return true;
}
static bool oracle_pixel3d(const struct gl_hw_command *g,unsigned x,unsigned y,uint32_t actual) {
    return oracle_colour3d(g,x,y,actual,g->clear_color);
}
static bool oracle_compare3d(unsigned a,unsigned b,unsigned f) {
    switch (f) {
    case GL_NEVER: return false; case GL_ALWAYS: return true;
    case GL_LESS: return a<b; case GL_LEQUAL: return a<=b;
    case GL_EQUAL: return a==b; case GL_NOTEQUAL: return a!=b;
    case GL_GREATER: return a>b; default: return a>=b;
    }
}
/* Exact top-left coverage, followed by alpha/depth outcome alternatives.
 * A failed test must preserve both exact destination attachments. */
static bool oracle_fragment3d(const struct gl_hw_command *g,unsigned x,unsigned y,uint32_t actual,unsigned depth,
                              uint32_t before,unsigned before_depth) {
    int64_t w[3];
    int64_t area=0;
    const struct sw_vertex *v=g->vertices;
    for (unsigned i=0;i<3;i++) {
        const struct sw_vertex *a=v+(i+1)%3,*b=v+(i+2)%3;
        int64_t ay=(int)g->state.height*16-a->y,by=(int)g->state.height*16-b->y;
        w[i]=((int64_t)b->x-a->x)*(y*16+8-ay)-(by-ay)*(x*16+8-(int64_t)a->x);
        area+=w[i];
    }
    bool covered=area!=0;
    for (unsigned i=0;i<3;i++) {
        const struct sw_vertex *a=v+(i+1)%3,*b=v+(i+2)%3;
        if (area<0) { const struct sw_vertex *t=a; a=b; b=t; }
        bool top=b->y>a->y || (b->y==a->y && b->x>a->x);
        int64_t weight=area<0 ? -w[i] : w[i];
        if (weight<0 || (!weight && !top)) covered=false;
    }
    bool unchanged=actual==before && depth==before_depth;
    if (!covered) return unchanged;
    long double z=g->state.offset;
    for (unsigned i=0;i<3;i++) z+=(long double)w[i]*v[i].z/area;
    unsigned expected_z=(unsigned)fmaxl(0,fminl(65535,roundl(z)));
    if (g->state.depth_test && !oracle_compare3d(expected_z,before_depth,g->state.depth_func)) return unchanged;
    long double low[4],high[4]; oracle_source3d(g,x,y,low,high);
    bool pass=!g->state.alpha_test,fail=false;
    if (g->state.alpha_test) for (unsigned a=(unsigned)low[3];a<=(unsigned)high[3];a++) {
        bool p=oracle_compare3d(a,g->state.alpha_ref,g->state.alpha_func);
        pass|=p; fail|=!p;
    }
    if (fail && unchanged) return true;
    unsigned wanted=g->state.depth_test && g->state.depth_write ? expected_z : before_depth;
    return pass && abs((int)depth-(int)wanted)<=1 && oracle_colour3d(g,x,y,actual,before);
}
