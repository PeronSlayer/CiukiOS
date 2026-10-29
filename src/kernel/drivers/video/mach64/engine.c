/*
	Haiku ATI video driver adapted from the X.org ATI driver.

	Copyright 1992,1993,1994,1995,1996,1997 by Kevin E. Martin, Chapel Hill, North Carolina.
	Copyright 1997 through 2004 by Marc Aurele La France (TSI @ UQV), tsi@xfree86.org

	Copyright 2009 Haiku, Inc.  All rights reserved.
	Distributed under the terms of the MIT license.

	Authors:
	Gerald Zajac 2009
*/
/* Origin: Haiku r1beta5 src/add-ons/accelerants/ati/mach64_draw.cpp,
 * mach64_init.cpp, mach64_cursor.cpp. Native bounded CiukiOS adaptation.
 * Full permission notice: docs/licenses/haiku-mach64-NOTICES.txt. */
#include <ciuki/kernel.h>
#include "engine.h"
#include "registers.h"

#define M64_WAIT_US 20000u
#define M64_MAX_POLLS 65536u
#define M64_FIFO_ERROR 0x80000000u /* Linux v6.12 mach64.h FIFO_ERR */
static uint32_t rd(struct m64_engine *s, unsigned r) { return s->io.read(s->io.ctx,r); }
static void wr(struct m64_engine *s, unsigned r, uint32_t v) { s->io.write(s->io.ctx,r,v); }
static void drain(struct m64_engine *s) { s->io.drain(s->io.ctx); }

/* A single budget covers both FIFO drain and GUI completion. A frozen clock
 * cannot turn any wait into an infinite loop. pause keeps fbdev's ownership. */
