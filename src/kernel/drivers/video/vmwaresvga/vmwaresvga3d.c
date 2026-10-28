/* SVGA3D legacy (VGPU9) fixed-function backend for the shared SVGA II owner.
 * Independent implementation from the pinned protocol headers and hosts cited
 * in docs/design/f7-family-vmware-svga3d.md; no upstream code is vendored.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/display_driver.h>
#include "vmwaresvga3d.h"

#define FIFO(s,r) ((s)->io.fifo_read((s)->io.ctx,4u*(r)))
#define IEEE_ONE 0x3f800000u

/* Correctly rounded binary32 of numerator/denominator (both <= 2^48), with
 * no floating point: shift-subtract division and round-half-to-even. */
uint32_t vmsvga3d_ratio_ieee(uint64_t n,uint64_t d,bool negative)
{
    if (!n || !d || n>(1ull<<48) || d>(1ull<<48)) return 0;
    int exponent=0;
    while (n>=(d<<1)) { d<<=1; exponent++; }
    while (n<d) { n<<=1; exponent--; }
    uint32_t mantissa=1; uint64_t rest=n-d;
    for (unsigned i=0;i<23;i++) {
        rest<<=1; mantissa<<=1;
        if (rest>=d) { rest-=d; mantissa|=1; }
    }
    rest<<=1;
    if (rest>d || (rest==d && (mantissa&1))) mantissa++;
    if (mantissa==(1u<<24)) { mantissa>>=1; exponent++; }
    return (negative ? 0x80000000u : 0)|((uint32_t)(exponent+127)<<23)|(mantissa&0x7fffffu);
}
uint32_t vmsvga3d_fixed_ieee(int32_t q16)
{
    uint64_t magnitude=q16<0 ? (uint64_t)(-(int64_t)q16) : (uint64_t)q16;
    return vmsvga3d_ratio_ieee(magnitude,65536,q16<0);
}
static bool varying_w(const struct render3d_request *q,unsigned first)
{
    return q->vertices[first].rhw!=q->vertices[first+1].rhw || q->vertices[first].rhw!=q->vertices[first+2].rhw;
}
/* POSITION FLOAT4 is homogeneous (X,Y,Z,W). Constant reciprocal W uses W=1;
 * varying W emits (x,y,z/65535)/rhw with W=1/rhw so the host's clip-space
 * divide restores screen x,y,z and interpolates texture coordinates with
 * the request's reciprocal W (D3D9 fixed-function vertex processing and
 * VirtualBox DevVGA-SVGA3d-ogl.cpp:6893-6900, glVertexPointer(4)). */
int vmsvga3d_vertex(const struct render3d_request *q,unsigned i,uint32_t out[VMSVGA3D_VERTEX_WORDS])
{
    if (!q || !out || i>=R3D_VERTICES || i>=q->count) return -EINVAL;
    const struct render3d_vertex *v=&q->vertices[i];
    if (v->x<0 || v->y<0 || v->x>(1024<<16) || v->y>(1024<<16) || v->z>65535 ||
        v->rhw<256 || v->rhw>16777216 || v->u < -16*65536 || v->u>16*65536 ||
        v->v < -16*65536 || v->v>16*65536 || q->uv1[i][0] < -16*65536 || q->uv1[i][0]>16*65536 ||
        q->uv1[i][1] < -16*65536 || q->uv1[i][1]>16*65536) return -EINVAL;
    if (!varying_w(q,i-i%3)) {
        out[0]=vmsvga3d_fixed_ieee(v->x); out[1]=vmsvga3d_fixed_ieee(v->y);
        out[2]=vmsvga3d_ratio_ieee(v->z,65535,false); out[3]=IEEE_ONE;
    } else {
        out[0]=vmsvga3d_ratio_ieee((uint32_t)v->x,v->rhw,false);
        out[1]=vmsvga3d_ratio_ieee((uint32_t)v->y,v->rhw,false);
        out[2]=vmsvga3d_ratio_ieee((uint64_t)v->z*65536u,(uint64_t)v->rhw*65535u,false);
        out[3]=vmsvga3d_ratio_ieee(65536,v->rhw,false);
    }
    out[4]=q->state.flat ? q->flat_color[i/3] : v->color; /* D3DCOLOR = 0xAARRGGBB */
    out[5]=vmsvga3d_fixed_ieee(v->u); out[6]=vmsvga3d_fixed_ieee(v->v);
    out[7]=vmsvga3d_fixed_ieee(q->uv1[i][0]); out[8]=vmsvga3d_fixed_ieee(q->uv1[i][1]);
    return 0;
}
/* Row-vector projection v*P from target pixels to clip space. D3D9 places
 * pixel centres on integer viewport coordinates; render3d samples at x+0.5,
 * so P also shifts by half a pixel: x_ndc=2(x-1/2)/W-1, y_ndc=1-2(y-1/2)/H,
 * z_ndc=z. Exact rationals, correctly rounded. */
void vmsvga3d_projection(uint32_t w,uint32_t h,uint32_t m[16])
{
    memset(m,0,16*sizeof(*m));
    if (!w || !h || w>1024 || h>1024) return;
    m[0]=vmsvga3d_ratio_ieee(2,w,false);
    m[5]=vmsvga3d_ratio_ieee(2,h,true);
    m[10]=IEEE_ONE;
    m[12]=vmsvga3d_ratio_ieee(w+1u,w,true);
    m[13]=vmsvga3d_ratio_ieee(h+1u,h,false);
    m[15]=IEEE_ONE;
}

/* Capability gates (design section "Capability gates"): SVGA_CAP_3D,
 * extended FIFO registers through SVGA_FIFO_3D_CAPS_LAST, FIFO fences, a 3D
 * hardware version >= WS8_B1 (Linux v6.1 vmwgfx_cmd.c:35-79 reads the
 * REVISED register when SVGA_FIFO_CAP_3D_HWVERSION_REVISED is set), then the
 * SVGA3dCapsRecord block (VirtualBox DevVGA-SVGA.cpp:8479-8506 writes it). */
static void devcap(struct vmsvga3d_caps *c,uint32_t index,uint32_t value)
{
    if (index<96) c->seen[index/32]|=1u<<(index%32);
    switch (index) {
    case VMSVGA3D_DEVCAP_3D: c->three_d=value; break;
    case VMSVGA3D_DEVCAP_MAX_TEXTURES: c->max_textures=value; break;
    case VMSVGA3D_DEVCAP_D16_BUFFER_FORMAT: c->d16_buffer=value; break;
    case VMSVGA3D_DEVCAP_MAX_TEXTURE_WIDTH: c->max_texture_width=value; break;
    case VMSVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT: c->max_texture_height=value; break;
    case VMSVGA3D_DEVCAP_MAX_PRIMITIVE_COUNT: c->max_primitives=value; break;
    case VMSVGA3D_DEVCAP_MAX_VERTEX_INDEX: c->max_vertex_index=value; break;
    case VMSVGA3D_DEVCAP_TEXTURE_OPS: c->texture_ops=value; break;
    case VMSVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8: c->argb_ops=value; break;
    case VMSVGA3D_DEVCAP_SURFACEFMT_Z_D16: c->d16_ops=value; break;
    case VMSVGA3D_DEVCAP_MAX_CONTEXT_IDS: c->max_context_ids=value; break;
    case VMSVGA3D_DEVCAP_MAX_SURFACE_IDS: c->max_surface_ids=value; break;
    default: break;
    }
}
static bool seen(const struct vmsvga3d_caps *c,uint32_t index)
{ return index<96 && (c->seen[index/32]>>(index%32)&1u); }
int vmsvga3d_read_caps(const struct vmsvga_engine *s,struct vmsvga3d_caps *c,const char **reason)
{
    const char *unused; if (!reason) reason=&unused;
    if (c) memset(c,0,sizeof(*c));
    if (!s || !c || !s->io.fifo_read) { *reason="no_engine"; return -EINVAL; }
    if (!s->initialized || !s->active || s->quarantined) { *reason="engine_inactive"; return -ENODEV; }
    if (!(s->caps&SVGA_CAP_3D)) { *reason="no_svga3d"; return -EOPNOTSUPP; }
    if (!(s->caps&SVGA_CAP_EXTENDED_FIFO) || s->min<4u*(SVGA_FIFO_3D_CAPS_LAST+1u)) {
        *reason="fifo_registers"; return -EOPNOTSUPP;
    }
    if (!(s->fifo_caps&SVGA_FIFO_CAP_FENCE)) { *reason="no_fifo_fence"; return -EOPNOTSUPP; }
    if (s->io.valid && !s->io.valid(s->io.ctx)) { *reason="resource_revoked"; return -EIO; }
    c->hwversion=FIFO(s,(s->fifo_caps&SVGA_FIFO_CAP_3D_HWVERSION_REVISED) ?
        SVGA_FIFO_3D_HWVERSION_REVISED : SVGA_FIFO_3D_HWVERSION);
    const uint32_t required[]={VMSVGA3D_DEVCAP_MAX_TEXTURES,VMSVGA3D_DEVCAP_D16_BUFFER_FORMAT,
        VMSVGA3D_DEVCAP_MAX_TEXTURE_WIDTH,VMSVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT,
        VMSVGA3D_DEVCAP_MAX_PRIMITIVE_COUNT,VMSVGA3D_DEVCAP_MAX_VERTEX_INDEX,
        VMSVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8,VMSVGA3D_DEVCAP_SURFACEFMT_Z_D16,
        VMSVGA3D_DEVCAP_MAX_CONTEXT_IDS,VMSVGA3D_DEVCAP_MAX_SURFACE_IDS};
    c->gb_objects=(s->caps&VMSVGA3D_CAP_GBOBJECTS)!=0;
    if (c->gb_objects) {
        /* Linux v6.1: GB hosts gate on SVGA3D_DEVCAP_3D (vmwgfx_cmd.c:43-51)
         * read through SVGA_REG_DEV_CAP (vmwgfx_devcaps.c:85-100). */
        if (!s->io.read || !s->io.write) { *reason="no_dev_cap"; return -EOPNOTSUPP; }
        const uint32_t extra[]={VMSVGA3D_DEVCAP_3D,VMSVGA3D_DEVCAP_TEXTURE_OPS};
        for (unsigned i=0;i<sizeof(required)/sizeof(*required)+2;i++) {
            uint32_t index=i<2 ? extra[i] : required[i-2];
            s->io.write(s->io.ctx,VMSVGA3D_REG_DEV_CAP,index);
            devcap(c,index,s->io.read(s->io.ctx,VMSVGA3D_REG_DEV_CAP));
        }
        if (!c->three_d) { *reason="devcap_3d"; return -EOPNOTSUPP; }
    } else {
        if (!c->hwversion || c->hwversion==UINT32_MAX || c->hwversion<VMSVGA3D_HWVERSION_WS8_B1) {
            *reason="hwversion"; return -EOPNOTSUPP;
        }
        unsigned at=SVGA_FIFO_3D_CAPS,records=0;
        while (at<=SVGA_FIFO_3D_CAPS_LAST) {
            uint32_t length=FIFO(s,at);
            if (!length) break;
            if (length<2 || length>SVGA_FIFO_3D_CAPS_LAST+1u-at || ++records>64) {
                *reason="caps_record"; return -EOPNOTSUPP;
            }
            uint32_t type=FIFO(s,at+1);
            if (type>=VMSVGA3D_CAPS_RECORD_DEVCAPS_MIN && type<=VMSVGA3D_CAPS_RECORD_DEVCAPS_MAX)
                for (unsigned p=at+2;p+1<at+length;p+=2) devcap(c,FIFO(s,p),FIFO(s,p+1));
            at+=length;
        }
    }
    /* A present but zero DEVCAP_3D is an explicit host refusal. */
    if (seen(c,VMSVGA3D_DEVCAP_3D) && !c->three_d) { *reason="devcap_3d"; return -EOPNOTSUPP; }
    for (unsigned i=0;i<sizeof(required)/sizeof(*required);i++)
        if (!seen(c,required[i])) { *reason="devcap_missing"; return -EOPNOTSUPP; }
    if (!c->max_textures || !c->d16_buffer || c->max_texture_width<64 || c->max_texture_height<64 ||
        c->max_primitives<R3D_VERTICES/3 || c->max_vertex_index<R3D_VERTICES-1 ||
        (c->argb_ops&(VMSVGA3D_FORMAT_OP_TEXTURE|VMSVGA3D_FORMAT_OP_OFFSCREEN_RENDERTARGET))!=
            (VMSVGA3D_FORMAT_OP_TEXTURE|VMSVGA3D_FORMAT_OP_OFFSCREEN_RENDERTARGET) ||
        !(c->d16_ops&VMSVGA3D_FORMAT_OP_ZSTENCIL) || c->max_context_ids<=VMSVGA3D_CID ||
        c->max_surface_ids<=VMSVGA3D_SID_BASE+VMSVGA3D_SIDS) { *reason="devcap_limits"; return -EOPNOTSUPP; }
    c->valid=true; *reason="none"; return 0;
}

