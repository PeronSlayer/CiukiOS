/* f7-27 Mach64 3D backend: differential tests against the scalar reference,
 * qualification on the independent model, containment faults, fuzzing and
 * software fallback. Host FP only in the model; kernel objects stay FP-free.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "mach64_3d_model.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d CHECK(%s)\n",__FILE__,__LINE__,#x); abort(); } } while (0)
#define M m64m
static const struct render3d_host host={0,m64m_alloc,m64m_free,m64m_now,0};
static struct render3d_service hw,sw;
static uint32_t seed=0x1234567u;
static uint32_t rnd(void) { seed=seed*1664525u+1013904223u; return seed>>8; }
static unsigned pick(unsigned n) { return rnd()%n; }

/* ---------------------------------------------------------------- encoders */
static void encoders(void)
{
    /* Q16.16 to IEEE: exact for <=24 significant bits, ties to even above. */
    for (unsigned i=0;i<200000;i++) {
        uint32_t v=i<1000 ? i : rnd()^(rnd()<<12);
        uint32_t bits=mach64_3d_fixed_ieee(v,16); float f; memcpy(&f,&bits,4);
        float ref=(float)((double)v/65536.0);
        CHECK(v ? f==ref : !bits);
    }
    for (unsigned i=0;i<200000;i++) {
        uint64_t n=1+rnd()%1000,d=1+(((uint64_t)rnd()<<8)|rnd())%((1ull<<34)-1);
        if (i<64) { n=16; d=1+i; }
        uint32_t bits=mach64_3d_ratio_ieee(n,d); float f; memcpy(&f,&bits,4);
        /* Host rounding through long double can double-round; accept the
         * correctly rounded float, checked by its own half-ulp bound. */
        long double exact=(long double)n/(long double)d,err=fabsl((long double)f-exact);
        CHECK(err<=exact*0x1p-24L);
        uint32_t below=bits-1,above=bits+1; float fb,fa; memcpy(&fb,&below,4); memcpy(&fa,&above,4);
        CHECK(err<=fabsl((long double)fb-exact) && err<=fabsl((long double)fa-exact));
    }
    CHECK(!mach64_3d_ratio_ieee(0,5) && !mach64_3d_ratio_ieee(5,0) && !mach64_3d_ratio_ieee(1ull<<40,1));
    /* Triangle packing: grid, X_Y, Z, W, OOA sign/magnitude, order. */
    static struct render3d_request q;
    for (unsigned i=0;i<20000;i++) {
        memset(&q,0,sizeof(q)); q.count=3; bool second=i&1;
        for (unsigned v=0;v<3;v++) {
            q.vertices[v]=(struct render3d_vertex){(int32_t)(pick(4097)<<14),(int32_t)(pick(4097)<<14),pick(65536),
                256+pick(16777216-255),rnd()|0x01000000u,(int32_t)(pick(32*65536+1))-16*65536,(int32_t)(pick(32*65536+1))-16*65536};
            q.uv1[v][0]=(int32_t)pick(32*65536+1)-16*65536; q.uv1[v][1]=(int32_t)pick(32*65536+1)-16*65536;
        }
        struct m64_write w[MACH64_3D_TRIANGLE_WRITES]; unsigned n=99;
        CHECK(!mach64_3d_triangle(&q,0,second,w,&n));
        int64_t x[3],y[3];
        for (unsigned v=0;v<3;v++) { x[v]=q.vertices[v].x>>14; y[v]=q.vertices[v].y>>14; }
        int64_t a=(x[0]-x[2])*(y[1]-y[2])-(y[0]-y[2])*(x[1]-x[2]);
        if (!a) { CHECK(!n); continue; }
        CHECK(n==(second ? 31u : 22u));
        unsigned k=0;
        for (unsigned v=0;v<3;v++) {
            unsigned stride=0x20;
            if (second) {
                static const uint32_t sec[3]={0xb28,0xb34,0xaa0};
                CHECK(w[k].reg==sec[v] && w[k+1].reg==sec[v]+4 && w[k+2].reg==sec[v]+8);
                CHECK(w[k+2].value==w[k+5].value); k+=3;
            }
            uint32_t base=0xa40+v*stride;
            for (unsigned j=0;j<7;j++) CHECK(w[k+j].reg==base+j*4);
            float u,vv,ww; memcpy(&u,&w[k].value,4); memcpy(&vv,&w[k+1].value,4); memcpy(&ww,&w[k+2].value,4);
            CHECK(u==(float)(q.vertices[v].u/65536.0) && vv==(float)(q.vertices[v].v/65536.0));
            CHECK(ww==(float)(q.vertices[v].rhw/65536.0));
            CHECK(!w[k+3].value && w[k+4].value==q.vertices[v].z<<16 && w[k+5].value==q.vertices[v].color);
            CHECK(w[k+6].value==(uint32_t)((x[v]<<16)|y[v]));
            k+=7;
        }
        CHECK(w[k].reg==0xa9c && k+1==n);
        float ooa; memcpy(&ooa,&w[k].value,4);
        CHECK((ooa<0)==(a<0) && fabsl((long double)ooa-16.0L/a)<=fabsl(16.0L/a)*0x1p-24L);
    }
    /* Off-grid and out-of-range vertices never encode. */
    memset(&q,0,sizeof(q)); q.count=3;
    q.vertices[0]=(struct render3d_vertex){1<<12,0,0,65536,0,0,0};
    q.vertices[1]=(struct render3d_vertex){8<<16,0,0,65536,0,0,0};
    q.vertices[2]=(struct render3d_vertex){0,8<<16,0,65536,0,0,0};
    struct m64_write w[MACH64_3D_TRIANGLE_WRITES]; unsigned n;
    CHECK(mach64_3d_triangle(&q,0,false,w,&n)==-EINVAL);
    q.vertices[0].x=0; q.vertices[0].rhw=255; CHECK(mach64_3d_triangle(&q,0,false,w,&n)==-EINVAL);
    q.count=1; CHECK(mach64_3d_triangle(&q,0,false,w,&n)==-EINVAL);
    q.count=2; CHECK(mach64_3d_triangle(&q,0,false,w,&n)==-EINVAL);
    printf("PASS mach64 encoders: Q16.16 IEEE, rational ONE_OVER_AREA, Mesa native vertex order, quarter-pixel grid\n");
}

