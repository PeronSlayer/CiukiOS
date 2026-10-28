/* Native implementation of the MIT SVGA protocol, not an X/Oracle driver port.
 * Sources and exact pins: docs/design/vmware-svga.md.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "engine.h"
#define WAIT_US 20000u
#define POLL_US 10u
#define POLLS (WAIT_US/POLL_US+1u)
#define R(s,r) ((s)->io.read((s)->io.ctx,(r)))
#define W(s,r,v) ((s)->io.write((s)->io.ctx,(r),(v)))
#define FR(s,r) ((s)->io.fifo_read((s)->io.ctx,4u*(r)))
#define FW(s,r,v) ((s)->io.fifo_write((s)->io.ctx,4u*(r),(v)))
static bool valid(struct vmsvga_engine *s)
{ return !s->io.valid || s->io.valid(s->io.ctx); }
static void drain(struct vmsvga_engine *s) { s->io.drain(s->io.ctx); }
static uint32_t swap_bytes(uint32_t v) { return __builtin_bswap32(v); }
static bool ring(struct vmsvga_engine *s, uint32_t *stop)
{
    uint32_t min=FR(s,SVGA_FIFO_MIN), max=FR(s,SVGA_FIFO_MAX);
    uint32_t next=FR(s,SVGA_FIFO_NEXT_CMD), tail=FR(s,SVGA_FIFO_STOP);
    if (min!=s->min || max!=s->fifo_size || next!=s->next ||
        (tail&3u) || tail<min || tail>=max) return false;
    *stop=tail; return true;
}
static bool hide(struct vmsvga_engine *s)
{
    if (!s->cursor) return true;
    if (!valid(s)) return false;
    if (s->bypass3) {
        FW(s,SVGA_FIFO_CURSOR_ON,0); drain(s);
        FW(s,SVGA_FIFO_CURSOR_COUNT,FR(s,SVGA_FIFO_CURSOR_COUNT)+1); drain(s);
        if (FR(s,SVGA_FIFO_CURSOR_ON)) return false;
    } else W(s,SVGA_REG_CURSOR_ON,SVGA_CURSOR_ON_HIDE);
    if (!s->bypass3 && R(s,SVGA_REG_CURSOR_ON)) return false;
    s->cursor_visible=false;
    return true;
}
void vmsvga_engine_quarantine(struct vmsvga_engine *s,const char *why)
{
    if (!hide(s)) s->cpu_safe=false;
    s->quarantined=true; s->active=false; s->reason=why;
    /* cpu_safe is unchanged. Quarantine is not a completion proof. */
}
static int fault(struct vmsvga_engine *s,const char *why)
{ vmsvga_engine_quarantine(s,why); return -EIO; }
/* Empty validated ring + BUSY=0 is the only drain proof. Disable FIFO before
 * publishing CPU safety. Reasserting the same ENABLE value restarts QEMU's
 * dirty log as well as firmware-compatible traced framebuffer updates. */
