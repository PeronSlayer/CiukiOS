/* Origin: Linux v6.12 drivers/video/fbdev/neofb.c, neo2200_* engine
 * fragments; CiukiOS glue replaces fbdev, palette and unbounded waits. */
/*
 * linux/drivers/video/neofb.c -- NeoMagic Framebuffer Driver
 *
 * Copyright (c) 2001-2002  Denis Oliver Kropp <dok@directfb.org>
 *
 *
 * Card specific code is based on XFree86's neomagic driver.
 * Framebuffer framework code is based on code of cyber2000fb.
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file COPYING in the main directory of this
 * archive for more details.
 *
 *
 * 0.4.1
 *  - Cosmetic changes (dok)
 *
 * 0.4
 *  - Toshiba Libretto support, allow modes larger than LCD size if
 *    LCD is disabled, keep BIOS settings if internal/external display
 *    haven't been enabled explicitly
 *                          (Thomas J. Moore <dark@mama.indstate.edu>)
 *
 * 0.3.3
 *  - Porting over to new fbdev api. (jsimmons)
 *
 * 0.3.2
 *  - got rid of all floating point (dok)
 *
 * 0.3.1
 *  - added module license (dok)
 *
 * 0.3
 *  - hardware accelerated clear and move for 2200 and above (dok)
 *  - maximum allowed dotclock is handled now (dok)
 *
 * 0.2.1
 *  - correct panning after X usage (dok)
 *  - added module and kernel parameters (dok)
 *  - no stretching if external display is enabled (dok)
 *
 * 0.2
 *  - initial version (dok)
 *
 *
 * TODO
 * - ioctl for internal/external switching
 * - blanking
 * - 32bit depth support, maybe impossible
 * - disable pan-on-sync, need specs
 *
 * BUGS
 * - white margin on bootup like with tdfxfb (colormap problem?)
 *
 */

#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "neo.h"

bool neo_chip(uint16_t id)
{ return id==0x0005 || id==0x0025 || id==0x0006 || id==0x0016; }
uint32_t neo_vram(uint16_t id)
{
    switch (id) {
    case 0x0005: return 2560u*1024;
    case 0x0025: return 3008u*1024;
    case 0x0006: return 4096u*1024;
    case 0x0016: return 6144u*1024;
    default: return 0;
    }
}
bool neo_geometry(const struct fb_device *d)
{
    return d && d->present && d->mapped && (d->bpp==16 || d->bpp==24) &&
        d->width>=16 && d->width<=4096 && d->height>=16 && d->height<=4096 &&
        d->pitch && d->pitch<=65535 && d->pitch>=d->width*(d->bpp/8u) &&
        (uint64_t)d->pitch*d->height<=d->size &&
        (uintptr_t)d->mapped<=UINTPTR_MAX-d->size &&
        d->red_pos==(d->bpp==16 ? 11 : 16) && d->red_size==(d->bpp==16 ? 5 : 8) &&
        d->green_pos==(d->bpp==16 ? 5 : 8) && d->green_size==(d->bpp==16 ? 6 : 8) &&
        d->blue_pos==0 && d->blue_size==(d->bpp==16 ? 5 : 8) && !d->rsvd_size;
}
static bool current(struct neo_engine *s)
{ return s->active && !s->quarantined; }
static void wr(struct neo_engine *s,unsigned reg,uint32_t value)
{ s->touched=true; s->io.write(s->io.ctx,reg,value); }

/* Busy-only polling is the donor's working FIFO alternative. Never enable
 * master BLT or host DMA, never reset a live engine. A dead all-ones read
 * cannot establish idle. Pauses keep the presenter's drawing claim pinned. */