/* --------------------------------------------------------- service helpers */
static void services(unsigned w,unsigned h,bool perspective,bool alpha,bool blend)
{
    m64m_init(w,h);
    M.device.qualified=M.device.fullsize_qualified=true;
    M.device.perspective_qualified=perspective; M.device.alpha_qualified=alpha; M.device.blend_qualified=blend;
    M.engine.render3d_qualified=true;
    render3d_init(&hw,&host,42,mach64_3d_backend(),&M.device,M.device.heap);
    render3d_init(&sw,&host,42,0,0,(struct render3d_extent){0});
}
static void close_services(void)
{
    CHECK(!render3d_close(&hw)); CHECK(!render3d_close(&sw));
    CHECK(!hw.ram_bytes && !sw.ram_bytes);
    m64m_destroy();
}
struct texspec { uint32_t format,w,h; unsigned pattern; };
static unsigned texel_bytes(uint32_t f) { return f<=R3D_RGBA8888 ? 4 : f<=R3D_ARGB4444 ? 2 : 1; }
static uint32_t canonical(const struct texspec *t,unsigned x,unsigned y)
{
    /* pattern 0: smooth (<=1 level per texel), 1: high contrast, 2: alpha ramp. */
    unsigned r=(x*7+y*3)&255,g=(x*3+y*5+40)&255,b=(x+y*2+90)&255,a=255;
    if (t->pattern==0) {
        /* Seamless under REPEAT and at most one level per texel: sampling
         * position differences of a fraction of a texel stay sub-level. */
        unsigned tx=x<t->w-1-x ? x : t->w-1-x,ty=y<t->h-1-y ? y : t->h-1-y;
        r=tx+ty/2; g=255-tx/2-ty; b=60+(tx+ty)/2; a=255-(tx+ty)/2;
    }
    if (t->pattern==1) { r=(x^y)&1 ? 255 : 0; g=(x*37+y*11)&255; b=(y*53)&255; a=((x+y)&3)*85; }
    if (t->pattern==2) { a=(x*255)/(t->w>1 ? t->w-1 : 1); }
    return a<<24|r<<16|g<<8|b;
}
static void encode_texel(const struct texspec *t,uint32_t c,uint8_t *out)
{
    unsigned a=c>>24,r=c>>16&255,g=c>>8&255,b=c&255;
    switch (t->format) {
    case R3D_ARGB8888: memcpy(out,&c,4); break;
    case R3D_RGBA8888: out[0]=r; out[1]=g; out[2]=b; out[3]=a; break;
    case R3D_RGB565: { uint16_t v=(uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3)); memcpy(out,&v,2); break; }
    case R3D_ARGB4444: { uint16_t v=(uint16_t)((a>>4)<<12|(r>>4)<<8|(g>>4)<<4|(b>>4)); memcpy(out,&v,2); break; }
    case R3D_L8: out[0]=(uint8_t)r; break;
    default: out[0]=(uint8_t)a; break;
    }
}
static uint32_t texture(struct render3d_service *s,uint32_t ctx,const struct texspec *t)
{
    unsigned bpp=texel_bytes(t->format);
    uint8_t *pixels=calloc(1,t->w*t->h*bpp); CHECK(pixels);
    for (unsigned y=0;y<t->h;y++) for (unsigned x=0;x<t->w;x++) encode_texel(t,canonical(t,x,y),pixels+(y*t->w+x)*bpp);
    struct render3d_texture_desc d={.format=t->format,.width=t->w,.height=t->h,.levels=1,
        .image={{pixels,t->w*t->h*bpp,t->w*bpp}}};
    uint32_t id=0; CHECK(!render3d_create_texture(s,7,ctx,&d,&id)); free(pixels);
    return id;
}
struct set { uint32_t ctx,target,depth,tex0,tex1,copy; };
static struct set make_set(struct render3d_service *s,enum render3d_policy policy,unsigned w,unsigned h,
                           const struct texspec *t0,const struct texspec *t1)
{
    struct set out={0};
    CHECK(!render3d_create_context(s,7,policy,&out.ctx));
    CHECK(!render3d_create_resource(s,7,out.ctx,R3D_COLOR,w,h,0,0,&out.target));
    CHECK(!render3d_create_resource(s,7,out.ctx,R3D_D16,w,h,0,0,&out.depth));
    CHECK(!render3d_create_resource(s,7,out.ctx,R3D_LINEAR_COLOR,w,h,0,0,&out.copy));
    if (t0) out.tex0=texture(s,out.ctx,t0);
    if (t1) out.tex1=texture(s,out.ctx,t1);
    return out;
}
static void bind(struct render3d_request *q,const struct set *s)
{
    q->target=s->target;
    q->depth=q->flags&R3D_DEPTH_FLAGS ? s->depth : 0;
    q->texture=q->flags&R3D_TEXTURED ? s->tex0 : 0;
    q->texture1=q->flags&R3D_TEXTURE1 ? s->tex1 : 0;
    q->copy_target=q->flags&R3D_COPY ? s->copy : 0;
}
static int submit(struct render3d_service *s,const struct set *set,struct render3d_request *q)
{
    bind(q,set);
    struct render3d_fence f;
    int e=render3d_submit(s,7,set->ctx,q,sizeof(*q),&f);
    if (e) return e;
    while (s->queued) { int r=render3d_step(s); if (r<0) return r; }
    return render3d_wait(s,7,set->ctx,f,0);
}
struct stats { unsigned cases,refused,max_channel,max_depth; uint64_t fragments; };
static struct stats stats;
static void compare(const struct set *a,const struct set *b,unsigned w,unsigned h,bool depth,bool copy,bool exact_coverage)
{
    uint32_t *ca=calloc(w*h,4),*cb=calloc(w*h,4),*pa=calloc(w*h,4); uint16_t *za=calloc(w*h,2),*zb=calloc(w*h,2);
    CHECK(ca && cb && pa && za && zb);
    CHECK(!render3d_readback(&hw,7,a->ctx,a->target,ca,w*h*4));
    CHECK(!render3d_readback(&sw,7,b->ctx,b->target,cb,w*h*4));
    if (copy) { CHECK(!render3d_readback(&hw,7,a->ctx,a->copy,pa,w*h*4)); CHECK(!memcmp(ca,pa,w*h*4)); }
    if (depth) { CHECK(!render3d_readback(&hw,7,a->ctx,a->depth,za,w*h*2)); CHECK(!render3d_readback(&sw,7,b->ctx,b->depth,zb,w*h*2)); }
    for (unsigned i=0;i<w*h;i++) {
        if (exact_coverage) CHECK(!ca[i]==!cb[i]);
        for (unsigned s=0;s<32;s+=8) {
            unsigned x=ca[i]>>s&255,y=cb[i]>>s&255,d=x>y ? x-y : y-x;
            if (d>stats.max_channel) stats.max_channel=d;
            if (d>2) { fprintf(stderr,"pixel %u,%u hw %08x sw %08x\n",i%w,i/w,ca[i],cb[i]); CHECK(0); }
        }
        if (depth) {
            unsigned d=za[i]>zb[i] ? za[i]-zb[i] : zb[i]-za[i];
            if (d>stats.max_depth) stats.max_depth=d;
            if (d>1) fprintf(stderr,"depth %u,%u hw %u sw %u colour hw %08x sw %08x\n",i%w,i/w,za[i],zb[i],ca[i],cb[i]);
            CHECK(d<=1);
        }
    }
    free(ca); free(cb); free(pa); free(za); free(zb);
}
static void destroy(const struct set *a,const struct set *b)
{ CHECK(!render3d_destroy_context(&hw,7,a->ctx)); CHECK(!render3d_destroy_context(&sw,7,b->ctx)); }

