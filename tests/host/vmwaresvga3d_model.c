/* Independent SVGA3D host model. It decodes exactly the FIFO words the
 * backend emits (strictly: unknown ids, sizes, sids or out-of-plan DMA abort
 * the test) and renders with long-double D3D9 fixed-function semantics; it
 * never calls the production scalar renderer or the backend encoders.
 * GPL-2.0-only. */
#include "vmwaresvga3d_model.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct vmsvga3d_model svga_model;
volatile uint64_t g_ticks __attribute__((weak));
#define M svga_model
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: model check failed: %s\n",__FILE__,__LINE__,#x); abort(); } } while (0)

static long double f32(uint32_t bits) { float f; memcpy(&f,&bits,4); CHECK(isfinite(f)); return f; }
static bool heap_range(uint32_t first,uint64_t bytes)
{
    const struct render3d_extent *h=&M.device.heap;
    return bytes && first>=h->first+4096u && first+bytes<=(uint64_t)h->first+h->bytes-4096u;
}
static struct svga_model_surface *surface(uint32_t sid)
{ CHECK(sid>=VMSVGA3D_SID_BASE && sid<MODEL_SURFACES && M.surfaces[sid].live); return &M.surfaces[sid]; }
unsigned svga_model_live(void)
{ unsigned n=0; for (unsigned i=0;i<MODEL_SURFACES;i++) n+=M.surfaces[i].live; return n; }

/* ---- commands ---------------------------------------------------------- */
static void define(const uint32_t *b,unsigned n)
{
    CHECK(n>=9); uint32_t sid=b[0],levels=b[3];
    CHECK(sid>=VMSVGA3D_SID_BASE && sid<VMSVGA3D_SID_BASE+VMSVGA3D_SIDS && !M.surfaces[sid].live);
    CHECK(levels>=1 && levels<=11 && n==9+3*levels);
    for (unsigned f=1;f<6;f++) CHECK(!b[3+f]);
    struct svga_model_surface *s=&M.surfaces[sid];
    *s=(struct svga_model_surface){.live=true,.format=b[2],.flags=b[1],.levels=levels};
    if (b[2]==VMSVGA3D_A8R8G8B8) { s->bpp=4; CHECK(b[1]==VMSVGA3D_HINT_TEXTURE || b[1]==VMSVGA3D_HINT_RENDERTARGET); }
    else if (b[2]==VMSVGA3D_Z_D16) { s->bpp=2; CHECK(b[1]==VMSVGA3D_HINT_DEPTHSTENCIL && levels==1); }
    else { CHECK(b[2]==VMSVGA3D_BUFFER && b[1]==VMSVGA3D_HINT_VERTEXBUFFER && levels==1 && b[10]==1); s->bpp=1; }
    for (unsigned l=0;l<levels;l++) {
        s->w[l]=b[9+3*l]; s->h[l]=b[10+3*l]; CHECK(b[11+3*l]==1);
        CHECK(s->w[l] && s->h[l] && (s->format==VMSVGA3D_BUFFER ? s->w[l]<=R3D_VERTICES*36u : s->w[l]<=1024 && s->h[l]<=1024));
        if (l) CHECK(s->w[l]==(s->w[0]>>l ? s->w[0]>>l : 1) && s->h[l]==(s->h[0]>>l ? s->h[0]>>l : 1));
        s->data[l]=calloc(1,(size_t)s->w[l]*s->h[l]*s->bpp); CHECK(s->data[l]);
    }
    M.defines++;
    unsigned live=svga_model_live(); if (live>M.max_live) M.max_live=live;
}
static void destroy(const uint32_t *b,unsigned n)
{
    CHECK(n==1); struct svga_model_surface *s=surface(b[0]);
    CHECK(M.rt_color!=b[0] && M.rt_depth!=b[0] && M.ts[0][VMSVGA3D_TS_BIND_TEXTURE]!=b[0] && M.ts[1][VMSVGA3D_TS_BIND_TEXTURE]!=b[0]);
    for (unsigned l=0;l<s->levels;l++) free(s->data[l]);
    memset(s,0,sizeof(*s)); M.destroys++;
}
static void dma(const uint32_t *b,unsigned n)
{
    CHECK(n==19 && b[0]==SVGA_GMR_FRAMEBUFFER && !b[4] && (b[6]==1 || b[6]==2));
    struct svga_model_surface *s=surface(b[3]); uint32_t level=b[5];
    CHECK(level<s->levels && !b[7] && !b[8] && !b[9] && b[12]==1 && !b[13] && !b[14] && !b[15]);
    uint32_t w=b[10],h=b[11],pitch=b[2],offset=b[1];
    CHECK(w==s->w[level] && h==s->h[level] && pitch==w*s->bpp && b[16]==12 && b[17]==pitch*h && !b[18]);
    CHECK(heap_range(offset,(uint64_t)pitch*h));
    if (b[6]==VMSVGA3D_WRITE_HOST_VRAM) memcpy(s->data[level],M.vram+offset,(size_t)pitch*h);
    else {
        memcpy(M.vram+offset,s->data[level],(size_t)pitch*h);
        if (M.corrupt_guard) M.vram[offset+pitch*h+4096u-1u]^=0x5a; /* into the trailing guard */
    }
    M.dmas++;
}
static void copy(const uint32_t *b,unsigned n)
{
    CHECK(n==15 && !b[1] && !b[2] && !b[4] && !b[5] && !b[6] && !b[7] && !b[8] && b[11]==1 && !b[12] && !b[13] && !b[14]);
    struct svga_model_surface *src=surface(b[0]),*dst=surface(b[3]);
    CHECK(src!=dst && src->format==VMSVGA3D_A8R8G8B8 && dst->format==src->format);
    CHECK(b[9]==src->w[0] && b[10]==src->h[0] && b[9]==dst->w[0] && b[10]==dst->h[0]);
    memcpy(dst->data[0],src->data[0],(size_t)b[9]*b[10]*4); M.copies++;
}
static bool ignored(void) { return M.host==SVGA_HOST_VBOX_DX; }
static void context_scoped(const uint32_t *b) { CHECK(b[0]==VMSVGA3D_CID); if (!ignored()) CHECK(M.context); }
static void render_states(const uint32_t *b,unsigned n)
{
    CHECK(n>=3 && (n-1)%2==0); context_scoped(b);
    static const uint8_t known[]={VMSVGA3D_RS_ZENABLE,VMSVGA3D_RS_ZWRITEENABLE,VMSVGA3D_RS_ALPHATESTENABLE,
        VMSVGA3D_RS_DITHERENABLE,VMSVGA3D_RS_BLENDENABLE,VMSVGA3D_RS_FOGENABLE,VMSVGA3D_RS_SPECULARENABLE,
        VMSVGA3D_RS_STENCILENABLE,VMSVGA3D_RS_LIGHTINGENABLE,VMSVGA3D_RS_SHADEMODE,VMSVGA3D_RS_SRCBLEND,
        VMSVGA3D_RS_DSTBLEND,VMSVGA3D_RS_BLENDEQUATION,VMSVGA3D_RS_CULLMODE,VMSVGA3D_RS_ZFUNC,
        VMSVGA3D_RS_ALPHAFUNC,VMSVGA3D_RS_ALPHAREF,VMSVGA3D_RS_COLORWRITEENABLE,VMSVGA3D_RS_SCISSORTESTENABLE,
        VMSVGA3D_RS_SEPARATEALPHABLENDENABLE};
    for (unsigned i=1;i<n;i+=2) {
        bool ok=false; for (unsigned k=0;k<sizeof(known);k++) ok|=b[i]==known[k];
        CHECK(ok); M.rs[b[i]]=b[i+1];
    }
}
static void texture_states(const uint32_t *b,unsigned n)
{
    CHECK(n>=4 && (n-1)%3==0); context_scoped(b);
    for (unsigned i=1;i<n;i+=3) {
        uint32_t stage=b[i],name=b[i+1],value=b[i+2];
        CHECK(stage<2 && name>=VMSVGA3D_TS_BIND_TEXTURE && name<=VMSVGA3D_TS_TEXCOORDINDEX && name!=13);
        if (name==VMSVGA3D_TS_BIND_TEXTURE && value!=VMSVGA3D_INVALID_ID) CHECK(surface(value)->format==VMSVGA3D_A8R8G8B8);
        M.ts[stage][name]=value;
    }
}
static void render_target(const uint32_t *b,unsigned n)
{
    CHECK(n==5 && !b[3] && !b[4]); context_scoped(b);
    CHECK(b[1]==VMSVGA3D_RT_COLOR0 || b[1]==VMSVGA3D_RT_DEPTH);
    if (b[2]!=VMSVGA3D_INVALID_ID) CHECK(surface(b[2])->format==(b[1]==VMSVGA3D_RT_DEPTH ? VMSVGA3D_Z_D16 : VMSVGA3D_A8R8G8B8));
    if (b[1]==VMSVGA3D_RT_COLOR0) M.rt_color=b[2]; else M.rt_depth=b[2];
}
static void clear(const uint32_t *b,unsigned n)
{
    CHECK(n==9 && (b[1]==1 || b[1]==2 || b[1]==3) && !b[4] && !b[5] && !b[6]); context_scoped(b);
    if (ignored() || M.drop_draws) return;
    if (b[1]&VMSVGA3D_CLEAR_COLOR) {
        struct svga_model_surface *c=surface(M.rt_color); CHECK(b[7]==c->w[0] && b[8]==c->h[0]);
        for (unsigned i=0;i<c->w[0]*c->h[0];i++) memcpy(c->data[0]+i*4,&b[2],4);
    }
    if (b[1]&VMSVGA3D_CLEAR_DEPTH) {
        struct svga_model_surface *z=surface(M.rt_depth); CHECK(b[7]==z->w[0] && b[8]==z->h[0]);
        long double depth=f32(b[3]); CHECK(depth>=0 && depth<=1);
        uint16_t value=(uint16_t)lrintl(depth*65535);
        for (unsigned i=0;i<z->w[0]*z->h[0];i++) memcpy(z->data[0]+i*2,&value,2);
    }
    M.clears++;
}

