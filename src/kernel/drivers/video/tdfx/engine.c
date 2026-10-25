/*
    Copyright 2010 Haiku, Inc.  All rights reserved.
    Distributed under the terms of the MIT license.

    Authors:
    Gerald Zajac
*/
/* Origin: haiku/haiku r1beta5 (a1f00118db8b858feb73f64ac4dc3d038b82bf9b),
 * src/add-ons/accelerants/3dfx/{3dfx_draw,3dfx_cursor,3dfx_init}.cpp and 3dfx.h.
 * Ported register/packing fragments; native safety/lifecycle glue is CiukiOS.
 * Idle fence follows 3dfx_mode.cpp:TDFX_WaitForIdle; its notice is in NOTICE.
 * No mode, DMA, AGP or PLL programming. See docs/design/tdfx-2d.md.
 * SPDX-License-Identifier: MIT
 */
/*
	Some of the code in this source file was adapted from the X.org tdfx
	video driver, and was covered by the following copyright and license.
	--------------------------------------------------------------------------

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
*/
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "engine.h"
#include "registers.h"

static uint32_t xy(uint32_t x, uint32_t y) { return x | (y << 16); }
static unsigned caps(void *ctx)
{
    struct tdfx_engine *e=ctx;
    return e->active && !e->quarantined ? 1u | (e->cursor ? 2u : 0u) : 0;
}
static bool cpu_ready(void *ctx) { return ((struct tdfx_engine *)ctx)->cpu_safe; }

bool tdfx_layout(const struct fb_device *f)
{
    if (!f || !f->present || !f->mapped || !f->width || !f->height ||
        f->width>4095 || f->height>4095 || !f->pitch || f->pitch>0x3fff ||
        (uint64_t)f->width*(f->bpp/8u)>f->pitch ||
        (uint64_t)f->height*f->pitch>f->size || f->pitch%8) return false;
    /* The donor exposes RGB565 and XRGB8888. It explicitly does not use
     * 24bpp; arbitrary VBE masks and RGB555 stay on the software engine. */
    return f->blue_pos==0 && f->green_pos==5 && f->red_pos==11 &&
        f->blue_size==5 && f->green_size==6 && f->red_size==5 && f->bpp==16 ? true :
        f->bpp==32 && f->blue_pos==0 && f->green_pos==8 && f->red_pos==16 &&
        f->blue_size==8 && f->green_size==8 && f->red_size==8;
}
uint32_t tdfx_vram(const struct tdfx_io *io, uint16_t device)
{
    uint32_t a=io->read(io->ctx,DRAM_INIT0), b=io->read(io->ctx,DRAM_INIT1);
    if (a==UINT32_MAX || b==UINT32_MAX) return 0;
    uint32_t count=(a&(1u<<26)) ? 8u : 4u;
    uint32_t mb=device==9 ? 1u<<((a>>27)&7u) :
        (b&(1u<<30)) || (a&(1u<<27)) ? 2u : 1u;
    uint32_t size=mb*count*1024u*1024u;
    /* Conservative aperture limit, not a destructive size probe. */
    return size<=32u*1024u*1024u ? size : 0;
}
static int fault(struct tdfx_engine *e, const char *reason, int error)
{
    e->quarantined=true; e->active=false; e->cpu_safe=false;
    e->reason=reason; e->fallbacks++;
    return error;
}
/* Every write reserves FIFO capacity first. The donor's low five STATUS
 * bits count empty slots. Never write a full FIFO (PCI stall risk), even
 * during recovery. Both the elapsed clock and frozen-clock cap apply.
 * No file I/O, logging, reset, or bus-master enable occurs in these waits. */