/* Tight guest images in claimed VRAM: [guard][data][guard], page aligned.
 * Colour/textures are A8R8G8B8 (the canonical ARGB8888 shadow bytes), depth
 * is Z_D16 (the canonical uint16 depth shadow). */
uint32_t vmsvga3d_staging_bytes(const struct render3d_resource *r)
{
    if (!r || !r->width || !r->height || r->width>1024 || r->height>1024) return 0;
    uint32_t bytes;
    if (r->kind==R3D_TEXTURE) bytes=render3d_texture_shape(r) ? 0 : r->bytes;
    else if (r->kind==R3D_D16) bytes=r->width*r->height*2u;
    else if (r->kind==R3D_COLOR || r->kind==R3D_LINEAR_COLOR) bytes=r->width*r->height*4u;
    else return 0;
    return (bytes+4095u)&~4095u;
}
static bool fullsize(const struct vmsvga3d_device *d)
{ return d && (d->fullsize_qualified || d->fullsize_testing) && d->mode_width && d->mode_height; }
static bool dimensions(const struct vmsvga3d_device *d,const struct render3d_resource *r)
{ return fullsize(d) ? r->width<=d->mode_width && r->height<=d->mode_height : r->width<=64 && r->height<=64; }
static uint32_t texture_limit(const struct vmsvga3d_device *d)
{
    uint32_t limit=d->perspective_qualified || d->fullsize_testing ? 256u : 64u;
    if (d->caps.max_texture_width<limit) limit=d->caps.max_texture_width;
    if (d->caps.max_texture_height<limit) limit=d->caps.max_texture_height;
    return limit;
}
static int layout(void *device,struct render3d_resource *r)
{
    const struct vmsvga3d_device *d=device;
    if (!d || !r || r->kind<R3D_COLOR || r->kind>R3D_LINEAR_COLOR || !r->width || !r->height) return -EOPNOTSUPP;
    if (r->kind==R3D_TEXTURE ? (r->width>1024 || r->height>1024 ||
        r->width>d->caps.max_texture_width || r->height>d->caps.max_texture_height) : !dimensions(d,r)) return -EOPNOTSUPP;
    if (r->kind==R3D_TEXTURE && render3d_texture_shape(r)) return -EINVAL;
    uint32_t bytes=vmsvga3d_staging_bytes(r);
    if (!bytes) return -EINVAL;
    r->reserved=bytes+8192u;
    return 0;
}
static bool overlaps(struct render3d_extent a,struct render3d_extent b)
{ return (uint64_t)a.first+a.bytes>b.first && (uint64_t)b.first+b.bytes>a.first; }
static uint32_t service_first(const struct vmsvga3d_device *d) { return d->heap.first+VMSVGA3D_SCRATCH_BYTES; }
static uint32_t heap_end(const struct vmsvga3d_device *d) { return d->heap.first+d->heap.bytes; }
static bool heap_valid(const struct vmsvga3d_device *d)
{
    return d && d->engine && d->heap.first && d->heap.bytes && !(d->heap.first&4095u) && !(d->heap.bytes&4095u) &&
        d->heap.first<=UINT32_MAX-d->heap.bytes && d->heap.bytes>VMSVGA3D_SCRATCH_BYTES+VMSVGA3D_TEXTURE_MIN &&
        d->texture_bytes>=VMSVGA3D_TEXTURE_MIN && !(d->texture_bytes&4095u) &&
        d->texture_bytes<d->heap.bytes-VMSVGA3D_SCRATCH_BYTES && d->caps.valid;
}
static bool extent(const struct vmsvga3d_device *d,const struct render3d_resource *r)
{
    uint32_t pool=heap_end(d)-d->texture_bytes;
    return r && r->kind>=R3D_COLOR && r->kind<=R3D_LINEAR_COLOR && r->hardware && r->shadow &&
        r->width && r->height && (r->kind==R3D_TEXTURE ? !render3d_texture_shape(r) &&
        r->width<=d->caps.max_texture_width && r->height<=d->caps.max_texture_height : dimensions(d,r)) &&
        r->offset>=service_first(d) && !(r->offset&4095u) && r->reserved>8192u &&
        (uint64_t)r->offset+r->reserved<=heap_end(d) &&
        (r->kind==R3D_TEXTURE ? r->offset>=pool : (uint64_t)r->offset+r->reserved<=pool) &&
        r->reserved==vmsvga3d_staging_bytes(r)+8192u && r->pitch==r->width*(r->kind==R3D_D16 ? 2u : 4u) &&
        (r->kind==R3D_TEXTURE || r->bytes==r->pitch*r->height);
}

/* Mode-sized VRAM plan above the scanout lease (fbdev claims exactly
 * [phys, phys+size)); SVGA II scans out from offset 0 (engine.c checks
 * SVGA_REG_FB_OFFSET==0). Scratch, two colour targets, D16, texture pool. */
int vmsvga3d_mode_heap(const struct vmsvga_engine *s,const struct fb_device *fb,uint32_t vram,
                       struct render3d_extent *heap,uint32_t *required)
{
    if (!s || !fb || !heap || !required || !fb->width || !fb->height || fb->width>1024 ||
        fb->height>1024 || fb->bpp!=32 || !fb->pitch || !fb->size || vram<4096 || fb->size>vram)
        return -EOPNOTSUPP;
    uint32_t first=(fb->size+4095u)&~4095u,end=vram&~4095u;
    if (first>=end) return -ENOMEM;
    uint32_t color=(fb->width*fb->height*4u+4095u)&~4095u,depth=(fb->width*fb->height*2u+4095u)&~4095u;
    *required=VMSVGA3D_SCRATCH_BYTES+2u*(color+8192u)+depth+8192u+VMSVGA3D_TEXTURE_MIN;
    uint32_t bytes=end-first;
    if (bytes>*required+VMSVGA3D_HEAP_EXTRA) bytes=*required+VMSVGA3D_HEAP_EXTRA;
    *heap=(struct render3d_extent){first,bytes};
    return bytes<*required ? -ENOMEM : 0;
}

/* Fixed-function stage programs (D3D9 texture-stage semantics, Mesa 21.3.9
 * svga3d_types.h:1262-1290 combiners, :1345-1352 arguments). Stage 0's
 * "current" input is DIFFUSE, stage 1's is PREVIOUS. The scalar contract is
 * render3d_scalar.c combine(): RGB-only formats keep the source alpha,
 * alpha-only formats keep the source RGB. */
