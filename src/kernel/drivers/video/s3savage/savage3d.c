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
int savage3d_texture_offset(uint32_t w, uint32_t h, uint32_t x, uint32_t y, uint32_t *out)
{
    if (!out || w<32 || h<16 || w>1024 || h>1024 || (w&(w-1)) || (h&(h-1)) || x>=w || y>=h) return -EINVAL;
    /* Mesa 7.7 tileInfo_pro[32bit]: 32x16, 8x2 subtiles, 4x8
     * subtiles per 2048-byte tile, row-major inside each subtile. */
    *out=((y/16)*(w/32)+x/32)*2048+((y%16)/2*4+(x%32)/8)*64+(y%2)*32+(x%8)*4;
    return 0;
}
int savage3d_pack_texture(void *dest, size_t bytes, const uint32_t *src, uint32_t w, uint32_t h)
{
    uint32_t offset;
    if (!dest || !src || savage3d_texture_offset(w,h,0,0,&offset) || bytes!=(uint64_t)w*h*4) return -EINVAL;
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
 * remainder is <65535; the 24-bit mantissa never needs a wide division. */
static uint32_t depth_ieee(uint32_t numerator)
{
    if (!numerator) return 0;
    uint32_t n=numerator, exponent=127;
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
int savage3d_hw_triangle_packet(const struct render3d_vertex v[3], uint32_t words[22])
{
    int e=savage3d_triangle_packet(v,words);
    if (!e) for (unsigned i=0;i<3;i++) {
        /* Mesa savagecontext.h/savageCalcViewport: hardware sample bias. */
        int32_t x=v[i].x-32768,y=v[i].y-24576;
        words[1+i*7]=savage3d_fixed_ieee((uint32_t)(x<0 ? -x : x),16)|(x<0 ? 0x80000000u : 0);
        words[2+i*7]=savage3d_fixed_ieee((uint32_t)(y<0 ? -y : y),16)|(y<0 ? 0x80000000u : 0);
        words[3+i*7]=depth_ieee(65535-v[i].z);
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
static uint32_t padded_bytes(const struct render3d_resource *r)
{
    uint32_t bpp=r->kind==R3D_D16 ? 2 : 4;
    if (r->kind==R3D_TEXTURE || r->kind==R3D_LINEAR_COLOR) return (r->bytes+4095)&~4095u;
    uint32_t pitch=(r->width*bpp+127)&~127u;
    return (pitch*((r->height+15)&~15u)+4095)&~4095u;
}
static int layout(void *device,struct render3d_resource *r)
{
    (void)device;
    if (!r || !r->width || !r->height || r->width>64 || r->height>64 ||
        r->kind<R3D_COLOR || r->kind>R3D_LINEAR_COLOR) return -EOPNOTSUPP;
    if (r->kind==R3D_TEXTURE && (r->width<32 || r->height<16 ||
        (r->width&(r->width-1)) || (r->height&(r->height-1)))) return -EOPNOTSUPP;
    r->reserved=padded_bytes(r)+8192;
    return 0;
}
static bool extent(const struct savage3d_device *d,const struct render3d_resource *r)
{
    return r && r->hardware && r->shadow && r->width && r->height && r->width<=64 && r->height<=64 &&
        r->offset>=d->heap.first && !(r->offset&4095u) &&
        (uint64_t)r->offset+r->reserved<=(uint64_t)d->heap.first+d->heap.bytes &&
        r->reserved==padded_bytes(r)+8192 && r->pitch==r->width*(r->kind==R3D_D16 ? 2u : 4u) &&
        r->bytes==r->pitch*r->height;
}
static int validate(void *device,const struct render3d_request *q,struct render3d_resource *const r[4])
{
    struct savage3d_device *d=device;
    if (!d || !d->engine || !q || !r || !q->flags || (q->flags&~R3D_FEATURES) ||
        q->count>3 || q->count%3 || q->clear_depth>65535 || !extent(d,r[0]) || r[0]->kind!=R3D_COLOR) return -EINVAL;
    for (unsigned i=0;i<4;i++) if (r[i]) {
        if (!extent(d,r[i])) return -EINVAL;
        for (unsigned j=0;j<i;j++) if (r[j] && overlaps((struct render3d_extent){r[i]->offset,r[i]->reserved},
            (struct render3d_extent){r[j]->offset,r[j]->reserved})) return -EINVAL;
    }
    if ((q->flags&(R3D_CLEAR_DEPTH|R3D_DEPTH_TEST|R3D_DEPTH_WRITE)) &&
        (!r[1] || r[1]->kind!=R3D_D16 || r[1]->width!=r[0]->width || r[1]->height!=r[0]->height)) return -EINVAL;
    if ((q->flags&R3D_TEXTURED) && (!r[2] || r[2]->kind!=R3D_TEXTURE ||
        r[2]->width<32 || r[2]->height<16 || (r[2]->width&(r[2]->width-1)) || (r[2]->height&(r[2]->height-1)))) return -EINVAL;
    if ((q->flags&R3D_COPY) && (!r[3] || r[3]->kind!=R3D_LINEAR_COLOR ||
        r[3]->width!=r[0]->width || r[3]->height!=r[0]->height)) return -EINVAL;
    for (unsigned i=0;i<q->count;i++) if (q->vertices[i].x<0 || q->vertices[i].y<0 ||
        q->vertices[i].x>(int32_t)(r[0]->width<<16) || q->vertices[i].y>(int32_t)(r[0]->height<<16)) return -EINVAL;
    uint32_t words[22];
    return q->count ? savage3d_hw_triangle_packet(q->vertices,words) : 0;
}
static uint32_t base(const struct render3d_resource *r) { return r->offset+4096; }
static uint32_t memory_read(struct savage3d_device *d,bool alias,uint32_t off,unsigned bytes)
{ return d->memory.read(d->memory.ctx,alias,off,bytes); }
static void memory_write(struct savage3d_device *d,bool alias,uint32_t off,uint32_t value,unsigned bytes)
{ d->memory.write(d->memory.ctx,alias,off,value,bytes); }
static int tile(struct savage3d_device *d,const struct render3d_resource *r)
{
    unsigned depth=r->kind==R3D_D16;
    unsigned tiles=(r->width+(depth ? 63 : 31))>>(depth ? 6 : 5);
    return checked_mmio(d,0x48c40,(depth ? 0x80000000u : 0xc0000000u)|(tiles<<20)|(base(r)>>6));
}
static int guards(struct savage3d_device *d,struct render3d_resource *r,bool initialize)
{
    for (unsigned side=0;side<2;side++) for (unsigned i=0;i<4096;i+=4) {
        uint32_t off=r->offset+(side ? r->reserved-4096 : 0)+i;
        if (initialize) memory_write(d,false,off,0xa55ac33cu,4);
        else {
            uint32_t value=memory_read(d,false,off,4);
            if (value!=0xa55ac33cu) { d->expected=0xa55ac33cu; d->actual=value; d->guard_offset=off; return -EIO; }
        }
    }
    return 0;
}
static int upload(struct savage3d_device *d,struct render3d_resource *r)
{
    if (r->resident) return guards(d,r,false);
    guards(d,r,true);
    if (r->kind==R3D_COLOR || r->kind==R3D_D16) {
        int e=tile(d,r); if (e) return e;
        unsigned bytes=r->kind==R3D_D16 ? 2 : 4, pitch=bytes==2 ? 4096 : 8192;
        for (unsigned y=0;y<r->height;y++) for (unsigned x=0;x<r->width;x++) {
            uint32_t value=0; memcpy(&value,(uint8_t *)r->shadow+y*r->pitch+x*bytes,bytes);
            if (bytes==2) value=65535-value;
            memory_write(d,true,y*pitch+x*bytes,value,bytes);
        }
    } else for (unsigned y=0;y<r->height;y++) for (unsigned x=0;x<r->width;x++) {
        uint32_t off=y*r->pitch+x*4,value;
        memcpy(&value,(uint8_t *)r->shadow+off,4);
        if (r->kind==R3D_TEXTURE) (void)savage3d_texture_offset(r->width,r->height,x,y,&off);
        memory_write(d,false,base(r)+off,value,4);
    }
    d->engine->io.drain(d->engine->io.ctx); r->resident=true; return 0;
}
static uint32_t descriptor(const struct render3d_resource *r,bool tiled)
{
    unsigned bytes=r->kind==R3D_D16 ? 2 : 4;
    uint32_t stride=tiled ? ((r->width*bytes+127)&~127u)/bytes : r->width;
    /* SuperSavage TILE_DESTINATION=1 (not Savage4 TILE_16/32=2/3). */
    return 0x10000000u|(tiled ? 0x01000000u : 0)|(bytes*8u<<16)|stride;
}
static int clear_resource(struct savage3d_device *d,struct render3d_resource *r,uint32_t value)
{
    if (r->kind==R3D_D16) value=65535-value;
    /* RECT + XP/YP, ROP copy, SEND_COLOR, DEST_PBD_NEW. Linux clear and
     * X.Org SAVAGESelectBuffer supply the inline PBD and destination layout. */
    uint32_t words[]={0x4bcc8c00u,base(r),descriptor(r,true),value,0,(r->height<<16)|r->width};
    return packet(d,words,6);
}
static int state(struct savage3d_device *d,const struct render3d_request *q,struct render3d_resource *const r[4])
{
    unsigned tw=0,th=0;
    if (r[2]) { while ((1u<<tw)<r[2]->width) tw++; while ((1u<<th)<r[2]->height) th++; }
    uint32_t values[]={0xc4000010u|((q->flags&R3D_DEPTH_WRITE) ? 0x08000000u : 0),
        0x0005f040u,0x0005f040u,r[2] ? base(r[2])|2u : base(r[0])|2u,
        r[2] ? base(r[2])|2u : base(r[0])|2u,
        (q->flags&R3D_TEXTURED) ? 0x00110011u : 0x00850405u,0x00870407u,
        0x60000000u|((q->flags&R3D_TEXTURED) ? 0x02000300u|tw|(th<<4) : 0),
        0,0x000ffff7u,0x80000000u|((q->flags&(R3D_DEPTH_TEST|R3D_DEPTH_WRITE)) ? 0x20u : 0u)|
            ((q->flags&R3D_DEPTH_TEST) ? 4u : 7u),
        r[1] ? (base(r[1])>>11)|(((r[1]->width+63)>>6)<<25) : 0,
        0x80000080u|((base(r[0])>>11)<<8)|((r[0]->width+31)>>5),
        0x800u,(1u<<26)|(r[0]->width-1)|((r[0]->height-1)<<12),0x1818u,0x4f000000u};
    for (unsigned i=0;i<17;i++) { int e=setreg(d,selectors[i],values[i]); if (e) return e; }
    return 0;
}
static int read_resource(struct savage3d_device *d,struct render3d_resource *r)
{
    if (r->kind==R3D_TEXTURE) return guards(d,r,false);
    bool alias=r->kind!=R3D_LINEAR_COLOR;
    int e=alias ? tile(d,r) : 0; if (e) return e;
    unsigned bytes=r->kind==R3D_D16 ? 2 : 4, pitch=alias ? (bytes==2 ? 4096 : 8192) : r->pitch;
    for (unsigned y=0;y<r->height;y++) for (unsigned x=0;x<r->width;x++) {
        uint32_t value=memory_read(d,alias,(alias ? 0 : base(r))+y*pitch+x*bytes,bytes);
        if (bytes==2) value=65535-(value&65535);
        memcpy((uint8_t *)r->shadow+y*r->pitch+x*bytes,&value,bytes);
    }
    return guards(d,r,false);
}
static int execute(void *device,const struct render3d_request *q,struct render3d_resource *const r[4])
{
    struct savage3d_device *d=device;
    int e=validate(d,q,r); if (e) return e; /* no write on invalid extents */
    if (!d->locked || (!d->qualified && !d->probing) || d->lost) return -ENODEV;
    d->slice_started=d->engine->io.now_us(d->engine->io.ctx);
    struct saved_state saved;
    d->failed_phase=1; e=save_state(d,&saved); if (e) return e;
    d->failed_phase=2; e=checked_mmio(d,0x850c,saved.agd|0x8000u);
    for (unsigned i=0;!e && i<4;i++) if (r[i]) e=upload(d,r[i]);
    if (!e) d->failed_phase=3;
    if (!e && (q->flags&R3D_CLEAR_COLOR)) e=clear_resource(d,r[0],q->clear_color);
    if (!e && (q->flags&R3D_CLEAR_DEPTH)) e=clear_resource(d,r[1],q->clear_depth);
    if (!e) d->failed_phase=4;
    if (!e && q->count) {
        e=state(d,q,r);
        uint32_t words[22];
        if (!e) e=savage3d_hw_triangle_packet(q->vertices,words);
        if (!e) e=packet(d,words,22);
        if (!e) d->triangles++;
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
        for (unsigned i=0;i<4 && !e;i++) if (r[i]) e=read_resource(d,r[i]);
    }
    /* Never restore/retry on an unproven writer. Preserve its mappings. */
    if (!e) { d->failed_phase=6; e=restore_state(d,&saved); }
    if (!e) d->failed_phase=0;
    return e;
}
static bool qualified(void *device,uint32_t generation)
{
    struct savage3d_device *d=device;
    return d && d->qualified && !d->lost && d->generation==generation && d->engine &&
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
    if (!d) return;
    d->qualified=false; d->lost=true;
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
const struct render3d_backend *savage3d_backend(void)
{
    static const struct render3d_backend backend={.features=R3D_FEATURES,.qualified=qualified,
        .layout=layout,.validate=validate,.enter=enter,.leave=leave,.execute=execute,
        .idle=idle,.quarantine=quarantine,.attach=savage3d_owner_attach,.max_size=64,.detach=detach};
    return &backend;
}

int savage3d_probe(struct savage3d_device *d)
{
    if (!d || !d->engine || !d->memory.read || !d->memory.write || !d->memory.enter ||
        !d->memory.leave || !d->generation || !d->heap.bytes || (d->heap.first&4095u) ||
        d->heap.first>UINT32_MAX-d->heap.bytes || d->heap.bytes<96*1024u ||
        !d->engine->active || d->engine->quarantined || d->engine->chip!=SAVAGE_SUPER) return -ENODEV;
    /* Small shadows are allocated by the owner before locks, not on the
     * kernel stack. Consumer is a scratch service with four typed resources. */
    if (!d->consumer) return -ENOMEM;
    struct render3d_context *c=&d->consumer->contexts[0];
    if (!c->resources) return -ENOMEM;
    struct render3d_resource *r[4];
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
