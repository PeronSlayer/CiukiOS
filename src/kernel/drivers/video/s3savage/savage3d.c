/* Independent implementation from pinned sources cited in the F7 contract.
 * No upstream code is vendored. See the pinned-source amendment for registers.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/mm.h>
#include <ciuki/display_driver.h>
#include "savage3d.h"
uint32_t savage3d_fixed_ieee(uint32_t value, unsigned fractional)
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
int savage3d_triangle_packet(const struct render3d_vertex v[3], uint32_t words[22])
{
    if (!v || !words) return -EINVAL;
    for (unsigned i=0;i<3;i++) {
        if (v[i].x<0 || v[i].y<0 || v[i].x>(1024<<16) || v[i].y>(1024<<16) || v[i].z>65535 ||
            v[i].rhw<256 || v[i].rhw>16777216 || v[i].u<0 || v[i].u>65536 || v[i].v<0 || v[i].v>65536) return -EINVAL;
    }
    /* Linux savage_drv.h DRAW_PRIM/TRILIST, NO_CS|NO_UV1. Mesa
     * savageChooseVertexFormat_s4: X,Y,Z,W, diffuse, U0,V0, no vertex DMA. */
    words[0]=0x800300c8u;
    for (unsigned i=0;i<3;i++) {
        uint32_t *p=words+1+i*7;
        p[0]=savage3d_fixed_ieee((uint32_t)v[i].x,16);
        p[1]=savage3d_fixed_ieee((uint32_t)v[i].y,16);
        /* Canonical Z fixture, not an invented Savage viewport conversion. */
        p[2]=savage3d_fixed_ieee(v[i].z,16);
        p[3]=savage3d_fixed_ieee(v[i].rhw,16);
        p[4]=v[i].color;
        p[5]=savage3d_fixed_ieee((uint32_t)v[i].u,16);
        p[6]=savage3d_fixed_ieee((uint32_t)v[i].v,16);
    }
    return 0;
}
static unsigned texture_bpp(const struct render3d_resource *r)
{ return r->format==R3D_RGB565 || r->format==R3D_ARGB4444 ? 2 : 4; }
static uint32_t mip_size(unsigned w,unsigned h,unsigned bpp)
{
    if (w<=4 && h<=4) return 0;
    return (w<8 ? 8 : w)*(h<8 ? 8 : h)*bpp;
}
uint32_t savage3d_texture_bytes(const struct render3d_resource *r)
{
    if (render3d_texture_shape(r)) return 0;
    unsigned w=r->width,h=r->height,bpp=texture_bpp(r),total=0;
    for (unsigned i=0;i<r->levels;i++) {
        unsigned n=mip_size(w,h,bpp); total+=n;
        if (!n && i+1==r->levels) total+=64*bpp;
        w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
    }
    return (total+4095)&~4095u;
}
int savage3d_mip_offset(const struct render3d_resource *r,unsigned level,unsigned x,unsigned y,uint32_t *out)
{
    if (!out || render3d_texture_shape(r) || level>=r->levels) return -EINVAL;
    unsigned w=r->width,h=r->height,bpp=texture_bpp(r),at=0;
    for (unsigned i=0;i<level;i++) {
        at+=mip_size(w,h,bpp); w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
    }
    if (x>=w || y>=h) return -EINVAL;
    if (w<=4 && h<=4) {
        unsigned size=w>h ? w : h;
        *out=at+(size==1 ? 36 : size==2 ? 4 : 0)*bpp+(y*8+x)*bpp;
    } else {
        unsigned pw=w<8 ? 8 : w,ph=h<8 ? 8 : h,tw=bpp==2 ? 64 : 32;
        if (tw>pw) tw=pw;
        unsigned th=ph<16 ? ph : 16;
        *out=at+(y/th*(pw/tw)+x/tw)*tw*th*bpp+
            ((y%th/8)*(tw/8)+x%tw/8)*64*bpp+((y%8)*8+x%8)*bpp;
    }
    return *out<savage3d_texture_bytes(r) ? 0 : -EINVAL;
}
int savage3d_texture_offset(uint32_t w,uint32_t h,uint32_t x,uint32_t y,uint32_t *out)
{
    struct render3d_resource r={.kind=R3D_TEXTURE,.width=w,.height=h,.pitch=w*4,.bytes=w*h*4,.levels=1};
    return savage3d_mip_offset(&r,0,x,y,out);
}
int savage3d_pack_texture(void *dest,size_t bytes,const uint32_t *src,uint32_t w,uint32_t h)
{
    uint32_t offset;
    if (!dest || !src || w<32 || h<16 || savage3d_texture_offset(w,h,0,0,&offset) || bytes!=(uint64_t)w*h*4) return -EINVAL;
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        (void)savage3d_texture_offset(w,h,x,y,&offset);
        memcpy((uint8_t *)dest+offset,src+y*w+x,4);
    }
    return 0;
}
static bool overlaps(struct render3d_extent a, struct render3d_extent b)
{ return (uint64_t)a.first+a.bytes>b.first && (uint64_t)b.first+b.bytes>a.first; }
int savage3d_plan_vram(uint32_t vram, const struct render3d_extent *reserved, unsigned count,
                      uint32_t w, uint32_t h, struct savage3d_plan *out)
{
    if (!out || !reserved || !count || count>32 || vram<4096 || vram>16u*1024*1024 ||
        !w || !h || w>1024 || h>1024) return -EINVAL;
    struct render3d_extent allocations[36];
    for (unsigned i=0;i<count;i++) {
        if (!reserved[i].bytes || reserved[i].first>vram || reserved[i].bytes>vram-reserved[i].first) return -EINVAL;
        allocations[i]=reserved[i];
        for (unsigned j=0;j<i;j++) if (overlaps(allocations[i],allocations[j])) return -EINVAL;
    }
    struct savage3d_plan plan={0};
    plan.color_pitch=((w+31)&~31u)*4; plan.depth_pitch=((w+63)&~63u)*2;
    uint32_t height=(h+15)&~15u;
    struct render3d_extent *a[4]={&plan.color[0],&plan.color[1],&plan.depth,&plan.texture};
    const uint32_t sizes[4]={plan.color_pitch*height,plan.color_pitch*height,plan.depth_pitch*height,4u*1024*1024};
    for (unsigned n=0;n<4;n++) {
        uint32_t size=(sizes[n]+4095)&~4095u;
        if (size>vram) return -ENOMEM;
        bool found=false;
        for (uint32_t at=0;at<=vram-size;at+=4096) {
            struct render3d_extent candidate={at,size}; bool hit=false;
            for (unsigned i=0;i<count+n;i++) if (overlaps(candidate,allocations[i])) { hit=true; break; }
            if (!hit) { *a[n]=candidate; allocations[count+n]=candidate; found=true; break; }
        }
        if (!found) return -ENOMEM;
    }
    uint32_t run=0;
    for (uint32_t at=0;at+4096<=vram;at+=4096) {
        bool used=false; struct render3d_extent page={at,4096};
        for (unsigned i=0;i<count+4;i++) if (overlaps(page,allocations[i])) { used=true; break; }
        if (used) run=0;
        else { run+=4096; plan.free_bytes+=4096; if (run>plan.largest_bytes) plan.largest_bytes=run; }
    }
    *out=plan; return 0;
}

