/* Independent register model, not a silicon emulator or hardware evidence. */
#ifndef NEO_FIXTURE_H
#define NEO_FIXTURE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "../../src/kernel/drivers/video/neomagic/neo.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%u: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
struct fake_neo {
    uint8_t pixels[300*32*3+32*8], cursor[1024+16];
    uint32_t regs[0x1020/4], payload[192];
    struct { unsigned reg; uint32_t value; } trace[4096];
    unsigned writes, reads, launches, payload_words, payload_need, drains, hides, polls, busy_reads;
    uint64_t now, step;
    bool stuck, lost, zero_map, hide_failed, mismatch, corrupt, after_launch_stuck;
    struct fb_device fb;
};
static struct fake_neo fake;
static struct neo_engine test_engine;
static void model_execute(struct fake_neo *f)
{
    unsigned bytes=f->fb.bpp/8, w=f->regs[NEO_EXTENT/4]&65535, h=f->regs[NEO_EXTENT/4]>>16;
    unsigned control=f->regs[NEO_CONTROL/4];
    bool reverse=!!(control&NEO_BC0_X_DEC);
    CHECK(!(control&NEO_BC1_MSTR_BLT));
    if (control&NEO_BC0_SYS_TO_VID) {
        CHECK(h==1 && w<=256);
        if (!f->corrupt && !(control&NEO_BC0_SRC_MONO))
            memcpy(f->pixels+f->regs[NEO_DST/4],f->payload,w*bytes);
        else if (!f->corrupt) for (unsigned x=0;x<w;x++) {
            const uint8_t *bits=(const uint8_t *)f->payload;
            uint32_t c=f->regs[(bits[x/8]&(0x80u>>(x%8))) ? NEO_FG/4 : NEO_BG/4];
            for (unsigned b=0;b<bytes;b++) f->pixels[f->regs[NEO_DST/4]+x*bytes+b]=(uint8_t)(c>>(8*b));
        }
        return;
    }
    for (unsigned y=0;y<h;y++) for (unsigned x=0;x<w;x++) {
        int32_t delta=(int32_t)(y*f->fb.pitch+x*bytes)*(reverse ? -1 : 1);
        int64_t dst=(int64_t)f->regs[NEO_DST/4]+delta;
        CHECK(dst>=0 && (uint64_t)dst+bytes<=f->fb.size);
        uint32_t value=f->regs[NEO_FG/4];
        if (!(control&NEO_BC0_SRC_IS_FG)) {
            int64_t src=(int64_t)f->regs[NEO_SRC/4]+delta;
            CHECK(src>=0 && (uint64_t)src+bytes<=f->fb.size);
            value=0;
            for (unsigned b=0;b<bytes;b++) value|=(uint32_t)f->pixels[src+b]<<(b*8);
        }
        if (!f->corrupt) for (unsigned b=0;b<bytes;b++) {
            uint8_t c=(uint8_t)(value>>(b*8));
            if ((control&0x0f0000)==0x060000) f->pixels[dst+b]^=c;
            else f->pixels[dst+b]=c;
        }
    }
}
static uint32_t model_read(void *ctx,unsigned reg)
{
    struct fake_neo *f=ctx; f->reads++;
    CHECK(!(reg&3u) && reg<sizeof(f->regs));
    if (f->zero_map) return 0;
    if (reg==NEO_STAT) {
        f->polls++;
        if (f->lost) return UINT32_MAX;
        if (f->stuck || f->payload_need || (f->busy_reads && f->busy_reads--)) return 1;
        return f->regs[0]&~1u;
    }
    return f->regs[reg/4]^(f->mismatch ? 1u : 0u);
}
static void model_write(void *ctx,unsigned reg,uint32_t value)
{
    struct fake_neo *f=ctx;
    CHECK(f->writes<ARRAY_SIZE(f->trace));
    f->trace[f->writes].reg=reg; f->trace[f->writes++].value=value;
    if (reg>=NEO_UPLOAD) {
        CHECK(f->payload_need && reg==NEO_UPLOAD+f->payload_words*4);
        f->payload[f->payload_words++]=value;
        if (!--f->payload_need) model_execute(f);
        return;
    }
    CHECK(!(reg&3u) && reg<sizeof(f->regs));
    f->regs[reg/4]=value;
    if (reg==NEO_EXTENT) {
        f->launches++;
        if (f->after_launch_stuck) f->stuck=true;
        if (f->regs[NEO_CONTROL/4]&NEO_BC0_SYS_TO_VID) {
            unsigned count=(value&65535);
            count=(f->regs[NEO_CONTROL/4]&NEO_BC0_SRC_MONO) ? (count+7)/8 : count*(f->fb.bpp/8u);
            f->payload_need=(count+3)/4;
            f->payload_words=0;
        } else model_execute(f);
    }
}
static uint64_t model_now(void *ctx)
{ struct fake_neo *f=ctx; f->now+=f->step; return f->now; }
static void model_drain(void *ctx) { ((struct fake_neo *)ctx)->drains++; }
static bool model_hide(void *ctx)
{
    struct fake_neo *f=ctx; f->hides++;
    if (!f->hide_failed) f->regs[(NEO_CURSOR+NEOREG_CURSCNTL)/4]=0;
    return !f->hide_failed;
}
static struct neo_io model_io(void)
{ return (struct neo_io){&fake,model_read,model_write,model_now,model_drain,model_hide}; }
static void model_reset(unsigned bpp,unsigned width)
{
    memset(&fake,0,sizeof(fake)); memset(&test_engine,0,sizeof(test_engine));
    fake.fb=(struct fb_device){.present=true,.mapped=fake.pixels,.width=width,.height=32,
        .bpp=(uint8_t)bpp,.pitch=width*(bpp/8)+5,.phys=0xe0000000,
        .red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8,
        .handle=0,.generation=42};
    fake.fb.size=fake.fb.pitch*fake.fb.height;
    for (unsigned i=0;i<fake.fb.size;i++) fake.pixels[i]=(uint8_t)(i*71+13);
    memset(fake.cursor,0xa5,sizeof(fake.cursor));
}
static inline void model_setup(unsigned bpp,unsigned width,bool cursor)
{
    model_reset(bpp,width);
    struct neo_io io=model_io();
    CHECK(!neo_setup(&test_engine,&io,&fake.fb,cursor ? fake.cursor+8 : NULL,0x1ff000));
    CHECK(!neo_dm9(&test_engine));
    CHECK(test_engine.active && test_engine.cpu_safe && !test_engine.quarantined);
    fake.writes=fake.launches=0;
}
#endif
