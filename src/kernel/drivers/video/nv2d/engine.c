/* Origin: X.Org xf86-video-nv 0b6619fec39fada995646fb95bb25e72d3c9a491,
 * src/nv_xaa.c, nv_dma.h, nv_cursor.c, nv_dac.c and nv_setup.c.
 * Native adaptation: synchronous PIO/VRAM transport, bounds and quarantine.
 * Original command packing and cursor conversion copyright follows. */
/*
 * Copyright (c) 2003 NVIDIA, Corporation
 * Copyright (c) 1993-2003 NVIDIA, Corporation (nv_hw.c)
 * Copyright (c) 2007 NVIDIA, Corporation (nv_dac.c)
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
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "nv2d.h"
#include "matches.h"

#define RD(s,r) (s)->io.read((s)->io.ctx,(r))
#define WR(s,r,v) (s)->io.write((s)->io.ctx,(r),(v))
static bool valid(struct nv2d *s) { return s->io.valid(s->io.ctx); }
static uint8_t cr_read(struct nv2d *s, uint8_t index)
{
    s->io.write8(s->io.ctx,NV2D_PCIO+s->head_offset+0x3d4,index);
    return s->io.read8(s->io.ctx,NV2D_PCIO+s->head_offset+0x3d5);
}
static void cr_write(struct nv2d *s, uint8_t index, uint8_t value)
{
    s->io.write8(s->io.ctx,NV2D_PCIO+s->head_offset+0x3d4,index);
    s->io.write8(s->io.ctx,NV2D_PCIO+s->head_offset+0x3d5,value);
}
/* nv_hw.c NVShowHideCursor: NV40 latches enable at a position rewrite. */
static int cursor_latch(struct nv2d *s)
{
    if (s->architecture<0x40) return 0;
    uint32_t position=RD(s,0x680300+s->head_offset);
    /* ffffffff is the valid signed position (-1,-1). Control readback and
     * the resource generation establish register availability separately. */
    WR(s,0x680300+s->head_offset,position);
    return RD(s,0x680300+s->head_offset)==position ? 0 : -EIO;
}
int nv2d_cursor_hide(struct nv2d *s)
{
    if (!s->touched) { s->cursor_hidden=true; return 0; }
    if (!valid(s)) { s->cursor_hidden=false; return -EIO; }
    uint8_t control=cr_read(s,0x31);
    if (control==255) { s->cursor_hidden=false; return -EIO; }
    cr_write(s,0x31,control&~1u);
    s->cursor_hidden=cr_read(s,0x31)==(control&~1u) && !cursor_latch(s);
    return s->cursor_hidden ? 0 : -EIO;
}
void nv2d_quarantine(struct nv2d *s, const char *reason)
{
    s->quarantined=true; s->active=false; s->cpu_safe=false;
    s->reason=reason ? reason : "engine_fault";
    /* Hiding the overlay is necessary, but never establishes engine idle. */
    (void)nv2d_cursor_hide(s);
}
static int fault(struct nv2d *s, const char *reason, int e)
{ nv2d_quarantine(s,reason); return e; }
static int checkpoint(struct nv2d *s)
{
    if (!valid(s)) return -EIO;
    uint64_t now=s->io.now(s->io.ctx);
    if (s->pause && now-s->slice>=1000) {
        s->slice=now;
        int e=s->pause(s->pause_ctx);
        if (e) return e;
        if (!valid(s)) return -EIO;
    }
    return 0;
}
/* count is bytes, not words. Every successful wait observes posted writes.
 * Idle uses the capacity sampled before any channel bindings, plus PGRAPH. */
static int wait_fifo(struct nv2d *s, unsigned bytes, bool idle)
{
    uint64_t start=s->io.now(s->io.ctx);
    s->waits++;
    for (unsigned i=0;i<NV2D_WAIT_POLLS;i++) {
        int e=checkpoint(s);
        if (e) return e;
        uint32_t free=RD(s,NV2D_FREE), busy=RD(s,NV2D_GRAPH_BUSY);
        if (free==UINT32_MAX || busy==UINT32_MAX) return -EIO;
        if ((free&0xffff)>=bytes && (!idle || !busy)) return 0;
        if (s->io.now(s->io.ctx)-start>=NV2D_WAIT_US) return -ETIMEDOUT;
        /* A frozen clock cannot remove either the poll or slice bound. */
        if (s->pause && i && !(i%200)) {
            e=s->pause(s->pause_ctx);
            if (e) return e;
        }
        s->io.delay(s->io.ctx,5);
    }
    return -ETIMEDOUT;
}
/* X nv nv_xaa.c NVDmaWait/NVSync and nv_local.h WRITE_PUT.
 * All offsets are private local-VRAM words; no host address enters PFIFO. */
