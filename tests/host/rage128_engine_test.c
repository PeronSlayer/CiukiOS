/* Fake MMIO transport plus an independent pixel model; production packets.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "../../src/kernel/drivers/video/rage128/rage128.h"
#include "../../src/kernel/drivers/video/rage128/regs.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
struct entry { unsigned reg; uint32_t value; };
struct fake {
    uint32_t regs[0x4000/4];
    uint8_t pixels[32*160], expected[32*160], cursor[4096];
    struct entry trace[256];
    unsigned count, reads, launches, drains, pauses, step;
    uint64_t clock;
    bool valid, busy, cache_busy, dead, wedge, disable_fail, stale_on_pause;
    struct fb_device fb, ref;
};
static struct fake f;
static struct r128_engine engine;
static unsigned tests;
static uint32_t read_mmio(void *ctx, unsigned reg)
{
    struct fake *m=ctx; m->reads++;
    if (m->dead) return UINT32_MAX;
    if (reg==0x1740) return m->busy ? 0x80000000u : 64;
    if (reg==0x184) return m->cache_busy ? 0x80000000u : 0;
    return m->regs[reg/4];
}
static void execute(struct fake *m, unsigned width, unsigned height)
{
    unsigned bytes=m->fb.bpp/8;
    uint32_t xy=m->regs[0x1438/4], src=m->regs[0x1434/4], dir=m->regs[0x16c0/4];
    unsigned rop=(m->regs[0x146c/4]>>16)&255;
    m->launches++;
    for (unsigned i=0;i<height;i++) for (unsigned j=0;j<width;j++) {
        int y=(xy>>16)+(dir&2 ? (int)i : -(int)i);
        int x=(xy&0xffff)+(dir&1 ? (int)j : -(int)j);
        CHECK(x>=0 && y>=0 && (unsigned)x<m->fb.width && (unsigned)y<m->fb.height);
        uint8_t *d=m->pixels+y*m->fb.pitch+x*bytes;
        uint32_t value=0, old=0;
        memcpy(&old,d,bytes);
        if (rop==0xf0) value=m->regs[0x147c/4];
        else if (rop==0xcc) {
            int sy=(src>>16)+(dir&2 ? (int)i : -(int)i);
            int sx=(src&0xffff)+(dir&1 ? (int)j : -(int)j);
            CHECK(sx>=0 && sy>=0 && (unsigned)sx<m->fb.width && (unsigned)sy<m->fb.height);
            memcpy(&value,m->pixels+sy*m->fb.pitch+sx*bytes,bytes);
        } else { CHECK(rop==0x55); value=~old; }
        uint32_t mask=m->regs[0x16cc/4];
        value=(value&mask)|(old&~mask);
        memcpy(d,&value,bytes);
    }
    if (m->wedge) m->busy=true;
}
static void write_mmio(void *ctx, unsigned reg, uint32_t value)
{
    struct fake *m=ctx;
    CHECK(m->count<256);
    m->trace[m->count++]=(struct entry){reg,value};
    if (reg==0x50 && m->disable_fail) value|=1u<<16;
    m->regs[reg/4]=value;
    if (reg==0x1598) execute(m,value>>16,value&0xffff);
    if (reg==0x143c) execute(m,value&0xffff,value>>16);
}
static uint64_t time_us(void *ctx) { struct fake *m=ctx; uint64_t t=m->clock; m->clock+=m->step; return t; }
static void drain_mmio(void *ctx) { ((struct fake *)ctx)->drains++; }
static bool valid_mmio(void *ctx) { return ((struct fake *)ctx)->valid; }
static int pause_mmio(void *ctx)
{
    struct fake *m=ctx; m->pauses++;
    if (m->stale_on_pause) m->valid=false;
    return 0;
}
static void fixture(unsigned bpp, bool cursor)
{
    memset(&f,0,sizeof(f)); memset(&engine,0,sizeof(engine));
    f.valid=true; f.step=10;
    f.fb=(struct fb_device){.width=32,.height=32,.pitch=bpp==32 ? 160 : 80,.bpp=bpp,
        .size=bpp==32 ? 32*160 : 32*80,.mapped=f.pixels,.present=true,
        .red_size=bpp==32 ? 8 : 5,.red_pos=bpp==32 ? 16 : 11,
        .green_size=bpp==32 ? 8 : 6,.green_pos=bpp==32 ? 8 : 5,.blue_size=bpp==32 ? 8 : 5};
    f.ref=f.fb; f.ref.mapped=f.expected;
    f.regs[0x50/4]=0x03000000u|((bpp==32 ? 6 : 4)<<8);
    f.regs[0x22c/4]=f.fb.pitch/(bpp);
    f.regs[0x200/4]=3<<16; f.regs[0x208/4]=31<<16;
    memset(f.pixels,0xa5,sizeof(f.pixels)); memcpy(f.expected,f.pixels,sizeof(f.pixels));
    struct r128_io io={&f,read_mmio,write_mmio,time_us,drain_mmio,valid_mmio};
    CHECK(!r128_engine_init(&engine,&f.fb,io,cursor ? f.cursor : NULL,0x10000));
    CHECK(engine.cpu_safe && engine.active && !engine.quarantined);
    CHECK(engine.ops.capabilities(&engine)==(cursor ? 3u : 1u));
    CHECK(f.regs[0x16e4/4]==f.fb.pitch/bpp); /* padded pitch, in 8-pixel units */
    CHECK(!f.launches);
    f.count=0;
}
static void equal_pixels(void) { CHECK(!memcmp(f.pixels,f.expected,sizeof(f.pixels))); }
static void trace(unsigned index, unsigned reg, uint32_t value)
{ CHECK(index<f.count); CHECK(f.trace[index].reg==reg && f.trace[index].value==value); }
static void commands(unsigned bpp)
{
    fixture(bpp,true);
    struct fb_rect r={2,3,8,7};
    uint32_t colour=swdib_pack(&f.fb,0x123456);
    CHECK(!engine.ops.fill_rect(&engine,&r,colour));
    trace(0,0x146c,(bpp==32 ? 0x300036d0u : 0x300034d0u)|0x00f00000u);
    trace(1,0x147c,colour); trace(2,0x16c0,3); trace(3,0x1438,0x00030002);
    trace(4,0x1598,0x00080007); trace(5,0x184,255);
    swdib_fill(&f.ref,&r,colour); equal_pixels();
    /* Independent scalar model sees the exact packet direction; reference
     * swdib supplies snapshot-safe pixels across all overlap quadrants. */
    const int moves[4][2]={{4,5},{0,0},{4,1},{0,5}};
    for (unsigned i=0;i<4;i++) {
        f.count=0;
        struct fb_rect seed={0,0,32,32};
        for (unsigned y=0;y<32;y++) for (unsigned x=0;x<32;x++) {
            struct fb_rect one={(int)x,(int)y,1,1};
            swdib_fill(&f.ref,&one,swdib_pack(&f.ref,(y*37u<<16)|(x*7u<<8)|x*y));
        }
        memcpy(f.pixels,f.expected,sizeof(f.pixels));
        (void)seed;
        CHECK(!engine.ops.copy_rect(&engine,&r,moves[i][0],moves[i][1]));
        unsigned dir=(moves[i][0]<=r.x ? 1 : 0)|(moves[i][1]<=r.y ? 2 : 0);
        trace(1,0x16c0,dir);
        unsigned sx=r.x+(dir&1 ? 0 : r.width-1), sy=r.y+(dir&2 ? 0 : r.height-1);
        unsigned dx=moves[i][0]+(dir&1 ? 0 : r.width-1), dy=moves[i][1]+(dir&2 ? 0 : r.height-1);
        trace(2,0x1434,(sy<<16)|sx); trace(3,0x1438,(dy<<16)|dx);
        trace(4,0x143c,0x00070008);
        swdib_copy(&f.ref,&r,moves[i][0],moves[i][1]); equal_pixels();
    }
    uint8_t saved[sizeof(f.pixels)]; memcpy(saved,f.pixels,sizeof(saved));
    f.count=0;
    CHECK(!r128_invert(&engine,&r));
    unsigned bytes=bpp/8; uint32_t mask=bpp==32 ? 0xffffff : 0xffff;
    for (int y=r.y;y<r.y+r.height;y++) for (int x=r.x;x<r.x+r.width;x++) {
        uint32_t p=0; memcpy(&p,f.expected+y*f.fb.pitch+x*bytes,bytes); p^=mask;
        memcpy(f.expected+y*f.fb.pitch+x*bytes,&p,bytes);
    }
    equal_pixels(); trace(0,0x16cc,mask);
    f.count=0; CHECK(!r128_invert(&engine,&r)); CHECK(!memcmp(saved,f.pixels,sizeof(saved)));
    CHECK(engine.ops.pattern_rect==NULL && engine.ops.write_rect==NULL && engine.ops.offscreen==NULL);
    f.count=0; CHECK(!engine.ops.sync(&engine)); trace(0,0x184,255);
    tests++;
}
static void refusals(void)
{
    fixture(32,true);
    unsigned count=f.count, reads=f.reads;
    struct fb_rect bad[]={ {-1,0,2,2},{0,0,-1,2},{INT32_MAX,0,INT32_MAX,2},{0,31,2,2} };
    for (unsigned i=0;i<ARRAY_SIZE(bad);i++) {
        CHECK(engine.ops.fill_rect(&engine,&bad[i],0)==-EOPNOTSUPP);
        CHECK(engine.ops.copy_rect(&engine,&bad[i],0,0)==-EOPNOTSUPP);
        CHECK(r128_invert(&engine,&bad[i])==-EOPNOTSUPP);
    }
    struct fb_rect r={0,0,2,2};
    CHECK(engine.ops.copy_rect(&engine,&r,INT32_MAX,0)==-EOPNOTSUPP);
    r.width=0; CHECK(!engine.ops.fill_rect(&engine,&r,0));
    CHECK(f.count==count && f.reads==reads && !f.launches);
    CHECK(engine.ops.cursor_move(&engine,0,0)==-EINVAL);
    CHECK(engine.ops.cursor_show(&engine,true)==-EINVAL);
    tests++;
}
static void cursors(unsigned bpp)
{
    fixture(bpp,true);
    struct accel_cursor c={.foreground=swdib_pack(&f.fb,0x123456),
        .background=swdib_pack(&f.fb,0xabcdef)};
    c.mask[0]=0xc0000001u; c.image[0]=0x80000001u;
    c.mask[31]=0xffffffffu; c.image[31]=0x01020304u;
    CHECK(!engine.ops.cursor_set(&engine,&c));
    CHECK(f.cursor[0]==0x3f && f.cursor[3]==0xfe && f.cursor[8]==0x80 && f.cursor[11]==1);
    CHECK(f.cursor[31*16+8]==1 && f.cursor[31*16+11]==4);
    for (unsigned y=0;y<64;y++) for (unsigned x=0;x<64;x++) {
        unsigned bit=7-x%8;
        bool and_bit=!!(f.cursor[y*16+x/8]&(1u<<bit));
        bool xor_bit=!!(f.cursor[y*16+x/8+8]&(1u<<bit));
        bool opaque=y<32 && x<32 && (c.mask[y]&(1u<<(31-x)));
        bool image=y<32 && x<32 && (c.image[y]&(1u<<(31-x)));
        CHECK(and_bit==!opaque && xor_bit==(opaque&&image));
    }
    CHECK(f.regs[0x26c/4]==(bpp==32 ? 0xabcdefu : 0xadceefu));
    CHECK(f.regs[0x270/4]==(bpp==32 ? 0x123456u : 0x103552u));
    CHECK(engine.cursor_hidden);
    CHECK(!engine.ops.cursor_move(&engine,-2,-4));
    CHECK(f.regs[0x268/4]==0x80020004u && f.regs[0x264/4]==0x80000000u && f.regs[0x260/4]==0x10040);
    unsigned count=f.count;
    CHECK(engine.ops.cursor_move(&engine,INT32_MIN,0)==-EOPNOTSUPP && f.count==count);
    CHECK(engine.ops.cursor_move(&engine,-1,-3)==-EOPNOTSUPP && f.count==count);
    CHECK(!engine.ops.cursor_show(&engine,true) && !engine.cursor_hidden);
    CHECK(!engine.ops.cursor_set(&engine,&c) && engine.cursor_hidden);
    CHECK(!engine.ops.cursor_move(&engine,17,19));
    CHECK(f.regs[0x264/4]==0x80110013u && f.regs[0x260/4]==0x10000);
    CHECK(!engine.ops.cursor_show(&engine,true));
    CHECK(!engine.ops.stop(&engine) && engine.cursor_hidden && !engine.ops.capabilities(&engine));
    fixture(bpp,false);
    CHECK(engine.ops.cursor_set(&engine,&c)==-EOPNOTSUPP);
    CHECK(engine.ops.cursor_move(&engine,0,0)==-EOPNOTSUPP);
    CHECK(engine.ops.cursor_show(&engine,true)==-EOPNOTSUPP);
    tests++;
}
static void timeouts(void)
{
    struct fb_rect r={0,0,4,4};
    fixture(32,true); f.busy=true; f.step=100;
    engine.ops.set_pause(&engine,pause_mmio,&f);
    CHECK(engine.ops.fill_rect(&engine,&r,0)==-ETIMEDOUT);
    CHECK(!f.launches && engine.quarantined && !engine.cpu_safe && f.pauses>0);
    CHECK(!engine.ops.capabilities(&engine));
    f.busy=false; CHECK(!engine.ops.sync(&engine) && engine.cpu_safe);
    CHECK(engine.ops.fill_rect(&engine,&r,0)==-ENODEV);
    CHECK(r128_engine_init(&engine,&f.fb,engine.io,f.cursor,0x10000)==-EIO);
    fixture(32,true); f.busy=true; f.step=0;
    unsigned reads=f.reads;
    CHECK(engine.ops.sync(&engine)==-ETIMEDOUT && f.reads-reads<=R128_POLL_LIMIT+5);
    CHECK(!engine.cpu_safe && engine.quarantined && !f.launches);
    fixture(32,true); f.wedge=true;
    CHECK(engine.ops.copy_rect(&engine,&r,1,1)==-ETIMEDOUT);
    CHECK(f.launches==1 && !engine.cpu_safe && engine.submitted);
    CHECK(engine.ops.stop(&engine)==-ETIMEDOUT); CHECK(!engine.cpu_safe);
    f.busy=false; CHECK(!engine.ops.sync(&engine)); CHECK(f.launches==1); /* no replay */
    fixture(32,true); f.cache_busy=true;
    CHECK(engine.ops.sync(&engine)==-ETIMEDOUT && !engine.cpu_safe);
    fixture(32,true); f.dead=true;
    CHECK(engine.ops.sync(&engine)==-EIO && engine.quarantined && !engine.cpu_safe);
    fixture(32,true); f.busy=true; f.step=100; f.stale_on_pause=true;
    engine.ops.set_pause(&engine,pause_mmio,&f);
    CHECK(engine.ops.fill_rect(&engine,&r,0)==-EIO && f.pauses==1 && !f.launches && !engine.cpu_safe);
    fixture(32,true); f.disable_fail=true;
    r128_quarantine(&engine,"test");
    CHECK(!engine.cpu_safe && !engine.cursor_hidden && engine.ops.sync(&engine)==-EIO);
    tests++;
}
static void scanout_refusal(void)
{
    for (unsigned which=0;which<8;which++) {
        fixture(32,false); struct r128_engine next={.cpu_safe=true};
        switch (which) {
        case 0: f.regs[0x224/4]=16; break;
        case 1: f.regs[0x228/4]=1; break;
        case 2: f.regs[0x22c/4]=1; break;
        case 3: f.regs[0x50/4]=0x03000400; break;
        case 4: f.regs[0x200/4]=0; break;
        case 5: f.regs[0x208/4]=0; break;
        case 6: f.regs[0x704/4]=1u<<28; break;
        default: f.regs[0x50/4]=UINT32_MAX; break;
        }
        f.count=0;
        CHECK(r128_engine_init(&next,&f.fb,engine.io,NULL,0)==-EOPNOTSUPP);
        CHECK(!f.count && !next.touched && !next.quarantined);
    }
    fixture(32,false); f.fb.bpp=24;
    CHECK(!r128_format(&f.fb));
    tests++;
}
static void rgb555(void)
{
    fixture(16,true);
    f.fb.green_size=5; f.fb.red_pos=10;
    f.ref=f.fb; f.ref.mapped=f.expected;
    f.regs[0x50/4]=0x03000300;
    struct r128_engine rgb={0};
    CHECK(!r128_engine_init(&rgb,&f.fb,engine.io,f.cursor,0x10000));
    struct fb_rect r={1,1,7,9};
    CHECK(!rgb.ops.fill_rect(&rgb,&r,0x7fff));
    swdib_fill(&f.ref,&r,0x7fff); equal_pixels();
    CHECK(rgb.rgb_mask==0x7fff && ((rgb.master>>8)&7u)==3);
    CHECK(!rgb.ops.copy_rect(&rgb,&r,3,4));
    swdib_copy(&f.ref,&r,3,4); equal_pixels();
    CHECK(!r128_invert(&rgb,&r));
    CHECK(!r128_invert(&rgb,&r)); equal_pixels();
    tests++;
}
int main(void)
{
    commands(16); commands(32); refusals(); cursors(16); cursors(32); timeouts(); scanout_refusal(); rgb555();
    printf("rage128 engine: %u groups passed\n",tests);
    return 0;
}
