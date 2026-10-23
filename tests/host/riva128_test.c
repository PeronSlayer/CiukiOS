/* Production NV3 engine, fake registers and independent software goldens.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "riva128_fixture.h"
static struct nv3_engine s;
static void init_mode(unsigned format,bool padded)
{
    fake_reset(format); memset(&s,0,sizeof(s));
    if (padded) {
        fake.fb.width=fake.fb.height=29;
        fake.fb.size=fake.fb.height*fake.fb.pitch;
    }
    struct nv3_io io=fake_io();
    assert(nv3_detect_vram(&io)==4u<<20);
    assert(!nv3_engine_init(&s,&io,&fake.fb,4u<<20));
    assert(s.active && s.cpu_safe && s.ops.capabilities(&s)==3);
    assert(fake.seq[6]==0x99 && fake.seq_index==4 && fake.cr_index==0x12);
}
static void init(unsigned format) { init_mode(format,false); }
static void commands(unsigned format,bool padded)
{
    init_mode(format,padded);
    assert(fake.regs[(NV3_INSTANCE+0x501*4)/4]==(4u<<20)-1);
    assert(fake.regs[(NV3_INSTANCE+(0x502+1023)*4)/4]==0x3ff003);
    assert(!fake.regs[(NV3_INSTANCE+(0x502+1024)*4)/4]);
    assert(fake.regs[(NV3_INSTANCE+0xd04*4)/4]==(format==32 ? 0x10110201u : 0x10110200u));
    last_write(0x200,0x11101100u); /* enable units, preserving all other bits */
    assert(!writes_to(NV3_PFB+0x200) && !writes_to(0x680508));
    last_write(NV3_PFIFO+0x140,0); last_write(NV3_PGRAPH+0x140,0);
    last_write(NV3_PFIFO+0x1220,0);
    for (unsigned i=0;i<4;i++) last_write(NV3_PGRAPH+0x650+i*4,fake.fb.pitch);
    last_write(NV3_FIFO,0x80000000); last_write(NV3_BLT,0x80000011);
    last_write(NV3_BITMAP,0x80000012);
    last_write(NV3_PATTERN+0x308,0); last_write(NV3_CLIP+0x300,0);
    last_write(NV3_CLIP+0x304,fake.fb.height<<16|fake.fb.width);
    struct fb_device g=golden();
    struct fb_rect r={3,5,17,9};
    uint32_t c=swdib_pack(&g,0x12ab56);
    unsigned first=fake.writes;
    swdib_fill(&g,&r,c); assert(!s.ops.fill_rect(&s,&r,c)); same_pixels();
    const uint32_t fill_sequence[]={NV3_PATTERN+0x310,NV3_PATTERN+0x314,NV3_PATTERN+0x318,
        NV3_PATTERN+0x31c,NV3_ROP+0x300,NV3_BITMAP+0x3fc,NV3_BITMAP+0x400,NV3_BITMAP+0x404};
    assert(fake.writes-first==ARRAY_SIZE(fill_sequence));
    for (unsigned i=0;i<ARRAY_SIZE(fill_sequence);i++) assert(fake.trace[first+i].reg==fill_sequence[i]);
    last_write(NV3_ROP+0x300,0xcc); last_write(NV3_BITMAP+0x3fc,c);
    last_write(NV3_BITMAP+0x400,3u<<16|5); last_write(NV3_BITMAP+0x404,17u<<16|9);
    const int direction[][4]={{2,3,6,7},{6,7,2,3},{2,7,6,3},{6,3,2,7},{3,3,7,3},{7,3,3,3}};
    for (unsigned i=0;i<6;i++) {
        struct fb_rect source={direction[i][0],direction[i][1],20,19};
        int x=direction[i][2],y=direction[i][3];
        first=fake.writes;
        swdib_copy(&g,&source,x,y); assert(!s.ops.copy_rect(&s,&source,x,y)); same_pixels();
        assert(fake.writes-first==8 && fake.trace[first+5].reg==NV3_BLT+0x300 &&
            fake.trace[first+6].reg==NV3_BLT+0x304 && fake.trace[first+7].reg==NV3_BLT+0x308);
        last_write(NV3_BLT+0x300,(uint32_t)source.y<<16|(uint32_t)source.x);
        last_write(NV3_BLT+0x304,(uint32_t)y<<16|(uint32_t)x);
        last_write(NV3_BLT+0x308,19u<<16|20);
    }
    const uint8_t pat[8]={0x81,0x02,0x34,0x18,0xf0,0x55,0x42,0x99};
    uint32_t fg=swdib_pack(&g,0xfedcba),bg=swdib_pack(&g,0x136579);
    swdib_pattern(&g,&r,pat,fg,bg); assert(!s.ops.pattern_rect(&s,&r,pat,fg,bg)); same_pixels();
    last_write(NV3_ROP+0x300,0xf0); last_write(NV3_PATTERN+0x318,0x182c4081);
    swdib_fill(&g,&r,c); assert(!s.ops.fill_rect(&s,&r,c)); same_pixels();
    last_write(NV3_PATTERN+0x318,UINT32_MAX); last_write(NV3_ROP+0x300,0xcc);
    unsigned writes=fake.writes;
    struct fb_rect invalid={-1,0,1,1},huge={INT32_MAX,0,INT32_MAX,3};
    struct fb_rect empty={(int32_t)fake.fb.width,(int32_t)fake.fb.height,0,0};
    assert(s.ops.fill_rect(&s,&invalid,0)==-EOPNOTSUPP);
    assert(s.ops.copy_rect(&s,&r,INT32_MAX,0)==-EOPNOTSUPP);
    assert(s.ops.fill_rect(&s,&huge,0)==-EOPNOTSUPP);
    assert(!s.ops.fill_rect(&s,&empty,0) && fake.writes==writes);
    assert(!s.ops.sync(&s) && s.cpu_safe);
    assert(!s.ops.write_rect && !s.ops.offscreen);
}
static void cursors(unsigned format)
{
    init(format);
    struct accel_cursor c={.foreground=swdib_pack(&fake.fb,0xf83018),.background=swdib_pack(&fake.fb,0x2878d8)};
    c.mask[0]=0xc0000001; c.image[0]=0x80000001;
    assert(!s.ops.cursor_set(&s,&c) && s.cursor_hidden && s.cursor_loaded);
    uint32_t first=fake.regs[NV3_CURSOR/4];
    unsigned fg=0xfcc3u,bg=0x95fbu;
    assert((first&0xffff)==fg && (first>>16)==bg);
    assert(fake.regs[(NV3_CURSOR+60)/4]==fg<<16);
    assert(!fake.regs[(NV3_CURSOR+4)/4] && !fake.regs[(NV3_CURSOR+64)/4]);
    assert(!s.ops.cursor_move(&s,-31,-7)); last_write(NV3_CURSOR_POSITION,0xfff9ffe1u);
    unsigned before=fake.writes;
    assert(s.ops.cursor_move(&s,32768,0)==-EOPNOTSUPP && fake.writes==before);
    assert(!s.ops.cursor_show(&s,true) && (fake.cr[0x31]&1));
    assert(!s.ops.cursor_set(&s,&c) && !(fake.cr[0x31]&1));
    assert(!nv3_engine_cursor_test(&s,fake.time+20000,0,0) && !(fake.cr[0x31]&1));
    assert(nv3_engine_cursor_test(&s,fake.time,cancelled,0)==-ETIMEDOUT && !(fake.cr[0x31]&1));
    assert(nv3_engine_cursor_test(&s,fake.time+20000,cancelled,0)==-EINTR && !(fake.cr[0x31]&1));
    assert(!s.ops.stop(&s) && s.cpu_safe && !s.active && s.ops.capabilities(&s)==0);
    last_write(NV3_PFIFO+0x1200,0); last_write(NV3_PFIFO+0x1240,0);
}
static int pause_wait(void *ctx)
{ struct fake_nv3 *f=ctx; assert(!s.cpu_safe); f->pauses++; return 0; }
static int refuse_pause(void *ctx) { (void)ctx; return -EOPNOTSUPP; }
static void failures(void)
{
    init(32); fake.busy=240; s.ops.set_pause(&s,pause_wait,&fake); s.cpu_safe=false;
    assert(!s.ops.sync(&s) && fake.pauses && s.cpu_safe);
    init(32); fake.frozen=true; fake.starve=NV3_BITMAP;
    struct fb_rect r={1,2,10,11}; unsigned launches=fake.launches;
    assert(s.ops.fill_rect(&s,&r,0x123456)==-ETIMEDOUT);
    assert(fake.delays<=NV3_POLL_CAP && s.quarantined && !s.cpu_safe && fake.launches==launches);
    assert(!s.ops.sync(&s) && s.cpu_safe && !s.ops.capabilities(&s));
    launches=fake.launches; assert(s.ops.copy_rect(&s,&r,3,4)==-EIO && fake.launches==launches);
    init(32); fake.hang_launch=true;
    assert(s.ops.copy_rect(&s,&r,3,4)==-ETIMEDOUT && s.quarantined && !s.cpu_safe);
    assert(s.ops.stop(&s)==-ETIMEDOUT && !s.cpu_safe);
    fake.busy=0; fake.hang_launch=false;
    assert(!s.ops.sync(&s) && s.cpu_safe && s.quarantined);
    init(32); fake.hang_launch=true; s.ops.set_pause(&s,refuse_pause,0);
    assert(s.ops.copy_rect(&s,&r,3,4)==-EIO && fake.launches && s.quarantined && !s.cpu_safe);
    init(32); fake.dead_register=NV3_PGRAPH+0x6b0;
    assert(s.ops.sync(&s)==-EIO && !s.cpu_safe && s.quarantined);
    init(32); fake.corrupt_cursor=true; struct accel_cursor c={0};
    assert(s.ops.cursor_set(&s,&c)==-EIO && s.quarantined && s.cursor_hidden);
    init(32); assert(!s.ops.cursor_set(&s,&c)); assert(!s.ops.cursor_show(&s,true));
    fake.ignore_cursor=true; nv3_engine_quarantine(&s,"test");
    assert(!s.cursor_hidden && !s.cpu_safe && s.ops.sync(&s)==-EIO);
    init(32); struct nv3_io io=fake_io();
    assert(nv3_engine_init(&s,&io,&fake.fb,4u<<20)==-EIO);
    nv3_engine_quarantine(&s,"test"); assert(nv3_engine_init(&s,&io,&fake.fb,4u<<20)==-EIO);
}
static void refusals(void)
{
    fake_reset(32); struct nv3_io io=fake_io(); struct nv3_engine unstarted={0};
    struct fb_device wrong=fake.fb; wrong.bpp=24;
    assert(nv3_engine_init(&unstarted,&io,&wrong,4u<<20)==-EOPNOTSUPP && !fake.writes);
    wrong=fake.fb; wrong.pitch++; assert(!nv3_mode_supported(&wrong));
    wrong=fake.fb; wrong.size=1; assert(!nv3_mode_supported(&wrong));
    wrong=fake.fb; wrong.red_pos=0; assert(!nv3_mode_supported(&wrong));
    fake.regs[(NV3_PFB+0x200)/4]^=1;
    assert(nv3_engine_init(&unstarted,&io,&fake.fb,4u<<20)==-EOPNOTSUPP && !unstarted.touched);
    fake_reset(32); fake.cr[0x0c]=1;
    assert(nv3_engine_init(&unstarted,&io,&fake.fb,4u<<20)==-EOPNOTSUPP && !unstarted.touched);
    fake_reset(32); fake.regs[(NV3_PFIFO+0x1220)/4]=1;
    assert(nv3_engine_init(&unstarted,&io,&fake.fb,4u<<20)==-EOPNOTSUPP && !unstarted.touched);
    fake_reset(32); fake.pan=2;
    assert(nv3_engine_init(&unstarted,&io,&fake.fb,4u<<20)==-EOPNOTSUPP && !unstarted.touched);
    fake_reset(32); fake.regs[NV3_PFB/4]=0x21; fake.regs[0]=0x22;
    assert(nv3_detect_vram(&io)==2u<<20);
    fake.regs[NV3_PFB/4]=0x22; assert(nv3_detect_vram(&io)==4u<<20);
    fake.regs[0]=0; assert(nv3_detect_vram(&io)==8u<<20);
}
int main(void)
{
    for (unsigned f=0;f<3;f++) {
        unsigned format=f==0 ? 32 : f==1 ? 16 : 15;
        commands(format,false); commands(format,true); cursors(format);
    }
    failures(); refusals(); free(fake.regs);
    puts("NV3: F/C/Y/P and cursor traces, 16/32-bit goldens, refusal, timeout, frozen clock, slices, quarantine PASS");
    return 0;
}