static int dma_get(struct nv2d *s, uint32_t *get)
{
    uint32_t raw=RD(s,NV2D_GET);
    if ((raw&3) || raw>=NV2D_RING_WORDS*4) return -EIO;
    *get=raw/4;
    return 0;
}
static int dma_put(struct nv2d *s, uint32_t put)
{
    if (put>=NV2D_RING_WORDS || !valid(s) || s->quarantined) return -EIO;
    s->cpu_safe=false;
    s->io.drain(s->io.ctx);
    /* A VRAM read completes posted aperture stores before the doorbell. */
    (void)s->ring[0];
    s->put=put;
    WR(s,NV2D_PUT,put*4);
    return RD(s,NV2D_PUT)==put*4 ? 0 : -EIO;
}
static int dma_poll(struct nv2d *s, uint64_t start, unsigned poll)
{
    if (s->io.now(s->io.ctx)-start>=NV2D_WAIT_US) return -ETIMEDOUT;
    if (s->pause && poll && !(poll%200)) {
        int e=s->pause(s->pause_ctx);
        if (e) return e;
    }
    s->io.delay(s->io.ctx,5);
    return 0;
}
static int dma_idle(struct nv2d *s)
{
    /* Interrupted wrap publication cannot be replaced with a guessed fence. */
    if (!s->ring_ready || s->wrap_pending) return -EIO;
    uint64_t start=s->io.now(s->io.ctx);
    s->waits++;
    for (unsigned i=0;i<NV2D_WAIT_POLLS;i++) {
        int e=checkpoint(s); uint32_t get=0;
        if (e) return e;
        if (RD(s,NV2D_PUT)!=s->put*4 || (e=dma_get(s,&get))) return -EIO;
        uint32_t busy=RD(s,NV2D_GRAPH_BUSY);
        if (busy==UINT32_MAX) return -EIO;
        if (get==s->put && !busy) return 0;
        e=dma_poll(s,start,i); if (e) return e;
    }
    return -ETIMEDOUT;
}
static int dma_emit(struct nv2d *s, uint32_t method, unsigned count, const uint32_t *words)
{
    if (!s->ring_ready || s->wrap_pending || s->current>=NV2D_RING_WORDS) return -EIO;
    unsigned needed=count+1;
    uint64_t start=s->io.now(s->io.ctx);
    s->waits++;
    for (unsigned i=0;i<NV2D_WAIT_POLLS;i++) {
        uint32_t get=0; int e=checkpoint(s);
        if (e) return e;
        if (s->quarantined || RD(s,NV2D_PUT)!=s->put*4 || (e=dma_get(s,&get))) return -EIO;
        uint32_t free;
        if (s->wrap_pending) {
            if (get<=NV2D_SKIPS) {
                if (s->put<=NV2D_SKIPS) { e=dma_put(s,NV2D_SKIPS+1); if (e) return e; }
                e=dma_poll(s,start,i); if (e) return e;
                continue;
            }
            e=dma_put(s,NV2D_SKIPS); if (e) return e;
            s->current=NV2D_SKIPS;
            s->wrap_pending=false;
            free=get-(NV2D_SKIPS+1);
        } else if (s->put>=get) {
            free=(NV2D_RING_WORDS-1)-s->current;
            if (free<needed) {
                s->cpu_safe=false;
                s->ring[s->current]=0x20000000;
                s->wrap_pending=true;
                continue;
            }
        } else {
            if (get<=s->current) return -EIO;
            free=get-s->current-1;
        }
        if (free>=needed) {
            if (s->quarantined || !valid(s)) return -EIO;
            s->cpu_safe=false;
            s->ring[s->current++]=(count<<18)|method;
            for (unsigned n=0;n<count;n++) s->ring[s->current++]=words[n];
            return dma_put(s,s->current);
        }
        e=dma_poll(s,start,i); if (e) return e;
    }
    return -ETIMEDOUT;
}
int nv2d_idle(struct nv2d *s)
{
    s->cpu_safe=false;
    if (!s->touched) { s->cpu_safe=true; return 0; }
    if (!s->dma && !s->empty_bytes) return -EIO;
    int e=s->dma ? dma_idle(s) : wait_fifo(s,s->empty_bytes,true);
    if (e) return e;
    /* Quarantine can be recoverable for CPU rendering after a drain. A
     * pending error still blocks further engine submissions permanently. */
    if (s->quarantined && nv2d_cursor_hide(s)) return -EIO;
    s->io.drain(s->io.ctx);
    s->cpu_safe=true;
    return 0;
}
int nv2d_emit(struct nv2d *s, uint32_t method, unsigned count, const uint32_t *words)
{
    if (s->quarantined || !valid(s)) return -EIO;
    if (!words || !count || count>32 || (method&3) ||
        method>=0x10000 || (method&0x1fff)+4*count>0x2000) return -EINVAL;
    if (s->dma) return dma_emit(s,method,count,words);
    int e=wait_fifo(s,4*count,false);
    if (e) return e;
    if (s->quarantined || !valid(s)) return -EIO;
    s->cpu_safe=false;
    s->io.drain(s->io.ctx);
    for (unsigned i=0;i<count;i++) WR(s,NV2D_FIFO+method+4*i,words[i]);
    return 0;
}
unsigned nv2d_architecture(uint16_t device)
{
    bool known=false;
    for (unsigned i=0;i<ARRAY_SIZE(nv2d_matches);i++)
        if (nv2d_matches[i].device==device) known=true;
    if (!known) return 0;
    switch (device&0xfff0) {
    case 0x0020: case 0x00a0: return 4;
    case 0x0100: case 0x0110: case 0x0150: case 0x0170:
    case 0x0180: case 0x01a0: case 0x01f0: return 0x10;
    case 0x0200: case 0x0250: case 0x0280: return 0x20;
    case 0x0300: case 0x0310: case 0x0320: case 0x0330: case 0x0340: return 0x30;
    case 0x0040: case 0x00c0: case 0x0140: case 0x0160:
    case 0x0210: case 0x0220: case 0x0240: return 0x40;
    default: return 0;
    }
}
uint16_t nv2d_chipset(const struct nv2d_io *io, uint16_t device)
{
    if (nv2d_architecture(device)) return device;
    bool bridge=false;
    for (unsigned i=0;i<ARRAY_SIZE(nv2d_matches);i++)
        if (nv2d_matches[i].device==device && (device&0xfff0)==0x00f0) bridge=true;
    if (!bridge) return 0;
    uint32_t id=io->read(io->ctx,0x1800);
    uint16_t chip;
    if ((id&0xffff)==0x10de) chip=id>>16;
    else if ((id&0xffff0000)==0xde100000) chip=((id<<8)&0xff00)|((id>>8)&0xff);
    else return 0;
    return nv2d_architecture(chip)>=0x30 ? chip : 0;
}
uint32_t nv2d_tail_bytes(unsigned arch)
{ return arch>=0x40 ? NV2D_NV40_TAIL_BYTES : NV2D_TAIL_BYTES; }
bool nv2d_format(const struct fb_device *f)
{
    if (!f || !f->present || !f->mapped || !f->width || !f->height ||
        f->width>4096 || f->height>4096 || !f->pitch || f->pitch>65535 || (f->pitch&63u) ||
        (f->bpp!=16 && f->bpp!=32) || f->blue_pos ||
        (uintptr_t)f->mapped>UINTPTR_MAX-f->size ||
        (uint64_t)f->width*(f->bpp/8)>f->pitch ||
        (uint64_t)f->height*f->pitch>f->size) return false;
    if (f->bpp==32) return f->red_size==8 && f->red_pos==16 &&
        f->green_size==8 && f->green_pos==8 && f->blue_size==8;
    return f->red_size==5 && f->blue_size==5 && f->green_pos==5 &&
        ((f->green_size==6 && f->red_pos==11) || (f->green_size==5 && f->red_pos==10));
}
uint32_t nv2d_vram(const struct nv2d_io *io, unsigned arch)
{
    /* Read-only donor nv_setup.c nv4GetConfig/nv10GetConfig, discrete only. */
    uint32_t value=io->read(io->ctx,arch==4 ? 0x100000 : 0x10020c);
    if (value==UINT32_MAX) return 0;
    if (arch!=4) {
        value&=0xfff00000;
        /* A truncated NV40 aperture would hide the actual top-of-VRAM
         * instance placement. Refuse it instead of reserving a guessed top. */
        if (arch>=0x40 && value>256u*1024*1024) return 0;
        return value>256u*1024*1024 ? 256u*1024*1024 : value;
    }
    if (value&0x100) return (((value>>12)&15)*2+2)*1024*1024;
    const uint32_t sizes[]={32,4,8,16};
    return sizes[value&3]*1024*1024;
}
static bool rectangle(struct nv2d *s, const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>0 && r->height>0 &&
        (uint64_t)(unsigned)r->x+(unsigned)r->width<=s->fb.width &&
        (uint64_t)(unsigned)r->y+(unsigned)r->height<=s->fb.height;
}
static int admission(struct nv2d *s)
{
    if (!valid(s)) return fault(s,"stale_generation",-EIO);
    if (s->quarantined || !s->active) return -EIO;
    return 0;
}
static int complete(struct nv2d *s, int e)
{
    if (!e) e=nv2d_idle(s);
    if (!e && (RD(s,NV2D_FIFO_INTR) || RD(s,NV2D_GRAPH_INTR))) e=-EIO;
    if (e) return fault(s,e==-ETIMEDOUT ? "engine_timeout" : "engine_fault",e);
    s->operations++;
    return 0;
}
static int solid(struct nv2d *s, uint32_t rop)
{
    uint32_t pattern[]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    int e=nv2d_emit(s,0x4310,4,pattern);
    if (!e) e=nv2d_emit(s,0x2300,1,&rop);
    return e;
}
static int draw_rect(struct nv2d *s, const struct fb_rect *r)
{
    uint32_t coords[]={((uint32_t)r->x<<16)|(unsigned)r->y,
        ((uint32_t)r->width<<16)|(unsigned)r->height};
    return nv2d_emit(s,0xc400,2,coords);
}
static int fill(void *ctx, const struct fb_rect *r, uint32_t colour)
{
    struct nv2d *s=ctx;
    if (!rectangle(s,r)) return -EOPNOTSUPP;
    int e=admission(s); if (e) return e;
    e=solid(s,0xcc);
    if (!e) e=nv2d_emit(s,0xc3fc,1,&colour);
    if (!e) e=draw_rect(s,r);
    return complete(s,e);
}
static int blit(struct nv2d *s, unsigned sx, unsigned sy, unsigned dx, unsigned dy,
                unsigned width, unsigned height)
{
    uint32_t words[]={sx|(sy<<16),dx|(dy<<16),width|(height<<16)};
    return nv2d_emit(s,0xa300,3,words);
}
static int copy(void *ctx, const struct fb_rect *r, int32_t x, int32_t y)
{
    struct nv2d *s=ctx;
    struct fb_rect dst={x,y,r ? r->width : 0,r ? r->height : 0};
    if (!rectangle(s,r) || !rectangle(s,&dst)) return -EOPNOTSUPP;
    int e=admission(s); if (e) return e;
    if (r->x==x && r->y==y) return complete(s,0);
    e=solid(s,0xcc);
    bool overlap=x<r->x+r->width && r->x<x+r->width &&
        y<r->y+r->height && r->y<y+r->height;
    if (!overlap) {
        if (!e) e=blit(s,r->x,r->y,x,y,r->width,r->height);
    } else if (y!=r->y) {
        /* Disjoint source/destination rows, in snapshot-preserving order.
         * No undocumented direction bit and no replay after a partial copy. */
        for (int i=0;!e && i<r->height;i++) {
            int row=y>r->y ? r->height-1-i : i;
            e=blit(s,r->x,r->y+row,x,y+row,r->width,1);
        }
    } else {
        int distance=x>r->x ? x-r->x : r->x-x;
        for (int done=0;!e && done<r->width;) {
            int n=r->width-done<distance ? r->width-done : distance;
            int offset=x>r->x ? r->width-done-n : done;
            e=blit(s,r->x+offset,r->y,x+offset,y,n,r->height);
            done+=n;
        }
    }
    return complete(s,e);
}
static uint8_t reverse8(uint8_t value)
{
    value=(uint8_t)(((value&0x55)<<1)|((value>>1)&0x55));
    value=(uint8_t)(((value&0x33)<<2)|((value>>2)&0x33));
    return (uint8_t)((value<<4)|(value>>4));
}
static int pattern(void *ctx, const struct fb_rect *r, const uint8_t bits[8],
                   uint32_t fg, uint32_t bg)
{
    struct nv2d *s=ctx;
    if (!bits || !rectangle(s,r)) return -EOPNOTSUPP;
    uint32_t packed[]={bg,fg,0,0}, rop=0xf0;
    for (unsigned i=0;i<8;i++) packed[2+i/4]|=(uint32_t)reverse8(bits[i])<<(8*(i%4));
    /* Pattern bits are LSB-left at the native method; Ciuki is MSB-left.
     * Force opacity as XAA does, including the RGB555 reserved bit. */
    uint32_t opaque=s->fb.bpp==32 ? 0xff000000u : s->fb.green_size==5 ? 0xffff8000u : 0xffff0000u;
    packed[0]|=opaque; packed[1]|=opaque;
    int e=admission(s); if (e) return e;
    e=nv2d_emit(s,0x2300,1,&rop);
    if (!e) e=nv2d_emit(s,0x4310,4,packed);
    if (!e) e=nv2d_emit(s,0xc3fc,1,&fg);
    if (!e) e=draw_rect(s,r);
    return complete(s,e);
}
static int write_rows(void *ctx, const struct fb_rect *dst, const struct fb_rect *clip,
                      const void *rows, uint32_t stride)
{
    struct nv2d *s=ctx;
    /* Part A rows are already packed by swdib_convert, but row storage
     * retains its original (4 bytes/pixel) stride. Only the clip is accessed. */
    if (!rows || !dst || !rectangle(s,clip) || dst->width<=0 || dst->height<=0 ||
        clip->x<dst->x || clip->y<dst->y ||
        (int64_t)clip->x+clip->width>(int64_t)dst->x+dst->width ||
        (int64_t)clip->y+clip->height>(int64_t)dst->y+dst->height ||
        (uint64_t)(unsigned)dst->width*(s->fb.bpp/8)>stride ||
        (uint64_t)(unsigned)clip->width*(s->fb.bpp/8)>NV2D_SCRATCH_BYTES) return -EOPNOTSUPP;
    uint64_t last=(uint64_t)(clip->y-dst->y+clip->height-1)*stride+
        (uint64_t)(clip->x-dst->x+clip->width)*(s->fb.bpp/8);
    if (last>UINT32_MAX || (uintptr_t)rows>UINTPTR_MAX-(uintptr_t)last) return -EOPNOTSUPP;
    /* Reject known video-memory aliasing before the first write. Public
     * request staging additionally excludes all user framebuffer mappings. */
    uintptr_t source=(uintptr_t)rows, end=source+(uintptr_t)last;
    uintptr_t fb=(uintptr_t)s->fb.mapped, tail=(uintptr_t)s->tail;
    if ((source<fb+s->fb.size && end>fb) || (source<tail+s->tail_bytes && end>tail))
        return -EOPNOTSUPP;
    int e=admission(s); if (e) return e;
    e=nv2d_idle(s);
    if (!e) e=solid(s,0xcc);
    unsigned bytes=s->fb.bpp/8, n=(unsigned)clip->width*bytes;
    uint32_t srcpitch=(n+63)&~63u;
    uint32_t scratch[]={srcpitch|(s->fb.pitch<<16),s->scratch_offset};
    if (!e) e=nv2d_emit(s,0x304,2,scratch);
    for (int y=0;!e && y<clip->height;y++) {
        e=nv2d_idle(s); if (e) break;
        const uint8_t *src=(const uint8_t *)rows+
            (uint64_t)(clip->y-dst->y+y)*stride+(uint64_t)(clip->x-dst->x)*bytes;
        for (unsigned b=0;b<n;b++) s->tail[s->scratch_offset-(s->vram_bytes-s->tail_bytes)+b]=src[b];
        s->io.drain(s->io.ctx);
        e=blit(s,0,0,clip->x,clip->y+y,clip->width,1);
    }
    if (!e) {
        uint32_t restore[]={s->fb.pitch|(s->fb.pitch<<16),0};
        e=nv2d_emit(s,0x304,2,restore);
    }
    return complete(s,e);
}
static uint32_t rgb(struct nv2d *s, uint32_t native)
{
    unsigned sizes[]={s->fb.red_size,s->fb.green_size,s->fb.blue_size};
    unsigned positions[]={s->fb.red_pos,s->fb.green_pos,s->fb.blue_pos};
    uint32_t value=0;
    for (unsigned i=0;i<3;i++) {
        unsigned max=(1u<<sizes[i])-1, channel=(native>>positions[i])&max;
        value|=((channel*255+max/2)/max)<<(16-8*i);
    }
    return value;
}
static uint16_t cursor1555(uint32_t colour)
{ return 0x8000|((colour&0xf80000)>>9)|((colour&0xf800)>>6)|((colour&0xf8)>>3); }
static int set_cursor(void *ctx, const struct accel_cursor *cursor)
{
    struct nv2d *s=ctx;
    if (!cursor) return -EOPNOTSUPP;
    int e=admission(s); if (e) return e;
    e=nv2d_cursor_hide(s);
    if (e) return fault(s,"cursor_disable_failed",e);
    uint32_t fg=rgb(s,cursor->foreground), bg=rgb(s,cursor->background);
    unsigned width=s->alpha_cursor ? 64 : 32, bytes=s->alpha_cursor ? 4 : 2;
    volatile uint8_t *memory=s->tail+(s->cursor_offset-(s->vram_bytes-s->tail_bytes));
    for (unsigned y=0;y<width;y++) for (unsigned x=0;x<width;x++) {
        uint32_t pixel=0;
        if (x<32 && y<32 && (cursor->mask[y]&(0x80000000u>>x))) {
            uint32_t c=(cursor->image[y]&(0x80000000u>>x)) ? fg : bg;
            pixel=s->alpha_cursor ? c|0xff000000u : cursor1555(c);
        }
        unsigned offset=(y*width+x)*bytes;
        if (s->architecture==4) {
            /* The NV4 cursor lives in the donor's fixed PRAMIN window. */
            if (!(x&1)) {
                uint32_t other=0;
                if (x+1<32 && (cursor->mask[y]&(0x80000000u>>(x+1))))
                    other=cursor1555((cursor->image[y]&(0x80000000u>>(x+1))) ? fg : bg);
                WR(s,0x717800+offset,pixel|(other<<16));
            }
        } else for (unsigned b=0;b<bytes;b++) memory[offset+b]=(uint8_t)(pixel>>(8*b));
    }
    s->io.drain(s->io.ctx);
    uint8_t c0=s->architecture==4 ? 0 : 0x80|(s->cursor_offset>>17);
    uint8_t c1=s->architecture==4 ? 0xbc : (uint8_t)((s->cursor_offset>>11)<<2);
    uint8_t c2=s->architecture==4 ? 0 : s->cursor_offset>>24;
    cr_write(s,0x30,c0); cr_write(s,0x31,c1); cr_write(s,0x2f,c2);
    if (s->architecture>=0x10) {
        uint32_t config=0x100|(s->alpha_cursor ?
            ((s->device&0xfff0)==0x0110 ? 0x14011000 : 0x04011000) : 0x02000000);
        WR(s,0x600810+s->head_offset,config);
        uint32_t general=RD(s,0x680600+s->head_offset);
        WR(s,0x680600+s->head_offset,(general&~(1u<<29))|(s->alpha_cursor ? 1u<<29 : 0));
        if (RD(s,0x600810+s->head_offset)!=config) return fault(s,"cursor_config_failed",-EIO);
    }
    if (cr_read(s,0x30)!=c0 || cr_read(s,0x31)!=c1 || cr_read(s,0x2f)!=c2)
        return fault(s,"cursor_base_failed",-EIO);
    s->cursor_set=true;
    return 0;
}
static int move_cursor(void *ctx, int32_t x, int32_t y)
{
    struct nv2d *s=ctx;
    if (x < -32768 || x>32767 || y < -32768 || y>32767) return -EOPNOTSUPP;
    int e=admission(s); if (e) return e;
    if (!s->cursor_set) return -EINVAL;
    uint32_t position=(uint16_t)x|((uint32_t)(uint16_t)y<<16);
    WR(s,0x680300+s->head_offset,position);
    return RD(s,0x680300+s->head_offset)==position ? 0 : fault(s,"cursor_position_failed",-EIO);
}
static int show_cursor(void *ctx, bool show)
{
    struct nv2d *s=ctx;
    if (!show) {
        int e=nv2d_cursor_hide(s);
        return e ? fault(s,"cursor_disable_failed",e) : 0;
    }
    int e=admission(s); if (e) return e;
    if (!s->cursor_set) return -EINVAL;
    uint8_t control=cr_read(s,0x31);
    if (control==255) return fault(s,"cursor_enable_failed",-EIO);
    cr_write(s,0x31,control|1u);
    s->cursor_hidden=false;
    if (cr_read(s,0x31)!=(control|1u) || cursor_latch(s)) return fault(s,"cursor_enable_failed",-EIO);
    return 0;
}
static int sync_engine(void *ctx)
{
    struct nv2d *s=ctx;
    int e=nv2d_idle(s);
    if (!e && !s->quarantined && (RD(s,NV2D_FIFO_INTR) || RD(s,NV2D_GRAPH_INTR))) e=-EIO;
    return e ? fault(s,"idle_unproved",e) : 0;
}
static int stop(void *ctx)
{
    struct nv2d *s=ctx;
    s->active=false;
    if (!s->touched) { s->cpu_safe=true; return 0; }
    if (nv2d_cursor_hide(s) || nv2d_idle(s)) return fault(s,"stop_idle_unproved",-EIO);
    WR(s,0x3200,0); WR(s,0x3250,0); WR(s,0x2500,0); WR(s,0x3220,0);
    s->io.drain(s->io.ctx);
    if (RD(s,0x3200) || RD(s,0x3250) || RD(s,0x2500) || RD(s,0x3220) || RD(s,NV2D_GRAPH_BUSY))
        return fault(s,"stop_unproved",-EIO);
    /* Restore the firmware cursor's address/format, always hidden. */
    if (s->cursor_saved) {
        cr_write(s,0x30,s->saved_cursor[0]); cr_write(s,0x31,s->saved_cursor[1]&~1u);
        cr_write(s,0x2f,s->saved_cursor[2]);
        if (s->architecture>=0x10) {
            WR(s,0x600810+s->head_offset,s->saved_cursor_config);
            WR(s,0x680600+s->head_offset,s->saved_general);
        }
        cr_write(s,0x1f,s->saved_lock);
    }
    s->cursor_set=false; s->cpu_safe=true;
    return 0;
}
static unsigned capabilities(void *ctx)
{ struct nv2d *s=ctx; return s->active && !s->quarantined ? 3 : 0; }
static bool cpu_ready(void *ctx) { return ((struct nv2d *)ctx)->cpu_safe; }
static void set_pause(void *ctx, int (*pause)(void *), void *arg)
{
    struct nv2d *s=ctx;
    s->pause=pause; s->pause_ctx=arg; s->slice=s->io.now(s->io.ctx);
}
int nv2d_engine_init(struct nv2d *s, const struct nv2d_io *io, const struct fb_device *fb,
                     uint16_t device, uint32_t vram_bytes, volatile uint8_t *tail)
{
    if (!s || !io || !io->read || !io->write || !io->read8 || !io->write8 ||
        !io->now || !io->delay || !io->drain || !io->valid || !io->valid(io->ctx)) return -EOPNOTSUPP;
    device=nv2d_chipset(io,device);
    unsigned arch=nv2d_architecture(device);
    uint32_t reserved=nv2d_tail_bytes(arch);
    if (!nv2d_format(fb) || !tail || (uintptr_t)tail>UINTPTR_MAX-reserved || ((uintptr_t)tail&3) ||
        !nv2d_architecture(device) ||
        device==0x01a0 || device==0x01f0 || device==0x00a0 || (device&0xfff0)==0x0240 ||
        vram_bytes<reserved || (vram_bytes&(reserved-1)) || vram_bytes>256u*1024*1024 ||
        fb->size>vram_bytes-reserved) return -EOPNOTSUPP;
    if (s->quarantined) return -EIO;
    if (s->active || (s->touched && !s->cpu_safe)) return -EBUSY;
    if ((arch==4 && (fb->height>2048 || fb->width>(fb->bpp==32 ? 2032u : 2048u))) ||
        (arch>=0x10 && fb->width>(fb->bpp==32 ? 4080u : 4096u))) return -EOPNOTSUPP;
    memset(s,0,sizeof(*s));
    s->io=*io; s->fb=*fb; s->device=device; s->architecture=nv2d_architecture(device);
    s->tail=tail; s->vram_bytes=vram_bytes; s->tail_bytes=reserved;
    s->dma=arch>=0x30;
    s->scratch_offset=vram_bytes-reserved;
    s->cursor_offset=s->scratch_offset+0x8000;
    if (s->dma) {
        s->ring_offset=arch>=0x40 ? vram_bytes-560u*1024 : s->scratch_offset+0x4000;
        s->scratch_offset=s->ring_offset-0x4000;
        s->cursor_offset=s->ring_offset+0x8000;
        s->ring=(volatile uint32_t *)(tail+(s->ring_offset-(vram_bytes-reserved)));
    }
    s->alpha_cursor=s->architecture>=0x10 && (device&0xfff0)!=0x0100;
    s->cpu_safe=true; s->cursor_hidden=true; s->reason="pending";
    s->ops=(struct accel_ops){.ctx=s,.sync=sync_engine,.copy_rect=copy,.fill_rect=fill,
        .pattern_rect=pattern,.cursor_set=set_cursor,.cursor_move=move_cursor,
        .cursor_show=show_cursor,.stop=stop,.capabilities=capabilities,.cpu_ready=cpu_ready,
        .write_rect=write_rows,.set_pause=set_pause};
    /* Donor nv_dac.c selects exactly one graphics head with bit 12 and
     * CR44 owner 0/3. Preserve firmware routing; refuse disagreement rather
     * than moving an overlay on an unproved head. No CRTC mode writes. */
    unsigned impl=device&0xfff0;
    bool two_heads=s->architecture>=0x10 && impl!=0x0100 && impl!=0x0150 &&
        impl!=0x01a0 && impl!=0x0200;
    if (two_heads) {
        uint32_t h0=RD(s,0x600860), h1=RD(s,0x602860);
        unsigned owner=cr_read(s,0x44);
        if (h0!=UINT32_MAX && h1!=UINT32_MAX && (h0&0x1000) && !(h1&0x1000) && owner==0)
            s->head_offset=0;
        else if (h0!=UINT32_MAX && h1!=UINT32_MAX && !(h0&0x1000) && (h1&0x1000) && owner==3)
            s->head_offset=0x2000;
        else { s->reason="head_ownership_unproved"; return -EOPNOTSUPP; }
    }
    if (RD(s,0x600800+s->head_offset)) {
        s->reason="scanout_offset_unproved"; return -EOPNOTSUPP;
    }
    /* setup may fail before touching a firmware DMA engine. */
    int e=nv2d_setup(s);
    if (e) {
        if (s->touched) return fault(s,"setup_failed",e);
        s->reason="firmware_engine_busy"; return e;
    }
    s->saved_lock=cr_read(s,0x1f);
    cr_write(s,0x1f,0x57);
    s->saved_cursor[0]=cr_read(s,0x30); s->saved_cursor[1]=cr_read(s,0x31);
    s->saved_cursor[2]=cr_read(s,0x2f);
    s->saved_cursor_config=RD(s,0x600810+s->head_offset);
    s->saved_general=RD(s,0x680600+s->head_offset);
    s->cursor_saved=true;
    if (nv2d_cursor_hide(s)) return fault(s,"cursor_disable_failed",-EIO);
    s->active=true; s->reason="none";
    return 0;
}

