/*
	Haiku ATI video driver adapted from the X.org ATI driver.

	Copyright 1999, 2000 ATI Technologies Inc., Markham, Ontario,
						 Precision Insight, Inc., Cedar Park, Texas, and
						 VA Linux Systems Inc., Fremont, California.

	Copyright 2009 Haiku, Inc.  All rights reserved.
	Distributed under the terms of the MIT license.

	Authors:
	Gerald Zajac 2009
*/
/* Origin: Haiku r1beta5 (a1f00118db8b858feb73f64ac4dc3d038b82bf9b),
 * src/add-ons/accelerants/ati/rage128_{draw,init,cursor}.cpp.
 * CiukiOS: native synchronous port, bounded waits, exclusive setup, checked
 * geometry and legacy cursor conversion. No PLL reset, DMA or mode code.
 * Full permission notices: docs/licenses/haiku-rage128-NOTICES.txt. */
#include <ciuki/kernel.h>
#include "rage128.h"
#include "regs.h"

static uint32_t rd(struct r128_engine *s, unsigned reg)
{ return s->io.read(s->io.ctx,reg); }
static void wr(struct r128_engine *s, unsigned reg, uint32_t value)
{ s->io.write(s->io.ctx,reg,value); }

bool r128_format(const struct fb_device *d)
{
    if (!d || !d->present || !d->mapped || !d->width || !d->height ||
        d->width>8192 || d->height>8192 || !d->pitch ||
        (uint64_t)d->pitch*d->height>d->size) return false;
    bool rgb16=d->bpp==16 && d->red_size==5 && d->blue_size==5 &&
        d->blue_pos==0 && d->green_pos==5 &&
        ((d->green_size==6 && d->red_pos==11) ||
         (d->green_size==5 && d->red_pos==10));
    bool rgb32=d->bpp==32 && d->red_size==8 && d->red_pos==16 &&
        d->green_size==8 && d->green_pos==8 && d->blue_size==8 && d->blue_pos==0;
    if (!rgb16 && !rgb32) return false;
    unsigned bytes=d->bpp/8;
    return !(d->pitch%(8*bytes)) && d->pitch/(8*bytes)<=0x7ff &&
        (uint64_t)d->width*bytes<=d->pitch;
}
static unsigned datatype(const struct fb_device *d)
{ return d->bpp==32 ? 6 : d->green_size==6 ? 4 : 3; }
static bool valid(struct r128_engine *s)
{ return s->io.valid(s->io.ctx); }

/* No wait, allocation, file I/O or engine reset. A disable read-back is not
 * an engine idle proof. If the cursor cannot be disabled, retain exclusion. */
static bool hide(struct r128_engine *s)
{
    if (!s->io.valid(s->io.ctx)) { s->cursor_hidden=false; s->cpu_safe=false; return false; }
    uint32_t c=rd(s,R128_CRTC_GEN_CNTL);
    if (c==UINT32_MAX) { s->cursor_hidden=false; s->cpu_safe=false; return false; }
    wr(s,R128_CRTC_GEN_CNTL,c&~R128_CRTC_CUR_EN);
    c=rd(s,R128_CRTC_GEN_CNTL);
    s->cursor_hidden=c!=UINT32_MAX && !(c&R128_CRTC_CUR_EN);
    if (!s->cursor_hidden) s->cpu_safe=false;
    return s->cursor_hidden;
}
void r128_quarantine(struct r128_engine *s, const char *reason)
{
    s->active=false; s->quarantined=true; s->reason=reason;
    if (s->touched) (void)hide(s);
}
static int fault(struct r128_engine *s, int error, const char *reason)
{
    s->cpu_safe=false;
    r128_quarantine(s,reason);
    return error;
}

/* One budget covers FIFO drain, GUI idle and cache flush. Even a stopped
 * monotonic clock terminates; pause preserves fbdev's drawing/inflight claim.
 * Revalidate after a slice before any further register access. */