static int wait_ready(struct m64_engine *s, unsigned slots, bool idle)
{
    uint64_t start=s->io.now(s->io.ctx), slice=start;
    uint32_t limit=0x8000u>>slots;
    for (unsigned polls=0;polls<M64_MAX_POLLS;polls++) {
        uint32_t fifo=rd(s,FIFO_STAT);
        if (fifo==UINT32_MAX || (fifo&M64_FIFO_ERROR)) return -EIO;
        if ((fifo&0xffffu)<=limit && !idle) return 0;
        if (!(fifo&0xffffu) && idle) {
            uint32_t gui=rd(s,GUI_STAT);
            if (gui==UINT32_MAX) return -EIO;
            if (!(gui&ENGINE_BUSY)) return 0;
        }
        uint64_t now=s->io.now(s->io.ctx);
        if (now-start>=M64_WAIT_US) break;
        if (s->pause && (now-slice>=1000 || (polls && !(polls%256)))) {
            int e=s->pause(s->pause_arg);
            if (e) return e;
            slice=s->io.now(s->io.ctx);
        }
    }
    s->timeouts++;
    return -ETIMEDOUT;
}
static bool hide(struct m64_engine *s)
{
    uint32_t gen=rd(s,GEN_TEST_CNTL);
    if (gen==UINT32_MAX) { s->cursor_hidden=false; return false; }
    wr(s,GEN_TEST_CNTL,gen&~HWCURSOR_ENABLE);
    uint32_t got=rd(s,GEN_TEST_CNTL);
    s->cursor_hidden=got!=UINT32_MAX && !(got&HWCURSOR_ENABLE);
    s->cursor_visible=false;
    return s->cursor_hidden;
}
void m64_quarantine(struct m64_engine *s, const char *reason)
{
    s->active=false; s->quarantined=true; s->reason=reason;
    /* Nonwaiting. Quarantine is not an idle proof, even if hide succeeds. */
    s->cpu_safe=false;
    if (s->touched) (void)hide(s);
}
static int fault(struct m64_engine *s, int e)
{
    m64_quarantine(s,e==-ETIMEDOUT ? "engine_timeout" : "engine_fault");
    return e;
}
int m64_idle_proof(struct m64_engine *s)
{
    if (!s->touched) return s->cpu_safe ? 0 : -EIO;
    int e=wait_ready(s,16,true);
    if (!e && !hide(s)) e=-EIO;
    s->cpu_safe=!e;
    if (!e) drain(s);
    return e;
}
static int sync_engine(void *ctx)
{
    struct m64_engine *s=ctx;
    if (!s->touched) return s->cpu_safe ? 0 : -EIO;
    int e=wait_ready(s,16,true);
    if (e) return fault(s,e);
    if (s->quarantined) {
        if (!hide(s)) return fault(s,-EIO);
    }
    s->cpu_safe=!s->quarantined || s->cursor_hidden;
    drain(s);
    return s->cpu_safe ? 0 : -EIO;
}
static bool rect(const struct m64_engine *s,const struct fb_rect *r)
{
    return r && r->x>=0 && r->y>=0 && r->width>0 && r->height>0 &&
        (uint64_t)(uint32_t)r->x+(uint32_t)r->width<=s->fb.width &&
        (uint64_t)(uint32_t)r->y+(uint32_t)r->height<=s->fb.height;
}
static int admission(struct m64_engine *s)
{
    return s->active && !s->quarantined ? 0 : -EIO;
}
static int complete(struct m64_engine *s)
{
    int e=sync_engine(s);
    if (!e) s->commands++;
    return e;
}
static int fill(void *ctx,const struct fb_rect *r,uint32_t colour)
{
    struct m64_engine *s=ctx;
    if (admission(s)) return -EIO;
    if (!rect(s,r)) return -EOPNOTSUPP;
    drain(s);
    int e=wait_ready(s,6,false);
    if (e) return fault(s,e);
    wr(s,DP_SRC,BKGD_SRC_BKGD_CLR|FRGD_SRC_FRGD_CLR|MONO_SRC_ONE);
    wr(s,DP_FRGD_CLR,colour);
    wr(s,DP_MIX,(MIX_SRC<<16)|MIX_DST);
    wr(s,DST_CNTL,DST_X_LEFT_TO_RIGHT|DST_Y_TOP_TO_BOTTOM|DST_LAST_PEL);
    wr(s,DST_Y_X,((uint32_t)r->x<<16)|(uint32_t)r->y);
    s->cpu_safe=false;
    wr(s,DST_HEIGHT_WIDTH,((uint32_t)r->width<<16)|(uint32_t)r->height);
    return complete(s);
}
static int copy(void *ctx,const struct fb_rect *r,int32_t dx,int32_t dy)
{
    struct m64_engine *s=ctx;
    if (admission(s)) return -EIO;
    struct fb_rect dst={dx,dy,r ? r->width : 0,r ? r->height : 0};
    if (!rect(s,r) || !rect(s,&dst)) return -EOPNOTSUPP;
    uint32_t sx=r->x,sy=r->y,x=dx,y=dy,cmd=DST_LAST_PEL;
    if (dx<=r->x) cmd|=DST_X_LEFT_TO_RIGHT;
    else { sx+=r->width-1; x+=r->width-1; }
    if (dy<=r->y) cmd|=DST_Y_TOP_TO_BOTTOM;
    else { sy+=r->height-1; y+=r->height-1; }
    drain(s);
    int e=wait_ready(s,7,false);
    if (e) return fault(s,e);
    wr(s,DP_SRC,FRGD_SRC_BLIT);
    wr(s,DP_MIX,MIX_SRC<<16);
    wr(s,DST_CNTL,cmd);
    wr(s,SRC_Y_X,(sx<<16)|sy);
    wr(s,SRC_HEIGHT1_WIDTH1,((uint32_t)r->width<<16)|(uint32_t)r->height);
    wr(s,DST_Y_X,(x<<16)|y);
    s->cpu_safe=false;
    wr(s,DST_HEIGHT_WIDTH,((uint32_t)r->width<<16)|(uint32_t)r->height);
    return complete(s);
}
static uint32_t rgb(const struct fb_device *d,uint32_t pixel)
{
    uint32_t r=(pixel>>d->red_pos)&((1u<<d->red_size)-1);
    uint32_t g=(pixel>>d->green_pos)&((1u<<d->green_size)-1);
    uint32_t b=(pixel>>d->blue_pos)&((1u<<d->blue_size)-1);
    uint32_t rm=(1u<<d->red_size)-1,gm=(1u<<d->green_size)-1,bm=(1u<<d->blue_size)-1;
    r=(r*255+rm/2)/rm; g=(g*255+gm/2)/gm; b=(b*255+bm/2)/bm;
    return (r<<16)|(g<<8)|b;
}
static int cursor_set(void *ctx,const struct accel_cursor *c)
{
    struct m64_engine *s=ctx;
    if (admission(s)) return -EIO;
    if (!s->cursor) return -EOPNOTSUPP;
    if (!c) return -EINVAL;
    int e=sync_engine(s);
    if (e) return e;
    if (!hide(s)) return fault(s,-EIO);
    s->cursor_loaded=false;
    memset(s->cursor,0xaa,1024);
    for (unsigned y=0;y<32;y++) for (unsigned x=0;x<32;x++) {
        uint32_t bit=1u<<(31-x);
        unsigned pair=!(c->mask[y]&bit) ? 2 : (c->image[y]&bit) ? 1 : 0;
        unsigned i=y*16+x/4, shift=(x%4)*2;
        s->cursor[i]=(s->cursor[i]&~(3u<<shift))|(pair<<shift);
    }
    drain(s);
    wr(s,CUR_CLR0,rgb(&s->fb,c->background));
    wr(s,CUR_CLR1,rgb(&s->fb,c->foreground));
    wr(s,CUR_OFFSET,s->cursor_offset>>3);
    wr(s,CUR_HORZ_VERT_OFF,0);
    wr(s,CUR_HORZ_VERT_POSN,0);
    if (rd(s,CUR_OFFSET)!=(s->cursor_offset>>3) ||
        rd(s,CUR_CLR0)!=rgb(&s->fb,c->background) || rd(s,CUR_CLR1)!=rgb(&s->fb,c->foreground)) return fault(s,-EIO);
    s->cursor_loaded=true; s->cursor_outside=false;
    return 0;
}
static int cursor_show(void *ctx,bool show)
{
    struct m64_engine *s=ctx;
    if (admission(s)) return -EIO;
    if (!s->cursor) return -EOPNOTSUPP;
    if (show && !s->cursor_loaded) return -EINVAL;
    uint32_t gen=rd(s,GEN_TEST_CNTL);
    if (gen==UINT32_MAX) return fault(s,-EIO);
    uint32_t enable=show && !s->cursor_outside ? HWCURSOR_ENABLE : 0;
    wr(s,GEN_TEST_CNTL,(gen&~HWCURSOR_ENABLE)|enable);
    uint32_t got=rd(s,GEN_TEST_CNTL);
    if (got==UINT32_MAX || (got&HWCURSOR_ENABLE)!=enable) return fault(s,-EIO);
    s->cursor_visible=show; s->cursor_hidden=!enable;
    return 0;
}
static int cursor_move(void *ctx,int32_t x,int32_t y)
{
    struct m64_engine *s=ctx;
    if (admission(s)) return -EIO;
    if (!s->cursor) return -EOPNOTSUPP;
    if (!s->cursor_loaded) return -EINVAL;
    bool visible=s->cursor_visible;
    if (!hide(s)) return fault(s,-EIO);
    s->cursor_outside=x<=-32 || y<=-32 || x>=(int32_t)s->fb.width || y>=(int32_t)s->fb.height;
    if (s->cursor_outside) x=y=0;
    uint32_t ox=x<0 ? (uint32_t)-x : 0,oy=y<0 ? (uint32_t)-y : 0;
    uint32_t pos=((uint32_t)(y<0 ? 0 : y)<<16)|(uint32_t)(x<0 ? 0 : x);
    uint32_t offset=(s->cursor_offset>>3)+(oy<<1), clip=(oy<<16)|ox;
    wr(s,CUR_OFFSET,offset); wr(s,CUR_HORZ_VERT_OFF,clip); wr(s,CUR_HORZ_VERT_POSN,pos);
    if (rd(s,CUR_OFFSET)!=offset || rd(s,CUR_HORZ_VERT_OFF)!=clip || rd(s,CUR_HORZ_VERT_POSN)!=pos) return fault(s,-EIO);
    return cursor_show(s,visible);
}
static int stop(void *ctx)
{
    struct m64_engine *s=ctx;
    /* f7-27: the 3D owner closes admission and releases its heap first;
     * a failed close keeps this 2D owner (and its rejecting guard) alive. */
    if (s->render3d_stop) {
        int e=s->render3d_stop(s);
        if (e) return e;
    }
    s->active=false;
    int e=m64_idle_proof(s);
    if (e) return fault(s,e);
    s->cursor_loaded=false;
    s->reason=s->quarantined ? s->reason : "stopped";
    return 0;
}
static unsigned caps(void *ctx)
{
    struct m64_engine *s=ctx;
    return s->active && !s->quarantined ? 1u|(s->cursor ? 2u : 0) : 0;
}
static bool cpu_ready(void *ctx) { return ((struct m64_engine *)ctx)->cpu_safe; }
static void set_pause(void *ctx,int (*pause)(void *),void *arg)
{ struct m64_engine *s=ctx; s->pause=pause; s->pause_arg=arg; }
bool m64_format(const struct fb_device *d)
{
    return d && d->present && d->mapped && d->width>=16 && d->height>=16 &&
        d->width<=2048 && d->height<=2048 &&
        ((d->bpp==16 && d->red_size==5 && d->red_pos==11 && d->green_size==6 && d->green_pos==5 && d->blue_size==5 && !d->blue_pos) ||
         (d->bpp==32 && d->red_size==8 && d->red_pos==16 && d->green_size==8 && d->green_pos==8 && d->blue_size==8 && !d->blue_pos)) &&
        d->pitch>=d->width*(d->bpp/8u) && !(d->pitch%((d->bpp/8u)*8u)) &&
        d->pitch/((d->bpp/8u)*8u)<=1023 && (uint64_t)d->pitch*d->height<=d->size;
}
void m64_attach(struct m64_engine *s,const struct fb_device *d,const struct m64_io *io,uint8_t *cursor,uint32_t offset)
{
    memset(s,0,sizeof(*s)); s->fb=*d; s->io=*io; s->cursor=cursor; s->cursor_offset=offset;
    s->cpu_safe=true; s->reason="pending";
    s->ops=(struct accel_ops){.ctx=s,.sync=sync_engine,.fill_rect=fill,.copy_rect=copy,
        .cursor_set=cursor_set,.cursor_move=cursor_move,.cursor_show=cursor_show,
        .stop=stop,.capabilities=caps,.cpu_ready=cpu_ready,.set_pause=set_pause};
}
int m64_configure(struct m64_engine *s)
{
    if (s->quarantined || !m64_format(&s->fb)) return -EOPNOTSUPP;
    uint32_t width=s->fb.bpp==16 ? 4 : 6;
    uint32_t pitch=(s->fb.pitch/(s->fb.bpp/8u)/8u)<<22;
    /* Preserve firmware scanout, including panning/base and pitch. */
    if (rd(s,CRTC_OFF_PITCH)!=pitch || (rd(s,CRTC_GEN_CNTL)&0x700u)!=(width<<8)) return -EOPNOTSUPP;
    s->touched=true; s->cpu_safe=false;
    int e=wait_ready(s,16,true);
    if (e) return fault(s,e);
    if (!hide(s)) return fault(s,-EIO);
    uint32_t gen=rd(s,GEN_TEST_CNTL);
    wr(s,GEN_TEST_CNTL,gen&~GUI_ENGINE_ENABLE);
    wr(s,GEN_TEST_CNTL,gen|GUI_ENGINE_ENABLE);
    if ((rd(s,GEN_TEST_CNTL)&(GUI_ENGINE_ENABLE|HWCURSOR_ENABLE))!=GUI_ENGINE_ENABLE) return fault(s,-EIO);
    e=wait_ready(s,16,true);
    if (e) return fault(s,e);
    const uint32_t setup[][2]={
        {DP_PIX_WIDTH,(width<<16)|(width<<8)|width|BYTE_ORDER_LSB_TO_MSB},
        {DP_CHAIN_MASK,s->fb.bpp==16 ? DP_CHAIN_16BPP : DP_CHAIN_32BPP},
        {CONTEXT_MASK,UINT32_MAX},{DST_OFF_PITCH,pitch},{DST_Y_X,0},{DST_HEIGHT,0},
        {DST_BRES_ERR,0},{DST_BRES_INC,0},{DST_BRES_DEC,0},
        {DST_CNTL,DST_X_LEFT_TO_RIGHT|DST_Y_TOP_TO_BOTTOM|DST_LAST_PEL},
        {SRC_OFF_PITCH,pitch},{SRC_Y_X,0},{SRC_HEIGHT1_WIDTH1,0x10001},
        {SRC_Y_X_START,0},{SRC_HEIGHT2_WIDTH2,0x10001},{SRC_CNTL,SRC_LINE_X_LEFT_TO_RIGHT},
        {HOST_CNTL,rd(s,HOST_CNTL)&~HOST_BYTE_ALIGN},{PAT_REG0,0},{PAT_REG1,0},{PAT_CNTL,0},
        {SC_LEFT_RIGHT,(s->fb.width-1)<<16},{SC_TOP_BOTTOM,(s->fb.height-1)<<16},
        {DP_BKGD_CLR,0},{DP_FRGD_CLR,UINT32_MAX},{DP_WRITE_MASK,UINT32_MAX},
        {DP_MIX,(MIX_SRC<<16)|MIX_DST},{DP_SRC,FRGD_SRC_FRGD_CLR},
        {CLR_CMP_CLR,0},{CLR_CMP_MASK,UINT32_MAX},{CLR_CMP_CNTL,0}
    };
    for (unsigned i=0;i<ARRAY_SIZE(setup);i++) {
        e=wait_ready(s,1,false);
        if (e) return fault(s,e);
        wr(s,setup[i][0],setup[i][1]);
    }
    e=sync_engine(s);
    if (e) return e;
    s->active=true; s->reason="active";
    return 0;
}