struct combiner { uint32_t color_op, color1, color2, alpha_op, alpha1, alpha2, keep, set, need; };
static unsigned format_class(uint32_t format)
{ return format==R3D_RGB565 || format==R3D_L8 ? 1u : format==R3D_A8 ? 2u : 0u; }
static int combiner(unsigned stage,unsigned mode,uint32_t format,struct combiner *c)
{
    uint32_t current=stage ? VMSVGA3D_TA_PREVIOUS : VMSVGA3D_TA_DIFFUSE;
    unsigned cls=format_class(format);
    *c=(struct combiner){VMSVGA3D_TC_MODULATE,VMSVGA3D_TA_TEXTURE,current,
        VMSVGA3D_TC_MODULATE,VMSVGA3D_TA_TEXTURE,current,0xffffffffu,0,0};
    if (mode==R3D_MODULATE) return 0;
    if (mode==R3D_ADD) {
        /* GL_ADD on an alpha-only texture adds zero RGB: MODULATE exactly. */
        if (cls!=2) { c->color_op=VMSVGA3D_TC_ADD; c->need=VMSVGA3D_COMBINE_ADD; }
        return 0;
    }
    if (mode==R3D_DECAL) {
        if (cls==2) return -EOPNOTSUPP; /* no GL-defined meaning (render3d contract) */
        if (!cls) {
            c->color_op=VMSVGA3D_TC_BLENDTEXTUREALPHA;
            c->alpha_op=VMSVGA3D_TC_SELECTARG1; c->alpha1=current;
            c->need=VMSVGA3D_COMBINE_DECAL; return 0;
        }
        mode=R3D_REPLACE; /* opaque RGB texture: DECAL equals REPLACE */
    }
    if (mode!=R3D_REPLACE) return -EINVAL;
    if (!stage) {
        /* MODULATE by 1.0 is exact: force the operand channels to white. */
        if (!cls) { c->keep=0; c->set=0xffffffffu; }
        else if (cls==1) { c->keep=0xff000000u; c->set=0x00ffffffu; }
        else { c->keep=0x00ffffffu; c->set=0xff000000u; }
        c->need=VMSVGA3D_COMBINE_REPLACE0; return 0;
    }
    c->color_op=c->alpha_op=VMSVGA3D_TC_SELECTARG1;
    c->color1=cls==2 ? current : VMSVGA3D_TA_TEXTURE;
    c->alpha1=cls==1 ? current : VMSVGA3D_TA_TEXTURE;
    c->need=VMSVGA3D_COMBINE_REPLACE1; return 0;
}
static uint32_t texture_op_bits(uint32_t need)
{
    if (need==VMSVGA3D_COMBINE_REPLACE1) return VMSVGA3D_TC_CAP_BIT(VMSVGA3D_TC_SELECTARG1);
    if (need==VMSVGA3D_COMBINE_ADD) return VMSVGA3D_TC_CAP_BIT(VMSVGA3D_TC_ADD)|VMSVGA3D_TC_CAP_BIT(VMSVGA3D_TC_MODULATE);
    if (need==VMSVGA3D_COMBINE_DECAL) return VMSVGA3D_TC_CAP_BIT(VMSVGA3D_TC_BLENDTEXTUREALPHA)|VMSVGA3D_TC_CAP_BIT(VMSVGA3D_TC_SELECTARG1);
    return 0;
}
static bool combine_admitted(const struct vmsvga3d_device *d,uint32_t need)
{
    if (!need) return true;
    if ((d->caps.texture_ops&texture_op_bits(need))!=texture_op_bits(need)) return false;
    return (d->combine&need) || (d->combine_trial&need);
}
/* Same sample/LOD proof as the f7-11 Savage admission (savage3d.c
 * mip_gradient), evaluated on native inputs; family-neutral by design. */
static bool mip_gradient(const struct render3d_request *q,unsigned first,unsigned stage,
                         const struct render3d_resource *t)
{
    const struct render3d_vertex *v=q->vertices+first;
    int64_t x[3],y[3],lo=UINT32_MAX,hi=0,perimeter=0;
    unsigned magnitude=0;
    for (unsigned i=0;i<3;i++) {
        if ((v[i].x&4095) || (v[i].y&4095) || v[i].rhw<32768 || v[i].rhw>16777216) return false;
        x[i]=v[i].x/4096; y[i]=v[i].y/4096;
        if (v[i].rhw<lo) lo=v[i].rhw;
        if (v[i].rhw>hi) hi=v[i].rhw;
        for (unsigned axis=0;axis<2;axis++) {
            int64_t uv=stage ? q->uv1[first+i][axis] : axis ? v[i].v : v[i].u;
            if (uv < -16*65536 || uv>16*65536) return false;
            unsigned n=(unsigned)(((uv<0 ? -uv : uv)+65535)/65536);
            if (n>magnitude) magnitude=n;
        }
    }
    if (hi>4*lo) return false;
    int64_t dx=x[1]-x[0],dy=y[1]-y[0],ex=x[2]-x[0],ey=y[2]-y[0];
    int64_t area=dx*ey-dy*ex;
    if (!area) return true;
    int64_t gx=((int64_t)v[1].rhw-v[0].rhw)*ey-((int64_t)v[2].rhw-v[0].rhw)*dy;
    int64_t gy=((int64_t)v[2].rhw-v[0].rhw)*dx-((int64_t)v[1].rhw-v[0].rhw)*ex;
    if (area<0) area=-area;
    if (gx<0) gx=-gx;
    if (gy<0) gy=-gy;
    if ((gx+gy)*16*32>area*lo) return false;
    for (unsigned i=0;i<3;i++) {
        int64_t a=x[(i+1)%3]-x[i],b=y[(i+1)%3]-y[i];
        perimeter+=(a<0 ? -a : a)+(b<0 ? -b : b);
    }
    unsigned size=t->width>t->height ? t->width : t->height;
    int64_t ratio=(hi+lo-1)/lo;
    return (int64_t)size*(33+2*magnitude)*(512*ratio*perimeter+area)<=262144*area;
}
static int validate(void *device,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    struct vmsvga3d_device *d=device;
    if (!heap_valid(d) || !q || !r || !q->flags || (q->flags&~R3D_ALL_FEATURES) ||
        render3d_validate_state(q) || q->count>(fullsize(d) ? R3D_VERTICES : 3u) || q->count%3 ||
        q->clear_depth>65535 || !extent(d,r[0]) || r[0]->kind!=R3D_COLOR) return -EINVAL;
    for (unsigned i=0;i<R3D_REFS;i++) if (r[i]) {
        if (!extent(d,r[i])) return -EINVAL;
        for (unsigned j=0;j<i;j++) if (r[j] && r[i]!=r[j] && overlaps((struct render3d_extent){r[i]->offset,r[i]->reserved},
            (struct render3d_extent){r[j]->offset,r[j]->reserved})) return -EINVAL;
    }
    bool depth=(q->flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE))!=0;
    if (depth ? (!r[1] || r[1]->kind!=R3D_D16 || r[1]->width!=r[0]->width || r[1]->height!=r[0]->height) : r[1]!=0) return -EINVAL;
    if ((q->flags&R3D_TEXTURED) ? (!r[2] || r[2]->kind!=R3D_TEXTURE) : r[2]!=0) return -EINVAL;
    if ((q->flags&R3D_TEXTURE1) ? (!(q->flags&R3D_TEXTURED) || !r[4] || r[4]->kind!=R3D_TEXTURE) : r[4]!=0) return -EINVAL;
    if ((q->flags&R3D_COPY) ? (!r[3] || r[3]==r[0] || (r[3]->kind!=R3D_LINEAR_COLOR && r[3]->kind!=R3D_COLOR) ||
        r[3]->width!=r[0]->width || r[3]->height!=r[0]->height) : r[3]!=0) return -EINVAL;
    for (unsigned i=0;i<q->count;i++) {
        const struct render3d_vertex *v=&q->vertices[i];
        if (v->x<0 || v->y<0 || v->x>(int32_t)(r[0]->width<<16) || v->y>(int32_t)(r[0]->height<<16) ||
            v->z>65535 || v->rhw<256 || v->rhw>16777216) return -EINVAL;
    }
    for (unsigned i=q->count;i<R3D_VERTICES;i++) {
        static const struct render3d_vertex zero;
        if (memcmp(&q->vertices[i],&zero,sizeof(zero)) || q->uv1[i][0] || q->uv1[i][1]) return -EINVAL;
    }
    for (unsigned stage=0;stage<2;stage++) if (q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED)) {
        const struct render3d_resource *t=r[stage ? 4 : 2];
        if (stage && d->caps.max_textures<2) return -EOPNOTSUPP;
        if (t->width>texture_limit(d) || t->height>texture_limit(d)) return -EOPNOTSUPP;
        struct combiner c;
        int e=combiner(stage,q->state.sampler[stage].combine,t->format,&c);
        if (e) return e;
        if (!combine_admitted(d,c.need)) return -EOPNOTSUPP;
    }
    if (!d->fullsize_testing && !d->perspective_qualified && q->state.blend &&
        (q->state.source>R3D_INV_SRC_ALPHA || q->state.destination>R3D_INV_SRC_ALPHA)) return -EOPNOTSUPP;
    for (unsigned first=0;first<q->count;first+=3) {
        if (!varying_w(q,first)) continue;
        /* Hosts interpolate colour perspective-correctly; the render3d
         * contract interpolates it affinely. Only uniform colour is exact. */
        if (!q->state.flat && (q->vertices[first].color!=q->vertices[first+1].color ||
            q->vertices[first].color!=q->vertices[first+2].color)) return -EOPNOTSUPP;
        if (!(q->flags&R3D_TEXTURED) || d->fullsize_testing) continue;
        if (!d->perspective_qualified) return -EOPNOTSUPP;
        unsigned lo=UINT32_MAX,hi=0;
        for (unsigned i=first;i<first+3;i++) {
            const struct render3d_vertex *v=&q->vertices[i];
            if (v->rhw<32768 || (v->x&4095) || (v->y&4095)) return -EOPNOTSUPP;
            if (v->rhw<lo) lo=v->rhw;
            if (v->rhw>hi) hi=v->rhw;
        }
        if (hi>4*lo) return -EOPNOTSUPP;
    }
    if (d->fullsize_qualified && !d->fullsize_testing)
      for (unsigned stage=0;stage<2;stage++) if ((q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED)) &&
          q->state.sampler[stage].filter>=R3D_NEAREST_MIP)
        for (unsigned first=0;first<q->count;first+=3)
            if (!mip_gradient(q,first,stage,r[stage ? 4 : 2])) return -EOPNOTSUPP;
    uint32_t words[VMSVGA3D_VERTEX_WORDS];
    for (unsigned i=0;i<q->count;i++) if (vmsvga3d_vertex(q,i,words)) return -EINVAL;
    return 0;
}

/* Command encoders. Layouts: Mesa 21.3.9 svga3d_cmd.h:341-349 header,
 * :351-385 DefineSurface (+SVGA3dSize per mip), :417-439 destroy/context,
 * :441-452 Clear, :496-523 RenderState/RenderTarget/SurfaceCopy,
 * :575-627 SurfaceDMA + suffix, :649-760 DrawPrimitives, :925-963 texture
 * state/transform/zrange, :1007-1014 viewport. Emission order follows Mesa
 * svga_cmd.c SVGA3D_BeginDefineSurface:274, SVGA3D_SurfaceDMA:428 (suffix),
 * SVGA3D_BeginDrawPrimitives:993. */
