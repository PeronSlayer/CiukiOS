/* Independent exact rational GL sample envelope. Host-only long double.
 * Never calls packet sampling, scalar sampling, or boot-envelope helpers.
 * SPDX-License-Identifier: GPL-2.0-only */
struct oracle_arithmetic3d { unsigned bits; bool rounded,separable; };
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
                            const long double box[6][2],unsigned stage,const struct oracle_arithmetic3d *arithmetic,long double *low,long double *high)
{
    /* Split at integer boundaries; a single arithmetic rule quantizes the
     * common sample's fractions for all four channels. */
    long double x=u*t->image[level].w-.5L,y=v*t->image[level].h-.5L;
    /* One normalized UV displacement feeds BOTH mip levels. Fraction
     * quantization below is derived separately from that shared position. */
    long double sx=(long double)t->image[level].w/t->image[0].w,sy=(long double)t->image[level].h/t->image[0].h;
    long double xx[4]={x+box[stage*3][0]*sx,x+box[stage*3][1]*sx},yy[4]={y+box[stage*3+1][0]*sy,y+box[stage*3+1][1]*sy};
    unsigned nx=xx[0]==xx[1] ? 1 : 2,ny=yy[0]==yy[1] ? 1 : 2;
    if (floorl(xx[0])+1<xx[1]) xx[nx++]=floorl(xx[0])+1;
    if (floorl(yy[0])+1<yy[1]) yy[ny++]=floorl(yy[0])+1;
    *low=255; *high=0;
    for (unsigned j=0;j<ny;j++) for (unsigned i=0;i<nx;i++) {
        int ix=floorl(xx[i]),iy=floorl(yy[j]);
        long double fx=xx[i]-ix,fy=yy[j]-iy;
        if (arithmetic->bits) {
            long double scale=ldexpl(1,arithmetic->bits);
            fx=(arithmetic->rounded ? roundl(fx*scale) : floorl(fx*scale))/scale;
            fy=(arithmetic->rounded ? roundl(fy*scale) : floorl(fy*scale))/scale;
        }
        long double row0=oracle_texel3d(t,ix,iy,shift,level)*(1-fx)+oracle_texel3d(t,ix+1,iy,shift,level)*fx;
        long double row1=oracle_texel3d(t,ix,iy+1,shift,level)*(1-fx)+oracle_texel3d(t,ix+1,iy+1,shift,level)*fx;
        long double c=arithmetic->separable ? roundl(roundl(row0)*(1-fy)+roundl(row1)*fy) : row0*(1-fy)+row1*fy;
        if (c<*low) *low=c;
        if (c>*high) *high=c;
    }
    /* Each row-rounded value can differ by one level from continuous
     * bilinear accumulation; two cover rounded corners versus interior.
     * This only widens a rejection bound, never a point witness. */
    if (arithmetic->separable && (xx[0]!=xx[1] || yy[0]!=yy[1])) {
        *low=fmaxl(0,*low-2); *high=fminl(255,*high+2);
    }
}
static void oracle_mip3d(const struct sw_texture *t,long double u,long double v,long double lambda,
    unsigned shift,const long double box[6][2],unsigned stage,const struct oracle_arithmetic3d *arithmetic,long double *low,long double *high)
{
    if (t->min==GL_LINEAR || lambda<=0) { oracle_sample3d(t,u,v,shift,0,box,stage,arithmetic,low,high); return; }
    unsigned max=0;
    while (max+1<SW_LEVELS && t->image[max+1].pixels) max++;
    if (lambda>max) lambda=max;
    long double begin=fmaxl(0,fminl(max,lambda+box[stage*3+2][0])),end=fmaxl(0,fminl(max,lambda+box[stage*3+2][1]));
    *low=255; *high=0;
    /* Piecewise linear LOD bounds: endpoints and the one possible
     * integer/half-integer boundary. Acceptance uses point witnesses only. */
    for (unsigned i=0;i<3;i++) {
        long double l=i==0 ? begin : i==1 ? end : (floorl(begin*2)+1)/2;
        if (l>end) continue;
        unsigned level=t->min==GL_LINEAR_MIPMAP_LINEAR ? floorl(l) : floorl(l+.5L);
        if (level>max) level=max;
        long double a,b; oracle_sample3d(t,u,v,shift,level,box,stage,arithmetic,&a,&b);
        if (t->min==GL_LINEAR_MIPMAP_LINEAR && level<max) {
            long double c,d; oracle_sample3d(t,u,v,shift,level+1,box,stage,arithmetic,&c,&d);
            long double f=l-level;
            a=a*(1-f)+c*f; b=b*(1-f)+d*f;
        }
        if (a<*low) *low=a;
        if (b>*high) *high=b;
    }
}
struct oracle_position3d { long double primary[4],u[2],v[2],lambda[2]; struct oracle_arithmetic3d arithmetic; };
static struct oracle_position3d oracle_position3d(const struct gl_hw_command *g,unsigned x,unsigned y)
{
    const struct sw_vertex *v=g->vertices;
    struct oracle_position3d p={0};
    long double xx=x*16+8,yy=(g->state.height-y)*16-8,w[3];
    for (unsigned i=0;i<3;i++) {
        const struct sw_vertex *a=v+(i+1)%3,*b=v+(i+2)%3;
        w[i]=((long double)a->x-xx)*(b->y-yy)-((long double)a->y-yy)*(b->x-xx);
    }
    long double area=w[0]+w[1]+w[2];
    if (!area) return p;
    for (unsigned k=0;k<4;k++) for (unsigned i=0;i<3;i++) p.primary[k]+=w[i]*v[i].color[k==3 ? 3 : 2-k]/area;
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
            rho=fmaxl(rho,fmaxl(fabsl((du*q-u*dq)*16/(q*q))*texture->image[0].w,
                                fabsl((dt*q-t*dq)*16/(q*q))*texture->image[0].h));
        }
        p.u[stage]=u*16/q; p.v[stage]=t*16/q; p.lambda[stage]=rho>1 ? log2l(rho) : 0;
    }
    return p;
}
static void oracle_box3d(const struct gl_hw_command *g,long double box[6][2]) {
    for (unsigned stage=0;stage<2;stage++) for (unsigned axis=0;axis<3;axis++) {
        long double radius=axis==2 ? 1.0L/16 : (long double)CIUKI_R3D_SAMPLE_DELTA_Q16/65536;
        if (!g->state.unit[stage].enabled || (axis==2 && g->state.unit[stage].texture->min==GL_LINEAR)) radius=0;
        box[stage*3+axis][0]=-radius; box[stage*3+axis][1]=radius;
    }
}
static void oracle_source_box3d(const struct gl_hw_command *g,const struct oracle_position3d *p,
    const long double box[6][2],long double lower[4],long double upper[4])
{
    for (unsigned shift=0;shift<32;shift+=8) {
        long double low=roundl(p->primary[shift/8]),high=low;
        for (unsigned stage=0;stage<2;stage++) if (g->state.unit[stage].enabled) {
            const struct sw_texture *texture=g->state.unit[stage].texture;
            long double a,b;
            oracle_mip3d(texture,p->u[stage],p->v[stage],p->lambda[stage],shift,box,stage,&p->arithmetic,&a,&b);
            if (shift==24 && gl_hw_texture_rgb(texture->image[0].internal)) continue;
            if (g->state.unit[stage].mode==GL_REPLACE) { low=roundl(a); high=roundl(b); }
            else if (g->state.unit[stage].mode==GL_ADD && shift!=24) {
                low=fminl(255,low+roundl(a)); high=fminl(255,high+roundl(b));
            } else if (g->state.unit[stage].mode==GL_DECAL) {
                if (shift!=24) {
                    long double al,ah;
                    oracle_mip3d(texture,p->u[stage],p->v[stage],p->lambda[stage],24,box,stage,&p->arithmetic,&al,&ah);
                    al=roundl(al); ah=roundl(ah);
                    long double values[]={low*(255-al)+roundl(a)*al,low*(255-ah)+roundl(a)*ah,
                        high*(255-al)+roundl(b)*al,high*(255-ah)+roundl(b)*ah};
                    low=high=values[0];
                    for (unsigned i=1;i<4;i++) { low=fminl(low,values[i]); high=fmaxl(high,values[i]); }
                    low=roundl(low/255); high=roundl(high/255);
                }
            } else { low=roundl(low*roundl(a)/255); high=roundl(high*roundl(b)/255); }
        }
        lower[shift/8]=low; upper[shift/8]=high;
    }
}
static void oracle_source3d(const struct gl_hw_command *g,unsigned x,unsigned y,long double lower[4],long double upper[4]) {
    struct oracle_position3d p=oracle_position3d(g,x,y);
    long double box[6][2]; oracle_box3d(g,box); oracle_source_box3d(g,&p,box,lower,upper);
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
static bool oracle_compare3d(unsigned a,unsigned b,unsigned f) {
    switch (f) {
    case GL_NEVER: return false; case GL_ALWAYS: return true;
    case GL_LESS: return a<b; case GL_LEQUAL: return a<=b;
    case GL_EQUAL: return a==b; case GL_NOTEQUAL: return a!=b;
    case GL_GREATER: return a>b; default: return a>=b;
    }
}
/* A box is only a rejection bound. Acceptance always has a concrete common
 * (u,v,LOD) witness per stage, including the alpha decision and blend factors.
 * Exhaustion refuses; it never turns a per-channel box into a colour proof. */
static bool oracle_outcome3d(const struct gl_hw_command *g,const long double lo[4],const long double hi[4],
    uint32_t actual,uint32_t destination,int outcome) {
    if (outcome) {
        bool possible=false;
        for (unsigned a=(unsigned)lo[3];a<=(unsigned)hi[3];a++)
            possible|=oracle_compare3d(a,g->state.alpha_ref,g->state.alpha_func)==(outcome>0);
        if (!possible) return false;
        if (outcome<0) return true;
    }
    for (unsigned k=0;k<4;k++) {
        long double a=lo[k],b=hi[k];
        if (g->state.blend) oracle_blend3d(&g->state,a,b,lo[3],hi[3],destination>>(k*8)&255,destination>>24,k==3,&a,&b);
        unsigned c=actual>>(k*8)&255;
        if (c+1<a || c>b+1) return false;
    }
    return true;
}
static long double oracle_error3d(const struct gl_hw_command *g,const long double color[4],
    uint32_t actual,uint32_t destination,int outcome) {
    long double error=0;
    if (outcome) {
        long double distance=256;
        for (unsigned a=0;a<256;a++) if (oracle_compare3d(a,g->state.alpha_ref,g->state.alpha_func)==(outcome>0))
            distance=fminl(distance,fabsl(a-color[3]));
        error+=distance*distance;
        if (outcome<0) return error;
    }
    for (unsigned k=0;k<4;k++) {
        long double a=color[k],b=a;
        if (g->state.blend) oracle_blend3d(&g->state,a,a,color[3],color[3],destination>>(k*8)&255,destination>>24,k==3,&a,&b);
        long double d=fabsl((actual>>(k*8)&255)-a);
        error+=d*d;
    }
    return error;
}
static bool oracle_fit3d(const struct gl_hw_command *g,const struct oracle_position3d *p,
    const long double box[6][2],uint32_t actual,uint32_t destination,int outcome,unsigned phase) {
    long double point[6][2]={{0}},lo[4],hi[4];
    oracle_source_box3d(g,p,point,lo,hi);
    if (oracle_outcome3d(g,lo,hi,actual,destination,outcome)) return true;
    oracle_source_box3d(g,p,box,lo,hi);
    if (!oracle_outcome3d(g,lo,hi,actual,destination,outcome)) return false;
    /* Finite-neighbour LOD and Q8 log recovery usually move both samplers
     * by a few Q8 steps. Try those bounded witnesses before spatial fitting. */
    if (!phase && (box[2][1] || box[5][1])) {
        for (unsigned step=1;step<=16;step++) for (unsigned mask=1;mask<4;mask++)
        for (int sign=-1;sign<=1;sign+=2) {
            for (unsigned stage=0;stage<2;stage++) {
                long double a=(mask&(1u<<stage)) ? sign*(long double)step/256 : 0;
                a=fmaxl(box[stage*3+2][0],fminl(box[stage*3+2][1],a));
                point[stage*3+2][0]=point[stage*3+2][1]=a;
            }
            oracle_source_box3d(g,p,point,lo,hi);
            if (oracle_outcome3d(g,lo,hi,actual,destination,outcome)) return true;
        }
    }
    if (!phase) return false;
    /* Probe near the exact sample first. Fraction truncation is usually a
     * sub-quantum move; these are witnesses, never additional tolerances. */
    for (unsigned scale=0;scale<4;scale++) for (unsigned code=1;code<81;code++) {
        unsigned c=code;
        for (unsigned stage=0;stage<2;stage++) for (unsigned axis=0;axis<2;axis++) {
            unsigned digit=c%3; c/=3;
            long double offset=digit==1 ? -ldexpl(1,(int)scale-9) : digit==2 ? ldexpl(1,(int)scale-9) : 0;
            point[stage*3+axis][0]=point[stage*3+axis][1]=offset;
        }
        oracle_source_box3d(g,p,point,lo,hi);
        if (oracle_outcome3d(g,lo,hi,actual,destination,outcome)) return true;
    }
    if (phase==1) return false;
    uint32_t seed=0x12345678;
    for (unsigned trial=0;trial<512;trial++) {
        for (unsigned i=0;i<6;i++) {
            seed=seed*1664525u+1013904223u;
            long double radius=(box[i][1]-box[i][0])/2;
            if (trial<256) radius=fminl(radius,i%3==2 ? 1.0L/64 : 1.0L/128);
            point[i][0]=point[i][1]=((long double)(seed>>8)/8388607.5L-1)*radius;
        }
        oracle_source_box3d(g,p,point,lo,hi);
        if (oracle_outcome3d(g,lo,hi,actual,destination,outcome)) return true;
    }
    if (phase==2) return false;
    for (unsigned i=0;i<6;i++) point[i][0]=point[i][1]=0;
    oracle_source_box3d(g,p,point,lo,hi);
    long double error=oracle_error3d(g,lo,actual,destination,outcome);
    for (unsigned sweep=0;sweep<12;sweep++) for (unsigned axis=0;axis<6;axis++) {
        long double origin=point[axis][0],best=origin;
        long double step=ldexpl(box[axis][1]-box[axis][0],-(int)(sweep+2));
        for (int direction=-1;direction<=1;direction+=2) {
            long double a=fmaxl(box[axis][0],fminl(box[axis][1],origin+direction*step));
            point[axis][0]=point[axis][1]=a;
            oracle_source_box3d(g,p,point,lo,hi);
            if (oracle_outcome3d(g,lo,hi,actual,destination,outcome)) return true;
            long double e=oracle_error3d(g,lo,actual,destination,outcome);
            if (e<error) { error=e; best=a; }
        }
        point[axis][0]=point[axis][1]=best;
    }
    return false;
}
static bool oracle_search3d(const struct gl_hw_command *g,const struct oracle_position3d *p,
    long double box[6][2],uint32_t actual,uint32_t destination,int outcome,unsigned *budget,unsigned depth) {
    if (!*budget) return false;
    --*budget;
    long double point[6][2],lo[4],hi[4];
    for (unsigned i=0;i<6;i++) point[i][0]=point[i][1]=(box[i][0]+box[i][1])/2;
    oracle_source_box3d(g,p,point,lo,hi);
    if (oracle_outcome3d(g,lo,hi,actual,destination,outcome)) return true;
    oracle_source_box3d(g,p,box,lo,hi);
    if (!oracle_outcome3d(g,lo,hi,actual,destination,outcome) || depth==42) return false;
    unsigned axis=depth%6;
    for (unsigned i=0;i<6 && box[axis][0]==box[axis][1];i++) axis=(axis+1)%6;
    long double a=box[axis][0],b=box[axis][1],mid=(a+b)/2;
    if (a==b) return false;
    box[axis][1]=mid;
    bool found=oracle_search3d(g,p,box,actual,destination,outcome,budget,depth+1);
    box[axis][0]=mid; box[axis][1]=b;
    if (!found) found=oracle_search3d(g,p,box,actual,destination,outcome,budget,depth+1);
    box[axis][0]=a;
    return found;
}
static bool oracle_joint3d(const struct gl_hw_command *g,unsigned x,unsigned y,uint32_t actual,uint32_t destination,int outcome) {
    struct oracle_position3d p=oracle_position3d(g,x,y);
    long double box[6][2]; oracle_box3d(g,box);
    for (unsigned phase=0;phase<4;phase++) for (unsigned mode=0;mode<10;mode++) {
        p.arithmetic=(struct oracle_arithmetic3d){.bits=mode<4 ? 8 : mode<8 ? 12 : 0,
            .rounded=(mode&2)!=0,.separable=(mode&1)!=0};
        unsigned budget=1024;
        if (oracle_fit3d(g,&p,box,actual,destination,outcome,phase) ||
            (phase==3 && oracle_search3d(g,&p,box,actual,destination,outcome,&budget,0))) return true;
    }
    return false;
}
static bool oracle_pixel3d(const struct gl_hw_command *g,unsigned x,unsigned y,uint32_t actual) {
    return oracle_joint3d(g,x,y,actual,g->clear_color,0);
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
    if (g->state.alpha_test && unchanged && oracle_joint3d(g,x,y,actual,before,-1)) return true;
    unsigned wanted=g->state.depth_test && g->state.depth_write ? expected_z : before_depth;
    return abs((int)depth-(int)wanted)<=1 && oracle_joint3d(g,x,y,actual,before,g->state.alpha_test ? 1 : 0);
}
