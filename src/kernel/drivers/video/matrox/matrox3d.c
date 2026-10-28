/* Host-programmed trapezoid backend for the G200/G400 family. Every register
 * and format is cited in docs/design/f7-family-matrox-g.md; nothing here is
 * silicon evidence. All writes go through the 2D owner's bounded bus FIFO.
 * No floating point. SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/display_driver.h>
#include "matrox3d.h"

/* ---- fixed-point helpers ------------------------------------------------ */
static int64_t rdiv(int64_t n,int64_t d)
{
    /* Round to nearest, ties away from zero; d is never zero here. */
    if (d<0) { n=-n; d=-d; }
    return n>=0 ? (n+d/2)/d : -((-n+d/2)/d);
}
static int64_t sat_mul(int64_t a,int64_t b)
{
    const int64_t limit=(int64_t)1<<62;
    if (!a || !b) return 0;
    int64_t aa=a<0 ? -a : a,bb=b<0 ? -b : b;
    if (aa>limit/bb) return limit;
    return a*b;
}
static bool fits(int64_t v,unsigned bits)
{ return v>=-((int64_t)1<<(bits-1)) && v<((int64_t)1<<(bits-1)); }
static uint32_t field(int64_t v) { return (uint32_t)(int32_t)v; }
static unsigned log2_of(unsigned n) { unsigned l=0; while ((1u<<l)<n) l++; return l; }
/* Rows whose centre y+0.5 lies at or below a 1/16-unit coordinate. */
static int32_t first_row(int32_t yq) { return (yq+7)>>4; }

/* ---- per-triangle setup --------------------------------------------------
 * Vertex positions are 1/16 pixel (Q4); attributes use the register formats
 * of the design table. Gradients are exact rationals over the Q4 area. */
struct plane { int64_t v[3]; };
struct setup_vertex { int32_t x,y; };
static int64_t gradient(const struct plane *p,const struct setup_vertex v[3],int64_t area,bool along_y)
{
    int64_t d1=p->v[1]-p->v[0],d2=p->v[2]-p->v[0];
    int64_t num=along_y ? d2*(v[1].x-v[0].x)-d1*(v[2].x-v[0].x) : d1*(v[2].y-v[0].y)-d2*(v[1].y-v[0].y);
    return rdiv(num*16,area); /* per pixel; retain precision until final encoding */
}
static int64_t evaluate(const struct plane *p,const struct setup_vertex v[3],int64_t gx,int64_t gy,int64_t xq,int64_t yq)
{ return p->v[0]+rdiv((xq-v[0].x)*gx+(yq-v[0].y)*gy,16); }
struct setup_planes { struct plane z,c[4],s[2][3]; int64_t gx[12],gy[12]; };
static int plane_value(struct setup_planes *sp,unsigned index,struct plane **out)
{
    struct plane *list[]={&sp->z,&sp->c[0],&sp->c[1],&sp->c[2],&sp->c[3],
        &sp->s[0][0],&sp->s[0][1],&sp->s[0][2],&sp->s[1][0],&sp->s[1][1],&sp->s[1][2]};
    if (index>=11) return -EINVAL;
    *out=list[index]; return 0;
}
static int encode_start(const struct setup_planes *sp,unsigned index,const struct setup_vertex v[3],
                        unsigned encoding,int64_t xq,int64_t yq,int64_t *start,int64_t *ix,int64_t *iy)
{
    struct plane *p; if (plane_value((struct setup_planes *)sp,index,&p)) return -EINVAL;
    int64_t gx=sp->gx[index],gy=sp->gy[index];
    if (!encoding) { *start=rdiv(evaluate(p,v,gx,gy,xq,yq),16); *ix=rdiv(gx,16); *iy=rdiv(gy,16); }
    else { *start=evaluate(p,v,gx,gy,xq,yq); *ix=gx; *iy=gy; }
    return 0;
}
int matrox3d_setup(const struct render3d_request *q,unsigned first,unsigned encoding,
                   struct matrox3d_trap out[2],unsigned *count)
{
    if (!q || !out || !count || encoding>=MATROX3D_ENCODINGS || q->count>R3D_VERTICES || first>q->count || q->count-first<3) return -EINVAL;
    *count=0;
    struct setup_vertex v[3]; unsigned order[3]={first,first+1,first+2};
    for (unsigned i=0;i<3;i++) {
        const struct render3d_vertex *a=&q->vertices[first+i];
        if (a->x<0 || a->y<0 || (a->x&4095) || (a->y&4095) || a->x>(1024<<16) || a->y>(1024<<16) ||
            a->z>65535 || a->rhw<256 || a->rhw>16777216) return -EOPNOTSUPP;
    }
    /* Sort by y, then x, keeping the request indices for the attributes. */
    for (unsigned i=0;i<3;i++) for (unsigned j=i+1;j<3;j++) {
        const struct render3d_vertex *a=&q->vertices[order[i]],*b=&q->vertices[order[j]];
        if (b->y<a->y || (b->y==a->y && b->x<a->x)) { unsigned t=order[i]; order[i]=order[j]; order[j]=t; }
    }
    for (unsigned i=0;i<3;i++) { v[i].x=q->vertices[order[i]].x>>12; v[i].y=q->vertices[order[i]].y>>12; }
    int64_t area=(int64_t)(v[1].x-v[0].x)*(v[2].y-v[0].y)-(int64_t)(v[2].x-v[0].x)*(v[1].y-v[0].y);
    if (!area) return 0;
    uint32_t rhw_max=0;
    for (unsigned i=0;i<3;i++) if (q->vertices[order[i]].rhw>rhw_max) rhw_max=q->vertices[order[i]].rhw;
    struct setup_planes sp;
    for (unsigned i=0;i<3;i++) {
        const struct render3d_vertex *a=&q->vertices[order[i]];
        uint32_t colour=q->state.flat ? q->flat_color[first/3] : a->color;
        sp.z.v[i]=(int64_t)a->z<<15;
        for (unsigned c=0;c<4;c++) sp.c[c].v[i]=(int64_t)((colour>>((c<3 ? 2-c : c)*8))&255)<<15;
        /* q' = rhw/max scaled 2^28 (16.16 register, integer <= 4096);
         * s/w = u*q' scaled 2^26 (12.20 register, integer <= 1024). */
        int64_t qs=rdiv((int64_t)a->rhw<<28,rhw_max);
        for (unsigned stage=0;stage<2;stage++) {
            int64_t u=stage ? q->uv1[order[i]][0] : a->u,t=stage ? q->uv1[order[i]][1] : a->v;
            if (u < -16*65536 || u>16*65536 || t < -16*65536 || t>16*65536) return -EINVAL;
            sp.s[stage][0].v[i]=rdiv(u*qs,1<<18);
            sp.s[stage][1].v[i]=rdiv(t*qs,1<<18);
            sp.s[stage][2].v[i]=qs;
        }
        if (rhw_max>16u*a->rhw) return -EOPNOTSUPP; /* 16.16 q/w keeps >= 12 fraction bits */
    }
    for (unsigned k=0;k<11;k++) {
        struct plane *p; (void)plane_value(&sp,k,&p);
        sp.gx[k]=gradient(p,v,area,false); sp.gy[k]=gradient(p,v,area,true);
        if (!fits(sp.gx[k],32) || !fits(sp.gy[k],32)) return -EOPNOTSUPP;
    }
    /* Left/right assignment of the broken vertex against the long edge. */
    int64_t cross=(int64_t)(v[1].x-v[0].x)*(v[2].y-v[0].y)-(int64_t)(v[1].y-v[0].y)*(v[2].x-v[0].x);
    bool broken_left=cross<0;
    int32_t rows_top=first_row(v[1].y)-first_row(v[0].y),rows_bottom=first_row(v[2].y)-first_row(v[1].y);
    /* Restarting the long edge at a rounded split vertex can change exact
     * coverage. With no top row there is no preserved DDA state to reuse. */
    if (!rows_top && v[0].y!=v[1].y) return -EOPNOTSUPP;
    struct matrox3d_trap *t=out;
    unsigned n=0;
    for (unsigned part=0;part<2;part++) {
        int32_t rows=part ? rows_bottom : rows_top;
        if (rows<=0) continue;
        struct setup_vertex ls,le,rs,re; uint32_t sgn=M3D_SGN_SUBPIXEN|M3D_SGN_CENTERSNAP|(encoding ? 0 : M3D_SGN_US);
        bool both=part==0 || n==0;
        if (v[0].y==v[1].y) { /* flat top: one trapezoid from both top vertices */
            ls=v[0]; rs=v[1]; le=re=v[2]; both=true;
        } else if (!part) {
            ls=rs=v[0]; le=broken_left ? v[1] : v[2]; re=broken_left ? v[2] : v[1];
        } else if (both) {
            /* No top rows: the shared edge starts at its rounded 1/16 position on y1. */
            int64_t xs=v[0].x+rdiv((int64_t)(v[2].x-v[0].x)*(v[1].y-v[0].y),v[2].y-v[0].y);
            struct setup_vertex shared={(int32_t)xs,v[1].y};
            ls=broken_left ? v[1] : shared; rs=broken_left ? shared : v[1]; le=re=v[2];
        } else {
            ls=rs=v[1]; le=re=v[2]; /* only the broken edge is rewritten below */
        }
        *t=(struct matrox3d_trap){0};
        t->bottom=part==1 && !both;
        t->ydst=field((part && !both) ? v[1].y : ls.y);
        t->len=(uint32_t)rows;
        if (both) {
            sgn|=M3D_SGN_SSE|M3D_SGN_SBE;
            t->fxleft=field(ls.x); t->fxright=field(rs.x);
            t->ar0=field(le.y-ls.y); t->ar2=field(le.x-ls.x);
            t->ar6=field(re.y-rs.y); t->ar5=field(re.x-rs.x);
            if (!fits(le.y-ls.y,18) || !fits(le.x-ls.x,18) || !fits(re.y-rs.y,18) || !fits(re.x-rs.x,18)) return -EOPNOTSUPP;
            /* Interpolants start at the left edge's initial vertex. */
            int64_t xq=ls.x,yq=ls.y;
            if (encoding) {
                /* First pixel centre of the first row under the top-left rule. */
                int64_t yc=((int64_t)first_row(ls.y)<<4)+8;
                int64_t xl=ls.x*16;
                if (le.y!=ls.y) xl=ls.x*16+rdiv((int64_t)(le.x-ls.x)*16*(yc-ls.y),le.y-ls.y);
                /* xl in 1/256 px; first pixel = ceil(xl/256 - 0.5). */
                int64_t px=(xl-128+255)/256;
                if (xl-128<0) px=-((-(xl-128))/256);
                xq=px*16+8; yq=yc;
            }
            int64_t start,ix,iy;
            for (unsigned k=0;k<11;k++) {
                if (encode_start(&sp,k,v,encoding,xq,yq,&start,&ix,&iy)) return -EINVAL;
                if (k==0) { if (!fits(start,32) || !fits(ix,32) || !fits(iy,32)) return -EOPNOTSUPP;
                            t->dr0=field(start); t->dr2=field(ix); t->dr3=field(iy); }
                else if (k<4) { if (!fits(start,24) || !fits(ix,24) || !fits(iy,24)) return -EOPNOTSUPP;
                            t->dr[(k-1)*3]=field(start)&0xffffffu; t->dr[(k-1)*3+1]=field(ix)&0xffffffu; t->dr[(k-1)*3+2]=field(iy)&0xffffffu; }
                else if (k==4) { if (!fits(start,24) || !fits(ix,24) || !fits(iy,24)) return -EOPNOTSUPP;
                            t->alpha[0]=field(start)&0xffffffu; t->alpha[1]=field(ix)&0xffffffu; t->alpha[2]=field(iy)&0xffffffu; }
                else {
                    unsigned stage=(k-5)/3,which=(k-5)%3;
                    if (!fits(start,32) || !fits(ix,32) || !fits(iy,32)) return -EOPNOTSUPP;
                    /* TMR6/7/8 starts, TMR0/1 s increments, TMR2/3 t, TMR4/5 q. */
                    t->tmr[stage][6+which]=field(start);
                    t->tmr[stage][which*2]=field(ix); t->tmr[stage][which*2+1]=field(iy);
                }
            }
        } else {
            sgn|=M3D_SGN_SBE|(broken_left ? M3D_SGN_BL : 0);
            if (!fits(v[2].y-v[1].y,18) || !fits(v[2].x-v[1].x,18)) return -EOPNOTSUPP;
            if (broken_left) { t->fxleft=field(v[1].x); t->ar0=field(v[2].y-v[1].y); t->ar2=field(v[2].x-v[1].x); }
            else { t->fxright=field(v[1].x); t->ar6=field(v[2].y-v[1].y); t->ar5=field(v[2].x-v[1].x); }
        }
        t->sgn=sgn; t++; n++;
        if (v[0].y==v[1].y) break;
    }
    *count=n; return 0;
}

