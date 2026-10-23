/*
	Haiku S3 Trio64 driver adapted from the X.org Virge driver.

	Copyright (C) 1994-1999 The XFree86 Project, Inc.  All Rights Reserved.

	Copyright 2007-2008 Haiku, Inc.  All rights reserved.
	Distributed under the terms of the MIT license.

	Authors:
	Gerald Zajac 2007-2008
*/
/* Origin: Haiku r1beta5 src/add-ons/accelerants/s3/{trio64,virge}_cursor.cpp.
 * Engine lifetime, bounded waits and Ciuki cursor translation are new glue.
 * See docs/licenses/haiku-s3-r1beta5-NOTICES.txt for the MIT permission. */
#include "private.h"

static bool hide(struct s3_device *s)
{
    if (!s->io.cr_read || !s->io.cr_write) return !s->touched;
    uint8_t v=s->io.cr_read(s->io.ctx,0x45);
    if (v==0xff) { s->cursor_unsafe=true; return false; }
    s->io.cr_write(s->io.ctx,0x45,v&~1u);
    s->cursor_hidden=!(s->io.cr_read(s->io.ctx,0x45)&1);
    s->cursor_unsafe=!s->cursor_hidden;
    return s->cursor_hidden;
}
void s3_fault(struct s3_device *s,const char *reason)
{
    s->active=false; s->quarantined=true; s->blocked=true;
    s->reason=reason ? reason : "engine_fault";
    if (!hide(s)) s->cpu_safe=false;
    /* Never turn quarantine itself into an idle proof. */
}
int s3_wait(struct s3_device *s,unsigned slots)
{
    uint64_t start=s->io.now(s->io.ctx), slice=start;
    for (unsigned polls=0;polls<4001;polls++) {
        if (s->backend->available(s,slots)) {
            if (!slots) {
                s->io.drain(s->io.ctx);
                s->cpu_safe=!s->cursor_unsafe;
            }
            return 0;
        }
        s->cpu_safe=false;
        uint64_t now=s->io.now(s->io.ctx);
        if (now-start>=20000) break;
        if (s->pause && (now-slice>=1000 || (polls && polls%200==0))) {
            int e=s->pause(s->pause_arg);
            if (e) { s3_fault(s,"wait_cancelled"); return e; }
            slice=s->io.now(s->io.ctx);
        }
        int e=s->io.delay(s->io.ctx,5);
        if (e) { s3_fault(s,"clock_failed"); return e; }
    }
    s->timeouts++;
    s3_fault(s,"engine_timeout");
    return -ETIMEDOUT;
}
void s3_launch(struct s3_device *s)
{
    s->io.drain(s->io.ctx);
    s->cpu_safe=false; s->touched=true;
}
bool s3_rect(const struct s3_device *s,const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>=0 && r->height>=0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height;
}
bool s3_cpu_ready(void *ctx) { return ((struct s3_device *)ctx)->cpu_safe; }
unsigned s3_caps(void *ctx)
{
    struct s3_device *s=ctx;
    return s->active && !s->quarantined ? 1u|(s->cursor_available ? 2u : 0u) : 0;
}
void s3_pause(void *ctx,int (*pause)(void *),void *arg)
{ struct s3_device *s=ctx; s->pause=pause; s->pause_arg=arg; }
int s3_sync(void *ctx)
{
    struct s3_device *s=ctx;
    if (!s->touched) return s->cpu_safe ? 0 : -EIO;
    if (!s->io.read) return s->cpu_safe ? 0 : -EIO;
    return s3_wait(s,0);
}
int s3_fill(void *ctx,const struct fb_rect *r,uint32_t colour)
{
    struct s3_device *s=ctx;
    if (!s->active || s->quarantined) return -EIO;
    if (!s3_rect(s,r)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    int e=s->backend->fill(s,r,colour);
    if (!e) s->fills++;
    return e;
}
int s3_copy(void *ctx,const struct fb_rect *r,int32_t dx,int32_t dy)
{
    struct s3_device *s=ctx;
    if (!s->active || s->quarantined) return -EIO;
    struct fb_rect dest={dx,dy,r ? r->width : 0,r ? r->height : 0};
    if (!s3_rect(s,r) || !s3_rect(s,&dest)) return -EOPNOTSUPP;
    if (!r->width || !r->height || (r->x==dx && r->y==dy)) return 0;
    int e=s->backend->copy(s,r,dx,dy);
    if (!e) s->copies++;
    return e;
}
static uint32_t cursor_colour(struct s3_device *s,uint32_t c)
{
    bool rgb=s->pci.device==0x8a10 || s->pci.device==0x8c01 || s->pci.device==0x8c03;
    if (s->fb.bpp!=16 || !rgb) return c;
    unsigned r=(c>>11)&31,g=(c>>5)&63,b=c&31;
    return ((r*255/31)<<16)|((g*255/63)<<8)|(b*255/31);
}
int s3_cursor_set(void *ctx,const struct accel_cursor *shape)
{
    struct s3_device *s=ctx;
    if (!shape) return -EINVAL;
    if (!(s3_caps(s)&2)) return -EOPNOTSUPP;
    if (!hide(s)) { s3_fault(s,"cursor_hide_failed"); return -EIO; }
    int e=s3_sync(s); if (e) return e;
    /* Hardware groups two MSB-left AND bytes, then two XOR bytes. Each
     * 64-pixel row is 16 bytes. Legacy mask=opaque, NOT an AND mask. */
    for (unsigned n=0;n<2048;n+=4) {
        s->cursor[n]=s->cursor[n+1]=255;
        s->cursor[n+2]=s->cursor[n+3]=0;
    }
    for (unsigned y=0;y<32;y++) for (unsigned b=0;b<4;b++) {
        unsigned off=y*16+(b/2)*4+(b%2), shift=24-b*8;
        s->cursor[off]=(uint8_t)(~shape->mask[y]>>shift);
        s->cursor[off+2]=(uint8_t)((shape->image[y]&shape->mask[y])>>shift);
    }
    s->io.drain(s->io.ctx);
    unsigned base=s->cursor_offset/1024;
    uint8_t first=s->backend->cursor_low_first ? 0x4d : 0x4c;
    uint8_t second=first==0x4c ? 0x4d : 0x4c;
    s->io.cr_write(s->io.ctx,first,first==0x4c ? base>>8 : base);
    s->io.cr_write(s->io.ctx,second,second==0x4c ? base>>8 : base);
    if (s->io.cr_read(s->io.ctx,0x4c)!=(base>>8) || s->io.cr_read(s->io.ctx,0x4d)!=(base&255)) {
        s3_fault(s,"cursor_base_failed"); return -EIO;
    }
    bool native16=s->fb.bpp==16 && s->pci.device!=0x8a10 &&
        s->pci.device!=0x8c01 && s->pci.device!=0x8c03;
    for (unsigned n=0;n<2;n++) {
        uint32_t c=cursor_colour(s,n ? shape->background : shape->foreground);
        (void)s->io.cr_read(s->io.ctx,0x45); /* reset colour stack */
        for (unsigned b=0;b<(native16 ? 4u : 3u);b++)
            s->io.cr_write(s->io.ctx,n ? 0x4b : 0x4a,c>>(8*(native16 ? b%2 : b)));
    }
    s->cursor_installed=true;
    return 0;
}
int s3_cursor_move(void *ctx,int32_t x,int32_t y)
{
    struct s3_device *s=ctx;
    if (!(s3_caps(s)&2)) return -EOPNOTSUPP;
    if (!s->cursor_installed) return -EINVAL;
    /* Donor edge offsets are even. Refuse unrepresentable positions before
     * altering registers, including INT_MIN (no signed negation overflow). */
    if (x < -30 || y < -30 || x>2047 || y>2047 ||
        (x<0 && (x&1)) || (y<0 && (y&1))) return -EOPNOTSUPP;
    uint8_t regs[]={0x4e,0x4f,0x47,0x46,0x49,0x48};
    unsigned xx=x<0 ? 0 : (unsigned)x, yy=y<0 ? 0 : (unsigned)y;
    uint8_t values[]={x<0 ? (uint8_t)-x : 0,y<0 ? (uint8_t)-y : 0,
        xx&255,(xx>>8)&7,yy&255,(yy>>8)&7};
    for (unsigned n=0;n<6;n++) s->io.cr_write(s->io.ctx,regs[n],values[n]);
    for (unsigned n=0;n<6;n++) if (s->io.cr_read(s->io.ctx,regs[n])!=values[n]) {
        s3_fault(s,"cursor_position_failed"); return -EIO;
    }
    return 0;
}
int s3_cursor_show(void *ctx,bool show)
{
    struct s3_device *s=ctx;
    if (!show) {
        if (hide(s)) return 0;
        s3_fault(s,"cursor_hide_failed"); return -EIO;
    }
    if (!(s3_caps(s)&2)) return -EOPNOTSUPP;
    if (!s->cursor_installed) return -EINVAL;
    uint8_t v=s->io.cr_read(s->io.ctx,0x45);
    s->io.cr_write(s->io.ctx,0x45,v|1);
    s->cursor_hidden=!(s->io.cr_read(s->io.ctx,0x45)&1);
    if (s->cursor_hidden) { s3_fault(s,"cursor_show_failed"); return -EIO; }
    return 0;
}
int s3_stop(void *ctx)
{
    struct s3_device *s=ctx;
    s->active=false;
    if (!hide(s)) { s3_fault(s,"cursor_hide_failed"); return -EIO; }
    int e=s3_sync(s); if (e) return e;
    s->cursor_installed=false;
    return s3_access_end(s);
}
int s3_engine_init(struct s3_device *s)
{
    s->ops=(struct accel_ops){.ctx=s,.sync=s3_sync,.copy_rect=s3_copy,.fill_rect=s3_fill,
        .cursor_set=s3_cursor_set,.cursor_move=s3_cursor_move,.cursor_show=s3_cursor_show,
        .stop=s3_stop,.capabilities=s3_caps,.cpu_ready=s3_cpu_ready,.set_pause=s3_pause};
    if (!s->backend->firmware(s)) { s->reason="firmware_mode_unsupported"; return -EOPNOTSUPP; }
    s->saved_cr53=s->io.cr_read(s->io.ctx,0x53);
    if (s->saved_cr53==0xff) return -EOPNOTSUPP;
    if (!(s->saved_cr53&8)) {
        s->io.cr_write(s->io.ctx,0x53,s->saved_cr53|8);
        s->mmio_changed=true;
        if (!(s->io.cr_read(s->io.ctx,0x53)&8)) return -EIO;
    }
    s->cpu_safe=false; s->touched=true; /* exclusive first status proof */
    if (!hide(s)) { s3_fault(s,"cursor_hide_failed"); return -EIO; }
    int e=s3_sync(s); if (e) return e;
    /* CR45.4 is right-side cursor storage; CR55.4 selects the X11 truth
     * table. This port uses contiguous Windows AND/XOR storage only. */
    s->cursor_available=s->cursor && !(s->io.cr_read(s->io.ctx,0x45)&0x10) &&
        !(s->io.cr_read(s->io.ctx,0x55)&0x10);
    e=s->backend->configure(s);
    if (e) return e;
    s->active=true; s->reason="none";
    return 0;
}
int s3_cursor_test(const struct accel_ops *ops,uint64_t deadline,bool (*cancel)(void *),void *ctx)
{
    if (!ops || !(ops->capabilities(ops->ctx)&2)) return -EOPNOTSUPP;
    struct accel_cursor transparent={0};
    int e=0;
    for (unsigned n=0;n<4 && !e;n++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        e=n==0 ? ops->cursor_show(ops->ctx,false) : n==1 ? ops->cursor_set(ops->ctx,&transparent) :
            n==2 ? ops->cursor_move(ops->ctx,0,0) : ops->cursor_show(ops->ctx,true);
    }
    int hidden=ops->cursor_show(ops->ctx,false);
    return e ? e : hidden;
}