/* Presenter + exclusive drawing ownership required. Reference rendering is
 * confined to RAM: every scanout write under test goes through native ops.
 * A pass establishes the optional legacy hooks before publishing them. */
int nv2d_compare(struct nv2d *s)
{
    if (s->fb.width<16 || s->fb.height<16) return -EOPNOTSUPP;
    int e=nv2d_idle(s);
    if (e) return fault(s,"comparison_idle_unproved",e);
    uint8_t saved[16*16*4], reference[16*16*4];
    uint32_t upload[16*16];
    unsigned pitch=16*(s->fb.bpp/8);
    struct fb_device ref=s->fb;
    ref.width=ref.height=16; ref.pitch=pitch; ref.size=pitch*16; ref.mapped=reference;
    for (unsigned y=0;y<16;y++) memcpy(saved+y*pitch,s->fb.mapped+y*s->fb.pitch,pitch);
    memcpy(reference,saved,ref.size);
    struct fb_rect block={0,0,16,16}, seed={0,0,4,4}, source={0,0,12,12};
    uint32_t c=swdib_pack(&s->fb,0x36a5e7), other=swdib_pack(&s->fb,0xcb7123);
    const uint8_t bits[]={0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81};
    for (unsigned op=0;op<5 && !e;op++) {
        s->io.drain(s->io.ctx);
        if (!op) {
            swdib_fill(&ref,&block,c); swdib_fill(&ref,&seed,other);
            e=fill(s,&block,c); if (!e) e=fill(s,&seed,other);
        } else if (op==1 || op==2) {
            if (op==2) source.x=source.y=4;
            int dst=op==1 ? 4 : 0;
            swdib_copy(&ref,&source,dst,dst);
            e=copy(s,&source,dst,dst);
        } else if (op==3) {
            swdib_pattern(&ref,&block,bits,c,other);
            e=pattern(s,&block,bits,c,other);
        } else {
            for (unsigned i=0;i<ARRAY_SIZE(upload);i++) upload[i]=i*34571u;
            swdib_convert(&ref,&block,upload,16*4);
            swdib_write(&ref,&block,&block,upload,16*4);
            e=write_rows(s,&block,&block,upload,16*4);
        }
        if (!e) e=nv2d_idle(s);
        if (!e) for (unsigned y=0;y<16;y++)
            if (memcmp(reference+y*pitch,s->fb.mapped+y*s->fb.pitch,pitch)) { e=-EIO; break; }
    }
    if (!nv2d_idle(s)) {
        for (unsigned y=0;y<16;y++) memcpy(s->fb.mapped+y*s->fb.pitch,saved+y*pitch,pitch);
        s->io.drain(s->io.ctx);
    } else e=-EIO;
    if (!e) {
        struct accel_cursor transparent={0};
        e=set_cursor(s,&transparent);
        if (!e) e=move_cursor(s,-1,-1);
        if (!e) e=show_cursor(s,true);
        int hidden=show_cursor(s,false);
        if (!e) e=hidden;
    }
    return e ? fault(s,"native_comparison_failed",e) : 0;
}
