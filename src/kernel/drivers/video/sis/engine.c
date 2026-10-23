// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SiS 300/540/630[S]/730[S],
 * SiS 315[E|PRO]/550/[M]650/651/[M]661[F|M]X/740/[M]741[GX]/330/[M]760[GX],
 * XGI V3XT/V5/V8, Z7
 * frame buffer driver for Linux kernels >= 2.4.14 and >=2.6.3
 *
 * 2D acceleration part
 *
 * Based on the XFree86/X.org driver which is
 *     Copyright (C) 2001-2005 by Thomas Winischhofer, Vienna, Austria
 *
 * Author: Thomas Winischhofer <thomas@winischhofer.net>
 *			(see http://www.winischhofer.net/
 *			for more information and updates)
 */

/* Origin: Linux v6.12 drivers/video/fbdev/sis/sis_accel.c, sis_accel.h,
 * sis_main.c and sis.h. Native bounded MMIO adaptation for CiukiOS.
 * Register layouts, ROPs and generation-specific overlap are derived from
 * Thomas Winischhofer's driver. No Linux OS glue or infinite waits retained. */
#include <ciuki/kernel.h>
#include "sis.h"

struct engine_layout { unsigned status, mask, observations, dst_height; };
/* 315 command bits 16/17 encode depth; on 300 they encode directions. */
static const struct engine_layout layouts[] = {
    [SIS_300]={0x8240,0xe0000000u,3,0xffff},
    [SIS_315]={0x85cc,0x80000000u,4,0x0fff}
};
static uint32_t rd(struct sis_engine *s,unsigned reg,unsigned width)
{ return s->io.read(s->io.ctx,reg,width); }
static void wr(struct sis_engine *s,unsigned reg,unsigned width,uint32_t v)
{ s->io.write(s->io.ctx,reg,width,v); }
static uint8_t seq(struct sis_engine *s,uint8_t r)
{ return s->io.indexed_read(s->io.ctx,false,r); }
static void setseq(struct sis_engine *s,uint8_t r,uint8_t v)
{ s->io.indexed_write(s->io.ctx,false,r,v); }
bool sis_mode_supported(const struct fb_device *d)
{
    if (!d || !d->present || !d->mapped || !d->width || !d->height ||
        d->width>2048 || d->height>2048 || !d->pitch || d->pitch>0xffff ||
        (d->pitch&7u) || (uint64_t)d->pitch*d->height>d->size) return false;
    if (d->bpp==16) return d->red_size==5 && d->red_pos==11 &&
        d->green_size==6 && d->green_pos==5 && d->blue_size==5 && !d->blue_pos &&
        d->pitch>=d->width*2;
    return d->bpp==32 && d->red_size==8 && d->red_pos==16 &&
        d->green_size==8 && d->green_pos==8 && d->blue_size==8 && !d->blue_pos &&
        d->pitch>=d->width*4;
}
static bool rect(struct sis_engine *s,const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>=0 && r->height>=0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height;
}
void sis_quarantine(struct sis_engine *s,const char *reason)
{
    s->quarantined=true; s->active=false; s->reason=reason;
    if (s->touched && !sis_cursor_hide(s)) s->cpu_safe=false;
}
static int idle(struct sis_engine *s,bool sliced)
{
    const struct engine_layout *l=&layouts[s->generation];
    uint64_t start=s->io.now(s->io.ctx), slice=start;
    unsigned good=0;
    uint64_t budget=sliced && s->pause ? SIS_WAIT_US : 1000u;
    s->waits++; s->cpu_safe=false;
    for (unsigned i=0;i<SIS_POLL_LIMIT;i++) {
        uint32_t v=rd(s,l->status,32);
        if (v==UINT32_MAX) { sis_quarantine(s,"dead_mmio"); return -EIO; }
        if ((v&l->mask)==l->mask) {
            if (++good==l->observations) {
                if (s->quarantined && s->touched && !sis_cursor_hide(s)) return -EIO;
                s->cpu_safe=true; return 0;
            }
        } else good=0;
        uint64_t now=s->io.now(s->io.ctx);
        if (now-start>=budget) break;
        if (sliced && s->pause && (now-slice>=1000 || (i && !(i%1024)))) {
            int e=s->pause(s->pause_arg);
            if (e) { sis_quarantine(s,"wait_cancelled"); return e; }
            slice=s->io.now(s->io.ctx);
        }
    }
    s->timeouts++; sis_quarantine(s,"idle_timeout"); return -ETIMEDOUT;
}
int sis_sync(void *ctx)
{
    struct sis_engine *s=ctx;
    s->io.drain(s->io.ctx);
    return idle(s,true);
}
bool sis_cpu_ready(void *ctx) { return ((struct sis_engine *)ctx)->cpu_safe; }
unsigned sis_capabilities(void *ctx)
{
    struct sis_engine *s=ctx;
    return s->active && !s->quarantined ? 1u | (s->cursor_ok ? 2u : 0u) : 0;
}
void sis_pause(void *ctx,int (*pause)(void *),void *arg)
{ struct sis_engine *s=ctx; s->pause=pause; s->pause_arg=arg; }
int sis_engine_start(struct sis_engine *s)
{
    if (s->quarantined || !sis_mode_supported(&s->fb)) return -EOPNOTSUPP;
    s->cpu_safe=true;
    s->saved05=seq(s,5);
    setseq(s,5,0x86);
    if (seq(s,5)!=0xa1) { setseq(s,5,s->saved05==0xa1 ? 0x86 : 0); return -EOPNOTSUPP; }
    s->saved1e=seq(s,0x1e); s->saved26=seq(s,0x26); s->saved27=seq(s,0x27);
    uint8_t depth=s->fb.bpp==16 ? 0x08 : 0x10;
    uint8_t sr06=seq(s,6);
    uint8_t cr30=s->io.indexed_read(s->io.ctx,true,0x30);
    bool wrong=(sr06&0x3c)!=depth ||
        (s->io.indexed_read(s->io.ctx,true,9)&0x80) || (cr30&0x7f) ||
        s->io.indexed_read(s->io.ctx,true,0x0c) ||
        s->io.indexed_read(s->io.ctx,true,0x0d) || seq(s,0x0d) ||
        (s->generation==SIS_315 && (seq(s,0x37)&1)) || (seq(s,0x20)&0x81)!=0x81;
    /* Do not inherit an unreserved firmware TurboQueue or AGP/VRAM queue. */
    wrong |= s->generation==SIS_300 ? (s->saved27&0x80)!=0 : (s->saved26&0xc0)!=0;
    if (wrong) { setseq(s,5,s->saved05==0xa1 ? 0x86 : 0); return -EOPNOTSUPP; }
    /* All-engine/queue idle is required even if the 2D module was off:
     * the shared 315 queue can also contain work for another engine. */
    if (idle(s,false)) return -EIO;
    s->touched=true;
    s->configured=true;
    s->queue_changed=false;
    setseq(s,0x1e,s->saved1e|0x40);
    if (!(seq(s,0x1e)&0x40)) { sis_quarantine(s,"engine_enable_failed"); return -EIO; }
    if (s->generation==SIS_315) {
        s->saved_queue=rd(s,0x85c0,32);
        if (s->saved_queue==UINT32_MAX) {
            s->cpu_safe=false; sis_quarantine(s,"dead_queue"); return -EIO;
        }
        s->queue_changed=true;
        setseq(s,0x27,0x1f); setseq(s,0x26,1);
        uint32_t rp=rd(s,0x85c8,32);
        if (rp==UINT32_MAX) { s->cpu_safe=false; sis_quarantine(s,"dead_queue"); return -EIO; }
        wr(s,0x85c4,32,rp);
        setseq(s,0x26,0x22);
        wr(s,0x85c0,32,s->queue_offset);
        if (seq(s,0x26)!=0x22 || rd(s,0x85c0,32)!=s->queue_offset) {
            s->cpu_safe=false; sis_quarantine(s,"queue_setup_failed"); return -EIO;
        }
    }
    if (idle(s,false)) return -EIO;
    if (!sis_cursor_hide(s)) { s->cpu_safe=false; sis_quarantine(s,"cursor_disable_failed"); return -EIO; }
    s->active=true; s->reason="none";
    return 0;
}
static int before(struct sis_engine *s)
{
    if (!s->active || s->quarantined) return -EIO;
    s->io.drain(s->io.ctx);
    int e=idle(s,true);
    if (!e && s->generation==SIS_300 && (rd(s,0x8240,16)&0xffff)<16) {
        sis_quarantine(s,"fifo_unavailable"); e=-EIO;
    }
    return e;
}
/* The 315 donor deliberately re-proves idle at each MMIO operand; unlike
 * 300 it supplies no trustworthy free-slot count. Preserve that policy. */
