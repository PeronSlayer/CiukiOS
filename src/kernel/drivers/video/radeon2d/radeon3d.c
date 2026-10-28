/* Radeon R100/R200 fixed-function backend for the family-neutral render3d
 * service. Register programming follows the pinned Linux v6.1 radeon_reg.h,
 * Mesa 7.11.2 radeon/r200 and X.Org ati 6.14.6 MMIO paths cited in
 * docs/design/f7-family-ati-radeon.md; no upstream code is vendored. The
 * engine is driven through the 2D driver's register FIFO (no CP), every job
 * saves/restores the 3D state and proves idle before the CPU reads VRAM.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/display_driver.h>
#include "radeon3d.h"
#include "radeon3d_regs.h"

/* ---- Serialization fixtures ------------------------------------------- */
uint32_t radeon3d_fixed_ieee(uint32_t value, unsigned fractional)
{
    if (!value || fractional>31) return 0;
    unsigned bit=31; while (!(value&(1u<<bit))) bit--;
    uint32_t mantissa;
    if (bit<=23) mantissa=value<<(23-bit);
    else {
        unsigned shift=bit-23;
        mantissa=value>>shift;
        uint32_t rem=value&((1u<<shift)-1), half=1u<<(shift-1);
        if (rem>half || (rem==half && (mantissa&1))) mantissa++;
        if (mantissa==0x1000000u) { mantissa>>=1; bit++; }
    }
    return ((bit+127-fractional)<<23)|(mantissa&0x7fffffu);
}
uint32_t radeon3d_signed_ieee(int32_t v)
{ return radeon3d_fixed_ieee((v<0 ? 0u-(uint32_t)v : (uint32_t)v),16)|(v<0 ? 0x80000000u : 0); }
/* Correctly rounded depth/65535 in [0,1]; the chip's Z input is a unit float
 * (Mesa's viewport maps NDC to 0..1 before the depth format scale). */
uint32_t radeon3d_depth_ieee(uint32_t depth)
{
    if (!depth) return 0;
    if (depth>=65535) return 0x3f800000u;
    uint32_t n=depth, exponent=127;
    while (n<65535) { n*=2; exponent--; }
    uint32_t mantissa=1, remainder=n-65535;
    for (unsigned i=0;i<23;i++) {
        remainder*=2; mantissa<<=1;
        if (remainder>=65535) { remainder-=65535; mantissa++; }
    }
    if (remainder*2>65535 || (remainder*2==65535 && (mantissa&1))) mantissa++;
    if (mantissa==0x1000000) { exponent++; mantissa>>=1; }
    return (exponent<<23)|(mantissa&0x7fffff);
}
int radeon3d_triangle_words(const struct render3d_request *q,unsigned first,bool r200,
                            uint32_t words[RADEON3D_WORDS_PER_TRIANGLE])
{
    if (!q || !words || q->count>R3D_VERTICES || first>q->count || q->count-first<3 || first%3) return -EINVAL;
    for (unsigned i=0;i<3;i++) {
        const struct render3d_vertex *v=&q->vertices[first+i];
        if (v->x<0 || v->y<0 || v->x>(1024<<16) || v->y>(1024<<16) || v->z>65535 ||
            v->rhw<256 || v->rhw>16777216 || v->u < -16*65536 || v->u>16*65536 ||
            v->v < -16*65536 || v->v>16*65536 || q->uv1[first+i][0] < -16*65536 ||
            q->uv1[first+i][0]>16*65536 || q->uv1[first+i][1] < -16*65536 || q->uv1[first+i][1]>16*65536) return -EINVAL;
    }
    /* X.Org radeon_exa_render.c MMIO composite: SE_VF_CNTL then the vertex
     * floats through SE_PORT_DATA0. R100 adds VF_RADEON_MODE. */
    words[0]=RADEON_VF_PRIM_TYPE_TRIANGLE_LIST|RADEON_VF_PRIM_WALK_DATA|(r200 ? 0u : RADEON_VF_RADEON_MODE)|
        (3u<<RADEON_VF_NUM_VERTICES_SHIFT);
    for (unsigned i=0;i<3;i++) {
        const struct render3d_vertex *v=&q->vertices[first+i];
        uint32_t *p=words+1+i*9;
        p[0]=radeon3d_fixed_ieee((uint32_t)v->x,16);
        p[1]=radeon3d_fixed_ieee((uint32_t)v->y,16);
        p[2]=radeon3d_depth_ieee(v->z);
        p[3]=radeon3d_fixed_ieee(v->rhw,16);
        p[4]=q->state.flat ? q->flat_color[first/3] : v->color;
        p[5]=radeon3d_signed_ieee(v->u); p[6]=radeon3d_signed_ieee(v->v);
        p[7]=radeon3d_signed_ieee(q->uv1[first+i][0]); p[8]=radeon3d_signed_ieee(q->uv1[first+i][1]);
    }
    return 0;
}

/* ---- Layout ------------------------------------------------------------ */
static unsigned texel_bytes(const struct render3d_resource *r)
{ return r->format==R3D_RGB565 || r->format==R3D_ARGB4444 ? 2 : 4; }
static uint32_t level_stride(unsigned w,unsigned bpp)
{ uint32_t s=w*bpp; if (s<32) s=32; return (s+31)&~31u; }
uint32_t radeon3d_texture_bytes(const struct render3d_resource *r)
{
    if (render3d_texture_shape(r)) return 0;
    unsigned w=r->width,h=r->height,bpp=texel_bytes(r); uint32_t total=0;
    for (unsigned i=0;i<r->levels;i++) {
        total+=level_stride(w,bpp)*h;
        w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
    }
    return (total+4095)&~4095u;
}
int radeon3d_mip_offset(const struct render3d_resource *r,unsigned level,unsigned x,unsigned y,uint32_t *out)
{
    if (!out || render3d_texture_shape(r) || level>=r->levels) return -EINVAL;
    unsigned w=r->width,h=r->height,bpp=texel_bytes(r); uint32_t at=0;
    for (unsigned i=0;i<level;i++) { at+=level_stride(w,bpp)*h; w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1; }
    if (x>=w || y>=h) return -EINVAL;
    *out=at+y*level_stride(w,bpp)+x*bpp;
    return *out<radeon3d_texture_bytes(r) ? 0 : -EINVAL;
}
/* Pixels: a multiple of 16 (colour) / 32 (depth) so that the byte pitch is
 * a multiple of 64 for the 2D engine and of 32 for RB3D_*PITCH, with at
 * least one spare column; one spare row covers inclusive RE_WIDTH_HEIGHT. */
uint32_t radeon3d_color_pitch(uint32_t w) { return (w+16)&~15u; }
uint32_t radeon3d_depth_pitch(uint32_t w) { return (w+32)&~31u; }
static uint32_t padded_bytes(const struct render3d_resource *r)
{
    if (r->kind==R3D_TEXTURE) return radeon3d_texture_bytes(r);
    if (r->kind==R3D_LINEAR_COLOR) return (r->bytes+4095)&~4095u;
    uint32_t pitch=r->kind==R3D_D16 ? radeon3d_depth_pitch(r->width)*2 : radeon3d_color_pitch(r->width)*4;
    return (pitch*(r->height+1)+4095)&~4095u;
}
static uint32_t hw_pitch(const struct render3d_resource *r)
{
    return r->kind==R3D_D16 ? radeon3d_depth_pitch(r->width)*2 : r->kind==R3D_LINEAR_COLOR ? r->pitch :
        radeon3d_color_pitch(r->width)*4;
}
int radeon3d_mode_heap(const struct radeon_engine *s,const struct fb_device *fb,uint32_t vram,
                       uint32_t cursor_offset,struct render3d_extent *heap,uint32_t *required)
{
    if (!s || !fb || !heap || !required || !fb->width || !fb->height || fb->width>1024 || fb->height>1024 ||
        fb->bpp!=32 || !fb->pitch || !s->group || (s->group_id!=RADEON_R100 && s->group_id!=RADEON_R200) || vram<4096 || vram>256u*1024*1024 || !cursor_offset ||
        cursor_offset>vram-4096u || fb->size>cursor_offset) return -EOPNOTSUPP;
    uint32_t first=(fb->size+4095)&~4095u, end=cursor_offset&~4095u;
    if (first>=end) return -ENOMEM;
    uint32_t cp=radeon3d_color_pitch(fb->width)*4,dp=radeon3d_depth_pitch(fb->width)*2,rows=fb->height+1;
    *required=2*((cp*rows+4095)&~4095u)+((dp*rows+4095)&~4095u)+RADEON3D_TEXTURE_MIN+4*8192u+24576u;
    *heap=(struct render3d_extent){first,end-first};
    return heap->bytes<*required ? -ENOMEM : 0;
}