int neo_idle(struct neo_engine *s)
{
    uint64_t first=s->io.now(s->io.ctx), slice=first;
    s->waits++;
    for (unsigned i=0;i<NEO_POLL_LIMIT;i++) {
        if (s->checking && s->io.now(s->io.ctx)-s->check_since>=NEO_CHECK_US) break;
        uint32_t status=s->io.read(s->io.ctx,NEO_STAT);
        uint32_t mode=(s->fb.bpp==16 ? NEO_MODE1_DEPTH16 : NEO_MODE1_DEPTH24)<<16;
        uint32_t pitch=s->fb.pitch<<16|s->fb.pitch;
        if (status!=UINT32_MAX && !(status&NEO_BS0_BLT_BUSY) &&
            (!s->configured || ((status&0xffff0000u)==mode &&
                s->io.read(s->io.ctx,NEO_PITCH)==pitch))) {
            s->cpu_safe=s->cursor_safe;
            return 0;
        }
        s->cpu_safe=false;
        uint64_t now=s->io.now(s->io.ctx);
        if (now-first>=NEO_TIMEOUT_US) break;
        if (s->pause && now-slice>=1000) {
            int e=s->pause(s->pause_ctx);
            if (e) { s->cpu_safe=false; return e; }
            slice=s->io.now(s->io.ctx);
        }
    }
    s->cpu_safe=false;
    s->timeouts++;
    return -ETIMEDOUT;
}
void neo_quarantine(struct neo_engine *s,const char *reason)
{
    s->quarantined=true; s->active=false; s->reason=reason;
    /* Quarantine is not idle proof. Cursor disable also needs independent
     * proof: MMIO can disappear (explicit X.Org NeoHideCursor warning). */
    s->cpu_safe=false;
    (void)neo_hide(s);
}
static int failed(struct neo_engine *s,int error)
{ neo_quarantine(s,"engine_fault"); return error; }
static int sync_engine(void *ctx)
{
    struct neo_engine *s=ctx;
    if (s->quarantined) return s->cpu_safe ? 0 : -EIO;
    if (!s->active && s->cpu_safe) return 0;
    int e=neo_idle(s);
    return e ? failed(s,e) : 0;
}
static bool rectangle(const struct neo_engine *s,const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>=0 && r->height>=0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height;
}
static int prepare(struct neo_engine *s)
{
    if (!current(s)) return -ENODEV;
    int e=neo_idle(s);
    if (e) return failed(s,e);
    s->io.drain(s->io.ctx);
    return 0;
}
static void launch(struct neo_engine *s,unsigned w,unsigned h)
{
    s->cpu_safe=false; s->submitted=true;
    wr(s,NEO_EXTENT,h<<16|w);
}
static int complete(struct neo_engine *s)
{
    int e=neo_idle(s);
    if (e) return failed(s,e);
    s->operations++;
    return 0;
}
static int solid(void *ctx,const struct fb_rect *r,uint32_t colour,uint32_t rop)
{
    struct neo_engine *s=ctx;
    if (!rectangle(s,r)) return -EOPNOTSUPP;
    if (!current(s)) return -ENODEV;
    if (!r->width || !r->height) return 0;
    int e=prepare(s); if (e) return e;
    wr(s,NEO_CONTROL,NEO_BC3_FIFO_EN|NEO_BC0_SRC_IS_FG|NEO_BC3_SKIP_MAPPING|rop);
    wr(s,NEO_FG,colour);
    wr(s,NEO_DST,(uint32_t)r->y*s->fb.pitch+(uint32_t)r->x*(s->fb.bpp/8u));
    launch(s,(unsigned)r->width,(unsigned)r->height);
    return complete(s);
}
static int fill(void *ctx,const struct fb_rect *r,uint32_t colour)
{ return solid(ctx,r,colour,0x0c0000); }
int neo_invert(void *ctx,const struct fb_rect *r)
{
    struct neo_engine *s=ctx;
    return solid(ctx,r,s->fb.bpp==16 ? 0xffff : 0xffffff,0x060000);
}
static int copy(void *ctx,const struct fb_rect *r,int32_t x,int32_t y)
{
    struct neo_engine *s=ctx;
    if (!r) return -EOPNOTSUPP;
    struct fb_rect d={x,y,r->width,r->height};
    if (!rectangle(s,r) || !rectangle(s,&d)) return -EOPNOTSUPP;
    if (!current(s)) return -ENODEV;
    if (!r->width || !r->height) return 0;
    unsigned sx=(unsigned)r->x, sy=(unsigned)r->y, dx=(unsigned)x, dy=(unsigned)y;
    uint32_t control=NEO_BC3_FIFO_EN|NEO_BC3_SKIP_MAPPING|0x0c0000;
    if (dy>sy || (dy==sy && dx>sx)) {
        sx+=(unsigned)r->width-1; dx+=(unsigned)r->width-1;
        sy+=(unsigned)r->height-1; dy+=(unsigned)r->height-1;
        control|=NEO_BC0_X_DEC|NEO_BC0_DST_Y_DEC|NEO_BC0_SRC_Y_DEC;
    }
    int e=prepare(s); if (e) return e;
    wr(s,NEO_CONTROL,control);
    wr(s,NEO_SRC,sy*s->fb.pitch+sx*(s->fb.bpp/8u));
    wr(s,NEO_DST,dy*s->fb.pitch+dx*(s->fb.bpp/8u));
    launch(s,(unsigned)r->width,(unsigned)r->height);
    return complete(s);
}
/* Finite host source: prevalidate the entire call; submit one <=256-pixel
 * scanline with dword padding, feed it completely before any sliced wait.
 * No pageable/user pointer, allocation or yield exists between trigger and
 * the last payload word. This never sets NEO_BC1_MSTR_BLT. */
