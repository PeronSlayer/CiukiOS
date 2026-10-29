/*
 *	drivers/video/aty/radeon_base.c
 *
 *	framebuffer driver for ATI Radeon chipset video boards
 *
 *	Copyright 2003	Ben. Herrenschmidt <benh@kernel.crashing.org>
 *	Copyright 2000	Ani Joshi <ajoshi@kernel.crashing.org>
 *
 *	i2c bits from Luca Tettamanti <kronos@kronoz.cjb.net>
 *
 *	Special thanks to ATI DevRel team for their hardware donations.
 *
 *	...Insert GPL boilerplate here...
 *
 *	Significant portions of this driver apdated from XFree86 Radeon
 *	driver which has the following copyright notice:
 *
 *	Copyright 2000 ATI Technologies Inc., Markham, Ontario, and
 *                     VA Linux Systems Inc., Fremont, California.
 *
 *	All Rights Reserved.
 *
 *	Permission is hereby granted, free of charge, to any person obtaining
 *	a copy of this software and associated documentation files (the
 *	"Software"), to deal in the Software without restriction, including
 *	without limitation on the rights to use, copy, modify, merge,
 *	publish, distribute, sublicense, and/or sell copies of the Software,
 *	and to permit persons to whom the Software is furnished to do so,
 *	subject to the following conditions:
 *
 *	The above copyright notice and this permission notice (including the
 *	next paragraph) shall be included in all copies or substantial
 *	portions of the Software.
 *
 *	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * 	EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *	MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *	NON-INFRINGEMENT.  IN NO EVENT SHALL ATI, VA LINUX SYSTEMS AND/OR
 *	THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 *	WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *	DEALINGS IN THE SOFTWARE.
 *
 *	XFree86 driver authors:
 *
 *	   Kevin E. Martin <martin@xfree86.org>
 *	   Rickard E. Faith <faith@valinux.com>
 *	   Alan Hourihane <alanh@fairlite.demon.co.uk>
 *
 */


// SPDX-License-Identifier: GPL-2.0
/* Origin: Linux v6.12 drivers/video/fbdev/aty/radeon_accel.c,
 * radeonfb_prim_fillrect/copyarea/engine_init; radeon_base.c idle/flush.
 * The accelerated functions there are patterned after the "ACCEL_MMIO"
 * branches in XFree86 (--dte). Native bounds, errors and transport below.
 * No PLL/reset, hardware WAIT packet, bus mastering, CP or GART programming. */
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "engine.h"

/* Linux v6.12 radeonfb.h IS_R300_VARIANT and radeon_accel.c engine_init.
 * Separate masks deliberately do not inherit future generations. */
static const struct radeon_group_info groups[] = {
    [RADEON_R100] = {"R100", RADEON_RECT | RADEON_CURSOR, 0},
    [RADEON_R200] = {"R200", RADEON_RECT | RADEON_CURSOR | RADEON_PATTERN | RADEON_WRITE, 0},
    [RADEON_R300] = {"R300", RADEON_RECT | RADEON_CURSOR | RADEON_PATTERN | RADEON_WRITE,
        RB2D_DC_AUTOFLUSH_ENABLE | RB2D_DC_DC_DISABLE_IGNORE_PE},
    [RADEON_R4XX] = {"R4xx", RADEON_RECT | RADEON_CURSOR | RADEON_PATTERN | RADEON_WRITE,
        RB2D_DC_AUTOFLUSH_ENABLE | RB2D_DC_DC_DISABLE_IGNORE_PE}
};

static uint32_t rd(struct radeon_engine *s, uint32_t r)
{ return s->io.read(s->io.ctx, r); }
static void wr(struct radeon_engine *s, uint32_t r, uint32_t v)
{ s->touched = true; s->io.write(s->io.ctx, r, v); }