/* One request on hardware and on the scalar reference; refusals emit nothing. */
static void pair(struct render3d_request *q,unsigned w,unsigned h,const struct texspec *t0,const struct texspec *t1,int expect)
{
    struct set a=make_set(&hw,R3D_HARDWARE_REQUIRED,w,h,t0,t1),b=make_set(&sw,R3D_SOFTWARE,w,h,t0,t1);
    uint64_t writes=M.writes,draws=M.draws;
    int e=submit(&hw,&a,q);
    if (e!=expect) fprintf(stderr,"expected %d got %d flags=%x\n",expect,e,q->flags);
    CHECK(e==expect);
    stats.cases++;
    if (e) { stats.refused++; CHECK(M.writes==writes && M.draws==draws); destroy(&a,&b); return; }
    CHECK(!submit(&sw,&b,q));
    if (getenv("M64_TRACE")) {
        fprintf(stderr,"case %u flags=%x depth_func=%u flat=%u count=%u clear_depth=%u\n",stats.cases,q->flags,q->state.depth_func,q->state.flat,q->count,q->clear_depth);
        for (unsigned i=0;i<q->count;i++) fprintf(stderr,"  v%u %d.%04x %d.%04x z=%u\n",i,q->vertices[i].x>>16,q->vertices[i].x&0xffff,q->vertices[i].y>>16,q->vertices[i].y&0xffff,q->vertices[i].z);
    }
    compare(&a,&b,w,h,(q->flags&(R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_CLEAR_DEPTH))!=0,(q->flags&R3D_COPY)!=0,false);
    destroy(&a,&b);
    /* Exact coverage of the same geometry: constant white, no texture, no
     * depth, no blend, cleared to zero. */
    struct render3d_request *c=calloc(1,sizeof(*c)); CHECK(c);
    c->flags=R3D_CLEAR_COLOR; c->count=q->count;
    for (unsigned i=0;i<q->count;i++) { c->vertices[i]=q->vertices[i]; c->vertices[i].color=0xffffffffu; c->vertices[i].u=c->vertices[i].v=0; }
    a=make_set(&hw,R3D_HARDWARE_REQUIRED,w,h,0,0); b=make_set(&sw,R3D_SOFTWARE,w,h,0,0);
    CHECK(!submit(&hw,&a,c)); CHECK(!submit(&sw,&b,c));
    compare(&a,&b,w,h,false,false,true);
    destroy(&a,&b); free(c);
}
static int32_t grid(unsigned limit) { return (int32_t)(pick(limit*4+1)<<14); }
static void geometry(struct render3d_request *q,unsigned w,unsigned h,unsigned triangles,bool constant_z,bool perspective)
{
    q->count=triangles*3;
    for (unsigned t=0;t<triangles;t++) {
        unsigned z=pick(60000)+1000;
        for (unsigned i=0;i<3;i++) {
            struct render3d_vertex *v=&q->vertices[t*3+i];
            *v=(struct render3d_vertex){grid(w),grid(h),constant_z ? z : z/2+pick(16384),65536,
                0xff000000u|(rnd()&0xffffffu),0,0};
            if (perspective) v->rhw=32768+pick(98305); /* ratio <=4 */
        }
        q->flat_color[t]=0xff000000u|(rnd()&0xffffffu);
    }
}
/* uv proportional to position: 1:1 texels for NEAREST exactness. */
static void exact_uv(struct render3d_request *q,unsigned tw,unsigned th,bool stage1)
{
    for (unsigned i=0;i<q->count;i++) {
        struct render3d_vertex *v=&q->vertices[i];
        v->x&=~0xffff; v->y&=~0xffff; /* integer pixel corners */
        int32_t u=(int32_t)(((int64_t)v->x)/tw),vv=(int32_t)(((int64_t)v->y)/th);
        if (stage1) { q->uv1[i][0]=u; q->uv1[i][1]=vv; } else { v->u=u; v->v=vv; }
    }
}
static void random_uv(struct render3d_request *q,bool stage1,int span)
{
    for (unsigned i=0;i<q->count;i++) {
        int32_t u=(int32_t)pick(2*span*65536+1)-span*65536,v=(int32_t)pick(2*span*65536+1)-span*65536;
        if (stage1) { q->uv1[i][0]=u; q->uv1[i][1]=v; } else { q->vertices[i].u=u; q->vertices[i].v=v; }
    }
}
static void differential(void)
{
    static struct render3d_request q;
    const unsigned W=96,H=72;
    services(128,96,true,true,true);
    struct m64_write one={0xec4,0};
    struct m64_budget capped={M.clock,M64_3D_READ_LIMIT};
    CHECK(m64_render3d_write(&M.engine,&one,M64_3D_WRITE_LIMIT+1,&capped)==-EINVAL);
    CHECK(m64_render3d_write(&M.engine,&one,1,&capped)==-ETIMEDOUT && !M.writes);
    capped=(struct m64_budget){M.clock,M64_3D_READ_LIMIT-1};
    CHECK(m64_render3d_idle(&M.engine,&capped)==-ETIMEDOUT && capped.reads==M64_3D_READ_LIMIT);
    static const uint32_t sizes[][2]={{16,16},{33,17},{64,64},{96,72},{1,1},{8,40}};
    /* Every feature-flag combination, including structurally invalid stage-1
     * combinations, goes through the same service and independent decoder. */
    struct texspec flags0={R3D_ARGB8888,32,32,0},flags1={R3D_RGBA8888,32,32,0};
    for (unsigned flags=1;flags<=R3D_ALL_FEATURES;flags++) {
        memset(&q,0,sizeof(q)); geometry(&q,32,32,1,true,false);
        q.flags=flags; q.clear_color=0xff102030u; q.clear_depth=65535;
        q.state.sampler[0].filter=q.state.sampler[1].filter=R3D_BILINEAR;
        random_uv(&q,false,1); random_uv(&q,true,1);
        bool invalid=((flags&R3D_TEXTURE1) && !(flags&R3D_TEXTURED)) ||
            (flags&(R3D_STENCIL_TEST|R3D_CLEAR_STENCIL));
        pair(&q,32,32,flags&R3D_TEXTURED ? &flags0 : 0,flags&R3D_TEXTURE1 ? &flags1 : 0,invalid ? -EINVAL : 0);
    }
    /* 1. Flat/Gouraud geometry, every depth function, write on/off. */
    for (unsigned i=0;i<160;i++) {
        unsigned w=sizes[i%6][0],h=sizes[i%6][1];
        memset(&q,0,sizeof(q));
        geometry(&q,w,h,1+pick(6),true,false);
        q.flags=R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH|(i&1 ? R3D_COPY : 0);
        q.clear_color=rnd()|0xff000000u; q.clear_depth=pick(65536);
        if (i%3) { q.flags|=R3D_DEPTH_TEST; q.state.depth_func=i%8; }
        if (i%5) q.flags|=R3D_DEPTH_WRITE;
        q.state.flat=i%4==0;
        pair(&q,w,h,0,0,0);
    }
    /* Varying depth inside single triangles: interpolation within +-1. */
    for (unsigned i=0;i<40;i++) {
        memset(&q,0,sizeof(q)); geometry(&q,W,H,1,false,false);
        q.flags=R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE; q.clear_depth=65535; q.state.depth_func=R3D_LESS;
        pair(&q,W,H,0,0,0);
    }
    /* 2. One unit: formats x combine x filter, exact NEAREST mapping and
     * smooth BILINEAR mapping, affine and perspective, REPEAT/CLAMP. */
    for (unsigned f=0;f<6;f++) for (unsigned c=0;c<4;c++) for (unsigned filter=0;filter<2;filter++) {
        struct texspec t={f,64,64,filter ? 0 : 1};
        memset(&q,0,sizeof(q)); geometry(&q,W,H,3,true,false);
        q.flags=R3D_CLEAR_COLOR|R3D_TEXTURED|R3D_COPY; q.clear_color=0xff203040u;
        q.state.sampler[0]=(struct render3d_sampler){c,filter ? R3D_BILINEAR : R3D_NEAREST,f&1,(f>>1)&1};
        if (filter) random_uv(&q,false,2); else exact_uv(&q,64,64,false);
        int expect=c==R3D_ADD || (f==R3D_A8 && c!=R3D_MODULATE) ? -EOPNOTSUPP : 0;
        pair(&q,W,H,&t,0,expect);
    }
    for (unsigned i=0;i<60;i++) {
        unsigned size=1u<<pick(9);
        struct texspec t={R3D_ARGB8888,size,1u<<pick(9),0};
        memset(&q,0,sizeof(q)); geometry(&q,W,H,1+pick(4),true,i&1);
        q.flags=R3D_CLEAR_COLOR|R3D_TEXTURED|R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE;
        q.clear_depth=65535;
        q.state.sampler[0]=(struct render3d_sampler){i%3==2 ? R3D_DECAL : i%3,R3D_BILINEAR,pick(2),pick(2)};
        random_uv(&q,false,i%4 ? 2 : 16);
        pair(&q,W,H,&t,0,0);
    }
    /* 3. Two units: stage 0 BILINEAR, stage 1 MODULATE NEAREST/BILINEAR. */
    for (unsigned i=0;i<60;i++) {
        struct texspec t0={pick(6),32u<<pick(3),16u<<pick(3),0},t1={pick(6),16u<<pick(3),32u<<pick(2),0};
        if (t0.format==R3D_A8) t0.format=R3D_ARGB8888;
        memset(&q,0,sizeof(q)); geometry(&q,W,H,1+pick(3),true,i&1);
        q.flags=R3D_CLEAR_COLOR|R3D_TEXTURED|R3D_TEXTURE1|R3D_COPY;
        q.state.sampler[0]=(struct render3d_sampler){i%3==2 ? R3D_DECAL : i%3==1 ? R3D_REPLACE : R3D_MODULATE,R3D_BILINEAR,pick(2),pick(2)};
        q.state.sampler[1]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,pick(2),pick(2)};
        random_uv(&q,false,2); random_uv(&q,true,3);
        if (i&1) {
            q.state.sampler[1].filter=R3D_NEAREST;
            for (unsigned v=0;v<q.count;v++) q.vertices[v].rhw=65536;
            exact_uv(&q,t1.w,t1.h,true);
        }
        pair(&q,W,H,&t0,&t1,0);
    }
    /* 4. Blend: the three alpha-expressible pairs; all others refuse. */
    for (unsigned s=0;s<10;s++) for (unsigned d=0;d<10;d++) {
        if (s==R3D_SRC_COLOR || s==R3D_INV_SRC_COLOR || d==R3D_DST_COLOR || d==R3D_INV_DST_COLOR) continue;
        memset(&q,0,sizeof(q)); geometry(&q,32,32,2,true,false);
        q.flags=R3D_CLEAR_COLOR; q.clear_color=0x80406080u;
        q.state.blend=1; q.state.source=s; q.state.destination=d;
        bool ok=(s==R3D_ONE && d==R3D_ZERO) || (s==R3D_ZERO && (d==R3D_ONE || d==R3D_ZERO));
        pair(&q,32,32,0,0,ok ? 0 : -EOPNOTSUPP);
    }
    /* 5. Alpha test on the unit-0 texel alpha with opaque fragments. */
    for (unsigned f=0;f<8;f++) for (unsigned fmt=0;fmt<6;fmt++) {
        struct texspec t={fmt,64,64,2};
        memset(&q,0,sizeof(q)); geometry(&q,W,H,2,true,false);
        for (unsigned i=0;i<q.count;i++) q.vertices[i].color|=0xff000000u;
        exact_uv(&q,64,64,false);
        q.flags=R3D_CLEAR_COLOR|R3D_TEXTURED|R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE;
        q.clear_depth=65535; q.clear_color=0xff000000u;
        q.state.sampler[0]=(struct render3d_sampler){R3D_MODULATE,R3D_NEAREST,0,0};
        q.state.alpha_test=1; q.state.alpha_func=f; q.state.alpha_ref=pick(256);
        bool alpha=fmt==R3D_ARGB8888 || fmt==R3D_RGBA8888 || fmt==R3D_ARGB4444 || fmt==R3D_A8;
        pair(&q,W,H,&t,0,alpha ? 0 : -EOPNOTSUPP);
    }
    /* 6. Refusals: mip filters, two units with NEAREST unit 0, unit-1
     * combine other than MODULATE, translucent fragments under alpha
     * test, off-grid vertices, oversize textures, W ratio above four. */
    struct texspec big={R3D_ARGB8888,512,64,0},t64={R3D_ARGB8888,64,64,0};
    for (unsigned filter=R3D_NEAREST_MIP;filter<=R3D_TRILINEAR;filter++) {
        memset(&q,0,sizeof(q)); geometry(&q,W,H,1,true,false); q.flags=R3D_TEXTURED;
        q.state.sampler[0].filter=filter; random_uv(&q,false,1);
        pair(&q,W,H,&t64,0,-EOPNOTSUPP);
        q.flags=R3D_TEXTURED|R3D_TEXTURE1; q.state.sampler[0].filter=R3D_BILINEAR; q.state.sampler[1].filter=filter;
        pair(&q,W,H,&t64,&t64,-EOPNOTSUPP);
    }
    memset(&q,0,sizeof(q)); geometry(&q,W,H,1,true,false); q.flags=R3D_TEXTURED|R3D_TEXTURE1;
    pair(&q,W,H,&t64,&t64,-EOPNOTSUPP); /* NEAREST unit 0 with two units */
    q.state.sampler[0].filter=R3D_BILINEAR;
    for (unsigned c=1;c<4;c++) { q.state.sampler[1].combine=c; pair(&q,W,H,&t64,&t64,-EOPNOTSUPP); }
    memset(&q,0,sizeof(q)); geometry(&q,W,H,1,true,false); q.flags=R3D_TEXTURED;
    q.state.alpha_test=1; q.vertices[1].color&=0x7fffffffu; pair(&q,W,H,&t64,0,-EOPNOTSUPP);
    memset(&q,0,sizeof(q)); geometry(&q,W,H,1,true,false); q.flags=R3D_CLEAR_COLOR;
    q.vertices[2].x+=4096; pair(&q,W,H,0,0,-EOPNOTSUPP);
    memset(&q,0,sizeof(q)); geometry(&q,W,H,1,true,false); q.flags=R3D_TEXTURED;
    pair(&q,W,H,&big,0,-EOPNOTSUPP);
    memset(&q,0,sizeof(q)); geometry(&q,W,H,1,true,false); q.flags=R3D_TEXTURED;
    q.vertices[0].rhw=32768; q.vertices[1].rhw=32768*5; pair(&q,W,H,&t64,0,-EOPNOTSUPP);
    memset(&q,0,sizeof(q)); geometry(&q,W,H,1,true,false); q.flags=R3D_DEPTH_WRITE;
    q.vertices[0].z=0; q.vertices[1].z=20000; q.vertices[2].z=40000; pair(&q,W,H,0,0,-EOPNOTSUPP);
    printf("PASS mach64 differential: %u cases (%u refused before emission), max channel delta %u, max depth delta %u, fragments %llu\n",
        stats.cases,stats.refused,stats.max_channel,stats.max_depth,(unsigned long long)M.fragments);
    close_services();
}