/* ---- Register images --------------------------------------------------- */
static const uint16_t state2d[]={RADEON_DST_PITCH_OFFSET,RADEON_SRC_PITCH_OFFSET,
    RADEON_DP_GUI_MASTER_CNTL,RADEON_DP_BRUSH_FRGD_CLR,RADEON_DP_WRITE_MSK,RADEON_DP_CNTL};
static const uint16_t r100_state[]={
    RADEON_PP_MISC,RADEON_PP_FOG_COLOR,RADEON_RB3D_BLENDCNTL,RADEON_RB3D_DEPTHOFFSET,RADEON_RB3D_DEPTHPITCH,
    RADEON_RB3D_ZSTENCILCNTL,RADEON_PP_CNTL,RADEON_RB3D_CNTL,RADEON_RB3D_COLOROFFSET,RADEON_RE_WIDTH_HEIGHT,
    RADEON_RB3D_COLORPITCH,RADEON_SE_CNTL,RADEON_SE_COORD_FMT,
    RADEON_PP_TXFILTER_0,RADEON_PP_TXFORMAT_0,RADEON_PP_TXOFFSET_0,RADEON_PP_TXCBLEND_0,RADEON_PP_TXABLEND_0,RADEON_PP_TFACTOR_0,
    RADEON_PP_TXFILTER_0+0x18,RADEON_PP_TXFORMAT_0+0x18,RADEON_PP_TXOFFSET_0+0x18,RADEON_PP_TXCBLEND_0+0x18,RADEON_PP_TXABLEND_0+0x18,RADEON_PP_TFACTOR_0+0x18,
    RADEON_PP_TEX_SIZE_0,RADEON_PP_TEX_PITCH_0,RADEON_PP_TEX_SIZE_0+8,RADEON_PP_TEX_PITCH_0+8,
    RADEON_PP_BORDER_COLOR_0,RADEON_PP_BORDER_COLOR_0+4,RADEON_RB3D_STENCILREFMASK,RADEON_RB3D_ROPCNTL,
    RADEON_RB3D_PLANEMASK,RADEON_SE_VTX_FMT,RADEON_SE_CNTL_STATUS,RADEON_RE_TOP_LEFT,RADEON_RE_MISC};
static const uint16_t r200_state[]={
    RADEON_PP_MISC,RADEON_PP_FOG_COLOR,RADEON_RB3D_BLENDCNTL,RADEON_RB3D_DEPTHOFFSET,RADEON_RB3D_DEPTHPITCH,
    RADEON_RB3D_ZSTENCILCNTL,RADEON_PP_CNTL,RADEON_RB3D_CNTL,RADEON_RB3D_COLOROFFSET,RADEON_RE_WIDTH_HEIGHT,
    RADEON_RB3D_COLORPITCH,RADEON_SE_CNTL,R200_RE_CNTL,R200_RE_SCISSOR_TL_0,R200_RE_SCISSOR_BR_0,R200_RB3D_DEPTHXY_OFFSET,
    RADEON_RB3D_STENCILREFMASK,RADEON_RB3D_ROPCNTL,RADEON_RB3D_PLANEMASK,R200_SE_VAP_CNTL,R200_SE_VTX_FMT_0,R200_SE_VTX_FMT_1,
    R200_SE_VTE_CNTL,RADEON_SE_CNTL_STATUS,R200_SE_VTX_STATE_CNTL,RADEON_RE_TOP_LEFT,RADEON_RE_MISC,R200_RE_AUX_SCISSOR_CNTL,
    R200_PP_TXFILTER_0,R200_PP_TXFORMAT_0,R200_PP_TXFORMAT_X_0,R200_PP_TXSIZE_0,R200_PP_TXPITCH_0,R200_PP_BORDER_COLOR_0,R200_PP_TXMULTI_CTL_0,
    R200_PP_TXFILTER_0+0x20,R200_PP_TXFORMAT_0+0x20,R200_PP_TXFORMAT_X_0+0x20,R200_PP_TXSIZE_0+0x20,R200_PP_TXPITCH_0+0x20,R200_PP_BORDER_COLOR_0+0x20,R200_PP_TXMULTI_CTL_0+0x20,
    R200_PP_CNTL_X,R200_PP_TXOFFSET_0,R200_PP_TXOFFSET_0+0x18,R200_PP_TFACTOR_0,R200_PP_TFACTOR_0+4,
    R200_PP_TXCBLEND_0,R200_PP_TXCBLEND2_0,R200_PP_TXABLEND_0,R200_PP_TXABLEND2_0,
    R200_PP_TXCBLEND_0+0x10,R200_PP_TXCBLEND2_0+0x10,R200_PP_TXABLEND_0+0x10,R200_PP_TXABLEND2_0+0x10};
_Static_assert(sizeof(r100_state)/2<=RADEON3D_STATE_MAX && sizeof(r200_state)/2<=RADEON3D_STATE_MAX,"state image");
unsigned radeon3d_state_registers(bool r200,const uint16_t **registers)
{
    *registers=r200 ? r200_state : r100_state;
    return r200 ? sizeof(r200_state)/2 : sizeof(r100_state)/2;
}
static uint32_t readback_mask(uint16_t reg)
{ return reg==RADEON_SE_CNTL_STATUS ? RADEON_TCL_BYPASS : 0xffffffffu; }

/* ---- Transport --------------------------------------------------------- */
static uint32_t mmio_read(struct radeon3d_device *d,uint32_t reg)
{ return d->engine->io.read(d->engine->io.ctx,reg); }
/* X.Org RADEONWaitForFifoFunction / Linux r100_rbbm_fifo_wait_for_entry:
 * free FIFO entries are RBBM_STATUS bits 6:0. One execution window bounds
 * every wait of a job; a frozen clock is bounded by the poll count. */
static int fifo(struct radeon3d_device *d,unsigned need)
{
    struct radeon_io *io=&d->engine->io;
    for (unsigned polls=0;;polls++) {
        uint32_t status=io->read(io->ctx,RADEON_RBBM_STATUS);
        if (status==UINT32_MAX) return -EIO;
        d->engine->status=status;
        if (io->now(io->ctx)-d->slice_started>=RADEON_WAIT_US) return -ETIMEDOUT;
        if ((status&RADEON_RBBM_FIFOCNT_MASK)>=need) return 0;
        d->fifo_waits++;
        if (polls>=RADEON_POLLS || io->now(io->ctx)-d->slice_started>=RADEON_WAIT_US) return -ETIMEDOUT;
        if (io->delay(io->ctx,RADEON_POLL_US)) return -EIO;
    }
}
static int emit(struct radeon3d_device *d,const uint32_t (*pairs)[2],unsigned n)
{
    struct radeon_io *io=&d->engine->io;
    for (unsigned at=0;at<n;) {
        unsigned chunk=n-at>32 ? 32 : n-at;
        int e=fifo(d,chunk); if (e) return e;
        for (unsigned i=0;i<chunk;i++) { io->write(io->ctx,pairs[at+i][0],pairs[at+i][1]); d->writes++; }
        at+=chunk;
    }
    return 0;
}
/* 2D proof (FIFO drained, GUI idle, 2D cache flushed) then the 3D caches,
 * as X.Org RADEONEngineFlush (<=RV280) and Linux r100_fence_ring_emit. */