static int write_rows(void *ctx,const struct fb_rect *dst,const struct fb_rect *clip,
                      const void *rows,uint32_t stride)
{
    struct neo_engine *s=ctx;
    if (!dst || !clip || !rows || dst->width<0 || dst->height<0 ||
        !rectangle(s,clip) || clip->width>256 || clip->x<dst->x || clip->y<dst->y ||
        (int64_t)clip->x+clip->width>(int64_t)dst->x+dst->width ||
        (int64_t)clip->y+clip->height>(int64_t)dst->y+dst->height ||
        (uint64_t)(uint32_t)dst->width*(s->fb.bpp/8u)>stride) return -EOPNOTSUPP;
    uint64_t extent=(uint64_t)(uint32_t)dst->height*stride;
    if (extent>UINT32_MAX || (uintptr_t)rows>UINTPTR_MAX-extent) return -EOPNOTSUPP;
    if (!current(s)) return -ENODEV;
    if (!clip->width || !clip->height) return 0;
    unsigned bytes=s->fb.bpp/8u, count=(unsigned)clip->width*bytes;
    uint32_t payload[192];
    const uint8_t *src=(const uint8_t *)rows+(uint32_t)((int64_t)clip->y-dst->y)*stride+
        (uint32_t)((int64_t)clip->x-dst->x)*bytes;
    for (unsigned y=0;y<(unsigned)clip->height;y++) {
        memset(payload,0,sizeof(payload));
        memcpy(payload,src+y*stride,count);
        int e=prepare(s); if (e) return e;
        wr(s,NEO_CONTROL,NEO_BC0_SYS_TO_VID|NEO_BC3_SKIP_MAPPING|0x0c0000);
        wr(s,NEO_SRC,0);
        wr(s,NEO_DST,((unsigned)clip->y+y)*s->fb.pitch+(unsigned)clip->x*bytes);
        launch(s,(unsigned)clip->width,1);
        for (unsigned i=0;i<(count+3)/4;i++) wr(s,NEO_UPLOAD+i*4,payload[i]);
        e=complete(s); if (e) return e;
    }
    return 0;
}
static bool cpu_ready(void *ctx) { return ((struct neo_engine *)ctx)->cpu_safe; }
static unsigned capabilities(void *ctx)
{
    struct neo_engine *s=ctx;
    return current(s) ? 1u|(s->cursor ? 2u : 0u) : 0;
}
static void set_pause(void *ctx,int (*pause)(void *),void *arg)
{ struct neo_engine *s=ctx; s->pause=pause; s->pause_ctx=arg; }
int neo_stop(void *ctx)
{
    struct neo_engine *s=ctx;
    s->active=false;
    int hidden=neo_hide(s), idle=neo_idle(s);
    if (hidden || idle) { neo_quarantine(s,"stop_unproved"); return -EIO; }
    s->loaded=false;
    return 0;
}
int neo_mono(void *ctx,const struct fb_rect *r,const void *rows,uint32_t stride,
             uint32_t foreground,uint32_t background,bool transparent)
{
    struct neo_engine *s=ctx;
    if (!rectangle(s,r) || r->width>256 || transparent || !rows ||
        stride<((unsigned)r->width+7)/8) return -EOPNOTSUPP;
    if (!current(s)) return -ENODEV;
    if (!r->width || !r->height) return 0;
    /* neofb.c:1443-1452: insufficient padding on narrow 24bpp MONO.
     * This clean refusal occurs before even a register read or cursor hide. */
    if (s->fb.bpp==24 && r->width<16) return -EOPNOTSUPP;
    uint64_t extent=(uint64_t)(unsigned)r->height*stride;
    if (extent>UINT32_MAX || (uintptr_t)rows>UINTPTR_MAX-extent) return -EOPNOTSUPP;
    unsigned count=((unsigned)r->width+7)/8;
    for (unsigned y=0;y<(unsigned)r->height;y++) {
        uint32_t payload[8]={0};
        memcpy(payload,(const uint8_t *)rows+y*stride,count);
        int e=prepare(s); if (e) return e;
        wr(s,NEO_FG,foreground); wr(s,NEO_BG,background);
        wr(s,NEO_CONTROL,NEO_BC0_SYS_TO_VID|NEO_BC0_SRC_MONO|NEO_BC3_SKIP_MAPPING|0x0c0000);
        wr(s,NEO_SRC,0);
        wr(s,NEO_DST,((unsigned)r->y+y)*s->fb.pitch+(unsigned)r->x*(s->fb.bpp/8u));
        launch(s,(unsigned)r->width,1);
        for (unsigned i=0;i<(count+3)/4;i++) wr(s,NEO_UPLOAD+i*4,payload[i]);
        e=complete(s); if (e) return e;
    }
    return 0;
}
int neo_setup(struct neo_engine *s,const struct neo_io *io,const struct fb_device *d,
              uint8_t *cursor,uint32_t offset)
{
    if (!s || !io || !io->read || !io->write || !io->now || !io->drain || !io->hide ||
        !neo_geometry(d) || s->quarantined || (cursor && ((offset&1023u) ||
        offset<d->size || offset>=0x400000u))) return -EOPNOTSUPP;
    *s=(struct neo_engine){.io=*io,.fb=*d,.cursor=cursor,.cursor_offset=offset,.reason="pending"};
    s->ops=(struct accel_ops){.ctx=s,.sync=sync_engine,.copy_rect=copy,.fill_rect=fill,
        .cursor_set=neo_cursor_set,.cursor_move=neo_cursor_move,.cursor_show=neo_cursor_show,
        .stop=neo_stop,.capabilities=capabilities,.cpu_ready=cpu_ready,.write_rect=write_rows,
        .set_pause=set_pause};
    /* No pixel command in setup; fbdev_accel_probe owns all writes/DM9. */
    return 0;
}
static bool compare(const struct neo_engine *s,const uint8_t *reference,unsigned side)
{
    unsigned pitch=side*(s->fb.bpp/8u);
    for (unsigned y=0;y<side;y++)
        for (unsigned x=0;x<pitch;x++)
            if (((volatile uint8_t *)s->fb.mapped)[y*s->fb.pitch+x]!=reference[y*pitch+x]) return false;
    return true;
}
/* Native-only, saved-block comparison at activation. Each command has a
 * separate reference result and readback; success cannot be software replay.
 * Local block is small enough for the kernel's 4 KiB frame budget. */