struct batch { uint32_t *w; unsigned n, cap; bool overflow; };
static void put(struct batch *b,uint32_t v)
{ if (b->n<b->cap) b->w[b->n++]=v; else b->overflow=true; }
static void header(struct batch *b,uint32_t id,unsigned body_words)
{ put(b,id); put(b,body_words*4u); }
static void define_surface(struct batch *b,uint32_t sid,uint32_t flags,uint32_t format,
                           uint32_t w,uint32_t h,unsigned levels)
{
    header(b,VMSVGA3D_CMD_SURFACE_DEFINE,3u+VMSVGA3D_MAX_SURFACE_FACES+3u*levels);
    put(b,sid); put(b,flags); put(b,format);
    for (unsigned f=0;f<VMSVGA3D_MAX_SURFACE_FACES;f++) put(b,f ? 0 : levels);
    for (unsigned l=0;l<levels;l++) {
        put(b,w>>l ? w>>l : 1); put(b,h>>l ? h>>l : 1); put(b,1);
    }
}
static void destroy_surface(struct batch *b,uint32_t sid)
{ header(b,VMSVGA3D_CMD_SURFACE_DESTROY,1); put(b,sid); }
static void dma(struct batch *b,uint32_t transfer,uint32_t sid,unsigned level,uint32_t offset,
                uint32_t pitch,uint32_t w,uint32_t h,uint32_t bytes)
{
    header(b,VMSVGA3D_CMD_SURFACE_DMA,7u+9u+3u);
    put(b,SVGA_GMR_FRAMEBUFFER); put(b,offset); put(b,pitch);   /* SVGAGuestImage */
    put(b,sid); put(b,0); put(b,level);                         /* SVGA3dSurfaceImageId */
    put(b,transfer);
    put(b,0); put(b,0); put(b,0); put(b,w); put(b,h); put(b,1); /* SVGA3dCopyBox host */
    put(b,0); put(b,0); put(b,0);                               /* guest src x,y,z */
    put(b,12); put(b,bytes); put(b,0);                          /* suffix: size, maximumOffset, flags */
}
static void render_target(struct batch *b,uint32_t type,uint32_t sid)
{ header(b,VMSVGA3D_CMD_SETRENDERTARGET,5); put(b,VMSVGA3D_CID); put(b,type); put(b,sid); put(b,0); put(b,0); }
static void transform(struct batch *b,uint32_t type,const uint32_t m[16])
{
    header(b,VMSVGA3D_CMD_SETTRANSFORM,18); put(b,VMSVGA3D_CID); put(b,type);
    for (unsigned i=0;i<16;i++) put(b,m[i]);
}
static uint32_t staging(const struct render3d_resource *r) { return r->offset+4096u; }
static unsigned levels_of(const struct render3d_resource *r) { return r->kind==R3D_TEXTURE ? r->levels : 1u; }
static void level_shape(const struct render3d_resource *r,unsigned level,uint32_t *w,uint32_t *h,uint32_t *offset)
{
    *w=r->width>>level ? r->width>>level : 1; *h=r->height>>level ? r->height>>level : 1;
    *offset=r->kind==R3D_TEXTURE ? r->level_offset[level] : 0;
}
static void upload_commands(struct batch *b,const struct render3d_resource *r,uint32_t sid,bool read)
{
    uint32_t bpp=r->kind==R3D_D16 ? 2u : 4u;
    for (unsigned level=0;level<levels_of(r);level++) {
        uint32_t w,h,offset; level_shape(r,level,&w,&h,&offset);
        dma(b,read ? VMSVGA3D_READ_HOST_VRAM : VMSVGA3D_WRITE_HOST_VRAM,sid,level,
            staging(r)+offset,w*bpp,w,h,w*h*bpp);
    }
}
static uint32_t compare_op(unsigned f)
{
    static const uint8_t map[]={VMSVGA3D_CMP_LESS,VMSVGA3D_CMP_NEVER,VMSVGA3D_CMP_ALWAYS,VMSVGA3D_CMP_EQUAL,
        VMSVGA3D_CMP_LESSEQUAL,VMSVGA3D_CMP_GREATER,VMSVGA3D_CMP_NOTEQUAL,VMSVGA3D_CMP_GREATEREQUAL};
    return map[f&7];
}
static uint32_t blend_op(unsigned f)
{
    static const uint8_t map[]={VMSVGA3D_BLENDOP_ONE,VMSVGA3D_BLENDOP_ZERO,VMSVGA3D_BLENDOP_SRCALPHA,
        VMSVGA3D_BLENDOP_INVSRCALPHA,VMSVGA3D_BLENDOP_DESTCOLOR,VMSVGA3D_BLENDOP_SRCCOLOR,
        VMSVGA3D_BLENDOP_INVDESTCOLOR,VMSVGA3D_BLENDOP_INVSRCCOLOR,VMSVGA3D_BLENDOP_DESTALPHA,
        VMSVGA3D_BLENDOP_INVDESTALPHA};
    return f<10 ? map[f] : VMSVGA3D_BLENDOP_ONE;
}
static void render_states(struct batch *b,const struct render3d_state *s,uint32_t flags)
{
    bool test=(flags&R3D_DEPTH_TEST)!=0,write=(flags&R3D_DEPTH_WRITE)!=0;
    /* D3D9 ZENABLE=0 also disables writes: write-only uses ALWAYS. */
    const uint32_t pairs[][2]={
        {VMSVGA3D_RS_ZENABLE,test || write},{VMSVGA3D_RS_ZWRITEENABLE,write},
        {VMSVGA3D_RS_ZFUNC,test ? compare_op(s->depth_func) : VMSVGA3D_CMP_ALWAYS},
        {VMSVGA3D_RS_ALPHATESTENABLE,s->alpha_test},{VMSVGA3D_RS_ALPHAFUNC,compare_op(s->alpha_func)},
        {VMSVGA3D_RS_ALPHAREF,vmsvga3d_ratio_ieee(s->alpha_ref,255,false)},
        {VMSVGA3D_RS_BLENDENABLE,s->blend},
        {VMSVGA3D_RS_SRCBLEND,s->blend ? blend_op(s->source) : VMSVGA3D_BLENDOP_ONE},
        {VMSVGA3D_RS_DSTBLEND,s->blend ? blend_op(s->destination) : VMSVGA3D_BLENDOP_ZERO},
        {VMSVGA3D_RS_BLENDEQUATION,VMSVGA3D_BLENDEQ_ADD},{VMSVGA3D_RS_SEPARATEALPHABLENDENABLE,0},
        {VMSVGA3D_RS_DITHERENABLE,0},{VMSVGA3D_RS_FOGENABLE,0},{VMSVGA3D_RS_SPECULARENABLE,0},
        {VMSVGA3D_RS_STENCILENABLE,0},{VMSVGA3D_RS_LIGHTINGENABLE,0},
        {VMSVGA3D_RS_SHADEMODE,VMSVGA3D_SHADEMODE_SMOOTH},{VMSVGA3D_RS_CULLMODE,VMSVGA3D_FACE_NONE},
        {VMSVGA3D_RS_COLORWRITEENABLE,15},{VMSVGA3D_RS_SCISSORTESTENABLE,0}};
    unsigned n=sizeof(pairs)/sizeof(*pairs);
    header(b,VMSVGA3D_CMD_SETRENDERSTATE,1u+2u*n); put(b,VMSVGA3D_CID);
    for (unsigned i=0;i<n;i++) { put(b,pairs[i][0]); put(b,pairs[i][1]); }
}
static void texture_states(struct batch *b,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    uint32_t list[2][11][2]; unsigned n[2]={0,0};
    for (unsigned stage=0;stage<2;stage++) {
        bool on=(q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED))!=0;
        uint32_t (*l)[2]=list[stage];
        if (!on) {
            l[n[stage]][0]=VMSVGA3D_TS_BIND_TEXTURE; l[n[stage]++][1]=VMSVGA3D_INVALID_ID;
            l[n[stage]][0]=VMSVGA3D_TS_COLOROP; l[n[stage]++][1]=VMSVGA3D_TC_DISABLE;
            l[n[stage]][0]=VMSVGA3D_TS_ALPHAOP; l[n[stage]++][1]=VMSVGA3D_TC_DISABLE;
            continue;
        }
        const struct render3d_resource *t=r[stage ? 4 : 2];
        const struct render3d_sampler *s=&q->state.sampler[stage];
        struct combiner c; (void)combiner(stage,s->combine,t->format,&c);
        uint32_t sid=VMSVGA3D_SID_BASE+(stage && t!=r[2] ? VMSVGA3D_SID_TEX1 : VMSVGA3D_SID_TEX0);
        bool linear=s->filter==R3D_BILINEAR || s->filter==R3D_BILINEAR_MIP || s->filter==R3D_TRILINEAR;
        const uint32_t values[11][2]={{VMSVGA3D_TS_BIND_TEXTURE,sid},
            {VMSVGA3D_TS_COLOROP,c.color_op},{VMSVGA3D_TS_COLORARG1,c.color1},{VMSVGA3D_TS_COLORARG2,c.color2},
            {VMSVGA3D_TS_ALPHAOP,c.alpha_op},{VMSVGA3D_TS_ALPHAARG1,c.alpha1},{VMSVGA3D_TS_ALPHAARG2,c.alpha2},
            {VMSVGA3D_TS_ADDRESSU,s->repeat_u ? VMSVGA3D_TEX_ADDRESS_WRAP : VMSVGA3D_TEX_ADDRESS_CLAMP},
            {VMSVGA3D_TS_ADDRESSV,s->repeat_v ? VMSVGA3D_TEX_ADDRESS_WRAP : VMSVGA3D_TEX_ADDRESS_CLAMP},
            {VMSVGA3D_TS_MAGFILTER,linear ? VMSVGA3D_TEX_FILTER_LINEAR : VMSVGA3D_TEX_FILTER_NEAREST},
            {VMSVGA3D_TS_MINFILTER,linear ? VMSVGA3D_TEX_FILTER_LINEAR : VMSVGA3D_TEX_FILTER_NEAREST}};
        memcpy(l,values,sizeof(values)); n[stage]=11;
    }
    unsigned total=n[0]+n[1]+4u;
    header(b,VMSVGA3D_CMD_SETTEXTURESTATE,1u+3u*total); put(b,VMSVGA3D_CID);
    for (unsigned stage=0;stage<2;stage++) {
        for (unsigned i=0;i<n[stage];i++) { put(b,stage); put(b,list[stage][i][0]); put(b,list[stage][i][1]); }
        const struct render3d_sampler *s=&q->state.sampler[stage];
        bool on=(q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED))!=0;
        uint32_t mip=!on ? VMSVGA3D_TEX_FILTER_NONE : s->filter==R3D_TRILINEAR ? VMSVGA3D_TEX_FILTER_LINEAR :
            s->filter>=R3D_NEAREST_MIP ? VMSVGA3D_TEX_FILTER_NEAREST : VMSVGA3D_TEX_FILTER_NONE;
        put(b,stage); put(b,VMSVGA3D_TS_MIPFILTER); put(b,mip);
        put(b,stage); put(b,VMSVGA3D_TS_TEXCOORDINDEX); put(b,stage);
    }
}
static const uint32_t identity[16]={IEEE_ONE,0,0,0, 0,IEEE_ONE,0,0, 0,0,IEEE_ONE,0, 0,0,0,IEEE_ONE};
/* D-F2: context-scoped state is write-only and this driver is its sole
 * writer. Re-emit the shadowed baseline before a completed batch publishes
 * its fence. Refusals emit nothing; an undrained fault keeps the owner lost. */
