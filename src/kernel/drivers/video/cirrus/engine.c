/*
 * drivers/video/cirrusfb.c - driver for Cirrus Logic chipsets
 *
 * Copyright 1999-2001 Jeff Garzik <jgarzik@pobox.com>
 *
 * Contributors (thanks, all!)
 *
 *	David Eger:
 *	Overhaul for Linux 2.6
 *
 *      Jeff Rugen:
 *      Major contributions;  Motorola PowerStack (PPC and PCI) support,
 *      GD54xx, 1280x1024 mode support, change MCLK based on VCLK.
 *
 *	Geert Uytterhoeven:
 *	Excellent code review.
 *
 *	Lars Hecking:
 *	Amiga updates and testing.
 *
 * Original cirrusfb author:  Frank Neumann
 *
 * Based on retz3fb.c and cirrusfb.c:
 *      Copyright (C) 1997 Jes Sorensen
 *      Copyright (C) 1996 Frank Neumann
 *
 ***************************************************************
 *
 * Format this code with GNU indent '-kr -i8 -pcs' options.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 *
 */
/* Origin: Linux v6.12 drivers/video/fbdev/cirrusfb.c,
 * cirrusfb_set_blitter/BitBLT/RectFill. Native synchronous adaptation;
 * bounded polling and quarantine replace the donor's unbounded waits.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "engine.h"
#include "registers.h"

bool cirrus_layout(const struct fb_device *d)
{
    return d && d->present && d->mapped && d->width >= 8 && d->height >= 2 &&
        (d->bpp == 16 || d->bpp == 24 || d->bpp == 32) &&
        d->pitch >= (uint64_t)d->width * (d->bpp / 8) && d->pitch <= 8191 &&
        d->size && d->size <= 4u*1024*1024 &&
        (uint64_t)d->pitch * d->height <= d->size;
}

static int poll_idle(struct cirrus_engine *s)
{
    s->waits++;
    uint64_t start=s->io.now(s->io.ctx), slice=start;
    unsigned idle=0;
    for (unsigned i=0; i<CIRRUS_WAIT_POLLS; i++) {
        if (!s->io.valid(s->io.ctx)) { s->cpu_safe=false; return -EIO; }
        uint64_t now=s->io.now(s->io.ctx);
        if (now-start >= CIRRUS_WAIT_US) break;
        s->status=s->io.read(s->io.ctx,CL_GR31);
        if (s->status==0xff) { s->cpu_safe=false; return -EIO; }
        /* Busy, start, reset and buffered registers must all be clear.
         * Two reads, not the reset write, establish the CPU access proof. */
        if (!(s->status & 0x17)) {
            if (++idle==2) { s->cpu_safe=true; return 0; }
        } else idle=0;
        if (s->pause && now-slice>=1000) {
            int e=s->pause(s->pause_arg);
            if (e) { s->cpu_safe=false; return e; }
            slice=s->io.now(s->io.ctx);
        }
    }
    s->cpu_safe=false; s->timeouts++;
    return -ETIMEDOUT;
}

void cirrus_engine_quarantine(struct cirrus_engine *s, const char *reason)
{
    s->active=false; s->quarantined=true; s->reason=reason;
}

static int recover(struct cirrus_engine *s)
{
    if (!s->io.valid(s->io.ctx)) { s->cpu_safe=false; return -EIO; }
    s->cpu_safe=false;
    /* No system-memory transfer is ever started. Disable autostart and
     * reset the VRAM engine, then independently prove it stopped. */
    if (s->io.read(s->io.ctx,CL_GR31)==0xff) return -EIO;
    s->io.write(s->io.ctx,CL_GR31,0x04);
    s->io.write(s->io.ctx,CL_GR31,0x00);
    return poll_idle(s);
}

static int fault(struct cirrus_engine *s, int error)
{
    cirrus_engine_quarantine(s,"engine_fault");
    (void)recover(s);
    return error;
}

static int sync_engine(void *ctx)
{
    struct cirrus_engine *s=ctx;
    if (!s->active) return s->cpu_safe ? 0 : recover(s);
    int e=poll_idle(s);
    return e ? fault(s,e) : 0;
}
static int stop_engine(void *ctx)
{
    struct cirrus_engine *s=ctx;
    int e=sync_engine(s);
    s->active=false;
    return e && !s->cpu_safe ? e : 0;
}
static unsigned caps(void *ctx)
{ struct cirrus_engine *s=ctx; return s->active && !s->quarantined ? 1u : 0u; }
static bool cpu_ready(void *ctx) { return ((struct cirrus_engine *)ctx)->cpu_safe; }
static void set_pause(void *ctx, int (*pause)(void *), void *arg)
{ struct cirrus_engine *s=ctx; s->pause=pause; s->pause_arg=arg; }