static int idle3d(struct radeon3d_device *d)
{
    int e=radeon_engine_idle(d->engine); if (e) return e;
    struct radeon_io *io=&d->engine->io;
    uint32_t dc=io->read(io->ctx,RADEON_RB3D_DSTCACHE_CTLSTAT),zc=io->read(io->ctx,RADEON_RB3D_ZCACHE_CTLSTAT);
    if (dc==UINT32_MAX || zc==UINT32_MAX) return -EIO;
    uint32_t flush[2][2]={{RADEON_RB3D_DSTCACHE_CTLSTAT,dc|RADEON_RB3D_DC_FLUSH_ALL},{RADEON_RB3D_ZCACHE_CTLSTAT,zc|RADEON_RB3D_ZC_FLUSH_ALL}};
    d->engine->cpu_safe=false;
    e=emit(d,flush,2); if (e) return e;
    for (unsigned polls=0;;polls++) {
        uint32_t v=io->read(io->ctx,RADEON_RB3D_DSTCACHE_CTLSTAT);
        if (v==UINT32_MAX) return -EIO;
        if (!(v&RADEON_RB3D_DC_BUSY)) break;
        if (polls>=RADEON_POLLS || io->now(io->ctx)-d->slice_started>=RADEON_WAIT_US) return -ETIMEDOUT;
        if (io->delay(io->ctx,RADEON_POLL_US)) return -EIO;
    }
    return radeon_engine_idle(d->engine);
}
static int save_state(struct radeon3d_device *d)
{
    const uint16_t *list; unsigned n=radeon3d_state_registers(d->r200,&list);
    for (unsigned i=0;i<n;i++) {
        uint32_t v=mmio_read(d,list[i]);
        if (v==UINT32_MAX && list[i]!=RADEON_RB3D_PLANEMASK) return -EIO;
        d->saved[i]=v;
    }
    for (unsigned i=0;i<6;i++) d->saved_2d[i]=mmio_read(d,state2d[i]);
    /* DP_WRITE_MSK legitimately reads all ones. */
    return d->saved_2d[0]==UINT32_MAX || d->saved_2d[1]==UINT32_MAX ? -EIO : 0;
}
static int restore_state(struct radeon3d_device *d)
{
    const uint16_t *list; unsigned n=radeon3d_state_registers(d->r200,&list);
    uint32_t pairs[RADEON3D_STATE_MAX+6][2];
    for (unsigned i=0;i<n;i++) { pairs[i][0]=list[i]; pairs[i][1]=d->saved[i]; }
    for (unsigned i=0;i<6;i++) { pairs[n+i][0]=state2d[i]; pairs[n+i][1]=d->saved_2d[i]; }
    int e=emit(d,pairs,n+6); if (e) return e;
    e=fifo(d,64); if (e) return e; /* posted writes landed */
    d->engine->io.drain(d->engine->io.ctx);
    for (unsigned i=0;i<n;i++)
        if ((mmio_read(d,list[i])&readback_mask(list[i]))!=(d->saved[i]&readback_mask(list[i]))) return -EIO;
    if (mmio_read(d,RADEON_DST_PITCH_OFFSET)!=d->saved_2d[0] || mmio_read(d,RADEON_SRC_PITCH_OFFSET)!=d->saved_2d[1]) return -EIO;
    return 0;
}