/* ---- fixed-function rasterizer -------------------------------------- */
struct vtx { long double x,y,z,w,c[4],t[2][2]; };
static bool compare(long double a,long double b,uint32_t f)
{
    switch (f) {
    case VMSVGA3D_CMP_NEVER: return false; case VMSVGA3D_CMP_LESS: return a<b;
    case VMSVGA3D_CMP_EQUAL: return a==b; case VMSVGA3D_CMP_LESSEQUAL: return a<=b;
    case VMSVGA3D_CMP_GREATER: return a>b; case VMSVGA3D_CMP_NOTEQUAL: return a!=b;
    case VMSVGA3D_CMP_GREATEREQUAL: return a>=b; default: CHECK(f==VMSVGA3D_CMP_ALWAYS); return true;
    }
}
static unsigned wrap(int a,unsigned n,uint32_t mode)
{
    if (mode==VMSVGA3D_TEX_ADDRESS_WRAP) return (unsigned)a&(n-1);
    CHECK(mode==VMSVGA3D_TEX_ADDRESS_CLAMP);
    return a<0 ? 0 : (unsigned)a>=n ? n-1 : (unsigned)a;
}
static void texel(const struct svga_model_surface *s,unsigned l,unsigned x,unsigned y,long double out[4])
{
    uint32_t v; memcpy(&v,s->data[l]+((size_t)y*s->w[l]+x)*4,4);
    out[0]=(v>>16&255)/255.0L; out[1]=(v>>8&255)/255.0L; out[2]=(v&255)/255.0L; out[3]=(v>>24)/255.0L;
}
/* Eight fractional filter and LOD bits, like the hardware Q3 measures. */
static void sample_level(unsigned stage,const struct svga_model_surface *s,unsigned l,long double u,long double v,bool linear,long double out[4])
{
    unsigned w=s->w[l],h=s->h[l];
    long double x=u*w-(linear ? .5L : 0),y=v*h-(linear ? .5L : 0);
    int ix=(int)floorl(x),iy=(int)floorl(y);
    uint32_t au=M.ts[stage][VMSVGA3D_TS_ADDRESSU],av=M.ts[stage][VMSVGA3D_TS_ADDRESSV];
    long double a[4]; texel(s,l,wrap(ix,w,au),wrap(iy,h,av),a);
    if (!linear) { memcpy(out,a,sizeof(a)); return; }
    long double fx=floorl((x-ix)*256)/256,fy=floorl((y-iy)*256)/256,b[4],c[4],d[4];
    texel(s,l,wrap(ix+1,w,au),wrap(iy,h,av),b); texel(s,l,wrap(ix,w,au),wrap(iy+1,h,av),c);
    texel(s,l,wrap(ix+1,w,au),wrap(iy+1,h,av),d);
    for (unsigned k=0;k<4;k++) out[k]=(a[k]*(1-fx)+b[k]*fx)*(1-fy)+(c[k]*(1-fx)+d[k]*fx)*fy;
}
static void sample(unsigned stage,long double u,long double v,long double rho,long double out[4])
{
    const struct svga_model_surface *s=surface(M.ts[stage][VMSVGA3D_TS_BIND_TEXTURE]);
    uint32_t mip=M.ts[stage][VMSVGA3D_TS_MIPFILTER],mag=M.ts[stage][VMSVGA3D_TS_MAGFILTER],min=M.ts[stage][VMSVGA3D_TS_MINFILTER];
    CHECK(mag==min && (mag==VMSVGA3D_TEX_FILTER_NEAREST || mag==VMSVGA3D_TEX_FILTER_LINEAR));
    CHECK(mip==VMSVGA3D_TEX_FILTER_NONE || mip==VMSVGA3D_TEX_FILTER_NEAREST || mip==VMSVGA3D_TEX_FILTER_LINEAR);
    bool linear=mag==VMSVGA3D_TEX_FILTER_LINEAR;
    long double lod=mip==VMSVGA3D_TEX_FILTER_NONE || rho<=1 ? 0 : log2l(rho);
    if (lod>s->levels-1) lod=s->levels-1;
    unsigned q=(unsigned)floorl(lod*256);
    unsigned level=mip==VMSVGA3D_TEX_FILTER_LINEAR ? q/256 : (q+128)/256;
    sample_level(stage,s,level,u,v,linear,out);
    if (mip==VMSVGA3D_TEX_FILTER_LINEAR && level+1<s->levels) {
        long double next[4]; sample_level(stage,s,level+1,u,v,linear,next);
        long double f=(q&255)/256.0L;
        for (unsigned k=0;k<4;k++) out[k]=out[k]*(1-f)+next[k]*f;
    }
}
static void arg(uint32_t a,const long double tex[4],const long double diffuse[4],const long double current[4],
                long double out[4])
{
    const long double *src=a==VMSVGA3D_TA_TEXTURE ? tex : a==VMSVGA3D_TA_DIFFUSE ? diffuse : current;
    CHECK(a==VMSVGA3D_TA_TEXTURE || a==VMSVGA3D_TA_DIFFUSE || a==VMSVGA3D_TA_PREVIOUS);
    memcpy(out,src,4*sizeof(*out));
}
static long double op(uint32_t o,long double x,long double y,long double ta)
{
    switch (o) {
    case VMSVGA3D_TC_SELECTARG1: return x;
    case VMSVGA3D_TC_MODULATE: return x*y;
    case VMSVGA3D_TC_ADD: return x+y>1 ? 1 : x+y;
    default: CHECK(o==VMSVGA3D_TC_BLENDTEXTUREALPHA); return x*ta+y*(1-ta);
    }
}
static void combine(unsigned stage,const long double tex[4],const long double diffuse[4],long double current[4])
{
    if (M.host==SVGA_HOST_VBOX_GL) { /* GL_MODULATE per enabled unit */
        for (unsigned k=0;k<4;k++) current[k]*=tex[k];
        return;
    }
    uint32_t *t=M.ts[stage];
    long double a1[4],a2[4],b1[4],b2[4],out[4];
    arg(t[VMSVGA3D_TS_COLORARG1],tex,diffuse,current,a1); arg(t[VMSVGA3D_TS_COLORARG2],tex,diffuse,current,a2);
    arg(t[VMSVGA3D_TS_ALPHAARG1],tex,diffuse,current,b1); arg(t[VMSVGA3D_TS_ALPHAARG2],tex,diffuse,current,b2);
    for (unsigned k=0;k<3;k++) out[k]=op(t[VMSVGA3D_TS_COLOROP],a1[k],a2[k],tex[3]);
    out[3]=op(t[VMSVGA3D_TS_ALPHAOP],b1[3],b2[3],tex[3]);
    memcpy(current,out,sizeof(out));
}
static long double factor(uint32_t f,const long double s[4],const long double d[4],unsigned k)
{
    switch (f) {
    case VMSVGA3D_BLENDOP_ZERO: return 0; case VMSVGA3D_BLENDOP_ONE: return 1;
    case VMSVGA3D_BLENDOP_SRCCOLOR: return s[k]; case VMSVGA3D_BLENDOP_INVSRCCOLOR: return 1-s[k];
    case VMSVGA3D_BLENDOP_SRCALPHA: return s[3]; case VMSVGA3D_BLENDOP_INVSRCALPHA: return 1-s[3];
    case VMSVGA3D_BLENDOP_DESTALPHA: return d[3]; case VMSVGA3D_BLENDOP_INVDESTALPHA: return 1-d[3];
    case VMSVGA3D_BLENDOP_DESTCOLOR: return d[k]; default: CHECK(f==VMSVGA3D_BLENDOP_INVDESTCOLOR); return 1-d[k];
    }
}
static bool stage_on(unsigned stage)
{
    if (M.host==SVGA_HOST_VBOX_GL) return M.ts[stage][VMSVGA3D_TS_BIND_TEXTURE]!=VMSVGA3D_INVALID_ID &&
        (stage==0 || stage_on(0));
    if (M.ts[stage][VMSVGA3D_TS_COLOROP]==VMSVGA3D_TC_DISABLE) { CHECK(M.ts[stage][VMSVGA3D_TS_ALPHAOP]==VMSVGA3D_TC_DISABLE); return false; }
    CHECK(M.ts[stage][VMSVGA3D_TS_BIND_TEXTURE]!=VMSVGA3D_INVALID_ID && M.ts[stage][VMSVGA3D_TS_TEXCOORDINDEX]==stage);
    return stage==0 || stage_on(0);
}
static void transform(const uint32_t *b,unsigned n)
{
    CHECK(n==18 && b[1]>=VMSVGA3D_TRANSFORM_WORLD && b[1]<=VMSVGA3D_TRANSFORM_PROJECTION); context_scoped(b);
    for (unsigned i=0;i<16;i++) M.transforms[b[1]][i]=(float)f32(b[2+i]);
}
static void project(const long double in[4],long double out[4])
{
    long double v[4]; memcpy(v,in,sizeof(v));
    for (unsigned t=VMSVGA3D_TRANSFORM_WORLD;t<=VMSVGA3D_TRANSFORM_PROJECTION;t++) {
        long double r[4]={0};
        for (unsigned j=0;j<4;j++) for (unsigned i=0;i<4;i++) r[j]+=v[i]*M.transforms[t][i*4+j];
        memcpy(v,r,sizeof(v));
    }
    memcpy(out,v,sizeof(v));
}
static void triangle(const struct svga_model_surface *buffer,unsigned first,unsigned decls)
{
    struct svga_model_surface *rt=surface(M.rt_color),*zs=M.rt_depth==VMSVGA3D_INVALID_ID ? 0 : surface(M.rt_depth);
    unsigned W=rt->w[0],H=rt->h[0];
    CHECK(M.viewport[0]==0 && M.viewport[1]==0 && M.viewport[2]==W && M.viewport[3]==H && M.zrange[0]==0 && M.zrange[1]==1);
    if (zs) CHECK(zs->w[0]==W && zs->h[0]==H);
    struct vtx v[3];
    for (unsigned i=0;i<3;i++) {
        const uint8_t *p=buffer->data[0]+(size_t)(first+i)*36; uint32_t word[9]; memcpy(word,p,36);
        long double in[4]={f32(word[0]),f32(word[1]),f32(word[2]),f32(word[3])},clip[4];
        project(in,clip); CHECK(clip[3]>0);
        long double ndc[3]={clip[0]/clip[3],clip[1]/clip[3],clip[2]/clip[3]};
        /* D3D9 viewport: pixel centres on integer screen coordinates. */
        v[i].x=(ndc[0]+1)*W/2; v[i].y=(1-ndc[1])*H/2; v[i].z=ndc[2]; v[i].w=clip[3];
        /* Rasterizers snap to a sub-pixel grid (8 bits here); float32
         * transform error far below half a step leaves ties exact. */
        v[i].x=roundl(v[i].x*256)/256; v[i].y=roundl(v[i].y*256)/256;
        CHECK(v[i].z>=-1e-6L && v[i].z<=1+1e-6L);
        for (unsigned k=0;k<4;k++) v[i].c[k]=((word[4]>>(k==3 ? 24 : 16-8*k))&255)/255.0L;
        for (unsigned t=0;t<2;t++) for (unsigned a=0;a<2;a++) v[i].t[t][a]=decls>2+t ? f32(word[5+t*2+a]) : 0;
    }
    long double area=(v[1].x-v[0].x)*(v[2].y-v[0].y)-(v[1].y-v[0].y)*(v[2].x-v[0].x);
    M.draws++;
    if (area==0 || M.drop_draws || ignored()) return;
    if (area<0) { struct vtx t=v[1]; v[1]=v[2]; v[2]=t; area=-area; }
    int x0=(int)floorl(fminl(v[0].x,fminl(v[1].x,v[2].x))),x1=(int)ceill(fmaxl(v[0].x,fmaxl(v[1].x,v[2].x)));
    int y0=(int)floorl(fminl(v[0].y,fminl(v[1].y,v[2].y))),y1=(int)ceill(fmaxl(v[0].y,fmaxl(v[1].y,v[2].y)));
    if (x0<0) x0=0;
    if (y0<0) y0=0;
    if (x1>(int)W-1) x1=(int)W-1;
    if (y1>(int)H-1) y1=(int)H-1;
    for (int py=y0;py<=y1;py++) for (int px=x0;px<=x1;px++) {
        long double l[3]; bool inside=true;
        for (unsigned i=0;i<3;i++) {
            const struct vtx *a=&v[(i+1)%3],*b=&v[(i+2)%3];
            l[i]=(b->x-a->x)*(py-a->y)-(b->y-a->y)*(px-a->x);
            /* D3D9 top-left fill convention. */
            bool top_left=b->y<a->y || (b->y==a->y && b->x>a->x);
            if (l[i]<0 || (l[i]==0 && !top_left)) inside=false;
        }
        if (!inside) continue;
        long double z=(l[0]*v[0].z+l[1]*v[1].z+l[2]*v[2].z)/area;
        uint16_t depth=(uint16_t)lrintl(fminl(fmaxl(z,0),1)*65535);
        bool zenable=M.rs[VMSVGA3D_RS_ZENABLE]!=0;
        uint16_t stored=0;
        if (zenable) { CHECK(zs); memcpy(&stored,zs->data[0]+((size_t)py*W+px)*2,2); }
        if (zenable && !compare(depth,stored,M.rs[VMSVGA3D_RS_ZFUNC])) continue;
        /* Perspective-correct attributes with the clip-space W. */
        long double q[3],qs=0;
        for (unsigned i=0;i<3;i++) { q[i]=l[i]/v[i].w; qs+=q[i]; }
        long double diffuse[4],current[4];
        for (unsigned k=0;k<4;k++) { diffuse[k]=0; for (unsigned i=0;i<3;i++) diffuse[k]+=q[i]*v[i].c[k]; diffuse[k]/=qs; }
        memcpy(current,diffuse,sizeof(current));
        for (unsigned stage=0;stage<2;stage++) {
            if (!stage_on(stage)) break;
            unsigned set=M.ts[stage][VMSVGA3D_TS_TEXCOORDINDEX];
            CHECK(set<2 && decls>2+set);
            long double uv[2]={0,0},rho=0;
            for (unsigned a=0;a<2;a++) { for (unsigned i=0;i<3;i++) uv[a]+=q[i]*v[i].t[set][a]; uv[a]/=qs; }
            const struct svga_model_surface *tex=surface(M.ts[stage][VMSVGA3D_TS_BIND_TEXTURE]);
            for (unsigned axis=0;axis<2;axis++) {
                /* One-pixel neighbour along the axis, same plane equations. */
                long double nq[3],ns=0;
                for (unsigned i=0;i<3;i++) {
                    const struct vtx *a=&v[(i+1)%3],*b=&v[(i+2)%3];
                    long double nl=(b->x-a->x)*(py+axis-a->y)-(b->y-a->y)*(px+!axis-a->x);
                    nq[i]=nl/v[i].w; ns+=nq[i];
                }
                for (unsigned a=0;a<2;a++) {
                    long double n=0; for (unsigned i=0;i<3;i++) n+=nq[i]*v[i].t[set][a];
                    long double footprint=fabsl(n/ns-uv[a])*(a ? tex->h[0] : tex->w[0]);
                    if (footprint>rho) rho=footprint;
                }
            }
            long double t[4]; sample(stage,uv[0],uv[1],rho,t);
            combine(stage,t,diffuse,current);
        }
        for (unsigned k=0;k<4;k++) current[k]=fminl(fmaxl(current[k],0),1);
        if (M.rs[VMSVGA3D_RS_ALPHATESTENABLE] &&
            !compare((float)(lrintl(current[3]*255)/255.0L),f32(M.rs[VMSVGA3D_RS_ALPHAREF]),M.rs[VMSVGA3D_RS_ALPHAFUNC])) continue;
        uint8_t *dst=rt->data[0]+((size_t)py*W+px)*4; uint32_t old; memcpy(&old,dst,4);
        long double d[4]={(old>>16&255)/255.0L,(old>>8&255)/255.0L,(old&255)/255.0L,(old>>24)/255.0L},out[4];
        for (unsigned k=0;k<4;k++) {
            out[k]=current[k];
            if (M.rs[VMSVGA3D_RS_BLENDENABLE]) {
                CHECK(M.rs[VMSVGA3D_RS_BLENDEQUATION]==VMSVGA3D_BLENDEQ_ADD && !M.rs[VMSVGA3D_RS_SEPARATEALPHABLENDENABLE]);
                out[k]=current[k]*factor(M.rs[VMSVGA3D_RS_SRCBLEND],current,d,k)+d[k]*factor(M.rs[VMSVGA3D_RS_DSTBLEND],current,d,k);
                out[k]=fminl(out[k],1);
            }
        }
        uint32_t value=((uint32_t)lrintl(out[3]*255)<<24)|((uint32_t)lrintl(out[0]*255)<<16)|
            ((uint32_t)lrintl(out[1]*255)<<8)|(uint32_t)lrintl(out[2]*255);
        CHECK(M.rs[VMSVGA3D_RS_COLORWRITEENABLE]==15);
        memcpy(dst,&value,4);
        if (zenable && M.rs[VMSVGA3D_RS_ZWRITEENABLE]) memcpy(zs->data[0]+((size_t)py*W+px)*2,&depth,2);
        M.fragments++;
    }
}
static void draw(const uint32_t *b,unsigned n)
{
    CHECK(n>=3); context_scoped(b);
    unsigned decls=b[1];
    CHECK(decls>=2 && decls<=4 && b[2]==1 && n==3+9*decls+7);
    static const uint32_t expect[4][4]={{VMSVGA3D_DECLTYPE_FLOAT4,VMSVGA3D_DECLUSAGE_POSITION,0,0},
        {VMSVGA3D_DECLTYPE_D3DCOLOR,VMSVGA3D_DECLUSAGE_COLOR,0,16},{VMSVGA3D_DECLTYPE_FLOAT2,VMSVGA3D_DECLUSAGE_TEXCOORD,0,20},
        {VMSVGA3D_DECLTYPE_FLOAT2,VMSVGA3D_DECLUSAGE_TEXCOORD,1,28}};
    uint32_t sid=b[3+4];
    for (unsigned i=0;i<decls;i++) {
        const uint32_t *d=b+3+9*i;
        CHECK(d[0]==expect[i][0] && d[1]==VMSVGA3D_DECLMETHOD_DEFAULT && d[2]==expect[i][1] && d[3]==expect[i][2]);
        CHECK(d[4]==sid && d[5]==expect[i][3] && d[6]==36 && !d[7] && !d[8]);
    }
    const uint32_t *r=b+3+9*decls;
    CHECK(r[0]==VMSVGA3D_PRIMITIVE_TRIANGLELIST && r[1] && r[2]==VMSVGA3D_INVALID_ID && !r[3] && !r[4] && !r[5] && !r[6]);
    const struct svga_model_surface *buffer=surface(sid);
    CHECK(buffer->format==VMSVGA3D_BUFFER && (uint64_t)r[1]*3*36<=buffer->w[0]);
    if (ignored()) return;
    CHECK(M.rs[VMSVGA3D_RS_LIGHTINGENABLE]==0 && M.rs[VMSVGA3D_RS_CULLMODE]==VMSVGA3D_FACE_NONE &&
          M.rs[VMSVGA3D_RS_SHADEMODE]==VMSVGA3D_SHADEMODE_SMOOTH && !M.rs[VMSVGA3D_RS_FOGENABLE] &&
          !M.rs[VMSVGA3D_RS_STENCILENABLE] && !M.rs[VMSVGA3D_RS_DITHERENABLE] && !M.rs[VMSVGA3D_RS_SCISSORTESTENABLE]);
    for (unsigned t=0;t<r[1];t++) triangle(buffer,t*3,decls);
}