static int wait_status(struct tdfx_engine *e, unsigned slots, bool idle)
{
    uint64_t start=e->io.now(e->io.ctx), slice=start;
    unsigned stable=0;
    for (unsigned i=0;i<TDFX_POLL_LIMIT;i++) {
        uint32_t s=e->io.read(e->io.ctx,STATUS);
        e->status=s;
        uint64_t now=e->io.now(e->io.ctx);
        if (s==UINT32_MAX) return fault(e,"register_unavailable",-EIO);
        if (now-start>=TDFX_WAIT_US) break;
        if ((s&31u)>=slots && (!idle || !(s&STATUS_BUSY))) {
            if (!idle || ++stable==3) return 0;
        } else stable=0;
        if (e->pause && (now-slice>=1000u || (i && i%200u==0))) {
            int err=e->pause(e->pause_arg);
            if (err) return fault(e,"wait_cancelled",err);
            slice=e->io.now(e->io.ctx);
        }
        if (e->io.delay(e->io.ctx,5)) return fault(e,"delay_failed",-EIO);
    }
    return fault(e,idle ? "idle_timeout" : "fifo_timeout",-ETIMEDOUT);
}
static void wr(struct tdfx_engine *e, uint32_t reg, uint32_t value)
{
    e->touched=true; e->cpu_safe=false;
    e->io.write(e->io.ctx,reg,value);
}
/* Disable is read back; quarantine itself never grants CPU access. */
static int hide(struct tdfx_engine *e)
{
    uint32_t cfg=e->io.read(e->io.ctx,VIDEO_PROC_CONFIG);
    if (cfg==UINT32_MAX) return fault(e,"cursor_unavailable",-EIO);
    if (cfg&CURSOR_ENABLE) {
        int err=wait_status(e,1,false); if (err) return err;
        wr(e,VIDEO_PROC_CONFIG,cfg&~CURSOR_ENABLE);
    }
    cfg=e->io.read(e->io.ctx,VIDEO_PROC_CONFIG);
    if (cfg==UINT32_MAX || (cfg&CURSOR_ENABLE)) return fault(e,"cursor_disable_failed",-EIO);
    e->hidden=true;
    return 0;
}
static int sync_engine(void *ctx)
{
    struct tdfx_engine *e=ctx;
    if (!e->io.read) return e->cpu_safe ? 0 : -EIO;
    e->idle_waits++;
    e->io.drain(e->io.ctx);
    /* A NOP flushes the command stream, then require three idle samples,
     * as in the donor. It is a recovery fence, never a drawing replay. */
    int err=wait_status(e,1,false);
    if (!err) { wr(e,CMD_3D,0); err=wait_status(e,0,true); }
    if (!err && e->quarantined) err=hide(e);
    if (!err) { e->io.drain(e->io.ctx); e->cpu_safe=true; }
    return err;
}
static bool rect(const struct tdfx_engine *e,const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>0 && r->height>0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=e->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=e->fb.height;
}
static int fill(void *ctx,const struct fb_rect *r,uint32_t colour)
{
    struct tdfx_engine *e=ctx;
    if (!e->active || e->quarantined) return -EIO;
    if (!rect(e,r)) return -EOPNOTSUPP;
    int err=wait_status(e,6,false); if (err) return err;
    e->io.drain(e->io.ctx);
    wr(e,DST_FORMAT,e->format);
    wr(e,COLOR_BACK,colour); wr(e,COLOR_FORE,colour);
    wr(e,DST_SIZE,xy(r->width,r->height)); wr(e,DST_XY,xy(r->x,r->y));
    wr(e,CMD_2D,RECTANGLE_FILL|CMD_2D_GO|(0xccu<<24));
    err=sync_engine(e); if (!err) e->operations++;
    return err;
}
static int copy(void *ctx,const struct fb_rect *r,int32_t x,int32_t y)
{
    struct tdfx_engine *e=ctx;
    if (!e->active || e->quarantined) return -EIO;
    struct fb_rect d={x,y,r ? r->width : 0,r ? r->height : 0};
    if (!rect(e,r) || !rect(e,&d)) return -EOPNOTSUPP;
    uint32_t sx=r->x,sy=r->y,dx=x,dy=y,cmd=1u|CMD_2D_GO|(0xccu<<24);
    /* Haiku dimensions are inclusive. Ciuki dimensions are counts, so
     * adjust endpoints by count-1 independently on BOTH axes. */
    if (sx<=dx) { cmd|=X_RIGHT_TO_LEFT; sx+=r->width-1; dx+=r->width-1; }
    if (sy<=dy) { cmd|=Y_BOTTOM_TO_TOP; sy+=r->height-1; dy+=r->height-1; }
    int err=wait_status(e,6,false); if (err) return err;
    e->io.drain(e->io.ctx);
    wr(e,DST_FORMAT,e->format); wr(e,SRC_FORMAT,e->format);
    wr(e,SRC_XY,xy(sx,sy)); wr(e,DST_SIZE,xy(r->width,r->height));
    wr(e,DST_XY,xy(dx,dy)); wr(e,CMD_2D,cmd);
    err=sync_engine(e); if (!err) e->operations++;
    /* No replay: an overlapping command may have executed partially. */
    return err;
}
static uint32_t command_bits(void *ctx)
{ return caps(ctx)&1u ? 1u<<CIUKI_CMD_SCALED : 0; }
static uint32_t command_features(void *ctx)
{ return caps(ctx)&1u ? CIUKI_CAP_STRETCH : 0; }
static bool scanout(const struct tdfx_engine *e,const struct swdib_target *t)
{
    const struct fb_device *f=t ? &t->device : 0;
    return f && t->kind==CIUKI_TARGET_SCANOUT && !t->offset &&
        f->phys==e->fb.phys && f->generation==e->fb.generation &&
        f->width==e->fb.width && f->height==e->fb.height &&
        f->pitch==e->fb.pitch && f->bpp==e->fb.bpp &&
        f->red_size==e->fb.red_size && f->red_pos==e->fb.red_pos &&
        f->green_size==e->fb.green_size && f->green_pos==e->fb.green_pos &&
        f->blue_size==e->fb.blue_size && f->blue_pos==e->fb.blue_pos;
}
static int stretch(void *ctx,const struct swdib_command *c)
{
    struct tdfx_engine *e=ctx;
    if (!e->active || e->quarantined) return -EIO;
    if (!c || !c->record) return -EOPNOTSUPP;
    const struct ciuki_batch_record *r=c->record;
    struct fb_rect src={r->sx,r->sy,(int32_t)r->sw,(int32_t)r->sh};
    struct fb_rect dst={r->dx,r->dy,(int32_t)r->dw,(int32_t)r->dh};
    /* No guessed Bresenham phase: only exact constant-row replication.
     * Validate the entire record, including overlap/clip list, before I/O. */
    if (r->opcode!=CIUKI_CMD_SCALED || r->filter!=CIUKI_FILTER_NEAREST_EDGE ||
        !scanout(e,c->src) || !scanout(e,c->dst) || r->sw!=r->dw || r->sh!=1 ||
        !rect(e,&src) || !rect(e,&dst) || swdib_validate(c)) return -EOPNOTSUPP;
    /* Voodoo3 guide 7.2.16/21/23.2, X.Org tdfx_video.c stretch sequence.
     * Keep the original source/destination for each clip: clipping must not
     * restart the sampling phase. Clip1 leaves the legacy clip0 intact. */
    for (unsigned n=0;n<(r->clip_count ? r->clip_count : 1);n++) {
        int64_t l=dst.x,t=dst.y,right=(int64_t)dst.x+dst.width,bottom=(int64_t)dst.y+dst.height;
        if (r->clip_count) {
            const struct ciuki_batch_clip *p=&c->clips[n];
            if (p->x>l) l=p->x;
            if (p->y>t) t=p->y;
            if ((int64_t)p->x+p->width<right) right=(int64_t)p->x+p->width;
            if ((int64_t)p->y+p->height<bottom) bottom=(int64_t)p->y+p->height;
        }
        if (l>=right || t>=bottom) continue;
        int err=wait_status(e,r->clip_count ? 8 : 6,false); if (err) return err;
        e->io.drain(e->io.ctx);
        if (r->clip_count) {
            wr(e,CLIP1_MIN,xy((uint32_t)l,(uint32_t)t)); wr(e,CLIP1_MAX,xy((uint32_t)right,(uint32_t)bottom));
        }
        wr(e,SRC_FORMAT,e->format); wr(e,DST_FORMAT,e->format);
        wr(e,SRC_SIZE,xy(r->sw,r->sh)); wr(e,SRC_XY,xy(r->sx,r->sy));
        wr(e,DST_SIZE,xy(r->dw,r->dh)); wr(e,DST_XY,xy(r->dx,r->dy));
        /* Reserve separately: the documented PCI FIFO has at most 31 slots. */
        err=wait_status(e,1,false); if (err) return err;
        wr(e,CMD_2D,SCRN_TO_SCRN_STRETCH|CMD_2D_GO|(r->clip_count ? STRETCH_CLIP1 : 0)|(0xccu<<24));
        err=sync_engine(e); if (err) return err;
        e->operations++;
    }
    return 0;
}
static uint32_t rgb(const struct tdfx_engine *e,uint32_t p)
{
    if (e->fb.bpp==32) return p&0xffffffu;
    unsigned r=(p>>11)&31u,g=(p>>5)&63u,b=p&31u;
    return ((r*255u+15u)/31u<<16)|((g*255u+31u)/63u<<8)|((b*255u+15u)/31u);
}
static int cursor_set(void *ctx,const struct accel_cursor *c)
{
    struct tdfx_engine *e=ctx;
    if (!e->active || e->quarantined) return -EIO;
    if (!e->cursor || !c) return -EOPNOTSUPP;
    int err=sync_engine(e); if (!err) err=hide(e); if (err) return err;
    e->wanted=false; e->shape=false;
    /* 64 rows: eight AND bytes then eight XOR bytes, MSB left. Unused
     * pixels stay transparent. Legacy mask means OPAQUE, not AND. */
    for (unsigned row=0;row<64;row++) {
        for (unsigned col=0;col<8;col++) {
            unsigned shift=col<4 ? 24-col*8 : 0;
            e->cursor[row*16+col]=row<32 && col<4 ? (uint8_t)~(c->mask[row]>>shift) : 0xff;
            e->cursor[row*16+8+col]=row<32 && col<4 ? (uint8_t)((c->image[row]&c->mask[row])>>shift) : 0;
        }
    }
    e->io.drain(e->io.ctx);
    err=wait_status(e,3,false); if (err) return err;
    wr(e,HW_CURSOR_PAT_ADDR,e->cursor_offset);
    wr(e,HW_CURSOR_COLOR0,rgb(e,c->background));
    wr(e,HW_CURSOR_COLOR1,rgb(e,c->foreground));
    if (e->io.read(e->io.ctx,HW_CURSOR_PAT_ADDR)!=e->cursor_offset ||
        e->io.read(e->io.ctx,HW_CURSOR_COLOR0)!=rgb(e,c->background) ||
        e->io.read(e->io.ctx,HW_CURSOR_COLOR1)!=rgb(e,c->foreground))
        return fault(e,"cursor_readback_failed",-EIO);
    err=sync_engine(e);
    if (!err) e->shape=true;
    return err;
}
static bool on_screen(const struct tdfx_engine *e)
{
    return e->cursor_x>-32 && e->cursor_y>-32 &&
        e->cursor_x<(int32_t)e->fb.width && e->cursor_y<(int32_t)e->fb.height;
}
static int enable_cursor(struct tdfx_engine *e)
{
    if (!e->wanted || !on_screen(e)) {
        int err=hide(e); return err ? err : sync_engine(e);
    }
    uint32_t cfg=e->io.read(e->io.ctx,VIDEO_PROC_CONFIG);
    if (cfg==UINT32_MAX) return fault(e,"cursor_unavailable",-EIO);
    int err=wait_status(e,1,false); if (err) return err;
    wr(e,VIDEO_PROC_CONFIG,cfg|CURSOR_ENABLE);
    uint32_t check=e->io.read(e->io.ctx,VIDEO_PROC_CONFIG);
    if (check==UINT32_MAX || !(check&CURSOR_ENABLE))
        return fault(e,"cursor_enable_failed",-EIO);
    e->hidden=false;
    return sync_engine(e);
}
static int cursor_move(void *ctx,int32_t x,int32_t y)
{
    struct tdfx_engine *e=ctx;
    if (!e->active || e->quarantined) return -EIO;
    if (!e->cursor) return -EOPNOTSUPP;
    if (!e->shape) return -EINVAL;
    e->cursor_x=x; e->cursor_y=y;
    /* Wholly outside the screen: hide instead of overflowing signed
     * hardware fields. Remember SHOW intent for subsequent re-entry. */
    if (!on_screen(e)) { int err=hide(e); return err ? err : sync_engine(e); }
    int err=wait_status(e,1,false); if (err) return err;
    uint32_t pos=xy((uint32_t)(x+63),(uint32_t)(y+63));
    wr(e,HW_CURSOR_LOC,pos);
    if (e->io.read(e->io.ctx,HW_CURSOR_LOC)!=pos) return fault(e,"cursor_position_failed",-EIO);
    e->positioned=true;
    err=sync_engine(e);
    return err ? err : enable_cursor(e);
}
static int cursor_show(void *ctx,bool show)
{
    struct tdfx_engine *e=ctx;
    if (!e->active || e->quarantined) return -EIO;
    if (!e->cursor) return -EOPNOTSUPP;
    if (show && (!e->shape || !e->positioned)) return -EINVAL;
    e->wanted=show;
    int err=enable_cursor(e);
    return err ? err : sync_engine(e);
}
static int stop(void *ctx)
{
    struct tdfx_engine *e=ctx;
    e->active=false; e->wanted=false;
    if (!e->io.read) return e->cpu_safe ? 0 : -EIO;
    int err=sync_engine(e); if (!err) err=hide(e);
    if (!err) err=sync_engine(e);
    return err;
}
void tdfx_engine_quarantine(struct tdfx_engine *e,const char *reason)
{
    if (!e->io.read) {
        /* Successful stop already revoked the hardware lease. Preserve
         * that idle proof when generic marker policy then quarantines. */
        e->quarantined=true; e->active=false;
        e->reason=reason ? reason : "quarantined";
        return;
    }
    (void)fault(e,reason ? reason : "quarantined",-EIO);
    e->wanted=false;
    /* A read-back disable may succeed, but cannot prove engine idle. */
    if (e->io.read) (void)hide(e);
    e->cpu_safe=false;
}
static void set_pause(void *ctx,int (*fn)(void *),void *arg)
{ struct tdfx_engine *e=ctx; e->pause=fn; e->pause_arg=arg; }