static int operand(struct sis_engine *s,unsigned reg,unsigned width,uint32_t value)
{
    if (s->generation==SIS_315) { int e=idle(s,true); if (e) return e; }
    wr(s,reg,width,value);
    return 0;
}
static int descriptor(struct sis_engine *s)
{
    int e=operand(s,0x8206,16,s->fb.bpp==16 ? 0x8000 : 0xc000);
    if (!e) e=operand(s,0x8214,32,(layouts[s->generation].dst_height<<16)|s->fb.pitch);
    if (!e) e=operand(s,0x8210,32,0);
    return e;
}
static uint32_t flags(struct sis_engine *s)
{ return s->generation==SIS_300 ? 0x30000 : s->fb.bpp==16 ? 0x10000 : 0x20000; }
static int fire(struct sis_engine *s,uint32_t cmd)
{
    int e=operand(s,0x823c,32,cmd);
    if (e) return e;
    if (s->generation==SIS_315) { e=idle(s,true); if (e) return e; }
    s->cpu_safe=false;
    wr(s,0x8240,32,0);
    e=idle(s,true);
    if (!e) s->operations++;
    return e;
}
static int solid(struct sis_engine *s,const struct fb_rect *r,uint32_t colour,bool invert)
{
    if (!rect(s,r)) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -EIO;
    if (!r->width || !r->height) return 0;
    int e=before(s); if (e) return e;
    e=descriptor(s);
    if (!e) e=operand(s,0x821c,32,colour);
    if (!e) e=operand(s,0x820c,32,((uint32_t)r->x<<16)|(uint32_t)r->y);
    if (!e) e=operand(s,0x8218,32,((uint32_t)r->height<<16)|(uint32_t)r->width);
    if (e) return e;
    return fire(s,flags(s)|((invert ? 0x5au : 0xf0u)<<8));
}
int sis_fill(void *ctx,const struct fb_rect *r,uint32_t colour)
{ return solid(ctx,r,colour,false); }
int sis_invert(void *ctx,const struct fb_rect *r)
{ struct sis_engine *s=ctx; return solid(s,r,s->fb.bpp==16 ? 0xffff : 0xffffff,true); }
int sis_copy(void *ctx,const struct fb_rect *r,int32_t dx,int32_t dy)
{
    struct sis_engine *s=ctx;
    struct fb_rect d={dx,dy,r ? r->width : 0,r ? r->height : 0};
    if (!rect(s,r) || !rect(s,&d)) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -EIO;
    if (!r->width || !r->height) return 0;
    int e=before(s); if (e) return e;
    uint32_t sx=r->x, sy=r->y, x=dx, y=dy, cmd=0xcc00;
    if (s->generation==SIS_300) {
        if (dx<=r->x) cmd|=0x10000; else { sx+=r->width-1; x+=r->width-1; }
        if (dy<=r->y) cmd|=0x20000; else { sy+=r->height-1; y+=r->height-1; }
    } else cmd|=flags(s);
    e=descriptor(s);
    if (!e) e=operand(s,0x8204,16,s->fb.pitch);
    /* Equal bases preserve 315 automatic overlap detection. Supported
     * geometry fits original coordinates; no independent base rebasing. */
    if (!e) e=operand(s,0x8200,32,0);
    if (!e) e=operand(s,0x8218,32,((uint32_t)r->height<<16)|(uint32_t)r->width);
    if (!e) e=operand(s,0x8208,32,(sx<<16)|sy);
    if (!e) e=operand(s,0x820c,32,(x<<16)|y);
    if (e) return e;
    return fire(s,cmd);
}
int sis_pattern(void *ctx,const struct fb_rect *r,const uint8_t p[8],uint32_t fg,uint32_t bg)
{
    struct sis_engine *s=ctx;
    if (!p || !rect(s,r)) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -EIO;
    if (!r->width || !r->height) return 0;
    int e=before(s); if (e) return e;
    e=descriptor(s);
    if (!e) e=operand(s,0x821c,32,fg);
    if (!e) e=operand(s,0x8220,32,bg);
    uint32_t lo=0, hi=0;
    for (unsigned i=0;i<4;i++) { lo|=(uint32_t)p[i]<<(8*i); hi|=(uint32_t)p[i+4]<<(8*i); }
    if (!e) e=operand(s,0x822c,32,lo);
    if (!e) e=operand(s,0x8230,32,hi);
    if (!e) e=operand(s,0x820c,32,((uint32_t)r->x<<16)|(uint32_t)r->y);
    if (!e) e=operand(s,0x8218,32,((uint32_t)r->height<<16)|(uint32_t)r->width);
    if (e) return e;
    return fire(s,flags(s)|0xf080);
}
int sis_stop(void *ctx)
{
    struct sis_engine *s=ctx;
    s->active=false;
    if (!s->touched) return s->cpu_safe ? 0 : idle(s,true);
    int e=idle(s,true);
    if (e || !sis_cursor_hide(s)) { s->cpu_safe=false; return e ? e : -EIO; }
    if (s->queue_changed && s->generation==SIS_315) {
        setseq(s,0x26,1); wr(s,0x85c0,32,s->saved_queue);
        setseq(s,0x27,s->saved27); setseq(s,0x26,s->saved26);
        if (seq(s,0x26)!=s->saved26 || seq(s,0x27)!=s->saved27 ||
            rd(s,0x85c0,32)!=s->saved_queue) {
            s->cpu_safe=false; sis_quarantine(s,"queue_restore_failed"); return -EIO;
        }
    }
    setseq(s,0x1e,s->saved1e);
    if (seq(s,0x1e)!=s->saved1e) { s->cpu_safe=false; sis_quarantine(s,"stop_failed"); return -EIO; }
    setseq(s,5,s->saved05==0xa1 ? 0x86 : 0);
    s->configured=false; s->queue_changed=false; s->cursor_loaded=false;
    s->touched=false; s->cpu_safe=true; return 0;
}
