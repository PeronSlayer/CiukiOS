// SPDX-License-Identifier: GPL-2.0-only
/*
 * Frame buffer driver for Trident TGUI, Blade and Image series
 *
 * Copyright 2001, 2002 - Jani Monoses   <jani@iv.ro>
 * Copyright 2009 Krzysztof Helt <krzysztof.h1@wp.pl>
 *
 * CREDITS:(in order of appearance)
 * skeletonfb.c by Geert Uytterhoeven and other fb code in drivers/video
 * Special thanks ;) to Mattia Crivellini <tia@mclink.it>
 * much inspired by the XFree86 4.x Trident driver sources
 * by Alan Hourihane the FreeVGA project
 * Francesco Salvestrini <salvestrini@users.sf.net> XP support,
 * code, suggestions
 *
 * Origin: Linux v6.12 drivers/video/fbdev/tridentfb.c, blade_* and xp_*;
 * register definitions: include/video/trident.h (SPDX: GPL-2.0).
 * Ciuki adaptation: synchronous local-VRAM commands, checked geometry,
 * byte-pitch correction, bounded waits and fail-closed quarantine. No Linux
 * mode setting, host-data commands or timeout reset is imported.
 */
#include <ciuki/kernel.h>
#include "engine.h"

enum tb_kind tb_device(uint16_t id)
{
    switch (id) {
    case 0x9880: case 0x8400: case 0x8420: case 0x8500:
    case 0x8520: case 0x8600: case 0x8620: return TB_BLADE;
    case 0x8820: return TB_XP;
    default: return TB_NONE;
    }
}
static int xp_pitch(uint32_t bytes)
{
    switch (bytes) {
    case 512: case 8192: return 0;
    case 1024: return 4;
    case 2048: return 8;
    case 4096: return 12;
    default: return -1;
    }
}
bool tb_format(enum tb_kind kind, const struct fb_device *d)
{
    if ((kind!=TB_BLADE && kind!=TB_XP) || !d || !d->present || !d->mapped || !d->width || !d->height ||
        d->width>2048 || d->height>2048 || (d->bpp!=16 && d->bpp!=32)) return false;
    unsigned bytes=d->bpp/8;
    if (d->blue_pos || d->green_pos!=(bytes==2 ? 5 : 8) || d->red_pos!=(bytes==2 ? 11 : 16) ||
        d->blue_size!=(bytes==2 ? 5 : 8) || d->green_size!=(bytes==2 ? 6 : 8) ||
        d->red_size!=(bytes==2 ? 5 : 8) || d->pitch%bytes ||
        (uint64_t)d->width*bytes>d->pitch || (uint64_t)d->pitch*d->height>d->size) return false;
    uint32_t pixels=d->pitch/bytes;
    return kind==TB_XP ? xp_pitch(d->pitch)>=0 : !(pixels&7u) && pixels<=4088;
}
static uint32_t point(unsigned x, unsigned y) { return (y<<16)|x; }
static void w32(struct tb_engine *e,unsigned r,uint32_t v) { e->io.write32(e->io.ctx,r,v); }
static void w8(struct tb_engine *e,unsigned r,uint8_t v) { e->io.write8(e->io.ctx,r,v); }
void tb_quarantine(struct tb_engine *e, const char *why)
{
    if (!e->quarantined || !e->reason) e->reason=why;
    e->active=false; e->quarantined=true; e->cpu_safe=false;
}
int tb_idle(struct tb_engine *e)
{
    if (e->io.valid && !e->io.valid(e->io.ctx)) {
        tb_quarantine(e,"access_lease_revoked"); return -EIO;
    }
    uint64_t start=e->io.now_us(e->io.ctx), slice=start;
    e->waits++;
    for (unsigned i=0;i<TB_POLL_LIMIT;i++) {
        uint32_t v=e->kind==TB_XP ? e->io.read8(e->io.ctx,TB_STATUS) : e->io.read32(e->io.ctx,TB_STATUS);
        e->status=v;
        if (v==(e->kind==TB_XP ? 0xffu : UINT32_MAX)) {
            tb_quarantine(e,"dead_mmio"); return -EIO;
        }
        if (!(v&(e->kind==TB_XP ? 0x80u : 0xfa800000u))) {
            e->cpu_safe=true; return 0;
        }
        uint64_t now=e->io.now_us(e->io.ctx);
        if (now-start>=TB_WAIT_US) break;
        if (e->pause && now-slice>=1000) {
            int err=e->pause(e->pause_ctx);
            if (err) { tb_quarantine(e,"wait_cancelled"); return err; }
            slice=e->io.now_us(e->io.ctx);
        }
    }
    tb_quarantine(e,"engine_timeout"); return -ETIMEDOUT;
}
static int sync_engine(void *ctx) { return tb_idle(ctx); }
static unsigned capabilities(void *ctx)
{ struct tb_engine *e=ctx; return e->active && !e->quarantined ? 1u : 0; }
static bool cpu_ready(void *ctx) { return ((struct tb_engine *)ctx)->cpu_safe; }
static void set_pause(void *ctx,int (*pause)(void *),void *arg)
{ struct tb_engine *e=ctx; e->pause=pause; e->pause_ctx=arg; }
static int stop_engine(void *ctx)
{
    struct tb_engine *e=ctx;
    e->active=false;
    return tb_idle(e);
}
static bool rect(const struct tb_engine *e,const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>=0 && r->height>=0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=e->fb->width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=e->fb->height;
}
static int fill(void *ctx,const struct fb_rect *r,uint32_t c)
{
    struct tb_engine *e=ctx;
    if (!e->active || e->quarantined) return -EIO;
    if (!rect(e,r)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    int err=tb_idle(e); if (err) return err;
    if (e->fb->bpp==16) c=(c&0xffffu)|((c&0xffffu)<<16);
    e->cpu_safe=false;
    if (e->kind==TB_BLADE) {
        w32(e,0x2160,c); w32(e,0x2148,0xcc);
        w32(e,0x2144,0x20080018);
        w32(e,0x2108,point(r->x,r->y));
        w32(e,0x210c,point(r->x+r->width-1,r->y+r->height-1));
    } else {
        w32(e,0x2127,0xf0); w32(e,0x2158,c); w32(e,0x2128,0x4000);
        w32(e,0x2140,point(r->height,r->width));
        w32(e,0x2138,point(r->y,r->x));
        w8(e,0x2124,1); w8(e,0x2125,e->operation);
    }
    err=tb_idle(e); if (!err) e->operations++;
    return err;
}
static int copy(void *ctx,const struct fb_rect *r,int32_t x,int32_t y)
{
    struct tb_engine *e=ctx;
    if (!e->active || e->quarantined) return -EIO;
    if (!rect(e,r)) return -EOPNOTSUPP;
    struct fb_rect d={x,y,r->width,r->height};
    if (!rect(e,&d)) return -EOPNOTSUPP;
    if (!r->width || !r->height || (r->x==x && r->y==y)) return 0;
    int err=tb_idle(e); if (err) return err;
    e->cpu_safe=false;
    if (e->kind==TB_BLADE) {
        bool reverse=!(r->y>y || (r->y==y && r->x>x));
        uint32_t s1=point(r->x,r->y), s2=point(r->x+r->width-1,r->y+r->height-1);
        uint32_t d1=point(x,y), d2=point(x+r->width-1,y+r->height-1);
        w32(e,0x2148,0xcc); w32(e,0x2144,0xe0080014u|(reverse ? 2u : 0u));
        w32(e,0x2100,reverse ? s2 : s1); w32(e,0x2104,reverse ? s1 : s2);
        w32(e,0x2108,reverse ? d2 : d1); w32(e,0x210c,reverse ? d1 : d2);
    } else {
        unsigned sx=r->x,sy=r->y,dx=x,dy=y,dir=4;
        if (r->x<x && r->y==y) { dir|=0x200; sx+=r->width-1; dx+=r->width-1; }
        if (r->y<y) { dir|=0x100; sy+=r->height-1; dy+=r->height-1; }
        w32(e,0x2128,dir); w8(e,0x2127,0xcc);
        w32(e,0x213c,point(sy,sx)); w32(e,0x2138,point(dy,dx));
        w32(e,0x2140,point(r->height,r->width)); w8(e,0x2124,1);
    }
    err=tb_idle(e); if (!err) e->operations++;
    return err;
}
/* Linux's selected donor has no cursor implementation. Clean refusal also
 * keeps the part A desktop's software cursor ownership unambiguous. */
static int cursor_set(void *ctx,const struct accel_cursor *c) { (void)ctx; (void)c; return -EOPNOTSUPP; }
static int cursor_move(void *ctx,int32_t x,int32_t y) { (void)ctx; (void)x; (void)y; return -EOPNOTSUPP; }
static int cursor_show(void *ctx,bool show) { (void)ctx; (void)show; return -EOPNOTSUPP; }
void tb_construct(struct tb_engine *e,enum tb_kind kind,const struct fb_device *fb,const struct tb_io *io)
{
    memset(e,0,sizeof(*e)); e->kind=kind; e->fb=fb; e->io=*io;
    e->cpu_safe=true; e->reason="pending";
    e->ops=(struct accel_ops){.ctx=e,.sync=sync_engine,.fill_rect=fill,.copy_rect=copy,
        .cursor_set=cursor_set,.cursor_move=cursor_move,.cursor_show=cursor_show,
        .stop=stop_engine,.capabilities=capabilities,.cpu_ready=cpu_ready,.set_pause=set_pause};
}
int tb_initialize(struct tb_engine *e)
{
    if (e->quarantined || !tb_format(e->kind,e->fb)) return -EOPNOTSUPP;
    int err=tb_idle(e); if (err) return err;
    unsigned pixels=e->fb->pitch/(e->fb->bpp/8);
    if (e->kind==TB_BLADE) {
        uint32_t stride=(pixels>>3)<<20, format=stride|((e->fb->bpp>>4)<<29);
        w32(e,0x21c0,format); w32(e,0x21c4,format);
        w32(e,0x21b8,format); w32(e,0x21bc,format);
        w32(e,0x21d0,stride); w32(e,0x21d4,stride);
        w32(e,0x21c8,stride); w32(e,0x21cc,stride); w32(e,0x216c,0);
    } else {
        uint8_t op=(e->fb->bpp>>4)|xp_pitch(e->fb->pitch);
        w8(e,0x2125,op); e->operation=op|0x40;
        uint32_t stride=e->fb->pitch<<18;
        w32(e,0x2154,stride); w32(e,0x2150,stride); w8(e,0x2126,3);
    }
    err=tb_idle(e); if (err) return err;
    e->active=true; e->reason="active"; return 0;
}