/* ---- textures and layout ------------------------------------------------- */
unsigned matrox3d_texture_levels(const struct render3d_resource *r,bool g400)
{
    if (render3d_texture_shape(r) || r->width<8 || r->height<8) return 0;
    unsigned smallest=r->width<r->height ? r->width : r->height;
    unsigned n=log2_of(smallest)-2; /* maps down to 8 texels */
    if (n>r->levels) n=r->levels;
    if (!g400 && n>5) n=5;
    return n;
}
static uint32_t level_bytes(const struct render3d_resource *r,unsigned level)
{
    uint32_t bytes=(r->width>>level)*(r->height>>level)*4u;
    return bytes<32 ? 32 : bytes;
}
uint32_t matrox3d_texture_bytes(const struct render3d_resource *r,bool g400)
{
    unsigned n=matrox3d_texture_levels(r,g400);
    if (!n) return 0;
    uint32_t total=0;
    for (unsigned i=0;i<n;i++) total+=level_bytes(r,i);
    return (total+4095)&~4095u;
}
static uint32_t level_offset(const struct render3d_resource *r,unsigned level)
{
    uint32_t at=0;
    for (unsigned i=0;i<level;i++) at+=level_bytes(r,i);
    return at;
}
static uint32_t hw_pitch(const struct render3d_resource *r) { return ((r->width+31)&~31u)*4u; }
static uint32_t padded_bytes(const struct render3d_resource *r,bool g400)
{
    if (r->kind==R3D_TEXTURE) return matrox3d_texture_bytes(r,g400);
    return (hw_pitch(r)*((r->height+15)&~15u)+4095)&~4095u;
}
int matrox3d_mode_heap(const struct matrox_engine *s,const struct fb_device *fb,uint32_t vram,
                       uint32_t cursor_offset,struct render3d_extent *heap,uint32_t *required)
{
    if (!s || !fb || !heap || !required || !fb->width || !fb->height || fb->width>1024 || fb->height>1024 ||
        fb->bpp!=32 || fb->pitch<fb->width*4 || fb->size>vram ||
        (uint64_t)fb->pitch*fb->height>fb->size || cursor_offset>vram ||
        vram<4096 || vram>32u*1024*1024) return -EOPNOTSUPP;
    uint32_t first=(fb->size+4095)&~4095u;
    uint32_t end=cursor_offset ? cursor_offset&~4095u : vram&~4095u;
    if (first>=end) return -ENOMEM;
    uint32_t height=(fb->height+15)&~15u,pitch=((fb->width+31)&~31u)*4u;
    uint32_t target=(pitch*height+4095)&~4095u;
    *required=3*target+MATROX3D_TEXTURE_MIN+4*8192u+24576u;
    *heap=(struct render3d_extent){first,end-first};
    return heap->bytes<*required ? -ENOMEM : 0;
}
static bool fullsize(const struct matrox3d_device *d)
{ return d && (d->fullsize_qualified || d->fullsize_testing) && d->mode_width && d->mode_height; }
static bool dimensions(const struct matrox3d_device *d,const struct render3d_resource *r)
{ return fullsize(d) ? r->width<=d->mode_width && r->height<=d->mode_height : r->width<=64 && r->height<=64; }
static int layout(void *device,struct render3d_resource *r)
{
    const struct matrox3d_device *d=device;
    if (!d || !r || !r->width || !r->height || r->kind<R3D_COLOR || r->kind>R3D_LINEAR_COLOR ||
        (r->kind==R3D_TEXTURE ? (r->width>1024 || r->height>1024) : !dimensions(d,r))) return -EOPNOTSUPP;
    if (r->kind==R3D_TEXTURE && render3d_texture_shape(r)) return -EINVAL;
    /* Textures below 8 texels have no hardware layout; the context may still
     * use them in software (AUTO) through the service's fallback. */
    if (r->kind==R3D_TEXTURE && !matrox3d_texture_levels(r,d->g400)) return -EOPNOTSUPP;
    r->reserved=padded_bytes(r,d->g400)+8192;
    return 0;
}
static bool overlaps(struct render3d_extent a,struct render3d_extent b)
{ return (uint64_t)a.first+a.bytes>b.first && (uint64_t)b.first+b.bytes>a.first; }
static bool extent(const struct matrox3d_device *d,const struct render3d_resource *r)
{
    uint32_t pool=d->heap.first+d->heap.bytes-(d->texture_bytes ? d->texture_bytes : MATROX3D_TEXTURE_MIN);
    return r && r->kind>=R3D_COLOR && r->kind<=R3D_LINEAR_COLOR &&
        r->hardware && r->shadow && r->width && r->height &&
        (r->kind==R3D_TEXTURE ? !render3d_texture_shape(r) && matrox3d_texture_levels(r,d->g400) : dimensions(d,r)) &&
        r->offset>=d->heap.first && !(r->offset&4095u) &&
        (uint64_t)r->offset+r->reserved<=(uint64_t)d->heap.first+d->heap.bytes &&
        (r->kind==R3D_TEXTURE ? r->offset>=pool : (uint64_t)r->offset+r->reserved<=pool) &&
        r->reserved==padded_bytes(r,d->g400)+8192 && r->pitch==r->width*(r->kind==R3D_D16 ? 2u : 4u) &&
        (r->kind==R3D_TEXTURE || r->bytes==r->pitch*r->height);
}
static uint32_t base(const struct render3d_resource *r) { return r->offset+4096; }