/* Correctly rounded positive rational <= 1, no floating point. The largest
 * remainder is <2^24; the 24-bit mantissa never needs a wide division. */
static uint32_t normalized_depth_ieee(uint32_t numerator,uint32_t maximum)
{
    if (!numerator) return 0;
    uint32_t n=numerator, exponent=127;
    while (n<maximum) { n*=2; exponent--; }
    uint32_t mantissa=1, remainder=n-maximum;
    for (unsigned i=0;i<23;i++) {
        remainder*=2; mantissa<<=1;
        if (remainder>=maximum) { remainder-=maximum; mantissa++; }
    }
    if (remainder*2>maximum || (remainder*2==maximum && (mantissa&1))) mantissa++;
    if (mantissa==0x1000000) { exponent++; mantissa>>=1; }
    return (exponent<<23)|(mantissa&0x7fffff);
}
int savage3d_hw_triangle_packet(const struct render3d_vertex v[3], uint32_t words[22])
{
    int e=savage3d_triangle_packet(v,words);
    if (!e) for (unsigned i=0;i<3;i++) {
        /* Mesa savagecontext.h/savageCalcViewport: hardware sample bias. */
        int32_t x=v[i].x-32768,y=v[i].y-24576;
        words[1+i*7]=savage3d_fixed_ieee((uint32_t)(x<0 ? -x : x),16)|(x<0 ? 0x80000000u : 0);
        words[2+i*7]=savage3d_fixed_ieee((uint32_t)(y<0 ? -y : y),16)|(y<0 ? 0x80000000u : 0);
        words[3+i*7]=normalized_depth_ieee(65535-v[i].z,65535);
    }
    return e;
}

int savage3d_mode_heap(const struct savage2d *s, const struct fb_device *fb,
                      uint32_t vram, struct render3d_extent *heap, uint32_t *required)
{
    if (!s || !fb || !heap || !required || !fb->width || !fb->height ||
        fb->width>1024 || fb->height>1024 || fb->bpp!=32 || !fb->pitch ||
        s->chip!=SAVAGE_SUPER || s->rows<s->height || s->rows>2048 ||
        vram<4096 || vram>16u*1024*1024 || !s->cursor_offset || s->cursor_offset>vram-4096u)
        return -EOPNOTSUPP;
    uint64_t first=(uint64_t)s->rows*fb->pitch;
    if (first<fb->size) first=fb->size;
    first=(first+4095)&~4095ull;
    uint32_t end=s->cursor_offset&~4095u;
    if (first>=end) return -ENOMEM;
    uint32_t height=(fb->height+15)&~15u;
    uint32_t cp=((fb->width+31)&~31u)*4, dp=((fb->width+63)&~63u)*2;
    *required=2*((cp*height+4095)&~4095u)+((dp*height+4095)&~4095u)+
        SAVAGE3D_TEXTURE_MIN+4*8192u+24576u; /* guards + linear probe scratch */
    *heap=(struct render3d_extent){(uint32_t)first,end-(uint32_t)first};
    return heap->bytes<*required ? -ENOMEM : 0;
}

/* BCI selectors and MMIO addresses: Mesa 7.7 savage_3d_reg.h, RE046-B §10.
 * The latter starts local control at MM48584; selectors are NOT MMIO/4.
 * No palette/fog-table, stream, PLL, GBD, DMA or COB programming. */
static const uint8_t selectors[]={0x1e,0x20,0x21,0x22,0x23,0x24,0x25,0x27,
                                 0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38};
static const uint32_t masks[]={0xfe0003ffu,0xffffffffu,0xffffffffu,0xfffffffbu,
    0xfffffffbu,0xffffffffu,0xffffffffu,0xefffffffu,0xefffffffu,0x1fffffffu,
    0xc3ff7fa7u,0xfe003fffu,0xe03fffffu,0xffffffffu,0xffffffffu,0x3f3f3f3fu,0xcfffffffu};