/* ----------------------------------------------------------- qualification */
static int qualify(unsigned w,unsigned h,uint32_t before[1024])
{
    struct render3d_service scratch,full,reference;
    m64m_resources(&scratch,64,64,true,false);
    M.device.consumer=&scratch;
    if (before) m64m_snapshot(before);
    int e=mach64_3d_probe(&M.device);
    M.device.consumer=0;
    if (M.device.lost) render3d_device_lost(&scratch);
    (void)render3d_close(&scratch);
    if (e) return e;
    M.device.fullsize_testing=true;
    m64m_resources(&full,w,h,true,true); m64m_resources(&reference,w,h,false,true);
    M.device.fullsize_testing=false; M.device.consumer=&full;
    e=mach64_3d_fullsize_probe(&M.device,&reference);
    M.device.consumer=0;
    if (M.device.lost) render3d_device_lost(&full);
    (void)render3d_close(&full); CHECK(!render3d_close(&reference));
    return e;
}
static void qualification(void)
{
    uint32_t before[1024];
    /* The filter sweep needs >=192 columns to resolve eight fraction bits. */
    m64m_init(320,240);
    CHECK(mach64_3d_fullsize_probe(&M.device,&sw)==-ENODEV && !M.writes);
    int e=qualify(320,240,before);
    if (e) fprintf(stderr,"qualification %d phase=%u precision=%u reg=%x exp=%08x act=%08x\n",e,M.device.failed_phase,
        M.device.precision_phase,M.device.failed_register,M.device.expected,M.device.actual);
    CHECK(!e && M.device.qualified && M.device.fullsize_qualified && M.device.perspective_qualified);
    CHECK(M.device.alpha_qualified && M.device.blend_qualified && !M.device.lost && !M.engine.quarantined);
    CHECK(m64m_restored(before) && M.engine.cpu_safe && M.engine.render3d_qualified);
    CHECK(M.device.minimum_qualified && M.device.selfchecks==2 && !M.device.selfcheck_error);
    CHECK(M.device.measured_filter_levels[0]>=192 && M.device.measured_filter_levels[1]>=192);
    struct ciuki_r3d_caps caps;
    mach64_3d_backend()->query_caps(&M.device,&caps);
    CHECK(caps.features==MACH64_3D_FEATURES && caps.features==127u);
    CHECK(!(caps.features&(R3D_STENCIL_TEST|R3D_CLEAR_STENCIL)));
    CHECK(caps.depth_formats==CIUKI_R3D_D16);
    CHECK(caps.max_width==320 && caps.max_height==240 && caps.max_vertices==R3D_VERTICES && caps.max_pixel_work==2*320*240);
    CHECK(caps.sampling_features==(CIUKI_R3D_AFFINE_EXACT|CIUKI_R3D_PROJECTIVE_BILINEAR) && caps.max_texture_size==256);
    CHECK(caps.alpha_functions==255 && caps.blend_factors==3 && caps.filters==3 && caps.combine_modes==11);
    printf("PASS mach64 qualification: Q1 (texture, depth, LESS rejection, Gouraud, coverage ties), Q2 four mode scenes "
        "(%u samples, channel %u, depth %u), Q3 %u envelope phases (%u samples, filter levels %u/%u), Q4 caps; registers restored, %llu writes\n",
        M.device.sample_count,M.device.max_channel_error,M.device.max_depth_error,MACH64_3D_PHASES,M.device.precision_samples,
        M.device.measured_filter_levels[0],M.device.measured_filter_levels[1],(unsigned long long)M.writes);
    m64m_destroy();
    /* Depth truncation instead of rounding stays inside the +-1 envelope. */
    m64m_init(320,240); M.depth_truncate=true;
    CHECK(!qualify(320,240,before) && M.device.qualified); m64m_destroy();
    /* The 3D attachment remains ARGB8888 on a 16-bit desktop. The D-F2
     * self-check must use the restored RGB565 2D format, not the attachment. */
    m64m_init(320,240);
    M.engine.fb.bpp=16; M.engine.fb.pitch=640; M.engine.fb.size=640*240;
    M.engine.fb.red_size=5; M.engine.fb.red_pos=11; M.engine.fb.green_size=6; M.engine.fb.green_pos=5;
    M.engine.fb.blue_size=5; M.regs[0xed0/4]=0x01040404u;
    CHECK(!qualify(320,240,before) && M.device.qualified && M.device.selfchecks==2);
    CHECK(m64m_restored(before)); m64m_destroy();
    /* The total qualification deadline includes Q1 and the gap before Q2.
     * Expiry refuses before another register write; it is not a partial
     * precision qualification that may still enable affine hardware. */
    m64m_init(320,240);
    struct render3d_service scratch;
    m64m_resources(&scratch,64,64,true,false); M.device.consumer=&scratch;
    CHECK(!mach64_3d_probe(&M.device) && M.device.minimum_qualified);
    CHECK(!render3d_close(&scratch)); M.device.consumer=0;
    M.clock+=MACH64_3D_QUALIFY_US;
    M.device.fullsize_testing=true;
    m64m_resources(&hw,320,240,true,true); m64m_resources(&sw,320,240,false,true);
    M.device.fullsize_testing=false; M.device.consumer=&hw;
    uint64_t writes=M.writes;
    CHECK(mach64_3d_fullsize_probe(&M.device,&sw)==-ETIMEDOUT);
    CHECK(!M.device.qualified && !M.device.lost && !M.engine.render3d_qualified && M.writes==writes);
    M.device.consumer=0; CHECK(!render3d_close(&hw) && !render3d_close(&sw)); m64m_destroy();
    /* A narrow mode cannot resolve the filter sweep: perspective is refused,
     * the affine full-size capability and the minimum remain. */
    m64m_init(128,96);
    CHECK(!qualify(128,96,before) && M.device.qualified && !M.device.perspective_qualified);
    CHECK(M.device.precision_error==-EILSEQ && !M.device.alpha_qualified && !M.device.blend_qualified);
    CHECK(M.device.precision_phase==5 && M.device.selfchecks==2);
    struct ciuki_r3d_caps narrow; mach64_3d_backend()->query_caps(&M.device,&narrow);
    CHECK(narrow.sampling_features==CIUKI_R3D_AFFINE_EXACT && narrow.max_texture_size==64 && !narrow.alpha_functions);
    CHECK(m64m_restored(before)); m64m_destroy();
}
/* Containment: refusal, numeric mismatch, timeout and fault paths. */
static void faults(unsigned variant)
{
    uint32_t before[1024];
    m64m_init(320,240);
    switch (variant) {
    case 0: M.block1_off=true; break;
    case 1: M.dead=true; break;
    case 2: M.hang_after_draw=true; break;
    case 3: M.corrupt_guard=true; break;
    case 4: M.bad_restore=true; break;
    case 5: M.wrong_tie=true; break;
    case 6: M.full_after=20; break;
    case 10: M.full=true; break;
    case 11: M.stuck=true; break;
    case 12: M.bad_selfcheck=true; break;
    case 13: M.bad_texture_restore=true; break;
    case 7: M.hang_after_draw=true; M.frozen=true; break;
    case 8: M.fifo_error=true; break;
    case 9: M.drop=true; break;
    }
    int e=qualify(320,240,before);
    CHECK(e && !M.device.qualified && !M.engine.render3d_qualified);
    bool quarantined=M.engine.quarantined && M.device.lost;
    if (getenv("M64_TRACE")) fprintf(stderr,"variant %u: error %d quarantine %u writes %llu phase %u\n",
        variant,e,quarantined,(unsigned long long)M.writes,M.device.failed_phase);
    switch (variant) {
    case 0: CHECK(e==-ENODEV && !quarantined && !M.writes && !M.memory_writes); break;
    case 1: CHECK(e==-EIO && quarantined && !M.writes); break;
    case 2: case 7: CHECK(e==-ETIMEDOUT && quarantined && !m64m_restored(before)); break;
    case 3: CHECK(e==-EIO && quarantined && m64m_restored(before)); break;
    case 4: CHECK(e==-EIO && quarantined); break;
    case 5: CHECK(e==-EILSEQ && !quarantined && M.device.coverage_mismatches && m64m_restored(before)); break;
    case 6: CHECK(e==-ETIMEDOUT && quarantined && M.writes>=20 && M.writes<36 && !m64m_restored(before)); break;
    case 10: case 11: CHECK(e==-EIO && quarantined && !M.writes && !M.memory_writes); break;
    case 8: CHECK(e==-EIO && quarantined && !M.writes); break;
    case 9: CHECK(e==-EILSEQ && !quarantined && m64m_restored(before)); break;
    case 12: CHECK(e==-EIO && quarantined && M.device.selfcheck_error && !M.device.minimum_qualified); break;
    case 13: CHECK(e==-EIO && quarantined && M.device.failed_register==0xdd4); break;
    }
    printf("PASS mach64 containment variant %u: error %d, quarantine %u, writes %llu, failed phase %u\n",
        variant,e,quarantined,(unsigned long long)M.writes,M.device.failed_phase);
    m64m_destroy();
}

