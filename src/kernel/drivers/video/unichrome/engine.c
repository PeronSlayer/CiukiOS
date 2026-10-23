/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright 1998-2008 VIA Technologies, Inc. All Rights Reserved.
 * Copyright 2001-2008 S3 Graphics, Inc. All Rights Reserved.
 * Copyright 2010 Florian Tobias Schandinat <FlorianSchandinat@gmx.de>
 * Origin: Linux v6.12 drivers/video/fbdev/via/accel.c: viafb_set_bpp,
 * hw_bitblt_1 and viafb_wait_engine_idle; via_modesetting.c scanout packing.
 * Native validation, synchronous completion and staging are CiukiOS glue.
 *
 * MMIO-enable / queue-disable sequences: OpenChrome via_ums.c,
 * viaMapMMIO, viaMMIOEnable and viaDisableVQ,
 * NetBSD/xsrc 947c2727683984b334ebe249fd981dad3e7a2f44,
 * external/mit/xf86-video-openchrome/dist/src/.
 * Copyright 2011-2016 The OpenChrome Project
 *                     [https://www.freedesktop.org/wiki/Openchrome]
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "engine.h"
#include "regs.h"

static uint32_t rd(struct uc_engine *e, unsigned r) { return e->io.read(e->io.ctx,r); }
static void wr(struct uc_engine *e, unsigned r, uint32_t v) { e->io.write(e->io.ctx,r,v); }
static bool valid(struct uc_engine *e)
{
    return e->fb && e->fb->generation==e->generation && e->io.valid(e->io.ctx);
}
static int hide(struct uc_engine *e)
{
    uint32_t cursor=rd(e,UC_CURSOR);
    if (cursor==UINT32_MAX) return -EIO;
    if (cursor&1u) wr(e,UC_CURSOR,cursor&~1u);
    cursor=rd(e,UC_CURSOR);
    return cursor==UINT32_MAX || (cursor&1u) ? -EIO : 0;
}
void uc_engine_quarantine(struct uc_engine *e, const char *reason)
{
    e->quarantined=true; e->active=false; e->verified_write=false;
    e->reason=reason ? reason : "engine_fault";
    /* Hiding is independent of engine completion. Neither action is an idle
     * proof. A failed cursor disable also forbids CPU/software ownership. */
    if (e->touched && (!valid(e) || hide(e))) e->cpu_safe=false;
}
static int idle(struct uc_engine *e, bool sliced)
{
    uint64_t start=e->io.now(e->io.ctx), slice=start;
    for (unsigned n=0;n<UC_POLL_LIMIT;n++) {
        e->status=rd(e,UC_STATUS);
        if (e->status==UINT32_MAX) { e->cpu_safe=false; return -EIO; }
        /* Original-engine queue-empty bit is positive polarity (the Linux
         * header calls it BUSY, but both donors wait for it to become one).
         * A clear engine-busy mask alone cannot prove a drained queue. */
        if ((e->status&UC_QUEUE_EMPTY) && !(e->status&UC_BUSY)) {
            if (e->touched && hide(e)) { e->cpu_safe=false; return -EIO; }
            e->cpu_safe=true;
            return 0;
        }
        e->cpu_safe=false;
        uint64_t now=e->io.now(e->io.ctx);
        if (now-start>=UC_WAIT_US) break;
        if (sliced && e->pause && (now-slice>=1000 || (n && !(n&255u)))) {
            int err=e->pause(e->pause_ctx);
            if (err) return err;
            /* A pause may release the presenter. Claims/generation remain
             * pinned by fbdev, but verify before another MMIO access. */
            if (!valid(e)) return -EIO;
            slice=e->io.now(e->io.ctx);
        }
    }
    e->timeouts++;
    return -ETIMEDOUT;
}
static int sync_engine(void *ctx)
{
    struct uc_engine *e=ctx;
    if (!valid(e)) { uc_engine_quarantine(e,"resource_changed"); e->cpu_safe=false; return -EIO; }
    int err=idle(e,true);
    if (err) uc_engine_quarantine(e,err==-ETIMEDOUT ? "idle_timeout" : "idle_failed");
    /* A quarantined engine may prove idle later, enabling software, but
     * never regains native capabilities in this boot. */
    return err;
}
static bool cpu_ready(void *ctx) { return ((struct uc_engine *)ctx)->cpu_safe; }
static unsigned caps(void *ctx)
{
    struct uc_engine *e=ctx;
    return e->active && !e->quarantined ? 1u : 0u;
}
static int prepare(struct uc_engine *e)
{
    if (e->quarantined || !e->active) return -EIO;
    return sync_engine(e);
}
static bool rect(struct uc_engine *e, const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>=0 && r->height>=0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=e->fb->width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=e->fb->height;
}
static uint32_t xy(unsigned x, unsigned y) { return x | (y<<16); }
static int mode(struct uc_engine *e)
{
    uint32_t v=rd(e,UC_MODE);
    if (v==UINT32_MAX) { uc_engine_quarantine(e,"dead_mode"); e->cpu_safe=false; return -EIO; }
    wr(e,UC_MODE,(v&0xfffffcfcu) | (e->fb->bpp==16 ? 0x100u : 0x300u));
    return 0;
}
static int launch(struct uc_engine *e, uint32_t cmd)
{
    /* No host source, DMA or quick-start bit; the command is always last. */
    e->io.drain(e->io.ctx);
    e->cpu_safe=false;
    wr(e,UC_CMD,cmd);
    int err=sync_engine(e);
    if (!err) e->operations++;
    return err;
}
static int fill(void *ctx, const struct fb_rect *r, uint32_t colour)
{
    struct uc_engine *e=ctx;
    if (!rect(e,r)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    int err=prepare(e); if (err) return err;
    err=mode(e); if (err) return err;
    wr(e,UC_DST,xy((unsigned)r->x,(unsigned)r->y));
    wr(e,UC_DIM,xy((unsigned)r->width-1,(unsigned)r->height-1));
    wr(e,UC_FG,colour);
    wr(e,UC_DSTBASE,0);
    wr(e,UC_PITCH,0x80000000u | (e->fb->pitch<<13));
    return launch(e,UC_FILL);
}
static int blit(struct uc_engine *e, unsigned sx, unsigned sy, unsigned dx, unsigned dy,
                unsigned w, unsigned h, uint32_t base, uint32_t pitch)
{
    int err=prepare(e); if (err) return err;
    uint32_t cmd=UC_COPY;
    if (!base) {
        if (sx<dx) { cmd|=UC_DECX; sx+=w-1; dx+=w-1; }
        if (sy<dy) { cmd|=UC_DECY; sy+=h-1; dy+=h-1; }
    }
    err=mode(e); if (err) return err;
    wr(e,UC_SRC,xy(sx,sy)); wr(e,UC_DST,xy(dx,dy));
    wr(e,UC_DIM,xy(w-1,h-1));
    wr(e,UC_SRCBASE,base>>3); wr(e,UC_DSTBASE,0);
    wr(e,UC_PITCH,0x80000000u | (pitch>>3) | (e->fb->pitch<<13));
    return launch(e,cmd);
}
static int copy(void *ctx, const struct fb_rect *r, int32_t x, int32_t y)
{
    struct uc_engine *e=ctx;
    struct fb_rect dst={x,y,r ? r->width : 0,r ? r->height : 0};
    if (!rect(e,r) || !rect(e,&dst)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    return blit(e,(unsigned)r->x,(unsigned)r->y,(unsigned)x,(unsigned)y,
                (unsigned)r->width,(unsigned)r->height,0,e->fb->pitch);
}
static int write_rows(void *ctx, const struct fb_rect *dst, const struct fb_rect *clip,
                      const void *rows, uint32_t stride)
{
    struct uc_engine *e=ctx;
    unsigned bytes=e->fb->bpp/8;
    if (!e->scratch || !rect(e,dst) || !rect(e,clip) || !rows || dst->width>256 ||
        clip->x<dst->x || clip->y<dst->y ||
        (int64_t)clip->x+clip->width>(int64_t)dst->x+dst->width ||
        (int64_t)clip->y+clip->height>(int64_t)dst->y+dst->height ||
        stride<(uint64_t)(uint32_t)dst->width*bytes) return -EOPNOTSUPP;
    uint64_t extent=dst->height ? (uint64_t)((uint32_t)dst->height-1)*stride+(unsigned)dst->width*bytes : 0;
    uintptr_t p=(uintptr_t)rows, fb=(uintptr_t)e->fb->mapped, scratch=(uintptr_t)e->scratch;
    if (extent>UINTPTR_MAX-p ||
        (p<fb+e->fb->size && p+extent>fb) ||
        (p<scratch+UC_SCRATCH_BYTES && p+extent>scratch)) return -EOPNOTSUPP;
    if (!clip->width || !clip->height) return 0;
    int err=prepare(e); if (err) return err;
    for (int32_t y=0;y<clip->height;y++) {
        const uint8_t *src=(const uint8_t *)rows+(size_t)(clip->y-dst->y+y)*stride+
                           (size_t)(clip->x-dst->x)*bytes;
        memcpy(e->scratch,src,(size_t)clip->width*bytes);
        e->io.drain(e->io.ctx);
        err=blit(e,0,0,(unsigned)clip->x,(unsigned)(clip->y+y),
                  (unsigned)clip->width,1,e->scratch_offset,UC_SCRATCH_BYTES);
        if (err) return err;
        if (y+1<clip->height && e->pause) {
            err=e->pause(e->pause_ctx);
            if (err) return err;
            err=prepare(e); if (err) return err;
        }
    }
    return 0;
}
static int verify_write(void *ctx)
{ return ((struct uc_engine *)ctx)->verified_write ? 0 : -EOPNOTSUPP; }
static int cursor_set(void *ctx, const struct accel_cursor *shape)
{ (void)ctx; (void)shape; return -EOPNOTSUPP; }
static int cursor_move(void *ctx, int32_t x, int32_t y)
{ (void)ctx; (void)x; (void)y; return -EOPNOTSUPP; }
static int cursor_show(void *ctx, bool show)
{ (void)ctx; (void)show; return -EOPNOTSUPP; }
static void set_pause(void *ctx, int (*pause)(void *), void *arg)
{ struct uc_engine *e=ctx; e->pause=pause; e->pause_ctx=arg; }
static int stop(void *ctx)
{
    struct uc_engine *e=ctx;
    e->active=false; e->verified_write=false;
    int err=sync_engine(e); if (err) return err;
    if (e->saved_valid) {
        /* No command, cursor-enable or VQ restore. Idle is established first. */
        for (unsigned r=4;r<=0x40;r+=4) wr(e,r,e->saved[r/4]);
        e->saved_valid=false;
    }
    if (e->transport_saved) {
        if (e->mmio_changed) {
            e->io.indexed_write(e->io.ctx,true,0x1a,e->saved_sr1a);
            if (e->io.indexed(e->io.ctx,true,0x1a)!=e->saved_sr1a) {
                uc_engine_quarantine(e,"mmio_restore_failed"); e->cpu_safe=false; return -EIO;
            }
            e->mmio_changed=false;
        }
        e->io.indexed_write(e->io.ctx,true,0x10,e->saved_sr10);
        if (e->io.indexed(e->io.ctx,true,0x10)!=e->saved_sr10) {
            uc_engine_quarantine(e,"unlock_restore_failed"); e->cpu_safe=false; return -EIO;
        }
        e->transport_saved=false;
    }
    return 0;
}
int uc_engine_init(struct uc_engine *e, const struct fb_device *fb, const struct uc_io *io,
                   uint8_t *scratch, uint32_t offset)
{
    if (!e || !fb || !io || !io->read || !io->write || !io->indexed ||
        !io->now || !io->drain || !io->valid || !io->indexed_write) return -EINVAL;
    if (e->quarantined || e->active || e->saved_valid || e->transport_saved) return -EIO;
    *e=(struct uc_engine){.io=*io,.fb=fb,.generation=fb->generation,.reason="pending"};
    e->ops=(struct accel_ops){.ctx=e,.sync=sync_engine,.copy_rect=copy,.fill_rect=fill,
        .cursor_set=cursor_set,.cursor_move=cursor_move,.cursor_show=cursor_show,
        .stop=stop,.capabilities=caps,.cpu_ready=cpu_ready,.set_pause=set_pause};
    if (!valid(e)) return -ENODEV;
    if (!fb->present || !fb->mapped || (fb->bpp!=16 && fb->bpp!=32) ||
        !fb->width || !fb->height || fb->width>4096 || fb->height>4096 ||
        !fb->pitch || (fb->pitch&0xffffc007u) ||
        (uint64_t)fb->width*(fb->bpp/8)>fb->pitch ||
        (uint64_t)fb->height*fb->pitch>fb->size) return -EOPNOTSUPP;
    /* The VGA register aliases are accessible before engine MMIO is enabled,
     * as in OpenChrome viaMapMMIO. No pixel/engine command is sent here.
     * Keep access enabled after an uncertain failure so idle can be proved. */
    e->saved_sr10=io->indexed(io->ctx,true,0x10);
    if (e->saved_sr10==255) { e->reason="vga_registers_unreadable"; return -EIO; }
    io->indexed_write(io->ctx,true,0x10,e->saved_sr10|1u);
    e->saved_sr1a=io->indexed(io->ctx,true,0x1a);
    e->transport_saved=true; e->touched=true;
    if (e->saved_sr1a==255 || io->indexed(io->ctx,true,0x10)!=(e->saved_sr10|1u)) {
        uc_engine_quarantine(e,"extended_unlock_failed"); return -EIO;
    }
    e->mmio_changed=true;
    io->indexed_write(io->ctx,true,0x1a,e->saved_sr1a|0x68u);
    if (io->indexed(io->ctx,true,0x1a)!=(e->saved_sr1a|0x68u)) {
        uc_engine_quarantine(e,"mmio_enable_failed"); return -EIO;
    }
    int err=idle(e,false);
    if (err) { uc_engine_quarantine(e,"initial_idle_failed"); return err; }
    uint8_t (*idx)(void *,bool,uint8_t)=io->indexed;
    uint32_t origin=idx(io->ctx,false,0x0d) | (uint32_t)idx(io->ctx,false,0x0c)<<8 |
        (uint32_t)idx(io->ctx,false,0x34)<<16 | (uint32_t)(idx(io->ctx,false,0x48)&31u)<<24;
    unsigned pitch=((unsigned)idx(io->ctx,false,0x13) |
                   (unsigned)(idx(io->ctx,false,0x35)&0xe0u)<<3)<<3;
    unsigned depth=idx(io->ctx,true,0x15)&0x1cu;
    if (origin || pitch!=fb->pitch || (idx(io->ctx,false,0x6a)&0x80u) ||
        depth!=(fb->bpp==16 ? 0x14u : 0x0cu)) { e->reason="scanout_mismatch"; return -EOPNOTSUPP; }
    for (unsigned r=4;r<=0x40;r+=4) {
        e->saved[r/4]=rd(e,r);
        if (e->saved[r/4]==UINT32_MAX) { uc_engine_quarantine(e,"dead_registers"); return -EIO; }
    }
    e->saved_valid=true; e->touched=true;
    if (hide(e)) { uc_engine_quarantine(e,"cursor_disable_failed"); e->cpu_safe=false; return -EIO; }
    /* PIO-only original-engine transport: disable VQ, never allocate/enable
     * it. Do not import Linux's AGP setup or write command register zero. */
    wr(e,UC_TRANSET,0x00fe0000);
    static const uint32_t disable[]={0x00000004,0x40008c0f,0x44000000,0x45080c04,0x46800408};
    for (unsigned i=0;i<ARRAY_SIZE(disable);i++) wr(e,UC_TRANSPACE,disable[i]);
    for (unsigned r=4;r<=0x40;r+=4) wr(e,r,0);
    if (scratch && offset>=fb->size && !(offset&4095u) && offset<=0x1ffff000u) {
        e->scratch=scratch; e->scratch_offset=offset;
        e->ops.write_rect=write_rows; e->ops.verify_write=verify_write;
    }
    err=sync_engine(e);
    if (err) return err;
    e->active=true; e->reason="none";
    return 0;
}

/* Only fbdev's exclusive activation probe calls this routine. No generic
 * fallback dispatch is used: every comparison requires a native completion. */
int uc_engine_probe(void *ctx)
{
    struct uc_engine *e=ctx;
    if (e->fb->width<8 || e->fb->height<8) return -EOPNOTSUPP;
    int err=sync_engine(e); if (err) return err;
    unsigned bytes=e->fb->bpp/8;
    uint8_t saved[8*8*4], pixels[sizeof(saved)], upload[sizeof(saved)];
    struct fb_device ref=*e->fb;
    ref.width=ref.height=8; ref.pitch=8*bytes; ref.size=8*ref.pitch; ref.mapped=pixels;
    for (unsigned y=0;y<8;y++) memcpy(saved+y*ref.pitch,e->fb->mapped+y*e->fb->pitch,ref.pitch);
    memcpy(pixels,saved,ref.size);
    struct fb_rect block={0,0,8,8}, seed={0,0,4,4}, src={0,0,6,6};
    for (unsigned op=0;op<5 && !err;op++) {
        if (!op || op==1) {
            struct fb_rect *r=op ? &seed : &block;
            uint32_t colour=swdib_pack(&ref,op ? 0x1284d6 : 0xc73542);
            swdib_fill(&ref,r,colour); err=fill(e,r,colour);
        } else if (op==2 || op==3) {
            int dst=op==2 ? 2 : 0;
            if (op==3) src.x=src.y=2;
            swdib_copy(&ref,&src,dst,dst); err=copy(e,&src,dst,dst);
        } else if (e->scratch) {
            for (unsigned i=0;i<ref.size;i++) upload[i]=(uint8_t)(i*37u+19);
            swdib_write(&ref,&block,&block,upload,ref.pitch);
            err=write_rows(e,&block,&block,upload,ref.pitch);
        }
        if (!err) err=sync_engine(e);
        if (!err) for (unsigned y=0;y<8;y++)
            if (memcmp(pixels+y*ref.pitch,e->fb->mapped+y*e->fb->pitch,ref.pitch)) { err=-EIO; break; }
    }
    /* Restoration after failure is allowed only after a new completion proof. */
    if (!sync_engine(e) && e->cpu_safe) {
        for (unsigned y=0;y<8;y++) memcpy(e->fb->mapped+y*e->fb->pitch,saved+y*ref.pitch,ref.pitch);
        e->io.drain(e->io.ctx);
    } else err=-EIO;
    if (err) uc_engine_quarantine(e,"pixel_probe_failed");
    else e->verified_write=e->scratch!=0;
    return err;
}
