/* Fake PIO register boundary, independent pixel interpreter and swdib goldens.
 * This proves command construction, never silicon behavior.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/swdib.h>
#include "../../src/kernel/drivers/video/nv2d/nv2d.h"

#include "nv2d_fixture.h"
static unsigned comparisons;
static struct fb_device setup(uint16_t id, unsigned bpp, bool rgb555)
{
    memset(&m,0,sizeof(m)); memset(&engine,0,sizeof(engine));
    m.valid=true; m.width=37; m.height=29; m.bytes=bpp/8;
    m.regs[0x600860/4]=0x1000;
    m.regs[0x100200/4]=0x12345678; m.regs[0x100204/4]=0x76543210;
    m.pitch=(37*m.bytes+63)&~63u; m.tail_offset=8*1024*1024-nv2d_tail_bytes(nv2d_architecture(id));
    m.scratch_offset=nv2d_architecture(id)>=0x40 ? 8*1024*1024-576*1024 : m.tail_offset;
    struct fb_device f={.present=true,.mapped=m.pixels,.width=m.width,.height=m.height,
        .bpp=bpp,.pitch=m.pitch,.size=29*m.pitch,.generation=7,.red_size=bpp==16 ? 5 : 8,
        .red_pos=bpp==16 ? rgb555 ? 10 : 11 : 16,.green_size=bpp==16 ? rgb555 ? 5 : 6 : 8,
        .green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8};
    int initialized=nv2d_engine_init(&engine,&model_io,&f,id,8*1024*1024,m.tail);
    if (initialized) fprintf(stderr,"init %04x: %d %s get=%x put=%x\n",id,initialized,engine.reason,reg(NV2D_GET),reg(NV2D_PUT));
    assert(!initialized);
    assert(engine.active && engine.cpu_safe && !engine.quarantined);
    assert(engine.ops.capabilities(&engine)==3);
    if (!engine.dma) {
    assert(reg(0x3204)==0 && reg(0x3220)==0 && reg(0x322c)==0); /* PIO, no DMA */
    assert(!reg(0x712090)); /* no command-fetch DMA object */
    assert(reg(0x712010)==(nv2d_architecture(id)==4 ? 0x01008042 : 0x01008062));
    } else {
        assert(reg(0x3204)==(engine.architecture==0x40 ? 0x10000 : 0x100));
        assert(reg(0x3220)==1 && reg(0x3224)==0xf0078);
        assert(reg(0x322c)==(engine.architecture==0x40 ? 0x1213 : 0x1209));
        unsigned object=engine.architecture==0x40 ? 0x712130 : 0x712090;
        assert(reg(object)==0x3002 && reg(object+4)==0x7fff);
        assert(reg(object+8)==(engine.ring_offset|2));
        assert(reg(engine.architecture==0x40 ? 0x7120c0 : 0x712060)==
            (engine.architecture==0x40 ? 0x0208009f : 0x0100809f));
        assert(reg(0x400084)==(engine.architecture==0x40 ? 0x401287c0 : 0x40108700));
        assert(reg(engine.architecture==0x30 || (id&0xfff0)==0x0040 ? 0x4009a4 : 0x4009f0)==0x12345678);
        assert(reg(engine.architecture==0x30 || (id&0xfff0)==0x0040 ? 0x4009a8 : 0x4009f4)==0x76543210);
        if (engine.architecture==0x40) {
            unsigned group=id&0xfff0;
            assert(reg(group==0x0040 ? 0x100244 : 0x100604)==(8u<<20)-1);
            if (group==0x0040 || group==0x0210) {
                /* NV48's subsequent tile mirror includes 09b8. Check the
                 * initialization write in order, rather than its final value. */
                bool initialized=false;
                for (unsigned i=0;i<m.traced;i++)
                    if (m.trace[i].reg==0x4009b8 && m.trace[i].value==0x0078e366) initialized=true;
                assert(initialized);
            }
            if (group==0x00c0) assert(reg(0x400828)==0x007596ff);
            if (group==0x0140) assert(reg(0x400828)==0x0072cb77);
            if (group==0x0160 || group==0x0220) assert(reg(0x680608)&0x00100000);
        }
    }
    assert(reg(0x712004)==8*1024*1024-1);
    for (unsigned i=0;i<8;i++) assert(reg(NV2D_FIFO+i*0x2000)==0x80000010+i);
    return f;
}
static void reset_pixels(const struct fb_device *f)
{
    memset(m.pixels,0xa6,sizeof(m.pixels)); memset(m.tail+(m.scratch_offset-m.tail_offset),0xb7,NV2D_SCRATCH_BYTES);
    for (unsigned y=0;y<f->height;y++) for (unsigned x=0;x<f->width;x++)
        put_pixel(m.pixels+y*f->pitch+x*m.bytes,swdib_pack(f,0x17538d+x*17083+y*571));
    memcpy(m.golden,m.pixels,sizeof(m.pixels));
}
static void compare(void) { assert(!memcmp(m.pixels,m.golden,sizeof(m.pixels))); comparisons++; }
static void sequence(uint16_t id, unsigned bpp, bool rgb555)
{
    struct fb_device f=setup(id,bpp,rgb555), ref=f; ref.mapped=m.golden;
    reset_pixels(&f);
    unsigned first=m.traced;
    struct fb_rect r={3,2,23,19};
    uint32_t colour=swdib_pack(&f,0x367a92);
    assert(!engine.ops.fill_rect(&engine,&r,colour)); swdib_fill(&ref,&r,colour); compare();
    const uint32_t methods[]={0x4310,0x4314,0x4318,0x431c,0x2300,0xc3fc,0xc400,0xc404};
    unsigned found=0;
    for (unsigned i=first;i<m.traced;i++) if (m.trace[i].reg!=NV2D_PUT) {
        assert(found<ARRAY_SIZE(methods) && m.trace[i].reg==NV2D_FIFO+methods[found++]);
    }
    assert(found==ARRAY_SIZE(methods));
    assert(reg(NV2D_FIFO+0xc400)==(3u<<16|2) && reg(NV2D_FIFO+0xc404)==(23u<<16|19));
    uint8_t bits[]={0x80,0x42,0x24,0x11,0x08,0x04,0x02,0x81};
    uint32_t fg=swdib_pack(&f,0x1245ac), bg=swdib_pack(&f,0xc75412);
    assert(!engine.ops.pattern_rect(&engine,&r,bits,fg,bg));
    swdib_pattern(&ref,&r,bits,fg,bg); compare();
    assert(reg(NV2D_FIFO+0x4318)==0x88244201);
    for (int dy=-4;dy<=4;dy++) for (int dx=-4;dx<=4;dx++) {
        reset_pixels(&f); struct fb_rect src={5,5,23,17};
        assert(!engine.ops.copy_rect(&engine,&src,src.x+dx,src.y+dy));
        swdib_copy(&ref,&src,src.x+dx,src.y+dy); compare();
    }
    reset_pixels(&f);
    struct fb_rect disjoint={0,0,4,3};
    assert(!engine.ops.copy_rect(&engine,&disjoint,30,24));
    swdib_copy(&ref,&disjoint,30,24); compare();
    uint8_t input[23*160];
    for (unsigned i=0;i<sizeof(input);i++) input[i]=(uint8_t)(i*71+19);
    struct fb_rect dst={-2,-1,31,23}, clip={3,4,19,17};
    assert(!engine.ops.write_rect(&engine,&dst,&clip,input,160));
    swdib_write(&ref,&dst,&clip,input,160); compare();
    assert(reg(NV2D_FIFO+0x304)==(f.pitch|(f.pitch<<16)) && !reg(NV2D_FIFO+0x308));
    assert(!engine.ops.sync(&engine) && engine.ops.cpu_ready(&engine));
    uint8_t saved[sizeof(m.pixels)]; memcpy(saved,m.pixels,sizeof(saved));
    assert(!nv2d_compare(&engine));
    assert(!memcmp(saved,m.pixels,sizeof(saved))); comparisons++;
    unsigned writes=m.traced;
    struct fb_rect bad={-1,0,1,1};
    assert(engine.ops.fill_rect(&engine,&bad,0)==-EOPNOTSUPP);
    assert(engine.ops.copy_rect(&engine,&r,36,28)==-EOPNOTSUPP);
    assert(engine.ops.pattern_rect(&engine,&r,NULL,0,0)==-EOPNOTSUPP);
    assert(engine.ops.write_rect(&engine,&dst,&clip,NULL,160)==-EOPNOTSUPP);
    assert(engine.ops.write_rect(&engine,&dst,&clip,m.pixels,160)==-EOPNOTSUPP);
    assert(engine.ops.write_rect(&engine,&dst,&clip,input,1)==-EOPNOTSUPP);
    assert(m.traced==writes); compare();
    assert(!engine.ops.stop(&engine) && engine.ops.cpu_ready(&engine));
    assert(!engine.ops.capabilities(&engine));
    printf("nv2d %04x/%u%s: object setup, transport, FILL/COPY(81 overlaps)/PATTERN/WRITE, guards PASS\n",
        id,bpp,rgb555 ? "/555" : "");
}
static void cursors(uint16_t id)
{
    setup(id,32,false);
    assert(engine.ops.cursor_show(&engine,true)==-EINVAL);
    assert(engine.ops.cursor_move(&engine,0,0)==-EINVAL);
    struct accel_cursor cursor={.foreground=0x123456,.background=0xabcdef};
    cursor.mask[0]=0xc0000000; cursor.image[0]=0x80000000;
    m.loading_cursor=true;
    assert(!engine.ops.cursor_set(&engine,&cursor));
    m.loading_cursor=false;
    assert(!(m.cursor[0x31]&1) && engine.cursor_hidden);
    if (engine.alpha_cursor) {
        uint32_t p; memcpy(&p,m.tail+(engine.cursor_offset-m.tail_offset),4); assert(p==0xff123456);
        memcpy(&p,m.tail+(engine.cursor_offset-m.tail_offset)+4,4); assert(p==0xffabcdef);
        for (unsigned x=8;x<64*4;x++) assert(m.tail[engine.cursor_offset-m.tail_offset+x]==0);
    } else if (engine.architecture==4) assert(reg(0x717800)==0xd73d88ca);
    else {
        uint32_t p; memcpy(&p,m.tail+(engine.cursor_offset-m.tail_offset),4); assert(p==0xd73d88ca);
    }
    assert(!engine.ops.cursor_move(&engine,-9,-7)); assert(reg(0x680300)==0xfff9fff7);
    unsigned before_show=m.traced;
    assert(!engine.ops.cursor_show(&engine,true) && (m.cursor[0x31]&1));
    if (engine.architecture==0x40) {
        assert(m.traced==before_show+1);
        assert(m.trace[before_show].reg==0x680300 && m.trace[before_show].value==0xfff9fff7);
    } else assert(m.traced==before_show);
    m.loading_cursor=true;
    assert(!engine.ops.cursor_set(&engine,&cursor) && !(m.cursor[0x31]&1));
    m.loading_cursor=false;
    unsigned writes=m.traced;
    assert(engine.ops.cursor_move(&engine,32768,0)==-EOPNOTSUPP && writes==m.traced);
    assert(!engine.ops.cursor_show(&engine,true));
    nv2d_quarantine(&engine,"test");
    assert(!(m.cursor[0x31]&1) && !engine.cpu_safe && !engine.ops.capabilities(&engine));
    assert(!engine.ops.sync(&engine) && engine.cpu_safe);
    assert(engine.ops.cursor_show(&engine,true)==-EIO);
    assert(!engine.ops.stop(&engine));
}
static void failures(void)
{
    setup(0x0250,32,false);
    m.stuck=true; m.frozen=true; engine.ops.set_pause(&engine,pause_wait,&m);
    assert(engine.ops.sync(&engine)==-ETIMEDOUT);
    assert(engine.quarantined && !engine.cpu_safe && m.delays==NV2D_WAIT_POLLS && m.pauses);
    unsigned writes=m.traced; struct fb_rect r={0,0,4,4};
    assert(engine.ops.fill_rect(&engine,&r,0)==-EIO && writes==m.traced);
    assert(engine.ops.stop(&engine)==-EIO && !engine.cpu_safe);
    m.stuck=false; assert(!engine.ops.sync(&engine) && engine.cpu_safe && engine.quarantined);
    assert(!engine.ops.stop(&engine));
    setup(0x0020,16,false); m.hold_fifo=true;
    assert(engine.ops.fill_rect(&engine,&r,0)==-ETIMEDOUT && engine.quarantined && !engine.cpu_safe);
    setup(0x0171,32,false); m.dead=true;
    assert(engine.ops.sync(&engine)==-EIO && !engine.cpu_safe);
    setup(0x0200,32,false); m.ignore_stop=true;
    assert(engine.ops.stop(&engine)==-EIO && !engine.cpu_safe && engine.quarantined);
    setup(0x0100,32,false); m.stuck=true; m.pause_error=-EINTR;
    engine.ops.set_pause(&engine,pause_wait,&m);
    assert(engine.ops.sync(&engine)==-EINTR && engine.quarantined);
    setup(0x0110,32,false); m.stuck=true; m.invalidate_on_pause=true;
    engine.ops.set_pause(&engine,pause_wait,&m);
    assert(engine.ops.sync(&engine)==-EIO && !engine.cpu_safe);
    setup(0x0280,32,false); m.fail_cursor_read=true;
    assert(engine.ops.cursor_show(&engine,false)==-EIO);
    nv2d_quarantine(&engine,"cursor");
    assert(engine.ops.sync(&engine)==-EIO && !engine.cpu_safe);
    setup(0x0250,32,false); m.fault_after=1; m.stop_after=m.methods+8;
    reset_pixels(&engine.fb);
    struct fb_rect src={5,5,23,17};
    int e=engine.ops.copy_rect(&engine,&src,6,6);
    assert(e==-ETIMEDOUT && m.partial && m.blits==1 && engine.quarantined && !engine.cpu_safe);
    writes=m.traced; m.stuck=false; m.hold_fifo=false;
    assert(!engine.ops.sync(&engine));
    assert(engine.ops.copy_rect(&engine,&src,6,6)==-EIO && writes==m.traced);
    setup(0x0028,16,false); m.valid=false; writes=m.traced;
    assert(engine.ops.fill_rect(&engine,&r,0)==-EIO && writes==m.traced && !engine.cpu_safe);
    setup(0x0250,32,false); m.hold_fifo=true; m.quarantine_on_pause=true;
    engine.ops.set_pause(&engine,pause_wait,&m); writes=m.traced;
    assert(engine.ops.fill_rect(&engine,&r,0)==-EIO && writes==m.traced && engine.quarantined);
    setup(0x0200,32,false); m.regs[NV2D_GRAPH_INTR/4]=1;
    assert(engine.ops.fill_rect(&engine,&r,0)==-EIO && engine.quarantined);
    assert(!engine.ops.sync(&engine) && engine.cpu_safe && engine.quarantined);
    setup(0x0171,32,false); reset_pixels(&engine.fb);
    uint8_t saved[sizeof(m.pixels)]; memcpy(saved,m.pixels,sizeof(saved)); m.drop_draw=true;
    assert(nv2d_compare(&engine)==-EIO && engine.quarantined && !engine.cpu_safe);
    assert(!memcmp(saved,m.pixels,sizeof(saved))); /* no native execution cannot pass */
    assert(!engine.ops.sync(&engine) && engine.cpu_safe);
}
static void dma_failures(uint16_t id)
{
    struct fb_rect r={0,0,4,4};
    setup(id,32,false);m.hold_get=true;m.frozen=true;
    engine.ops.set_pause(&engine,pause_wait,&m);
    assert(engine.ops.fill_rect(&engine,&r,0x123456)==-ETIMEDOUT);
    assert(engine.quarantined && !engine.cpu_safe && m.delays==NV2D_WAIT_POLLS && m.pauses);
    unsigned packets=m.packets;
    assert(engine.ops.fill_rect(&engine,&r,0)==-EIO && packets==m.packets);
    assert(engine.ops.stop(&engine)==-EIO && !engine.cpu_safe);
    m.hold_get=false;consume();
    assert(!engine.ops.sync(&engine) && engine.cpu_safe && engine.quarantined);
    assert(!engine.ops.stop(&engine) && !reg(0x3220));

    setup(id,32,false);m.malformed_get=true;
    assert(engine.ops.sync(&engine)==-EIO && !engine.cpu_safe && engine.quarantined);
    m.malformed_get=false;assert(!engine.ops.sync(&engine) && engine.cpu_safe);
    setup(id,32,false);m.regs[NV2D_GET/4]=NV2D_RING_WORDS*4;
    assert(engine.ops.sync(&engine)==-EIO && !engine.cpu_safe);
    setup(id,32,false);m.ignore_put=true;
    assert(engine.ops.fill_rect(&engine,&r,0)==-EIO && !engine.cpu_safe);
    assert(engine.ops.sync(&engine)==-EIO && !engine.cpu_safe); /* lost PUT is not idle */
    setup(id,32,false);m.dead=true;
    assert(engine.ops.sync(&engine)==-EIO && !engine.cpu_safe);
    setup(id,32,false);m.ignore_stop=true;
    assert(engine.ops.stop(&engine)==-EIO && !engine.cpu_safe);
    setup(id,32,false);m.hold_get=true;m.pause_error=-EINTR;
    engine.ops.set_pause(&engine,pause_wait,&m);
    assert(engine.ops.fill_rect(&engine,&r,0)==-EINTR && engine.quarantined && !engine.cpu_safe);
    setup(id,32,false);m.hold_get=true;m.invalidate_on_pause=true;
    engine.ops.set_pause(&engine,pause_wait,&m);
    assert(engine.ops.fill_rect(&engine,&r,0)==-EIO && !engine.cpu_safe);
    setup(id,32,false);m.regs[NV2D_FIFO_INTR/4]=1;
    assert(engine.ops.fill_rect(&engine,&r,0)==-EIO && engine.quarantined);
    assert(!engine.ops.sync(&engine) && engine.cpu_safe);
    setup(id,32,false);m.fault_after=1;
    struct fb_rect src={5,5,23,17};
    assert(engine.ops.copy_rect(&engine,&src,6,6)==-ETIMEDOUT && m.blits==1);
    assert(engine.quarantined && !engine.cpu_safe); /* no overlapping replay */
    setup(id,32,false);m.drop_draw=true;
    assert(nv2d_compare(&engine)==-EIO && engine.quarantined);
    assert(!engine.ops.sync(&engine) && engine.cpu_safe);

    /* A consumer gap is one word, even if a whole method group almost fits. */
    setup(id,32,false);m.hold_get=true;
    engine.current=engine.put=50;m.regs[NV2D_PUT/4]=50*4;m.regs[NV2D_GET/4]=55*4;
    uint32_t saved[5];
    for (unsigned i=0;i<5;i++) saved[i]=engine.ring[50+i];
    assert(engine.ops.fill_rect(&engine,&r,0)==-ETIMEDOUT && engine.quarantined);
    for (unsigned i=0;i<5;i++) assert(saved[i]==engine.ring[50+i]);
    assert(reg(NV2D_PUT)==50*4 && !engine.cpu_safe);

    /* An interrupted wrap has no public completion fence. Retain the guard. */
    setup(id,32,false);m.hold_get=true;m.frozen=true;
    engine.current=engine.put=NV2D_RING_WORDS-1;
    m.regs[NV2D_PUT/4]=engine.put*4;m.regs[NV2D_GET/4]=0;
    assert(engine.ops.fill_rect(&engine,&r,0)==-ETIMEDOUT && engine.wrap_pending);
    assert(engine.ring[NV2D_RING_WORDS-1]==0x20000000 && !engine.cpu_safe);
    m.regs[NV2D_GET/4]=reg(NV2D_PUT);
    assert(engine.ops.sync(&engine)==-EIO && !engine.cpu_safe);
    assert(engine.ops.stop(&engine)==-EIO && !engine.cpu_safe);
}
static void wraps(uint16_t id)
{
    struct fb_device f=setup(id,32,false), ref=f;ref.mapped=m.golden;
    reset_pixels(&f);
    struct fb_rect r={1,1,3,3};
    for (unsigned n=0;n<6000;n++) {
        m.traced=0;
        assert(!engine.ops.fill_rect(&engine,&r,n));
        swdib_fill(&ref,&r,n);
    }
    compare();
    assert(m.wraps>=8 && m.padding_puts==m.wraps && m.packets>20000);
    for (unsigned i=0;i<NV2D_SKIPS;i++) assert(!engine.ring[i]);
    assert(reg(NV2D_GET)==reg(NV2D_PUT) && engine.cpu_safe);
    assert(engine.scratch_offset+NV2D_SCRATCH_BYTES==engine.ring_offset);
    assert(engine.ring_offset+NV2D_RING_WORDS*4==engine.cursor_offset);
    if (engine.architecture==0x40) assert(engine.cursor_offset+0x4000==engine.vram_bytes-0x80000);

    /* Exact last-word jump and low-PUT prefetch corner, independent of load. */
    setup(id,32,false);
    engine.current=engine.put=NV2D_RING_WORDS-1;
    m.regs[NV2D_GET/4]=engine.put*4;m.regs[NV2D_PUT/4]=engine.put*4;
    assert(!engine.ops.fill_rect(&engine,&r,0x123456));
    assert(m.wraps==1 && m.padding_puts==1 && engine.ring[NV2D_RING_WORDS-1]==0x20000000);
    setup(id,32,false);
    for (unsigned i=0;i<NV2D_RING_WORDS;i++) engine.ring[i]=0;
    engine.current=NV2D_RING_WORDS-1;engine.put=NV2D_SKIPS;
    m.regs[NV2D_PUT/4]=NV2D_SKIPS*4;m.regs[NV2D_GET/4]=0;
    assert(!engine.ops.fill_rect(&engine,&r,0xabcdef));
    assert(m.wraps==1 && m.padding_puts==1 && !engine.wrap_pending && engine.cpu_safe);
    bool corner=false;
    for (unsigned i=0;i<m.traced;i++)
        if (m.trace[i].reg==NV2D_PUT && m.trace[i].value==(NV2D_SKIPS+1)*4) corner=true;
    assert(corner);
}
static void dma_refusals(void)
{
    struct fb_device f=setup(0x0040,32,false);
    assert(!engine.ops.stop(&engine));
    memset(&engine,0,sizeof(engine)); unsigned writes=m.traced;
    assert(nv2d_engine_init(&engine,&model_io,&f,0x0040,1<<20,m.tail)==-EOPNOTSUPP);
    assert(nv2d_engine_init(&engine,&model_io,&f,0x0040,8<<20,m.tail+1)==-EOPNOTSUPP);
    f.size=(8<<20)-NV2D_NV40_TAIL_BYTES+1;
    assert(nv2d_engine_init(&engine,&model_io,&f,0x0040,8<<20,m.tail)==-EOPNOTSUPP);
    assert(writes==m.traced);
    f.size=29*f.pitch;m.regs[0x3220/4]=1;
    assert(nv2d_engine_init(&engine,&model_io,&f,0x0322,8<<20,m.tail)==-EBUSY && writes==m.traced);
    m.regs[0x3220/4]=0;
    m.regs[0x1800/4]=0x032210de;
    assert(nv2d_chipset(&model_io,0x00fa)==0x0322);
    assert(nv2d_chipset(&model_io,0x00f5)==0); /* excluded GF7 alias */
    m.regs[0x1800/4]=0xde104000;
    assert(nv2d_chipset(&model_io,0x00f1)==0x0040); /* swapped ID */
    m.regs[0x1800/4]=0x029010de;assert(!nv2d_chipset(&model_io,0x00f1));
    m.regs[0x1800/4]=0xffffffff;assert(!nv2d_chipset(&model_io,0x00f1));
    m.regs[0x1800/4]=0x00401234;assert(!nv2d_chipset(&model_io,0x00f1));
    m.regs[0x10020c/4]=512u<<20;
    assert(!nv2d_vram(&model_io,0x40)); /* never guess instance placement at a capped top */
    assert(nv2d_vram(&model_io,0x30)==256u<<20);
    m.regs[0x1800/4]=0x004010de;
    m.tail_offset=(8<<20)-NV2D_NV40_TAIL_BYTES;m.scratch_offset=(8<<20)-576*1024;
    assert(!nv2d_engine_init(&engine,&model_io,&f,0x00f1,8<<20,m.tail) && engine.device==0x0040);
    assert(!engine.ops.stop(&engine));
}
static void refusal(void)
{
    struct fb_device f=setup(0x0110,32,false);
    memset(&engine,0,sizeof(engine)); unsigned writes=m.traced;
    f.bpp=24; assert(nv2d_engine_init(&engine,&model_io,&f,0x0110,8<<20,m.tail)==-EOPNOTSUPP);
    f.bpp=32; f.red_pos=0; assert(!nv2d_format(&f)); f.red_pos=16;
    f.pitch++; assert(!nv2d_format(&f)); f.pitch--;
    const uint16_t excluded[]={0x0018,0x0329,0x016a,0x0240,0x032f,0x0290,0xffff,0x01a0,0x01f0,0x00a0};
    for (unsigned i=0;i<ARRAY_SIZE(excluded);i++)
        assert(nv2d_engine_init(&engine,&model_io,&f,excluded[i],8<<20,m.tail)==-EOPNOTSUPP);
    assert(nv2d_engine_init(&engine,&model_io,&f,0x0110,128<<10,m.tail)==-EOPNOTSUPP);
    assert(writes==m.traced);
    m.regs[0x3220/4]=1;
    assert(nv2d_engine_init(&engine,&model_io,&f,0x0110,8<<20,m.tail)==-EBUSY && writes==m.traced && !engine.touched);
    m.regs[0x3220/4]=0; m.stuck=true;
    assert(nv2d_engine_init(&engine,&model_io,&f,0x0110,8<<20,m.tail)==-EBUSY && writes==m.traced);
    m.stuck=false;m.regs[0x600860/4]=0;
    assert(nv2d_engine_init(&engine,&model_io,&f,0x0110,8<<20,m.tail)==-EOPNOTSUPP && writes==m.traced);
    m.regs[0x602860/4]=0x1000; m.cursor[0x44]=3;
    assert(!nv2d_engine_init(&engine,&model_io,&f,0x0110,8<<20,m.tail) && engine.head_offset==0x2000);
    struct accel_cursor cursor={0};
    assert(!engine.ops.cursor_set(&engine,&cursor));
    assert(!engine.ops.cursor_move(&engine,7,8) && reg(0x682300)==0x00080007);
    assert(!engine.ops.stop(&engine));
    m.regs[0x100000/4]=2; assert(nv2d_vram(&model_io,4)==8<<20);
    m.regs[0x10020c/4]=16<<20; assert(nv2d_vram(&model_io,0x20)==16<<20);
}
int main(void)
{
    const uint16_t devices[]={0x0020,0x0028,0x0100,0x0110,0x0150,0x0171,0x0200,0x0250,0x0280,
        0x0301,0x0311,0x0322,0x0330,0x0341,0x0040,0x00c1,0x0140,0x0161,0x0211,0x0221};
    for (unsigned i=0;i<ARRAY_SIZE(devices);i++) {
        sequence(devices[i],16,false); sequence(devices[i],32,false); cursors(devices[i]);
    }
    sequence(0x0250,16,true); sequence(0x0322,16,true); sequence(0x0040,16,true); failures(); refusal();
    wraps(0x0322);wraps(0x0040);dma_failures(0x0322);dma_failures(0x0040);dma_refusals();
    printf("nv2d: %u full-allocation goldens; cursor, repeated wrap/low-GET, GET/PUT faults, timeout, cancellation, quarantine, refusal PASS\n",comparisons);
    return 0;
}