/* ---- mip admission: a conservative LOD bound over the triangle ------------ */
static bool mip_admissible(const struct render3d_request *q,unsigned first,unsigned stage,
                           const struct render3d_resource *t,unsigned levels_hw)
{
    const struct render3d_vertex *v=q->vertices+first;
    int64_t x[3],y[3],bmin=INT64_MAX,bmax=0,amax=0;
    int64_t a[2][3];
    for (unsigned i=0;i<3;i++) {
        x[i]=v[i].x/4096; y[i]=v[i].y/4096;
        if (v[i].rhw<bmin) bmin=v[i].rhw;
        if (v[i].rhw>bmax) bmax=v[i].rhw;
        for (unsigned axis=0;axis<2;axis++) {
            int64_t uv=stage ? q->uv1[first+i][axis] : axis ? v[i].v : v[i].u;
            a[axis][i]=rdiv(uv*(int64_t)v[i].rhw,65536); /* Q16 of the real product */
            int64_t m=a[axis][i]<0 ? -a[axis][i] : a[axis][i];
            if (m>amax) amax=m;
        }
    }
    int64_t area=(x[1]-x[0])*(y[2]-y[0])-(x[2]-x[0])*(y[1]-y[0]);
    if (!area) return true;
    if (area<0) area=-area;
    /* Numerator of d(A/B) is affine over the triangle. Its extrema are
     * at vertices; keep cancellation instead of independently bounding A/B.
     * One unit of gradient rounding error is included on both terms. */
    int64_t bg[2]={
        (((int64_t)v[1].rhw-v[0].rhw)*(y[2]-y[0])-((int64_t)v[2].rhw-v[0].rhw)*(y[1]-y[0]))*16,
        (((int64_t)v[2].rhw-v[0].rhw)*(x[1]-x[0])-((int64_t)v[1].rhw-v[0].rhw)*(x[2]-x[0]))*16};
    int64_t signed_area=(x[1]-x[0])*(y[2]-y[0])-(x[2]-x[0])*(y[1]-y[0]);
    bg[0]=rdiv(bg[0],signed_area); bg[1]=rdiv(bg[1],signed_area);
    int64_t maximum=0;
    for (unsigned axis=0;axis<2;axis++) {
        int64_t ag[2]={rdiv(((a[axis][1]-a[axis][0])*(y[2]-y[0])-(a[axis][2]-a[axis][0])*(y[1]-y[0]))*16,signed_area),
            rdiv(((a[axis][2]-a[axis][0])*(x[1]-x[0])-(a[axis][1]-a[axis][0])*(x[2]-x[0]))*16,signed_area)};
        int64_t bound[2]={0};
        for (unsigned direction=0;direction<2;direction++) for (unsigned i=0;i<3;i++) {
            int64_t n=ag[direction]*v[i].rhw-a[axis][i]*bg[direction];
            if (n<0) n=-n;
            n+=bmax+amax; /* conservative derivative quantization */
            if (n>bound[direction]) bound[direction]=n;
        }
        int64_t sum=bound[0]+bound[1];
        if (sum>maximum) maximum=sum;
    }
    unsigned size=t->width>t->height ? t->width : t->height;
    int64_t lhs=sat_mul(maximum,size);
    int64_t rhs=sat_mul(sat_mul(bmin,bmin),(int64_t)1<<(levels_hw-1));
    return lhs<=rhs;
}

/* ---- validation -------------------------------------------------------- */
static int validate(void *device,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    struct matrox3d_device *d=device;
    if (!d || !d->engine || !d->heap.first || !d->heap.bytes ||
        d->heap.first>UINT32_MAX-d->heap.bytes || (d->heap.first&4095u) || (d->heap.bytes&4095u) ||
        (uint64_t)d->heap.first+d->heap.bytes>32u*1024*1024 || d->heap.bytes<=MATROX3D_TEXTURE_MIN ||
        (d->texture_bytes && ((d->texture_bytes&4095u) || d->texture_bytes<MATROX3D_TEXTURE_MIN || d->texture_bytes>=d->heap.bytes)) ||
        !q || !r || !q->flags || (q->flags&~R3D_ALL_FEATURES) ||
        render3d_validate_state(q) || q->count>(fullsize(d) ? R3D_VERTICES : 3u) || q->count%3 || q->clear_depth>65535 ||
        !extent(d,r[0]) || r[0]->kind!=R3D_COLOR) return -EINVAL;
    for (unsigned i=0;i<R3D_REFS;i++) if (r[i]) {
        if (!extent(d,r[i])) return -EINVAL;
        for (unsigned j=0;j<i;j++) if (r[j] && r[i]!=r[j] && overlaps((struct render3d_extent){r[i]->offset,r[i]->reserved},
            (struct render3d_extent){r[j]->offset,r[j]->reserved})) return -EINVAL;
    }
    if ((q->flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE)) &&
        (!r[1] || r[1]->kind!=R3D_D16 || r[1]->width!=r[0]->width || r[1]->height!=r[0]->height)) return -EINVAL;
    if ((q->flags&R3D_TEXTURED) && (!r[2] || render3d_texture_shape(r[2]))) return -EINVAL;
    if ((q->flags&R3D_TEXTURE1) && (!(q->flags&R3D_TEXTURED) || !r[4] || render3d_texture_shape(r[4]))) return -EINVAL;
    if (!(q->flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE)) && r[1]) return -EINVAL;
    if (!(q->flags&R3D_TEXTURED) && r[2]) return -EINVAL;
    if (!(q->flags&R3D_COPY) && r[3]) return -EINVAL;
    if (!(q->flags&R3D_TEXTURE1) && r[4]) return -EINVAL;
    if ((q->flags&R3D_COPY) && (!r[3] || (r[3]->kind!=R3D_LINEAR_COLOR && r[3]->kind!=R3D_COLOR) ||
        r[3]->width!=r[0]->width || r[3]->height!=r[0]->height)) return -EINVAL;
    for (unsigned i=0;i<q->count;i++) if (q->vertices[i].x<0 || q->vertices[i].y<0 ||
        q->vertices[i].x>(int32_t)(r[0]->width<<16) || q->vertices[i].y>(int32_t)(r[0]->height<<16)) return -EINVAL;
    /* Bound attributes and gradients before the mip derivative arithmetic.
     * A malformed RHW/UV must never reach those signed products. */
    struct matrox3d_trap traps[2]; unsigned count;
    for (unsigned first=0;first<q->count;first+=3) {
        int e=matrox3d_setup(q,first,d->encoding,traps,&count); if (e) return e;
    }
    /* Family limits that keep the whole context in software. */
    if ((q->flags&R3D_DEPTH_TEST) && q->state.depth_func==R3D_NEVER) return -EOPNOTSUPP;
    if (q->state.blend && (q->state.source==R3D_SRC_COLOR || q->state.source==R3D_INV_SRC_COLOR ||
        q->state.destination==R3D_DST_COLOR || q->state.destination==R3D_INV_DST_COLOR)) return -EOPNOTSUPP;
    if ((q->flags&R3D_TEXTURE1) && !d->g400) return -EOPNOTSUPP;
    for (unsigned stage=0;stage<2;stage++) if (q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED)) {
        const struct render3d_resource *t=r[stage ? 4 : 2];
        const struct render3d_sampler *s=&q->state.sampler[stage];
        unsigned limit=d->perspective_qualified || d->fullsize_testing ? 256 : 64;
        if (t->width>limit || t->height>limit) return -EOPNOTSUPP;
        if (s->combine==R3D_DECAL && t->format==R3D_A8) return -EOPNOTSUPP;
        if (!d->g400 && (s->combine==R3D_ADD || (t->format==R3D_A8 && s->combine==R3D_REPLACE))) return -EOPNOTSUPP;
        if (s->filter>=R3D_NEAREST_MIP) {
            unsigned levels=matrox3d_texture_levels(t,d->g400);
            if (t->width<16 || t->height<16 || !levels) return -EOPNOTSUPP;
            for (unsigned first=0;first<q->count;first+=3)
                if (!mip_admissible(q,first,stage,t,levels)) return -EOPNOTSUPP;
        }
    }
    if (!d->fullsize_testing && !d->perspective_qualified && q->state.blend &&
        (q->state.source>R3D_INV_SRC_ALPHA || q->state.destination>R3D_INV_SRC_ALPHA)) return -EOPNOTSUPP;
    if (d->fullsize_qualified && !d->fullsize_testing && (q->flags&R3D_TEXTURED))
      for (unsigned first=0;first<q->count;first+=3) {
        bool varying=q->vertices[first].rhw!=q->vertices[first+1].rhw ||
                     q->vertices[first].rhw!=q->vertices[first+2].rhw;
        if (varying && !d->perspective_qualified) return -EOPNOTSUPP;
      }
    return 0;
}

/* ---- transport --------------------------------------------------------- */
static struct { uint16_t reg; uint32_t value; } pending[16];
static unsigned pending_count;
static int flush(struct matrox3d_device *d)
{
    if (!pending_count) return 0;
    if (d->engine->io.now_us(d->engine->io.ctx)-d->slice_started>=20000) { pending_count=0; return -ETIMEDOUT; }
    int e=matrox_engine_reserve(d->engine,pending_count);
    if (e) { pending_count=0; return e; }
    for (unsigned i=0;i<pending_count;i++) d->engine->io.write32(d->engine->io.ctx,pending[i].reg,pending[i].value);
    d->writes+=pending_count; pending_count=0;
    return 0;
}
static int emit(struct matrox3d_device *d,unsigned reg,uint32_t value)
{
    if (pending_count==16) { int e=flush(d); if (e) return e; }
    pending[pending_count].reg=(uint16_t)reg; pending[pending_count].value=value; pending_count++;
    return 0;
}
static int launch(struct matrox3d_device *d,unsigned reg,uint32_t value)
{
    d->engine->cpu_safe=false;
    int e=emit(d,reg|M_EXEC,value); if (e) return e;
    return flush(d);
}
static int sync(struct matrox3d_device *d)
{
    int e=flush(d); if (e) return e;
    if (d->engine->io.now_us(d->engine->io.ctx)-d->slice_started>=20000) return -ETIMEDOUT;
    return matrox_engine_sync(d->engine);
}
/* The 2D owner's initialization set plus the 3D registers with documented
 * reset values. Every register is write-only; the host model checks the trace. */
static int restore_state(struct matrox3d_device *d)
{
    const struct matrox_engine *s=d->engine;
    const struct { unsigned reg; uint32_t value; } set[]={
        {M_DWGCTL,0},{M3D_SGN,0},{M_PITCH,s->pitch},{M_YDSTORG,0},{M_PLNWT,UINT32_MAX},
        {M_SRCORG,0},{M_DSTORG,0},{M3D_ZORG,0},{M_CXBNDRY,0xffff0000u},{M_YTOP,0},{M_YBOT,0x01ffffffu},
        {M_MACCESS,s->maccess},{M3D_ALPHACTRL,1},{M3D_TEXCTL2,0},{M3D_TEXCTL,0},{M3D_TEXFILTER,0}};
    for (unsigned i=0;i<sizeof(set)/sizeof(set[0]);i++) { int e=emit(d,set[i].reg,set[i].value); if (e) return e; }
    int e=flush(d); if (e) return e;
    d->engine->io.write32(d->engine->io.ctx,M_OPMODE,4);
    return sync(d);
}