/* ---- Admission --------------------------------------------------------- */
static bool overlaps(struct render3d_extent a,struct render3d_extent b)
{ return (uint64_t)a.first+a.bytes>b.first && (uint64_t)b.first+b.bytes>a.first; }
static bool fullsize(const struct radeon3d_device *d)
{ return d && (d->fullsize_qualified || d->fullsize_testing) && d->mode_width && d->mode_height; }
static bool dimensions(const struct radeon3d_device *d,const struct render3d_resource *r)
{ return fullsize(d) ? r->width<=d->mode_width && r->height<=d->mode_height : r->width<=64 && r->height<=64; }
static int layout(void *device,struct render3d_resource *r)
{
    const struct radeon3d_device *d=device;
    if (!r || !r->width || !r->height || (r->kind==R3D_TEXTURE ? (r->width>1024 || r->height>1024) : !dimensions(d,r)) ||
        r->kind<R3D_COLOR || r->kind>R3D_LINEAR_COLOR) return -EOPNOTSUPP;
    if (r->kind==R3D_TEXTURE && render3d_texture_shape(r)) return -EINVAL;
    r->reserved=padded_bytes(r)+2*RADEON3D_GUARD;
    return 0;
}
static uint32_t base(const struct render3d_resource *r) { return r->offset+RADEON3D_GUARD; }
static uint32_t chip(const struct radeon3d_device *d,const struct render3d_resource *r)
{ return d->engine->local_base+base(r); }
static bool extent(const struct radeon3d_device *d,const struct render3d_resource *r)
{
    uint32_t pool=d->heap.first+d->heap.bytes-(d->texture_bytes ? d->texture_bytes : RADEON3D_TEXTURE_MIN);
    return r && r->kind>=R3D_COLOR && r->kind<=R3D_LINEAR_COLOR &&
        r->hardware && r->shadow && r->width && r->height && (r->kind==R3D_TEXTURE ? !render3d_texture_shape(r) : dimensions(d,r)) &&
        r->offset>=d->heap.first && !(r->offset&4095u) &&
        (uint64_t)r->offset+r->reserved<=(uint64_t)d->heap.first+d->heap.bytes &&
        (r->kind==R3D_TEXTURE ? r->offset>=pool : (uint64_t)r->offset+r->reserved<=pool) &&
        r->reserved==padded_bytes(r)+2*RADEON3D_GUARD && r->pitch==r->width*(r->kind==R3D_D16 ? 2u : 4u) &&
        (r->kind==R3D_TEXTURE || r->bytes==r->pitch*r->height);
}
/* Same sample/LOD proof as the GL encoder and the Savage validator. */
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
    struct radeon3d_device *d=device;
    if (!d || !d->engine || !d->engine->group || (d->engine->group_id!=RADEON_R100 && d->engine->group_id!=RADEON_R200) ||
        d->r200!=(d->engine->group_id==RADEON_R200) || !d->heap.first || !d->heap.bytes ||
        d->heap.first>UINT32_MAX-d->heap.bytes || (d->heap.first&4095u) || (d->heap.bytes&4095u) ||
        (uint64_t)d->engine->local_base+d->heap.first+d->heap.bytes>0xffffffffu ||
        (uint64_t)d->heap.first+d->heap.bytes>256u*1024*1024 || d->heap.bytes<=RADEON3D_TEXTURE_MIN ||
        (d->texture_bytes && ((d->texture_bytes&4095u) || d->texture_bytes<RADEON3D_TEXTURE_MIN || d->texture_bytes>=d->heap.bytes)) ||
        !q || !r || !q->flags || (q->flags&~RADEON3D_FEATURES) ||
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
    for (unsigned stage=0;stage<2;stage++) if (q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED)) {
        const struct render3d_resource *t=r[stage ? 4 : 2];
        unsigned limit=d->perspective_qualified || d->fullsize_testing ? 256 : 64;
        if (t->width>limit || t->height>limit) return -EOPNOTSUPP;
        if (q->state.sampler[stage].combine==R3D_DECAL && t->format==R3D_A8) return -EOPNOTSUPP;
    }
    if ((q->flags&R3D_COPY) && (!r[3] || (r[3]->kind!=R3D_LINEAR_COLOR && r[3]->kind!=R3D_COLOR) ||
        r[3]->width!=r[0]->width || r[3]->height!=r[0]->height)) return -EINVAL;
    /* The 2D blit needs a 64-byte destination pitch (DST_PITCH_OFFSET). */
    if ((q->flags&R3D_COPY) && (r[3]->kind==R3D_COLOR || r[3]->width%16)) return -EOPNOTSUPP;
    for (unsigned i=0;i<q->count;i++) if (q->vertices[i].x<0 || q->vertices[i].y<0 ||
        q->vertices[i].x>(int32_t)(r[0]->width<<16) || q->vertices[i].y>(int32_t)(r[0]->height<<16)) return -EINVAL;
    for (unsigned i=0;i<q->count;i++)
        if ((q->vertices[i].x&4095) || (q->vertices[i].y&4095)) return -EOPNOTSUPP;
    if (!d->fullsize_testing && !d->perspective_qualified && q->state.blend &&
        (q->state.source>R3D_INV_SRC_ALPHA || q->state.destination>R3D_INV_SRC_ALPHA)) return -EOPNOTSUPP;
    if (!d->fullsize_testing && (q->flags&R3D_TEXTURED))
      for (unsigned first=0;first<q->count;first+=3) {
        bool varying=q->vertices[first].rhw!=q->vertices[first+1].rhw ||
                     q->vertices[first].rhw!=q->vertices[first+2].rhw;
        if (!varying) continue;
        if (!d->perspective_qualified) return -EOPNOTSUPP;
        unsigned lo=UINT32_MAX,hi=0;
        for (unsigned i=first;i<first+3;i++) {
            const struct render3d_vertex *v=&q->vertices[i];
            if (v->rhw<32768 || v->rhw>16777216 || (v->x&4095) || (v->y&4095)) return -EOPNOTSUPP;
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
    uint32_t words[RADEON3D_WORDS_PER_TRIANGLE];
    for (unsigned first=0;first<q->count;first+=3) {
        int e=radeon3d_triangle_words(q,first,d->r200,words); if (e) return e;
    }
    return 0;
}

/* ---- VRAM transfers ---------------------------------------------------- */
static uint32_t memory_read(struct radeon3d_device *d,uint32_t off,unsigned bytes)
{ return d->memory.read(d->memory.ctx,off,bytes); }
static void memory_write(struct radeon3d_device *d,uint32_t off,uint32_t value,unsigned bytes)
{ d->memory.write(d->memory.ctx,off,value,bytes); }
static int guards(struct radeon3d_device *d,struct render3d_resource *r,bool initialize)
{
    uint64_t start=0; int e=0;
    for (unsigned side=0;side<2;side++) for (unsigned i=0;i<RADEON3D_GUARD;i+=4) {
        if (!(i%512)) start=d->engine->io.now(d->engine->io.ctx);
        uint32_t off=r->offset+(side ? r->reserved-RADEON3D_GUARD : 0)+i;
        if (initialize) memory_write(d,off,0xa55ac33cu,4);
        else {
            uint32_t value=memory_read(d,off,4);
            /* Corruption is recorded before any deadline decision. */
            if (value!=0xa55ac33cu) { d->expected=0xa55ac33cu; d->actual=value; d->guard_offset=off; return -EIO; }
        }
        if (i%512==508 && d->engine->io.now(d->engine->io.ctx)-start>=1000) e=-ETIMEDOUT;
        if (e) return e;
    }
    return 0;
}
static int upload(struct radeon3d_device *d,struct render3d_resource *r)
{
    if (r->resident) return guards(d,r,false);
    int e=guards(d,r,true); if (e) return e;
    uint64_t chunk_start=0; unsigned chunk=0;
    if (r->kind==R3D_TEXTURE) {
        unsigned bpp=texel_bytes(r);
        for (unsigned level=0;level<r->levels;level++) {
            unsigned w=r->width>>level,h=r->height>>level;
            if (!w) w=1;
            if (!h) h=1;
            const uint32_t *pixels=(const void *)((const uint8_t *)r->shadow+r->level_offset[level]);
            uint32_t first; (void)radeon3d_mip_offset(r,level,0,0,&first);
            uint32_t stride=level_stride(w,bpp);
            for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
                if (!chunk) chunk_start=d->engine->io.now(d->engine->io.ctx);
                uint32_t value=pixels[y*w+x];
                if (r->format==R3D_RGB565) value=((value>>8)&0xf800)|((value>>5)&0x7e0)|((value>>3)&31);
                else if (r->format==R3D_ARGB4444) value=((value>>16)&0xf000)|((value>>12)&0xf00)|((value>>8)&0xf0)|((value>>4)&15);
                memory_write(d,base(r)+first+y*stride+x*bpp,value,bpp);
                if (++chunk==128 || (x==w-1 && y==h-1)) {
                    if (d->engine->io.now(d->engine->io.ctx)-chunk_start>=1000) return -ETIMEDOUT;
                    chunk=0;
                }
            }
        }
    } else {
        unsigned bytes=r->kind==R3D_D16 ? 2 : 4; uint32_t pitch=hw_pitch(r);
        for (unsigned y=0;y<r->height;y++) for (unsigned x=0;x<r->width;x++) {
            if (!chunk) chunk_start=d->engine->io.now(d->engine->io.ctx);
            uint32_t value=0; memcpy(&value,(uint8_t *)r->shadow+y*r->pitch+x*bytes,bytes);
            memory_write(d,base(r)+y*pitch+x*bytes,value,bytes);
            if (++chunk==128 || (x==r->width-1 && y==r->height-1)) {
                if (d->engine->io.now(d->engine->io.ctx)-chunk_start>=1000) return -ETIMEDOUT;
                chunk=0;
            }
        }
    }
    d->engine->io.drain(d->engine->io.ctx); r->resident=true; return 0;
}
static int read_resource(struct radeon3d_device *d,struct render3d_resource *r)
{
    if (r->kind==R3D_TEXTURE) return guards(d,r,false);
    unsigned bytes=r->kind==R3D_D16 ? 2 : 4; uint32_t pitch=hw_pitch(r);
    for (unsigned y=0;y<r->height;y++) {
        if (d->fullsize_testing && y%16 && y!=r->height-1) continue;
        for (unsigned first=0;first<r->width;first+=128) {
            uint64_t start=d->engine->io.now(d->engine->io.ctx);
            unsigned end=first+128; if (end>r->width) end=r->width;
            for (unsigned x=first;x<end;x++) {
                uint32_t value=memory_read(d,base(r)+y*pitch+x*bytes,bytes);
                memcpy((uint8_t *)r->shadow+y*r->pitch+x*bytes,&value,bytes);
            }
            if (d->engine->io.now(d->engine->io.ctx)-start>=1000) return -ETIMEDOUT;
        }
    }
    return guards(d,r,false);
}

/* ---- 2D clears and the conversion copy ---------------------------------- */
static uint32_t pitch_offset(const struct radeon3d_device *d,const struct render3d_resource *r)
{ return ((hw_pitch(r)/64u)<<22)|(chip(d,r)>>10); }
static int clear_resource(struct radeon3d_device *d,struct render3d_resource *r,uint32_t value)
{
    bool depth=r->kind==R3D_D16;
    /* X.Org EXA PrepareSolid/Emit2DState with the pixmap's pitch/offset. */
    uint32_t pairs[7][2]={{RADEON_DST_PITCH_OFFSET,pitch_offset(d,r)},
        {RADEON_DP_GUI_MASTER_CNTL,((depth ? RADEON_DATATYPE_RGB565 : RADEON_DATATYPE_ARGB8888)<<8)|RADEON_GMC_CLR_CMP_CNTL_DIS|RADEON_GMC_DST_PITCH_OFFSET_CNTL|RADEON_GMC_SRC_DSTCOLOR|RADEON_GMC_BRUSH_SOLID_COLOR|RADEON_ROP3_P},
        {RADEON_DP_BRUSH_FRGD_CLR,depth ? value&65535u : value},{RADEON_DP_WRITE_MSK,0xffffffffu},
        {RADEON_DP_CNTL,RADEON_DST_X_LEFT_TO_RIGHT|RADEON_DST_Y_TOP_TO_BOTTOM},{RADEON_DST_Y_X,0},
        {RADEON_DST_WIDTH_HEIGHT,(r->width<<16)|r->height}};
    int e=emit(d,pairs,7); if (e) return e;
    return idle3d(d);
}
static int copy_resource(struct radeon3d_device *d,struct render3d_resource *src,struct render3d_resource *dst)
{
    uint32_t pairs[9][2]={{RADEON_SRC_PITCH_OFFSET,pitch_offset(d,src)},{RADEON_DST_PITCH_OFFSET,pitch_offset(d,dst)},
        {RADEON_DP_GUI_MASTER_CNTL,(RADEON_DATATYPE_ARGB8888<<8)|RADEON_GMC_CLR_CMP_CNTL_DIS|RADEON_GMC_SRC_PITCH_OFFSET_CNTL|RADEON_GMC_DST_PITCH_OFFSET_CNTL|RADEON_GMC_BRUSH_NONE|RADEON_GMC_SRC_DSTCOLOR|RADEON_ROP3_S|RADEON_DP_SRC_SOURCE_MEMORY},
        {RADEON_DP_WRITE_MSK,0xffffffffu},{RADEON_DP_CNTL,RADEON_DST_X_LEFT_TO_RIGHT|RADEON_DST_Y_TOP_TO_BOTTOM},
        {RADEON_SRC_Y_X,0},{RADEON_DST_Y_X,0},{RADEON_DST_HEIGHT_WIDTH,(src->height<<16)|src->width},{RADEON_SRC_PITCH_OFFSET,pitch_offset(d,src)}};
    int e=emit(d,pairs,8); if (e) return e;
    e=idle3d(d); if (!e) d->copies++;
    return e;
}

