/* Fake-register packet, pixel and failure goldens. SPDX-License-Identifier: GPL-2.0-only */
#include "matrox_fixture.h"
static unsigned checks;
static void goldens(unsigned bpp,unsigned capacity,bool rgb555)
{
    fake_reset(bpp);
    fake.credits=fake.capacity=capacity;
    fake.fb.width=300; fake.fb.pitch=320*(bpp/8); fake.fb.size=fake.fb.pitch*fake.fb.height;
    if (rgb555) { fake.fb.green_size=5; fake.fb.red_pos=10; }
    struct matrox_engine e=fresh_engine(); fix_context(&e);
    assert(fake.writes==10 && fake.regs[M_YDSTORG/4]==0 && fake.regs[M_SRCORG/4]==0 &&
        fake.regs[M_DSTORG/4]==0 && fake.regs[M_OPMODE/4]==4 && fake.regs[M_PLNWT/4]==UINT32_MAX);
    uint8_t golden[sizeof(fake.pixels)]; memcpy(golden,fake.pixels,sizeof(golden));
    struct fb_device sw=fake.fb; sw.mapped=golden+16;
    const struct accel_ops *ops=&e.ops;
    uint32_t colour=swdib_pack(&sw,0x123456);
    struct fb_rect r={3,4,43,27};
    clear_trace();
    assert(!ops->fill_rect(&e,&r,colour)); swdib_fill(&sw,&r,colour);
    assert(fake.writes==6 && fake.trace[0].reg==M_MACCESS && fake.trace[1].reg==M_PITCH);
    assert(fake.trace[2].value==0xc7804u && fake.trace[4].value==((46u<<16)|3u));
    assert(fake.trace[5].reg==(M_YDSTLEN|M_EXEC) && fake.trace[5].value==((4u<<16)|27u));
    assert(!memcmp(golden,fake.pixels,sizeof(golden))); checks++;
    /* Seed every pixel differently so traversal errors cannot pass as a
     * constant-colour copy, then compare all 25 overlap directions. */
    for (int dy=-2;dy<=2;dy++) for (int dx=-2;dx<=2;dx++) {
        for (unsigned y=0;y<sw.height;y++) for (unsigned x=0;x<sw.width;x++) store_pixel(x,y,(y*737+x*123)^0x151555);
        memcpy(golden,fake.pixels,sizeof(golden));
        clear_trace(); assert(!ops->copy_rect(&e,&r,r.x+dx,r.y+dy));
        swdib_copy(&sw,&r,r.x+dx,r.y+dy);
        bool back=dy>0 || (!dy && dx>0);
        assert(fake.writes==(back ? 9u : 8u));
        assert(fake.trace[2].reg==M_DWGCTL && fake.trace[2].value==(0x40c4008u|(back ? 0 : 0x2000u)));
        if (back) assert(fake.trace[3].reg==M_SGN && fake.trace[3].value==5);
        assert(!memcmp(golden,fake.pixels,sizeof(golden))); checks++;
    }
    /* Widths crossing byte/dword boundaries, minimal readable last row,
     * padding and tail bits: each mono pixel compared to swdib solid pixels. */
    for (unsigned width=1;width<=256;width++) {
        uint8_t input[102]; for (unsigned i=0;i<sizeof(input);i++) input[i]=(uint8_t)(0x81u+i*37u);
        unsigned stride=(width+7)/8+2, size=2*stride+(width+7)/8;
        struct fb_rect mono={1,1,(int)width,3};
        memcpy(golden,fake.pixels,sizeof(golden)); clear_trace();
        assert(!matrox_mono(&e,&mono,input,stride,size,colour,0));
        for (unsigned y=0;y<3;y++) for (unsigned x=0;x<width;x++) {
            struct fb_rect pixel={1+(int)x,1+(int)y,1,1};
            swdib_fill(&sw,&pixel,input[y*stride+x/8]&(128u>>(x%8)) ? colour : 0);
        }
        assert(!memcmp(golden,fake.pixels,sizeof(golden))); checks++;
    }
    clear_trace(); r.x=-1;
    assert(ops->fill_rect(&e,&r,1)==-EOPNOTSUPP && !fake.writes);
    assert(ops->copy_rect(&e,&r,0,0)==-EOPNOTSUPP && !fake.writes);
    r=(struct fb_rect){1,1,12,2}; uint8_t small[2]={0};
    assert(matrox_mono(&e,&r,small,2,2,1,0)==-EOPNOTSUPP && !fake.writes);
    r.width=257; assert(matrox_mono(&e,&r,small,2,2,1,0)==-EOPNOTSUPP && !fake.writes);
    r.width=0; assert(!ops->fill_rect(&e,&r,1) && !fake.writes);
    printf("Matrox %u bpp/%u-entry FIFO%s: fill trace, 25 directional copies and 256 mono widths vs swdib PASS\n",bpp,capacity,rgb555 ? "/RGB555" : "");
}
static void cursor_goldens(void)
{
    fake_reset(16); struct matrox_engine e=fresh_engine(); fix_context(&e);
    struct accel_cursor c={.foreground=0xf800,.background=0x07e0};
    c.mask[0]=0xc0000001u; c.image[0]=0x80000002u;
    assert(!e.ops.cursor_set(&e,&c) && fake.dac[XCURCTRL]==0);
    /* leftmost pixels are bit 63/62: byte 7 of plane 0 and byte 15
     * of plane 1. Masked-out image bits cannot create opaque pixels. */
    assert(fake.cursor[16+7]==0x80 && fake.cursor[16+15]==0xc0);
    assert(fake.cursor[16+4]==0 && fake.cursor[16+12]==1);
    assert(fake.cursor[15]==0xba && fake.cursor[16+1024]==0xba);
    for (unsigned i=16;i<1024;i++) assert(!fake.cursor[16+i]);
    assert(fake.dac[XCURCOL0]==0 && fake.dac[XCURCOL0+1]==255 && fake.dac[XCURCOL0+2]==0);
    assert(fake.dac[XCURCOL1]==255 && !fake.dac[XCURCOL1+1] && !fake.dac[XCURCOL1+2]);
    assert(fake.dac[XCURADDL]==0 && fake.dac[XCURADDH]==28);
    assert(!e.ops.cursor_move(&e,-1,-2) && fake.regs[M_CURSOR_POS/4]==((62u<<16)|63u));
    assert(!e.ops.cursor_move(&e,INT32_MIN,INT32_MAX) && !fake.regs[M_CURSOR_POS/4]);
    assert(!e.ops.cursor_show(&e,true) && fake.dac[XCURCTRL]==3);
    assert(!e.ops.cursor_set(&e,&c) && !fake.dac[XCURCTRL]);
    matrox_engine_quarantine(&e,"fixture"); assert(!fake.dac[XCURCTRL] && !e.ops.capabilities(&e));
    assert(!e.ops.cpu_ready(&e) && !matrox_engine_prove(&e) && e.ops.cpu_ready(&e));
    puts("Matrox cursor: DAC colours/base, planes, masked image, signed positions, replacement and quarantine PASS");
}
static void failures(void)
{
    fake_reset(32); struct matrox_engine e=fresh_engine(); fix_context(&e);
    e.ops.set_pause(&e,fake_pause,&fake);
    fake.busy_reads=500; assert(!e.ops.sync(&e) && fake.pauses && e.cpu_safe);
    fake.full=true; clear_trace(); struct fb_rect r={0,0,8,8};
    assert(e.ops.fill_rect(&e,&r,1)==-ETIMEDOUT && !fake.writes && e.quarantined && !e.cpu_safe);
    fake.full=false; assert(!e.ops.sync(&e) && e.cpu_safe && !e.ops.capabilities(&e));
    assert(e.ops.fill_rect(&e,&r,1)==-ENODEV);
    fake_reset(32); e=fresh_engine(); fix_context(&e);
    fake.wedge_launch=true;
    assert(e.ops.copy_rect(&e,&r,1,1)==-ETIMEDOUT && fake.launches==1 && !e.cpu_safe);
    unsigned launches=fake.launches;
    assert(matrox_engine_prove(&e)==-ETIMEDOUT && !e.cpu_safe && fake.launches==launches);
    fake.wedge=false; fake.bad_dac=true;
    assert(matrox_engine_prove(&e)==-EIO && !e.cpu_safe);
    fake.bad_dac=false; assert(!matrox_engine_prove(&e) && e.cpu_safe);
    assert(matrox_engine_init(&e,&fake.fb,&e.io,0,0)==-ENODEV);
    fake_reset(24); e=fresh_engine(); fix_context(&e); fake.step=0; fake.wedge=true;
    unsigned reads=fake.reads;
    assert(e.ops.sync(&e)==-ETIMEDOUT && fake.reads-reads<=400002 && !e.cpu_safe);
    fake_reset(32); e=fresh_engine(); fix_context(&e); fake.dead=true;
    assert(e.ops.sync(&e)==-EIO && e.quarantined && !e.cpu_safe);
    fake_reset(32); e=fresh_engine(); fix_context(&e); fake.vsync=true;
    struct accel_cursor c={0}; assert(!e.ops.cursor_set(&e,&c));
    assert(e.ops.cursor_move(&e,1,1)==-ETIMEDOUT && !e.cpu_safe && !fake.dac[XCURCTRL]);
    puts("Matrox failure paths: sliced sync, FIFO/idle/VSYNC timeouts, frozen clock, dead MMIO, cursor-disable failure, permanent quarantine PASS");
}
int main(void)
{
    for (unsigned bpp=16;bpp<=32;bpp+=8) { goldens(bpp,64,false); goldens(bpp,16,false); }
    goldens(16,64,true); goldens(16,16,true);
    cursor_goldens(); failures();
    printf("Matrox engine: %u pixel goldens PASS\n",checks); return 0;
}