bool radeon_format(const struct fb_device *d, uint32_t *datatype)
{
    if (!d || !d->present || !d->mapped || !d->width || !d->height ||
        d->width > 8192 || d->height > 8192 || !d->pitch ||
        (d->pitch & 63u) || d->pitch > 0x3fc0u ||
        d->blue_pos || d->red_size != d->blue_size ||
        (uint64_t)d->pitch * d->height > d->size) return false;
    uint32_t t;
    if (d->bpp == 16 && d->blue_size == 5 && d->green_pos == 5 &&
        ((d->green_size == 6 && d->red_pos == 11) ||
         (d->green_size == 5 && d->red_pos == 10))) t = d->green_size == 6 ? 4 : 3;
    else if ((d->bpp == 24 || d->bpp == 32) && d->blue_size == 8 &&
             d->green_size == 8 && d->green_pos == 8 && d->red_pos == 16)
        t = d->bpp == 24 ? 5 : 6;
    else return false;
    if ((uint64_t)d->width * (d->bpp / 8u) > d->pitch) return false;
    if (datatype) *datatype = t;
    return true;
}

/* One deadline covers all phases. Even a frozen/broken clock terminates.
 * A service pause releases the presenter after at most 1 ms of polling;
 * its generic in-flight drawing claim remains held across the release. */