/* ------------------------------------------------------------------- fuzz */
static void fuzz(void)
{
    services(128,96,true,true,true);
    /* Invalid groups are rejected in full before even their first allowed
     * write. Exercise unaligned/out-of-window and sensitive in-window regs. */
    static const uint32_t forbidden[]={0xca0,0xcd0,0xc14,0xc1c,0xcb0,0xdb0,0xdb4,0xffc};
    for (unsigned i=0;i<6000;i++) {
        uint32_t bad=i%3==0 ? forbidden[pick(sizeof(forbidden)/sizeof(*forbidden))] :
            i%3==1 ? (rnd()|1u) : 0x1000u+rnd();
        struct m64_write packet[2]={{0xec4,0xff123456u},{bad,rnd()}};
        struct m64_budget budget={M.clock,0}; uint64_t writes=M.writes,reads=M.reads;
        CHECK(m64_render3d_write(&M.engine,packet,2,&budget)==-EINVAL);
        CHECK(M.writes==writes && M.reads==reads);
    }
    static struct render3d_request q;
    unsigned accepted=0,refused=0,invalid=0;
    struct texspec t0={R3D_ARGB8888,64,32,0},t1={R3D_L8,32,64,0};
    struct set a=make_set(&hw,R3D_HARDWARE_REQUIRED,96,72,&t0,&t1);
    /* One admitted submission gives both textures their heap offsets. */
    memset(&q,0,sizeof(q)); geometry(&q,96,72,1,true,false);
    q.flags=R3D_TEXTURED|R3D_TEXTURE1|R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH|R3D_COPY;
    q.state.sampler[0].filter=q.state.sampler[1].filter=R3D_BILINEAR;
    CHECK(!submit(&hw,&a,&q));
    /* Malformed heap extents never reach MMIO or mapped VRAM. */
    struct render3d_context *owner=&hw.contexts[0];
    struct render3d_resource *resources[R3D_REFS]={&owner->resources[0],&owner->resources[1],
        &owner->resources[3],&owner->resources[2],&owner->resources[4]};
    CHECK(!mach64_3d_backend()->validate(&M.device,&q,resources));
    struct render3d_resource saved=*resources[0];
    for (unsigned i=0;i<2000;i++) {
        *resources[0]=saved;
        if (i%4==0) resources[0]->offset=UINT32_MAX-(rnd()&4095u);
        else if (i%4==1) resources[0]->reserved^=1u+pick(4095);
        else if (i%4==2) resources[0]->offset=M.device.heap.first-4096u;
        else resources[0]->offset=M.device.heap.first+M.device.heap.bytes;
        uint64_t writes=M.writes,reads=M.memory_reads;
        CHECK(mach64_3d_backend()->validate(&M.device,&q,resources)==-EINVAL);
        CHECK(M.writes==writes && M.memory_reads==reads);
    }
    *resources[0]=saved;
    for (unsigned i=0;i<6000;i++) {
        memset(&q,0,sizeof(q));
        static const uint32_t combos[]={R3D_CLEAR_COLOR,R3D_TEXTURED,R3D_TEXTURED|R3D_TEXTURE1,
            R3D_DEPTH_TEST|R3D_DEPTH_WRITE,R3D_FEATURES,R3D_TEXTURED|R3D_TEXTURE1|R3D_DEPTH_TEST|R3D_COPY,
            R3D_ALL_FEATURES,R3D_CLEAR_DEPTH|R3D_COPY,R3D_TEXTURED|R3D_DEPTH_WRITE};
        q.flags=i%3 ? combos[pick(sizeof(combos)/sizeof(*combos))] : rnd()&(i%7 ? R3D_ALL_FEATURES : 0xffu);
        q.count=pick(5)*3; if (i%11==0) q.count=pick(800);
        q.clear_color=rnd(); q.clear_depth=i%13 ? pick(65536) : rnd();
        uint32_t *state=(uint32_t *)&q.state;
        static const unsigned range[16]={8,8,256,2,2,10,10,2,4,5,2,2,4,5,2,2};
        for (unsigned k=0;k<sizeof(q.state)/4;k++) state[k]=i%7 ? pick(range[k]) : rnd();
        if (i%4) q.state.alpha_test=q.state.blend=0;
        for (unsigned v=0;v<q.count && v<R3D_VERTICES;v++) {
            bool sane=i%3;
            q.vertices[v]=(struct render3d_vertex){sane ? grid(96) : (int32_t)rnd(),sane ? grid(72) : (int32_t)rnd(),
                sane ? pick(65536) : rnd(),sane ? 32768+pick(65536) : rnd(),rnd(),
                sane ? (int32_t)pick(8*65536)-4*65536 : (int32_t)rnd(),sane ? (int32_t)pick(8*65536)-4*65536 : (int32_t)rnd()};
            if (i%17==0) q.vertices[v].x+=pick(16384);
        }
        struct render3d_resource *r[R3D_REFS]={0};
        struct render3d_context *c=0;
        for (unsigned k=0;k<R3D_CONTEXTS;k++) if (hw.contexts[k].live && hw.contexts[k].handle==a.ctx) c=&hw.contexts[k];
        CHECK(c);
        bind(&q,&a);
        uint32_t handles[R3D_REFS]={q.target,q.depth,q.texture,q.copy_target,q.texture1};
        for (unsigned k=0;k<R3D_REFS;k++) for (unsigned j=0;j<R3D_RESOURCES;j++)
            if (handles[k] && c->resources[j].live && c->resources[j].handle==handles[k]) r[k]=&c->resources[j];
        if (i%9==0) r[pick(R3D_REFS)]=0;
        uint64_t writes=M.writes;
        int e=mach64_3d_backend()->validate(&M.device,&q,r);
        CHECK(M.writes==writes);
        if (e==-EOPNOTSUPP) { refused++; continue; }
        if (e) { invalid++; continue; }
        CHECK(!mach64_3d_backend()->enter(&M.device));
        /* The model asserts the whitelist, FIFO credits and the heap bound. */
        uint64_t polls=M.reads;
        CHECK(!mach64_3d_backend()->execute(&M.device,&q,r));
        CHECK(M.reads-polls<M64_3D_READ_LIMIT*3);
        mach64_3d_backend()->leave(&M.device);
        accepted++;
    }
    fprintf(stderr,"fuzz accepted=%u refused=%u invalid=%u\n",accepted,refused,invalid);
    CHECK(accepted>200 && refused>200 && invalid>200 && !M.engine.quarantined);
    CHECK(!render3d_destroy_context(&hw,7,a.ctx));
    printf("PASS mach64 fuzz: 6000 register groups, 2000 invalid extents, 6000 requests, %u executed inside the whitelist/heap/FIFO model, %u refused, %u invalid, zero emission on refusal\n",
        accepted,refused,invalid);
    close_services();
}