/* ---- CPU transfers ------------------------------------------------------- */
static uint32_t memory_read(struct matrox3d_device *d,uint32_t off,unsigned bytes)
{ return d->memory.read(d->memory.ctx,off,bytes); }
static void memory_write(struct matrox3d_device *d,uint32_t off,uint32_t value,unsigned bytes)
{ d->memory.write(d->memory.ctx,off,value,bytes); }
static int guards(struct matrox3d_device *d,struct render3d_resource *r,bool initialize)
{
    uint64_t start=0;
    for (unsigned side=0;side<2;side++) for (unsigned i=0;i<4096;i+=4) {
        if (!(i%512)) start=d->engine->io.now_us(d->engine->io.ctx);
        uint32_t off=r->offset+(side ? r->reserved-4096 : 0)+i;
        if (initialize) memory_write(d,off,0xa55ac33cu,4);
        else {
            uint32_t value=memory_read(d,off,4);
            if (value!=0xa55ac33cu) { d->expected=0xa55ac33cu; d->actual=value; d->guard_offset=off; return -EIO; }
        }
        if (i%512==508 && d->engine->io.now_us(d->engine->io.ctx)-start>=1000) return -ETIMEDOUT;
    }
    return 0;
}
static int upload(struct matrox3d_device *d,struct render3d_resource *r)
{
    if (r->resident) return guards(d,r,false);
    int e=guards(d,r,true); if (e) return e;
    if (r->kind==R3D_TEXTURE) {
        unsigned n=matrox3d_texture_levels(r,d->g400);
        for (unsigned level=0;level<n;level++) {
            unsigned w=r->width>>level,h=r->height>>level;
            const uint32_t *pixels=(const void *)((const uint8_t *)r->shadow+r->level_offset[level]);
            uint32_t at=base(r)+level_offset(r,level);
            for (unsigned first=0;first<w*h;first+=128) {
                uint64_t start=d->engine->io.now_us(d->engine->io.ctx);
                unsigned end=first+128; if (end>w*h) end=w*h;
                for (unsigned i=first;i<end;i++) memory_write(d,at+i*4,pixels[i],4);
                if (d->engine->io.now_us(d->engine->io.ctx)-start>=1000) return -ETIMEDOUT;
            }
        }
    } else {
        unsigned bytes=r->kind==R3D_D16 ? 2 : 4,pitch=hw_pitch(r);
        for (unsigned y=0;y<r->height;y++) for (unsigned first=0;first<r->width;first+=128) {
            uint64_t start=d->engine->io.now_us(d->engine->io.ctx);
            unsigned end=first+128; if (end>r->width) end=r->width;
            for (unsigned x=first;x<end;x++) {
                uint32_t value=0; memcpy(&value,(uint8_t *)r->shadow+y*r->pitch+x*bytes,bytes);
                memory_write(d,base(r)+y*pitch+x*bytes,value,bytes);
            }
            if (d->engine->io.now_us(d->engine->io.ctx)-start>=1000) return -ETIMEDOUT;
        }
    }
    d->engine->io.drain(d->engine->io.ctx); r->resident=true; return 0;
}
static int read_resource(struct matrox3d_device *d,struct render3d_resource *r)
{
    if (r->kind==R3D_TEXTURE) return guards(d,r,false);
    unsigned bytes=r->kind==R3D_D16 ? 2 : 4,pitch=hw_pitch(r);
    for (unsigned y=0;y<r->height;y++) {
        if (d->fullsize_testing && y%16 && y!=r->height-1) continue;
        for (unsigned first=0;first<r->width;first+=128) {
            uint64_t start=d->engine->io.now_us(d->engine->io.ctx);
            unsigned end=first+128; if (end>r->width) end=r->width;
            for (unsigned x=first;x<end;x++) {
                uint32_t value=memory_read(d,base(r)+y*pitch+x*bytes,bytes);
                memcpy((uint8_t *)r->shadow+y*r->pitch+x*bytes,&value,bytes);
            }
            if (d->engine->io.now_us(d->engine->io.ctx)-start>=1000) return -ETIMEDOUT;
        }
    }
    return guards(d,r,false);
}