static void baseline(struct batch *b,const struct vmsvga3d_device *d)
{
    static const struct render3d_state state;
    render_states(b,&state,0);
    static const uint32_t states[][2]={
        {VMSVGA3D_TS_BIND_TEXTURE,VMSVGA3D_INVALID_ID},
        {VMSVGA3D_TS_COLOROP,VMSVGA3D_TC_DISABLE},{VMSVGA3D_TS_COLORARG1,VMSVGA3D_TA_TEXTURE},
        {VMSVGA3D_TS_COLORARG2,VMSVGA3D_TA_DIFFUSE},{VMSVGA3D_TS_ALPHAOP,VMSVGA3D_TC_DISABLE},
        {VMSVGA3D_TS_ALPHAARG1,VMSVGA3D_TA_TEXTURE},{VMSVGA3D_TS_ALPHAARG2,VMSVGA3D_TA_DIFFUSE},
        {VMSVGA3D_TS_ADDRESSU,VMSVGA3D_TEX_ADDRESS_CLAMP},{VMSVGA3D_TS_ADDRESSV,VMSVGA3D_TEX_ADDRESS_CLAMP},
        {VMSVGA3D_TS_MAGFILTER,VMSVGA3D_TEX_FILTER_NEAREST},{VMSVGA3D_TS_MINFILTER,VMSVGA3D_TEX_FILTER_NEAREST},
        {VMSVGA3D_TS_MIPFILTER,VMSVGA3D_TEX_FILTER_NONE}};
    unsigned count=sizeof(states)/sizeof(*states);
    header(b,VMSVGA3D_CMD_SETTEXTURESTATE,1u+6u*(count+1u)); put(b,VMSVGA3D_CID);
    for (unsigned stage=0;stage<2;stage++) {
        for (unsigned i=0;i<count;i++) { put(b,stage); put(b,states[i][0]); put(b,states[i][1]); }
        put(b,stage); put(b,VMSVGA3D_TS_TEXCOORDINDEX); put(b,stage);
    }
    render_target(b,VMSVGA3D_RT_COLOR0,VMSVGA3D_INVALID_ID);
    render_target(b,VMSVGA3D_RT_DEPTH,VMSVGA3D_INVALID_ID);
    transform(b,VMSVGA3D_TRANSFORM_WORLD,identity);
    transform(b,VMSVGA3D_TRANSFORM_VIEW,identity);
    transform(b,VMSVGA3D_TRANSFORM_PROJECTION,identity);
    header(b,VMSVGA3D_CMD_SETZRANGE,3); put(b,VMSVGA3D_CID); put(b,0); put(b,IEEE_ONE);
    header(b,VMSVGA3D_CMD_SETVIEWPORT,5); put(b,VMSVGA3D_CID); put(b,0); put(b,0);
    put(b,d->mode_width); put(b,d->mode_height);
}
static uint32_t vertex_offset(const struct vmsvga3d_device *d) { return d->heap.first+4096u; }
/* One complete job. Every surface defined here is unbound and destroyed in
 * the same batch, before the engine-appended fence; the persistent context
 * (VMSVGA3D_CID) is re-programmed in full every time. */
int vmsvga3d_encode(struct vmsvga3d_device *d,const struct render3d_request *q,
                    struct render3d_resource *const r[R3D_REFS],unsigned *words)
{
    if (!d || !q || !r || !words || !r[0]) return -EINVAL;
    struct batch b={d->commands,0,VMSVGA3D_BATCH_WORDS,false};
    static const uint32_t roles[R3D_REFS]={VMSVGA3D_SID_COLOR,VMSVGA3D_SID_DEPTH,VMSVGA3D_SID_TEX0,
                                           VMSVGA3D_SID_COPY,VMSVGA3D_SID_TEX1};
    bool defined[R3D_REFS]={false};
    for (unsigned i=0;i<R3D_REFS;i++) {
        const struct render3d_resource *t=r[i];
        if (!t || (i==4 && t==r[2])) continue;
        uint32_t flags=t->kind==R3D_D16 ? VMSVGA3D_HINT_DEPTHSTENCIL :
            t->kind==R3D_TEXTURE ? VMSVGA3D_HINT_TEXTURE : VMSVGA3D_HINT_RENDERTARGET;
        define_surface(&b,VMSVGA3D_SID_BASE+roles[i],flags,t->kind==R3D_D16 ? VMSVGA3D_Z_D16 : VMSVGA3D_A8R8G8B8,
            t->width,t->height,levels_of(t));
        defined[i]=true;
        /* The copy target is fully overwritten by SURFACE_COPY. */
        if (i!=3) upload_commands(&b,t,VMSVGA3D_SID_BASE+roles[i],false);
    }
    uint32_t vertex_sid=VMSVGA3D_SID_BASE+VMSVGA3D_SID_VERTEX,bytes=q->count*VMSVGA3D_VERTEX_WORDS*4u;
    if (q->count) {
        define_surface(&b,vertex_sid,VMSVGA3D_HINT_VERTEXBUFFER,VMSVGA3D_BUFFER,bytes,1,1);
        dma(&b,VMSVGA3D_WRITE_HOST_VRAM,vertex_sid,0,vertex_offset(d),bytes,bytes,1,bytes);
    }
    uint32_t w=r[0]->width,h=r[0]->height,projection[16];
    vmsvga3d_projection(w,h,projection);
    render_target(&b,VMSVGA3D_RT_COLOR0,VMSVGA3D_SID_BASE+VMSVGA3D_SID_COLOR);
    render_target(&b,VMSVGA3D_RT_DEPTH,r[1] ? VMSVGA3D_SID_BASE+VMSVGA3D_SID_DEPTH : VMSVGA3D_INVALID_ID);
    header(&b,VMSVGA3D_CMD_SETVIEWPORT,5); put(&b,VMSVGA3D_CID); put(&b,0); put(&b,0); put(&b,w); put(&b,h);
    header(&b,VMSVGA3D_CMD_SETZRANGE,3); put(&b,VMSVGA3D_CID); put(&b,0); put(&b,IEEE_ONE);
    transform(&b,VMSVGA3D_TRANSFORM_WORLD,identity);
    transform(&b,VMSVGA3D_TRANSFORM_VIEW,identity);
    transform(&b,VMSVGA3D_TRANSFORM_PROJECTION,projection);
    render_states(&b,&q->state,q->flags);
    texture_states(&b,q,r);
    if (q->flags&(R3D_CLEAR_COLOR|R3D_CLEAR_DEPTH)) {
        header(&b,VMSVGA3D_CMD_CLEAR,9); put(&b,VMSVGA3D_CID);
        put(&b,((q->flags&R3D_CLEAR_COLOR) ? VMSVGA3D_CLEAR_COLOR : 0)|((q->flags&R3D_CLEAR_DEPTH) ? VMSVGA3D_CLEAR_DEPTH : 0));
        put(&b,q->clear_color); put(&b,vmsvga3d_ratio_ieee(q->clear_depth,65535,false)); put(&b,0);
        put(&b,0); put(&b,0); put(&b,w); put(&b,h);
    }
    if (q->count) {
        const uint32_t decls[4][3]={{VMSVGA3D_DECLTYPE_FLOAT4,VMSVGA3D_DECLUSAGE_POSITION,0},
            {VMSVGA3D_DECLTYPE_D3DCOLOR,VMSVGA3D_DECLUSAGE_COLOR,0},
            {VMSVGA3D_DECLTYPE_FLOAT2,VMSVGA3D_DECLUSAGE_TEXCOORD,0},
            {VMSVGA3D_DECLTYPE_FLOAT2,VMSVGA3D_DECLUSAGE_TEXCOORD,1}};
        static const uint32_t offsets[4]={0,16,20,28};
        unsigned count=(q->flags&R3D_TEXTURE1) ? 4u : (q->flags&R3D_TEXTURED) ? 3u : 2u;
        header(&b,VMSVGA3D_CMD_DRAW_PRIMITIVES,3u+9u*count+7u);
        put(&b,VMSVGA3D_CID); put(&b,count); put(&b,1);
        for (unsigned i=0;i<count;i++) {
            put(&b,decls[i][0]); put(&b,VMSVGA3D_DECLMETHOD_DEFAULT); put(&b,decls[i][1]); put(&b,decls[i][2]);
            put(&b,vertex_sid); put(&b,offsets[i]); put(&b,VMSVGA3D_VERTEX_WORDS*4u);
            put(&b,0); put(&b,0); /* range hint: whole array */
        }
        put(&b,VMSVGA3D_PRIMITIVE_TRIANGLELIST); put(&b,q->count/3);
        put(&b,VMSVGA3D_INVALID_ID); put(&b,0); put(&b,0); put(&b,0); put(&b,0); /* no index buffer */
    }
    if (q->flags&R3D_COPY) {
        header(&b,VMSVGA3D_CMD_SURFACE_COPY,15);
        put(&b,VMSVGA3D_SID_BASE+VMSVGA3D_SID_COLOR); put(&b,0); put(&b,0);
        put(&b,VMSVGA3D_SID_BASE+VMSVGA3D_SID_COPY); put(&b,0); put(&b,0);
        put(&b,0); put(&b,0); put(&b,0); put(&b,w); put(&b,h); put(&b,1); put(&b,0); put(&b,0); put(&b,0);
    }
    /* Canonical attachments come back through the same claimed staging. */
    for (unsigned i=0;i<4;i++) if (r[i] && i!=2) upload_commands(&b,r[i],VMSVGA3D_SID_BASE+roles[i],true);
    baseline(&b,d);
    for (unsigned i=0;i<R3D_REFS;i++) if (defined[i]) destroy_surface(&b,VMSVGA3D_SID_BASE+roles[i]);
    if (q->count) destroy_surface(&b,vertex_sid);
    if (b.overflow) return -E2BIG;
    *words=b.n; return 0;
}