static int poll(struct r128_engine *s, unsigned reg, uint32_t mask,
                uint32_t expect, bool fifo, uint64_t start, unsigned *iterations,
                uint64_t *slice)
{
    while ((*iterations)++<R128_POLL_LIMIT) {
        if (!valid(s)) return -EIO;
        uint32_t value=rd(s,reg);
        s->status=value;
        if (value==UINT32_MAX) return -EIO;
        if (fifo ? (value&mask)>=expect : (value&mask)==expect) return 0;
        uint64_t now=s->io.now(s->io.ctx);
        if (now-start>=R128_WAIT_US) return -ETIMEDOUT;
        if (s->pause && now-*slice>=1000) {
            int e=s->pause(s->pause_arg);
            if (e) return e;
            *slice=s->io.now(s->io.ctx);
            if (*slice-start>=R128_WAIT_US) return -ETIMEDOUT;
        }
    }
    return -ETIMEDOUT;
}
static int fifo(struct r128_engine *s, unsigned count)
{
    unsigned n=0;
    uint64_t start=s->io.now(s->io.ctx), slice=start;
    return poll(s,R128_GUI_STAT,R128_GUI_FIFOCNT_MASK,count,true,start,&n,&slice);
}
static int idle(struct r128_engine *s)
{
    s->idle_waits++;
    s->cpu_safe=false;
    unsigned n=0;
    uint64_t start=s->io.now(s->io.ctx), slice=start;
    int e=poll(s,R128_GUI_STAT,R128_GUI_FIFOCNT_MASK,64,true,start,&n,&slice);
    if (!e) e=poll(s,R128_GUI_STAT,R128_GUI_ACTIVE,0,false,start,&n,&slice);
    if (!e) {
        uint32_t cache=rd(s,R128_PC_NGUI_CTLSTAT);
        if (cache==UINT32_MAX) e=-EIO;
        else {
            wr(s,R128_PC_NGUI_CTLSTAT,cache|R128_PC_FLUSH_ALL);
            e=poll(s,R128_PC_NGUI_CTLSTAT,R128_PC_BUSY,0,false,start,&n,&slice);
        }
    }
    if (!e && (!s->quarantined || s->cursor_hidden)) s->cpu_safe=true;
    return e;
}
static int sync_engine(void *ctx)
{
    struct r128_engine *s=ctx;
    if (!s->touched) return s->cpu_safe ? 0 : -EIO;
    /* A completed quarantine proof stays cached. A still unsafe guard may
     * only allow CPU access after another bounded idle/disable proof. */
    if (s->quarantined && s->cpu_safe) return 0;
    int e=idle(s);
    if (e) return fault(s,e,"idle_timeout");
    return s->cpu_safe ? 0 : -EIO;
}
static bool rect(struct r128_engine *s, const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>=0 && r->height>=0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height;
}
static int prepare(struct r128_engine *s, unsigned slots)
{
    if (!s->active || s->quarantined) { s->fallbacks++; return -ENODEV; }
    if (!valid(s)) return fault(s,-EIO,"stale_mode");
    s->io.drain(s->io.ctx);
    int e=fifo(s,slots);
    return e ? fault(s,e,"fifo_timeout") : 0;
}
static int complete(struct r128_engine *s)
{
    int e=idle(s);
    if (e) return fault(s,e,"completion_timeout");
    s->operations++;
    return 0;
}
static int fill(void *ctx, const struct fb_rect *r, uint32_t colour)
{
    struct r128_engine *s=ctx;
    if (!rect(s,r)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    int e=prepare(s,5); if (e) return e;
    s->cpu_safe=false;
    wr(s,R128_DP_GUI_MASTER_CNTL,s->master|R128_GMC_BRUSH_SOLID_COLOR|
        R128_GMC_SRC_DATATYPE_COLOR|R128_ROP3_P);
    wr(s,R128_DP_BRUSH_FRGD_CLR,colour&s->rgb_mask);
    wr(s,R128_DP_CNTL,R128_DST_X_LEFT_TO_RIGHT|R128_DST_Y_TOP_TO_BOTTOM);
    wr(s,R128_DST_Y_X,((uint32_t)r->y<<16)|(uint32_t)r->x);
    s->submitted=true;
    wr(s,R128_DST_WIDTH_HEIGHT,((uint32_t)r->width<<16)|(uint32_t)r->height);
    return complete(s);
}
static int copy(void *ctx, const struct fb_rect *r, int32_t dx, int32_t dy)
{
    struct r128_engine *s=ctx;
    if (!rect(s,r) || dx<0 || dy<0 ||
        (uint64_t)(uint32_t)dx+(uint32_t)r->width>s->fb.width ||
        (uint64_t)(uint32_t)dy+(uint32_t)r->height>s->fb.height) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    int e=prepare(s,5); if (e) return e;
    uint32_t sx=r->x, sy=r->y, x=dx, y=dy, dir=0;
    if (dx<=r->x) dir|=R128_DST_X_LEFT_TO_RIGHT;
    else { sx+=(uint32_t)r->width-1; x+=(uint32_t)r->width-1; }
    if (dy<=r->y) dir|=R128_DST_Y_TOP_TO_BOTTOM;
    else { sy+=(uint32_t)r->height-1; y+=(uint32_t)r->height-1; }
    s->cpu_safe=false;
    wr(s,R128_DP_GUI_MASTER_CNTL,s->master|R128_GMC_BRUSH_NONE|
        R128_GMC_SRC_DATATYPE_COLOR|R128_DP_SRC_SOURCE_MEMORY|R128_ROP3_S);
    wr(s,R128_DP_CNTL,dir);
    wr(s,R128_SRC_Y_X,(sy<<16)|sx);
    wr(s,R128_DST_Y_X,(y<<16)|x);
    s->submitted=true;
    wr(s,R128_DST_HEIGHT_WIDTH,((uint32_t)r->height<<16)|(uint32_t)r->width);
    return complete(s);
}
int r128_invert(struct r128_engine *s, const struct fb_rect *r)
{
    if (!rect(s,r)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return 0;
    int e=prepare(s,6); if (e) return e;
    s->cpu_safe=false;
    /* RGB-only write mask preserves reserved bits for the part C semantics. */
    wr(s,R128_DP_WRITE_MASK,s->rgb_mask);
    wr(s,R128_DP_GUI_MASTER_CNTL,s->master|R128_GMC_BRUSH_NONE|
        R128_GMC_SRC_DATATYPE_COLOR|R128_DP_SRC_SOURCE_MEMORY|R128_ROP3_Dn);
    wr(s,R128_DP_CNTL,R128_DST_X_LEFT_TO_RIGHT|R128_DST_Y_TOP_TO_BOTTOM);
    wr(s,R128_DST_Y_X,((uint32_t)r->y<<16)|(uint32_t)r->x);
    s->submitted=true;
    wr(s,R128_DST_WIDTH_HEIGHT,((uint32_t)r->width<<16)|(uint32_t)r->height);
    e=complete(s);
    if (!e) {
        e=prepare(s,1);
        if (!e) { wr(s,R128_DP_WRITE_MASK,UINT32_MAX); e=sync_engine(s); }
    }
    return e;
}
static int cursor_show(void *ctx, bool show)
{
    struct r128_engine *s=ctx;
    if (!show) {
        if (!s->touched) return 0;
        return hide(s) ? 0 : fault(s,-EIO,"cursor_disable_failed");
    }
    if (!s->cursor) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -ENODEV;
    if (!s->cursor_loaded) return -EINVAL;
    if (!valid(s)) return fault(s,-EIO,"stale_mode");
    uint32_t c=rd(s,R128_CRTC_GEN_CNTL);
    if (c==UINT32_MAX) return fault(s,-EIO,"dead_cursor_mmio");
    wr(s,R128_CRTC_GEN_CNTL,(c&~R128_CRTC_CUR_MODE_MASK)|R128_CRTC_CUR_EN);
    c=rd(s,R128_CRTC_GEN_CNTL);
    if (c==UINT32_MAX || !(c&R128_CRTC_CUR_EN) || (c&R128_CRTC_CUR_MODE_MASK))
        return fault(s,-EIO,"cursor_enable_failed");
    s->cursor_hidden=false;
    return 0;
}
static uint32_t cursor_rgb(struct r128_engine *s, uint32_t pixel)
{
    const struct fb_device *d=&s->fb;
    unsigned r=(pixel>>d->red_pos)&((1u<<d->red_size)-1);
    unsigned g=(pixel>>d->green_pos)&((1u<<d->green_size)-1);
    unsigned b=(pixel>>d->blue_pos)&((1u<<d->blue_size)-1);
    r=(255*r+((1u<<d->red_size)-1)/2)/((1u<<d->red_size)-1);
    g=(255*g+((1u<<d->green_size)-1)/2)/((1u<<d->green_size)-1);
    b=(255*b+((1u<<d->blue_size)-1)/2)/((1u<<d->blue_size)-1);
    return (r<<16)|(g<<8)|b;
}
static int cursor_set(void *ctx, const struct accel_cursor *c)
{
    struct r128_engine *s=ctx;
    if (!c) return -EINVAL;
    if (!s->cursor) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -ENODEV;
    if (!valid(s)) return fault(s,-EIO,"stale_mode");
    int e=cursor_show(s,false); if (e) return e;
    e=sync_engine(s); if (e) return e;
    for (unsigned y=0;y<64;y++) {
        for (unsigned x=0;x<8;x++) {
            unsigned shift=24-8*(x%4);
            uint8_t a=0xff, v=0;
            if (y<32 && x<4) {
                a=(uint8_t)(~c->mask[y]>>shift);
                v=(uint8_t)((c->image[y]&c->mask[y])>>shift);
            }
            s->cursor[y*16+x]=a;
            s->cursor[y*16+x+8]=v;
        }
    }
    s->io.drain(s->io.ctx);
    wr(s,R128_CUR_CLR0,cursor_rgb(s,c->background));
    wr(s,R128_CUR_CLR1,cursor_rgb(s,c->foreground));
    wr(s,R128_CUR_HORZ_VERT_OFF,R128_CUR_LOCK);
    wr(s,R128_CUR_HORZ_VERT_POSN,R128_CUR_LOCK);
    wr(s,R128_CUR_OFFSET,s->cursor_offset);
    if (rd(s,R128_CUR_OFFSET)!=s->cursor_offset ||
        rd(s,R128_CUR_CLR0)!=cursor_rgb(s,c->background) ||
        rd(s,R128_CUR_CLR1)!=cursor_rgb(s,c->foreground))
        return fault(s,-EIO,"cursor_load_failed");
    s->cursor_loaded=true;
    return 0;
}
static int cursor_move(void *ctx, int32_t x, int32_t y)
{
    struct r128_engine *s=ctx;
    if (!s->cursor) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -ENODEV;
    if (!s->cursor_loaded) return -EINVAL;
    /* The donor rounds negative origins to even offsets. Refuse odd offsets
     * rather than moving the shape by a pixel; do not assume unqualified
     * silicon accepts bits the donor masks off. Widen before negation. */
    if (x< -62 || y< -62 || x>8191 || y>8191 ||
        (x<0 && (x&1)) || (y<0 && (y&1))) return -EOPNOTSUPP;
    if (!valid(s)) return fault(s,-EIO,"stale_mode");
    uint32_t xo=x<0 ? (uint32_t)(-(int64_t)x) : 0;
    uint32_t yo=y<0 ? (uint32_t)(-(int64_t)y) : 0;
    uint32_t pos=((uint32_t)(x<0 ? 0 : x)<<16)|(uint32_t)(y<0 ? 0 : y);
    uint32_t off=(xo<<16)|yo;
    wr(s,R128_CUR_HORZ_VERT_OFF,R128_CUR_LOCK|off);
    wr(s,R128_CUR_HORZ_VERT_POSN,R128_CUR_LOCK|pos);
    wr(s,R128_CUR_OFFSET,s->cursor_offset+yo*16);
    uint32_t actual_pos=rd(s,R128_CUR_HORZ_VERT_POSN);
    uint32_t actual_off=rd(s,R128_CUR_HORZ_VERT_OFF);
    if (rd(s,R128_CUR_OFFSET)!=s->cursor_offset+yo*16 ||
        (actual_pos&~R128_CUR_LOCK)!=pos || (actual_off&~R128_CUR_LOCK)!=off)
        return fault(s,-EIO,"cursor_move_failed");
    return 0;
}
static int stop(void *ctx)
{
    struct r128_engine *s=ctx;
    s->active=false;
    if (!s->touched) return 0;
    if (!hide(s)) return fault(s,-EIO,"cursor_disable_failed");
    int e=sync_engine(s);
    if (!e) s->cursor_loaded=false;
    return e;
}
static unsigned caps(void *ctx)
{
    struct r128_engine *s=ctx;
    return s->active && !s->quarantined ? 1u|(s->cursor ? 2u : 0u) : 0;
}
static bool cpu_ready(void *ctx) { return ((struct r128_engine *)ctx)->cpu_safe; }
static void set_pause(void *ctx, int (*pause)(void *), void *arg)
{ struct r128_engine *s=ctx; s->pause=pause; s->pause_arg=arg; }

int r128_engine_init(struct r128_engine *s, const struct fb_device *d, struct r128_io io,
                     volatile uint8_t *cursor, uint32_t cursor_offset)
{
    if (s->quarantined) return -EIO;
    if (!r128_format(d) || !io.read || !io.write || !io.now || !io.drain || !io.valid ||
        (cursor && ((cursor_offset&0xfffu) || cursor_offset<(uint64_t)d->pitch*d->height ||
                    cursor_offset>0x07fff000u))) return -EOPNOTSUPP;
    s->io=io; s->fb=*d; s->cursor=cursor; s->cursor_offset=cursor_offset;
    s->ops=(struct accel_ops){.ctx=s,.sync=sync_engine,.copy_rect=copy,.fill_rect=fill,
        .cursor_set=cursor_set,.cursor_move=cursor_move,.cursor_show=cursor_show,
        .stop=stop,.capabilities=caps,.cpu_ready=cpu_ready,.set_pause=set_pause};
    s->reason="unsupported_scanout";
    /* Read-only scanout proof. Nonzero offsets/tiling and CRTC2 ownership
     * require a separate mode-provider contract. VBE mode setting stays intact. */
    uint32_t gen=rd(s,R128_CRTC_GEN_CNTL), pitch=rd(s,R128_CRTC_PITCH);
    uint32_t width=rd(s,R128_CRTC_H_TOTAL_DISP), height=rd(s,R128_CRTC_V_TOTAL_DISP);
    if (!valid(s) || gen==UINT32_MAX || pitch==UINT32_MAX ||
        (gen&(R128_CRTC_EN|R128_CRTC_EXT_DISP_EN))!=(R128_CRTC_EN|R128_CRTC_EXT_DISP_EN) ||
        (gen&3u) || ((gen>>8)&7u)!=datatype(d) || rd(s,R128_CRTC_OFFSET) ||
        rd(s,R128_CRTC_OFFSET_CNTL) || (pitch&0x7ffu)!=d->pitch/(8*(d->bpp/8)) ||
        (((width>>16)&0x1ffu)+1)*8!=d->width ||
        (((height>>16)&0x7ffu)+1)!=d->height ||
        (rd(s,R128_PM4_BUFFER_CNTL)&0xf0000000u)) return -EOPNOTSUPP;
    /* No reset can turn an unknown outstanding writer into a CPU-safe proof.
     * Mark touched before observing firmware engine state, even pre-launch. */
    s->touched=true; s->cpu_safe=false;
    int e=sync_engine(s); if (e) return e;
    if (!hide(s)) return fault(s,-EIO,"cursor_disable_failed");
    s->active=true;
    e=prepare(s,14);
    if (e) return e;
    s->rgb_mask=d->bpp==32 ? 0x00ffffffu : d->green_size==6 ? 0xffffu : 0x7fffu;
    s->master=(datatype(d)<<R128_GMC_DST_DATATYPE_SHIFT)|
        R128_GMC_CLR_CMP_CNTL_DIS|R128_GMC_AUX_CLIP_DIS;
    wr(s,R128_SCALE_3D_CNTL,0);
    wr(s,R128_DEFAULT_OFFSET,0);
    wr(s,R128_DEFAULT_PITCH,d->pitch/(8*(d->bpp/8)));
    wr(s,R128_AUX_SC_CNTL,0);
    uint32_t bound=((d->height-1)<<16)|(d->width-1);
    wr(s,R128_DEFAULT_SC_BOTTOM_RIGHT,bound);
    wr(s,R128_SC_TOP_LEFT,0);
    wr(s,R128_SC_BOTTOM_RIGHT,bound);
    wr(s,R128_DP_GUI_MASTER_CNTL,s->master|R128_GMC_BRUSH_SOLID_COLOR|R128_GMC_SRC_DATATYPE_COLOR);
    wr(s,R128_DST_BRES_ERR,0);
    wr(s,R128_DST_BRES_INC,0);
    wr(s,R128_DST_BRES_DEC,0);
    wr(s,R128_DP_WRITE_MASK,UINT32_MAX);
    wr(s,R128_DP_DATATYPE,rd(s,R128_DP_DATATYPE)&~R128_HOST_BIG_ENDIAN_EN);
    e=sync_engine(s);
    if (!e) { s->active=true; s->reason="active"; }
    return e;
}