/* ---- state and primitives ------------------------------------------------ */
static int target_state(struct matrox3d_device *d,const struct render3d_resource *target,const struct render3d_resource *depth)
{
    uint32_t pitch=hw_pitch(target)/4;
    int e=emit(d,M_DSTORG,base(target));
    if (!e) e=emit(d,M3D_ZORG,depth ? base(depth) : 0);
    if (!e) e=emit(d,M_PITCH,pitch);
    if (!e) e=emit(d,M_YDSTORG,0);
    if (!e) e=emit(d,M_PLNWT,UINT32_MAX);
    if (!e) e=emit(d,M_MACCESS,2u|(1u<<30)); /* PW32, ZW16, nodither */
    if (!e) e=emit(d,M_CXBNDRY,((target->width-1)<<16));
    if (!e) e=emit(d,M_YTOP,0);
    if (!e) e=emit(d,M_YBOT,(target->height-1)*pitch);
    return e;
}
static int clear_resource(struct matrox3d_device *d,const struct render3d_resource *r,uint32_t value)
{
    uint32_t words=r->kind==R3D_D16 ? (r->width+1)/2 : r->width;
    if (r->kind==R3D_D16) value=(value&65535u)|(value<<16);
    int e=emit(d,M_MACCESS,2u|(1u<<30));
    if (!e) e=emit(d,M_PLNWT,UINT32_MAX);
    if (!e) e=emit(d,M_YDSTORG,0);
    if (!e) e=emit(d,M_YTOP,0);
    if (!e) e=emit(d,M_DSTORG,base(r));
    if (!e) e=emit(d,M_PITCH,hw_pitch(r)/4);
    if (!e) e=emit(d,M_YBOT,(r->height-1)*(hw_pitch(r)/4));
    if (!e) e=emit(d,M_CXBNDRY,(words-1)<<16);
    if (!e) e=emit(d,M_DWGCTL,M_DWG_TRAP|M_DWG_SOLID|M_DWG_ARZERO|M_DWG_SGNZERO|M_DWG_SHIFTZERO|M_DWG_REPLACE);
    if (!e) e=emit(d,M_FCOL,value);
    if (!e) e=emit(d,M_FXBNDRY,words<<16);
    if (!e) e=launch(d,M_YDSTLEN,r->height);
    if (!e) d->clears++;
    return e;
}
static int copy_resource(struct matrox3d_device *d,const struct render3d_resource *dst,const struct render3d_resource *src)
{
    uint32_t pitch=hw_pitch(dst)/4;
    int e=emit(d,M_DSTORG,base(dst));
    if (!e) e=emit(d,M_SRCORG,base(src));
    if (!e) e=emit(d,M_PITCH,pitch);
    if (!e) e=emit(d,M_YBOT,(dst->height-1)*pitch);
    if (!e) e=emit(d,M_CXBNDRY,(dst->width-1)<<16);
    if (!e) e=emit(d,M_DWGCTL,M_DWG_BITBLT|M_DWG_SGNZERO|M_DWG_SHIFTZERO|M_DWG_BFCOL|M_DWG_REPLACE);
    if (!e) e=emit(d,M_AR5,hw_pitch(src)/4);
    if (!e) e=emit(d,M_AR0,dst->width-1);
    if (!e) e=emit(d,M_AR3,0);
    if (!e) e=emit(d,M_FXBNDRY,(dst->width-1)<<16);
    if (!e) e=launch(d,M_YDSTLEN,dst->height);
    if (!e) { e=emit(d,M_SRCORG,0); if (!e) e=emit(d,M_AR5,0); }
    if (!e) d->copies++;
    return e;
}
/* TDUALSTAGE field vocabulary (Mesa mgaregs.h); scalar reference semantics. */
#define TD_ARG2_DIFFUSE 0u
#define TD_ARG2_PREV 3u
#define TD_ALPHASEL_CURRTEX (2u<<2)
#define TD_ARG1_INV (1u<<6)
#define TD_ALPHA1INV (1u<<9)
#define TD_ARG1MUL_ALPHA1 (1u<<11)
#define TD_ARG1ADD_MULOUT (1u<<13)
#define TD_ARG2ADD_MULOUT (1u<<14)
#define TD_COLOR_ADD (1u<<17)
#define TD_COLOR_BLEND (1u<<20)
#define TD_COLOR_SEL_ARG1 (0u<<21)
#define TD_COLOR_SEL_ARG2 (1u<<21)
#define TD_COLOR_SEL_ADD (2u<<21)
#define TD_COLOR_SEL_MUL (3u<<21)
#define TD_ALPHA_ARG2_DIFFUSE (0u<<24)
#define TD_ALPHA_ARG2_PREV (3u<<24)
#define TD_ALPHA_SEL_ARG1 (0u<<30)
#define TD_ALPHA_SEL_ARG2 (1u<<30)
#define TD_ALPHA_SEL_MUL (3u<<30)
static uint32_t combiner(unsigned stage,unsigned combine,unsigned format)
{
    bool alpha_only=format==R3D_A8,rgb_only=format==R3D_RGB565 || format==R3D_L8;
    uint32_t arg2=stage ? TD_ARG2_PREV : TD_ARG2_DIFFUSE,aarg2=stage ? TD_ALPHA_ARG2_PREV : TD_ALPHA_ARG2_DIFFUSE;
    uint32_t colour,alpha;
    switch (combine) {
    case R3D_REPLACE:
        colour=alpha_only ? arg2|TD_COLOR_SEL_ARG2 : TD_COLOR_SEL_ARG1;
        alpha=rgb_only ? aarg2|TD_ALPHA_SEL_ARG2 : TD_ALPHA_SEL_ARG1;
        break;
    case R3D_DECAL:
        colour=rgb_only ? TD_COLOR_SEL_ARG1 : arg2|TD_ALPHASEL_CURRTEX|TD_ALPHA1INV|TD_ARG1MUL_ALPHA1|
            TD_COLOR_BLEND|TD_ARG1ADD_MULOUT|TD_ARG2ADD_MULOUT|TD_COLOR_ADD|TD_COLOR_SEL_ADD;
        alpha=aarg2|TD_ALPHA_SEL_ARG2;
        break;
    case R3D_ADD:
        colour=alpha_only ? arg2|TD_COLOR_SEL_ARG2 : arg2|TD_COLOR_ADD|TD_COLOR_SEL_ADD;
        alpha=aarg2|TD_ALPHA_SEL_MUL;
        break;
    default:
        colour=arg2|TD_COLOR_SEL_MUL; alpha=aarg2|TD_ALPHA_SEL_MUL;
        break;
    }
    return colour|alpha;
}
static int texture_state(struct matrox3d_device *d,unsigned stage,const struct render3d_resource *t,
                         const struct render3d_sampler *s,unsigned combine,bool dual)
{
    unsigned lw=log2_of(t->width),lh=log2_of(t->height),levels=matrox3d_texture_levels(t,d->g400);
    bool mips=s->filter>=R3D_NEAREST_MIP;
    unsigned used=mips ? levels : 1;
    /* tw = log2(w) + (4 - s_frac + q_frac) with s_frac 26, q_frac 28;
     * rfw = 8 - log2(w) - (q_frac - 16). Six-bit two's complement fields. */
    uint32_t texwidth=((t->width-1)<<18)|(((8u-lw-12u)&63u)<<9)|((lw+6u)&63u);
    uint32_t texheight=((t->height-1)<<18)|(((8u-lh-12u)&63u)<<9)|((lh+6u)&63u);
    uint32_t texctl=6u|(1u<<8)|((t->width&2047u)<<9)|(1u<<25)|(s->repeat_v ? 0 : 1u<<27)|(s->repeat_u ? 0 : 1u<<28);
    uint32_t texctl2=(d->g400 ? (1u<<15) : 0)|(dual ? 1u<<7 : 0);
    if (!d->g400) {
        bool alpha_only=t->format==R3D_A8,rgb_only=t->format==R3D_RGB565 || t->format==R3D_L8;
        if (combine==R3D_MODULATE) texctl|=1u<<29;
        else if (combine==R3D_DECAL && !rgb_only && !alpha_only) texctl2|=1u;
    }
    unsigned minf=s->filter==R3D_NEAREST ? 0 : s->filter==R3D_BILINEAR ? 2 : s->filter==R3D_NEAREST_MIP ? 8 : s->filter==R3D_BILINEAR_MIP ? 10 : 12;
    unsigned magf=s->filter==R3D_NEAREST || s->filter==R3D_NEAREST_MIP ? 0 : 2;
    uint32_t texfilter=(1u<<20)|minf|(magf<<4)|(0x10u<<21)|(((used-1)&7u)<<29)|((((used-1)>>3)&1u)<<18);
    uint32_t texorg=base(t),texorg1=0,texorg2=0,texorg3=0,texorg4=0;
    if (d->g400) { texorg|=4u; texorg1=level_bytes(t,0); }
    else {
        uint32_t *regs[]={&texorg1,&texorg2,&texorg3,&texorg4};
        for (unsigned i=1;i<levels && i<5;i++) *regs[i-1]=base(t)+level_offset(t,i);
    }
    int e=0;
    e=emit(d,M3D_TEXCTL2,texctl2|(stage ? (1u<<7)|(1u<<31) : 0));
    if (!e) e=emit(d,M3D_TEXCTL,texctl);
    if (!e) e=emit(d,M3D_TEXFILTER,texfilter);
    if (!e) e=emit(d,M3D_TEXBORDERCOL,0);
    if (!e) e=emit(d,M3D_TEXORG,texorg);
    if (!e) e=emit(d,M3D_TEXORG1,texorg1);
    if (!e) e=emit(d,M3D_TEXORG2,texorg2);
    if (!e) e=emit(d,M3D_TEXORG3,texorg3);
    if (!e) e=emit(d,M3D_TEXORG4,texorg4);
    if (!e) e=emit(d,M3D_TEXWIDTH,texwidth);
    if (!e) e=emit(d,M3D_TEXHEIGHT,texheight);
    if (!e) e=emit(d,M3D_TEXTRANS,0x0000ffffu);
    if (!e) e=emit(d,M3D_TEXTRANSHIGH,0x0000ffffu);
    return e;
}
static int state(struct matrox3d_device *d,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    static const uint8_t zmode[]={4,0,0,2,5,6,3,7},atmode[]={4,4,0,2,5,6,3,7};
    static const uint8_t sfactor[]={1,0,4,5,2,0,3,0,6,7},dfactor[]={1,0,4,5,0,2,0,3,6,7};
    bool textured=q->flags&R3D_TEXTURED,dual=q->flags&R3D_TEXTURE1;
    bool depth_test=q->flags&R3D_DEPTH_TEST,depth_write=q->flags&R3D_DEPTH_WRITE;
    uint32_t dwgctl=(textured ? 6u : 4u)|((depth_write ? 3u : 7u)<<4)|((depth_test ? (uint32_t)zmode[q->state.depth_func] : 0u)<<8)|
        M_DWG_SHIFTZERO|(0xCu<<16);
    uint32_t alphactrl=(1u<<8)|(q->state.blend ? (uint32_t)sfactor[q->state.source]|((uint32_t)dfactor[q->state.destination]<<4) : 1u);
    if (q->state.alpha_test) {
        unsigned mode=atmode[q->state.alpha_func],ref=q->state.alpha_ref;
        if (q->state.alpha_func==R3D_NEVER) ref=0; /* alpha < 0 never passes */
        /* ATEN enables an extra test before modulation, unlike GL. The
         * final test uses ATMODE independently (G400 3-35; Mesa mgastate). */
        alphactrl|=(mode<<13)|(ref<<16);
    }
    /* alphasel: textured draws take the combiner alpha (G400) or the
     * modulated/texture/diffuse selection (G200); Gouraud takes diffuse. */
    uint32_t alphasel=1;
    if (textured) {
        if (d->g400) alphasel=0;
        else {
            unsigned combine=q->state.sampler[0].combine,format=r[2]->format;
            bool rgb_only=format==R3D_RGB565 || format==R3D_L8;
            alphasel=combine==R3D_MODULATE ? (rgb_only ? 1 : 2) : combine==R3D_REPLACE ? (rgb_only ? 1 : 0) : 1;
        }
    }
    alphactrl|=alphasel<<24;
    int e=target_state(d,r[0],r[1]);
    if (!e) e=emit(d,M_DWGCTL,dwgctl);
    if (!e) e=emit(d,M3D_ALPHACTRL,alphactrl);
    if (!e && textured) {
        e=emit(d,M3D_TEXCTL2,(d->g400 ? (1u<<15) : 0)|(dual ? 1u<<7 : 0));
        if (!e) e=texture_state(d,0,r[2],&q->state.sampler[0],q->state.sampler[0].combine,dual);
        if (!e && dual) {
            e=texture_state(d,1,r[4],&q->state.sampler[1],q->state.sampler[1].combine,dual);
            if (!e) e=emit(d,M3D_TEXCTL2,(1u<<15)|(1u<<7));
        }
        if (!e && d->g400) {
            uint32_t td0=combiner(0,q->state.sampler[0].combine,r[2]->format);
            e=emit(d,M3D_TDUALSTAGE0,td0);
            if (!e) e=emit(d,M3D_TDUALSTAGE1,dual ? combiner(1,q->state.sampler[1].combine,r[4]->format) : td0);
        }
    } else if (!e && d->g400) {
        e=emit(d,M3D_TDUALSTAGE0,TD_COLOR_SEL_MUL);
        if (!e) e=emit(d,M3D_TDUALSTAGE1,TD_COLOR_SEL_MUL);
    }
    return e;
}
static int draw_trap(struct matrox3d_device *d,const struct matrox3d_trap *t,bool textured,bool dual)
{
    int e=0;
    if (!t->bottom) {
        const unsigned dr[]={M3D_DR4,M3D_DR6,M3D_DR7,M3D_DR8,M3D_DR10,M3D_DR11,M3D_DR12,M3D_DR14,M3D_DR15};
        e=emit(d,M3D_DR0,t->dr0);
        if (!e) e=emit(d,M3D_DR2,t->dr2);
        if (!e) e=emit(d,M3D_DR3,t->dr3);
        for (unsigned i=0;!e && i<9;i++) e=emit(d,dr[i],t->dr[i]);
        if (!e) e=emit(d,M3D_ALPHASTART,t->alpha[0]);
        if (!e) e=emit(d,M3D_ALPHAXINC,t->alpha[1]);
        if (!e) e=emit(d,M3D_ALPHAYINC,t->alpha[2]);
        if (textured) {
            for (unsigned i=0;!e && i<9;i++) e=emit(d,M3D_TMR0+i*4,t->tmr[0][i]);
            if (dual) {
                if (!e) e=emit(d,M3D_TEXCTL2,(1u<<15)|(1u<<7)|(1u<<31));
                for (unsigned i=0;!e && i<9;i++) e=emit(d,M3D_TMR0+i*4,t->tmr[1][i]);
                if (!e) e=emit(d,M3D_TEXCTL2,(1u<<15)|(1u<<7));
            }
        }
        if (!e) e=emit(d,M3D_FXLEFT,t->fxleft);
        if (!e) e=emit(d,M3D_FXRIGHT,t->fxright);
        if (!e) e=emit(d,M3D_AR0,t->ar0);
        if (!e) e=emit(d,M3D_AR2,t->ar2);
        if (!e) e=emit(d,M3D_AR5,t->ar5);
        if (!e) e=emit(d,M3D_AR6,t->ar6);
    } else if (t->sgn&M3D_SGN_BL) {
        e=emit(d,M3D_FXLEFT,t->fxleft);
        if (!e) e=emit(d,M3D_AR0,t->ar0);
        if (!e) e=emit(d,M3D_AR2,t->ar2);
    } else {
        e=emit(d,M3D_FXRIGHT,t->fxright);
        if (!e) e=emit(d,M3D_AR6,t->ar6);
        if (!e) e=emit(d,M3D_AR5,t->ar5);
    }
    if (!e) e=emit(d,M3D_YDST,t->ydst);
    if (!e) e=emit(d,M3D_SGN,t->sgn);
    if (!e) e=launch(d,M3D_LEN,t->len);
    return e;
}

