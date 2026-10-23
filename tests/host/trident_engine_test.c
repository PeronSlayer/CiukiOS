/* Exact sequences, independent register interpretation and swdib goldens.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "trident_fixture.h"
static struct tb_fixture f;
static uint8_t golden[sizeof(f.pixels)];
static struct tb_engine e;
static void expect(unsigned i,unsigned r,unsigned bits,uint32_t v)
{ assert(i<f.traces && f.trace[i].reg==r && f.trace[i].bits==bits && f.trace[i].value==v); }
static void setup(enum tb_kind kind,unsigned bpp,unsigned pitch)
{
    fixture_reset(&f,kind,bpp,pitch);
    struct tb_io io=fixture_io(&f);
    tb_construct(&e,kind,&f.fb,&io);
    assert(!tb_initialize(&e));
    if (kind==TB_BLADE) {
        uint32_t stride=((pitch/(bpp/8))>>3)<<20,format=stride|((bpp>>4)<<29);
        const unsigned regs[]={0x21c0,0x21c4,0x21b8,0x21bc,0x21d0,0x21d4,0x21c8,0x21cc,0x216c};
        assert(f.traces==9);
        for (unsigned i=0;i<9;i++) expect(i,regs[i],32,i<4 ? format : i==8 ? 0 : stride);
    } else {
        assert(f.traces==4);
        unsigned code=pitch==1024 ? 4 : pitch==2048 ? 8 : pitch==4096 ? 12 : 0;
        expect(0,0x2125,8,(bpp>>4)|code);
        expect(1,0x2154,32,pitch<<18); expect(2,0x2150,32,pitch<<18); expect(3,0x2126,8,3);
    }
    f.traces=0;
}
static struct fb_device reference(void)
{ struct fb_device ref=f.fb; memcpy(golden,f.pixels,f.fb.size); ref.mapped=golden; return ref; }
static void fill_test(void)
{
    struct fb_device ref=reference();
    struct fb_rect r={3,4,11,9}; uint32_t colour=swdib_pack(&f.fb,0x123456);
    swdib_fill(&ref,&r,colour);
    assert(!e.ops.fill_rect(&e,&r,colour)); assert(e.cpu_safe && e.operations==1);
    assert(!memcmp(golden,f.pixels,f.fb.size));
    uint32_t c=f.fb.bpp==16 ? colour|(colour<<16) : colour;
    if (f.kind==TB_BLADE) {
        assert(f.traces==5); expect(0,0x2160,32,c); expect(1,0x2148,32,0xcc);
        expect(2,0x2144,32,0x20080018); expect(3,0x2108,32,0x00040003); expect(4,0x210c,32,0x000c000d);
    } else {
        assert(f.traces==7); expect(0,0x2127,32,0xf0); expect(1,0x2158,32,c);
        expect(2,0x2128,32,0x4000); expect(3,0x2140,32,0x000b0009);
        expect(4,0x2138,32,0x00030004); expect(5,0x2124,8,1); expect(6,0x2125,8,e.operation);
    }
}
static void copy_test(int sx,int sy,int dx,int dy)
{
    struct fb_rect src={sx,sy,12,10}; struct fb_device ref=reference(); f.traces=0;
    swdib_copy(&ref,&src,dx,dy);
    assert(!e.ops.copy_rect(&e,&src,dx,dy)); assert(e.cpu_safe);
    assert(!memcmp(golden,f.pixels,f.fb.size));
    if (f.kind==TB_BLADE) {
        bool rev=sy<dy || (sy==dy && sx<=dx);
        assert(f.traces==6); expect(0,0x2148,32,0xcc); expect(1,0x2144,32,0xe0080014u|(rev ? 2 : 0));
        uint32_t a=((uint32_t)sy<<16)|sx,b=((uint32_t)(sy+9)<<16)|(sx+11);
        uint32_t c=((uint32_t)dy<<16)|dx,d=((uint32_t)(dy+9)<<16)|(dx+11);
        expect(2,0x2100,32,rev ? b : a); expect(3,0x2104,32,rev ? a : b);
        expect(4,0x2108,32,rev ? d : c); expect(5,0x210c,32,rev ? c : d);
    } else {
        unsigned flags=4;
        if (sx<dx && sy==dy) { flags|=0x200; sx+=11; dx+=11; }
        if (sy<dy) { flags|=0x100; sy+=9; dy+=9; }
        assert(f.traces==6); expect(0,0x2128,32,flags); expect(1,0x2127,8,0xcc);
        expect(2,0x213c,32,((uint32_t)sx<<16)|sy); expect(3,0x2138,32,((uint32_t)dx<<16)|dy);
        expect(4,0x2140,32,0x000c000a); expect(5,0x2124,8,1);
    }
}
static unsigned pauses;
static int paused(void *ctx) { (void)ctx; pauses++; return 0; }
static int cancelled(void *ctx) { (void)ctx; return -EINTR; }
static bool lease_revoked(void *ctx) { (void)ctx; return false; }
static void failure_tests(enum tb_kind kind)
{
    setup(kind,32,1024); struct fb_rect r={0,0,3,3};
    struct fb_device ref=reference(); (void)ref;
    assert(e.ops.cursor_set(&e,NULL)==-EOPNOTSUPP);
    assert(e.ops.cursor_move(&e,-32,INT32_MAX)==-EOPNOTSUPP);
    assert(e.ops.cursor_show(&e,true)==-EOPNOTSUPP);
    assert(e.ops.cursor_show(&e,false)==-EOPNOTSUPP);
    assert(e.ops.capabilities(&e)==1 && !f.traces);
    struct fb_rect invalid={-1,0,3,3};
    assert(e.ops.fill_rect(&e,&invalid,1)==-EOPNOTSUPP);
    assert(e.ops.copy_rect(&e,&r,INT32_MAX,0)==-EOPNOTSUPP);
    invalid=(struct fb_rect){0,0,INT32_MAX,1}; assert(e.ops.fill_rect(&e,&invalid,1)==-EOPNOTSUPP);
    invalid=(struct fb_rect){0,0,0,0}; assert(!e.ops.fill_rect(&e,&invalid,1));
    assert(!f.traces && !memcmp(golden,f.pixels,f.fb.size));
    f.busy_reads=10; f.step=250; e.ops.set_pause(&e,paused,NULL);
    assert(!e.ops.sync(&e) && pauses && e.cpu_safe && !f.traces);
    f.busy=true; f.step=0; unsigned before=f.reads;
    assert(e.ops.sync(&e)==-ETIMEDOUT && f.reads-before==TB_POLL_LIMIT);
    assert(e.quarantined && !e.cpu_safe && !e.ops.capabilities(&e));
    before=f.traces; assert(e.ops.fill_rect(&e,&r,1)==-EIO && f.traces==before);
    f.busy=false; assert(!e.ops.sync(&e) && e.cpu_safe && e.quarantined);
    assert(tb_initialize(&e)==-EOPNOTSUPP);
    setup(kind,32,1024); f.busy_after=true;
    assert(e.ops.copy_rect(&e,&r,1,1)==-ETIMEDOUT);
    unsigned commands=f.commands; assert(!e.cpu_safe && e.quarantined && !e.operations);
    assert(e.ops.copy_rect(&e,&r,1,1)==-EIO && f.commands==commands);
    assert(e.ops.stop(&e)==-ETIMEDOUT && !e.cpu_safe);
    f.busy=false; assert(!e.ops.stop(&e) && e.cpu_safe && !e.ops.capabilities(&e));
    setup(kind,32,1024); f.busy=true; f.step=1000;
    e.ops.set_pause(&e,cancelled,NULL); assert(e.ops.sync(&e)==-EINTR && !e.cpu_safe);
    setup(kind,32,1024); f.busy=true; f.step=1000;
    before=f.reads; assert(e.ops.sync(&e)==-ETIMEDOUT && f.reads-before<TB_POLL_LIMIT);
    setup(kind,32,1024); f.dead=true;
    assert(e.ops.sync(&e)==-EIO && !e.cpu_safe && e.quarantined);
    setup(kind,32,1024); e.io.valid=lease_revoked; before=f.reads;
    assert(e.ops.sync(&e)==-EIO && f.reads==before && e.quarantined && !e.cpu_safe);
}
int main(void)
{
    const uint16_t ids[]={0x9880,0x8400,0x8420,0x8500,0x8520,0x8600,0x8620};
    for (unsigned i=0;i<ARRAY_SIZE(ids);i++) assert(tb_device(ids[i])==TB_BLADE);
    assert(tb_device(0x8820)==TB_XP && !tb_device(0x9750) && !tb_device(0x9910));
    for (unsigned kind=TB_BLADE;kind<=TB_XP;kind++) {
        for (unsigned bpp=16;bpp<=32;bpp+=16) {
            setup(kind,bpp,1024); fill_test();
            copy_test(0,0,1,0); copy_test(1,0,0,0);
            copy_test(0,0,0,1); copy_test(0,1,0,0);
            copy_test(0,0,1,1); copy_test(1,1,0,0);
            copy_test(0,1,1,0); copy_test(1,0,0,1);
            unsigned n=f.traces; struct fb_rect self={0,0,4,4};
            assert(!e.ops.copy_rect(&e,&self,0,0) && f.traces==n);
        }
        failure_tests(kind);
    }
    for (unsigned bpp=16;bpp<=32;bpp+=16) for (unsigned pitch=512;pitch<=8192;pitch*=2) setup(TB_XP,bpp,pitch);
    setup(TB_BLADE,32,1024);
    struct fb_device d=f.fb;
    d.bpp=24; assert(!tb_format(TB_BLADE,&d)); d=f.fb; d.red_pos=10; assert(!tb_format(TB_BLADE,&d));
    d=f.fb; d.pitch=1001; assert(!tb_format(TB_BLADE,&d));
    d=f.fb; d.pitch=768; assert(!tb_format(TB_XP,&d));
    d=f.fb; d.size=1; assert(!tb_format(TB_BLADE,&d));
    d=f.fb; d.width=UINT32_MAX; assert(!tb_format(TB_BLADE,&d));
    assert(!tb_format(TB_NONE,&d) && !tb_format(TB_BLADE,NULL));
    puts("Blade/XP exact initialization/fill/copy traces, 16/32bpp swdib goldens, all overlap directions, cursor refusal, bounded/sliced waits and quarantine PASS");
}
