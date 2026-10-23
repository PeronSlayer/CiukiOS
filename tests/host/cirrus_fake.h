/* Independent fake for the QEMU v9.2.0 GD5446 BitBLT register subset.
 * Reference: hw/display/cirrus_vga.c, not an imported emulator implementation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <string.h>
#include <ciuki/kernel.h>
#include "../../src/kernel/drivers/video/cirrus/engine.h"
struct cirrus_fake {
    uint8_t pixels[64*32*4], gr[256];
    struct { uint8_t index,value; } trace[4096];
    unsigned trace_count, starts, resets, drains, reads, busy_reads;
    unsigned step, busy, delay_reads;
    uint64_t time;
    bool stuck, reset_fails, absent, invalid, corrupt, partial;
};
static uint32_t fake_value(struct cirrus_fake *f, unsigned index, unsigned bytes)
{
    uint32_t v=0;
    for (unsigned b=0;b<bytes;b++) v|=(uint32_t)f->gr[index+b]<<(b*8);
    return v;
}
static void fake_execute(struct cirrus_fake *f)
{
    unsigned width=fake_value(f,0x20,2)+1, height=fake_value(f,0x22,2)+1;
    unsigned pitch=fake_value(f,0x24,2), source_pitch=fake_value(f,0x26,2);
    int64_t src=fake_value(f,0x2c,3), dst=fake_value(f,0x28,3);
    uint8_t mode=f->gr[0x30];
    assert(f->gr[0x32]==0x0d && pitch==source_pitch);
    assert(!(mode&0x0e)); /* Never host transfers or transparency. */
    f->starts++; f->gr[0x31]|=3;
    f->busy=f->delay_reads;
    if (f->corrupt) return;
    unsigned bytes=((mode>>4)&3)+1;
    static const unsigned colours[4]={1,0x11,0x13,0x15};
    int direction=mode&1 ? -1 : 1;
    for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++) {
        int64_t d=dst+direction*((int64_t)y*pitch+x);
        assert(d>=0 && d<(int64_t)sizeof(f->pixels));
        if ((mode&0xc0)==0xc0) f->pixels[d]=f->gr[colours[x%bytes]];
        else {
            assert((mode&~1)==0);
            int64_t s=src+direction*((int64_t)y*source_pitch+x);
            assert(s>=0 && s<(int64_t)sizeof(f->pixels));
            f->pixels[d]=f->pixels[s];
        }
        if (f->partial) return;
    }
}
static uint8_t fake_read(void *ctx, uint8_t index)
{
    struct cirrus_fake *f=ctx;
    f->reads++;
    if (f->absent) return 255;
    if (index==0x31) {
        f->busy_reads++;
        if (!f->stuck && !f->busy) f->gr[index]&=(uint8_t)~3u;
        if (f->busy) f->busy--;
    }
    return f->gr[index];
}
static void fake_write(void *ctx, uint8_t index, uint8_t value)
{
    struct cirrus_fake *f=ctx;
    assert(f->trace_count<ARRAY_SIZE(f->trace));
    f->trace[f->trace_count].index=index;
    f->trace[f->trace_count++].value=value;
    uint8_t old=f->gr[index];
    if (index==0x21 || index==0x23 || index==0x25 || index==0x27) value&=0x1f;
    if (index==0x2a || index==0x2e) value&=0x3f;
    f->gr[index]=value;
    if (index==0x31) {
        if ((old&4) && !(value&4)) {
            f->resets++;
            if (f->reset_fails) f->gr[index]=3;
            else { f->gr[index]=0; f->stuck=false; f->busy=0; }
        } else if (!(old&2) && (value&2)) fake_execute(f);
    } else if (index==0x2a && (f->gr[0x31]&0x80)) fake_execute(f);
}
static uint64_t fake_now(void *ctx)
{ struct cirrus_fake *f=ctx; uint64_t n=f->time; f->time+=f->step; return n; }
static void fake_drain(void *ctx) { ((struct cirrus_fake *)ctx)->drains++; }
static bool fake_valid(void *ctx) { return !((struct cirrus_fake *)ctx)->invalid; }
static inline struct cirrus_io fake_io(struct cirrus_fake *f)
{ return (struct cirrus_io){f,fake_read,fake_write,fake_now,fake_drain,fake_valid}; }
static struct fb_device fake_fb(struct cirrus_fake *f, unsigned bpp)
{
    unsigned bytes=bpp/8;
    return (struct fb_device){.width=32,.height=24,.pitch=32*bytes+8,.size=24*(32*bytes+8),
        .phys=0xe0000000,.mapped=f->pixels,.bpp=(uint8_t)bpp,
        .red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,
        .blue_size=bpp==16 ? 5 : 8,.blue_pos=0,
        .present=true,.handle=0,.generation=42};
}