/* ---- backend hooks ------------------------------------------------------- */
static void quarantine(void *device);
static int execute(void *device,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    struct matrox3d_device *d=device;
    int e=validate(d,q,r); if (e) return e;
    if (!d->locked || (!d->qualified && !d->probing) || d->lost) return -ENODEV;
    d->slice_started=d->engine->io.now_us(d->engine->io.ctx);
    pending_count=0;
    d->failed_phase=1;
    e=matrox_engine_sync(d->engine); /* idle before any CPU transfer */
    if (!e) d->failed_phase=2;
    for (unsigned i=0;!e && i<R3D_REFS;i++) if (r[i]) e=upload(d,r[i]);
    d->slice_started=d->engine->io.now_us(d->engine->io.ctx);
    if (!e) d->failed_phase=3;
    if (!e && (q->flags&(R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH))) {
        if (q->flags&R3D_CLEAR_COLOR) e=clear_resource(d,r[0],q->clear_color);
        if (!e && (q->flags&R3D_CLEAR_DEPTH)) e=clear_resource(d,r[1],q->clear_depth);
    }
    if (!e) d->failed_phase=4;
    if (!e && q->count) {
        e=state(d,q,r);
        for (unsigned first=0;!e && first<q->count;first+=3) {
            struct matrox3d_trap traps[2]; unsigned count=0;
            e=matrox3d_setup(q,first,d->encoding,traps,&count);
            for (unsigned i=0;!e && i<count;i++) e=draw_trap(d,&traps[i],q->flags&R3D_TEXTURED,q->flags&R3D_TEXTURE1);
            if (!e) d->triangles++;
        }
    }
    if (!e) d->failed_phase=5;
    if (!e && (q->flags&R3D_COPY)) e=copy_resource(d,r[3],r[0]);
    if (!e) e=sync(d);
    if (!e) {
        d->failed_phase=8;
        for (unsigned i=0;i<R3D_REFS && !e;i++) if (r[i]) e=read_resource(d,r[i]);
    }
    /* Restoration needs an idle proof; a timeout after proven completion
     * still restores and fails only this context. */
    bool idle=d->engine->cpu_safe && !d->engine->quarantined;
    if (!idle && (e==-ETIMEDOUT || !e)) { pending_count=0; idle=!matrox_engine_sync(d->engine); }
    if (idle) {
        if (e) pending_count=0;
        uint32_t phase=d->failed_phase;
        d->slice_started=d->engine->io.now_us(d->engine->io.ctx);
        d->failed_phase=6;
        int restored=restore_state(d);
        if (restored) { quarantine(d); return restored; }
        d->failed_phase=phase;
    } else if (!idle) { quarantine(d); return e ? e : -EIO; }
    if (!e) d->failed_phase=0;
    return e;
}
static bool qualified(void *device,uint32_t generation)
{
    struct matrox3d_device *d=device;
    return d && d->qualified && !d->lost && d->generation==generation && d->engine &&
        d->engine->active && !d->engine->quarantined;
}
static int enter(void *device)
{
    struct matrox3d_device *d=device;
    if (!d || d->locked || d->lost || !d->memory.enter) return -EBUSY;
    int e=d->memory.enter(d->memory.ctx,d->generation);
    if (!e) d->locked=true;
    return e;
}
static void leave(void *device)
{
    struct matrox3d_device *d=device;
    d->locked=false; d->memory.leave(d->memory.ctx);
}
static int idle(void *device)
{
    struct matrox3d_device *d=device;
    return d->engine->cpu_safe && !d->engine->quarantined ? 0 : -EIO;
}
static void quarantine(void *device)
{
    struct matrox3d_device *d=device;
    if (!d || d->lost) return;
    d->qualified=d->fullsize_qualified=d->perspective_qualified=false; d->lost=true;
    matrox_engine_quarantine(d->engine,"render3d_fault");
    if (d->lost_notify) d->lost_notify(d->memory.ctx);
}
extern int matrox3d_owner_attach(struct render3d_service *,const struct render3d_host *,uint32_t) __attribute__((weak));
static void detach(struct render3d_service *s,void *owner)
{
    struct matrox3d_device *d=owner;
    if (d && d->consumer==s) d->consumer=0;
}
static void query_caps(void *owner,struct ciuki_r3d_caps *out)
{
    const struct matrox3d_device *d=owner;
    *out=*matrox3d_backend()->capabilities;
    if (d && !d->g400) { out->texture_units=1; out->combine_modes=11; }
    if (d && d->fullsize_qualified && !d->lost) {
        out->max_size=d->mode_width>d->mode_height ? d->mode_width : d->mode_height;
        out->max_width=d->mode_width; out->max_height=d->mode_height;
        out->max_vertices=R3D_VERTICES;
        out->max_pixel_work=4*d->mode_width*d->mode_height;
        out->sampling_features=CIUKI_R3D_AFFINE_EXACT;
        if (d->perspective_qualified) {
            out->sampling_features|=CIUKI_R3D_PROJECTIVE_BILINEAR;
            out->max_texture_size=256; out->blend_factors=1023;
        }
    }
}
static uint32_t texture_pool_bytes(void *owner)
{
    const struct matrox3d_device *d=owner;
    return d && d->texture_bytes ? d->texture_bytes : MATROX3D_TEXTURE_MIN;
}
const struct render3d_backend *matrox3d_backend(void)
{
    static const struct ciuki_r3d_caps capabilities={
        .version=CIUKI_R3D_VERSION,.features=R3D_ALL_FEATURES,.max_size=64,
        .max_vertices=3,.texture_units=2,.max_texture_size=64,
        .depth_formats=CIUKI_R3D_D16,.color_formats=CIUKI_R3D_ARGB8888,
        .texture_formats=63,.depth_functions=253,.alpha_functions=255,
        .blend_factors=15,.combine_modes=15,.filters=31,.max_pixel_work=4096};
    static const struct render3d_backend backend={.features=R3D_ALL_FEATURES,.qualified=qualified,
        .layout=layout,.validate=validate,.enter=enter,.leave=leave,.execute=execute,
        .idle=idle,.quarantine=quarantine,.attach=matrox3d_owner_attach,.max_size=64,.detach=detach,
        .texture_pool_bytes=texture_pool_bytes,.capabilities=&capabilities,.query_caps=query_caps,
        .resident_bytes=(sizeof(struct matrox3d_device)+4095u)&~4095u};
    return &backend;
}

/* D-F2: positive 2D packet after every probe, confined to its claimed,
 * guarded scratch colour target. Never touch the desktop as a self-test. */
static int post_probe_2d(struct matrox3d_device *d)
{
    struct render3d_resource *r=&d->consumer->contexts[0].resources[0];
    if (!extent(d,r) || !d->engine->cpu_safe || d->lost) return -EIO;
    d->slice_started=d->engine->io.now_us(d->engine->io.ctx);
    int e=target_state(d,r,0);
    if (!e) e=clear_resource(d,r,0xa53c96e1u);
    if (!e) e=sync(d);
    if (!e && (memory_read(d,base(r),4)!=0xa53c96e1u ||
        memory_read(d,base(r)+(r->height-1)*hw_pitch(r)+(r->width-1)*4,4)!=0xa53c96e1u)) e=-EIO;
    if (!e) e=guards(d,r,false);
    if (d->engine->cpu_safe && !d->engine->quarantined) {
        pending_count=0;
        d->slice_started=d->engine->io.now_us(d->engine->io.ctx);
        int restored=restore_state(d); if (restored) e=restored;
    }
    if (e) quarantine(d);
    return e;
}

/* ---- boot qualification -------------------------------------------------
 * Positive probes only inside the claimed heap. Every scene is shared with
 * the Savage programme so the same evidence is produced per family. */
