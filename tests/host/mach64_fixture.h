/* Independent fake MMIO executor, not a hardware qualification.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_MACH64_FIXTURE_H
#define CIUKI_MACH64_FIXTURE_H
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../../src/kernel/drivers/video/mach64/engine.h"
#include "../../src/kernel/drivers/video/mach64/registers.h"
struct m64_model {
    uint32_t reg[1024];
    struct { unsigned reg; uint32_t value; } writes[4096];
    unsigned count,reads,drains,launches,pauses;
    uint64_t clock;
    bool frozen,hang_launch,busy_after_launch,sticky_cursor,corrupt,disconnected;
    int cancel;
    struct fb_device *fb;
};
static struct m64_model model;
static uint32_t model_read(void *ctx,unsigned reg)
{
    struct m64_model *m=ctx;
    assert(reg<4096 && !(reg&3)); m->reads++;
    return m->disconnected ? UINT32_MAX : m->reg[reg/4];
}
static void model_write(void *ctx,unsigned reg,uint32_t v)
{
    struct m64_model *m=ctx;
    assert(reg<4096 && !(reg&3) && m->count<4096);
    /* No scanout, panel, memory timing, PLL, PCI, DMA or bus-control writes. */
    assert(reg==GEN_TEST_CNTL || (reg>=CUR_CLR0 && reg<=CUR_HORZ_VERT_OFF) ||
        (reg>=DST_OFF_PITCH && reg<=DST_CNTL) ||
        (reg>=SRC_OFF_PITCH && reg<=SRC_CNTL) || reg==HOST_CNTL ||
        (reg>=PAT_REG0 && reg<=PAT_CNTL) || reg==SC_LEFT_RIGHT || reg==SC_TOP_BOTTOM ||
        (reg>=DP_BKGD_CLR && reg<=DP_SRC) ||
        (reg>=CLR_CMP_CLR && reg<=CLR_CMP_CNTL) || reg==CONTEXT_MASK);
    m->writes[m->count].reg=reg; m->writes[m->count++].value=v;
    if (reg==GEN_TEST_CNTL && m->sticky_cursor) v|=HWCURSOR_ENABLE;
    m->reg[reg/4]=v;
    if (reg!=DST_HEIGHT_WIDTH) return;
    assert(m->drains); m->launches++;
    if (m->hang_launch) { m->fb->mapped[0]^=0x55; m->reg[GUI_STAT/4]=1; return; }
    if (m->corrupt) return;
    unsigned w=v>>16,h=v&0xffff,bytes=m->fb->bpp/8;
    uint32_t dst=m->reg[DST_Y_X/4],src=m->reg[SRC_Y_X/4],dir=m->reg[DST_CNTL/4];
    int xdir=(dir&1) ? 1 : -1,ydir=(dir&2) ? 1 : -1;
    int dx=dst>>16,dy=dst&0xffff,sx=src>>16,sy=src&0xffff;
    bool blit=m->reg[DP_SRC/4]==0x300;
    assert(w && h && m->reg[DP_MIX/4]==(blit ? 0x70000u : 0x70003u));
    assert(m->reg[DST_OFF_PITCH/4]==(m->fb->pitch/bytes/8u)<<22);
    assert(m->reg[SRC_OFF_PITCH/4]==m->reg[DST_OFF_PITCH/4]);
    if (blit) assert(m->reg[SRC_HEIGHT1_WIDTH1/4]==v);
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        int px=dx+xdir*(int)x,py=dy+ydir*(int)y;
        assert(px>=0 && py>=0 && px<(int)m->fb->width && py<(int)m->fb->height);
        uint8_t *out=m->fb->mapped+py*m->fb->pitch+px*bytes;
        if (blit) {
            int qx=sx+xdir*(int)x,qy=sy+ydir*(int)y;
            assert(qx>=0 && qy>=0 && qx<(int)m->fb->width && qy<(int)m->fb->height);
            memcpy(out,m->fb->mapped+qy*m->fb->pitch+qx*bytes,bytes);
        } else {
            uint32_t pixel=m->reg[DP_FRGD_CLR/4]; memcpy(out,&pixel,bytes);
        }
    }
    if (m->busy_after_launch) m->reg[GUI_STAT/4]=1;
}
static uint64_t model_now(void *ctx)
{
    struct m64_model *m=ctx;
    if (!m->frozen) m->clock+=10;
    return m->clock;
}
static void model_drain(void *ctx) { ((struct m64_model *)ctx)->drains++; }
static inline int model_pause(void *ctx)
{ struct m64_model *m=ctx; m->pauses++; return m->cancel; }
static struct m64_io model_io(void)
{ return (struct m64_io){&model,model_read,model_write,model_now,model_drain}; }
static void model_reset(struct fb_device *fb)
{
    memset(&model,0,sizeof(model)); model.fb=fb;
    model.reg[CRTC_OFF_PITCH/4]=(fb->pitch/(fb->bpp/8u)/8u)<<22;
    model.reg[CRTC_GEN_CNTL/4]=fb->bpp==16 ? 0x400 : 0x600;
    model.reg[GEN_TEST_CNTL/4]=GUI_ENGINE_ENABLE|HWCURSOR_ENABLE|0x20;
    model.reg[CONFIG_CHIP_ID/4]=0x4c4d;
    model.reg[MEM_CNTL/4]=7; /* 4 MiB on this late-generation whitelist */
}
#endif