static int finish(struct vmsvga_engine *s)
{
    if (s->quarantined && !hide(s)) return fault(s,"cursor_disable_failed");
    W(s,SVGA_REG_CONFIG_DONE,0);
    W(s,SVGA_REG_ENABLE,SVGA_REG_ENABLE_ENABLE);
    if (R(s,SVGA_REG_CONFIG_DONE) || R(s,SVGA_REG_ENABLE)!=SVGA_REG_ENABLE_ENABLE)
        return fault(s,"fifo_disable_failed");
    drain(s); s->pending=false; s->cpu_safe=true; return 0;
}
static int wait_idle(struct vmsvga_engine *s)
{
    uint64_t start=s->io.now(s->io.ctx);
    for (unsigned i=0;i<POLLS;i++) {
        if (!valid(s)) return fault(s,"resource_revoked");
        uint32_t tail;
        if (!ring(s,&tail)) return fault(s,"fifo_corrupt");
        s->status=R(s,SVGA_REG_BUSY);
        if (!s->status && tail==s->next) return finish(s);
        if (s->io.now(s->io.ctx)-start>=WAIT_US) break;
        if (i && !(i%(1000u/POLL_US)) && s->pause) {
            int e=s->pause(s->pause_ctx);
            if (e) { vmsvga_engine_quarantine(s,"wait_cancelled"); return e; }
        }
        if (s->io.delay(s->io.ctx,POLL_US)) break;
    }
    s->timeouts++; vmsvga_engine_quarantine(s,"fifo_timeout"); return -ETIMEDOUT;
}
static int sync_engine(void *ctx)
{
    struct vmsvga_engine *s=ctx;
    drain(s);
    if (!valid(s)) { s->cpu_safe=false; return fault(s,"resource_revoked"); }
    if (s->cpu_safe) return 0;
    if (!s->initialized) return -EIO;
    /* A failed cursor-disable proof can reject CPU use with an already
     * disabled, empty FIFO. Do not wake a disabled FIFO to retry that proof. */
    if (!s->pending) return wait_idle(s);
    W(s,SVGA_REG_SYNC,1);
    return wait_idle(s);
}
static int submit(struct vmsvga_engine *s,unsigned words)
{
    if (!s->active || s->quarantined) return -EIO;
    if (!valid(s)) { s->cpu_safe=false; return fault(s,"resource_revoked"); }
    uint32_t tail;
    if (!ring(s,&tail)) { s->cpu_safe=false; return fault(s,"fifo_corrupt"); }
    /* Each call is synchronous. Do not append after a foreign producer or
     * overwrite an uncompleted packet. No partial packet is ever published. */
    if (!s->cpu_safe || tail!=s->next) {
        s->cpu_safe=false; return fault(s,"fifo_not_idle");
    }
    if (words*4u>=s->fifo_size-s->min) return -EOPNOTSUPP;
    drain(s); /* CPU framebuffer writes precede engine access. */
    s->cpu_safe=false; s->pending=true;
    W(s,SVGA_REG_CONFIG_DONE,1);
    if (R(s,SVGA_REG_CONFIG_DONE)!=1) return fault(s,"fifo_enable_failed");
    uint32_t at=s->next;
    for (unsigned i=0;i<words;i++) {
        s->io.fifo_write(s->io.ctx,at,s->packet[i]);
        at+=4;
        if (at==s->fifo_size) at=s->min;
    }
    drain(s);
    s->next=at;
    FW(s,SVGA_FIFO_NEXT_CMD,at);
    drain(s); s->packets++;
    W(s,SVGA_REG_SYNC,1);
    return wait_idle(s);
}
/* SVGA3D batches (f7-31). SVGA_CMD_FENCE semantics: Mesa 21.3.9
 * svga_reg.h:1508-1525 ("copy the 'fence' value into SVGA_FIFO_FENCE");
 * availability needs SVGA_FIFO_CAP_FENCE and a FIFO_MIN beyond the register. */