static bool rectangle(const struct cirrus_engine *s, const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>0 && r->height>0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height &&
        (uint64_t)(uint32_t)r->width*(s->fb.bpp/8)<=s->max_width &&
        (uint32_t)r->height<=s->max_height;
}
static void reg16(struct cirrus_engine *s, uint8_t index, uint32_t value)
{
    s->io.write(s->io.ctx,index,(uint8_t)value);
    s->io.write(s->io.ctx,index+1,(uint8_t)(value>>8));
}
static void address(struct cirrus_engine *s, uint8_t index, uint32_t value)
{
    reg16(s,index,value);
    s->io.write(s->io.ctx,index+2,(uint8_t)(value>>16));
}
static int launch(struct cirrus_engine *s, const struct fb_rect *r,
                  uint32_t src, uint32_t dst, uint8_t mode)
{
    unsigned bytes=s->fb.bpp/8;
    /* Width is in bytes; height and both pitches retain the donor order. */
    reg16(s,CL_GR24,s->fb.pitch); reg16(s,CL_GR26,s->fb.pitch);
    reg16(s,CL_GR20,(uint32_t)r->width*bytes-1);
    reg16(s,CL_GR22,(uint32_t)r->height-1);
    address(s,CL_GR28,dst); address(s,CL_GR2C,src);
    s->io.write(s->io.ctx,CL_GR30,mode);
    s->io.write(s->io.ctx,CL_GR32,0x0d);
    s->cpu_safe=false; s->touched=true;
    s->io.write(s->io.ctx,CL_GR31,0x02);
    int e=poll_idle(s);
    if (e) return fault(s,e);
    s->operations++;
    return 0;
}
static int prepare(struct cirrus_engine *s)
{
    if (!s->active || s->quarantined) return -EIO;
    int e=sync_engine(s);
    if (e) return e;
    s->io.drain(s->io.ctx);
    /* Clear inherited autostart before any parameter register is changed. */
    s->io.write(s->io.ctx,CL_GR31,0);
    if (s->io.read(s->io.ctx,CL_GR31)!=0) return fault(s,-EIO);
    return 0;
}
static int fill_rect(void *ctx, const struct fb_rect *r, uint32_t colour)
{
    struct cirrus_engine *s=ctx;
    if (!rectangle(s,r)) return -EOPNOTSUPP;
    int e=prepare(s); if (e) return e;
    /* The donor uses an 8x8 colour-expanded pattern with equal FG and BG;
     * its source bytes at offset zero are read, never modified. */
    static const uint8_t bg[]={0,CL_GR10,CL_GR12,CL_GR14};
    static const uint8_t fg[]={1,CL_GR11,CL_GR13,CL_GR15};
    unsigned bytes=s->fb.bpp/8;
    for (unsigned i=0;i<bytes;i++) {
        s->io.write(s->io.ctx,bg[i],(uint8_t)(colour>>(8*i)));
        s->io.write(s->io.ctx,fg[i],(uint8_t)(colour>>(8*i)));
    }
    return launch(s,r,0,(uint32_t)r->y*s->fb.pitch+(uint32_t)r->x*bytes,
                  (uint8_t)(0xc0 | ((bytes-1)<<4)));
}
static int copy_rect(void *ctx, const struct fb_rect *r, int32_t x, int32_t y)
{
    struct cirrus_engine *s=ctx;
    if (!rectangle(s,r)) return -EOPNOTSUPP;
    struct fb_rect dst={x,y,r->width,r->height};
    if (!rectangle(s,&dst)) return -EOPNOTSUPP;
    int e=prepare(s); if (e) return e;
    unsigned bytes=s->fb.bpp/8;
    uint32_t src=(uint32_t)r->y*s->fb.pitch+(uint32_t)r->x*bytes;
    uint32_t dest=(uint32_t)y*s->fb.pitch+(uint32_t)x*bytes;
    bool reverse=dest>src;
    if (reverse) {
        uint32_t last=((uint32_t)r->height-1)*s->fb.pitch+(uint32_t)r->width*bytes-1;
        src+=last; dest+=last;
    }
    /* Byte copy works for all packed formats, including 24 bpp. */
    return launch(s,r,src,dest,reverse ? 1 : 0);
}

int cirrus_engine_start(struct cirrus_engine *s, const struct fb_device *fb,
                        const struct cirrus_io *io, uint16_t device)
{
    if (!cirrus_layout(fb) || !io || !io->read || !io->write || !io->now ||
        !io->drain || !io->valid || s->quarantined) return -EINVAL;
    bool extended=device==0x00b8;
    if (!extended && (fb->bpp==32 || fb->pitch>4095 || fb->size>2u*1024*1024))
        return -EOPNOTSUPP;
    *s=(struct cirrus_engine){.io=*io,.fb=*fb,.reason="pending",
        .max_width=extended ? 8192 : 2048,.max_height=extended ? 8192 : 1024};
    s->ops=(struct accel_ops){.ctx=s,.sync=sync_engine,.stop=stop_engine,
        .fill_rect=fill_rect,.copy_rect=copy_rect,.capabilities=caps,
        .cpu_ready=cpu_ready,.set_pause=set_pause};
    int e=poll_idle(s);
    if (e) return fault(s,e);
    s->active=true; s->reason="none";
    return 0;
}