static int minimum_scene(struct matrox3d_device *d,struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    memset(q,0,sizeof(*q)); q->flags=R3D_FEATURES; q->count=3; q->clear_depth=65535;
    q->vertices[0]=(struct render3d_vertex){8<<16,8<<16,16384,65536,0xffffffffu,0,0};
    q->vertices[1]=(struct render3d_vertex){56<<16,8<<16,16384,65536,0xffffffffu,65536,0};
    q->vertices[2]=(struct render3d_vertex){8<<16,56<<16,16384,65536,0xffffffffu,0,65536};
    for (unsigned i=0;i<4;i++) r[i]->resident=false;
    int e=validate(d,q,r); if (e) return e;
    e=execute(d,q,r);
    if (!e) {
        d->failed_phase=7;
        uint32_t color=((uint32_t *)r[0]->shadow)[16*64+16]&0xffffffu;
        uint32_t copy=((uint32_t *)r[3]->shadow)[16*64+16]&0xffffffu;
        uint32_t depth=((uint16_t *)r[1]->shadow)[16*64+16];
        d->expected=0x4080c0u; d->actual=color;
        if (color!=d->expected || copy!=d->expected || depth<16383 || depth>16385 ||
            (((uint32_t *)r[0]->shadow)[12*64+48]&0xffffffu)!=0xc04080u ||
            (((uint32_t *)r[0]->shadow)[63*64+63]&0xffffffu)!=0 ||
            ((uint16_t *)r[1]->shadow)[63*64+63]!=65535) e=-EILSEQ;
        if (copy!=0x4080c0u) d->actual=copy;
        if (depth<16383 || depth>16385) { d->expected=16384; d->actual=depth; }
    }
    if (!e) {
        q->flags=R3D_TEXTURED|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY;
        for (unsigned i=0;i<3;i++) { q->vertices[i].z=49152; q->vertices[i].color=0xffff0000u; }
        e=execute(d,q,r);
        if (!e) d->failed_phase=7;
        if (!e && ((((uint32_t *)r[0]->shadow)[16*64+16]&0xffffffu)!=0x4080c0u ||
            (((uint32_t *)r[3]->shadow)[16*64+16]&0xffffffu)!=0x4080c0u)) e=-EILSEQ;
    }
    if (!e) {
        q->flags=R3D_FEATURES;
        for (unsigned i=0;i<3;i++) { q->vertices[i].z=16384; q->vertices[i].color=0xff000000u|(0xffu<<(16-i*8)); }
        e=execute(d,q,r);
        if (!e) {
            d->failed_phase=7;
            uint32_t pixel=((uint32_t *)r[0]->shadow)[16*64+16];
            const uint32_t channels[]={34,23,41};
            for (unsigned i=0;i<3;i++) {
                uint32_t c=(pixel>>(i*8))&255;
                if (c+2<channels[i] || c>channels[i]+2) { d->expected=0x00291722; d->actual=pixel; e=-EILSEQ; }
            }
        }
    }
    if (!e) for (unsigned i=0;i<64*64;i++)
        if ((((uint32_t *)r[0]->shadow)[i]&0xffffffu)!=(((uint32_t *)r[3]->shadow)[i]&0xffffffu)) { e=-EILSEQ; break; }
    if (!e) e=idle(d);
    return e;
}
int matrox3d_probe(struct matrox3d_device *d)
{
    if (!d || !d->engine || !d->memory.read || !d->memory.write || !d->memory.enter ||
        !d->memory.leave || !d->generation || !d->heap.bytes || (d->heap.first&4095u) ||
        d->heap.first>UINT32_MAX-d->heap.bytes || d->heap.bytes<MATROX3D_TEXTURE_MIN+96*1024u ||
        !d->engine->active || d->engine->quarantined) return -ENODEV;
    if (!d->consumer) return -ENOMEM;
    struct render3d_context *c=&d->consumer->contexts[0];
    if (!c->resources) return -ENOMEM;
    struct render3d_resource *r[R3D_REFS]={0};
    for (unsigned i=0;i<4;i++) r[i]=&c->resources[i];
    struct render3d_request *q=(void *)d->consumer->ring[0];
    int e=enter(d); if (e) return e;
    d->probing=true;
    for (unsigned encoding=0;encoding<MATROX3D_ENCODINGS;encoding++) {
        d->encoding=MATROX3D_ENCODINGS-1-encoding;
        e=minimum_scene(d,q,r);
        /* A restored numeric mismatch tries the next setup encoding; any
         * engine, guard or restore fault stops here and quarantines. */
        if (!e || e!=-EILSEQ || d->lost) break;
    }
    if (!d->lost && d->engine->cpu_safe) {
        int check=post_probe_2d(d); if (check) e=check;
    }
    d->probing=false;
    if (e && e!=-EILSEQ) quarantine(d);
    else if (!e) { d->qualified=true; d->failed_phase=0; }
    leave(d); return e;
}
static void fullsize_scene(const struct matrox3d_device *d,struct render3d_request *q,unsigned w,unsigned h,unsigned phase)
{
    memset(q,0,sizeof(*q));
    q->flags=R3D_TEXTURED|(d->g400 ? R3D_TEXTURE1 : 0)|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY;
    if (!phase) q->flags|=R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH;
    q->clear_color=0xff102030; q->clear_depth=65535; q->count=6;
    q->state.sampler[0]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,0,0};
    q->state.sampler[1]=(struct render3d_sampler){R3D_MODULATE,R3D_BILINEAR,0,0};
    if (phase==2) {
        q->state.blend=1; q->state.source=R3D_SRC_ALPHA; q->state.destination=R3D_INV_SRC_ALPHA;
        q->state.sampler[0].filter=R3D_BILINEAR_MIP;
        q->state.sampler[1].filter=R3D_TRILINEAR;
        q->state.sampler[0].repeat_u=q->state.sampler[0].repeat_v=1;
        q->state.sampler[1].repeat_u=q->state.sampler[1].repeat_v=1;
    }
    static const unsigned corners[6][2]={{0,0},{1,0},{0,1},{1,0},{1,1},{0,1}};
    for (unsigned i=0;i<6;i++) {
        unsigned x=corners[i][0],y=corners[i][1];
        q->vertices[i]=(struct render3d_vertex){(int32_t)(x*w<<16),(int32_t)(y*h<<16),
            phase==1 ? 49152u : phase==2 ? 8192u : 32768u,
            65536,phase==1 ? 0xffff0000u : phase==2 ? 0x8090b0d0u : 0xffe0c0a0u,
            (int32_t)(x*(phase==2 ? 16 : 1)*65536),(int32_t)(y*(phase==2 ? 16 : 1)*65536)};
        if (q->flags&R3D_TEXTURE1) { q->uv1[i][0]=q->vertices[i].v; q->uv1[i][1]=q->vertices[i].u; }
    }
    if (phase==3) {
        q->count=R3D_VERTICES;
        for (unsigned triangle=0;triangle<256;triangle++) for (unsigned v=0;v<3;v++) {
            unsigned i=triangle*3+v;
            unsigned x=(triangle%16)*(w-16)/15+(v==1 ? 16 : 0);
            unsigned y=(triangle/16)*(h-16)/15+(v==2 ? 16 : 0);
            q->vertices[i]=(struct render3d_vertex){x<<16,y<<16,4096,65536,0xffc0e0a0u,
                v==1 ? 49152 : 16384,v==2 ? 49152 : 16384};
            if (q->flags&R3D_TEXTURE1) { q->uv1[i][0]=q->vertices[i].v; q->uv1[i][1]=q->vertices[i].u; }
        }
    }
}
static int fullsize_compare(struct matrox3d_device *d,struct render3d_resource *const hw[R3D_REFS],
                            struct render3d_resource *const ref[R3D_REFS])
{
    const uint32_t *a=hw[0]->shadow,*b=ref[0]->shadow,*copy=hw[3]->shadow;
    const uint16_t *az=hw[1]->shadow,*bz=ref[1]->shadow;
    uint32_t ah=2166136261u,bh=2166136261u;
    for (unsigned i=0;i<hw[0]->width*hw[0]->height;i++) {
        unsigned row=i/hw[0]->width;
        if (row%16 && row!=hw[0]->height-1) continue;
        if (a[i]!=copy[i] || (a[i]==0xff102030u)!=(b[i]==0xff102030u)) return -EILSEQ;
        for (unsigned shift=0;shift<32;shift+=8) {
            unsigned ac=a[i]>>shift&255,bc=b[i]>>shift&255,delta=ac>bc ? ac-bc : bc-ac;
            if (delta>d->max_channel_error) d->max_channel_error=delta;
            if (delta>2) { d->expected=b[i]; d->actual=a[i]; return -EILSEQ; }
        }
        unsigned delta=az[i]>bz[i] ? az[i]-bz[i] : bz[i]-az[i];
        if (delta>d->max_depth_error) d->max_depth_error=delta;
        if (delta>1) { d->expected=bz[i]; d->actual=az[i]; return -EILSEQ; }
        ah=(ah^a[i])*16777619u; ah=(ah^az[i])*16777619u;
        bh=(bh^b[i])*16777619u; bh=(bh^bz[i])*16777619u;
        d->sample_count++;
    }
    d->hardware_checksum=ah; d->reference_checksum=bh; return 0;
}
#include "../render3d_precision.h"
static void precision_scene(const struct matrox3d_device *d,struct render3d_request *q,struct render3d_resource *const r[R3D_REFS],unsigned phase)
{
    fullsize_scene(d,q,r[0]->width,r[0]->height,0);
    unsigned stages=d->g400 ? 2 : 1;
    for (unsigned stage=0;stage<stages;stage++) {
        struct render3d_resource *t=r[stage ? 4 : 2];
        for (unsigned level=0;level<t->levels;level++) {
            unsigned width=t->width>>level,height=t->height>>level;
            if (!width) width=1;
            if (!height) height=1;
            uint32_t *pixels=(void *)((uint8_t *)t->shadow+t->level_offset[level]);
            for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++) {
                unsigned ramp=phase&1 ? ((x+y+level)&1)*255 : width>1 ? x*255/(width-1) : level*37;
                unsigned cross=phase&1 ? ((x/2+y/3+level)&1)*255 : height>1 ? y*255/(height-1) : level*29;
                unsigned alpha=phase>=9 ? ((x*17+y*29+level*37)&255) : 255;
                pixels[y*width+x]=(alpha<<24)|(stage ? 0xff0000u|(ramp<<8)|cross : 0x00ff00u|(ramp<<16)|cross);
                if (phase==6) pixels[y*width+x]=stage ? 0xffff00ffu|(((x&1)*255)<<8) : 0xff00ffffu|(((x&1)*255)<<16);
            }
        }
        t->resident=false;
        q->state.sampler[stage]=(struct render3d_sampler){R3D_MODULATE,
            phase<4 || (phase>=6 && phase<10) || phase>=21 ? R3D_BILINEAR : phase==4 || phase==10 ? R3D_BILINEAR_MIP : R3D_TRILINEAR,phase<21,phase<21};
    }
    for (unsigned i=0;i<6;i++) {
        struct render3d_vertex *v=&q->vertices[i];
        unsigned x=v->x!=0,y=v->y!=0;
        v->rhw=phase<2 ? (x ? 131072 : 32768) : (y ? 32768 : 131072);
        v->color=0xffffffffu;
        v->u=(int32_t)(x*4*65536)-2*65536+1385;
        v->v=(int32_t)(y*2*65536)-65536+1190;
        q->uv1[i][0]=(int32_t)(y*3*65536)-65536+3522;
        q->uv1[i][1]=(int32_t)(x*2*65536)-65536+3570;
        if (phase==4 || phase==5) {
            unsigned span=d->g400 ? 2 : 1; /* G200 has only five mip origins */
            v->u=(int32_t)(x*span*65536)-(int32_t)(span/2*65536)+1385;
            v->v=(int32_t)(y*span*65536)-(int32_t)(span/2*65536)+1190;
            q->uv1[i][0]=(int32_t)(y*span*65536)-(int32_t)(span/2*65536)+3522;
            q->uv1[i][1]=(int32_t)(x*span*65536)-(int32_t)(span/2*65536)+3570;
        }
        if (phase==6) {
            v->rhw=65536;
            v->u=(32768+x*65536)/r[2]->width; v->v=32768/r[2]->height;
            q->uv1[i][0]=(32768+(1-x)*65536)/r[4]->width; q->uv1[i][1]=32768/r[4]->height;
        }
        if (phase>=7) {
            v->rhw=(x ? 256u : 64u)*65536;
            unsigned span=phase>=10 && phase<21 ? (d->g400 ? 2 : 1) : 28;
            v->u=(int32_t)(x*span*65536)-(int32_t)(span/2*65536)+1385;
            v->v=(int32_t)(y*span*65536)-(int32_t)(span/2*65536)+1190;
            q->uv1[i][0]=(int32_t)(y*span*65536)-(int32_t)(span/2*65536)+3522;
            q->uv1[i][1]=(int32_t)(x*span*65536)-(int32_t)(span/2*65536)+3570;
        }
        if (phase>=21) {
            v->u=(phase==21 ? -16 : 16)*65536; v->v=-v->u;
            q->uv1[i][0]=-v->u; q->uv1[i][1]=v->u;
        }
        if (!(q->flags&R3D_TEXTURE1)) q->uv1[i][0]=q->uv1[i][1]=0;
    }
    if (phase>=9 && phase<21) {
        q->state.alpha_test=1; q->state.alpha_func=R3D_GEQUAL; q->state.alpha_ref=64;
    }
    if (phase>=11 && phase<21) {
        static const unsigned src[]={R3D_ONE,R3D_SRC_ALPHA,R3D_ONE,R3D_DST_COLOR,R3D_ZERO,R3D_ONE,
            R3D_INV_DST_COLOR,R3D_DST_ALPHA,R3D_INV_DST_ALPHA,R3D_INV_SRC_ALPHA};
        static const unsigned dst[]={R3D_ONE,R3D_INV_SRC_ALPHA,R3D_INV_SRC_ALPHA,R3D_ZERO,R3D_SRC_COLOR,R3D_ZERO,
            R3D_INV_SRC_COLOR,R3D_INV_DST_ALPHA,R3D_DST_ALPHA,R3D_SRC_ALPHA};
        q->state.blend=1; q->state.source=src[phase-11]; q->state.destination=dst[phase-11];
    }
}
static int precision_probe(struct matrox3d_device *d,struct render3d_request *q,
    struct render3d_resource *const hardware[R3D_REFS],struct render3d_resource *const reference[R3D_REFS])
{
    d->sample_delta_q16=CIUKI_R3D_SAMPLE_DELTA_Q16;
    d->filter_fraction_bits=CIUKI_R3D_FILTER_FRACTION_BITS;
    int e=0;
    for (unsigned phase=0;phase<23 && !e;phase++) {
        struct render3d_resource *hw[R3D_REFS],*ref[R3D_REFS];
        for (unsigned i=0;i<R3D_REFS;i++) { hw[i]=hardware[i]; ref[i]=reference[i]; }
        if (phase==8 || phase==10 || phase>=16) {
            hw[2]=hardware[4]; hw[4]=hardware[2]; ref[2]=reference[4]; ref[4]=reference[2];
        }
        uint32_t levels[2][8]={{0}};
        d->precision_phase=phase+1;
        precision_scene(d,q,hw,phase);
        struct render3d_resource *rr[R3D_REFS]; memcpy(rr,ref,sizeof(rr));
        if (!(q->flags&R3D_TEXTURE1)) { hw[4]=0; rr[4]=0; }
        for (unsigned t=2;t<=4;t+=2) if (hw[t]) memcpy(rr[t]->shadow,hw[t]->shadow,hw[t]->bytes);
        struct render3d_job job={0};
        while (!render3d_scalar_slice(q,rr,&job,&d->consumer->host)) {}
        e=validate(d,q,hw); if (e) { if (e==-EOPNOTSUPP) e=-EILSEQ; break; }
        e=enter(d); if (e) break;
        uint64_t start=d->engine->io.now_us(d->engine->io.ctx);
        e=execute(d,q,hw);
        d->fullsize_us+=d->engine->io.now_us(d->engine->io.ctx)-start;
        if (e && (e!=-ETIMEDOUT || idle(d))) quarantine(d);
        leave(d);
        if (e) break;
        const uint32_t *a=hw[0]->shadow,*copy=hw[3]->shadow;
        const uint16_t *az=hw[1]->shadow;
        for (unsigned y=0;y<hw[0]->height && !e;y++) {
            if (y%16 && y!=hw[0]->height-1) continue;
            for (unsigned x=0;x<hw[0]->width;x++) {
                unsigned at=y*hw[0]->width+x;
                if (a[at]!=copy[at] ||
                    !precision_pixel(q,hw,x,y,a[at],az[at],d->sample_delta_q16,d->filter_fraction_bits)) {
                    d->precision_x=x; d->precision_y=y;
                    d->expected=((uint32_t *)ref[0]->shadow)[at]; d->actual=a[at]; e=-EILSEQ; break;
                }
                d->precision_samples++;
                if (phase==6) {
                    unsigned r=a[at]>>16&255,g=a[at]>>8&255;
                    levels[0][r/32]|=1u<<(r%32); levels[1][g/32]|=1u<<(g%32);
                }
            }
        }
        if (phase==6 && !e) for (unsigned stage=0;stage<(d->g400 ? 2u : 1u);stage++) {
            unsigned count=0;
            for (unsigned i=0;i<8;i++) for (unsigned bit=0;bit<32;bit++) count+=(levels[stage][i]>>bit)&1;
            d->measured_filter_levels[stage]=count;
            if (count<192) e=-EILSEQ;
        }
    }
    if (!e) d->precision_phase=0;
    return e;
}
int matrox3d_fullsize_probe(struct matrox3d_device *d,struct render3d_service *reference)
{
    if (!d || !d->qualified || d->lost || d->fullsize_qualified || !d->consumer || !reference ||
        d->mode_width<64 || d->mode_height<64 || d->mode_width>1024 || d->mode_height>1024 ||
        !d->consumer->contexts[0].resources || !reference->contexts[0].resources) return -ENODEV;
    struct render3d_resource *hw[R3D_REFS],*ref[R3D_REFS];
    for (unsigned i=0;i<R3D_REFS;i++) {
        hw[i]=&d->consumer->contexts[0].resources[i]; ref[i]=&reference->contexts[0].resources[i];
        if (!hw[i]->shadow || !ref[i]->shadow || hw[i]->bytes!=ref[i]->bytes) return -EINVAL;
    }
    if (hw[0]->width!=d->mode_width || hw[0]->height!=d->mode_height) return -EINVAL;
    d->fullsize_testing=true;
    struct render3d_request *q=(void *)d->consumer->ring[0];
    int e=0;
    for (unsigned phase=0;phase<4 && !e;phase++) {
        fullsize_scene(d,q,d->mode_width,d->mode_height,phase);
        struct render3d_resource *hh[R3D_REFS],*rr[R3D_REFS];
        memcpy(hh,hw,sizeof(hh)); memcpy(rr,ref,sizeof(rr));
        if (!(q->flags&R3D_TEXTURE1)) { hh[4]=0; rr[4]=0; }
        e=validate(d,q,hh); if (e) break;
        struct render3d_job job={0};
        while (!render3d_scalar_slice(q,rr,&job,&reference->host)) {}
        e=enter(d); if (e) break;
        uint64_t start=d->engine->io.now_us(d->engine->io.ctx);
        e=execute(d,q,hh);
        d->fullsize_us+=d->engine->io.now_us(d->engine->io.ctx)-start;
        if (e && (e!=-ETIMEDOUT || idle(d))) quarantine(d);
        leave(d);
        if (!e) { d->failed_phase=9; e=fullsize_compare(d,hw,ref); }
    }
    d->fullsize_testing=false;
    if (!e) {
        d->fullsize_qualified=true; d->failed_phase=0;
        d->fullsize_testing=true;
        d->precision_error=precision_probe(d,q,hw,ref);
        d->fullsize_testing=false;
        d->perspective_qualified=!d->precision_error;
        if (d->lost || (d->precision_error && d->precision_error!=-EILSEQ && d->precision_error!=-ETIMEDOUT))
            e=d->precision_error;
    }
    if (!d->lost && d->engine->cpu_safe) {
        int check=enter(d);
        if (!check) { check=post_probe_2d(d); leave(d); }
        if (check) { quarantine(d); e=check; }
    }
    if (!e) d->failed_phase=0;
    else if (!d->precision_error) d->precision_error=e;
    return e;
}