int neo_dm9(void *ctx)
{
    struct neo_engine *s=ctx;
    uint8_t saved[16*16*3], expected[sizeof(saved)];
    uint32_t upload[16*4];
    struct fb_device ref=s->fb;
    ref.width=ref.height=16; ref.pitch=16*(ref.bpp/8u); ref.size=16*ref.pitch; ref.mapped=expected;
    s->checking=true; s->check_since=s->io.now(s->io.ctx);
    int e=neo_hide(s);
    if (e || (e=neo_idle(s))) { s->checking=false; return failed(s,e); }
    for (unsigned y=0;y<16;y++)
        for (unsigned x=0;x<ref.pitch;x++)
            saved[y*ref.pitch+x]=((volatile uint8_t *)s->fb.mapped)[y*s->fb.pitch+x];
    memcpy(expected,saved,ref.size);
    uint32_t mode=(s->fb.bpp==16 ? NEO_MODE1_DEPTH16 : NEO_MODE1_DEPTH24)<<16;
    uint32_t pitch=s->fb.pitch<<16|s->fb.pitch;
    wr(s,NEO_STAT,mode); wr(s,NEO_PITCH,pitch);
    if ((s->io.read(s->io.ctx,NEO_STAT)&0xffff0000u)!=mode ||
        s->io.read(s->io.ctx,NEO_PITCH)!=pitch) e=-EIO;
    s->configured=!e;
    s->active=!e;
    struct fb_rect block={0,0,16,16}, seed={1,1,4,4}, region={0,0,12,12};
    uint32_t a=swdib_pack(&ref,0x369ae7), b=swdib_pack(&ref,0xc75123);
    if (!e) { swdib_fill(&ref,&block,a); e=fill(s,&block,a); }
    if (!e && !compare(s,expected,16)) e=-EIO;
    if (!e) { swdib_fill(&ref,&seed,b); e=fill(s,&seed,b); }
    if (!e && !compare(s,expected,16)) e=-EIO;
    if (!e) { swdib_copy(&ref,&region,4,4); e=copy(s,&region,4,4); }
    if (!e && !compare(s,expected,16)) e=-EIO;
    region.x=region.y=4;
    if (!e) { swdib_copy(&ref,&region,0,0); e=copy(s,&region,0,0); }
    if (!e && !compare(s,expected,16)) e=-EIO;
    if (!e) {
        for (unsigned i=0;i<ARRAY_SIZE(upload);i++) upload[i]=i*0x10371u;
        struct fb_rect band={0,0,16,4};
        swdib_convert(&ref,&band,upload,64);
        swdib_write(&ref,&band,&band,upload,64);
        e=write_rows(s,&band,&band,upload,64);
    }
    if (!e && !compare(s,expected,16)) e=-EIO;
    if (!e) {
        /* Private optional primitive: XOR twice must recover the exact block. */
        e=neo_invert(s,&block);
        if (!e) {
            for (unsigned i=0;i<ref.size;i++) expected[i]^=255;
            if (!compare(s,expected,16)) e=-EIO;
        }
        if (!e) e=neo_invert(s,&block);
        if (!e) {
            for (unsigned i=0;i<ref.size;i++) expected[i]^=255;
            if (!compare(s,expected,16)) e=-EIO;
        }
    }
    if (!e) {
        const uint8_t bits[4]={0x81,0x80,0x5a,0xa5};
        struct fb_rect mono={0,0,16,2};
        for (unsigned y=0;y<2;y++) for (unsigned x=0;x<16;x++) {
            struct fb_rect pixel={(int32_t)x,(int32_t)y,1,1};
            swdib_fill(&ref,&pixel,(bits[y*2+x/8]&(0x80u>>(x%8))) ? a : b);
        }
        e=neo_mono(s,&mono,bits,2,a,b,false);
        if (!e && !compare(s,expected,16)) e=-EIO;
    }
    if (!e && s->cursor) {
        struct accel_cursor transparent={0};
        e=neo_cursor_set(s,&transparent);
        if (!e) e=neo_cursor_move(s,0,0);
        if (!e) e=neo_cursor_show(s,true);
        if (!e) e=neo_cursor_show(s,false);
    }
    /* Restore only after independent idle + cursor-disable proof. */
    s->checking=false;
    int hidden=neo_hide(s), idle=neo_idle(s);
    if (!hidden && !idle && s->cpu_safe) {
        for (unsigned y=0;y<16;y++)
            for (unsigned x=0;x<ref.pitch;x++)
                ((volatile uint8_t *)s->fb.mapped)[y*s->fb.pitch+x]=saved[y*ref.pitch+x];
        s->io.drain(s->io.ctx);
    } else if (!e) e=-EIO;
    if (e) { neo_quarantine(s,"dm9_failed"); return e; }
    s->reason="none";
    return 0;
}
