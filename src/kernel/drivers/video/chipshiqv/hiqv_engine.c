/*
 * Copyright 1996, 1997, 1998 by David Bateman <dbateman@ee.uts.edu.au>
 *   Modified 1997, 1998 by Nozomi Ytow
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the authors not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  The authors makes no representations
 * about the suitability of this software for any purpose.  It is provided
 * "as is" without express or implied warranty.
 *
 * THE AUTHORS DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Copyright 1994  The XFree86 Project
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
 * DAVID WEXELBLAT BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Hardware Cursor for Trident utilizing XAA Cursor code.
 * Written by Alan Hourihane <alanh@fairlite.demon.co.uk>
 * Modified for Chips and Technologies by David Bateman <dbateman@eng.uts.edu.au>
 */
/* Adapted from X.Org xf86-video-chips src/ct_accel.c, ct_cursor.c and ct_BltHiQV.h,
 * commit 9187d76bd4f8d500fc3403b6d4a7bef55508bf2b. Original notices below.
 * CiukiOS adaptation: synchronous, bounded, no host data-port transfers. */
#include "hiqv.h"
#include <ciuki/swdib.h>

bool hiqv_mode_supported(const struct fb_device *d)
{
    if (!d || !d->present || !d->mapped || d->width < 16 || d->height < 16 ||
        d->width > 2048 || d->height > 2048 || d->pitch > 8191 ||
        (d->pitch & 7u) || (uint64_t)d->pitch*d->height > d->size ||
        d->size > HIQV_MMIO_OFFSET || d->pitch < d->width*(d->bpp/8u)) return false;
    if ((d->bpp!=16 && d->bpp!=24) || d->pitch%((d->bpp/8u)*8u)) return false;
    if (d->bpp==16) return d->green_pos==5 && d->green_size==6 && d->red_pos==11 &&
        d->red_size==5 && d->blue_pos==0 && d->blue_size==5;
    return d->red_pos==16 && d->green_pos==8 && d->blue_pos==0 &&
        d->red_size==8 && d->green_size==8 && d->blue_size==8;
}
static uint8_t xr(struct hiqv_engine *s, uint8_t reg)
{ return s->io.read_xr(s->io.ctx,reg); }
static void wxr(struct hiqv_engine *s, uint8_t reg, uint8_t value)
{ s->io.write_xr(s->io.ctx,reg,value); }
static void br(struct hiqv_engine *s, uint32_t reg, uint32_t value)
{ s->io.write_br(s->io.ctx,reg,value); }
static void drain(struct hiqv_engine *s) { s->io.drain(s->io.ctx); }
static bool valid(struct hiqv_engine *s) { return s->io.valid(s->io.ctx); }
static int hide(struct hiqv_engine *s)
{
    uint8_t a0=xr(s,0xa0);
    if (a0==255) { s->cursor_hidden=false; return -EIO; }
    wxr(s,0xa0,a0 & 0xf8u);
    s->cursor_hidden=(xr(s,0xa0)&7u)==0;
    return s->cursor_hidden ? 0 : -EIO;
}
/* Only XR20 is read: some 6555x lock the bus on BR register reads. Poll
 * pacing also avoids the donor's fast-poll busy-bit latch erratum. */