/* ---- 3D state ---------------------------------------------------------- */
struct state_image { const uint16_t *list; unsigned n; uint32_t value[RADEON3D_STATE_MAX]; };
static void put(struct state_image *s,uint16_t reg_,uint32_t value)
{ for (unsigned i=0;i<s->n;i++) if (s->list[i]==reg_) { s->value[i]=value; return; } }
static unsigned log2u(unsigned v) { unsigned n=0; while ((1u<<n)<v) n++; return n; }
static uint32_t filter_bits(const struct render3d_sampler *s,unsigned levels)
{
    static const uint8_t min[]={RADEON_MIN_FILTER_NEAREST,RADEON_MIN_FILTER_LINEAR,RADEON_MIN_FILTER_NEAREST_MIP_NEAREST,
        RADEON_MIN_FILTER_LINEAR_MIP_NEAREST,RADEON_MIN_FILTER_LINEAR_MIP_LINEAR};
    bool linear=s->filter==R3D_BILINEAR || s->filter==R3D_BILINEAR_MIP || s->filter==R3D_TRILINEAR;
    bool mips=s->filter>=R3D_NEAREST_MIP;
    return (linear ? RADEON_MAG_FILTER_LINEAR : 0)|((uint32_t)min[s->filter]<<RADEON_MIN_FILTER_SHIFT)|
        ((mips ? levels-1 : 0u)<<RADEON_MAX_MIP_LEVEL_SHIFT)|
        (s->repeat_u ? RADEON_CLAMP_S_WRAP : RADEON_CLAMP_S_CLAMP_LAST)|(s->repeat_v ? RADEON_CLAMP_T_WRAP : RADEON_CLAMP_T_CLAMP_LAST);
}
static uint32_t format_bits(const struct render3d_resource *t)
{
    uint32_t f=t->format==R3D_RGB565 ? RADEON_TXFORMAT_RGB565 : t->format==R3D_ARGB4444 ? RADEON_TXFORMAT_ARGB4444|RADEON_TXFORMAT_ALPHA_IN_MAP :
        RADEON_TXFORMAT_ARGB8888|RADEON_TXFORMAT_ALPHA_IN_MAP;
    return f|(log2u(t->width)<<RADEON_TXFORMAT_WIDTH_SHIFT)|(log2u(t->height)<<RADEON_TXFORMAT_HEIGHT_SHIFT);
}
static void stage_r100(struct state_image *s,unsigned n,const struct render3d_sampler *sm,const struct render3d_resource *t)
{
    uint32_t tc=n ? RADEON_COLOR_ARG_T1_COLOR : RADEON_COLOR_ARG_T0_COLOR,tca=n ? RADEON_COLOR_ARG_T1_ALPHA : RADEON_COLOR_ARG_T0_ALPHA;
    uint32_t ta=n ? RADEON_ALPHA_ARG_T1_ALPHA : RADEON_ALPHA_ARG_T0_ALPHA,common=RADEON_SCALE_1X|RADEON_CLAMP_TX;
    bool rgb=t->format==R3D_RGB565 || t->format==R3D_L8,alpha=t->format==R3D_A8;
    uint32_t c,a;
    /* Mesa radeon_texstate.c slot assignments for MODULATE/REPLACE/ADD and
     * INTERPOLATE (DECAL), with the native per-format passthrough rules. */
    if (sm->combine==R3D_REPLACE || (sm->combine==R3D_DECAL && rgb)) {
        c=((alpha ? RADEON_COLOR_ARG_CURRENT_COLOR : tc)<<RADEON_COLOR_ARG_C_SHIFT)|RADEON_BLEND_CTL_ADD;
        a=((rgb || sm->combine==R3D_DECAL ? RADEON_ALPHA_ARG_CURRENT_ALPHA : ta)<<RADEON_ALPHA_ARG_C_SHIFT)|RADEON_BLEND_CTL_ADD;
    } else if (sm->combine==R3D_DECAL) {
        c=RADEON_COLOR_ARG_CURRENT_COLOR|(tc<<RADEON_COLOR_ARG_B_SHIFT)|(tca<<RADEON_COLOR_ARG_C_SHIFT)|RADEON_BLEND_CTL_BLEND;
        a=(RADEON_ALPHA_ARG_CURRENT_ALPHA<<RADEON_ALPHA_ARG_C_SHIFT)|RADEON_BLEND_CTL_ADD;
    } else if (sm->combine==R3D_ADD) {
        c=RADEON_COLOR_ARG_CURRENT_COLOR|RADEON_COMP_ARG_B|((alpha ? RADEON_COLOR_ARG_ZERO : tc)<<RADEON_COLOR_ARG_C_SHIFT)|RADEON_BLEND_CTL_ADD;
        a=RADEON_ALPHA_ARG_CURRENT_ALPHA|(ta<<RADEON_ALPHA_ARG_B_SHIFT)|RADEON_BLEND_CTL_ADD;
    } else {
        c=RADEON_COLOR_ARG_CURRENT_COLOR|(tc<<RADEON_COLOR_ARG_B_SHIFT)|RADEON_BLEND_CTL_ADD;
        a=RADEON_ALPHA_ARG_CURRENT_ALPHA|(ta<<RADEON_ALPHA_ARG_B_SHIFT)|RADEON_BLEND_CTL_ADD;
    }
    uint32_t stride=n*RADEON_PP_TEX_UNIT_STRIDE;
    put(s,RADEON_PP_TXCBLEND_0+stride,c|common); put(s,RADEON_PP_TXABLEND_0+stride,a|common);
}
static void stage_r200(struct state_image *s,unsigned n,const struct render3d_sampler *sm,const struct render3d_resource *t)
{
    uint32_t tc=n ? R200_TXC_ARG_R1_COLOR : R200_TXC_ARG_R0_COLOR,tca=n ? R200_TXC_ARG_R1_ALPHA : R200_TXC_ARG_R0_ALPHA;
    uint32_t prev=n ? R200_TXC_ARG_R0_COLOR : R200_TXC_ARG_DIFFUSE_COLOR;
    uint32_t ta=n ? R200_TXA_ARG_R1_ALPHA : R200_TXA_ARG_R0_ALPHA,preva=n ? R200_TXA_ARG_R0_ALPHA : R200_TXA_ARG_DIFFUSE_ALPHA;
    bool rgb=t->format==R3D_RGB565 || t->format==R3D_L8,alpha=t->format==R3D_A8;
    uint32_t c,a;
    if (sm->combine==R3D_REPLACE || (sm->combine==R3D_DECAL && rgb)) {
        c=((alpha ? prev : tc)<<R200_TX_ARG_C_SHIFT)|R200_TX_OP_MADD;
        a=((rgb || sm->combine==R3D_DECAL ? preva : ta)<<R200_TX_ARG_C_SHIFT)|R200_TX_OP_MADD;
    } else if (sm->combine==R3D_DECAL) {
        c=prev|(tc<<R200_TX_ARG_B_SHIFT)|(tca<<R200_TX_ARG_C_SHIFT)|R200_TX_OP_LERP;
        a=(preva<<R200_TX_ARG_C_SHIFT)|R200_TX_OP_MADD;
    } else if (sm->combine==R3D_ADD) {
        c=prev|R200_TX_COMP_ARG_B|((alpha ? R200_TXC_ARG_ZERO : tc)<<R200_TX_ARG_C_SHIFT)|R200_TX_OP_MADD;
        a=preva|(ta<<R200_TX_ARG_B_SHIFT)|R200_TX_OP_MADD;
    } else {
        c=prev|(tc<<R200_TX_ARG_B_SHIFT)|R200_TX_OP_MADD;
        a=preva|(ta<<R200_TX_ARG_B_SHIFT)|R200_TX_OP_MADD;
    }
    uint32_t stride=n*R200_PP_BLEND_STRIDE,out=R200_TX_CLAMP_0_1|R200_TX_OUTPUT_REG_R0;
    put(s,R200_PP_TXCBLEND_0+stride,c); put(s,R200_PP_TXCBLEND2_0+stride,out);
    put(s,R200_PP_TXABLEND_0+stride,a); put(s,R200_PP_TXABLEND2_0+stride,out);
}
static int state(struct radeon3d_device *d,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    static const uint8_t func[]={1,0,7,3,2,5,6,4},factor[]={33,32,38,39,36,34,37,35,40,41};
    struct state_image s; s.n=radeon3d_state_registers(d->r200,&s.list);
    memcpy(s.value,d->saved,sizeof(s.value));
    bool ztest=q->flags&R3D_DEPTH_TEST,zwrite=q->flags&R3D_DEPTH_WRITE,tex0=q->flags&R3D_TEXTURED,tex1=q->flags&R3D_TEXTURE1;
    put(&s,RADEON_PP_MISC,(q->state.alpha_ref&RADEON_REF_ALPHA_MASK)|((uint32_t)func[q->state.alpha_test ? q->state.alpha_func : R3D_ALWAYS]<<RADEON_ALPHA_TEST_OP_SHIFT));
    put(&s,RADEON_PP_FOG_COLOR,0);
    put(&s,RADEON_RB3D_BLENDCNTL,RADEON_COMB_FCN_ADD_CLAMP|((uint32_t)factor[q->state.blend ? q->state.source : R3D_ONE]<<RADEON_SRC_BLEND_SHIFT)|
        ((uint32_t)factor[q->state.blend ? q->state.destination : R3D_ZERO]<<RADEON_DST_BLEND_SHIFT));
    put(&s,RADEON_RB3D_DEPTHOFFSET,r[1] ? chip(d,r[1]) : chip(d,r[0]));
    put(&s,RADEON_RB3D_DEPTHPITCH,(r[1] ? radeon3d_depth_pitch(r[1]->width) : radeon3d_color_pitch(r[0]->width))&RADEON_DEPTHPITCH_MASK);
    put(&s,RADEON_RB3D_ZSTENCILCNTL,RADEON_DEPTH_FORMAT_16BIT_INT_Z|((uint32_t)func[ztest ? q->state.depth_func : R3D_ALWAYS]<<RADEON_Z_TEST_SHIFT)|
        RADEON_STENCIL_TEST_ALWAYS|(zwrite ? RADEON_Z_WRITE_ENABLE : 0));
    uint32_t pp=RADEON_SCISSOR_ENABLE|RADEON_ANTI_ALIAS_NONE|(q->state.alpha_test ? RADEON_ALPHA_TEST_ENABLE : 0)|
        (tex0 ? RADEON_TEX_0_ENABLE|RADEON_TEX_BLEND_0_ENABLE : 0)|(tex1 ? RADEON_TEX_1_ENABLE|RADEON_TEX_BLEND_1_ENABLE : 0);
    if (d->r200) pp|=RADEON_TEX_BLEND_0_ENABLE; /* r200_texstate.c: one stage at least */
    put(&s,RADEON_PP_CNTL,pp);
    put(&s,RADEON_RB3D_CNTL,RADEON_PLANE_MASK_ENABLE|RADEON_COLOR_FORMAT_ARGB8888|(q->state.blend ? RADEON_ALPHA_BLEND_ENABLE : 0)|
        (ztest || zwrite ? RADEON_Z_ENABLE : 0));
    put(&s,RADEON_RB3D_COLOROFFSET,chip(d,r[0])&RADEON_COLOROFFSET_MASK);
    put(&s,RADEON_RE_WIDTH_HEIGHT,r[0]->width|(r[0]->height<<16));
    put(&s,RADEON_RB3D_COLORPITCH,radeon3d_color_pitch(r[0]->width)&RADEON_COLORPITCH_MASK);
    put(&s,RADEON_SE_CNTL,RADEON_BFACE_SOLID|RADEON_FFACE_SOLID|RADEON_DIFFUSE_SHADE_GOURAUD|RADEON_ALPHA_SHADE_GOURAUD|
        RADEON_VTX_PIX_CENTER_OGL|RADEON_ROUND_MODE_ROUND|RADEON_ROUND_PREC_16TH_PIX);
    put(&s,RADEON_RB3D_STENCILREFMASK,(0xffu<<16)|(0xffu<<24));
    put(&s,RADEON_RB3D_ROPCNTL,RADEON_ROP_COPY); put(&s,RADEON_RB3D_PLANEMASK,0xffffffffu);
    put(&s,RADEON_SE_CNTL_STATUS,d->tcl_bypass ? RADEON_TCL_BYPASS : 0);
    put(&s,RADEON_RE_TOP_LEFT,0); put(&s,RADEON_RE_MISC,RADEON_STIPPLE_BIG_BIT_ORDER);
    uint32_t pool=d->engine->local_base+d->heap.first;
    if (!d->r200) {
        put(&s,RADEON_SE_COORD_FMT,RADEON_VTX_XY_PRE_MULT_1_OVER_W0|RADEON_VTX_Z_PRE_MULT_1_OVER_W0|RADEON_TEX1_W_ROUTING_USE_W0);
        put(&s,RADEON_SE_VTX_FMT,RADEON_SE_VTX_FMT_Z|RADEON_SE_VTX_FMT_W0|RADEON_SE_VTX_FMT_PKCOLOR|RADEON_SE_VTX_FMT_ST0|RADEON_SE_VTX_FMT_ST1);
        for (unsigned n=0;n<2;n++) {
            const struct render3d_resource *t=n ? r[4] : r[2]; uint32_t u=n*RADEON_PP_TEX_UNIT_STRIDE;
            put(&s,RADEON_PP_TFACTOR_0+u,0); put(&s,RADEON_PP_BORDER_COLOR_0+n*4,0);
            if (!(n ? tex1 : tex0)) {
                put(&s,RADEON_PP_TXFILTER_0+u,0);
                put(&s,RADEON_PP_TXFORMAT_0+u,(2u<<RADEON_TXFORMAT_WIDTH_SHIFT)|(2u<<RADEON_TXFORMAT_HEIGHT_SHIFT)|(n<<RADEON_TXFORMAT_ST_ROUTE_SHIFT)|RADEON_TXFORMAT_PERSPECTIVE_ENABLE);
                put(&s,RADEON_PP_TXOFFSET_0+u,pool);
                put(&s,RADEON_PP_TEX_SIZE_0+n*8,0); put(&s,RADEON_PP_TEX_PITCH_0+n*8,0);
                put(&s,RADEON_PP_TXCBLEND_0+u,(RADEON_COLOR_ARG_CURRENT_COLOR<<RADEON_COLOR_ARG_C_SHIFT)|RADEON_BLEND_CTL_ADD|RADEON_SCALE_1X|RADEON_CLAMP_TX);
                put(&s,RADEON_PP_TXABLEND_0+u,(RADEON_ALPHA_ARG_CURRENT_ALPHA<<RADEON_ALPHA_ARG_C_SHIFT)|RADEON_BLEND_CTL_ADD|RADEON_SCALE_1X|RADEON_CLAMP_TX);
                continue;
            }
            put(&s,RADEON_PP_TXFILTER_0+u,filter_bits(&q->state.sampler[n],t->levels));
            put(&s,RADEON_PP_TXFORMAT_0+u,format_bits(t)|(n<<RADEON_TXFORMAT_ST_ROUTE_SHIFT)|RADEON_TXFORMAT_PERSPECTIVE_ENABLE);
            put(&s,RADEON_PP_TXOFFSET_0+u,chip(d,t));
            put(&s,RADEON_PP_TEX_SIZE_0+n*8,(t->width-1)|((t->height-1)<<RADEON_TEX_VSIZE_SHIFT));
            put(&s,RADEON_PP_TEX_PITCH_0+n*8,level_stride(t->width,texel_bytes(t))-32);
            stage_r100(&s,n,&q->state.sampler[n],t);
        }
    } else {
        put(&s,R200_RE_CNTL,R200_PERSPECTIVE_ENABLE|R200_SCISSOR_ENABLE_RE);
        put(&s,R200_RE_SCISSOR_TL_0,0); put(&s,R200_RE_SCISSOR_BR_0,0); put(&s,R200_RB3D_DEPTHXY_OFFSET,0);
        put(&s,R200_SE_VAP_CNTL,R200_VAP_VF_MAX_VTX_NUM);
        put(&s,R200_SE_VTX_FMT_0,R200_VTX_Z0|R200_VTX_W0|(R200_VTX_PK_RGBA<<R200_VTX_COLOR_0_SHIFT));
        put(&s,R200_SE_VTX_FMT_1,(2u<<R200_VTX_TEX0_COMP_CNT_SHIFT)|(2u<<R200_VTX_TEX1_COMP_CNT_SHIFT));
        put(&s,R200_SE_VTE_CNTL,R200_VTX_XY_FMT|R200_VTX_Z_FMT);
        put(&s,R200_SE_VTX_STATE_CNTL,0); put(&s,R200_RE_AUX_SCISSOR_CNTL,0); put(&s,R200_PP_CNTL_X,0);
        for (unsigned n=0;n<2;n++) {
            const struct render3d_resource *t=n ? r[4] : r[2]; uint32_t u=n*R200_PP_TEX_UNIT_STRIDE;
            put(&s,R200_PP_TFACTOR_0+n*4,0); put(&s,R200_PP_BORDER_COLOR_0+u,0); put(&s,R200_PP_TXMULTI_CTL_0+u,0);
            put(&s,R200_PP_TXFORMAT_X_0+u,R200_TEXCOORD_NONPROJ|R200_LOD_BIAS_CORRECTION);
            uint32_t out=R200_TX_CLAMP_0_1|R200_TX_OUTPUT_REG_R0,b=n*R200_PP_BLEND_STRIDE;
            if (!(n ? tex1 : tex0)) {
                put(&s,R200_PP_TXFILTER_0+u,0);
                put(&s,R200_PP_TXFORMAT_0+u,(2u<<RADEON_TXFORMAT_WIDTH_SHIFT)|(2u<<RADEON_TXFORMAT_HEIGHT_SHIFT)|(n<<RADEON_TXFORMAT_ST_ROUTE_SHIFT));
                put(&s,R200_PP_TXSIZE_0+u,0); put(&s,R200_PP_TXPITCH_0+u,0);
                put(&s,R200_PP_TXOFFSET_0+n*R200_PP_TXOFFSET_STRIDE,pool);
                put(&s,R200_PP_TXCBLEND_0+b,(R200_TXC_ARG_DIFFUSE_COLOR<<R200_TX_ARG_C_SHIFT)|R200_TX_OP_MADD); put(&s,R200_PP_TXCBLEND2_0+b,out);
                put(&s,R200_PP_TXABLEND_0+b,(R200_TXA_ARG_DIFFUSE_ALPHA<<R200_TX_ARG_C_SHIFT)|R200_TX_OP_MADD); put(&s,R200_PP_TXABLEND2_0+b,out);
                continue;
            }
            put(&s,R200_PP_TXFILTER_0+u,filter_bits(&q->state.sampler[n],t->levels));
            put(&s,R200_PP_TXFORMAT_0+u,format_bits(t)|(n<<RADEON_TXFORMAT_ST_ROUTE_SHIFT));
            put(&s,R200_PP_TXSIZE_0+u,(t->width-1)|((t->height-1)<<RADEON_TEX_VSIZE_SHIFT));
            put(&s,R200_PP_TXPITCH_0+u,level_stride(t->width,texel_bytes(t))-32);
            put(&s,R200_PP_TXOFFSET_0+n*R200_PP_TXOFFSET_STRIDE,chip(d,t));
            stage_r200(&s,n,&q->state.sampler[n],t);
        }
    }
    uint32_t pairs[RADEON3D_STATE_MAX][2];
    for (unsigned i=0;i<s.n;i++) { pairs[i][0]=s.list[i]; pairs[i][1]=s.value[i]; }
    return emit(d,pairs,s.n);
}
static int draw(struct radeon3d_device *d,const struct render3d_request *q)
{
    uint32_t words[RADEON3D_WORDS_PER_TRIANGLE]; struct radeon_io *io=&d->engine->io;
    for (unsigned first=0;first<q->count;first+=3) {
        int e=radeon3d_triangle_words(q,first,d->r200,words); if (e) return e;
        e=fifo(d,RADEON3D_WORDS_PER_TRIANGLE); if (e) return e;
        io->write(io->ctx,RADEON_SE_VF_CNTL,words[0]);
        for (unsigned i=1;i<RADEON3D_WORDS_PER_TRIANGLE;i++) io->write(io->ctx,RADEON_SE_PORT_DATA0,words[i]);
        d->writes+=RADEON3D_WORDS_PER_TRIANGLE; d->triangles++;
    }
    return 0;
}

