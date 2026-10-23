/* Register interpreter independent of the driver's packet construction.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef MATROX_FIXTURE_H
#define MATROX_FIXTURE_H
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "../../src/kernel/drivers/video/matrox/matrox.h"

struct fake_write { unsigned reg; uint32_t value; };
struct fake_matrox {
    uint32_t regs[MATROX_MMIO_BYTES/4];
    uint8_t dac[256], index;
    uint8_t pixels[320*64*4+32], cursor[4096+32];
    struct fake_write trace[4096];
    unsigned writes, credits, capacity, reads, launches, drains, pauses;
    unsigned pending, words, busy_reads;
    uint32_t mono[8], mono_exec;
    uint64_t time, step;
    bool dead, full, wedge, wedge_launch, no_draw, bad_dac, vsync;
    struct fb_device fb;
};
static struct fake_matrox fake;
static uint32_t load_pixel(unsigned x,unsigned y)
{
    assert(x<fake.fb.width && y<fake.fb.height);
    uint32_t v=0;
    for (unsigned b=0;b<fake.fb.bpp/8;b++) v|=(uint32_t)fake.fb.mapped[y*fake.fb.pitch+x*(fake.fb.bpp/8)+b]<<(8*b);
    return v;
}
static void store_pixel(unsigned x,unsigned y,uint32_t value)
{
    assert(x<fake.fb.width && y<fake.fb.height);
    for (unsigned b=0;b<fake.fb.bpp/8;b++) fake.fb.mapped[y*fake.fb.pitch+x*(fake.fb.bpp/8)+b]=(uint8_t)(value>>(8*b));
}
static void execute(uint32_t ylen)
{
    unsigned op=fake.regs[M_DWGCTL/4]&15u;
    unsigned x0=fake.regs[M_FXBNDRY/4]&65535u, x1=fake.regs[M_FXBNDRY/4]>>16;
    int y=(int)(ylen>>16), lines=(int)(ylen&65535u);
    if (fake.no_draw) return;
    assert(fake.regs[M_PITCH/4]==fake.fb.pitch/(fake.fb.bpp/8));
    if (op==4) {
        for (int row=0;row<lines;row++) for (unsigned x=x0;x<x1;x++) store_pixel(x,(unsigned)y+row,fake.regs[M_FCOL/4]);
    } else if (op==8) {
        bool reverse=!(fake.regs[M_DWGCTL/4]&M_DWG_SGNZERO);
        int step=reverse ? -1 : 1;
        int64_t source=fake.regs[M_AR3/4], pitch=fake.regs[M_PITCH/4];
        assert(!reverse || fake.regs[M_SGN/4]==5);
        assert((int32_t)fake.regs[M_AR5/4]==step*pitch);
        for (int row=0;row<lines;row++,y+=step,source+=step*pitch) {
            for (unsigned i=0;i<=x1-x0;i++) {
                int64_t from=source+step*(int)i;
                assert(from>=0);
                store_pixel(reverse ? x1-i : x0+i,(unsigned)y,
                    load_pixel((unsigned)(from%pitch),(unsigned)(from/pitch)));
            }
        }
    } else if (op==9) {
        unsigned width=x1-x0+1;
        for (unsigned x=0;x<width;x++) {
            unsigned bit=(fake.mono[x/32]>>(8*((x/8)%4)+7-(x%8)))&1u;
            store_pixel(x0+x,(unsigned)y, fake.regs[(bit ? M_FCOL : M_BCOL)/4]);
        }
    } else assert(!"unexpected drawing opcode");
}
static uint32_t fake_read32(void *ctx,unsigned reg)
{
    assert(ctx==&fake && reg<MATROX_MMIO_BYTES && !(reg&3)); fake.reads++;
    if (fake.dead) return UINT32_MAX;
    bool busy=fake.wedge || fake.busy_reads;
    if (reg==M_STATUS) {
        if (fake.busy_reads) fake.busy_reads--;
        if (!busy && !fake.full) fake.credits=fake.capacity;
        return (busy ? M_BUSY : 0)|(fake.vsync ? M_VSYNC : 0);
    }
    if (reg==M_FIFOSTATUS) return fake.full ? 0 : fake.credits|(fake.credits==fake.capacity ? M_FIFO_EMPTY : 0);
    return fake.regs[reg/4];
}
static void fake_write32(void *ctx,unsigned reg,uint32_t value)
{
    assert(ctx==&fake && reg<MATROX_MMIO_BYTES && !(reg&3));
    assert(fake.writes<ARRAY_SIZE(fake.trace));
    fake.trace[fake.writes++]=(struct fake_write){reg,value};
    if ((reg>=0x1c00 && reg<0x1e00) || reg==M_SRCORG || reg==M_DSTORG || reg==M_OPMODE || reg==0) {
        assert(fake.credits && !fake.full); fake.credits--;
    }
    fake.regs[reg/4]=value;
    if (reg==(M_YDSTLEN|M_EXEC)) {
        fake.launches++;
        fake.wedge=fake.wedge_launch;
        if ((fake.regs[M_DWGCTL/4]&15u)==9) {
            fake.pending=((fake.regs[M_AR0/4]+1+7)/8+3)/4;
            fake.words=0; fake.mono_exec=value;
        } else execute(value);
    }
    if (!reg) {
        assert(fake.pending && fake.words<8);
        fake.mono[fake.words++]=value;
        if (fake.words==fake.pending) { execute(fake.mono_exec); fake.pending=0; }
    }
}
static uint8_t fake_read8(void *ctx,unsigned reg)
{
    assert(ctx==&fake);
    if (fake.dead || fake.bad_dac) return 255;
    assert(reg==M_DAC_INDEX || reg==M_DAC_DATA);
    return reg==M_DAC_INDEX ? fake.index : fake.dac[fake.index];
}
static void fake_write8(void *ctx,unsigned reg,uint8_t value)
{
    assert(ctx==&fake && (reg==M_DAC_INDEX || reg==M_DAC_DATA));
    if (reg==M_DAC_INDEX) fake.index=value;
    else if (!fake.bad_dac) fake.dac[fake.index]=value;
}
static uint64_t fake_now(void *ctx)
{ assert(ctx==&fake); fake.time+=fake.step; return fake.time; }
static void fake_drain(void *ctx) { assert(ctx==&fake); fake.drains++; }
static inline int fake_pause(void *ctx) { assert(ctx==&fake); fake.pauses++; return 0; }
static struct matrox_io fake_io(void)
{ return (struct matrox_io){&fake,fake_read32,fake_write32,fake_read8,fake_write8,fake_now,fake_drain}; }
static void fake_reset(unsigned bpp)
{
    memset(&fake,0,sizeof(fake)); fake.credits=fake.capacity=64; fake.step=10;
    memset(fake.pixels,0xa7,sizeof(fake.pixels)); memset(fake.cursor,0xba,sizeof(fake.cursor));
    fake.fb=(struct fb_device){.width=61,.height=43,.pitch=96*(bpp/8),.size=43*96*(bpp/8),
        .mapped=fake.pixels+16,.phys=0xe0000000,.generation=42,.handle=0,.bpp=bpp,.present=true,
        .red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,
        .blue_size=bpp==16 ? 5 : 8};
    if (bpp==24) fake.fb.pitch=64*3;
    fake.fb.size=fake.fb.pitch*fake.fb.height;
}
static inline struct matrox_engine fresh_engine(void)
{
    struct matrox_engine e={0}; struct matrox_io io=fake_io();
    assert(!matrox_engine_init(&e,&fake.fb,&io,fake.cursor+16,0x700000));
    return e;
}
static inline void fix_context(struct matrox_engine *e) { e->ops.ctx=e; }
static inline void clear_trace(void) { fake.writes=0; fake.launches=0; }
#endif