int tdfx_engine_start(struct tdfx_engine *e,const struct tdfx_io *io,
                       const struct fb_device *f,uint8_t *cursor,uint32_t offset)
{
    if (!e || !io || !io->read || !io->write || !io->now || !io->delay || !io->drain ||
        !tdfx_layout(f)) return -EOPNOTSUPP;
    if (e->quarantined || (e->touched && !e->cpu_safe)) return -EIO;
    memset(e,0,sizeof(*e)); e->io=*io; e->fb=*f; e->cpu_safe=true; e->hidden=true;
    e->reason="pending";
    e->format=f->pitch|(f->bpp==16 ? 0x30000u : 0x50000u);
    e->ops=(struct accel_ops){.ctx=e,.sync=sync_engine,.copy_rect=copy,.fill_rect=fill,
        .cursor_set=cursor_set,.cursor_move=cursor_move,.cursor_show=cursor_show,
        .stop=stop,.capabilities=caps,.cpu_ready=cpu_ready,.set_pause=set_pause,
        .command_bits=command_bits,.command_features=command_features,.stretch_rect=stretch};
    uint32_t cfg=io->read(io->ctx,VIDEO_PROC_CONFIG);
    uint32_t misc=io->read(io->ctx,MISC_INIT1);
    uint32_t expected=1u|128u|(1u<<10)|((f->bpp/8u-1u)<<18);
    /* Native mode providers are excluded. Refuse non-linear, panned,
     * interlaced, half-height, overlay or unknown firmware layouts. */
    if (misc==UINT32_MAX || (io->read(io->ctx,PCI_INIT0)&PCI_STALL_ENABLE) ||
        (cfg&~((1u<<26)|CURSOR_ENABLE))!=expected ||
        io->read(io->ctx,VIDEO_DESKTOP_START_ADDR)!=0 ||
        (io->read(io->ctx,VIDEO_DESKTOP_OVERLAY_STRIDE)&0xffffu)!=f->pitch ||
        io->read(io->ctx,VIDEO_SCREEN_SIZE)!=(f->width|(f->height<<12)) ||
        (io->read(io->ctx,MISC_INIT0)&0xc0000000u)) {
        e->reason="firmware_layout_unsupported"; return -EOPNOTSUPP;
    }
    if (cursor && (offset%1024u || offset<f->size || offset>32u*1024u*1024u-TDFX_CURSOR_BYTES))
        return -EOPNOTSUPP;
    /* On entry the firmware engine must first be idle; no reset may
     * disguise unrelated work. No command before presenter exclusion. */
    int err=wait_status(e,0,true); if (err) return err;
    err=hide(e); if (err) return err;
    err=wait_status(e,9,false); if (err) return err;
    wr(e,CLIP0_MIN,0); wr(e,CLIP0_MAX,xy(f->width,f->height));
    wr(e,SRC_BASE_ADDR,0); wr(e,DST_BASE_ADDR,0);
    wr(e,COMMAND_EXTRA,0);
    /* Guide 7.2.8/8.2.6: disable firmware's forced line/polygon errors.
     * This private engine exposes no operation that can re-enable them. */
    wr(e,BRES_ERROR0,0); wr(e,BRES_ERROR1,0);
    /* The donor disables 2D block writes for SDRAM. Use the conservative
     * setting for both memory types, without altering CLUT/palette state. */
    wr(e,MISC_INIT1,misc|(1u<<15));
    wr(e,DST_FORMAT,e->format);
    err=sync_engine(e); if (err) return err;
    e->cursor=cursor; e->cursor_offset=offset;
    e->active=true; e->reason="none";
    return 0;
}
int tdfx_cursor_check(const struct accel_ops *ops,uint64_t deadline,bool (*cancel)(void *),void *ctx)
{
    if (!ops || !ops->cursor_set || !ops->cursor_move || !ops->cursor_show) return -EOPNOTSUPP;
    struct accel_cursor c={.foreground=0xffffff,.background=0};
    int err=0;
    for (unsigned step=0;step<4 && !err;step++) {
        if (g_ticks>=deadline || (cancel && cancel(ctx))) { err=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!step) err=ops->cursor_show(ops->ctx,false);
        else if (step==1) err=ops->cursor_set(ops->ctx,&c);
        else if (step==2) err=ops->cursor_move(ops->ctx,-1,-1);
        else err=ops->cursor_show(ops->ctx,true);
    }
    int hidden=ops->cursor_show(ops->ctx,false);
    return err ? err : hidden;
}