struct saved_state { uint32_t state[17], tile, agd, bitmap[4]; };
static uint32_t mmio_read(struct savage3d_device *d,uint32_t reg)
{ return d->engine->io.read32(d->engine->io.ctx,reg); }
static int packet(struct savage3d_device *d,const uint32_t *words,unsigned n)
{
    if (d->engine->io.now_us(d->engine->io.ctx)-d->slice_started>=20000) return -ETIMEDOUT;
    int e=savage2d_render3d_packet(d->engine,words,n);
    if (!e) d->packets++;
    return e;
}
static int setreg(struct savage3d_device *d,unsigned selector,uint32_t value)
{
    uint32_t words[2]={0x96010000u|selector,value};
    return packet(d,words,2);
}
static int checked_mmio(struct savage3d_device *d,uint32_t reg,uint32_t value)
{
    d->engine->io.write32(d->engine->io.ctx,reg,value);
    d->engine->io.drain(d->engine->io.ctx);
    return mmio_read(d,reg)==value ? 0 : -EIO;
}
static uint32_t bitmap_read(struct savage3d_device *d,uint32_t reg)
{
    struct savage_io *io=&d->engine->io;
    return io->read16(io->ctx,reg)|((uint32_t)io->read16(io->ctx,reg+2)<<16);
}
static int save_state(struct savage3d_device *d,struct saved_state *saved)
{
    for (unsigned i=0;i<17;i++) {
        uint32_t value=mmio_read(d,0x4850cu+selectors[i]*4u);
        if (value==UINT32_MAX) return -EIO;
        saved->state[i]=value&masks[i];
    }
    saved->tile=mmio_read(d,0x48c40); saved->agd=mmio_read(d,0x850c);
    if (saved->tile==UINT32_MAX || saved->agd==UINT32_MAX) return -EIO;
    /* PBD/SBD are the only 2D descriptors this path modifies. Halfword
     * readback is mandatory for this bank (existing owner rule). */
    for (unsigned i=0;i<4;i++) saved->bitmap[i]=bitmap_read(d,0x8170+i*4);
    return 0;
}
static int restore_state(struct savage3d_device *d,const struct saved_state *saved)
{
    for (unsigned i=0;i<17;i++) {
        int e=setreg(d,selectors[i],saved->state[i]); if (e) return e;
    }
    uint32_t bitmap[5]={0x960400e2u,saved->bitmap[0],saved->bitmap[1],saved->bitmap[2],saved->bitmap[3]};
    int e=packet(d,bitmap,5); if (e) return e;
    for (unsigned i=0;i<4;i++) if (bitmap_read(d,0x8170+i*4)!=saved->bitmap[i]) return -EIO;
    e=checked_mmio(d,0x48c40,saved->tile);
    if (!e) e=checked_mmio(d,0x850c,saved->agd);
    if (!e) for (unsigned i=0;i<17;i++)
        if ((mmio_read(d,0x4850cu+selectors[i]*4u)&masks[i])!=saved->state[i]) return -EIO;
    return e;
}
static unsigned pixel_bytes(const struct render3d_resource *r)
{ return r->kind==R3D_D16 || r->kind==R3D_COLOR565 || r->kind==R3D_LINEAR565 ? 2u : 4u; }
static uint32_t padded_bytes(const struct render3d_resource *r)
{
    uint32_t bpp=pixel_bytes(r);
    if (r->kind==R3D_TEXTURE) return savage3d_texture_bytes(r);
    if (r->kind==R3D_LINEAR_COLOR || r->kind==R3D_LINEAR565) return (r->width*r->height*bpp+4095)&~4095u;
    uint32_t pitch=(r->width*bpp+127)&~127u;
    return (pitch*((r->height+15)&~15u)+4095)&~4095u;
}
static bool fullsize(const struct savage3d_device *d)
{ return d && (d->fullsize_qualified || d->fullsize_testing) && d->mode_width && d->mode_height; }
static bool dimensions(const struct savage3d_device *d,const struct render3d_resource *r)
{ return fullsize(d) ? r->width<=d->mode_width && r->height<=d->mode_height : r->width<=64 && r->height<=64; }
static int layout(void *device,struct render3d_resource *r)
{
    const struct savage3d_device *d=device;
    if (!r || !r->width || !r->height || (r->kind==R3D_TEXTURE ? (r->width>1024 || r->height>1024) : !dimensions(d,r)) ||
        r->kind<R3D_COLOR || r->kind>R3D_LINEAR565) return -EOPNOTSUPP;
    if ((r->kind==R3D_COLOR565 || r->kind==R3D_LINEAR565) && (!d || (!d->color565_testing && !d->color565_qualified))) return -EOPNOTSUPP;
    if (r->kind==R3D_D24S8 && (!d || (!d->depth24_testing && !d->depth24_qualified))) return -EOPNOTSUPP;
    if (r->kind==R3D_TEXTURE && render3d_texture_shape(r)) return -EINVAL;
    r->reserved=padded_bytes(r)+8192;
    return 0;
}
static bool extent(const struct savage3d_device *d,const struct render3d_resource *r)
{
    uint32_t pool=d->heap.first+d->heap.bytes-(d->texture_bytes ? d->texture_bytes : SAVAGE3D_TEXTURE_MIN);
    return r && r->kind>=R3D_COLOR && r->kind<=R3D_LINEAR565 &&
        r->hardware && r->shadow && r->width && r->height && (r->kind==R3D_TEXTURE ? !render3d_texture_shape(r) : dimensions(d,r)) &&
        r->offset>=d->heap.first && !(r->offset&4095u) &&
        (uint64_t)r->offset+r->reserved<=(uint64_t)d->heap.first+d->heap.bytes &&
        (r->kind==R3D_TEXTURE ? r->offset>=pool : (uint64_t)r->offset+r->reserved<=pool) &&
        r->reserved==padded_bytes(r)+8192 && r->pitch==r->width*(r->kind==R3D_D16 ? 2u : 4u) &&
        (r->kind==R3D_TEXTURE || r->bytes==r->pitch*r->height);
}
static uint32_t signed_ieee(int32_t v)
{ return savage3d_fixed_ieee((uint32_t)(v<0 ? -v : v),16)|(v<0 ? 0x80000000u : 0); }
static int draw_packet(const struct render3d_request *q,uint32_t zmax,unsigned first,uint32_t out[28],unsigned *count)
{
    struct render3d_vertex v[3]; memcpy(v,q->vertices+first,sizeof(v));
    for (unsigned i=0;i<3;i++) {
        if (v[i].u < -16*65536 || v[i].u>16*65536 || v[i].v < -16*65536 || v[i].v>16*65536 ||
            q->uv1[first+i][0] < -16*65536 || q->uv1[first+i][0]>16*65536 ||
            q->uv1[first+i][1] < -16*65536 || q->uv1[first+i][1]>16*65536) return -EINVAL;
        v[i].u=v[i].v=0;
        if (v[i].z>zmax) return -EINVAL;
        v[i].z=0; /* The legacy serializer validates only D16; replace Z below. */
        if (q->state.flat) v[i].color=q->flat_color[first/3];
    }
    uint32_t minimum[22]; int e=savage3d_hw_triangle_packet(v,minimum); if (e) return e;
    unsigned stride=q->flags&R3D_TEXTURE1 ? 9 : 7;
    out[0]=stride==9 ? 0x80030008u : 0x800300c8u;
    for (unsigned i=0;i<3;i++) {
        memcpy(out+1+i*stride,minimum+1+i*7,5*4);
        out[3+i*stride]=normalized_depth_ieee(zmax-q->vertices[first+i].z,zmax);
        out[1+i*stride+5]=signed_ieee(q->vertices[first+i].u);
        out[1+i*stride+6]=signed_ieee(q->vertices[first+i].v);
        if (stride==9) {
            out[1+i*stride+7]=signed_ieee(q->uv1[first+i][0]);
            out[1+i*stride+8]=signed_ieee(q->uv1[first+i][1]);
        }
    }
    *count=1+3*stride; return 0;
}
/* Same sample/LOD proof as the GL encoder, evaluated on native packet inputs.
 * Work in Q4 screen units: qualified coordinates are multiples of 1/16 pixel.
 * At <=1024 pixels, W<=256 and |UV|<=16 all products fit signed 64 bits. */
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
    if (!area) return true; /* No fragments or LOD samples. */
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
    struct savage3d_device *d=device;
    if (!d || !d->engine || !d->heap.first || !d->heap.bytes ||
        d->heap.first>UINT32_MAX-d->heap.bytes || (d->heap.first&4095u) || (d->heap.bytes&4095u) ||
        (uint64_t)d->heap.first+d->heap.bytes>16u*1024*1024 || d->heap.bytes<=SAVAGE3D_TEXTURE_MIN ||
        (d->texture_bytes && ((d->texture_bytes&4095u) || d->texture_bytes<SAVAGE3D_TEXTURE_MIN || d->texture_bytes>=d->heap.bytes)) ||
        !q || !r || !q->flags || (q->flags&~R3D_ALL_FEATURES) ||
        render3d_validate_state(q) || q->count>(fullsize(d) ? R3D_VERTICES : 3u) || q->count%3 || !extent(d,r[0]) || (r[0]->kind!=R3D_COLOR && r[0]->kind!=R3D_COLOR565)) return -EINVAL;
    for (unsigned i=0;i<R3D_REFS;i++) if (r[i]) {
        if (!extent(d,r[i])) return -EINVAL;
        for (unsigned j=0;j<i;j++) if (r[j] && r[i]!=r[j] && overlaps((struct render3d_extent){r[i]->offset,r[i]->reserved},
            (struct render3d_extent){r[j]->offset,r[j]->reserved})) return -EINVAL;
    }
    if ((q->flags&R3D_DEPTH_FLAGS) &&
        (!r[1] || (r[1]->kind!=R3D_D16 && r[1]->kind!=R3D_D24S8) || r[1]->width!=r[0]->width || r[1]->height!=r[0]->height)) return -EINVAL;
    if ((q->flags&R3D_TEXTURED) && (!r[2] || render3d_texture_shape(r[2]))) return -EINVAL;
    if ((q->flags&R3D_TEXTURE1) && (!(q->flags&R3D_TEXTURED) || !r[4] || render3d_texture_shape(r[4]))) return -EINVAL;
    if (!(q->flags&R3D_DEPTH_FLAGS) && r[1]) return -EINVAL;
    if (!(q->flags&R3D_TEXTURED) && r[2]) return -EINVAL;
    if (!(q->flags&R3D_COPY) && r[3]) return -EINVAL;
    if (!(q->flags&R3D_TEXTURE1) && r[4]) return -EINVAL;
    if (r[0]->kind==R3D_COLOR565 && ((!d->color565_testing && !d->color565_qualified) ||
        (r[1] && r[1]->kind!=R3D_D16))) return -EOPNOTSUPP;
    uint32_t zmax=r[1] && r[1]->kind==R3D_D24S8 ? 16777215u : 65535u;
    if (q->clear_depth>zmax || (r[1] && r[1]->kind==R3D_D24S8 &&
        !d->depth24_testing && !d->depth24_qualified) ||
        ((q->flags&(R3D_STENCIL_TEST|R3D_CLEAR_STENCIL)) && (!r[1] || r[1]->kind!=R3D_D24S8))) return -EOPNOTSUPP;
    for (unsigned stage=0;stage<2;stage++) if (q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED)) {
        const struct render3d_resource *t=r[stage ? 4 : 2];
        unsigned limit=d->perspective_qualified || d->fullsize_testing ? 256 : 64;
        if (t->width>limit || t->height>limit) return -EOPNOTSUPP;
        /* DECAL of alpha-only textures has no GL-defined hardware meaning. */
        if (q->state.sampler[stage].combine==R3D_DECAL && t->format==R3D_A8) return -EOPNOTSUPP;
    }
    if ((q->flags&R3D_COPY) && (!r[3] || (r[0]->kind==R3D_COLOR565 ? r[3]->kind!=R3D_LINEAR565 :
        (r[3]->kind!=R3D_LINEAR_COLOR && r[3]->kind!=R3D_COLOR)) ||
        r[3]->width!=r[0]->width || r[3]->height!=r[0]->height)) return -EINVAL;
    if ((q->flags&R3D_COPY) && r[3]->kind==R3D_COLOR) return -EOPNOTSUPP;
    for (unsigned i=0;i<q->count;i++) if (q->vertices[i].x<0 || q->vertices[i].y<0 ||
        q->vertices[i].x>(int32_t)(r[0]->width<<16) || q->vertices[i].y>(int32_t)(r[0]->height<<16)) return -EINVAL;
    if (!d->fullsize_testing && !d->perspective_qualified && q->state.blend &&
        (q->state.source>R3D_INV_SRC_ALPHA || q->state.destination>R3D_INV_SRC_ALPHA)) return -EOPNOTSUPP;
    if (d->fullsize_qualified && !d->fullsize_testing && (q->flags&R3D_TEXTURED))
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
    uint32_t words[28]; unsigned count;
    for (unsigned first=0;first<q->count;first+=3) {
        int e=draw_packet(q,zmax,first,words,&count); if (e) return e;
    }
    return 0;
}
static uint32_t base(const struct render3d_resource *r) { return r->offset+4096; }
static uint32_t memory_read(struct savage3d_device *d,bool alias,uint32_t off,unsigned bytes)
{ return d->memory.read(d->memory.ctx,alias,off,bytes); }
static void memory_write(struct savage3d_device *d,bool alias,uint32_t off,uint32_t value,unsigned bytes)
{ d->memory.write(d->memory.ctx,alias,off,value,bytes); }
static int tile(struct savage3d_device *d,const struct render3d_resource *r)
{
    unsigned depth=pixel_bytes(r)==2;
    unsigned tiles=(r->width+(depth ? 63 : 31))>>(depth ? 6 : 5);
    return checked_mmio(d,0x48c40,(depth ? 0x80000000u : 0xc0000000u)|(tiles<<20)|(base(r)>>6));
}
static int guards(struct savage3d_device *d,struct render3d_resource *r,bool initialize)
{
    uint64_t start=0;
    for (unsigned side=0;side<2;side++) for (unsigned i=0;i<4096;i+=4) {
        if (!(i%512)) start=d->engine->io.now_us(d->engine->io.ctx);
        uint32_t off=r->offset+(side ? r->reserved-4096 : 0)+i;
        if (initialize) memory_write(d,false,off,0xa55ac33cu,4);
        else {
            uint32_t value=memory_read(d,false,off,4);
            if (value!=0xa55ac33cu) { d->expected=0xa55ac33cu; d->actual=value; d->guard_offset=off; return -EIO; }
            if (i%512==508 && d->engine->io.now_us(d->engine->io.ctx)-start>=1000) return -ETIMEDOUT;
        }
    }
    return 0;
}
static int upload(struct savage3d_device *d,struct render3d_resource *r)
{
    if (r->resident) return guards(d,r,false);
    guards(d,r,true);
    if (r->kind==R3D_COLOR || r->kind==R3D_D16 || r->kind==R3D_D24S8 || r->kind==R3D_COLOR565) {
        int e=tile(d,r); if (e) return e;
        unsigned bytes=pixel_bytes(r), pitch=bytes==2 ? 4096 : 8192;
        for (unsigned y=0;y<r->height;y++) for (unsigned x=0;x<r->width;x++) {
            uint32_t value=0; memcpy(&value,(uint8_t *)r->shadow+y*r->pitch+x*(r->kind==R3D_D16 ? 2 : 4),r->kind==R3D_D16 ? 2 : 4);
            if (r->kind==R3D_D16) value=65535-value;
            else if (r->kind==R3D_COLOR565) value=render3d_pack565(value);
            else if (r->kind==R3D_D24S8) value^=0xffffffu;
            memory_write(d,true,y*pitch+x*bytes,value,bytes);
        }
    } else if (r->kind==R3D_TEXTURE) {
        unsigned w=r->width,h=r->height,at=0,bpp=texture_bpp(r);
        for (unsigned level=0;level<r->levels;level++) {
            bool tail=w<=4 && h<=4;
            unsigned pw=tail ? 4 : w<8 ? 8 : w,ph=tail ? 4 : h<8 ? 8 : h;
            uint32_t first; (void)savage3d_mip_offset(r,level,0,0,&first);
            for (unsigned y=0;y<ph;y++) for (unsigned x=0;x<pw;x++) {
                uint32_t off,value=((uint32_t *)((uint8_t *)r->shadow+r->level_offset[level]))[(y%h)*w+x%w];
                if (tail) off=first+(y*8+x)*bpp;
                else {
                    /* Offset formula with the padded dimensions for repeats. */
                    unsigned tw=bpp==2 ? 64 : 32; if (tw>pw) tw=pw;
                    unsigned th=ph<16 ? ph : 16;
                    off=at+(y/th*(pw/tw)+x/tw)*tw*th*bpp+
                        ((y%th/8)*(tw/8)+x%tw/8)*64*bpp+((y%8)*8+x%8)*bpp;
                }
                if (r->format==R3D_RGB565) value=((value>>8)&0xf800)|((value>>5)&0x7e0)|((value>>3)&31);
                else if (r->format==R3D_ARGB4444) value=((value>>16)&0xf000)|((value>>12)&0xf00)|((value>>8)&0xf0)|((value>>4)&15);
                memory_write(d,false,base(r)+off,value,bpp);
            }
            at+=mip_size(w,h,bpp); w=w>1 ? w/2 : 1; h=h>1 ? h/2 : 1;
        }
    } else for (unsigned y=0;y<r->height;y++) for (unsigned x=0;x<r->width;x++) {
        uint32_t off=y*r->pitch+x*4,value;
        memcpy(&value,(uint8_t *)r->shadow+off,4);
        if (r->kind==R3D_TEXTURE) (void)savage3d_texture_offset(r->width,r->height,x,y,&off);
        if (r->kind==R3D_LINEAR565) { off=y*r->width*2+x*2; value=render3d_pack565(value); }
        memory_write(d,false,base(r)+off,value,pixel_bytes(r));
    }
    d->engine->io.drain(d->engine->io.ctx); r->resident=true; return 0;
}
static uint32_t descriptor(const struct render3d_resource *r,bool tiled)
{
    unsigned bytes=pixel_bytes(r);
    uint32_t stride=tiled ? ((r->width*bytes+127)&~127u)/bytes : r->width;
    /* SuperSavage TILE_DESTINATION=1 (not Savage4 TILE_16/32=2/3). */
    return 0x10000000u|(tiled ? 0x01000000u : 0)|(bytes*8u<<16)|stride;
}
static int clear_resource(struct savage3d_device *d,struct render3d_resource *r,uint32_t value)
{
    if (r->kind==R3D_COLOR565) value=render3d_pack565(value);
    if (r->kind==R3D_D16) value=65535-value;
    /* RECT + XP/YP, ROP copy, SEND_COLOR, DEST_PBD_NEW. Linux clear and
     * X.Org SAVAGESelectBuffer supply the inline PBD and destination layout. */
    uint32_t words[]={0x4bcc8c00u,base(r),descriptor(r,true),value,0,(r->height<<16)|r->width};
    return packet(d,words,6);
}
/* Partial D24/S8 clears preserve the other attachment and masked bits.
 * Preserve the actual physical component after fenced idle, including boot
 * rows omitted from sampled shadow readback. Alias transfers are chunk bounded
 * just like readback, under the existing owner. */