static int wait_fence(struct vmsvga_engine *s,uint32_t fence,unsigned wait_us)
{
    uint64_t start=s->io.now(s->io.ctx);
    unsigned polls=wait_us/POLL_US+1u;
    for (unsigned i=0;i<polls;i++) {
        if (!valid(s)) return fault(s,"resource_revoked");
        uint32_t tail;
        if (!ring(s,&tail)) return fault(s,"fifo_corrupt");
        s->status=R(s,SVGA_REG_BUSY);
        if (!s->status && tail==s->next) {
            /* Drained ring: the batch's own fence must be the last one seen. */
            if (FR(s,SVGA_FIFO_FENCE)!=fence) return fault(s,"fence_mismatch");
            return finish(s);
        }
        if (s->io.now(s->io.ctx)-start>=wait_us) break;
        if (i && !(i%(1000u/POLL_US)) && s->pause) {
            int e=s->pause(s->pause_ctx);
            if (e) { vmsvga_engine_quarantine(s,"wait_cancelled"); return e; }
        }
        if (s->io.delay(s->io.ctx,POLL_US)) break;
    }
    s->timeouts++; vmsvga_engine_quarantine(s,"fence_timeout"); return -ETIMEDOUT;
}
int vmsvga_engine_submit3d(struct vmsvga_engine *s,const uint32_t *words,unsigned count,
                           uint32_t fence,unsigned wait_us)
{
    if (!s || !words || !count || !fence || !wait_us || wait_us>VMSVGA_WAIT3D_LIMIT_US) return -EINVAL;
    if (!s->active || s->quarantined) return -EIO;
    if (!(s->fifo_caps&SVGA_FIFO_CAP_FENCE) || s->min<=4u*SVGA_FIFO_FENCE) return -EOPNOTSUPP;
    if (count>(s->fifo_size-s->min)/4u || (count+2u)*4u>=s->fifo_size-s->min) return -EOPNOTSUPP;
    if (!valid(s)) { s->cpu_safe=false; return fault(s,"resource_revoked"); }
    uint32_t tail;
    if (!ring(s,&tail)) { s->cpu_safe=false; return fault(s,"fifo_corrupt"); }
    if (!s->cpu_safe || tail!=s->next) { s->cpu_safe=false; return fault(s,"fifo_not_idle"); }
    drain(s); /* CPU staging writes precede device DMA reads. */
    s->cpu_safe=false; s->pending=true;
    W(s,SVGA_REG_CONFIG_DONE,1);
    if (R(s,SVGA_REG_CONFIG_DONE)!=1) return fault(s,"fifo_enable_failed");
    uint32_t at=s->next;
    for (unsigned i=0;i<count+2u;i++) {
        uint32_t value=i<count ? words[i] : i==count ? SVGA_CMD_FENCE : fence;
        s->io.fifo_write(s->io.ctx,at,value);
        at+=4;
        if (at==s->fifo_size) at=s->min;
    }
    drain(s);
    s->next=at;
    FW(s,SVGA_FIFO_NEXT_CMD,at);
    drain(s); s->packets++;
    W(s,SVGA_REG_SYNC,1);
    return wait_fence(s,fence,wait_us);
}
static bool rect(struct vmsvga_engine *s,const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>=0 && r->height>=0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height;
}
static int fill(void *ctx,const struct fb_rect *r,uint32_t colour)
{
    struct vmsvga_engine *s=ctx;
    if (!rect(s,r)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return s->quarantined ? -EIO : 0;
    uint32_t *p=s->packet;
    p[0]=VMSVGA_CMD_FILL; p[1]=colour; p[2]=r->x; p[3]=r->y;
    p[4]=r->width; p[5]=r->height;
    int e=submit(s,6); if (!e) s->fills++; return e;
}
static int copy(void *ctx,const struct fb_rect *r,int32_t x,int32_t y)
{
    struct vmsvga_engine *s=ctx;
    if (!r) return -EOPNOTSUPP;
    struct fb_rect dst={x,y,r->width,r->height};
    if (!rect(s,r) || !rect(s,&dst)) return -EOPNOTSUPP;
    if (!r->width || !r->height) return s->quarantined ? -EIO : 0;
    /* SVGA RECT_COPY itself guarantees overlap-safe transfer, unlike a
     * silicon blitter that requires separate direction flags. */
    uint32_t *p=s->packet;
    p[0]=SVGA_CMD_RECT_COPY; p[1]=r->x; p[2]=r->y; p[3]=x; p[4]=y;
    p[5]=r->width; p[6]=r->height;
    int e=submit(s,7); if (!e) s->copies++; return e;
}
static int cursor_set(void *ctx,const struct accel_cursor *shape)
{
    struct vmsvga_engine *s=ctx;
    if (!s->cursor || !shape) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -EIO;
    if (!valid(s)) { s->cpu_safe=false; return fault(s,"resource_revoked"); }
    if (!hide(s)) { s->cpu_safe=false; return fault(s,"cursor_disable_failed"); }
    s->cursor_loaded=false;
    uint32_t *p=s->packet;
    p[0]=SVGA_CMD_DEFINE_CURSOR; p[1]=p[2]=p[3]=0;
    p[4]=p[5]=32; p[6]=1; p[7]=32;
    for (unsigned y=0;y<32;y++) {
        p[8+y]=swap_bytes(~shape->mask[y]);
        for (unsigned x=0;x<32;x++)
            p[40+y*32+x]=(shape->mask[y]&(0x80000000u>>x)) ?
                (shape->image[y]&(0x80000000u>>x) ? shape->foreground : shape->background)&0xffffffu : 0;
    }
    int e=submit(s,8+32+32*32);
    if (!e) s->cursor_loaded=true;
    return e;
}
static int cursor_move(void *ctx,int32_t x,int32_t y)
{
    struct vmsvga_engine *s=ctx;
    if (!s->cursor) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -EIO;
    if (!valid(s)) { s->cpu_safe=false; return fault(s,"resource_revoked"); }
    if (s->bypass3) {
        FW(s,SVGA_FIFO_CURSOR_X,(uint32_t)x); FW(s,SVGA_FIFO_CURSOR_Y,(uint32_t)y);
        drain(s); FW(s,SVGA_FIFO_CURSOR_COUNT,FR(s,SVGA_FIFO_CURSOR_COUNT)+1); drain(s);
        if (FR(s,SVGA_FIFO_CURSOR_X)!=(uint32_t)x || FR(s,SVGA_FIFO_CURSOR_Y)!=(uint32_t)y)
            return fault(s,"cursor_position_failed");
    } else {
        W(s,SVGA_REG_CURSOR_X,(uint32_t)x); W(s,SVGA_REG_CURSOR_Y,(uint32_t)y);
        if (R(s,SVGA_REG_CURSOR_X)!=(uint32_t)x || R(s,SVGA_REG_CURSOR_Y)!=(uint32_t)y)
            return fault(s,"cursor_position_failed");
    }
    s->cursor_x=x; s->cursor_y=y; return 0;
}
static int cursor_show(void *ctx,bool show)
{
    struct vmsvga_engine *s=ctx;
    if (!s->cursor) return -EOPNOTSUPP;
    if (show && !s->cursor_loaded) return -EOPNOTSUPP;
    if (!s->active || s->quarantined) return -EIO;
    if (!valid(s)) { s->cpu_safe=false; return fault(s,"resource_revoked"); }
    if (s->bypass3) {
        FW(s,SVGA_FIFO_CURSOR_ON,show); drain(s);
        FW(s,SVGA_FIFO_CURSOR_COUNT,FR(s,SVGA_FIFO_CURSOR_COUNT)+1); drain(s);
        if (FR(s,SVGA_FIFO_CURSOR_ON)!=(unsigned)show) return fault(s,"cursor_visibility_failed");
    } else {
        W(s,SVGA_REG_CURSOR_ON,show);
        if (R(s,SVGA_REG_CURSOR_ON)!=(unsigned)show) return fault(s,"cursor_visibility_failed");
    }
    s->cursor_visible=show; return 0;
}
static unsigned capabilities(void *ctx)
{ struct vmsvga_engine *s=ctx; return s->active && !s->quarantined ? (s->cursor ? 3u : 1u) : 0; }
static bool cpu_ready(void *ctx) { return ((struct vmsvga_engine *)ctx)->cpu_safe; }
static void set_pause(void *ctx,int (*pause)(void *),void *arg)
{ struct vmsvga_engine *s=ctx; s->pause=pause; s->pause_ctx=arg; }
static int stop_engine(void *ctx)
{
    struct vmsvga_engine *s=ctx;
    if (!hide(s)) { s->cpu_safe=false; return fault(s,"cursor_disable_failed"); }
    int e=sync_engine(s);
    if (e) return e;
    s->active=false; s->cursor_loaded=false; return 0;
}
static uint32_t mask(unsigned n,unsigned at)
{ return n && n<=8 && at<32 && n+at<=32 ? ((1u<<n)-1u)<<at : 0; }
int vmsvga_engine_init(struct vmsvga_engine *s,const struct vmsvga_io *io,
                       const struct fb_device *fb,uint32_t phys,uint32_t bytes)
{
    if (!s || !io || !fb || !io->read || !io->write || !io->fifo_read ||
        !io->fifo_write || !io->drain || !io->now || !io->delay) return -EINVAL;
    memset(s,0,sizeof(*s)); s->io=*io; s->fb=*fb; s->cpu_safe=true;
    s->reason="unsupported_interface";
    s->ops=(struct accel_ops){.ctx=s,.sync=sync_engine,.fill_rect=fill,.copy_rect=copy,
        .cursor_set=cursor_set,.cursor_move=cursor_move,.cursor_show=cursor_show,
        .stop=stop_engine,.capabilities=capabilities,.cpu_ready=cpu_ready,.set_pause=set_pause};
    if (!valid(s) || !fb->present || !fb->mapped || !fb->width || !fb->height ||
        fb->width>INT32_MAX || fb->height>INT32_MAX ||
        (fb->bpp!=16 && fb->bpp!=24 && fb->bpp!=32) ||
        (uint64_t)fb->width*(fb->bpp/8)>fb->pitch ||
        (uint64_t)fb->pitch*fb->height>fb->size ||
        bytes>VMSVGA_FIFO_LIMIT || bytes<16384 || (bytes&4095u) ||
        R(s,SVGA_REG_ID)!=SVGA_ID_2 || R(s,SVGA_REG_ENABLE)!=SVGA_REG_ENABLE_ENABLE ||
        R(s,SVGA_REG_WIDTH)!=fb->width || R(s,SVGA_REG_HEIGHT)!=fb->height ||
        R(s,SVGA_REG_BITS_PER_PIXEL)!=fb->bpp || R(s,SVGA_REG_BYTES_PER_LINE)!=fb->pitch ||
        R(s,SVGA_REG_FB_START)!=fb->phys || R(s,SVGA_REG_FB_OFFSET) ||
        R(s,SVGA_REG_PSEUDOCOLOR) || R(s,SVGA_REG_FB_SIZE)<fb->size ||
        R(s,SVGA_REG_MEM_START)!=phys || R(s,SVGA_REG_MEM_SIZE)!=bytes) return -EOPNOTSUPP;
    uint32_t red=mask(fb->red_size,fb->red_pos),green=mask(fb->green_size,fb->green_pos);
    uint32_t blue=mask(fb->blue_size,fb->blue_pos);
    if (!red || !green || !blue || (red&green) || (red&blue) || (green&blue) ||
        ((uint64_t)(red|green|blue)>>fb->bpp) ||
        R(s,SVGA_REG_RED_MASK)!=red || R(s,SVGA_REG_GREEN_MASK)!=green ||
        R(s,SVGA_REG_BLUE_MASK)!=blue) return -EOPNOTSUPP;
    s->caps=R(s,SVGA_REG_CAPABILITIES);
    s->vram_size=R(s,SVGA_REG_VRAM_SIZE); /* read-only plan input for SVGA3D staging */
    if ((s->caps&(VMSVGA_CAP_FILL|SVGA_CAP_RECT_COPY))!=(VMSVGA_CAP_FILL|SVGA_CAP_RECT_COPY)) {
        s->reason="native_fill_unavailable"; return -EOPNOTSUPP;
    }
    if (R(s,SVGA_REG_CONFIG_DONE) || R(s,SVGA_REG_BUSY)) {
        s->reason="foreign_fifo"; return -EOPNOTSUPP;
    }
    s->fifo_size=bytes; s->min=16;
    if (s->caps&SVGA_CAP_EXTENDED_FIFO) {
        uint32_t regs=R(s,SVGA_REG_MEM_REGS);
        if (regs<SVGA_FIFO_EXTENDED_MANDATORY_REGS || regs>(bytes-10240u)/4u)
            return -EOPNOTSUPP;
        s->min=regs*4u;
        s->fifo_caps=FR(s,SVGA_FIFO_CAPABILITIES);
        s->bypass3=!!(s->fifo_caps&SVGA_FIFO_CAP_CURSOR_BYPASS_3);
    }
    s->next=s->min;
    FW(s,SVGA_FIFO_MIN,s->min); FW(s,SVGA_FIFO_MAX,bytes);
    if (s->fifo_caps&SVGA_FIFO_CAP_RESERVE) FW(s,SVGA_FIFO_RESERVED,0);
    FW(s,SVGA_FIFO_NEXT_CMD,s->min); FW(s,SVGA_FIFO_STOP,s->min); drain(s);
    s->initialized=s->active=true;
    s->cursor=fb->bpp==32 && red==0xff0000 && green==0xff00 && blue==0xff &&
        (s->caps&SVGA_CAP_CURSOR) && (s->bypass3 || (s->caps&SVGA_CAP_CURSOR_BYPASS_2));
    if (!hide(s)) { s->cpu_safe=false; return fault(s,"cursor_disable_failed"); }
    s->reason="none"; return 0;
}
int vmsvga_engine_cursor_test(struct vmsvga_engine *s,uint64_t deadline,
                             bool (*cancel)(void *),void *arg)
{
    struct accel_cursor empty={0};
    int e=0;
    for (unsigned i=0;i<4 && !e;i++) {
        if (g_ticks>=deadline || (cancel && cancel(arg))) { e=g_ticks>=deadline ? -ETIMEDOUT : -EINTR; break; }
        if (!i) e=cursor_show(s,false);
        else if (i==1) e=cursor_set(s,&empty);
        else if (i==2) e=cursor_move(s,-1,1);
        else e=cursor_show(s,true);
    }
    int hidden=cursor_show(s,false);
    return e ? e : hidden;
}
