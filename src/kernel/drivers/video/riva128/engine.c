/*
 * Copyright (c) 1993-1999 NVIDIA, Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/* Origin: X.Org xf86-video-nv 0b6619fec39fada995646fb95bb25e72d3c9a491,
 * src/riva_hw.{c,h}, riva_tbl.h, riva_xaa.c. Jarno Paananen's XAA adaptation.
 * NV3 register fragments, with CiukiOS ownership and bounded waits. */
#include <ciuki/kernel.h>
#include "nv3.h"
#include "tables.h"
/*
 * Copyright 1996-1997  David J. McKay
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * DAVID J. MCKAY BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Rewritten with reference from mga driver and 3.3.4 NVIDIA driver by
   Jarno Paananen <jpaana@s2.org> */

static uint32_t rd(struct nv3_engine *s, uint32_t r)
{ return s->io.read32(s->io.ctx,r); }
static void wr(struct nv3_engine *s, uint32_t r, uint32_t v)
{ s->io.write32(s->io.ctx,r,v); }
static uint8_t indexed_read(struct nv3_engine *s, uint32_t index, uint8_t reg)
{
    uint8_t saved=s->io.read8(s->io.ctx,index);
    s->io.write8(s->io.ctx,index,reg);
    uint8_t v=s->io.read8(s->io.ctx,index+1);
    s->io.write8(s->io.ctx,index,saved);
    return v;
}
static uint8_t pixel_pan(struct nv3_engine *s)
{
    /* Reading INPUT_STATUS resets the attribute controller's address/data
     * latch. Only its selector is changed; preserve palette enable. */
    uint8_t saved=s->io.read8(s->io.ctx,0x6013c0);
    (void)s->io.read8(s->io.ctx,0x6013da);
    s->io.write8(s->io.ctx,0x6013c0,0x13u|(saved&0x20u));
    uint8_t pan=s->io.read8(s->io.ctx,0x6013c1);
    (void)s->io.read8(s->io.ctx,0x6013da);
    s->io.write8(s->io.ctx,0x6013c0,saved);
    (void)s->io.read8(s->io.ctx,0x6013da);
    return pan;
}
static bool cr_write(struct nv3_engine *s, uint8_t reg, uint8_t v)
{
    uint8_t seq=s->io.read8(s->io.ctx,NV3_SEQ_INDEX);
    s->io.write8(s->io.ctx,NV3_SEQ_INDEX,6);
    uint8_t lock=s->io.read8(s->io.ctx,NV3_SEQ_DATA);
    s->io.write8(s->io.ctx,NV3_SEQ_DATA,0x57);
    uint8_t saved=s->io.read8(s->io.ctx,NV3_CRTC_INDEX);
    s->io.write8(s->io.ctx,NV3_CRTC_INDEX,reg);
    s->io.write8(s->io.ctx,NV3_CRTC_DATA,v);
    bool ok=s->io.read8(s->io.ctx,NV3_CRTC_DATA)==v;
    s->io.write8(s->io.ctx,NV3_CRTC_INDEX,saved);
    s->io.write8(s->io.ctx,NV3_SEQ_DATA,lock);
    s->io.write8(s->io.ctx,NV3_SEQ_INDEX,seq);
    return ok;
}
static bool hide(struct nv3_engine *s)
{
    uint8_t control=indexed_read(s,NV3_CRTC_INDEX,0x31);
    s->cursor_hidden=control!=0xff && cr_write(s,0x31,control&~1u);
    return s->cursor_hidden;
}
void nv3_engine_quarantine(struct nv3_engine *s, const char *reason)
{
    s->active=false; s->quarantined=true; s->cpu_safe=false;
    s->reason=reason; s->fallbacks++;
    if (s->touched) (void)hide(s);
}
static int fault(struct nv3_engine *s, int error)
{
    /* Only preflight may return the clean-refusal code. In particular a
     * pause/clock service failure after COPY launch must never invite replay. */
    if (error==-EOPNOTSUPP) error=-EIO;
    nv3_engine_quarantine(s,error==-ETIMEDOUT ? "engine_timeout" : "engine_fault");
    return error;
}
/* Cache0, cache1, runout and PGRAPH all have to drain. The donor's
 * subchannel free-count check alone is insufficient before object setup. */