static int clear_depth_stencil(struct savage3d_device *d,const struct render3d_request *q,
                              struct render3d_resource *r)
{
    if (r->kind==R3D_D16) return clear_resource(d,r,q->clear_depth);
    if ((q->flags&(R3D_CLEAR_DEPTH|R3D_CLEAR_STENCIL))==(R3D_CLEAR_DEPTH|R3D_CLEAR_STENCIL) &&
        q->stencil.write_mask==255) return clear_resource(d,r,(q->stencil.clear<<24)|(q->clear_depth^0xffffffu));
    int e=tile(d,r); if (e) return e;
    for (unsigned y=0;y<r->height;y++) for (unsigned x=0;x<r->width;x++) {
        if (!(x%128)) d->slice_started=d->engine->io.now_us(d->engine->io.ctx);
        uint32_t value=memory_read(d,true,y*8192+x*4,4)^0xffffffu;
        if (q->flags&R3D_CLEAR_DEPTH) value=(value&0xff000000u)|q->clear_depth;
        if (q->flags&R3D_CLEAR_STENCIL) {
            unsigned mask=q->stencil.write_mask<<24;
            value=(value&~mask)|((q->stencil.clear<<24)&mask);
        }
        memory_write(d,true,y*8192+x*4,value^0xffffffu,4);
        if ((x%128==127 || x+1==r->width) && d->engine->io.now_us(d->engine->io.ctx)-d->slice_started>=1000) return -ETIMEDOUT;
    }
    d->engine->io.drain(d->engine->io.ctx);
    d->slice_started=d->engine->io.now_us(d->engine->io.ctx);
    return 0;
}
static int state(struct savage3d_device *d,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    static const uint8_t depth_func[]={4,0,7,2,6,1,5,3},alpha_func[]={1,0,7,2,3,4,5,6},factors[]={1,0,4,5,2,2,3,3,6,7};
    uint32_t ctrl[2]={0x5f040,0x5f040},blend[2]={0x00850405,0x00870407},desc=0x60000000;
    for (unsigned stage=0;stage<2;stage++) if (q->flags&(stage ? R3D_TEXTURE1 : R3D_TEXTURED)) {
        const struct render3d_resource *t=r[stage ? 4 : 2];
        const struct render3d_sampler *s=&q->state.sampler[stage];
        unsigned tw=0,th=0;
        while ((1u<<tw)<t->width) tw++;
        while ((1u<<th)<t->height) th++;
        unsigned format=t->format==R3D_RGB565 ? 6 : t->format==R3D_ARGB4444 ? 5 : 3;
        desc|=(tw|(th<<4)|(format<<8))<<(stage*12);
        desc|=1u<<(25+stage);
        if (stage) desc|=1u<<24; /* Mesa texBLoopEn: feed stage 0 to stage 1. */
        unsigned filter=s->filter==R3D_NEAREST || s->filter==R3D_NEAREST_MIP ? 0 : s->filter==R3D_TRILINEAR ? 3 : 1;
        bool mips=s->filter>=R3D_NEAREST_MIP;
        /* Mesa savage4_set_filter_mode + SAVAGE4_LOD_OFFSET=10 for
         * mipmapped sampling; retain the pinned minimum's bias 8 otherwise. */
        ctrl[stage]=(mips ? 10u<<3 : 0x40u)|filter|(mips ? 4u : 0u)|((mips ? t->levels-1 : 15u)<<12)|
            (s->repeat_u ? 0u : 1u<<16)|(s->repeat_v ? 0u : 1u<<18);
        bool rgb=t->format==R3D_RGB565 || t->format==R3D_L8,alpha=t->format==R3D_A8;
        /* RE046-B MM485A0: stage 1 preserves CURRENT, not diffuse (or
         * the expanded white RGB of an alpha-only texture). */
        if (s->combine==R3D_REPLACE) blend[stage]=alpha ? (stage ? 0x00900403 : 0x00900405) :
            rgb ? (stage ? 0x00870410 : 0x00850410) : 0x00840410;
        else if (s->combine==R3D_DECAL) blend[stage]=rgb ? (stage ? 0x00870410 : 0x00850410) : (stage ? 0x00832a00 : 0x00852a04);
        else if (s->combine==R3D_ADD && !alpha) blend[stage]=stage ? 0x18130c13 : 0x18110c11;
        else blend[stage]=stage ? 0x00130013 : 0x00110011;
    }
    uint32_t local=0xc4000000u|((q->flags&R3D_DEPTH_WRITE) ? 0x08000000u : 0)|
        (q->state.flat ? 1u<<28 : 0)|(q->state.alpha_test ? 1u<<25 : 0)|
        (q->state.blend ? (uint32_t)(factors[q->state.source]<<4)|factors[q->state.destination] : 0x10u);
    const struct ciuki_r3d_stencil *st=&q->stencil;
    uint32_t stencil=0x000ffff7u;
    if (q->flags&R3D_STENCIL_TEST) stencil=alpha_func[st->function]|8u|
        (st->read_mask<<4)|(st->write_mask<<12)|(st->fail<<20)|(st->zfail<<23)|(st->zpass<<26);
    bool d24=r[1] && r[1]->kind==R3D_D24S8;
    uint32_t values[]={local,
        ctrl[0],ctrl[1],r[2] ? base(r[2])|2u : base(r[0])|2u,
        r[4] ? base(r[4])|2u : base(r[0])|2u,blend[0],blend[1],desc,
        0,stencil,0x80000000u|((q->flags&(R3D_DEPTH_TEST|R3D_DEPTH_WRITE|R3D_STENCIL_TEST)) ? 0x20u : 0u)|
            ((q->flags&R3D_STENCIL_TEST) ? st->reference<<16 : 0)|
            ((q->flags&R3D_DEPTH_TEST) ? depth_func[q->state.depth_func] : 7u),
        r[1] ? (base(r[1])>>11)|(((r[1]->width+(d24 ? 31 : 63))>>(d24 ? 5 : 6))<<25)|(d24 ? 0x80000000u : 0) : 0,
        (r[0]->kind==R3D_COLOR565 ? 0x80000000u : 0x80000080u)|((base(r[0])>>11)<<8)|
            ((r[0]->width+(r[0]->kind==R3D_COLOR565 ? 63 : 31))>>(r[0]->kind==R3D_COLOR565 ? 6 : 5)),
        0x800u|(q->state.alpha_ref<<24),(1u<<26)|(r[0]->width-1)|((r[0]->height-1)<<12)|
        (q->state.alpha_test ? 0x80000000u|((uint32_t)alpha_func[q->state.alpha_func]<<28) : 0),0x1818u,0x4f000000u};
    for (unsigned i=0;i<17;i++) { int e=setreg(d,selectors[i],values[i]); if (e) return e; }
    return 0;
}
static int read_resource(struct savage3d_device *d,struct render3d_resource *r)
{
    if (r->kind==R3D_TEXTURE) return guards(d,r,false);
    bool alias=r->kind!=R3D_LINEAR_COLOR && r->kind!=R3D_LINEAR565;
    int e=alias ? tile(d,r) : 0; if (e) return e;
    unsigned bytes=pixel_bytes(r), pitch=alias ? (bytes==2 ? 4096 : 8192) : r->width*bytes;
    for (unsigned y=0;y<r->height;y++) {
        /* Boot qualification samples tile rows plus the final row; ordinary
         * jobs must publish complete canonical attachments. */
        if (d->fullsize_testing && y%16 && y!=r->height-1) continue;
        for (unsigned first=0;first<r->width;first+=128) {
            uint64_t start=d->engine->io.now_us(d->engine->io.ctx);
            unsigned end=first+128; if (end>r->width) end=r->width;
            for (unsigned x=first;x<end;x++) {
                uint32_t value=memory_read(d,alias,(alias ? 0 : base(r))+y*pitch+x*bytes,bytes);
                if (r->kind==R3D_D16) value=65535-(value&65535);
                else if (r->kind==R3D_COLOR565 || r->kind==R3D_LINEAR565) value=render3d_expand565(value);
                else if (r->kind==R3D_D24S8) value^=0xffffffu;
                memcpy((uint8_t *)r->shadow+y*r->pitch+x*(r->kind==R3D_D16 ? 2 : 4),&value,r->kind==R3D_D16 ? 2 : 4);
            }
            if (d->engine->io.now_us(d->engine->io.ctx)-start>=1000) return -ETIMEDOUT;
        }
    }
    return guards(d,r,false);
}
static void quarantine(void *device);
static int execute(void *device,const struct render3d_request *q,struct render3d_resource *const r[R3D_REFS])
{
    struct savage3d_device *d=device;
    int e=validate(d,q,r); if (e) return e; /* no write on invalid extents */
    if (!d->locked || (!d->qualified && !d->probing) || d->lost) return -ENODEV;
    d->slice_started=d->engine->io.now_us(d->engine->io.ctx);
    struct saved_state saved;
    d->failed_phase=1; e=save_state(d,&saved); if (e) return e;
    d->failed_phase=2; e=checked_mmio(d,0x850c,saved.agd|0x8000u);
    for (unsigned i=0;!e && i<R3D_REFS;i++) if (r[i]) e=upload(d,r[i]);
    /* CPU transfers are separate bounded work from the engine's unchanged
     * execution window. No command has been submitted by upload(). */
    d->slice_started=d->engine->io.now_us(d->engine->io.ctx);
    if (!e) d->failed_phase=3;
    if (!e && (q->flags&R3D_CLEAR_COLOR)) e=clear_resource(d,r[0],q->clear_color);
    if (!e && (q->flags&(R3D_CLEAR_DEPTH|R3D_CLEAR_STENCIL))) e=clear_depth_stencil(d,q,r[1]);
    if (!e) d->failed_phase=4;
    if (!e && q->count) {
        e=state(d,q,r);
        unsigned total=0;
        for (unsigned first=0;!e && first<q->count;first+=3) {
            unsigned count=0;
            e=draw_packet(q,r[1] && r[1]->kind==R3D_D24S8 ? 16777215u : 65535u,first,d->commands+total,&count); total+=count;
        }
        if (!e && q->count<=3) e=packet(d,d->commands,total);
        else if (!e) {
            if (d->engine->io.now_us(d->engine->io.ctx)-d->slice_started>=20000) e=-ETIMEDOUT;
            else e=savage2d_render3d_batch(d->engine,d->commands,total);
            if (!e) d->packets++;
        }
        if (!e) d->triangles+=q->count/3;
    }
    if (!e) d->failed_phase=5;
    if (!e && (q->flags&R3D_COPY)) {
        /* Both inline descriptors avoid changing the retained linear GBD.
         * DRM swap uses PBD source; SBD_NEW gives an owned linear target. */
        uint32_t words[]={0x4bcc14c0u,base(r[3]),descriptor(r[3],false),
            base(r[0]),descriptor(r[0],true),0,0,(r[0]->height<<16)|r[0]->width};
        e=packet(d,words,8); if (!e) d->copies++;
    }
    if (!e) {
        d->failed_phase=8;
        for (unsigned i=0;i<R3D_REFS && !e;i++) if (r[i]) e=read_resource(d,r[i]);
    }
    /* A completed event permits restoration even after a CPU read timeout or
     * an expired work deadline. Never write again without that idle proof. */
    if ((!e || e==-ETIMEDOUT || d->failed_phase>=3) &&
        d->engine->cpu_safe && !d->engine->event_pending && !d->engine->quarantined) {
        uint32_t phase=d->failed_phase;
        d->slice_started=d->engine->io.now_us(d->engine->io.ctx);
        d->failed_phase=6;
        int restored=restore_state(d,&saved);
        if (restored) { quarantine(d); return restored; }
        d->failed_phase=phase;
    }
    if (!e) d->failed_phase=0;
    return e;
}
static bool qualified(void *device,uint32_t generation)
{
    struct savage3d_device *d=device;
    return d && d->qualified && !d->lost && d->generation==generation && d->engine && d->engine->chip==SAVAGE_SUPER &&
        d->engine->active && !d->engine->quarantined && d->engine->render3d_qualified;
}
static int enter(void *device)
{
    struct savage3d_device *d=device;
    if (!d || d->locked || d->lost || !d->memory.enter) return -EBUSY;
    int e=d->memory.enter(d->memory.ctx,d->generation);
    if (!e) d->locked=true;
    return e;
}
static void leave(void *device)
{
    struct savage3d_device *d=device;
    d->locked=false; d->memory.leave(d->memory.ctx);
}
static int idle(void *device)
{
    struct savage3d_device *d=device;
    return d->engine->cpu_safe && !d->engine->quarantined ? 0 : -EIO;
}
static void quarantine(void *device)
{
    struct savage3d_device *d=device;
    if (!d || d->lost) return;
    d->qualified=d->fullsize_qualified=d->perspective_qualified=false; d->lost=true;
    d->engine->render3d_qualified=false;
    d->engine->quarantined=true; d->engine->active=false;
    d->engine->reason="render3d_fault";
    if (d->engine->io.fault) d->engine->io.fault(d->engine->io.ctx);
    if (d->lost_notify) d->lost_notify(d->memory.ctx);
}
extern int savage3d_owner_attach(struct render3d_service *,const struct render3d_host *,uint32_t) __attribute__((weak));
static void detach(struct render3d_service *s,void *owner)
{
    struct savage3d_device *d=owner;
    if (d && d->consumer==s) d->consumer=0;
}
static void query_caps(void *owner,struct ciuki_r3d_caps *out)
{
    const struct savage3d_device *d=owner;
    *out=*savage3d_backend()->capabilities;
    if (d && d->color565_qualified && !d->lost) out->color_formats|=CIUKI_R3D_RGB565;
    if (d && d->depth24_qualified && !d->lost) out->depth_formats|=CIUKI_R3D_D24S8;
    else out->features&=~(R3D_STENCIL_TEST|R3D_CLEAR_STENCIL);
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
    const struct savage3d_device *d=owner;
    return d && d->texture_bytes ? d->texture_bytes : SAVAGE3D_TEXTURE_MIN;
}
const struct render3d_backend *savage3d_backend(void)
{
    static const struct ciuki_r3d_caps capabilities={
        .version=CIUKI_R3D_VERSION,.features=R3D_ALL_FEATURES,.max_size=64,
        .max_vertices=3,.texture_units=2,.max_texture_size=64,
        .depth_formats=CIUKI_R3D_D16,.color_formats=CIUKI_R3D_ARGB8888,
        .texture_formats=63,.depth_functions=255,.alpha_functions=255,
        .blend_factors=15,.combine_modes=15,.filters=31,.max_pixel_work=4096};
    static const struct render3d_backend backend={.features=R3D_ALL_FEATURES,.qualified=qualified,
        .layout=layout,.validate=validate,.enter=enter,.leave=leave,.execute=execute,
        .idle=idle,.quarantine=quarantine,.attach=savage3d_owner_attach,.max_size=64,.detach=detach,
        .texture_pool_bytes=texture_pool_bytes,.capabilities=&capabilities,.query_caps=query_caps,
        .resident_bytes=(sizeof(struct savage3d_device)+4095u)&~4095u};
    return &backend;
}