/* ---- FIFO ---------------------------------------------------------------- */
static void process(void)
{
    uint32_t at=M.fifo[SVGA_FIFO_STOP],next=M.fifo[SVGA_FIFO_NEXT_CMD],min=M.fifo[SVGA_FIFO_MIN],max=M.fifo[SVGA_FIFO_MAX];
    static uint32_t words[MODEL_FIFO_BYTES/4];
    unsigned count=0;
    while (at!=next) {
        CHECK(at>=min && at<max && !(at&3) && count<MODEL_FIFO_BYTES/4);
        words[count++]=M.fifo[at/4]; at+=4;
        if (at==max) at=min;
    }
    if (!count) return;
    M.batches++;
    uint64_t defined_before=M.defines;
    for (unsigned i=0;i<count;) {
        uint32_t id=words[i];
        if (id==SVGA_CMD_FENCE) {
            CHECK(i+2<=count); M.fifo[SVGA_FIFO_FENCE]=M.wrong_fence ? words[i+1]-1 : words[i+1];
            M.fences++; i+=2; continue;
        }
        CHECK(id>=VMSVGA3D_CMD_SURFACE_DEFINE && id<=VMSVGA3D_CMD_DRAW_PRIMITIVES && i+2<=count);
        uint32_t bytes=words[i+1]; CHECK(!(bytes&3) && i+2+bytes/4<=count);
        const uint32_t *b=words+i+2; unsigned n=bytes/4;
        M.commands++;
        switch (id) {
        case VMSVGA3D_CMD_SURFACE_DEFINE: define(b,n); break;
        case VMSVGA3D_CMD_SURFACE_DESTROY: destroy(b,n); break;
        case VMSVGA3D_CMD_SURFACE_COPY: copy(b,n); break;
        case VMSVGA3D_CMD_SURFACE_DMA: dma(b,n); break;
        case VMSVGA3D_CMD_CONTEXT_DEFINE:
            CHECK(n==1 && b[0]==VMSVGA3D_CID); M.context_defines++;
            if (!ignored()) { CHECK(!M.context); M.context=true; }
            break;
        case VMSVGA3D_CMD_CONTEXT_DESTROY:
            CHECK(n==1 && b[0]==VMSVGA3D_CID); M.context_destroys++;
            if (!ignored()) { CHECK(M.context); M.context=false; }
            if (M.fail_context_destroy) M.wrong_fence=true;
            break;
        case VMSVGA3D_CMD_SETTRANSFORM: transform(b,n); break;
        case VMSVGA3D_CMD_SETZRANGE: CHECK(n==3); context_scoped(b); M.zrange[0]=(float)f32(b[1]); M.zrange[1]=(float)f32(b[2]); break;
        case VMSVGA3D_CMD_SETRENDERSTATE: render_states(b,n); break;
        case VMSVGA3D_CMD_SETRENDERTARGET: render_target(b,n); break;
        case VMSVGA3D_CMD_SETTEXTURESTATE: texture_states(b,n); break;
        case VMSVGA3D_CMD_SETVIEWPORT: CHECK(n==5); context_scoped(b); memcpy(M.viewport,b+1,16); break;
        case VMSVGA3D_CMD_CLEAR: clear(b,n); break;
        case VMSVGA3D_CMD_DRAW_PRIMITIVES: draw(b,n); break;
        default: CHECK(0);
        }
        i+=2+n;
    }
    M.live_after_batch=svga_model_live();
    if (M.defines!=defined_before) {
        /* Independent D-F2 oracle: every job, including numeric refusals,
         * ends with unbound surfaces and the shadowed baseline restored. */
        CHECK(!M.live_after_batch && M.rt_color==VMSVGA3D_INVALID_ID && M.rt_depth==VMSVGA3D_INVALID_ID);
        CHECK(!M.rs[VMSVGA3D_RS_ZENABLE] && !M.rs[VMSVGA3D_RS_ZWRITEENABLE] &&
              !M.rs[VMSVGA3D_RS_ALPHATESTENABLE] && !M.rs[VMSVGA3D_RS_BLENDENABLE]);
        CHECK(M.viewport[0]==0 && M.viewport[1]==0 && M.viewport[2]==M.fb.width && M.viewport[3]==M.fb.height);
        for (unsigned t=1;t<=3;t++) for (unsigned i=0;i<16;i++)
            CHECK(M.transforms[t][i]==(i%5==0 ? 1.0f : 0.0f));
        for (unsigned stage=0;stage<2;stage++) {
            CHECK(M.ts[stage][VMSVGA3D_TS_BIND_TEXTURE]==VMSVGA3D_INVALID_ID);
            CHECK(M.ts[stage][VMSVGA3D_TS_COLOROP]==VMSVGA3D_TC_DISABLE &&
                  M.ts[stage][VMSVGA3D_TS_ALPHAOP]==VMSVGA3D_TC_DISABLE);
            CHECK(M.ts[stage][VMSVGA3D_TS_MINFILTER]==VMSVGA3D_TEX_FILTER_NEAREST &&
                  M.ts[stage][VMSVGA3D_TS_MAGFILTER]==VMSVGA3D_TEX_FILTER_NEAREST &&
                  M.ts[stage][VMSVGA3D_TS_MIPFILTER]==VMSVGA3D_TEX_FILTER_NONE);
        }
    }
}
static uint32_t devcap_value(uint32_t);
static uint32_t reg_read(void *ctx,unsigned r)
{
    (void)ctx; CHECK(r<64);
    if (M.revoked) return UINT32_MAX;
    if (r==VMSVGA3D_REG_DEV_CAP) { CHECK(M.gb_objects && M.locks); M.dev_cap_reads++; return devcap_value(M.dev_cap_index); }
    return M.regs[r];
}
static void reg_write(void *ctx,unsigned r,uint32_t v)
{
    (void)ctx; CHECK(r==SVGA_REG_CONFIG_DONE || r==SVGA_REG_ENABLE || r==SVGA_REG_SYNC || r==VMSVGA3D_REG_DEV_CAP);
    M.register_writes++;
    if (r==VMSVGA3D_REG_DEV_CAP) { CHECK(M.gb_objects && M.locks); M.dev_cap_index=v; return; }
    if (r==SVGA_REG_ENABLE) CHECK(v==SVGA_REG_ENABLE_ENABLE);
    M.regs[r]=v;
    if (r==SVGA_REG_SYNC && M.regs[SVGA_REG_CONFIG_DONE]) {
        if (M.hang) { M.regs[SVGA_REG_BUSY]=1; return; }
        process(); M.fifo[SVGA_FIFO_STOP]=M.fifo[SVGA_FIFO_NEXT_CMD]; M.regs[SVGA_REG_BUSY]=0;
    }
}
static uint32_t fifo_read(void *ctx,unsigned off) { (void)ctx; CHECK(!(off&3) && off<MODEL_FIFO_BYTES); return M.fifo[off/4]; }
static void fifo_write(void *ctx,unsigned off,uint32_t v)
{
    (void)ctx; CHECK(!(off&3) && off<MODEL_FIFO_BYTES);
    bool reg=off==4*SVGA_FIFO_MIN || off==4*SVGA_FIFO_MAX || off==4*SVGA_FIFO_NEXT_CMD ||
        off==4*SVGA_FIFO_STOP || off==4*SVGA_FIFO_RESERVED;
    CHECK(reg || (M.fifo[SVGA_FIFO_MIN] && off>=M.fifo[SVGA_FIFO_MIN]));
    M.fifo_writes++; M.fifo[off/4]=v;
}
static void drain(void *ctx) { (void)ctx; }
uint64_t svga_model_now(void *ctx) { (void)ctx; return M.stuck_clock ? M.clock : M.clock++; }
static int delay(void *ctx,unsigned us) { (void)ctx; M.delay_calls++; if (!M.stuck_clock) M.clock+=us; return 0; }
static bool valid(void *ctx) { (void)ctx; return !M.revoked; }
static uint32_t mem_read(void *ctx,uint32_t at,unsigned n)
{
    (void)ctx; CHECK(M.locks==1 && (n==2 || n==4) && at>=M.device.heap.first && at+n<=M.device.heap.first+M.device.heap.bytes);
    uint32_t v=0; memcpy(&v,M.vram+at,n); return v;
}
static void mem_write(void *ctx,uint32_t at,uint32_t v,unsigned n)
{
    (void)ctx; CHECK(M.locks==1 && (n==2 || n==4) && at>=M.device.heap.first && at+n<=M.device.heap.first+M.device.heap.bytes);
    memcpy(M.vram+at,&v,n); M.memory_writes++;
}
static int lock(void *ctx,uint32_t generation) { (void)ctx; CHECK(generation==42 && !M.locks); M.locks=1; return 0; }
static void unlock(void *ctx) { (void)ctx; CHECK(M.locks==1); M.locks=0; }
void *svga_model_alloc(void *ctx,size_t n) { (void)ctx; CHECK(!M.locks); return calloc(1,n); }
void svga_model_free(void *ctx,void *p) { (void)ctx; CHECK(!M.locks); free(p); }

