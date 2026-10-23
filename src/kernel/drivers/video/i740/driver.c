
/**************************************************************************

Copyright 1998-1999 Precision Insight, Inc., Cedar Park, Texas.
All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/*
 * Authors:
 *   Daryll Strauss <daryll@precisioninsight.com>
 *
 */


/* Origin: X.Org xf86-video-i740, commit 1f13c18a17914a6e919e604d1dc5589eb18f0141,
 * src/{i740_accel.c,i740_cursor.c,i740_macros.h}. Native bounded port.
 * Original donor copyright/permission notice is retained at the top. */
#include "driver.h"
#include "registers.h"

#define XRX_PORT 0x3d6u
static struct i740_engine engine;
static uint8_t xr(struct video_native *s, uint8_t index)
{ return s->io.indexed_read(s->io.ctx,XRX_PORT,index); }
static void xw(struct video_native *s, uint8_t index, uint8_t value)
{ s->io.indexed_write(s->io.ctx,XRX_PORT,index,value); }
static int fifo_idle(struct video_native *s)
{
    uint32_t lp=video_read(s,8,LP_FIFO_COUNT), hp=video_read(s,8,HP_FIFO_COUNT);
    uint32_t status=video_read(s,32,BITBLT_CONTROL), pio=xr(s,BITBLT_CNTL);
    if (lp>15 || hp>15 || status==UINT32_MAX || pio==255) return -EIO;
    return lp || hp || (status&BLTR_STATUS) || (pio&BITBLT_STATUS) ? -EAGAIN : 0;
}
static int hide(struct video_native *s)
{
    uint8_t conf=xr(s,PIXPIPE_CONFIG_0);
    if (conf==255) return -EIO;
    xw(s,PIXPIPE_CONFIG_0,conf&~HW_CURSOR_ENABLE);
    return xr(s,PIXPIPE_CONFIG_0)==(conf&~HW_CURSOR_ENABLE) ? 0 : -EIO;
}
static int restore(struct video_native *s)
{
    struct i740_engine *e=(struct i740_engine *)s;
    if (e->cursor_saved_valid) {
        for (unsigned i=0;i<4096;i++) e->cursor[i]=e->cursor_saved[i];
        s->io.drain(s->io.ctx);
        static const uint8_t regs[]={CURSOR_CONTROL,CURSOR_BASEADDR_LO,CURSOR_BASEADDR_HI,
            CURSOR_X_LO,CURSOR_X_HI,CURSOR_Y_LO,CURSOR_Y_HI};
        for (unsigned i=0;i<ARRAY_SIZE(regs);i++) {
            xw(s,regs[i],e->saved_cursor[i]);
            if (xr(s,regs[i])!=e->saved_cursor[i]) return -EIO;
        }
        uint8_t conf=xr(s,PIXPIPE_CONFIG_0);
        xw(s,PIXPIPE_CONFIG_0,conf|EXTENDED_PALETTE);
        video_palette_load(s,s->palette_saved);
        xw(s,PIXPIPE_CONFIG_0,conf);
    }
    return hide(s);
}
static int submit(struct i740_engine *e, const uint32_t words[12])
{
    struct video_native *s=&e->native;
    if (!s->ready || s->quarantined) return -EIO;
    int err=video_sync(s);
    if (err) return err;
    /* Empty LP FIFO has 15 entries. A complete 12-word full-load command
     * cannot block waiting for CPU mono data, even if the caller is preempted. */
    s->io.drain(s->io.ctx); s->cpu_safe=false;
    for (unsigned i=0;i<12;i++) video_write(s,32,LP_FIFO,words[i]);
    err=video_sync(s);
    if (!err) s->operations++;
    return err;
}
static int packet(struct i740_engine *e, uint32_t control, uint32_t colour,
                  uint32_t src, uint32_t dst, uint32_t width, uint32_t height)
{
    struct video_native *s=&e->native;
    uint32_t words[12]={0x6000000a,s->fb.pitch<<16|s->fb.pitch,colour,0,0,
        control,0,src,dst,0,0,height<<16|width};
    return submit(e,words);
}
static int fill(void *ctx, const struct fb_rect *r, uint32_t colour)
{
    struct i740_engine *e=ctx; struct video_native *s=&e->native;
    if (!video_rect(s,r)) return -EOPNOTSUPP;
    return packet(e,SOLID_PAT_SELECT|PAT_IS_MONO|0xf0,colour,0,
        (uint32_t)r->y*s->fb.pitch+(uint32_t)r->x*(s->fb.bpp/8),
        (uint32_t)r->width*(s->fb.bpp/8),(uint32_t)r->height);
}
static int copy(void *ctx, const struct fb_rect *r, int32_t x, int32_t y)
{
    struct i740_engine *e=ctx; struct video_native *s=&e->native;
    struct fb_rect dst={x,y,r ? r->width : 0,r ? r->height : 0};
    if (!video_rect(s,r) || !video_rect(s,&dst)) return -EOPNOTSUPP;
    uint32_t bytes=s->fb.bpp/8, width=(uint32_t)r->width*bytes;
    uint32_t sx=(uint32_t)r->x*bytes, dx=(uint32_t)x*bytes, sy=r->y, dy=y, control=0xcc;
    if (x>r->x) { control|=BLT_RIGHT_TO_LEFT; sx+=width-1; dx+=width-1; }
    if (y>r->y) { control|=BLT_BOT_TO_TOP; sy+=r->height-1; dy+=r->height-1; }
    return packet(e,control,0,sy*s->fb.pitch+sx,dy*s->fb.pitch+dx,width,r->height);
}
static int pattern(void *ctx, const struct fb_rect *r, const uint8_t bits[8], uint32_t fg, uint32_t bg)
{
    struct i740_engine *e=ctx; struct video_native *s=&e->native;
    if (!e->cursor_saved_valid || !bits || !video_rect(s,r)) return -EOPNOTSUPP;
    if (!s->ready || s->quarantined) return -EIO;
    int err=video_sync(s);
    if (err) return err;
    /* Eight packed mono rows, disjoint from the 1024-byte cursor image. The
     * complete page is reserved/saved; global x/y modulo 8 is the origin. */
    for (unsigned i=0;i<8;i++) e->cursor[1024+i]=bits[i];
    s->io.drain(s->io.ctx);
    uint32_t words[12]={0x6000000a,s->fb.pitch<<16|s->fb.pitch,bg,fg,0,
        PAT_IS_MONO|0xf0|((uint32_t)r->y<<20&PAT_VERT_ALIGN),e->cursor_offset+1024,
        0,(uint32_t)r->y*s->fb.pitch+(uint32_t)r->x*(s->fb.bpp/8),0,0,
        (uint32_t)r->height<<16|(uint32_t)r->width*(s->fb.bpp/8)};
    return submit(e,words);
}
static int cursor_show(void *ctx, bool show)
{
    struct i740_engine *e=ctx; struct video_native *s=&e->native;
    if (!s->cursor_available) return -EOPNOTSUPP;
    if (!show) { int err=hide(s); return err ? video_fault(s,"cursor_hide_readback",err) : 0; }
    if (!s->shape) return -EINVAL;
    if (!s->ready || s->quarantined) return -EIO;
    uint8_t conf=xr(s,PIXPIPE_CONFIG_0);
    if (conf==255) return video_fault(s,"cursor_dead_read",-EIO);
    xw(s,CURSOR_BASEADDR_LO,(e->cursor_offset&0xf000)>>8);
    xw(s,CURSOR_BASEADDR_HI,(e->cursor_offset&0x3f0000)>>16);
    xw(s,CURSOR_CONTROL,CURSOR_ORIGIN_DISPLAY|CURSOR_MODE_64_3C);
    xw(s,PIXPIPE_CONFIG_0,conf|HW_CURSOR_ENABLE);
    if (xr(s,CURSOR_BASEADDR_LO)!=((e->cursor_offset&0xf000)>>8) ||
        xr(s,CURSOR_BASEADDR_HI)!=((e->cursor_offset&0x3f0000)>>16) ||
        xr(s,CURSOR_CONTROL)!=(CURSOR_ORIGIN_DISPLAY|CURSOR_MODE_64_3C) ||
        xr(s,PIXPIPE_CONFIG_0)!=(conf|HW_CURSOR_ENABLE)) return video_fault(s,"cursor_show_readback",-EIO);
    return 0;
}
static int cursor_set(void *ctx, const struct accel_cursor *c)
{
    struct i740_engine *e=ctx; struct video_native *s=&e->native;
    if (!s->cursor_available) return -EOPNOTSUPP;
    if (!c) return -EINVAL;
    if (!s->ready || s->quarantined) return -EIO;
    uint8_t image[1024]; video_cursor_bits(image,c);
    int err=hide(s);
    if (err) return video_fault(s,"cursor_hide_readback",err);
    for (unsigned i=0;i<1024;i++) e->cursor[i]=image[i];
    s->io.drain(s->io.ctx);
    uint8_t conf=xr(s,PIXPIPE_CONFIG_0);
    xw(s,PIXPIPE_CONFIG_0,conf|EXTENDED_PALETTE);
    uint8_t colours[6];
    for (unsigned n=0;n<2;n++) {
        uint32_t rgb=video_cursor_rgb(&s->fb,n ? c->foreground : c->background);
        for (unsigned b=0;b<3;b++) colours[n*3+b]=(uint8_t)((rgb>>(16-b*8)&255) >> ((conf&DAC_8_BIT) ? 0 : 2));
    }
    video_palette_load(s,colours);
    xw(s,PIXPIPE_CONFIG_0,conf);
    if (xr(s,PIXPIPE_CONFIG_0)!=conf) return video_fault(s,"cursor_palette_readback",-EIO);
    s->shape=true; return 0;
}
static int cursor_move(void *ctx, int32_t x, int32_t y)
{
    struct video_native *s=ctx;
    if (!s->cursor_available) return -EOPNOTSUPP;
    if (!s->shape) return -EINVAL;
    if (x < -2047 || y < -2047 || x>2047 || y>2047) return -EOPNOTSUPP;
    if (!s->ready || s->quarantined) return -EIO;
    uint32_t px=x<0 ? (uint32_t)-x : (uint32_t)x, py=y<0 ? (uint32_t)-y : (uint32_t)y;
    uint8_t values[]={(uint8_t)px,(uint8_t)((px>>8)|(x<0 ? 0x80 : 0)),
        (uint8_t)py,(uint8_t)((py>>8)|(y<0 ? 0x80 : 0))};
    for (unsigned i=0;i<4;i++) {
        xw(s,(uint8_t)(CURSOR_X_LO+i),values[i]);
        if (xr(s,(uint8_t)(CURSOR_X_LO+i))!=values[i]) return video_fault(s,"cursor_position_readback",-EIO);
    }
    return 0;
}
int i740_engine_prepare(struct i740_engine *e)
{
    struct video_native *s=&e->native;
    s->idle=fifo_idle; s->hide=hide; s->restore=restore;
    if (!video_format(&s->fb) || xr(s,PIXPIPE_CONFIG_0)==255 ||
        (xr(s,PIXPIPE_CONFIG_0)&HW_CURSOR_ENABLE) || video_wait(s,fifo_idle)) return -EOPNOTSUPP;
    e->cursor_saved_valid=e->cursor && e->cursor_offset && !(e->cursor_offset&4095) &&
        e->cursor_offset>=s->fb.size && e->cursor_offset<=0x3ff000;
    if (e->cursor_saved_valid) {
        static const uint8_t regs[]={CURSOR_CONTROL,CURSOR_BASEADDR_LO,CURSOR_BASEADDR_HI,
            CURSOR_X_LO,CURSOR_X_HI,CURSOR_Y_LO,CURSOR_Y_HI};
        for (unsigned i=0;i<ARRAY_SIZE(regs);i++) e->saved_cursor[i]=xr(s,regs[i]);
        for (unsigned i=0;i<4096;i++) e->cursor_saved[i]=e->cursor[i];
        uint8_t conf=xr(s,PIXPIPE_CONFIG_0);
        xw(s,PIXPIPE_CONFIG_0,conf|EXTENDED_PALETTE); video_palette_save(s);
        xw(s,PIXPIPE_CONFIG_0,conf);
    }
    s->cursor_available=e->cursor_saved_valid;
    s->ops=(struct accel_ops){.ctx=s,.sync=video_sync,.copy_rect=copy,.fill_rect=fill,
        .pattern_rect=e->cursor_saved_valid ? pattern : 0,
        .cursor_set=cursor_set,.cursor_move=cursor_move,.cursor_show=cursor_show,
        .stop=video_stop,.capabilities=video_capabilities,.cpu_ready=video_cpu_ready,.set_pause=video_pause};
    s->touched=true; s->ready=true; s->cpu_safe=true; return 0;
}
static const struct display_match matches[]={{0x8086,0x7800,3,255},{0x8086,0x00d1,3,255}};
static bool probe(const struct pci_func *p)
{
    return p && p->vendor==0x8086 && (p->device==0x7800 || p->device==0x00d1) &&
        p->class_code==3 && !p->subclass && !(p->header_type&0x7f) && (p->command&3)==3 &&
        !(p->bar[0]&7) && !(p->bar[1]&7) && (p->bar[0]&~15u) && (p->bar[1]&~15u);
}
static bool eligible(const struct fb_device *fb)
{
    struct video_native *s=&engine.native; const char *why;
    if (display_accel_disabled(&why)) { s->reason=why; return false; }
    if (s->quarantined || s->attempted) return false;
    if (!video_format(fb)) { s->reason="unsupported_format"; return false; }
    return true;
}
static const struct accel_ops *start(const struct fb_device *fb, struct vfs *vfs)
{
    (void)vfs;
    struct i740_engine *e=&engine; struct video_native *s=&e->native;
    const struct pci_func *p=display_driver_owner(), *proved=0;
    const struct display_driver *table[]={&i740_driver}; const char *why;
    if (display_accel_disabled(&why) || s->quarantined || !s->armed || !video_format(fb) ||
        fb!=fbdev_get() || display_driver_select(fb,table,1,&proved)!=&i740_driver || p!=proved) return 0;
    s->fb=*fb; s->cpu_safe=true; s->reason="transport_refused"; s->pending_record=true;
    uint32_t base=p->bar[1]&~15u;
    if ((base&0x7ffff) || !video_disjoint(base,0x80000,fb->phys,g_boot.display.vram_bytes)) return 0;
    void *mmio=video_map(s,RES_MMIO,base,0x80000);
    if (!mmio || !video_map(s,RES_PORT,0x3b0,0x30)) goto fail;
    video_hardware_io(s,mmio);
    uint32_t pitch=((uint32_t)s->io.indexed_read(s->io.ctx,0x3d4,0x13)|
        (uint32_t)s->io.indexed_read(s->io.ctx,0x3d4,EXT_OFFSET)<<8)*8;
    if (pitch!=fb->pitch || (xr(s,BITBLT_CNTL)&COLEXP_MODE)!=(fb->bpp==16 ? COLEXP_16BPP : COLEXP_24BPP) ||
        (xr(s,PIXPIPE_CONFIG_1)&DISPLAY_COLOR_MODE)!=(fb->bpp==16 ? DISPLAY_16BPP_MODE : DISPLAY_24BPP_MODE) ||
        s->io.indexed_read(s->io.ctx,0x3d4,0x0c) || s->io.indexed_read(s->io.ctx,0x3d4,0x0d) ||
        (s->io.indexed_read(s->io.ctx,0x3d4,EXT_START_ADDR)&0x7f) ||
        s->io.indexed_read(s->io.ctx,0x3d4,EXT_START_ADDR_HI) ||
        (video_read(s,8,COMPARS)&TWO_D_INST_DISABLE) || (xr(s,PIXPIPE_CONFIG_0)&HW_CURSOR_ENABLE)) goto fail;
    e->cursor_offset=(uint32_t)(xr(s,CURSOR_BASEADDR_LO)&0xf0)<<8|
        (uint32_t)(xr(s,CURSOR_BASEADDR_HI)&0x3f)<<16;
    e->cursor=0;
    if (e->cursor_offset>=fb->size && e->cursor_offset<=0x3ff000 &&
        (uint64_t)e->cursor_offset+4096<=g_boot.display.vram_bytes)
        e->cursor=video_map(s,RES_MMIO,fb->phys+e->cursor_offset,4096);
    if (i740_engine_prepare(e)) goto fail;
    if (fbdev_accel_probe(fb,video_probe_pixels,s)) goto fail;
    return &s->ops;
fail:
    if (s->touched) (void)video_stop(s);
    video_release(s); return 0;
}
static const struct accel_ops *fallback(void)
{ struct video_native *s=&engine.native; if (s->touched) { (void)video_sync(s); return &s->ops; } return 0; }
static int begin(struct vfs *v) { return video_marker_begin(&engine.native,v); }
static int end(struct vfs *v) { return video_marker_end(&engine.native,v); }
static int failed(struct vfs *v) { return video_marker_failed(&engine.native,v); }
static void record(void) { video_record(&engine.native); }
static void quarantine(const char *why) { (void)video_fault(&engine.native,why,-EIO); }
static bool summary(bool *r, bool *u, const char **why) { return video_summary(&engine.native,r,u,why); }
static void snapshot(struct ciuki_accel_info *out) { video_snapshot(&engine.native,out); }
static struct i740_engine engine={.native={.id="i740",.name="Intel i740",
    .marker="C:/system/settings/accel-i740.cfg",.reason="pending",.cpu_safe=true}};
const struct display_driver i740_driver={.id="i740",.attempt_files={"C:/system/settings/accel.cfg","C:/system/settings/accel-i740.cfg"},.name="Intel i740",.matches=matches,
    .match_count=ARRAY_SIZE(matches),.framebuffer_bar=0,.probe=probe,.eligible=eligible,.start=start,
    .fallback=fallback,.attempt_begin=begin,.attempt_end=end,.attempt_failed=failed,.record=record,
    .quarantine=quarantine,.summary=summary,.snapshot=snapshot,.cursor_test=video_cursor_test};