int savage3d_probe(struct savage3d_device *d)
{
    if (!d || !d->engine || !d->memory.read || !d->memory.write || !d->memory.enter ||
        !d->memory.leave || !d->generation || !d->heap.bytes || (d->heap.first&4095u) ||
        d->heap.first>UINT32_MAX-d->heap.bytes || d->heap.bytes<SAVAGE3D_TEXTURE_MIN+96*1024u ||
        !d->engine->active || d->engine->quarantined || d->engine->chip!=SAVAGE_SUPER) return -ENODEV;
    /* Small shadows are allocated by the owner before locks, not on the
     * kernel stack. Consumer is a scratch service with four typed resources. */
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
    /* A farther triangle must fail LESS without altering the previous pixel. */
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
                uint32_t c=(pixel>>(i*8))&255;
                if (c+2<channels[i] || c>channels[i]+2) { d->expected=0x00291722; d->actual=pixel; e=-EIO; }
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

/* Trusted boot-only scene. All storage is allocated by the owner before entry.
 * The scalar and packet engines share input, never rasterization or sampling. */
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
static int fullsize_compare(struct savage3d_device *d,struct render3d_resource *const hw[R3D_REFS],
                            struct render3d_resource *const ref[R3D_REFS])
{
    const uint32_t *a=hw[0]->shadow,*b=ref[0]->shadow,*copy=hw[3]->shadow;
    bool d24=hw[1]->kind==R3D_D24S8;
    const uint16_t *az=hw[1]->shadow,*bz=ref[1]->shadow;
    const uint32_t *az24=hw[1]->shadow,*bz24=ref[1]->shadow;
    uint32_t ah=2166136261u,bh=2166136261u;
    for (unsigned i=0;i<hw[0]->width*hw[0]->height;i++) {
        unsigned row=i/hw[0]->width;
        if (row%16 && row!=hw[0]->height-1) continue;
        uint32_t clear=hw[0]->kind==R3D_COLOR565 ? render3d_expand565(render3d_pack565(0xff102030u)) : 0xff102030u;
        if (a[i]!=copy[i] || (a[i]==clear)!=(b[i]==clear)) return -EILSEQ;
        for (unsigned shift=0;shift<32;shift+=8) {
            unsigned ac=a[i]>>shift&255,bc=b[i]>>shift&255,delta=ac>bc ? ac-bc : bc-ac;
            if (delta>d->max_channel_error) d->max_channel_error=delta;
            if (delta>2) { d->expected=b[i]; d->actual=a[i]; return -EILSEQ; }
        }
        unsigned za=d24 ? az24[i]&0xffffffu : az[i],zb=d24 ? bz24[i]&0xffffffu : bz[i];
        if (d24 && (az24[i]>>24)!=(bz24[i]>>24)) { d->expected=bz24[i]; d->actual=az24[i]; return -EILSEQ; }
        unsigned delta=za>zb ? za-zb : zb-za;
        if (delta>d->max_depth_error) d->max_depth_error=delta;
        if (delta>1) { d->expected=zb; d->actual=za; return -EILSEQ; }
        /* Sampled rows cover every column, tile boundaries and corners.
         * Raw hashes are evidence; tolerance comparisons above are normative. */
        ah=(ah^a[i])*16777619u; ah=(ah^(d24 ? az24[i] : az[i]))*16777619u;
        bh=(bh^b[i])*16777619u; bh=(bh^(d24 ? bz24[i] : bz[i]))*16777619u;
        d->sample_count++;
    }
    d->hardware_checksum=ah; d->reference_checksum=bh; return 0;
}
#include "savage3d_precision.h"
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
            /* Constant extreme coordinates isolate address overflow before
             * CLAMP_TO_EDGE; REPEAT alone can hide lost high address bits. */
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
static int precision_probe(struct savage3d_device *d,struct render3d_request *q,
    struct render3d_resource *const hardware[R3D_REFS],struct render3d_resource *const reference[R3D_REFS])
{
    d->sample_delta_q16=CIUKI_R3D_SAMPLE_DELTA_Q16;
    d->filter_fraction_bits=CIUKI_R3D_FILTER_FRACTION_BITS;
    int e=0;
    for (unsigned phase=0;phase<23 && !e;phase++) {
        struct render3d_resource *hw[R3D_REFS],*ref[R3D_REFS];
        for (unsigned i=0;i<R3D_REFS;i++) { hw[i]=hardware[i]; ref[i]=reference[i]; }
        /* Qualify 256-square addressing and mip selection in EACH unit while
         * reusing the same two allocations and their guard extents. */
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
                /* Full rectangle has no holes; D16/coverage/copy are separate
                 * from colour envelopes and never inherit their tolerance. */
                if (a[at]!=copy[at] ||
                    !precision_pixel(q,hw,x,y,a[at],az[at],d->sample_delta_q16,d->filter_fraction_bits)) {
                    d->expected=((uint32_t *)ref[0]->shadow)[at]; d->actual=a[at]; e=-EILSEQ; break;
                }
                d->precision_samples++;
                if (phase==6) { /* One-dimensional white/black transitions
                                * isolate the two units' filter fractions. */
                    unsigned r=a[at]>>16&255,g=a[at]>>8&255;
                    levels[0][r/32]|=1u<<(r%32); levels[1][g/32]|=1u<<(g%32);
                }
            }
        }
        if (phase==6 && !e) for (unsigned stage=0;stage<2;stage++) {
            unsigned count=0;
            for (unsigned i=0;i<8;i++) for (unsigned bit=0;bit<32;bit++) count+=(levels[stage][i]>>bit)&1;
            d->measured_filter_levels[stage]=count;
            /* Seven fractional bits produce at most 129 colours on a
             * one-dimensional black/white edge; eight produce >=255. */
            if (count<192) e=-EILSEQ;
        }
    }
    if (!e) d->precision_phase=0;
    return e;
}
int savage3d_fullsize_probe(struct savage3d_device *d,struct render3d_service *reference)
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
        uint64_t start=d->engine->io.now_us(d->engine->io.ctx);
        e=execute(d,q,hw);
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
        /* A restored numeric mismatch refuses perspective only. Integrity,
         * engine and restoration failures keep their existing containment. */
        if (d->lost || (d->precision_error && d->precision_error!=-EILSEQ && d->precision_error!=-ETIMEDOUT))
            e=d->precision_error;
    }
    if (!e) d->failed_phase=0;
    else if (!d->precision_error) d->precision_error=e;
    return e;
}