struct poll_budget { uint64_t start, slice_start; unsigned polls, slice; };
static int wait_reg(struct radeon_engine *s, struct poll_budget *b,
                    uint32_t reg, uint32_t mask, uint32_t value, bool fifo)
{
    for (;;) {
        uint32_t v = rd(s, reg);
        if (reg == RBBM_STATUS) s->status = v;
        if (v != UINT32_MAX && (fifo ? (v & mask) >= value : (v & mask) == value)) return 0;
        uint64_t now=s->io.now(s->io.ctx);
        if (++b->polls >= RADEON_POLLS || now - b->start >= RADEON_WAIT_US)
            return -ETIMEDOUT;
        if (s->io.delay(s->io.ctx, RADEON_POLL_US)) return -EIO;
        b->slice += RADEON_POLL_US;
        if (s->pause && (b->slice >= 1000u || now - b->slice_start >= 1000u)) {
            b->slice = 0;
            int e = s->pause(s->pause_arg);
            if (e) return e;
            b->slice_start=s->io.now(s->io.ctx);
        }
    }
}
static int idle_proof(struct radeon_engine *s)
{
    struct poll_budget b = {.start = s->io.now(s->io.ctx)};
    b.slice_start=b.start;
    s->waits++;
    s->cpu_safe = false;
    if (s->unowned || rd(s, CP_CSQ_CNTL) || rd(s, MC_FB_LOCATION) != s->memory_location) {
        s->unowned = true;
        return -EIO;
    }
    int e = wait_reg(s, &b, RBBM_STATUS, 0x7fu, 64, true);
    if (!e) e = wait_reg(s, &b, RBBM_STATUS, GUI_ACTIVE, 0, false);
    if (!e) {
        /* Linux radeon_engine_flush: drain all posted FIFO/cache writes. */
        uint32_t cache=rd(s, DSTCACHE_CTLSTAT);
        if (cache == UINT32_MAX) return -EIO;
        wr(s, DSTCACHE_CTLSTAT, cache | RB2D_DC_FLUSH_ALL);
        e = wait_reg(s, &b, RBBM_STATUS, 0x7fu, 64, true);
    }
    if (!e) e = wait_reg(s, &b, DSTCACHE_CTLSTAT, RB2D_DC_BUSY, 0, false);
    if (!e) e = wait_reg(s, &b, RBBM_STATUS, GUI_ACTIVE | 0x7fu, 64, false);
    if (!e) { s->io.drain(s->io.ctx); s->cpu_safe = true; }
    return e;
}
void radeon_engine_quarantine(struct radeon_engine *s, const char *reason)
{
    s->active = false;
    s->quarantined = true;
    s->reason = reason;
    /* This is nonwaiting and does not establish idle. A failed cursor
     * disable keeps CPU drawing excluded even if 2D was already idle. */
    if (radeon_cursor_hide(s)) s->cpu_safe = false;
}
int radeon_engine_idle(struct radeon_engine *s)
{
    int e = idle_proof(s);
    if (e) {
        if (e == -ETIMEDOUT) s->timeouts++;
        radeon_engine_quarantine(s, e == -ETIMEDOUT ? "engine_timeout" : "engine_wait_failed");
    }
    return e;
}
static int sync_engine(void *ctx) { return radeon_engine_idle(ctx); }
static bool cpu_ready(void *ctx) { return ((struct radeon_engine *)ctx)->cpu_safe; }
static unsigned capabilities(void *ctx)
{
    struct radeon_engine *s = ctx;
    return s->active && !s->quarantined && (s->verified & RADEON_RECT) ?
        RADEON_RECT | (s->cursor && (s->group->features & RADEON_CURSOR) ? RADEON_CURSOR : 0u) : 0u;
}
static void set_pause(void *ctx, int (*pause)(void *), void *arg)
{ struct radeon_engine *s = ctx; s->pause = pause; s->pause_arg = arg; }
static int stop_engine(void *ctx)
{
    struct radeon_engine *s = ctx;
    s->active = false;
    int e = radeon_engine_idle(s);
    int hidden = radeon_cursor_hide(s);
    if (hidden) { s->cpu_safe = false; radeon_engine_quarantine(s, "cursor_disable_failed"); }
    return e ? e : hidden;
}
static bool rect_ok(const struct radeon_engine *s, const struct fb_rect *r)
{
    return r && r->x >= 0 && r->y >= 0 && r->width > 0 && r->height > 0 &&
        (uint64_t)(uint32_t)r->x + (uint32_t)r->width <= s->fb.width &&
        (uint64_t)(uint32_t)r->y + (uint32_t)r->height <= s->fb.height;
}
static int before_draw(struct radeon_engine *s)
{
    if (!s->active || s->quarantined) return -ENODEV;
    /* Never fix up a changed MC base silently: no writes to an unknown
     * address space, and no retry with a different engine this boot. */
    if (rd(s, MC_FB_LOCATION) != s->memory_location ||
        rd(s, CRTC_OFFSET) || rd(s, CRTC_OFFSET_CNTL) ||
        (rd(s, CRTC_PITCH) & 0x7ffu) * 8u * (s->fb.bpp / 8u) != s->fb.pitch ||
        ((rd(s, CRTC_GEN_CNTL) >> 8) & 15u) != s->datatype ||
        rd(s, SURFACE_CNTL) || rd(s, CP_CSQ_CNTL) || rd(s, RB2D_DSTCACHE_MODE) != s->cache_mode) {
        s->cpu_safe = false;
        s->unowned = true;
        radeon_engine_quarantine(s, "scanout_changed");
        return -EIO;
    }
    s->io.drain(s->io.ctx);
    return radeon_engine_idle(s);
}
static int fill_rect(void *ctx, const struct fb_rect *r, uint32_t colour)
{
    struct radeon_engine *s = ctx;
    if (!rect_ok(s, r)) return -EINVAL;
    int e = before_draw(s);
    if (e) return e;
    /* Empty FIFO has 64 slots; the whole packet fits without mid-command
     * waits. No launch precedes complete parameter validation. */
    s->cpu_safe = false;
    wr(s, DP_GUI_MASTER_CNTL, (s->datatype << 8) | GMC_CLR_CMP_CNTL_DIS |
        GMC_BRUSH_SOLID_COLOR | ROP3_P);
    wr(s, DP_BRUSH_FRGD_CLR, colour);
    wr(s, DP_WRITE_MSK, UINT32_MAX);
    wr(s, DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wr(s, DST_Y_X, ((uint32_t)r->y << 16) | (uint32_t)r->x);
    wr(s, DST_WIDTH_HEIGHT, ((uint32_t)r->width << 16) | (uint32_t)r->height);
    e = radeon_engine_idle(s);
    if (!e) s->commands++;
    return e;
}
static int copy_rect(void *ctx, const struct fb_rect *r, int32_t dx, int32_t dy)
{
    struct radeon_engine *s = ctx;
    struct fb_rect dst = {dx, dy, r ? r->width : 0, r ? r->height : 0};
    if (!rect_ok(s, r) || !rect_ok(s, &dst)) return -EINVAL;
    int e = before_draw(s);
    if (e) return e;
    uint32_t sx = r->x, sy = r->y, x = dx, y = dy, direction = 0;
    if (dx <= r->x) direction |= DST_X_LEFT_TO_RIGHT;
    else { sx += r->width - 1; x += r->width - 1; }
    if (dy <= r->y) direction |= DST_Y_TOP_TO_BOTTOM;
    else { sy += r->height - 1; y += r->height - 1; }
    s->cpu_safe = false;
    wr(s, DP_GUI_MASTER_CNTL, (s->datatype << 8) | GMC_CLR_CMP_CNTL_DIS |
        GMC_BRUSH_NONE | GMC_SRC_DSTCOLOR | ROP3_S | DP_SRC_SOURCE_MEMORY);
    wr(s, DP_WRITE_MSK, UINT32_MAX);
    wr(s, DP_CNTL, direction);
    wr(s, SRC_Y_X, (sy << 16) | sx);
    wr(s, DST_Y_X, (y << 16) | x);
    wr(s, DST_HEIGHT_WIDTH, ((uint32_t)r->height << 16) | (uint32_t)r->width);
    e = radeon_engine_idle(s);
    if (!e) s->commands++;
    return e;
}

/* X.Org ATI xf86-video-ati-6.14.6 src/radeon_accelfuncs.c, ACCEL_MMIO:
 * Mono8x8PatternFill, ScanlineImageWrite and SubsequentScanline. Original
 * ATI/VA Linux MIT notice is retained above. No queued WAIT_UNTIL packet. */
static bool feature_ready(const struct radeon_engine *s, unsigned feature)
{ return (s->group->features & feature) && (s->testing || (s->verified & feature)); }
static int pattern_rect(void *ctx, const struct fb_rect *r, const uint8_t pattern[8], uint32_t fg, uint32_t bg)
{
    struct radeon_engine *s=ctx;
    if (!pattern || !rect_ok(s,r)) return -EINVAL;
    if (!feature_ready(s,RADEON_PATTERN)) return -EOPNOTSUPP;
    int e=before_draw(s);
    if (e) return e;
    uint32_t data[2]={0};
    for (unsigned i=0;i<8;i++) data[i/4] |= (uint32_t)pattern[i] << ((i%4)*8);
    s->cpu_safe=false;
    wr(s,DP_GUI_MASTER_CNTL,(s->datatype<<8) | GMC_CLR_CMP_CNTL_DIS |
        GMC_BRUSH_8X8_MONO_FG_BG | GMC_BYTE_MSB_TO_LSB | ROP3_P);
    wr(s,DP_WRITE_MSK,UINT32_MAX);
    wr(s,DP_BRUSH_FRGD_CLR,fg);
    wr(s,DP_BRUSH_BKGD_CLR,bg);
    wr(s,BRUSH_DATA0,data[0]); wr(s,BRUSH_DATA1,data[1]);
    wr(s,DP_CNTL,DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wr(s,BRUSH_Y_X,0); /* absolute pattern origin, as in the legacy ABI */
    wr(s,DST_Y_X,((uint32_t)r->y<<16) | (uint32_t)r->x);
    wr(s,DST_HEIGHT_WIDTH,((uint32_t)r->height<<16) | (uint32_t)r->width);
    e=radeon_engine_idle(s);
    if (!e) s->commands++;
    return e;
}
static int write_rect(void *ctx, const struct fb_rect *dst, const struct fb_rect *clip,
                      const void *rows, uint32_t stride)
{
    struct radeon_engine *s=ctx;
    unsigned bytes=s->fb.bpp/8u;
    if (!rows || !dst || dst->width<=0 || dst->height<=0 || !rect_ok(s,clip) ||
        clip->x<dst->x || clip->y<dst->y ||
        (int64_t)clip->x+clip->width>(int64_t)dst->x+dst->width ||
        (int64_t)clip->y+clip->height>(int64_t)dst->y+dst->height ||
        (uint64_t)(uint32_t)dst->width*bytes>stride) return -EINVAL;
    uint64_t extent=(uint64_t)((uint32_t)dst->height-1u)*stride+(uint64_t)(uint32_t)dst->width*bytes;
    uintptr_t begin=(uintptr_t)rows, fb=(uintptr_t)s->fb.mapped;
    if (extent>UINTPTR_MAX-begin) return -EINVAL;
    if (begin<(uint64_t)fb+s->fb.size && (uint64_t)begin+extent>fb) return -EOPNOTSUPP;
    unsigned words=((unsigned)clip->width*bytes+3u)/4u;
    if (!feature_ready(s,RADEON_WRITE) || words>RADEON_UPLOAD_WORDS) return -EOPNOTSUPP;
    int e=before_draw(s);
    if (e) return e;
    uint64_t started=s->io.now(s->io.ctx), slice=started;
    unsigned slice_rows=0;
    for (int32_t row=0;row<clip->height;row++) {
        if (s->io.now(s->io.ctx)-started>=RADEON_WAIT_US) {
            s->timeouts++;
            radeon_engine_quarantine(s,"upload_timeout");
            return -ETIMEDOUT;
        }
        uint32_t data[RADEON_UPLOAD_WORDS]={0};
        size_t offset=(size_t)((int64_t)clip->y-dst->y+row)*stride+
            (size_t)((int64_t)clip->x-dst->x)*bytes;
        memcpy(data,(const uint8_t *)rows+offset,(unsigned)clip->width*bytes);
        unsigned y=(unsigned)(clip->y+row), x=(unsigned)clip->x;
        unsigned width=(unsigned)clip->width;
        if (bytes==2) width=(width+1u)&~1u;
        s->cpu_safe=false;
        wr(s,DP_GUI_MASTER_CNTL,(s->datatype<<8) | GMC_CLR_CMP_CNTL_DIS | GMC_DST_CLIPPING |
            GMC_BRUSH_NONE | GMC_SRC_DSTCOLOR | ROP3_S | GMC_BYTE_MSB_TO_LSB | DP_SRC_SOURCE_HOST_DATA);
        wr(s,DP_WRITE_MSK,UINT32_MAX);
        wr(s,DP_CNTL,DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
        wr(s,RBBM_GUICNTL,0); /* little-endian host, no byte swap */
        wr(s,SC_TOP_LEFT,0);
        wr(s,SC_BOTTOM_RIGHT,((y+1u)<<16) | (x+(unsigned)clip->width));
        wr(s,DST_Y_X,(y<<16) | x);
        wr(s,DST_HEIGHT_WIDTH,(1u<<16) | width);
        /* Empty FIFO was proved; all eight setup slots + <=48 payload
         * slots fit. No mid-transfer wait can strand a host-data command.
         * Every row terminates at HOST_DATA_LAST, never at HOST_DATA7. */
        for (unsigned i=0;i<words;) {
            unsigned count=words-i>8 ? 8 : words-i;
            uint32_t first=words-i>8 ? HOST_DATA0 : HOST_DATA_LAST-(count-1u)*4u;
            for (unsigned j=0;j<count;j++) wr(s,first+j*4u,data[i++]);
        }
        e=radeon_engine_idle(s);
        if (e) return e; /* may be partially executed; never clean refusal */
        if (row+1<clip->height) {
            uint64_t now=s->io.now(s->io.ctx);
            if (now-started>=RADEON_WAIT_US) {
                s->timeouts++;
                radeon_engine_quarantine(s,"upload_timeout");
                return -ETIMEDOUT;
            }
            /* Also slice between completed rows: repeated fast completions
             * must not monopolize the presenter. The row cap works with a
             * frozen clock, while the drawing claim stays in flight. */
            if (s->pause && (now-slice>=1000u || ++slice_rows>=16u)) {
                e=s->pause(s->pause_arg);
                if (e) {
                    radeon_engine_quarantine(s,"upload_cancelled");
                    return e==-EOPNOTSUPP ? -EIO : e;
                }
                slice=s->io.now(s->io.ctx); slice_rows=0;
            }
        }
    }
    s->commands++;
    return 0;
}

int radeon_engine_prepare(struct radeon_engine *s, const struct fb_device *d, const struct radeon_io *io,
                          enum radeon_group group)
{
    uint32_t datatype;
    if (!s || !io || !io->read || !io->write || !io->now || !io->delay || !io->drain ||
        !radeon_format(d, &datatype) || d->width<8 || d->height<4 ||
        group<RADEON_R100 || group>RADEON_R4XX) return -EOPNOTSUPP;
    /* Read-only qualification. A failure cannot submit or change pixels. */
    uint32_t crtc = io->read(io->ctx, CRTC_GEN_CNTL);
    uint32_t vram = io->read(io->ctx, CNFG_MEMSIZE);
    if ((crtc & (3u | CRTC_CUR_EN)) || !(crtc & (1u << 25)) ||
        ((crtc >> 8) & 15u) != datatype || io->read(io->ctx, CRTC_OFFSET) ||
        io->read(io->ctx, CRTC_OFFSET_CNTL) || io->read(io->ctx, SURFACE_CNTL) ||
        io->read(io->ctx, CP_CSQ_CNTL) ||
        (io->read(io->ctx, CRTC_PITCH) & 0x7ffu) * 8u * (d->bpp / 8u) != d->pitch ||
        vram==UINT32_MAX || vram < d->size) return -EOPNOTSUPP;
    uint32_t cache=io->read(io->ctx,RB2D_DSTCACHE_MODE);
    uint32_t location=io->read(io->ctx,MC_FB_LOCATION);
    if (cache==UINT32_MAX || location==UINT32_MAX) return -EOPNOTSUPP;
    memset(s, 0, sizeof(*s));
    s->group=&groups[group]; s->group_id=group;
    s->cache_mode=s->group->cache_bits ? cache | s->group->cache_bits : 0;
    s->fb = *d; s->io = *io; s->datatype = datatype;
    s->memory_location=location;
    s->local_base = location << 16;
    uint64_t local_end=((uint64_t)(location>>16)+1u)<<16;
    /* Preserve f2-103's zero-base R100 path: QEMU v9.2.0 ati.c does not
     * implement MC_FB_LOCATION, so zero is not a 64 KiB VRAM-size claim.
     * Its existing CNFG_MEMSIZE + firmware/PCI lease proof stays binding.
     * Added generations require a consistent complete MC window. */
    if (group==RADEON_R100 && !location) local_end=vram;
    if (local_end<=s->local_base || d->size>local_end-s->local_base) return -EOPNOTSUPP;
    s->local_bytes=local_end-s->local_base;
    s->pitch_offset = ((d->pitch / 64u) << 22) | (s->local_base >> 10);
    s->cursor_hidden = true; s->cpu_safe = true; s->reason = "pending";
    s->ops = (struct accel_ops){.ctx=s, .sync=sync_engine, .copy_rect=copy_rect,
        .fill_rect=fill_rect, .cursor_set=radeon_cursor_set, .cursor_move=radeon_cursor_move,
        .cursor_show=radeon_cursor_show, .stop=stop_engine, .capabilities=capabilities,
        .cpu_ready=cpu_ready, .set_pause=set_pause};
    if (s->group->features & RADEON_PATTERN) s->ops.pattern_rect=pattern_rect;
    if (s->group->features & RADEON_WRITE) s->ops.write_rect=write_rect;
    return 0;
}

int radeon_engine_activate(void *ctx)
{
    struct radeon_engine *s = ctx;
    /* Serialized before *any* hardware changes, including idle/cache flush. */
    int e = radeon_engine_idle(s);
    if (e) return e;
    wr(s, RB3D_CNTL, 0);
    wr(s, RB2D_DSTCACHE_MODE, s->cache_mode);
    if (rd(s,RB2D_DSTCACHE_MODE)!=s->cache_mode) {
        s->cpu_safe=false;
        radeon_engine_quarantine(s,"cache_setup_failed");
        return -EIO;
    }
    wr(s, DEFAULT_PITCH_OFFSET, s->pitch_offset);
    wr(s, DST_PITCH_OFFSET, s->pitch_offset);
    wr(s, SRC_PITCH_OFFSET, s->pitch_offset);
    wr(s, DEFAULT_SC_TOP_LEFT, 0);
    wr(s, DEFAULT_SC_BOTTOM_RIGHT, DEFAULT_SC_RIGHT_MAX | DEFAULT_SC_BOTTOM_MAX);
    e = radeon_engine_idle(s);
    if (e) return e;
    s->active = true;
    /* Actual native FILL and COPY read-back, with no software replay.
     * Save/restore only with proven idle, under the presenter's claim. */
    uint8_t saved[8*4*4], expected[8*4*4];
    uint32_t upload[8*4];
    unsigned bytes=s->fb.bpp/8u;
    struct fb_device ref=s->fb;
    ref.width=8; ref.height=4; ref.pitch=8*bytes; ref.size=ref.pitch*4; ref.mapped=expected;
    for (unsigned y=0;y<4;y++)
        memcpy(saved+y*ref.pitch,s->fb.mapped+y*s->fb.pitch,ref.pitch);
    memcpy(expected,saved,ref.size);
    struct fb_rect block={0,0,8,4}, left={0,0,4,4};
    uint32_t a=swdib_pack(&ref,0x12ab56u), b=swdib_pack(&ref,0xe76193u);
    s->testing=true;
    /* Compare each command with swdib, including both overlapping axes.
     * A fill cannot hide a no-op COPY, pattern or host-data transport. */
    for (unsigned step=0;step<6 && !e;step++) {
        if (!step || step==1) {
            const struct fb_rect *r=step ? &left : &block;
            uint32_t colour=step ? b : a;
            swdib_fill(&ref,r,colour); e=fill_rect(s,r,colour);
        } else if (step==2 || step==3) {
            struct fb_rect source={step==2 ? 0 : 2,step==2 ? 0 : 1,6,3};
            int x=step==2 ? 2 : 0, y=step==2 ? 1 : 0;
            swdib_copy(&ref,&source,x,y); e=copy_rect(s,&source,x,y);
        } else if (step==4) {
            if (!(s->group->features & RADEON_PATTERN)) continue;
            const uint8_t pattern[8]={0x80,0x41,0x22,0x14,0x08,0x05,0x02,0x01};
            struct fb_rect r={1,1,5,3};
            swdib_pattern(&ref,&r,pattern,b,a); e=pattern_rect(s,&r,pattern,b,a);
        } else {
            if (!(s->group->features & RADEON_WRITE)) continue;
            struct fb_rect clip={1,1,5,3};
            for (unsigned i=0;i<ARRAY_SIZE(upload);i++) upload[i]=0xa5000000u+i*34571u;
            swdib_convert(&ref,&block,upload,8*4);
            swdib_write(&ref,&block,&clip,upload,8*4);
            e=write_rect(s,&block,&clip,upload,8*4);
        }
        if (!e && !s->cpu_safe) e=-EIO;
        if (!e) for (unsigned y=0;y<4;y++)
            if (memcmp(expected+y*ref.pitch,s->fb.mapped+y*s->fb.pitch,ref.pitch)) { e=-EIO; break; }
        if (!e && step==3) s->verified |= RADEON_RECT;
        if (!e && step==4) s->verified |= RADEON_PATTERN;
        if (!e && step==5) s->verified |= RADEON_WRITE;
    }
    s->testing=false;
    /* Failure may have partially executed. Always prove completion before
     * restoring even this saved, idempotent diagnostic block. */
    if (!radeon_engine_idle(s) && !radeon_cursor_hide(s)) {
        for (unsigned y=0; y<4; y++)
            memcpy(s->fb.mapped + y*s->fb.pitch, saved+y*8*bytes, 8*bytes);
        s->io.drain(s->io.ctx);
    } else e = -EIO;
    if (e) radeon_engine_quarantine(s, "native_probe_failed");
    else s->reason = "none";
    return e;
}