static int idle(struct hiqv_engine *s, bool slices)
{
    uint64_t start=s->io.now_us(s->io.ctx), slice=start;
    s->waits++;
    for (unsigned i=0;i<HIQV_WAIT_POLLS;i++) {
        uint8_t status=xr(s,0x20);
        if (status==255) return -EIO;
        if (!(status&3u)) {
            drain(s);
            s->cpu_safe=!s->quarantined || s->cursor_hidden;
            return 0;
        }
        uint64_t now=s->io.now_us(s->io.ctx);
        if (now-start>=HIQV_WAIT_US) return -ETIMEDOUT;
        if (slices && s->pause && (now-slice>=1000 || (i && !(i%200)))) {
            int e=s->pause(s->pause_arg);
            if (e) return e;
            if (!valid(s)) return -EIO;
            slice=s->io.now_us(s->io.ctx);
        }
        if (s->io.delay_us(s->io.ctx,5)) return -EIO;
    }
    return -ETIMEDOUT;
}
void hiqv_engine_quarantine(struct hiqv_engine *s, const char *why)
{
    s->active=false; s->quarantined=true; s->cpu_safe=false;
    s->reason=why;
    /* Nonwaiting: even an observed idle here would not authorize CPU access. */
    if (s->configured && valid(s)) (void)hide(s);
    else if (s->configured) s->cursor_hidden=false;
}
int hiqv_engine_recover(struct hiqv_engine *s)
{
    if (s->cpu_safe) return 0;
    if (s->stopped) { s->cpu_safe=true; return 0; }
    if (s->recovery_tried || !s->configured || !valid(s)) return -EIO;
    s->recovery_tried=true;
    int hidden=hide(s);
    if (!idle(s,true) && !hidden) return 0;
    uint8_t status=xr(s,0x20);
    if (status==255) return -EIO;
    /* One donor reset pulse. Always deassert even if delay/cancellation fails.
     * No GO, host FIFO write, PCI mastering, or bus ownership is involved. */
    wxr(s,0x20,(status&0x30u)|2u);
    int delay=0;
    for (unsigned i=0;i<10;i++) {
        if (s->io.delay_us(s->io.ctx,1000)) delay=-EIO;
        if (s->pause && s->pause(s->pause_arg)) delay=-EIO;
    }
    wxr(s,0x20,status&0x30u);
    hidden=hide(s);
    int e=idle(s,true);
    if (e || delay || hidden) { s->cpu_safe=false; return -EIO; }
    return 0;
}
static int fault(struct hiqv_engine *s, int e)
{
    if (e==-EOPNOTSUPP) e=-EIO; /* Refusal is impossible after touching hardware. */
    if (e==-ETIMEDOUT) s->timeouts++;
    hiqv_engine_quarantine(s,e==-ETIMEDOUT ? "engine_timeout" : "engine_fault");
    (void)hiqv_engine_recover(s);
    return e;
}
static bool rect(struct hiqv_engine *s,const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>0 && r->height>0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height;
}
static int begin(struct hiqv_engine *s)
{
    if (!s->active || s->quarantined) return -EIO;
    if (!valid(s)) return fault(s,-EIO);
    drain(s);
    int e=idle(s,true);
    return e ? fault(s,e) : 0;
}
static int go(struct hiqv_engine *s,unsigned height,unsigned bytes)
{
    s->cpu_safe=false;
    br(s,HIQV_GO,(height<<16)|bytes);
    int e=idle(s,true);
    if (e) return fault(s,e);
    s->operations++;
    return 0;
}
static int sync_engine(void *ctx)
{
    struct hiqv_engine *s=ctx;
    if (s->quarantined) return s->cpu_safe ? 0 : -EIO;
    if (!s->active) return s->cpu_safe ? 0 : -EIO;
    return begin(s);
}
static int fill(void *ctx,const struct fb_rect *r,uint32_t colour)
{
    struct hiqv_engine *s=ctx;
    if (!rect(s,r)) return -EOPNOTSUPP;
    int e=begin(s); if (e) return e;
    colour &= s->fb.bpp==16 ? 0xffffu : 0xffffffu;
    br(s,HIQV_BG,colour); br(s,HIQV_FG,colour);
    br(s,HIQV_ROP,0x000c00f0u); /* PATCOPY | PATSOLID | PATMONO */
    br(s,HIQV_PITCH,s->fb.pitch<<16);
    br(s,HIQV_DST,(uint32_t)r->y*s->fb.pitch+(uint32_t)r->x*(s->fb.bpp/8));
    return go(s,(unsigned)r->height,(unsigned)r->width*(s->fb.bpp/8));
}
static int copy(void *ctx,const struct fb_rect *r,int32_t x,int32_t y)
{
    struct hiqv_engine *s=ctx;
    struct fb_rect dst={x,y,r ? r->width : 0,r ? r->height : 0};
    if (!rect(s,r) || !rect(s,&dst)) return -EOPNOTSUPP;
    int e=begin(s); if (e) return e;
    unsigned bytes=s->fb.bpp/8, width=(unsigned)r->width*bytes;
    uint32_t source=(unsigned)r->y*s->fb.pitch+(unsigned)r->x*bytes;
    uint32_t dest=(unsigned)y*s->fb.pitch+(unsigned)x*bytes, rop=0xcc;
    if (y>r->y) {
        source+=((unsigned)r->height-1)*s->fb.pitch;
        dest+=((unsigned)r->height-1)*s->fb.pitch; rop|=0x200;
    }
    if (x>r->x) { source+=width-1; dest+=width-1; rop|=0x100; }
    br(s,HIQV_ROP,rop); br(s,HIQV_PITCH,(s->fb.pitch<<16)|s->fb.pitch);
    br(s,HIQV_SRC,source); br(s,HIQV_DST,dest);
    e=go(s,(unsigned)r->height,width);
    return e ? -EAGAIN : 0; /* Overlap must be reconstructed, never replayed. */
}
static int pattern(void *ctx,const struct fb_rect *r,const uint8_t p[8],uint32_t fg,uint32_t bg)
{
    struct hiqv_engine *s=ctx;
    if (!s->scratch || !p || !rect(s,r)) return -EOPNOTSUPP;
    int e=begin(s); if (e) return e;
    memcpy(s->scratch+HIQV_PATTERN_OFFSET,p,8); drain(s);
    br(s,HIQV_PATTERN,s->scratch_offset+HIQV_PATTERN_OFFSET);
    uint32_t mask=s->fb.bpp==16 ? 0xffffu : 0xffffffu;
    br(s,HIQV_BG,bg&mask); br(s,HIQV_FG,fg&mask);
    br(s,HIQV_MONO,0x04000000u); /* DWORDALIGN */
    br(s,HIQV_PITCH,(s->fb.pitch<<16)|1u);
    br(s,HIQV_DST,(unsigned)r->y*s->fb.pitch+(unsigned)r->x*(s->fb.bpp/8));
    br(s,HIQV_ROP,0x400f0u|(((unsigned)r->y&7u)<<20));
    return go(s,(unsigned)r->height,(unsigned)r->width*(s->fb.bpp/8));
}
/* Immutable staged native rows, matching the legacy WRITE hook. Upload via
 * a VRAM source avoids hardware waiting indefinitely for host-data writes. */