/* ---- Backend hooks ----------------------------------------------------- */
static void quarantine(void *device);
static int execute(void *device,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    struct radeon3d_device *d=device;
    int e=validate(d,q,r); if (e) return e;
    if (!d->locked || (!d->qualified && !d->probing) || d->lost) return -ENODEV;
    if (!d->engine->active || d->engine->quarantined || !d->engine->render3d_qualified) return -ENODEV;
    d->slice_started=d->engine->io.now(d->engine->io.ctx);
    d->failed_phase=1; e=save_state(d); if (e) return e;
    d->failed_phase=2;
    for (unsigned i=0;!e && i<R3D_REFS;i++) if (r[i]) e=upload(d,r[i]);
    d->slice_started=d->engine->io.now(d->engine->io.ctx);
    if (!e) d->failed_phase=3;
    if (!e && (q->flags&R3D_CLEAR_COLOR)) e=clear_resource(d,r[0],q->clear_color);
    if (!e && (q->flags&R3D_CLEAR_DEPTH)) e=clear_resource(d,r[1],q->clear_depth);
    if (!e) d->failed_phase=4;
    if (!e && q->count) {
        e=state(d,q,r);
        if (!e) e=draw(d,q);
        if (!e) e=idle3d(d);
    }
    if (!e) d->failed_phase=5;
    if (!e && (q->flags&R3D_COPY)) e=copy_resource(d,r[0],r[3]);
    if (!e) {
        d->failed_phase=8;
        for (unsigned i=0;i<R3D_REFS && !e;i++) if (r[i]) e=read_resource(d,r[i]);
    }
    /* Restoration needs an idle proof even after a timed-out job; the
     * engine's own bounded proof quarantines when it cannot be given. */
    if (d->failed_phase>=3 && !d->engine->quarantined) {
        uint32_t phase=d->failed_phase;
        d->slice_started=d->engine->io.now(d->engine->io.ctx);
        d->failed_phase=6;
        int proof=idle3d(d);
        int restored=proof ? proof : restore_state(d);
        if (restored) { quarantine(d); return restored; }
        d->failed_phase=phase;
    }
    if (!e) d->failed_phase=0;
    return e;
}
static bool qualified(void *device,uint32_t generation)
{
    struct radeon3d_device *d=device;
    return d && d->qualified && !d->lost && d->generation==generation && d->engine && d->engine->group &&
        d->engine->active && !d->engine->quarantined && d->engine->render3d_qualified;
}
static int enter(void *device)
{
    struct radeon3d_device *d=device;
    if (!d || d->locked || d->lost || !d->memory.enter) return -EBUSY;
    int e=d->memory.enter(d->memory.ctx,d->generation);
    if (!e) d->locked=true;
    return e;
}
static void leave(void *device)
{ struct radeon3d_device *d=device; d->locked=false; d->memory.leave(d->memory.ctx); }
static int idle(void *device)
{ struct radeon3d_device *d=device; return d->engine->cpu_safe && !d->engine->quarantined ? 0 : -EIO; }
static void quarantine(void *device)
{
    struct radeon3d_device *d=device;
    if (!d || d->lost) return;
    d->qualified=d->fullsize_qualified=d->perspective_qualified=false; d->lost=true;
    d->engine->render3d_qualified=false;
    radeon_engine_quarantine(d->engine,"render3d_fault");
    d->engine->cpu_safe=false;
    if (d->lost_notify) d->lost_notify(d->memory.ctx);
}
extern int radeon3d_owner_attach(struct render3d_service *,const struct render3d_host *,uint32_t) __attribute__((weak));
static void detach(struct render3d_service *s,void *owner)
{ struct radeon3d_device *d=owner; if (d && d->consumer==s) d->consumer=0; }
static void query_caps(void *owner,struct ciuki_r3d_caps *out)
{
    const struct radeon3d_device *d=owner;
    *out=*radeon3d_backend()->capabilities;
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
{ const struct radeon3d_device *d=owner; return d && d->texture_bytes ? d->texture_bytes : RADEON3D_TEXTURE_MIN; }
const struct render3d_backend *radeon3d_backend(void)
{
    static const struct ciuki_r3d_caps capabilities={
        .version=CIUKI_R3D_VERSION,.features=RADEON3D_FEATURES,.max_size=64,
        .max_vertices=3,.texture_units=2,.max_texture_size=64,
        .depth_formats=CIUKI_R3D_D16,.color_formats=CIUKI_R3D_ARGB8888,
        .texture_formats=63,.depth_functions=255,.alpha_functions=255,
        .blend_factors=15,.combine_modes=15,.filters=31,.max_pixel_work=4096};
    static const struct render3d_backend backend={.features=RADEON3D_FEATURES,.qualified=qualified,
        .layout=layout,.validate=validate,.enter=enter,.leave=leave,.execute=execute,
        .idle=idle,.quarantine=quarantine,.attach=radeon3d_owner_attach,.max_size=64,.detach=detach,
        .texture_pool_bytes=texture_pool_bytes,.capabilities=&capabilities,.query_caps=query_caps,
        .resident_bytes=(sizeof(struct radeon3d_device)+4095u)&~4095u};
    return &backend;
}

/* ---- Boot qualification --------------------------------------------------- */
int radeon3d_probe(struct radeon3d_device *d)
{
    if (!d || !d->engine || !d->engine->group || !d->memory.read || !d->memory.write || !d->memory.enter ||
        !d->memory.leave || !d->generation || !d->heap.bytes || (d->heap.first&4095u) ||
        d->heap.first>UINT32_MAX-d->heap.bytes || d->heap.bytes<RADEON3D_TEXTURE_MIN+96*1024u ||
        !d->engine->active || d->engine->quarantined) return -ENODEV;
    if (!d->consumer) return -ENOMEM;
    struct render3d_context *c=&d->consumer->contexts[0];
    if (!c->resources) return -ENOMEM;
    struct render3d_resource *r[R3D_REFS]={0};
    for (unsigned i=0;i<4;i++) r[i]=&c->resources[i];
    struct render3d_request *q=(void *)d->consumer->ring[0];
    memset(q,0,sizeof(*q)); q->flags=R3D_FEATURES; q->count=3; q->clear_depth=65535;
    q->vertices[0]=(struct render3d_vertex){8<<16,8<<16,16384,65536,0xffffffffu,0,0};
    q->vertices[1]=(struct render3d_vertex){56<<16,8<<16,16384,65536,0xffffffffu,65536,0};
    q->vertices[2]=(struct render3d_vertex){8<<16,56<<16,16384,65536,0xffffffffu,0,65536};
    int e=validate(d,q,r); if (e) return e;
    e=enter(d); if (e) return e;
    d->probing=true; d->engine->render3d_qualified=true;
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
            ((uint16_t *)r[1]->shadow)[63*64+63]!=65535) e=-EIO;
        if (copy!=0x4080c0u) d->actual=copy;
        if (depth<16383 || depth>16385) { d->expected=16384; d->actual=depth; }
    }
    if (!e) {
        q->flags=R3D_TEXTURED|R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_COPY;
        for (unsigned i=0;i<3;i++) { q->vertices[i].z=49152; q->vertices[i].color=0xffff0000u; }
        e=execute(d,q,r);
        if (!e) d->failed_phase=7;
        if (!e && ((((uint32_t *)r[0]->shadow)[16*64+16]&0xffffffu)!=0x4080c0u ||
            (((uint32_t *)r[3]->shadow)[16*64+16]&0xffffffu)!=0x4080c0u)) e=-EIO;
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
                uint32_t ch=(pixel>>(i*8))&255;
                if (ch+2<channels[i] || ch>channels[i]+2) { d->expected=0x00291722; d->actual=pixel; e=-EIO; }
            }
        }
    }
    if (!e) for (unsigned i=0;i<64*64;i++)
        if ((((uint32_t *)r[0]->shadow)[i]&0xffffffu)!=(((uint32_t *)r[3]->shadow)[i]&0xffffffu)) { e=-EIO; break; }
    if (!e) e=idle(d);
    d->probing=false;
    if (e) quarantine(d);
    else { d->qualified=true; d->failed_phase=0; }
    leave(d); return e;
}
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
static int fullsize_compare(struct radeon3d_device *d,struct render3d_resource *const hw[R3D_REFS],
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
static int precision_probe(struct radeon3d_device *d,struct render3d_request *q,
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
        e=validate(d,q,hw); if (e) break;
        e=enter(d); if (e) break;
        uint64_t start=d->engine->io.now(d->engine->io.ctx);
        e=execute(d,q,hw);
        d->fullsize_us+=d->engine->io.now(d->engine->io.ctx)-start;
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
                    d->expected=((uint32_t *)ref[0]->shadow)[at]; d->actual=a[at]; e=-EILSEQ; break;
                }
                d->precision_samples++;
                if (phase==6) {
                    unsigned rr=a[at]>>16&255,g=a[at]>>8&255;
                    levels[0][rr/32]|=1u<<(rr%32); levels[1][g/32]|=1u<<(g%32);
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
int radeon3d_fullsize_probe(struct radeon3d_device *d,struct render3d_service *reference)
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
        fullsize_scene(q,d->mode_width,d->mode_height,phase);
        e=validate(d,q,hw); if (e) break;
        struct render3d_job job={0};
        while (!render3d_scalar_slice(q,ref,&job,&reference->host)) {}
        e=enter(d); if (e) break;
        uint64_t start=d->engine->io.now(d->engine->io.ctx);
        e=execute(d,q,hw);
        d->fullsize_us+=d->engine->io.now(d->engine->io.ctx)-start;
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
    if (!e) d->failed_phase=0;
    else if (!d->precision_error) d->precision_error=e;
    return e;
}
