/* Register traces, error paths and independent swdib goldens.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <stdio.h>
#include <limits.h>
#include <ciuki/swdib.h>
#include "mach64_fixture.h"
static struct m64_engine native;
static uint8_t pixels[64*40*4],reference[sizeof(pixels)],cursor[4096];
static struct fb_device fb,ref;
static void setup(unsigned bpp,bool has_cursor)
{
    unsigned bytes=bpp/8;
    fb=(struct fb_device){.width=48,.height=40,.pitch=64*bytes,.size=40*64*bytes,
        .mapped=pixels,.phys=0xe0000000,.present=true,.bpp=bpp,
        .red_size=bpp==16 ? 5 : 8,.red_pos=bpp==16 ? 11 : 16,
        .green_size=bpp==16 ? 6 : 8,.green_pos=bpp==16 ? 5 : 8,.blue_size=bpp==16 ? 5 : 8};
    ref=fb; ref.mapped=reference;
    for (unsigned i=0;i<sizeof(pixels);i++) pixels[i]=(uint8_t)(i*71+13);
    memcpy(reference,pixels,sizeof(pixels)); memset(cursor,0x5c,sizeof(cursor));
    model_reset(&fb); struct m64_io io=model_io();
    m64_attach(&native,&fb,&io,has_cursor ? cursor : NULL,0x3ff000);
    assert(!m64_configure(&native));
    assert(native.cpu_safe && native.active && !native.quarantined);
    assert(model.reg[GEN_TEST_CNTL/4]==(GUI_ENGINE_ENABLE|0x20));
    assert(!memcmp(reference,pixels,sizeof(pixels))); /* init never draws */
    assert(model.reg[DP_CHAIN_MASK/4]==(bpp==16 ? 0x8410u : 0x8080u));
    assert(model.reg[DP_PIX_WIDTH/4]==(bpp==16 ? 0x1040404u : 0x1060606u));
    assert(model.reg[SC_LEFT_RIGHT/4]==47u<<16 && model.reg[SC_TOP_BOTTOM/4]==39u<<16);
    model.count=model.reads=model.launches=0;
}
static void equal(void) { assert(!memcmp(reference,pixels,sizeof(pixels))); }
static void drawings(void)
{
    for (unsigned bpp=16;bpp<=32;bpp+=16) {
        setup(bpp,true);
        struct fb_rect r={2,3,21,17};
        uint32_t colour=swdib_pack(&fb,0x36a5e7);
        swdib_fill(&ref,&r,colour);
        assert(!native.ops.fill_rect(&native,&r,colour)); equal();
        const unsigned fill_regs[]={DP_SRC,DP_FRGD_CLR,DP_MIX,DST_CNTL,DST_Y_X,DST_HEIGHT_WIDTH};
        assert(model.count==6 && model.launches==1 && native.cpu_safe);
        for (unsigned i=0;i<6;i++) assert(model.writes[i].reg==fill_regs[i]);
        assert(model.writes[4].value==0x20003 && model.writes[5].value==0x150011);
        /* All four direction combinations, horizontal/vertical aliases and
         * identity copies: one source snapshot per call. */
        const int positions[][4]={{0,0,1,1},{1,1,0,0},{0,1,1,0},{1,0,0,1},
            {0,0,1,0},{1,0,0,0},{0,0,0,1},{0,1,0,0},{0,0,0,0}};
        for (unsigned i=0;i<sizeof(positions)/sizeof(positions[0]);i++) {
            struct fb_rect src={positions[i][0],positions[i][1],32,24};
            int dx=positions[i][2],dy=positions[i][3];
            model.count=0;
            swdib_copy(&ref,&src,dx,dy);
            assert(!native.ops.copy_rect(&native,&src,dx,dy)); equal();
            const unsigned regs[]={DP_SRC,DP_MIX,DST_CNTL,SRC_Y_X,SRC_HEIGHT1_WIDTH1,DST_Y_X,DST_HEIGHT_WIDTH};
            assert(model.count==7);
            for (unsigned n=0;n<7;n++) assert(model.writes[n].reg==regs[n]);
            assert((model.writes[2].value&3u)==((dx<=src.x ? 1u : 0)|(dy<=src.y ? 2u : 0)));
        }
        unsigned n=model.count;
        assert(!native.ops.sync(&native) && model.count==n);
        struct fb_rect bad={-1,0,1,1},overflow={INT32_MAX,0,INT32_MAX,1};
        assert(native.ops.fill_rect(&native,&bad,0)==-EOPNOTSUPP);
        assert(native.ops.copy_rect(&native,&overflow,0,0)==-EOPNOTSUPP);
        assert(native.ops.copy_rect(&native,&r,INT32_MAX,0)==-EOPNOTSUPP);
        assert(model.count==n); equal();
    }
}
static void faults(void)
{
    struct fb_rect r={0,0,4,4};
    setup(32,true); model.reg[FIFO_STAT/4]=0xffff;
    native.ops.set_pause(&native,model_pause,&model);
    assert(native.ops.fill_rect(&native,&r,0)==-ETIMEDOUT);
    assert(model.pauses && !model.launches && native.quarantined && !native.cpu_safe);
    assert(!native.ops.capabilities(&native));
    assert(m64_idle_proof(&native)==-ETIMEDOUT && !native.cpu_safe);
    model.reg[FIFO_STAT/4]=0;
    assert(!m64_idle_proof(&native) && native.cpu_safe && native.quarantined);
    assert(native.ops.fill_rect(&native,&r,0)==-EIO); equal();

    setup(32,true); model.hang_launch=true;
    assert(native.ops.copy_rect(&native,&r,1,1)==-ETIMEDOUT);
    assert(model.launches==1 && !native.cpu_safe && native.quarantined);
    assert(native.ops.stop(&native)==-ETIMEDOUT);
    model.reg[GUI_STAT/4]=0;
    assert(!native.ops.stop(&native) && native.cpu_safe && !native.active);

    setup(32,true); model.frozen=true; model.reg[GUI_STAT/4]=1;
    assert(native.ops.sync(&native)==-ETIMEDOUT && model.reads<=131080);
    setup(32,true); model.reg[FIFO_STAT/4]=0x80000000;
    assert(native.ops.sync(&native)==-EIO && native.quarantined);
    setup(32,true); model.disconnected=true;
    assert(native.ops.sync(&native)==-EIO && !native.cpu_safe);
    setup(32,true); model.reg[GUI_STAT/4]=1; model.cancel=-EINTR;
    native.ops.set_pause(&native,model_pause,&model);
    assert(native.ops.sync(&native)==-EINTR && native.quarantined && model.pauses==1);
    setup(32,true); model.sticky_cursor=true;
    m64_quarantine(&native,"test");
    assert(!native.cpu_safe && m64_idle_proof(&native)==-EIO);
}
static void cursors(void)
{
    for (unsigned bpp=16;bpp<=32;bpp+=16) {
        setup(bpp,true);
        struct accel_cursor c={.background=0,.foreground=bpp==16 ? 0xffffu : 0xffffffu};
        c.mask[0]=0xc0000000; c.image[0]=0x40000000;
        c.mask[31]=0x1; c.image[31]=0xffffffff; /* ignored outside mask */
        assert(native.ops.cursor_show(&native,true)==-EINVAL);
        assert(native.ops.cursor_set(&native,NULL)==-EINVAL);
        assert(!native.ops.cursor_set(&native,&c));
        assert(model.writes[0].reg==GEN_TEST_CNTL && !(model.writes[0].value&HWCURSOR_ENABLE));
        assert(cursor[0]==0xa4 && cursor[31*16+7]==0x6a);
        for (unsigned y=0;y<64;y++) for (unsigned x=0;x<64;x++) {
            unsigned got=(cursor[y*16+x/4]>>((x%4)*2))&3u;
            unsigned want=2;
            if (x<32 && y<32 && (c.mask[y]&(1u<<(31-x)))) want=(c.image[y]&(1u<<(31-x))) ? 1 : 0;
            assert(got==want);
        }
        for (unsigned i=1024;i<4096;i++) assert(cursor[i]==0x5c);
        assert(model.reg[CUR_CLR0/4]==0 && model.reg[CUR_CLR1/4]==0xffffff);
        c.foreground=bpp==16 ? 0x11aa : 0x123456;
        assert(!native.ops.cursor_set(&native,&c));
        assert(model.reg[CUR_CLR1/4]==(bpp==16 ? 0x103552u : 0x123456u));
        assert(!native.ops.cursor_show(&native,true));
        assert(!native.ops.cursor_move(&native,-3,-7));
        assert(model.reg[CUR_OFFSET/4]==(0x3ff000u>>3)+14);
        assert(model.reg[CUR_HORZ_VERT_OFF/4]==0x70003 && !model.reg[CUR_HORZ_VERT_POSN/4]);
        assert(model.reg[GEN_TEST_CNTL/4]&HWCURSOR_ENABLE);
        assert(!native.ops.cursor_move(&native,INT32_MIN,INT32_MAX));
        assert(!(model.reg[GEN_TEST_CNTL/4]&HWCURSOR_ENABLE));
        assert(!native.ops.cursor_move(&native,10,20));
        assert(model.reg[CUR_HORZ_VERT_POSN/4]==0x14000a && (model.reg[GEN_TEST_CNTL/4]&HWCURSOR_ENABLE));
        assert(!native.ops.cursor_set(&native,&c)); /* replacement leaves hidden */
        assert(!(model.reg[GEN_TEST_CNTL/4]&HWCURSOR_ENABLE));
        assert(!native.ops.stop(&native) && native.cpu_safe && !native.ops.capabilities(&native));
    }
    setup(32,false); struct accel_cursor c={0}; unsigned n=model.count;
    assert(native.ops.capabilities(&native)==1);
    assert(native.ops.cursor_set(&native,&c)==-EOPNOTSUPP);
    assert(native.ops.cursor_move(&native,0,0)==-EOPNOTSUPP);
    assert(native.ops.cursor_show(&native,true)==-EOPNOTSUPP && model.count==n);
}
static void refusals(void)
{
    setup(32,true); struct m64_io io=model_io();
    m64_attach(&native,&fb,&io,cursor,0x3ff000);
    model.reg[CRTC_OFF_PITCH/4]|=1; model.count=0;
    assert(m64_configure(&native)==-EOPNOTSUPP && !model.count && native.cpu_safe);
    model.reg[CRTC_OFF_PITCH/4]&=~1u; model.reg[CRTC_GEN_CNTL/4]=0x500;
    assert(m64_configure(&native)==-EOPNOTSUPP && !model.count);
    fb.bpp=24; assert(!m64_format(&fb)); fb.bpp=32;
    fb.pitch++; assert(!m64_format(&fb)); fb.pitch--;
    fb.red_pos=0; assert(!m64_format(&fb));
}
int main(void)
{
    drawings(); faults(); cursors(); refusals();
    puts("mach64 engine: register sequences, RGB565/XRGB8888 swdib goldens, 9 copy directions/aliases, cursor packing/edges, deadlines/frozen clock, refusal, quarantine and idle proof PASS");
    return 0;
}