static int write_rows(void *ctx,const struct fb_rect *dst,const struct fb_rect *clip,
                      const void *rows,uint32_t stride)
{
    struct hiqv_engine *s=ctx;
    unsigned bytes=s->fb.bpp/8;
    if (!s->scratch || !rows || !rect(s,dst) || !rect(s,clip) ||
        clip->x<dst->x || clip->y<dst->y ||
        (int64_t)clip->x+clip->width>(int64_t)dst->x+dst->width ||
        (int64_t)clip->y+clip->height>(int64_t)dst->y+dst->height ||
        (uint64_t)(unsigned)dst->width*bytes>stride ||
        (uint64_t)stride*(unsigned)dst->height>UINT32_MAX ||
        (unsigned)clip->width*bytes>HIQV_SCRATCH_BYTES-HIQV_UPLOAD_OFFSET) return -EOPNOTSUPP;
    /* Reject aliasing with both CPU-mapped video allocations before submission. */
    uintptr_t addr=(uintptr_t)rows;
    uint64_t length=(uint64_t)stride*(unsigned)dst->height;
    if (length>UINTPTR_MAX-addr) return -EOPNOTSUPP;
    if ((addr<(uintptr_t)s->fb.mapped+s->fb.size && addr+length>(uintptr_t)s->fb.mapped) ||
        (addr<(uintptr_t)s->scratch+HIQV_SCRATCH_BYTES && addr+length>(uintptr_t)s->scratch)) return -EOPNOTSUPP;
    int e=begin(s); if (e) return e;
    for (int32_t y=0;y<clip->height;y++) {
        const uint8_t *source=(const uint8_t *)rows+(unsigned)(clip->y+y-dst->y)*stride+
                              (unsigned)(clip->x-dst->x)*bytes;
        memcpy(s->scratch+HIQV_UPLOAD_OFFSET,source,(unsigned)clip->width*bytes); drain(s);
        br(s,HIQV_ROP,0xcc); br(s,HIQV_PITCH,s->fb.pitch<<16);
        br(s,HIQV_SRC,s->scratch_offset+HIQV_UPLOAD_OFFSET);
        br(s,HIQV_DST,(unsigned)(clip->y+y)*s->fb.pitch+(unsigned)clip->x*bytes);
        e=go(s,1,(unsigned)clip->width*bytes); if (e) return e;
        if (s->pause && y+1<clip->height) {
            e=s->pause(s->pause_arg);
            if (e) return e==-EOPNOTSUPP ? -EIO : e;
            e=begin(s); if (e) return e;
        }
    }
    return 0;
}
static uint8_t component(uint32_t pixel,unsigned pos,unsigned bits)
{
    unsigned v=(pixel>>pos)&((1u<<bits)-1);
    return bits==5 ? (uint8_t)((v<<3)|(v>>2)) : bits==6 ? (uint8_t)((v<<2)|(v>>4)) : (uint8_t)v;
}
static int cursor_set(void *ctx,const struct accel_cursor *c)
{
    struct hiqv_engine *s=ctx;
    if (!s->scratch || !c) return -EOPNOTSUPP;
    int e=begin(s); if (e) return e;
    if (hide(s)) return fault(s,-EIO);
    /* Donor 64x64 mode: 8 AND bytes, then 8 XOR bytes per line, MSB-left.
     * Legacy opaque mask becomes AND=~mask, XOR=image&mask. */
    for (unsigned y=0;y<64;y++) for (unsigned b=0;b<8;b++) {
        s->scratch[y*16+b]=(y<32 && b<4) ? (uint8_t)(~c->mask[y]>>(24-b*8)) : 255;
        s->scratch[y*16+8+b]=(y<32 && b<4) ? (uint8_t)((c->image[y]&c->mask[y])>>(24-b*8)) : 0;
    }
    uint8_t colours[6];
    for (unsigned i=0;i<2;i++) {
        uint32_t p=i ? c->foreground : c->background;
        colours[i*3]=component(p,s->fb.red_pos,s->fb.red_size);
        colours[i*3+1]=component(p,s->fb.green_pos,s->fb.green_size);
        colours[i*3+2]=component(p,s->fb.blue_pos,s->fb.blue_size);
    }
    s->io.palette(s->io.ctx,colours); drain(s);
    wxr(s,0xa2,(s->scratch_offset>>8)&0xf0u); wxr(s,0xa3,(s->scratch_offset>>16)&0x3fu);
    if (xr(s,0xa2)!=((s->scratch_offset>>8)&0xf0u) ||
        xr(s,0xa3)!=((s->scratch_offset>>16)&0x3fu)) return fault(s,-EIO);
    s->cursor_loaded=true;
    return 0;
}
static int cursor_move(void *ctx,int32_t x,int32_t y)
{
    struct hiqv_engine *s=ctx;
    if (!s->scratch || x<-2047 || y<-2047 || x>2047 || y>2047) return -EOPNOTSUPP;
    if (!s->cursor_loaded) return -EINVAL;
    int e=begin(s); if (e) return e;
    unsigned ux=x<0 ? (unsigned)(-x)|0x8000u : (unsigned)x;
    unsigned uy=y<0 ? (unsigned)(-y)|0x8000u : (unsigned)y;
    const uint8_t values[4]={ux&255u,(ux>>8)&0x87u,uy&255u,(uy>>8)&0x87u};
    for (unsigned i=0;i<4;i++) wxr(s,0xa4+i,values[i]);
    for (unsigned i=0;i<4;i++) if (xr(s,0xa4+i)!=values[i]) return fault(s,-EIO);
    return 0;
}
static int cursor_show(void *ctx,bool show)
{
    struct hiqv_engine *s=ctx;
    if (!show && s->quarantined) return s->cursor_hidden ? 0 : -EIO;
    if (!s->scratch) return -EOPNOTSUPP;
    if (show && !s->cursor_loaded) return -EINVAL;
    int e=begin(s); if (e) return e;
    if (!show) return hide(s) ? fault(s,-EIO) : 0;
    uint8_t value=(xr(s,0xa0)&0xf0u)|5u;
    wxr(s,0xa0,value);
    if (xr(s,0xa0)!=value) return fault(s,-EIO);
    s->cursor_hidden=false;
    /* A visible cursor is safe during ordinary CPU drawing; disable proof is
     * required only on handoff/quarantine, not while the driver owns it. */
    s->cpu_safe=true;
    return 0;
}
static int stop_engine(void *ctx)
{
    struct hiqv_engine *s=ctx;
    s->active=false; s->cpu_safe=false;
    if (!s->configured) { s->cpu_safe=true; return 0; }
    if (!valid(s) || hide(s)) return fault(s,-EIO);
    int e=idle(s,true);
    if (e) { hiqv_engine_quarantine(s,"stop_timeout"); e=hiqv_engine_recover(s); }
    if (e) return e;
    wxr(s,0x20,s->saved20&0x30u);
    if ((xr(s,0x20)&0x33u)!=(s->saved20&0x30u)) return fault(s,-EIO);
    s->configured=false; s->stopped=true; s->reason=s->quarantined ? s->reason : "stopped";
    return 0;
}
static unsigned capabilities(void *ctx)
{ struct hiqv_engine *s=ctx; return s->active && !s->quarantined ? 1u|(s->scratch ? 2u : 0) : 0; }
static bool cpu_ready(void *ctx) { return ((struct hiqv_engine *)ctx)->cpu_safe; }
static void set_pause(void *ctx,int (*pause)(void *),void *arg)
{ struct hiqv_engine *s=ctx; s->pause=pause; s->pause_arg=arg; }
int hiqv_engine_open(struct hiqv_engine *s,const struct hiqv_io *io,
                     const struct fb_device *fb,uint8_t *scratch,uint32_t offset)
{
    if (!s || !io || !io->read_xr || !io->write_xr || !io->write_br || !io->palette ||
        !io->now_us || !io->delay_us || !io->drain || !io->valid || !hiqv_mode_supported(fb) ||
        (scratch && ((offset&(PAGE_SIZE-1)) || offset<fb->size ||
         offset>HIQV_MMIO_OFFSET-HIQV_SCRATCH_BYTES))) return -EINVAL;
    memset(s,0,sizeof(*s)); s->io=*io; s->fb=*fb; s->scratch=scratch; s->scratch_offset=offset;
    s->cpu_safe=true; s->stopped=true; s->reason="pending";
    s->ops=(struct accel_ops){.ctx=s,.sync=sync_engine,.copy_rect=copy,.fill_rect=fill,
        .pattern_rect=scratch ? pattern : 0,.cursor_set=scratch ? cursor_set : 0,
        .cursor_move=scratch ? cursor_move : 0,.cursor_show=scratch ? cursor_show : 0,
        .stop=stop_engine,.capabilities=capabilities,.cpu_ready=cpu_ready,
        .write_rect=scratch ? write_rows : 0,.set_pause=set_pause};
    return 0;
}
int hiqv_engine_activate(struct hiqv_engine *s)
{
    if (s->quarantined || s->configured || !valid(s)) return -EIO;
    s->cpu_safe=false; s->stopped=false; s->saved20=xr(s,0x20); s->configured=true;
    if (hide(s)) return fault(s,-EIO);
    int e=idle(s,true); if (e) return fault(s,e);
    uint8_t depth=s->fb.bpp==16 ? 0x10 : 0x20;
    wxr(s,0x20,depth);
    if ((xr(s,0x20)&0x33u)!=depth) return fault(s,-EIO);
    s->active=true; s->reason="none";
    return 0;
}
int hiqv_cursor_check(const struct accel_ops *ops,uint64_t deadline,bool (*cancel)(void *),void *ctx)
{
    if (!ops || !ops->cursor_set) return -EOPNOTSUPP;
    struct accel_cursor transparent={0};
    int e=0;
    for (unsigned i=0;i<4 && !e;i++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!i) e=ops->cursor_set(ops->ctx,&transparent);
        else if (i==1) e=ops->cursor_move(ops->ctx,0,0);
        else e=ops->cursor_show(ops->ctx,i==2);
    }
    int hidden=ops->cursor_show(ops->ctx,false);
    return e ? e : hidden;
}
int hiqv_engine_check(struct hiqv_engine *s)
{
    int e=hiqv_engine_activate(s); if (e) return e;
    uint8_t saved[16*16*3], pixels[16*16*3], upload[16*16*3];
    unsigned bytes=s->fb.bpp/8, pitch=16*bytes;
    for (unsigned y=0;y<16;y++) memcpy(saved+y*pitch,s->fb.mapped+y*s->fb.pitch,pitch);
    memcpy(pixels,saved,16*pitch);
    struct fb_device ref=s->fb; ref.mapped=pixels; ref.width=ref.height=16;
    ref.pitch=pitch; ref.size=16*pitch;
    struct fb_rect block={0,0,16,16}, seed={0,0,4,4};
    uint32_t fg=swdib_pack(&ref,0x37a5e1),bg=swdib_pack(&ref,0xc1582b);
    for (unsigned op=0;op<5 && !e;op++) {
        if (!op) {
            swdib_fill(&ref,&block,fg); swdib_fill(&ref,&seed,bg);
            e=fill(s,&block,fg); if (!e) e=fill(s,&seed,bg);
        } else if (op<3) {
            struct fb_rect from={op==1 ? 0 : 4,op==1 ? 0 : 4,12,12};
            int to=op==1 ? 4 : 0;
            swdib_copy(&ref,&from,to,to); e=copy(s,&from,to,to);
        } else if (op==3 && s->scratch) {
            const uint8_t pat[8]={0x81,0x42,0x24,0x18,0x18,0x24,0x42,0x81};
            swdib_pattern(&ref,&block,pat,fg,bg); e=pattern(s,&block,pat,fg,bg);
        } else if (op==4 && s->scratch) {
            for (unsigned i=0;i<16*pitch;i++) upload[i]=(uint8_t)(i*71+13);
            swdib_write(&ref,&block,&block,upload,pitch); e=write_rows(s,&block,&block,upload,pitch);
        }
        if (!e) for (unsigned y=0;y<16;y++)
            if (memcmp(pixels+y*pitch,s->fb.mapped+y*s->fb.pitch,pitch)) { e=-EIO; break; }
    }
    if (!e && s->scratch) e=hiqv_cursor_check(&s->ops,g_ticks+100,0,0);
    if (e && !s->quarantined) hiqv_engine_quarantine(s,"activation_comparison_failed");
    if (s->quarantined) (void)hiqv_engine_recover(s);
    if (s->cpu_safe && s->cursor_hidden) {
        for (unsigned y=0;y<16;y++) memcpy(s->fb.mapped+y*s->fb.pitch,saved+y*pitch,pitch);
        drain(s);
    } else e=-EIO;
    return e;
}