static uint32_t devcap_value(uint32_t i)
{
    switch (i) {
    case VMSVGA3D_DEVCAP_3D: return 1;
    case VMSVGA3D_DEVCAP_MAX_TEXTURES: return M.max_textures;
    case VMSVGA3D_DEVCAP_D16_BUFFER_FORMAT: return 1;
    case VMSVGA3D_DEVCAP_MAX_TEXTURE_WIDTH: case VMSVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT: return M.max_texture_size;
    case VMSVGA3D_DEVCAP_MAX_PRIMITIVE_COUNT: case VMSVGA3D_DEVCAP_MAX_VERTEX_INDEX: return 0xfffff;
    case VMSVGA3D_DEVCAP_TEXTURE_OPS: return M.texture_ops;
    case VMSVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8: return VMSVGA3D_FORMAT_OP_TEXTURE|VMSVGA3D_FORMAT_OP_OFFSCREEN_RENDERTARGET;
    case VMSVGA3D_DEVCAP_SURFACEFMT_Z_D16: return VMSVGA3D_FORMAT_OP_ZSTENCIL;
    case VMSVGA3D_DEVCAP_MAX_CONTEXT_IDS: return 256;
    case VMSVGA3D_DEVCAP_MAX_SURFACE_IDS: return 32768;
    default: return 0;
    }
}
static void caps_block(void)
{
    uint32_t *f=M.fifo;
    memset(f+SVGA_FIFO_3D_CAPS,0,(SVGA_FIFO_3D_CAPS_LAST-SVGA_FIFO_3D_CAPS+1)*4);
    f[SVGA_FIFO_3D_HWVERSION]=f[SVGA_FIFO_3D_HWVERSION_REVISED]=M.gb_objects ? 0 : M.hwversion;
    if (M.host==SVGA_HOST_QEMU || M.gb_objects) return;
    uint32_t *r=f+SVGA_FIFO_3D_CAPS; unsigned n=0;
    r[n++]=2+2*84; r[n++]=VMSVGA3D_CAPS_RECORD_DEVCAPS_MIN;
    for (unsigned i=0;i<84;i++) { r[n++]=i; r[n++]=devcap_value(i); }
    r[n]=0;
    if (M.bad_caps) r[0]=400; /* runs past SVGA_FIFO_3D_CAPS_LAST */
}
int svga_model_engine(void)
{
    struct vmsvga_io io={0,reg_read,reg_write,fifo_read,fifo_write,drain,svga_model_now,delay,valid};
    memset(M.regs,0,sizeof(M.regs)); memset(M.fifo,0,sizeof(M.fifo));
    M.regs[SVGA_REG_ID]=SVGA_ID_2; M.regs[SVGA_REG_ENABLE]=SVGA_REG_ENABLE_ENABLE;
    M.regs[SVGA_REG_WIDTH]=M.fb.width; M.regs[SVGA_REG_HEIGHT]=M.fb.height;
    M.regs[SVGA_REG_BITS_PER_PIXEL]=32; M.regs[SVGA_REG_BYTES_PER_LINE]=M.fb.pitch;
    M.regs[SVGA_REG_FB_START]=M.fb.phys; M.regs[SVGA_REG_FB_SIZE]=MODEL_VRAM_BYTES;
    M.regs[SVGA_REG_VRAM_SIZE]=MODEL_VRAM_BYTES;
    M.regs[SVGA_REG_RED_MASK]=0xff0000; M.regs[SVGA_REG_GREEN_MASK]=0xff00; M.regs[SVGA_REG_BLUE_MASK]=0xff;
    M.regs[SVGA_REG_CAPABILITIES]=M.caps_override ? M.caps_override :
        VMSVGA_CAP_FILL|SVGA_CAP_RECT_COPY|SVGA_CAP_EXTENDED_FIFO|(M.host==SVGA_HOST_QEMU ? 0 : SVGA_CAP_3D)|
        (M.gb_objects ? VMSVGA3D_CAP_GBOBJECTS : 0);
    M.regs[SVGA_REG_MEM_START]=0xe8000000u; M.regs[SVGA_REG_MEM_SIZE]=MODEL_FIFO_BYTES;
    M.regs[SVGA_REG_MEM_REGS]=SVGA_FIFO_NUM_REGS;
    M.fifo[SVGA_FIFO_CAPABILITIES]=SVGA_FIFO_CAP_FENCE|SVGA_FIFO_CAP_RESERVE|SVGA_FIFO_CAP_3D_HWVERSION_REVISED;
    caps_block();
    return vmsvga_engine_init(&M.engine,&io,&M.fb,0xe8000000u,MODEL_FIFO_BYTES);
}
void svga_model_init(unsigned w,unsigned h,enum svga_host host)
{
    for (unsigned i=0;i<MODEL_SURFACES;i++) for (unsigned l=0;l<11;l++) free(M.surfaces[i].data[l]);
    free(M.vram);
    memset(&M,0,sizeof(M));
    M.host=host; M.hwversion=VMSVGA3D_HWVERSION_WS8_B1; M.max_textures=8; M.max_texture_size=4096;
    M.texture_ops=host==SVGA_HOST_VMWARE ? (1u<<26)-1u : 0;
    M.vram=calloc(1,MODEL_VRAM_BYTES); CHECK(M.vram);
    M.fb=(struct fb_device){.width=w,.height=h,.pitch=w*4,.size=w*h*4,.phys=MODEL_FB_PHYS,.mapped=M.vram,.bpp=32,
        .present=true,.generation=42,.red_size=8,.red_pos=16,.green_size=8,.green_pos=8,.blue_size=8};
    CHECK(!svga_model_engine());
    struct render3d_extent heap; uint32_t required=0;
    const char *reason=0; struct vmsvga3d_caps caps;
    int caps_error=vmsvga3d_read_caps(&M.engine,&caps,&reason);
    CHECK(!vmsvga3d_mode_heap(&M.engine,&M.fb,MODEL_VRAM_BYTES,&heap,&required));
    M.device=(struct vmsvga3d_device){.engine=&M.engine,.heap=heap,.caps=caps,.generation=42,
        .texture_bytes=(heap.bytes-(required-VMSVGA3D_TEXTURE_MIN))&~4095u,.mode_width=w,.mode_height=h,
        .memory={0,mem_read,mem_write,lock,unlock}};
    CHECK(host==SVGA_HOST_QEMU ? caps_error==-EOPNOTSUPP : !caps_error);
    for (unsigned i=0;i<4;i++) for (unsigned j=0;j<16;j++) M.transforms[i][j]=0;
    M.rt_color=M.rt_depth=VMSVGA3D_INVALID_ID;
    M.ts[0][VMSVGA3D_TS_BIND_TEXTURE]=M.ts[1][VMSVGA3D_TS_BIND_TEXTURE]=VMSVGA3D_INVALID_ID;
}
void svga_model_destroy(void)
{
    CHECK(!M.locks);
    for (unsigned i=0;i<MODEL_SURFACES;i++) for (unsigned l=0;l<11;l++) { free(M.surfaces[i].data[l]); M.surfaces[i].data[l]=0; }
    free(M.vram); M.vram=0;
}
static const struct render3d_host host={0,svga_model_alloc,svga_model_free,svga_model_now,0};
void svga_model_resources(struct render3d_service *s,unsigned w,unsigned h,bool hw,bool full)
{
    render3d_init(s,&host,42,0,0,(struct render3d_extent){0}); unsigned c,id;
    CHECK(!render3d_create_context(s,7,R3D_SOFTWARE,&c));
    const unsigned kinds[]={R3D_COLOR,R3D_D16,R3D_TEXTURE,R3D_LINEAR_COLOR,R3D_TEXTURE};
    uint32_t place=M.device.heap.first+VMSVGA3D_SCRATCH_BYTES,tex=M.device.heap.first+M.device.heap.bytes-M.device.texture_bytes;
    for (unsigned i=0;i<(full ? 5u : 4u);i++) {
        if (kinds[i]==R3D_TEXTURE) {
            unsigned n=full ? (i==2 ? 256 : 128) : 32,hh=full ? n : 16,levels=full ? (i==2 ? 9 : 8) : 1;
            uint32_t *pixels=calloc(1,(size_t)n*hh*8); CHECK(pixels);
            struct render3d_texture_desc t={.format=R3D_ARGB8888,.width=n,.height=hh,.levels=levels};
            unsigned offset=0;
            for (unsigned l=0,ww=n,hg=hh;l<levels;l++,ww=ww>1 ? ww/2 : 1,hg=hg>1 ? hg/2 : 1) {
                for (unsigned y=0;y<hg;y++) for (unsigned x=0;x<ww;x++)
                    pixels[offset+y*ww+x]=full ? 0xff000000u|((96+x*32/ww+l*4)<<16)|((112+y*32/hg+l*4)<<8)|(128+l*4+i) :
                        x<16 ? 0xff4080c0u : 0xffc04080u;
                t.image[l]=(struct render3d_texture_image){pixels+offset,ww*hg*4,ww*4}; offset+=ww*hg;
            }
            CHECK(!render3d_create_texture(s,7,c,&t,&id)); free(pixels);
        } else CHECK(!render3d_create_resource(s,7,c,kinds[i],w,h,0,0,&id));
        struct render3d_resource *r=&s->contexts[0].resources[i];
        if (hw) {
            r->hardware=true; r->offset=kinds[i]==R3D_TEXTURE ? tex : place;
            CHECK(!vmsvga3d_backend()->layout(&M.device,r));
            if (kinds[i]==R3D_TEXTURE) tex+=r->reserved; else place+=r->reserved;
        }
    }
}