/* Separate current-mode D24/S8 gate; D16 remains qualified after a restored
 * numeric refusal. Callers allocate disjoint scratch before entering here. */
static int attachment_probe(struct savage3d_device *d,struct render3d_service *reference,bool depth24)
{
    if (!d || !d->qualified || !d->fullsize_qualified || d->lost || (depth24 ? d->depth24_qualified : d->color565_qualified) ||
        !d->consumer || !reference || !d->consumer->contexts[0].resources || !reference->contexts[0].resources) return -ENODEV;
    struct render3d_resource *hw[R3D_REFS],*ref[R3D_REFS];
    for (unsigned i=0;i<R3D_REFS;i++) {
        hw[i]=&d->consumer->contexts[0].resources[i]; ref[i]=&reference->contexts[0].resources[i];
        if (!hw[i]->shadow || !ref[i]->shadow || hw[i]->bytes!=ref[i]->bytes) return -EINVAL;
    }
    if ((depth24 ? hw[1]->kind!=R3D_D24S8 || ref[1]->kind!=R3D_D24S8 :
        hw[0]->kind!=R3D_COLOR565 || ref[0]->kind!=R3D_COLOR565) ||
        hw[0]->width!=d->mode_width || hw[0]->height!=d->mode_height) return -EINVAL;
    d->depth24_testing=depth24; d->color565_testing=!depth24; d->fullsize_testing=true;
    struct render3d_request *q=(void *)d->consumer->ring[0];
    int e=0;
    for (unsigned phase=0;phase<(depth24 ? 34u : 4u) && !e;phase++) {
        fullsize_scene(q,d->mode_width,d->mode_height,phase<4 ? phase : 0);
        if (depth24) {
            q->clear_depth=16777215;
            for (unsigned i=0;i<q->count;i++) q->vertices[i].z=q->vertices[i].z*256u;
        }
        if (phase>=4) {
            q->flags|=R3D_CLEAR_STENCIL|R3D_STENCIL_TEST;
            q->flags&=~(R3D_TEXTURED|R3D_TEXTURE1);
            /* Attachment semantics need no texture sampling. Cover a bounded
             * 64-square patch at every mode; clears/copy/guards still cover
             * the full mode, including its final row and tile boundaries. */
            for (unsigned i=0;i<q->count;i++) {
                q->vertices[i].x=q->vertices[i].x ? 63*65536 : 0;
                q->vertices[i].y=q->vertices[i].y ? 59*65536 : 0;
            }
            q->stencil=(struct ciuki_r3d_stencil){.function=R3D_ALWAYS,.reference=165,
                .read_mask=255,.write_mask=phase&1 ? 0x5a : 255,.clear=phase&1 ? 255 : 0};
            unsigned op=(phase-4)%6,branch=(phase-4)/6;
            q->stencil.fail=op; q->stencil.zfail=(op+1)%6; q->stencil.zpass=(op+2)%6;
            if (branch==1) q->stencil.function=R3D_NEVER;
            if (branch==2) q->state.depth_func=R3D_NEVER;
            if (phase>=22 && phase<30) {
                q->stencil.function=phase-22; q->stencil.read_mask=0x5a;
                q->stencil.reference=85; q->stencil.clear=85;
            }
            if (phase==30) q->flags&=~(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE); /* stencil only */
            if (phase==31) q->flags&=~R3D_CLEAR_STENCIL; /* depth-only clear preserves stencil */
            if (phase==32) { q->flags&=~R3D_STENCIL_TEST; q->state.alpha_test=1; q->state.alpha_func=R3D_NEVER; }
            if (phase==33) { q->state.alpha_test=1; q->state.alpha_func=R3D_NEVER; }
            /* Vary the D24 plane below D16 resolution, away from endpoints. */
            for (unsigned i=0;i<q->count;i++) {
                q->vertices[i].z=0x123450u+(i%3)*7;
                /* Non-binary edge slopes expose Q16 barycentric truncation
                 * at D24 precision; a D16-scaled fixture would miss it. */
                if (phase==4) q->vertices[i].z=0x100000u+
                    (q->vertices[i].x ? 0x500001u : 0)+(q->vertices[i].y ? 0x700003u : 0);
            }
        }
        struct render3d_resource *selected[R3D_REFS]; memcpy(selected,hw,sizeof(selected));
        if (!(q->flags&R3D_TEXTURED)) selected[2]=0;
        if (!(q->flags&R3D_TEXTURE1)) selected[4]=0;
        e=validate(d,q,selected); if (e) break;
        struct render3d_job job={0};
        while (!render3d_scalar_slice(q,ref,&job,&reference->host)) {}
        e=enter(d); if (e) break;
        e=execute(d,q,selected);
        if (e && (e!=-ETIMEDOUT || idle(d))) quarantine(d);
        leave(d);
        if (!e) e=fullsize_compare(d,hw,ref);
    }
    d->depth24_testing=d->color565_testing=d->fullsize_testing=false;
    if (depth24) d->depth24_qualified=!e; else d->color565_qualified=!e;
    return e;
}

int savage3d_depth24_probe(struct savage3d_device *d,struct render3d_service *r)
{ return attachment_probe(d,r,true); }
int savage3d_color565_probe(struct savage3d_device *d,struct render3d_service *r)
{ return attachment_probe(d,r,false); }