/* Owner-mapped staging. Guard pages surround every extent and are checked
 * after every job; all CPU loops are chunked against dead-mapping stalls. */
#define GUARD 0xa55ac33cu
static uint64_t now(struct vmsvga3d_device *d) { return d->engine->io.now(d->engine->io.ctx); }
static uint32_t mem_read(struct vmsvga3d_device *d,uint32_t at) { return d->memory.read(d->memory.ctx,at,4); }
static void mem_write(struct vmsvga3d_device *d,uint32_t at,uint32_t v) { d->memory.write(d->memory.ctx,at,v,4); }
static int guard_page(struct vmsvga3d_device *d,uint32_t first,bool initialize)
{
    uint64_t start=now(d);
    for (unsigned i=0;i<4096;i+=4) {
        if (initialize) mem_write(d,first+i,GUARD);
        else {
            uint32_t value=mem_read(d,first+i);
            if (value!=GUARD) { d->expected=GUARD; d->actual=value; d->guard_offset=first+i; return -EIO; }
        }
        if (i%1024==1020 && now(d)-start>=1000) return -ETIMEDOUT;
    }
    return 0;
}
static int guards(struct vmsvga3d_device *d,const struct render3d_resource *r,bool initialize)
{
    int e=guard_page(d,r->offset,initialize);
    if (!e) e=guard_page(d,r->offset+r->reserved-4096u,initialize);
    return e;
}
static int copy_words(struct vmsvga3d_device *d,uint8_t *shadow,uint32_t at,uint32_t bytes,bool to_device)
{
    uint64_t start=now(d);
    for (uint32_t i=0;i<bytes;i+=4) {
        uint32_t value=0; unsigned n=bytes-i<4 ? bytes-i : 4;
        if (to_device) { memcpy(&value,shadow+i,n); d->memory.write(d->memory.ctx,at+i,value,n==4 ? 4 : 2); }
        else { value=d->memory.read(d->memory.ctx,at+i,n==4 ? 4 : 2); memcpy(shadow+i,&value,n); }
        if (i%1024==1020) {
            if (now(d)-start>=1000) return -ETIMEDOUT;
            start=now(d);
        }
    }
    return 0;
}
static int upload(struct vmsvga3d_device *d,struct render3d_resource *r)
{
    if (r->resident) return guards(d,r,false);
    int e=guards(d,r,true);
    if (!e) e=copy_words(d,r->shadow,staging(r),r->kind==R3D_TEXTURE ? r->bytes : r->pitch*r->height,true);
    if (!e) r->resident=true;
    return e;
}
static int readback(struct vmsvga3d_device *d,struct render3d_resource *r)
{
    if (r->kind==R3D_TEXTURE) return guards(d,r,false);
    for (unsigned y=0;y<r->height;y++) {
        /* Qualification samples tile rows plus the final row; ordinary jobs
         * publish complete canonical attachments. */
        if (d->fullsize_testing && y%16 && y!=r->height-1) continue;
        int e=copy_words(d,(uint8_t *)r->shadow+y*r->pitch,staging(r)+y*r->pitch,r->pitch,false);
        if (e) return e;
    }
    /* Staging now holds the device result; it is the next job's source. */
    r->resident=true;
    return guards(d,r,false);
}
static int vertices(struct vmsvga3d_device *d,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    uint32_t at=vertex_offset(d),words[VMSVGA3D_VERTEX_WORDS];
    int e=guard_page(d,d->heap.first,true);
    if (!e) e=guard_page(d,d->heap.first+VMSVGA3D_SCRATCH_BYTES-4096u,true);
    struct combiner c={.keep=0xffffffffu};
    if (q->flags&R3D_TEXTURED) (void)combiner(0,q->state.sampler[0].combine,r[2]->format,&c);
    uint64_t start=now(d);
    for (unsigned i=0;!e && i<q->count;i++) {
        e=vmsvga3d_vertex(q,i,words);
        words[4]=(words[4]&c.keep)|c.set;
        for (unsigned k=0;!e && k<VMSVGA3D_VERTEX_WORDS;k++) mem_write(d,at+(i*VMSVGA3D_VERTEX_WORDS+k)*4u,words[k]);
        if (!e && i%64==63 && now(d)-start>=1000) e=-ETIMEDOUT;
    }
    return e;
}
static uint32_t next_fence(struct vmsvga3d_device *d)
{ if (!++d->fence) d->fence=1; return d->fence; }
static void quarantine(void *device);
static int submit(struct vmsvga3d_device *d,unsigned words,unsigned wait_us)
{
    int e=vmsvga_engine_submit3d(d->engine,d->commands,words,next_fence(d),wait_us);
    if (!e) d->batches++;
    else if (d->engine->quarantined || !d->engine->cpu_safe) quarantine(d);
    return e;
}
static int execute(void *device,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    struct vmsvga3d_device *d=device;
    int e=validate(d,q,r); if (e) return e; /* nothing is written for refused work */
    if (!d->locked || (!d->qualified && !d->probing) || d->lost || !d->context_live) return -ENODEV;
    d->slice_started=now(d);
    d->failed_phase=1;
    for (unsigned i=0;!e && i<R3D_REFS;i++) if (r[i] && !(i==4 && r[4]==r[2])) {
        if (i==3) { e=guards(d,r[i],!r[i]->resident); continue; }
        e=upload(d,r[i]);
    }
    if (!e && q->count) e=vertices(d,q,r);
    unsigned words=0;
    if (!e) { d->failed_phase=2; e=vmsvga3d_encode(d,q,r,&words); }
    if (!e) {
        d->failed_phase=3;
        e=submit(d,words,VMSVGA3D_WAIT_US);
        if (!e) {
            d->triangles+=q->count/3; d->copies+=(q->flags&R3D_COPY)!=0;
            for (unsigned i=0;i<R3D_REFS;i++) if (r[i]) d->dmas+=r[i]->kind==R3D_TEXTURE ? r[i]->levels : i==3 ? 1 : 2;
            unsigned defined=0;
            for (unsigned i=0;i<R3D_REFS;i++) if (r[i] && !(i==4 && r[4]==r[2])) defined++;
            d->defined+=defined+(q->count!=0); d->destroyed+=defined+(q->count!=0);
        }
    }
    if (!e) {
        d->failed_phase=8;
        for (unsigned i=0;!e && i<R3D_REFS;i++) if (r[i] && !(i==4 && r[4]==r[2])) e=readback(d,r[i]);
        if (!e && q->count) e=guard_page(d,d->heap.first,false);
        if (!e && q->count) e=guard_page(d,d->heap.first+VMSVGA3D_SCRATCH_BYTES-4096u,false);
    }
    if (!e) d->failed_phase=0;
    return e;
}
/* Persistent host context: defined once per qualified generation, destroyed
 * by the owner before its claims or FIFO mapping go away. The caller owns
 * the FIFO: inside the render3d enter hook, or on the presenter-held stop
 * path that also runs the 2D engine's own synchronization. */