/* ------------------------------------------------- fallback and eviction */
static void fallback(void)
{
    /* Unqualified chip: zero capabilities, AUTO is software, REQUIRED fails. */
    m64m_init(128,96);
    render3d_init(&hw,&host,42,mach64_3d_backend(),&M.device,M.device.heap);
    struct ciuki_r3d_caps caps; render3d_backend_caps(&hw,&caps);
    CHECK(!caps.features && !caps.max_size);
    mach64_3d_backend()->query_caps(&M.device,&caps);
    CHECK(!caps.features && !caps.max_size && caps.version==CIUKI_R3D_VERSION);
    uint32_t c;
    CHECK(render3d_create_context(&hw,7,R3D_HARDWARE_REQUIRED,&c)==-EOPNOTSUPP);
    CHECK(!render3d_create_context(&hw,7,R3D_AUTO,&c));
    struct render3d_context *ctx=&hw.contexts[0]; CHECK(!ctx->hardware);
    CHECK(!render3d_close(&hw)); CHECK(!M.writes && !M.memory_writes); m64m_destroy();
    services(128,96,true,true,true);
    mach64_3d_backend()->query_caps(&M.device,&caps); CHECK(caps.features);
    M.engine.fb.generation++;
    mach64_3d_backend()->query_caps(&M.device,&caps); CHECK(!caps.features);
    M.engine.fb.generation--; M.engine.fb.bpp=24;
    mach64_3d_backend()->query_caps(&M.device,&caps); CHECK(!caps.features);
    M.engine.fb.bpp=32; M.engine.fb.width++;
    mach64_3d_backend()->query_caps(&M.device,&caps); CHECK(!caps.features);
    M.engine.fb.width--;
    struct render3d_resource too_large={.kind=R3D_TEXTURE,.width=2048,.height=16};
    CHECK(mach64_3d_backend()->layout(&M.device,&too_large)==-EOPNOTSUPP && !M.writes);
    close_services();
    services(128,96,true,true,true);
    mach64_3d_backend()->quarantine(&M.device); /* loss without an enter guard */
    CHECK(M.device.lost && M.engine.quarantined && !M.engine.cpu_safe && !M.writes);
    close_services();
    /* Whole-context fallback: one refused AUTO job retires hardware. */
    services(128,96,true,true,true);
    static struct render3d_request q;
    struct texspec t={R3D_ARGB8888,64,64,0};
    struct set a=make_set(&hw,R3D_AUTO,64,64,&t,0),b=make_set(&sw,R3D_SOFTWARE,64,64,&t,0);
    memset(&q,0,sizeof(q)); geometry(&q,64,64,2,true,false); q.flags=R3D_CLEAR_COLOR|R3D_TEXTURED;
    q.state.sampler[0].filter=R3D_BILINEAR; random_uv(&q,false,1);
    uint64_t draws=M.draws;
    CHECK(!submit(&hw,&a,&q) && M.draws>draws); CHECK(!submit(&sw,&b,&q));
    q.flags=R3D_TEXTURED; q.state.sampler[0].filter=R3D_TRILINEAR; draws=M.draws;
    CHECK(!submit(&hw,&a,&q) && M.draws==draws); CHECK(!submit(&sw,&b,&q));
    q.state.sampler[0].filter=R3D_BILINEAR;
    CHECK(!submit(&hw,&a,&q) && M.draws==draws); CHECK(!submit(&sw,&b,&q));
    compare(&a,&b,64,64,false,false,false);
    destroy(&a,&b);
    /* Texture pool pressure: LRU eviction and re-upload keep results. */
    M.device.texture_bytes=MACH64_3D_TEXTURE_MIN;
    close_services();
    services(128,96,true,true,true);
    M.device.texture_bytes=MACH64_3D_TEXTURE_MIN;
    render3d_init(&hw,&host,42,mach64_3d_backend(),&M.device,M.device.heap);
    struct texspec tex[4]={{R3D_ARGB8888,256,128,0},{R3D_RGB565,256,128,0},{R3D_ARGB4444,128,128,0},{R3D_L8,256,64,0}};
    a=make_set(&hw,R3D_HARDWARE_REQUIRED,64,64,0,0); b=make_set(&sw,R3D_SOFTWARE,64,64,0,0);
    uint32_t ha[4],hb[4];
    for (unsigned i=0;i<4;i++) { ha[i]=texture(&hw,a.ctx,&tex[i]); hb[i]=texture(&sw,b.ctx,&tex[i]); }
    for (unsigned i=0;i<12;i++) {
        memset(&q,0,sizeof(q)); geometry(&q,64,64,2,true,false);
        q.flags=R3D_TEXTURED|(i ? 0 : R3D_CLEAR_COLOR); q.state.sampler[0]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,1,1};
        random_uv(&q,false,2);
        a.tex0=ha[i%4]; b.tex0=hb[i%4];
        CHECK(!submit(&hw,&a,&q)); CHECK(!submit(&sw,&b,&q));
    }
    compare(&a,&b,64,64,false,false,false);
    destroy(&a,&b);
    printf("PASS mach64 fallback: unqualified chip software-only, whole-context latch after a refused AUTO state, texture eviction and re-upload\n");
    close_services();
    /* A software work-budget retry must not undo the whole-context latch. */
    services(256,256,true,true,true);
    a=make_set(&hw,R3D_AUTO,256,256,0,0);
    memset(&q,0,sizeof(q)); q.flags=R3D_CLEAR_COLOR; q.count=6;
    q.state.blend=1; q.state.source=q.state.destination=R3D_ONE;
    for (unsigned i=0;i<6;i++) q.vertices[i]=(struct render3d_vertex){i%3==1 ? 256<<16 : 0,
        i%3==2 ? 256<<16 : 0,0,65536,0xffffffffu,0,0};
    draws=M.draws;
    CHECK(submit(&hw,&a,&q)==-EAGAIN && !hw.contexts[0].hardware && M.draws==draws);
    q.count=0; memset(q.vertices,0,sizeof(q.vertices)); memset(&q.state,0,sizeof(q.state));
    CHECK(!submit(&hw,&a,&q) && M.draws==draws);
    CHECK(!render3d_destroy_context(&hw,7,a.ctx)); close_services();
}

int main(int argc,char **argv)
{
    const char *mode=argc>1 ? argv[1] : "differential";
    if (!strcmp(mode,"encoders")) encoders();
    else if (!strcmp(mode,"differential")) differential();
    else if (!strcmp(mode,"qualification")) qualification();
    else if (!strcmp(mode,"fault")) faults(argc>2 ? (unsigned)atoi(argv[2]) : 0);
    else if (!strcmp(mode,"fuzz")) fuzz();
    else if (!strcmp(mode,"fallback")) fallback();
    else { fprintf(stderr,"unknown mode %s\n",mode); return 2; }
    return 0;
}