/* f7-27 3D transport. Register writes go through the same GUI FIFO as the
 * 2D path (X.Org atimach64render.c:724-741 and :775-790 program the setup
 * engine with outf after ATIMach64WaitForFIFO). The DRM's MMIO pseudo-DMA
 * path waits for FIFO room before each group of at most 16 writes
 * (FreeBSD 8.4 mach64_dma.c:1324-1341). No sliced pause: the presenter is
 * retained across a 3D window, and one deadline/read budget covers it. */
#include "registers3d.h"
static const uint16_t writable3d[]={
    M64_DST_OFF_PITCH,M64_DST_Y_X,M64_DST_HEIGHT_WIDTH,M64_DST_CNTL,
    M64_Z_OFF_PITCH,M64_Z_CNTL,M64_ALPHA_TST_CNTL,
    M64_SRC_OFF_PITCH,M64_SRC_Y_X,M64_SRC_HEIGHT1_WIDTH1,M64_SCALE_3D_CNTL,
    M64_SC_LEFT_RIGHT,M64_SC_TOP_BOTTOM,M64_DP_FRGD_CLR,M64_DP_WRITE_MASK,
    M64_DP_PIX_WIDTH,M64_DP_MIX,M64_DP_SRC,M64_CLR_CMP_CNTL,
    M64_TEX_SIZE_PITCH,M64_TEX_CNTL,M64_SECONDARY_TEX_OFF,
    M64_ONE_OVER_AREA,M64_SETUP_CNTL
};
static const uint16_t readable3d[]={
    M64_FIFO_STAT,M64_GUI_STAT,M64_BUS_CNTL,M64_CONFIG_CHIP_ID,M64_CONFIG_CHIP_ID_LOW,
    M64_DST_OFF_PITCH,M64_DST_CNTL,M64_SRC_OFF_PITCH,M64_SCALE_3D_CNTL,
    M64_SC_LEFT,M64_SC_RIGHT,M64_SC_TOP,M64_SC_BOTTOM,M64_DP_FRGD_CLR,
    M64_DP_WRITE_MASK,M64_DP_PIX_WIDTH,M64_DP_MIX,M64_DP_SRC,M64_CLR_CMP_CNTL,
    M64_TEX_SIZE_PITCH,M64_TEX_CNTL
};
bool m64_render3d_writable(unsigned reg)
{
    if (reg&3u) return false;
    /* Vertex 1..3 S,T,W,SPEC_ARGB,Z,ARGB,X_Y: the DRM's verified setup
     * range (FreeBSD 8.4 mach64_state.c:521-526), plus the secondaries. */
    for (unsigned v=0;v<3;v++) {
        unsigned first=M64_VERTEX_1_S+v*M64_VERTEX_STRIDE;
        if (reg>=first && reg<=first+0x18u) return true;
    }
    if ((reg>=M64_VERTEX_3_SECONDARY && reg<=M64_VERTEX_3_SECONDARY+8u) ||
        (reg>=M64_VERTEX_1_SECONDARY && reg<=M64_VERTEX_2_SECONDARY+8u)) return true;
    if (reg>=M64_TEX_0_OFF && reg<=M64_TEX_0_OFF+40u) return true;
    for (unsigned i=0;i<ARRAY_SIZE(writable3d);i++) if (writable3d[i]==reg) return true;
    return false;
}
bool m64_render3d_readable(unsigned reg)
{
    if (reg&3u) return false;
    if (reg>=M64_TEX_0_OFF && reg<=M64_TEX_0_OFF+40u) return true;
    for (unsigned i=0;i<ARRAY_SIZE(readable3d);i++) if (readable3d[i]==reg) return true;
    return false;
}
static int wait3d(struct m64_engine *s,unsigned slots,bool idle,struct m64_budget *b)
{
    uint32_t limit=0x8000u>>slots;
    for (;;) {
        if (b->reads>=M64_3D_READ_LIMIT) { s->reason="render3d_poll_limit"; return -ETIMEDOUT; }
        if (s->io.now(s->io.ctx)-b->start>=M64_3D_WINDOW_US) { s->reason="render3d_deadline"; return -ETIMEDOUT; }
        b->reads++;
        uint32_t fifo=rd(s,FIFO_STAT);
        if (fifo==UINT32_MAX || (fifo&M64_FIFO_ERROR)) { s->reason="render3d_fifo"; return -EIO; }
        if (!idle && (fifo&0xffffu)<=limit) return 0;
        if (idle && !(fifo&0xffffu)) {
            if (b->reads>=M64_3D_READ_LIMIT) { s->reason="render3d_poll_limit"; return -ETIMEDOUT; }
            b->reads++;
            uint32_t gui=rd(s,GUI_STAT);
            if (gui==UINT32_MAX) { s->reason="render3d_dead_mmio"; return -EIO; }
            if (!(gui&ENGINE_BUSY)) return 0;
        }
    }
}
int m64_render3d_write(struct m64_engine *s,const struct m64_write *w,unsigned n,struct m64_budget *b)
{
    if (!s || !w || !b || !n || n>M64_3D_WRITE_LIMIT) return -EINVAL;
    for (unsigned i=0;i<n;i++) if (!m64_render3d_writable(w[i].reg)) return -EINVAL;
    if (!s->active || s->quarantined || !s->render3d_qualified) return -EIO;
    while (n) {
        unsigned group=n<16 ? n : 16;
        int e=wait3d(s,group,false,b);
        if (e) return e;
        s->cpu_safe=false; s->touched=true;
        for (unsigned i=0;i<group;i++) wr(s,w[i].reg,w[i].value);
        s->render3d_writes+=group; w+=group; n-=group;
    }
    return 0;
}
int m64_render3d_idle(struct m64_engine *s,struct m64_budget *b)
{
    if (!s || !b) return -EINVAL;
    drain(s);
    int e=wait3d(s,16,true,b);
    if (!e) { drain(s); s->cpu_safe=true; }
    return e;
}
int m64_render3d_read(struct m64_engine *s,unsigned reg,uint32_t *out)
{
    if (!s || !out || !m64_render3d_readable(reg)) return -EINVAL;
    *out=rd(s,reg);
    /* All-ones status or identity proves a dead/unclaimed window. State
     * registers can hold all-ones legitimately (2D DP_WRITE_MASK). */
    bool status=reg==M64_FIFO_STAT || reg==M64_GUI_STAT || reg==M64_BUS_CNTL ||
        reg==M64_CONFIG_CHIP_ID || reg==M64_CONFIG_CHIP_ID_LOW;
    return status && *out==UINT32_MAX ? -EIO : 0;
}