int vmsvga3d_context(struct vmsvga3d_device *d,bool define)
{
    if (!d || !d->engine || d->lost || !d->engine->active || d->engine->quarantined) return -ENODEV;
    if (define==d->context_live) return 0;
    struct batch b={d->commands,0,VMSVGA3D_BATCH_WORDS,false};
    header(&b,define ? VMSVGA3D_CMD_CONTEXT_DEFINE : VMSVGA3D_CMD_CONTEXT_DESTROY,1); put(&b,VMSVGA3D_CID);
    int e=submit(d,b.n,VMSVGA3D_DEFINE_WAIT_US);
    if (!e) d->context_live=define;
    return e;
}
static bool qualified(void *device,uint32_t generation)
{
    struct vmsvga3d_device *d=device;
    return d && d->qualified && d->minimum_passed && d->fullsize_qualified && d->context_live && !d->lost &&
        d->generation==generation && d->engine && d->engine->active && !d->engine->quarantined && d->caps.valid;
}
static int enter(void *device)
{
    struct vmsvga3d_device *d=device;
    if (!d || d->locked || d->lost || !d->memory.enter) return -EBUSY;
    int e=d->memory.enter(d->memory.ctx,d->generation);
    if (!e) d->locked=true;
    return e;
}
static void leave(void *device)
{
    struct vmsvga3d_device *d=device;
    d->locked=false; d->memory.leave(d->memory.ctx);
}
static int idle(void *device)
{
    struct vmsvga3d_device *d=device;
    return d->engine->cpu_safe && !d->engine->quarantined ? 0 : -EIO;
}
static void quarantine(void *device)
{
    struct vmsvga3d_device *d=device;
    if (!d || d->lost) return;
    d->qualified=d->fullsize_qualified=d->perspective_qualified=d->minimum_passed=false;
    d->lost=true; d->combine=0;
    if (!d->engine->quarantined) vmsvga_engine_quarantine(d->engine,"render3d_fault");
    if (d->lost_notify) d->lost_notify(d->memory.ctx);
}
extern int vmsvga3d_owner_attach(struct render3d_service *,const struct render3d_host *,uint32_t) __attribute__((weak));
static void detach(struct render3d_service *s,void *owner)
{
    struct vmsvga3d_device *d=owner;
    if (d && d->consumer==s) d->consumer=0;
}
static uint32_t combine_modes(const struct vmsvga3d_device *d)
{
    uint32_t modes=1u<<R3D_MODULATE;
    if (d->combine&VMSVGA3D_COMBINE_REPLACE0) modes|=1u<<R3D_REPLACE;
    if (d->combine&VMSVGA3D_COMBINE_ADD) modes|=1u<<R3D_ADD;
    if ((d->combine&VMSVGA3D_COMBINE_DECAL) && (d->combine&VMSVGA3D_COMBINE_REPLACE0)) modes|=1u<<R3D_DECAL;
    return modes;
}
static void query_caps(void *owner,struct ciuki_r3d_caps *out)
{
    const struct vmsvga3d_device *d=owner;
    *out=*vmsvga3d_backend()->capabilities;
    if (!d || d->lost) return;
    out->texture_units=d->caps.max_textures>=2 ? 2 : 1;
    out->combine_modes=combine_modes(d);
    if (d->fullsize_qualified) {
        out->max_size=d->mode_width>d->mode_height ? d->mode_width : d->mode_height;
        out->max_width=d->mode_width; out->max_height=d->mode_height;
        out->max_vertices=R3D_VERTICES;
        out->max_pixel_work=4*d->mode_width*d->mode_height;
        out->sampling_features=CIUKI_R3D_AFFINE_EXACT;
        if (d->perspective_qualified) {
            out->sampling_features|=CIUKI_R3D_PROJECTIVE_BILINEAR;
            out->max_texture_size=texture_limit(d); out->blend_factors=1023;
        }
    }
}
static uint32_t texture_pool_bytes(void *owner)
{
    const struct vmsvga3d_device *d=owner;
    return d && d->texture_bytes ? d->texture_bytes : VMSVGA3D_TEXTURE_MIN;
}
const struct render3d_backend *vmsvga3d_backend(void)
{
    static const struct ciuki_r3d_caps capabilities={
        .version=CIUKI_R3D_VERSION,.features=R3D_ALL_FEATURES,.max_size=64,
        .max_vertices=3,.texture_units=2,.max_texture_size=64,
        .depth_formats=CIUKI_R3D_D16,.color_formats=CIUKI_R3D_ARGB8888,
        .texture_formats=63,.depth_functions=255,.alpha_functions=255,
        .blend_factors=15,.combine_modes=1,.filters=31,.max_pixel_work=4096};
    static const struct render3d_backend backend={.features=R3D_ALL_FEATURES,.qualified=qualified,
        .layout=layout,.validate=validate,.enter=enter,.leave=leave,.execute=execute,
        .idle=idle,.quarantine=quarantine,.attach=vmsvga3d_owner_attach,.max_size=64,.detach=detach,
        .texture_pool_bytes=texture_pool_bytes,.capabilities=&capabilities,.query_caps=query_caps,
        .resident_bytes=(sizeof(struct vmsvga3d_device)+4095u)&~4095u,.whole_context_fallback=true};
    return &backend;
}

static bool budget_left(struct vmsvga3d_device *d)
{ return !d->qualification_started || now(d)-d->qualification_started<VMSVGA3D_QUALIFY_US; }
static int run(struct vmsvga3d_device *d,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    if (!budget_left(d)) return -ETIMEDOUT;
    int e=validate(d,q,r); if (e) return e;
    e=enter(d); if (e) return e;
    uint64_t start=now(d);
    e=execute(d,q,r);
    d->fullsize_us+=now(d)-start;
    int drained=idle(d);
    if (!e) e=drained;
    /* Guard damage, protocol faults and dead mappings quarantine both
     * domains; a CPU-side timeout with a drained engine does not. */
    if (e && (e!=-ETIMEDOUT || drained)) quarantine(d);
    leave(d);
    return e;
}
static uint32_t pixel(const struct render3d_resource *r,unsigned x,unsigned y)
{ return ((const uint32_t *)r->shadow)[y*r->width+x]; }
/* Q1 (contract section B.5): one 64x64 colour+D16 target, a textured and a
 * Gouraud triangle, clear, copy into an owned linear buffer, plus the
 * MODULATE alpha rule this family must prove (D3D9's default stage-0
 * ALPHAOP is SELECTARG1, GL's texture environment modulates). */
int vmsvga3d_probe(struct vmsvga3d_device *d)
{
    if (!d || !d->engine || !d->memory.read || !d->memory.write || !d->memory.enter ||
        !d->memory.leave || !d->generation || !heap_valid(d) || !d->engine->active ||
        d->engine->quarantined || d->lost) return -ENODEV;
    if (!d->consumer || !d->consumer->contexts[0].resources) return -ENOMEM;
    struct render3d_context *c=&d->consumer->contexts[0];
    struct render3d_resource *r[R3D_REFS]={0};
    for (unsigned i=0;i<4;i++) r[i]=&c->resources[i];
    struct render3d_request *q=(void *)d->consumer->ring[0];
    memset(q,0,sizeof(*q)); q->flags=R3D_FEATURES; q->count=3; q->clear_depth=65535;
    q->vertices[0]=(struct render3d_vertex){8<<16,8<<16,16384,65536,0xffffffffu,0,0};
    q->vertices[1]=(struct render3d_vertex){56<<16,8<<16,16384,65536,0xffffffffu,65536,0};
    q->vertices[2]=(struct render3d_vertex){8<<16,56<<16,16384,65536,0xffffffffu,0,65536};
    int e=validate(d,q,r); if (e) return e;
    d->probing=true; d->failed_phase=10;
    e=enter(d);
    if (!e) { e=vmsvga3d_context(d,true); leave(d); }
    if (!e) e=run(d,q,r);
    if (!e) {
        d->failed_phase=7;
        uint32_t color=pixel(r[0],16,16)&0xffffffu,copy=pixel(r[3],16,16)&0xffffffu;
        uint32_t depth=((uint16_t *)r[1]->shadow)[16*64+16];
        d->expected=0x4080c0u; d->actual=color;
        if (color!=d->expected || copy!=d->expected || depth<16383 || depth>16385 ||
            (pixel(r[0],48,12)&0xffffffu)!=0xc04080u || (pixel(r[0],63,63)&0xffffffu)!=0 ||
            ((uint16_t *)r[1]->shadow)[63*64+63]!=65535) e=-EILSEQ;
        if (copy!=0x4080c0u) d->actual=copy;
        if (depth<16383 || depth>16385) { d->expected=16384; d->actual=depth; }
    }
    if (!e) { /* A farther triangle must fail LESS without altering the pixel. */
        q->flags=R3D_TEXTURED|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY;
        for (unsigned i=0;i<3;i++) { q->vertices[i].z=49152; q->vertices[i].color=0xffff0000u; }
        e=run(d,q,r);
        if (!e && ((pixel(r[0],16,16)&0xffffffu)!=0x4080c0u || (pixel(r[3],16,16)&0xffffffu)!=0x4080c0u)) {
            d->failed_phase=7; e=-EILSEQ;
        }
    }
    if (!e) {
        q->flags=R3D_FEATURES;
        for (unsigned i=0;i<3;i++) { q->vertices[i].z=16384; q->vertices[i].color=0xff000000u|(0xffu<<(16-i*8)); }
        e=run(d,q,r);
        if (!e) {
            uint32_t value=pixel(r[0],16,16);
            const uint32_t channels[]={34,23,41};
            for (unsigned i=0;i<3;i++) {
                uint32_t ch=(value>>(i*8))&255;
                if (ch+2<channels[i] || ch>channels[i]+2) { d->failed_phase=7; d->expected=0x00291722; d->actual=value; e=-EILSEQ; }
            }
        }
    }
    if (!e) {
        q->flags=R3D_TEXTURED|R3D_CLEAR_COLOR|R3D_COPY; q->clear_depth=0; r[1]=0;
        for (unsigned i=0;i<3;i++) q->vertices[i].color=0x80ffffffu;
        e=run(d,q,r);
        uint32_t alpha=pixel(r[0],16,16)>>24;
        if (!e && (alpha+2<0x80 || alpha>0x82 || (pixel(r[0],16,16)&0xffffffu)!=0x4080c0u)) {
            d->failed_phase=7; d->expected=0x804080c0u; d->actual=pixel(r[0],16,16); e=-EILSEQ;
        }
    }
    if (!e) for (unsigned i=0;i<64*64;i++)
        if (((uint32_t *)r[0]->shadow)[i]!=((uint32_t *)r[3]->shadow)[i]) { d->failed_phase=7; e=-EILSEQ; break; }
    d->probing=false;
    if (!e) { d->minimum_passed=true; d->failed_phase=0; }
    return e;
}

/* Q2/Q3 boot scenes are the f7-11 family-neutral ones (duplicated per the
 * f7-20 amendment section E until the f7-33 consolidation). */
