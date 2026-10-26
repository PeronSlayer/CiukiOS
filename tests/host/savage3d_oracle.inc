/* Independent exact rational GL sample envelope. Host-only long double.
 * Never calls packet sampling, scalar sampling, or boot-envelope helpers.
 * SPDX-License-Identifier: GPL-2.0-only */
static unsigned oracle_texel3d(const struct sw_texture *t,int x,int y,unsigned shift,unsigned level)
{
    const struct sw_image *im=&t->image[level];
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
static bool oracle_pixel3d(const struct gl_hw_command *g,unsigned x,unsigned y,uint32_t actual)
{
    const struct sw_vertex *v=g->vertices;
    long double xx=x*16+8,yy=(g->state.height-y)*16-8;
    long double w[3];
    for (unsigned i=0;i<3;i++) {
        const struct sw_vertex *a=v+(i+1)%3,*b=v+(i+2)%3;
        w[i]=((long double)a->x-xx)*(b->y-yy)-((long double)a->y-yy)*(b->x-xx);
    }
    long double area=w[0]+w[1]+w[2];
    if (!area) return true;
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
            if (g->state.unit[stage].mode==GL_REPLACE) { low=roundl(a); high=roundl(b); }
            else { low=roundl(low*roundl(a)/255); high=roundl(high*roundl(b)/255); }
        }
        unsigned c=actual>>shift&255;
        if (c+1<low || c>high+1) return false;
    }
    return true;
}