static int idle_sample(struct nv3_engine *s)
{
    const uint32_t addresses[]={NV3_PFIFO+0x1014,NV3_PFIFO+0x1214,NV3_PFIFO+0x400};
    bool empty=true;
    for (unsigned i=0;i<ARRAY_SIZE(addresses);i++) {
        uint32_t v=rd(s,addresses[i]);
        if (v==UINT32_MAX) return -EIO;
        if (!(v&0x10u)) empty=false;
    }
    uint32_t busy=rd(s,NV3_PGRAPH+0x6b0);
    if (busy==UINT32_MAX) return -EIO;
    if (busy&1u) empty=false;
    if (s->configured) {
        uint16_t free=s->io.read16(s->io.ctx,NV3_ROP+0x10);
        if (free==UINT16_MAX) return -EIO;
        if (free<s->empty_count) empty=false;
    }
    return empty ? 0 : -EBUSY;
}
/* Every wait has an elapsed-time deadline AND a frozen-clock read cap.
 * pause retains the generic drawing claim while releasing its mutex. */
static int wait_for(struct nv3_engine *s, uint32_t subchannel, unsigned words)
{
    uint64_t start=s->io.now_us(s->io.ctx), slice=start;
    s->waits++;
    for (unsigned n=0;n<NV3_POLL_CAP;n++) {
        int e;
        if (words) {
            uint16_t free=s->io.read16(s->io.ctx,subchannel+0x10);
            e=free==UINT16_MAX ? -EIO : free>=words*4u ? 0 : -EBUSY;
        } else e=idle_sample(s);
        if (e!=-EBUSY) return e;
        uint64_t now=s->io.now_us(s->io.ctx);
        if (now-start>=NV3_WAIT_US) return -ETIMEDOUT;
        if (s->pause && (now-slice>=1000u || (n && n%200u==0))) {
            e=s->pause(s->pause_ctx);
            if (e) return e;
            slice=s->io.now_us(s->io.ctx);
        }
        e=s->io.delay_us(s->io.ctx,5);
        if (e) return e;
    }
    return -ETIMEDOUT;
}
static int sync_engine(void *ctx)
{
    struct nv3_engine *s=ctx;
    s->io.drain(s->io.ctx);
    int e=wait_for(s,0,0);
    if (e) return fault(s,e);
    if (s->quarantined && !hide(s)) return fault(s,-EIO);
    s->cpu_safe=true;
    return 0;
}
static int room(struct nv3_engine *s, uint32_t channel, unsigned words)
{
    int e=wait_for(s,channel,words);
    return e ? fault(s,e) : 0;
}
static bool rect_valid(const struct nv3_engine *s,const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>=0 && r->height>=0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height;
}
static int begin(struct nv3_engine *s)
{
    if (!s->active || s->quarantined) return -EIO;
    int e=sync_engine(s);
    if (e) return e;
    s->io.drain(s->io.ctx);
    s->cpu_safe=false;
    return 0;
}
static int pattern_state(struct nv3_engine *s,uint32_t a,uint32_t b,uint32_t p0,uint32_t p1)
{
    int e=room(s,NV3_PATTERN,4);
    if (e) return e;
    wr(s,NV3_PATTERN+0x310,a); wr(s,NV3_PATTERN+0x314,b);
    wr(s,NV3_PATTERN+0x318,p0); wr(s,NV3_PATTERN+0x31c,p1);
    return 0;
}
static int rop(struct nv3_engine *s,uint32_t value)
{
    int e=room(s,NV3_ROP,1);
    if (!e) wr(s,NV3_ROP+0x300,value);
    return e;
}
static int solid(struct nv3_engine *s)
{
    int e=pattern_state(s,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX);
    return e ? e : rop(s,0xcc);
}
static int complete(struct nv3_engine *s)
{
    int e=sync_engine(s);
    if (!e) s->operations++;
    return e;
}
static int fill(void *ctx,const struct fb_rect *r,uint32_t colour)
{
    struct nv3_engine *s=ctx;
    if (!rect_valid(s,r)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    int e=begin(s);
    if (!e) e=solid(s);
    if (!e) e=room(s,NV3_BITMAP,3);
    if (e) return e;
    wr(s,NV3_BITMAP+0x3fc,colour);
    wr(s,NV3_BITMAP+0x400,(uint32_t)r->x<<16 | (uint32_t)r->y);
    s->io.drain(s->io.ctx);
    wr(s,NV3_BITMAP+0x404,(uint32_t)r->width<<16 | (uint32_t)r->height);
    return complete(s);
}
static int copy(void *ctx,const struct fb_rect *r,int32_t x,int32_t y)
{
    struct nv3_engine *s=ctx;
    struct fb_rect dst={x,y,r ? r->width : 0,r ? r->height : 0};
    if (!rect_valid(s,r) || !rect_valid(s,&dst)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    int e=begin(s);
    if (!e) e=solid(s);
    if (!e) e=room(s,NV3_BLT,3);
    if (e) return e;
    /* NV3 selects overlap traversal internally; unlike NV4 DMA packets,
     * no direction flags or bottom-right adjustment belong here. */
    wr(s,NV3_BLT+0x300,(uint32_t)r->y<<16 | (uint32_t)r->x);
    wr(s,NV3_BLT+0x304,(uint32_t)y<<16 | (uint32_t)x);
    s->io.drain(s->io.ctx);
    wr(s,NV3_BLT+0x308,(uint32_t)r->height<<16 | (uint32_t)r->width);
    return complete(s);
}
static uint32_t pattern_colour(const struct nv3_engine *s,uint32_t c)
{
    if (s->fb.bpp==32) return (c&0xffffffu)|0xff000000u;
    if (s->fb.green_size==5) return (c&0x7fffu)|0xffff8000u;
    /* riva_xaa.c: the pattern object accepts RGB555/RGB888, not RGB565. */
    return (c&0xf800u)<<8 | (c&0x7e0u)<<5 | (c&0x1fu)<<3 | 0xff000000u;
}
static uint32_t pattern_word(const uint8_t *p)
{
    uint32_t word=0;
    for (unsigned y=0;y<4;y++) for (unsigned x=0;x<8;x++)
        if (p[y]&(0x80u>>x)) word|=1u<<(y*8+x);
    return word;
}
static int pattern(void *ctx,const struct fb_rect *r,const uint8_t p[8],uint32_t fg,uint32_t bg)
{
    struct nv3_engine *s=ctx;
    if (!p || !rect_valid(s,r)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    int e=begin(s);
    if (!e) e=pattern_state(s,pattern_colour(s,bg),pattern_colour(s,fg),pattern_word(p),pattern_word(p+4));
    if (!e) e=rop(s,0xf0);
    if (!e) e=room(s,NV3_BITMAP,3);
    if (e) return e;
    wr(s,NV3_BITMAP+0x3fc,pattern_colour(s,fg));
    wr(s,NV3_BITMAP+0x400,(uint32_t)r->x<<16 | (uint32_t)r->y);
    s->io.drain(s->io.ctx);
    wr(s,NV3_BITMAP+0x404,(uint32_t)r->width<<16 | (uint32_t)r->height);
    return complete(s);
}
static uint16_t cursor_colour(const struct nv3_engine *s,uint32_t c)
{
    if (s->fb.bpp==16) {
        if (s->fb.green_size==5) return (c&0x7fffu)|0x8000u;
        return (c&0x1fu) | (c&0x7c0u)>>1 | (c&0xf800u)>>1 | 0x8000u;
    }
    return (c&0xf80000u)>>9 | (c&0xf800u)>>6 | (c&0xf8u)>>3 | 0x8000u;
}
static int cursor_set(void *ctx,const struct accel_cursor *c)
{
    struct nv3_engine *s=ctx;
    if (!c) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -EIO;
    if (!hide(s)) return fault(s,-EIO);
    s->cursor_loaded=false;
    uint16_t fg=cursor_colour(s,c->foreground),bg=cursor_colour(s,c->background);
    for (unsigned y=0;y<32;y++) for (unsigned x=0;x<32;x+=2) {
        uint32_t packed=0;
        for (unsigned k=0;k<2;k++) {
            unsigned bit=31-x-k;
            uint16_t pixel=(c->mask[y]&(1u<<bit)) ? ((c->image[y]&(1u<<bit)) ? fg : bg) : 0;
            packed|=(uint32_t)pixel<<(16*k);
        }
        uint32_t address=NV3_CURSOR+y*64+x*2;
        wr(s,address,packed);
        if (rd(s,address)!=packed) return fault(s,-EIO);
    }
    s->io.drain(s->io.ctx);
    s->cursor_loaded=true;
    return 0;
}
static int cursor_move(void *ctx,int32_t x,int32_t y)
{
    struct nv3_engine *s=ctx;
    if (x<INT16_MIN || x>INT16_MAX || y<INT16_MIN || y>INT16_MAX) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -EIO;
    if (!s->cursor_loaded) return -EINVAL;
    uint32_t pos=(uint16_t)x | (uint32_t)(uint16_t)y<<16;
    wr(s,NV3_CURSOR_POSITION,pos);
    return rd(s,NV3_CURSOR_POSITION)==pos ? 0 : fault(s,-EIO);
}
static int cursor_show(void *ctx,bool visible)
{
    struct nv3_engine *s=ctx;
    if (!visible) return hide(s) ? 0 : fault(s,-EIO);
    if (!s->active || s->quarantined) return -EIO;
    if (!s->cursor_loaded) return -EINVAL;
    uint8_t v=indexed_read(s,NV3_CRTC_INDEX,0x31);
    if (v==0xff || !cr_write(s,0x31,v|1u)) return fault(s,-EIO);
    s->cursor_hidden=false;
    return 0;
}
static int stop(void *ctx)
{
    struct nv3_engine *s=ctx;
    s->active=false; s->cpu_safe=false;
    s->io.drain(s->io.ctx);
    bool hidden=hide(s);
    int e=wait_for(s,0,0);
    if (!hidden || e) return fault(s,e ? e : -EIO);
    /* No reset: an empty, disabled PIO producer is the only stop proof. */
    const uint32_t controls[]={NV3_PFIFO+0x1000,NV3_PFIFO+0x1040,
                              NV3_PFIFO+0x1200,NV3_PFIFO+0x1240,NV3_PFIFO+0x1220};
    for (unsigned i=0;i<ARRAY_SIZE(controls);i++) {
        wr(s,controls[i],0);
        if (rd(s,controls[i])&1u) return fault(s,-EIO);
    }
    s->io.drain(s->io.ctx);
    s->cpu_safe=true; s->cursor_loaded=false;
    return 0;
}
static unsigned caps(void *ctx)
{ const struct nv3_engine *s=ctx; return s->active && !s->quarantined ? 3u : 0; }
static bool ready(void *ctx) { return ((struct nv3_engine *)ctx)->cpu_safe; }
static void pause_hook(void *ctx,int (*fn)(void *),void *arg)
{ struct nv3_engine *s=ctx; s->pause=fn; s->pause_ctx=arg; }

bool nv3_mode_supported(const struct fb_device *d)
{
    if (!d || !d->present || !d->mapped || d->width<16 || d->width>2048 ||
        d->height<16 || d->height>2048 || (d->bpp!=16 && d->bpp!=32) ||
        d->blue_size!=5+(d->bpp==32 ? 3 : 0) || d->blue_pos ||
        d->red_size!=d->blue_size || d->green_size<5 ||
        (d->bpp==32 ? (d->red_pos!=16 || d->green_size!=8 || d->green_pos!=8) :
         (d->green_pos!=5 || d->green_size>6 || d->red_pos!=d->green_size+5))) return false;
    unsigned bytes=d->bpp/8;
    return d->pitch && !(d->pitch%(bytes*32u)) && d->pitch/bytes<=2048 &&
        (uint64_t)d->width*bytes<=d->pitch && (uint64_t)d->height*d->pitch<=d->size &&
        (uintptr_t)d->mapped<=UINTPTR_MAX-d->size;
}
uint32_t nv3_detect_vram(const struct nv3_io *io)
{
    uint32_t pfb=io->read32(io->ctx,NV3_PFB),boot=io->read32(io->ctx,0);
    if (pfb==UINT32_MAX || boot==UINT32_MAX) return 0;
    if (pfb&0x20u) {
        if ((boot&0xf0u)!=0x20u || (boot&15u)<2) return 8u<<20;
        return (pfb&3u)==2 ? 4u<<20 : (pfb&3u)==1 ? 2u<<20 : 8u<<20;
    }
    return (pfb&3u)==0 ? 8u<<20 : (pfb&3u)==2 ? 4u<<20 : 2u<<20;
}
static void table(struct nv3_engine *s,uint32_t base,const uint32_t (*t)[2],unsigned count)
{
    for (unsigned i=0;i<count;i++) {
        uint32_t address=base+t[i][0]*4u,value=t[i][1];
        /* No unhandled device interrupts in this polling driver. */
        if (address==NV3_PFIFO+0x140 || address==NV3_PGRAPH+0x140) value=0;
        wr(s,address,value);
    }
}
#define LOAD(base,name) table(s,base,name,ARRAY_SIZE(name))
int nv3_engine_init(struct nv3_engine *s,const struct nv3_io *io,const struct fb_device *fb,uint32_t ram)
{
    if (s->quarantined || s->active) return -EIO;
    if (!nv3_mode_supported(fb) || ram<2u<<20 || ram>8u<<20 ||
        fb->size>ram-0x8000u || !io || !io->read32 || !io->write32 ||
        !io->read16 || !io->read8 || !io->write8 || !io->now_us || !io->delay_us || !io->drain)
        return -EOPNOTSUPP;
    s->io=*io; s->fb=*fb; s->ram_bytes=ram; s->reason="pending";
    s->ops=(struct accel_ops){.ctx=s,.sync=sync_engine,.fill_rect=fill,.copy_rect=copy,
        .pattern_rect=pattern,.cursor_set=cursor_set,.cursor_move=cursor_move,.cursor_show=cursor_show,
        .stop=stop,.capabilities=caps,.cpu_ready=ready,.set_pause=pause_hook};
    /* PFB configuration is firmware-owned. Require its format and padded
     * pitch to agree; never silently reprogram scanout to fit the engine. */
    uint32_t config=((fb->pitch/(fb->bpp/8)+31u)/32u) | (fb->bpp==32 ? 3u : 2u)<<8 | 0x1000u;
    if ((rd(s,NV3_PFB+0x200)&0x137fu)!=config ||
        indexed_read(s,NV3_CRTC_INDEX,0x0c) || indexed_read(s,NV3_CRTC_INDEX,0x0d) ||
        (indexed_read(s,NV3_CRTC_INDEX,0x19)&0x1fu) ||
        (indexed_read(s,NV3_CRTC_INDEX,0x2d)&0x60u) || pixel_pan(s)) return -EOPNOTSUPP;
    /* The caller has already refused PCI bus mastering. Refuse firmware's
     * DMA producer too; there is no host DMA takeover in this transport. */
    if (rd(s,NV3_PFIFO+0x1220)&1u) return -EOPNOTSUPP;
    s->touched=true; s->cpu_safe=false;
    /* Enable only the PIO FIFO and graphics units, preserving scanout and
     * every other enable bit. Do not import the donor's all-unit reset. */
    uint32_t enabled=rd(s,0x200);
    if (enabled==UINT32_MAX) return fault(s,-EIO);
    wr(s,0x200,enabled|0x1100u);
    if (rd(s,0x200)!=(enabled|0x1100u)) return fault(s,-EIO);
    int e=wait_for(s,0,0);
    if (e) return fault(s,e);
    /* Unlock extended CRTC access, preserving sequencer index and lock. */
    uint8_t seq=s->io.read8(s->io.ctx,NV3_SEQ_INDEX);
    s->io.write8(s->io.ctx,NV3_SEQ_INDEX,6);
    uint8_t lock=s->io.read8(s->io.ctx,NV3_SEQ_DATA);
    s->io.write8(s->io.ctx,NV3_SEQ_DATA,0x57);
    bool cursor_ok=cr_write(s,0x30,0) && cr_write(s,0x2f,0) &&
        cr_write(s,0x31,0x78u | (indexed_read(s,NV3_CRTC_INDEX,0x31)&2u));
    s->io.write8(s->io.ctx,NV3_SEQ_DATA,lock);
    s->io.write8(s->io.ctx,NV3_SEQ_INDEX,seq);
    if (!cursor_ok || !hide(s)) return fault(s,-EIO);
    LOAD(NV3_PFIFO,nv3TablePFIFO);
    wr(s,NV3_PFIFO+0x1220,0);
    if (rd(s,NV3_PFIFO+0x1220)&1u) return fault(s,-EIO);
    LOAD(NV3_INSTANCE,nv3TablePRAMIN);
    /* Local translation is limited to this card's VRAM; no host addresses. */
    wr(s,NV3_INSTANCE+0x501*4,ram-1u);
    for (unsigned i=0;i<0x800;i++) wr(s,NV3_INSTANCE+(0x502+i)*4,i<ram/4096 ? (i<<12)|3u : 0);
    LOAD(NV3_PGRAPH,nv3TablePGRAPH);
    if (fb->bpp==16) LOAD(NV3_INSTANCE,nv3TablePRAMIN_15BPP);
    else LOAD(NV3_INSTANCE,nv3TablePRAMIN_32BPP);
    wr(s,NV3_PGRAPH+0x6a8,fb->bpp==16 ? 0x2222 : 0x3333);
    for (unsigned i=0;i<4;i++) {
        wr(s,NV3_PGRAPH+0x630+i*4,0);
        wr(s,NV3_PGRAPH+0x650+i*4,fb->pitch);
    }
    LOAD(NV3_FIFO,RivaTableFIFO);
    e=wait_for(s,0,0);
    if (e) return fault(s,e);
    s->empty_count=s->io.read16(s->io.ctx,NV3_ROP+0x10);
    if (!s->empty_count || s->empty_count==UINT16_MAX || (s->empty_count&3u)) return fault(s,-EIO);
    s->configured=true;
    if ((e=room(s,NV3_PATTERN,1))) return e;
    wr(s,NV3_PATTERN+0x308,0);
    if ((e=room(s,NV3_CLIP,2))) return e;
    wr(s,NV3_CLIP+0x300,0);
    wr(s,NV3_CLIP+0x304,fb->height<<16 | fb->width);
    s->active=true;
    if ((e=solid(s)) || (e=sync_engine(s))) return fault(s,e);
    s->reason="none";
    return 0;
}
int nv3_engine_cursor_test(struct nv3_engine *s,uint64_t deadline,bool (*cancel)(void *),void *ctx)
{
    struct accel_cursor transparent={0};
    int e=0;
    for (unsigned step=0;step<4 && !e;step++) {
        if (s->io.now_us(s->io.ctx)>=deadline || (cancel && cancel(ctx))) {
            e=s->io.now_us(s->io.ctx)>=deadline ? -ETIMEDOUT : -EINTR; break;
        }
        if (!step) e=cursor_set(s,&transparent);
        else if (step==1) e=cursor_move(s,-1,-1);
        else if (step==2) e=cursor_show(s,true);
        else e=cursor_show(s,false);
    }
    bool hidden=hide(s);
    return e ? e : hidden ? 0 : fault(s,-EIO);
}