static void fullsize_scene(struct render3d_request *q,unsigned w,unsigned h,unsigned phase)
{
    memset(q,0,sizeof(*q));
    q->flags=R3D_TEXTURED|R3D_TEXTURE1|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY;
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
        q->uv1[i][0]=q->vertices[i].v; q->uv1[i][1]=q->vertices[i].u;
    }
    if (phase==3) {
        q->count=R3D_VERTICES;
        for (unsigned triangle=0;triangle<256;triangle++) for (unsigned v=0;v<3;v++) {
            unsigned i=triangle*3+v;
            unsigned x=(triangle%16)*(w-16)/15+(v==1 ? 16 : 0);
            unsigned y=(triangle/16)*(h-16)/15+(v==2 ? 16 : 0);
            q->vertices[i]=(struct render3d_vertex){x<<16,y<<16,4096,65536,0xffc0e0a0u,
                v==1 ? 49152 : 16384,v==2 ? 49152 : 16384};
            q->uv1[i][0]=q->vertices[i].v; q->uv1[i][1]=q->vertices[i].u;
        }
    }
}
static int fullsize_compare(struct vmsvga3d_device *d,struct render3d_resource *const hw[R3D_REFS],
                            struct render3d_resource *const ref[R3D_REFS])
{
    const uint32_t *a=hw[0]->shadow,*b=ref[0]->shadow,*copy=hw[3]->shadow;
    const uint16_t *az=hw[1]->shadow,*bz=ref[1]->shadow;
    uint32_t ah=2166136261u,bh=2166136261u;
    for (unsigned i=0;i<hw[0]->width*hw[0]->height;i++) {
        unsigned row=i/hw[0]->width;
        if (row%16 && row!=hw[0]->height-1) continue;
        if (a[i]!=copy[i] || (a[i]==0xff102030u)!=(b[i]==0xff102030u)) { d->expected=b[i]; d->actual=a[i]; return -EILSEQ; }
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
#include "vmwaresvga3d_precision.h"
static void precision_scene(struct render3d_request *q,struct render3d_resource *const r[R3D_REFS],unsigned phase)
{
    fullsize_scene(q,r[0]->width,r[0]->height,0);
    for (unsigned stage=0;stage<2;stage++) {
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
            v->u=(int32_t)(x*30*65536)-15*65536+1385;
            v->v=(int32_t)(y*28*65536)-14*65536+1190;
            q->uv1[i][0]=(int32_t)(y*28*65536)-14*65536+3522;
            q->uv1[i][1]=(int32_t)(x*30*65536)-15*65536+3570;
        }
        if (phase==6) {
            v->rhw=65536;
            v->u=(32768+x*65536)/r[2]->width; v->v=32768/r[2]->height;
            q->uv1[i][0]=(32768+(1-x)*65536)/r[4]->width; q->uv1[i][1]=32768/r[4]->height;
        }
        if (phase>=7) {
            v->rhw=(x ? 256u : 64u)*65536;
            v->u=(int32_t)(x*30*65536)-15*65536+1385;
            v->v=(int32_t)(y*28*65536)-14*65536+1190;
            q->uv1[i][0]=(int32_t)(y*28*65536)-14*65536+3522;
            q->uv1[i][1]=(int32_t)(x*30*65536)-15*65536+3570;
        }
        if (phase>=21) {
            v->u=(phase==21 ? -16 : 16)*65536; v->v=-v->u;
            q->uv1[i][0]=-v->u; q->uv1[i][1]=v->u;
        }
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
static int precision_probe(struct vmsvga3d_device *d,struct render3d_request *q,
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
        precision_scene(q,hw,phase);
        for (unsigned t=2;t<=4;t+=2) memcpy(ref[t]->shadow,hw[t]->shadow,hw[t]->bytes);
        struct render3d_job job={0};
        while (!render3d_scalar_slice(q,ref,&job,&d->consumer->host)) {}
        e=run(d,q,hw);
        if (e) break;
        const uint32_t *a=hw[0]->shadow,*copy=hw[3]->shadow;
        const uint16_t *az=hw[1]->shadow;
        for (unsigned y=0;y<hw[0]->height && !e;y++) {
            if (y%16 && y!=hw[0]->height-1) continue;
            for (unsigned x=0;x<hw[0]->width;x++) {
                unsigned at=y*hw[0]->width+x;
                if (a[at]!=copy[at] ||
                    !precision_pixel(q,hw,x,y,a[at],az[at],d->sample_delta_q16,d->filter_fraction_bits)) {
                    d->expected=((uint32_t *)ref[0]->shadow)[at]; d->actual=a[at]; e=-EILSEQ; break;
                }
                d->precision_samples++;
                if (phase==6) {
                    unsigned red=a[at]>>16&255,green=a[at]>>8&255;
                    levels[0][red/32]|=1u<<(red%32); levels[1][green/32]|=1u<<(green%32);
                }
            }
        }
        if (phase==6 && !e) for (unsigned stage=0;stage<2;stage++) {
            unsigned count=0;
            for (unsigned i=0;i<8;i++) for (unsigned bit=0;bit<32;bit++) count+=(levels[stage][i]>>bit)&1;
            d->measured_filter_levels[stage]=count;
            if (count<192) e=-EILSEQ;
        }
    }
    if (!e) d->precision_phase=0;
    return e;
}
/* Combiner phases: each optional encoding is admitted only after the
 * mode-sized scene matches the scalar reference within Q2 tolerances, and
 * only if SVGA3D_DEVCAP_TEXTURE_OPS reports the operations it uses. */
static int combiner_phase(struct vmsvga3d_device *d,struct render3d_request *q,
    struct render3d_resource *const hw[R3D_REFS],struct render3d_resource *const ref[R3D_REFS],
    unsigned mode,unsigned stage,uint32_t format)
{
    fullsize_scene(q,hw[0]->width,hw[0]->height,0);
    q->state.sampler[stage].combine=mode;
    for (unsigned i=0;i<6;i++) q->vertices[i].color=0xc0806040u;
    for (unsigned t=2;t<=4;t+=2) {
        struct render3d_resource *texture=hw[t];
        texture->format=ref[t]->format=t==(stage ? 4u : 2u) ? format : R3D_ARGB8888;
        for (unsigned level=0;level<texture->levels;level++) {
            unsigned width=texture->width>>level ? texture->width>>level : 1,height=texture->height>>level ? texture->height>>level : 1;
            uint32_t *pixels=(void *)((uint8_t *)texture->shadow+texture->level_offset[level]);
            for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++) {
                unsigned ramp=width>1 ? x*255/(width-1) : 128,cross=height>1 ? y*255/(height-1) : 64;
                uint32_t value=((ramp^0x55)<<24)|(ramp<<16)|(cross<<8)|((ramp+cross)/2);
                if (format_class(texture->format)==1) value|=0xff000000u;
                if (format_class(texture->format)==2) value|=0x00ffffffu;
                pixels[y*width+x]=value;
            }
        }
        texture->resident=false;
        memcpy(ref[t]->shadow,texture->shadow,texture->bytes);
    }
    struct render3d_job job={0};
    while (!render3d_scalar_slice(q,ref,&job,&d->consumer->host)) {}
    int e=run(d,q,hw);
    if (!e) e=fullsize_compare(d,hw,ref);
    for (unsigned t=2;t<=4;t+=2) hw[t]->format=ref[t]->format=R3D_ARGB8888;
    return e;
}
static int combiner_trials(struct vmsvga3d_device *d,struct render3d_request *q,
    struct render3d_resource *const hw[R3D_REFS],struct render3d_resource *const ref[R3D_REFS])
{
    static const struct { uint32_t bit; uint8_t mode, stage, format; } trials[]={
        {VMSVGA3D_COMBINE_REPLACE0,R3D_REPLACE,0,R3D_ARGB8888},{VMSVGA3D_COMBINE_REPLACE0,R3D_REPLACE,0,R3D_RGB565},
        {VMSVGA3D_COMBINE_REPLACE0,R3D_REPLACE,0,R3D_A8},
        {VMSVGA3D_COMBINE_REPLACE1,R3D_REPLACE,1,R3D_ARGB8888},{VMSVGA3D_COMBINE_REPLACE1,R3D_REPLACE,1,R3D_RGB565},
        {VMSVGA3D_COMBINE_REPLACE1,R3D_REPLACE,1,R3D_A8},
        {VMSVGA3D_COMBINE_ADD,R3D_ADD,0,R3D_ARGB8888},{VMSVGA3D_COMBINE_ADD,R3D_ADD,1,R3D_ARGB8888},
        {VMSVGA3D_COMBINE_DECAL,R3D_DECAL,0,R3D_ARGB8888},{VMSVGA3D_COMBINE_DECAL,R3D_DECAL,1,R3D_ARGB8888}};
    uint32_t failed=0,tried=0;
    for (unsigned i=0;i<sizeof(trials)/sizeof(*trials);i++) {
        uint32_t bit=trials[i].bit,ops=texture_op_bits(bit);
        if ((failed&bit) || (d->caps.texture_ops&ops)!=ops) continue;
        d->combine_trial=bit; tried|=bit;
        int e=combiner_phase(d,q,hw,ref,trials[i].mode,trials[i].stage,trials[i].format);
        d->combine_trial=0;
        if (e==-EILSEQ || e==-EOPNOTSUPP) { failed|=bit; continue; }
        /* A spent budget refuses the remaining encodings, not the scene. */
        if (e==-ETIMEDOUT && !d->lost) { failed=~0u; break; }
        if (e) return e;
    }
    d->combine=tried&~failed;
    return 0;
}
int vmsvga3d_fullsize_probe(struct vmsvga3d_device *d,struct render3d_service *reference)
{
    if (!d || !d->minimum_passed || d->lost || d->fullsize_qualified || !d->consumer || !reference ||
        d->mode_width<64 || d->mode_height<64 || d->mode_width>1024 || d->mode_height>1024 ||
        !d->consumer->contexts[0].resources || !reference->contexts[0].resources) return -ENODEV;
    struct render3d_resource *hw[R3D_REFS],*ref[R3D_REFS];
    for (unsigned i=0;i<R3D_REFS;i++) {
        hw[i]=&d->consumer->contexts[0].resources[i]; ref[i]=&reference->contexts[0].resources[i];
        if (!hw[i]->shadow || !ref[i]->shadow || hw[i]->bytes!=ref[i]->bytes) return -EINVAL;
    }
    if (hw[0]->width!=d->mode_width || hw[0]->height!=d->mode_height) return -EINVAL;
    d->fullsize_testing=d->probing=true;
    struct render3d_request *q=(void *)d->consumer->ring[0];
    int e=0;
    for (unsigned phase=0;phase<4 && !e;phase++) {
        fullsize_scene(q,d->mode_width,d->mode_height,phase);
        struct render3d_job job={0};
        while (!render3d_scalar_slice(q,ref,&job,&reference->host)) {}
        e=run(d,q,hw);
        if (!e) { d->failed_phase=9; e=fullsize_compare(d,hw,ref); }
    }
    if (!e) {
        d->fullsize_qualified=true; d->failed_phase=0;
        d->precision_error=precision_probe(d,q,hw,ref);
        d->perspective_qualified=!d->precision_error;
        /* A clean numeric or budget refusal only reduces claims. */
        if (d->lost || (d->precision_error && d->precision_error!=-EILSEQ && d->precision_error!=-ETIMEDOUT))
            e=d->precision_error;
    }
    if (!e && !d->lost) e=combiner_trials(d,q,hw,ref);
    d->fullsize_testing=d->probing=false;
    if (e) { d->fullsize_qualified=d->perspective_qualified=false; d->combine=0; }
    if (!e) d->failed_phase=0;
    else if (!d->precision_error) d->precision_error=e;
    return e;
}
