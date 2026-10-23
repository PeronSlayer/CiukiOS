#include "neomagic_fixture.h"
static uint8_t golden[sizeof(fake.pixels)], before[sizeof(fake.pixels)];
static void check_pixels(void) { CHECK(!memcmp(fake.pixels,golden,fake.fb.size)); }
static void trace(unsigned i,unsigned reg,uint32_t value)
{ CHECK(i<fake.writes && fake.trace[i].reg==reg && fake.trace[i].value==value); }
static void goldens(unsigned bpp)
{
    model_setup(bpp,32,true);
    memcpy(golden,fake.pixels,fake.fb.size);
    struct fb_device ref=fake.fb; ref.mapped=golden;
    struct fb_rect r={3,4,7,5};
    uint32_t colour=swdib_pack(&ref,0x123456);
    swdib_fill(&ref,&r,colour);
    CHECK(!test_engine.ops.fill_rect(&test_engine,&r,colour)); check_pixels();
    CHECK(fake.writes==4);
    trace(0,NEO_CONTROL,0x880c0008); trace(1,NEO_FG,colour);
    trace(2,NEO_DST,4*ref.pitch+3*(bpp/8)); trace(3,NEO_EXTENT,5u<<16|7);
    const int dest[][2]={{4,4},{0,0},{4,0},{0,4},{2,2},{3,2},{1,2}};
    for (unsigned i=0;i<ARRAY_SIZE(dest);i++) {
        struct fb_rect src={2,2,16,16};
        fake.writes=0;
        swdib_copy(&ref,&src,dest[i][0],dest[i][1]);
        CHECK(!test_engine.ops.copy_rect(&test_engine,&src,dest[i][0],dest[i][1])); check_pixels();
        CHECK(fake.writes==4);
        bool reverse=dest[i][1]>2 || (dest[i][1]==2 && dest[i][0]>2);
        unsigned step=reverse ? 15 : 0, bytes=bpp/8;
        trace(0,NEO_CONTROL,0x880c0000|(reverse ? 0x13 : 0));
        trace(1,NEO_SRC,(2+step)*ref.pitch+(2+step)*bytes);
        trace(2,NEO_DST,((unsigned)dest[i][1]+step)*ref.pitch+((unsigned)dest[i][0]+step)*bytes);
        trace(3,NEO_EXTENT,16u<<16|16);
    }
    /* Randomly located snapshots exercise disjoint/vertical/horizontal aliasing. */
    uint32_t random=0x121;
    for (unsigned i=0;i<200;i++) {
        random=random*1664525+1013904223;
        struct fb_rect src={(int32_t)(random&15),(int32_t)((random>>4)&15),8,8};
        int x=(random>>8)&15, y=(random>>12)&15;
        fake.writes=0;
        swdib_copy(&ref,&src,x,y);
        CHECK(!test_engine.ops.copy_rect(&test_engine,&src,x,y)); check_pixels();
    }
    /* Sync must never submit, and success proves completion. */
    fake.writes=0; fake.busy_reads=7;
    CHECK(!test_engine.ops.sync(&test_engine)); CHECK(!fake.writes && test_engine.cpu_safe);
    struct fb_rect invalid={-1,0,2,2};
    CHECK(test_engine.ops.fill_rect(&test_engine,&invalid,0)==-EOPNOTSUPP && !fake.writes);
    CHECK(test_engine.ops.copy_rect(&test_engine,&r,INT32_MAX,0)==-EOPNOTSUPP && !fake.writes);
    struct fb_rect empty={0,0,0,0};
    CHECK(!test_engine.ops.fill_rect(&test_engine,&empty,0) && !fake.writes);
    CHECK(!test_engine.ops.pattern_rect && !test_engine.ops.offscreen);
}
static void write_goldens(unsigned bpp)
{
    model_setup(bpp,300,false);
    struct fb_device ref=fake.fb; ref.mapped=golden;
    memcpy(golden,fake.pixels,fake.fb.size);
    uint8_t rows[2*900+6];
    for (unsigned i=0;i<sizeof(rows);i++) rows[i]=(uint8_t)(i*41);
    const unsigned widths[]={1,2,15,16,255,256};
    for (unsigned n=0;n<ARRAY_SIZE(widths);n++) {
        struct fb_rect dst={1,2,(int32_t)widths[n]+2,2}, clip={2,2,(int32_t)widths[n],2};
        fake.writes=0;
        swdib_write(&ref,&dst,&clip,rows,903);
        CHECK(!test_engine.ops.write_rect(&test_engine,&dst,&clip,rows,903)); check_pixels();
        trace(0,NEO_CONTROL,0x800c0080); trace(1,NEO_SRC,0);
        trace(2,NEO_DST,2*ref.pitch+2*(bpp/8)); trace(3,NEO_EXTENT,1u<<16|widths[n]);
        CHECK(!fake.payload_need);
        unsigned bytes=widths[n]*(bpp/8), padded=(bytes+3)&~3u;
        const uint8_t *fed=(const uint8_t *)fake.payload;
        for (unsigned i=bytes;i<padded;i++) CHECK(!fed[i]);
    }
    fake.writes=0;
    struct fb_rect too_wide={0,0,257,1};
    CHECK(test_engine.ops.write_rect(&test_engine,&too_wide,&too_wide,rows,903)==-EOPNOTSUPP);
    CHECK(!fake.writes);
}
static void optional_goldens(unsigned bpp)
{
    model_setup(bpp,32,false);
    struct fb_device ref=fake.fb; ref.mapped=golden;
    memcpy(golden,fake.pixels,fake.fb.size);
    struct fb_rect region={1,2,16,3};
    unsigned bytes=bpp/8;
    for (unsigned y=2;y<5;y++) for (unsigned x=bytes;x<17*bytes;x++) golden[y*ref.pitch+x]^=255;
    CHECK(!neo_invert(&test_engine,&region)); check_pixels();
    trace(0,NEO_CONTROL,0x88060008);
    CHECK(!neo_invert(&test_engine,&region));
    const uint8_t bits[12]={0x81,0x80,0xee,0xee,0x42,0x01,0xee,0xee,0x10,0x20,0xee,0xee};
    for (unsigned y=0;y<3;y++) for (unsigned x=0;x<16;x++) {
        struct fb_rect pixel={(int32_t)x+1,(int32_t)y+2,1,1};
        swdib_fill(&ref,&pixel,(bits[y*4+x/8]&(0x80u>>(x%8))) ? 0x1234 : 0x5678);
    }
    fake.writes=0;
    CHECK(!neo_mono(&test_engine,&region,bits,4,0x1234,0x5678,false)); check_pixels();
    trace(0,NEO_FG,0x1234); trace(1,NEO_BG,0x5678); trace(2,NEO_CONTROL,0x800c00c0);
    fake.writes=0;
    CHECK(neo_mono(&test_engine,&region,bits,4,0,0,true)==-EOPNOTSUPP && !fake.writes);
    if (bpp==24) {
        unsigned reads=fake.reads;
        region.width=15;
        CHECK(neo_mono(&test_engine,&region,bits,4,0,0,false)==-EOPNOTSUPP);
        CHECK(!fake.writes && reads==fake.reads);
        /* The caller's software punt writes the expected narrow image. */
        for (unsigned y=0;y<3;y++) for (unsigned x=0;x<15;x++) {
            struct fb_rect pixel={(int32_t)x+1,(int32_t)y+2,1,1};
            uint32_t c=(bits[y*4+x/8]&(0x80u>>(x%8))) ? 0x1234 : 0x5678;
            swdib_fill(&fake.fb,&pixel,c); swdib_fill(&ref,&pixel,c);
        }
        check_pixels();
    }
}
static unsigned pauses;
static int pause_test(void *ctx)
{
    CHECK(ctx==&fake && !test_engine.cpu_safe); pauses++;
    return 0;
}
static int pause_cancel(void *ctx) { CHECK(ctx==&fake); return -EINTR; }
static void faults(void)
{
    model_setup(24,32,true);
    fake.stuck=true; fake.step=0; fake.polls=0;
    CHECK(test_engine.ops.sync(&test_engine)==-ETIMEDOUT);
    CHECK(fake.polls==NEO_POLL_LIMIT && test_engine.quarantined && !test_engine.cpu_safe);
    CHECK(!test_engine.ops.capabilities(&test_engine));
    unsigned reads=fake.reads;
    CHECK(test_engine.ops.sync(&test_engine)==-EIO && reads==fake.reads); /* no per-frame wait */
    fake.stuck=false;
    CHECK(!neo_idle(&test_engine) && test_engine.cpu_safe && test_engine.quarantined);
    CHECK(!test_engine.ops.stop(&test_engine));
    CHECK(neo_setup(&test_engine,&test_engine.io,&fake.fb,NULL,0)==-EOPNOTSUPP);
    model_setup(16,32,false); fake.stuck=true; fake.step=100;
    test_engine.ops.set_pause(&test_engine,pause_test,&fake);
    /* Engine is actually busy, even before our next command. */
    test_engine.cpu_safe=false;
    CHECK(test_engine.ops.sync(&test_engine)==-ETIMEDOUT && pauses>=1 && fake.polls<NEO_POLL_LIMIT);
    model_setup(24,32,true); fake.lost=true;
    CHECK(test_engine.ops.stop(&test_engine)==-EIO && !test_engine.cpu_safe && test_engine.quarantined);
    model_setup(24,32,true); fake.zero_map=true;
    CHECK(test_engine.ops.stop(&test_engine)==-EIO && !test_engine.cpu_safe && test_engine.quarantined);
    model_setup(24,32,true); fake.hide_failed=true;
    CHECK(test_engine.ops.stop(&test_engine)==-EIO && !test_engine.cpu_safe);
    model_setup(24,32,true); fake.stuck=true; fake.step=1000;
    test_engine.ops.set_pause(&test_engine,pause_cancel,&fake);
    CHECK(test_engine.ops.sync(&test_engine)==-EINTR && test_engine.quarantined);
    model_setup(24,32,false); fake.after_launch_stuck=true;
    struct fb_rect src={0,0,8,8};
    CHECK(test_engine.ops.copy_rect(&test_engine,&src,1,1)==-ETIMEDOUT);
    CHECK(fake.launches==1 && test_engine.submitted && !test_engine.cpu_safe);
}
static void cursors(unsigned bpp)
{
    model_setup(bpp,32,true);
    struct accel_cursor shape={0};
    shape.mask[0]=0xc0000000; shape.image[0]=0x80000001; /* unmasked image is ignored */
    shape.mask[1]=0x80000000; shape.image[1]=0x80000000;
    shape.foreground=swdib_pack(&fake.fb,0xff0000); shape.background=swdib_pack(&fake.fb,0x0000ff);
    CHECK(!test_engine.ops.cursor_set(&test_engine,&shape));
    CHECK(fake.writes==5);
    trace(0,NEO_CURSOR+NEOREG_CURSFGCOLOR,0xff);
    trace(1,NEO_CURSOR+NEOREG_CURSBGCOLOR,0xff0000);
    trace(2,NEO_CURSOR+NEOREG_CURSMEMPOS,0xc7f);
    trace(3,NEO_CURSOR+NEOREG_CURSX,0); trace(4,NEO_CURSOR+NEOREG_CURSY,0);
    CHECK(test_engine.hidden && test_engine.loaded && !test_engine.shown);
    CHECK(fake.cursor[8]==0x40 && fake.cursor[16]==0xc0);
    CHECK(fake.regs[(NEO_CURSOR+NEOREG_CURSFGCOLOR)/4]==0xff);
    CHECK(fake.regs[(NEO_CURSOR+NEOREG_CURSBGCOLOR)/4]==0xff0000);
    CHECK(fake.regs[(NEO_CURSOR+NEOREG_CURSMEMPOS)/4]==0xc7f);
    CHECK(!test_engine.ops.cursor_show(&test_engine,true) && !test_engine.hidden);
    trace(8,NEO_CURSOR+NEOREG_CURSCNTL,NEO_CURS_ENABLE);
    CHECK(!test_engine.ops.cursor_move(&test_engine,-1,0));
    CHECK(fake.cursor[8]==0x80 && fake.cursor[16]==0x80);
    CHECK(!test_engine.ops.cursor_move(&test_engine,0,-1));
    CHECK(fake.cursor[8]==0 && fake.cursor[16]==0x80);
    const int positions[][2]={{-31,-31},{-32,0},{INT32_MIN,INT32_MIN},{INT32_MAX,0},{32,0},{0,32},{0,0}};
    for (unsigned i=0;i<ARRAY_SIZE(positions);i++) {
        CHECK(!test_engine.ops.cursor_move(&test_engine,positions[i][0],positions[i][1]));
        CHECK(test_engine.hidden==(i!=0 && i!=6));
    }
    for (unsigned i=0;i<8;i++) CHECK(fake.cursor[i]==0xa5 && fake.cursor[1032+i]==0xa5);
    CHECK(!test_engine.ops.cursor_set(&test_engine,&shape) && test_engine.hidden && !test_engine.shown);
    shape.foreground=swdib_pack(&fake.fb,0x123456);
    CHECK(!test_engine.ops.cursor_set(&test_engine,&shape));
    CHECK(fake.regs[(NEO_CURSOR+NEOREG_CURSFGCOLOR)/4]==(bpp==16 ? 0x523510u : 0x563412u));
    fake.mismatch=true;
    CHECK(test_engine.ops.cursor_move(&test_engine,1,1)==-EIO);
    CHECK(test_engine.quarantined && test_engine.hidden && !test_engine.cpu_safe);
    model_setup(bpp,32,false);
    CHECK(test_engine.ops.cursor_set(&test_engine,&shape)==-EOPNOTSUPP);
    CHECK(test_engine.ops.capabilities(&test_engine)==1);
}
static void dm9_and_modes(void)
{
    model_reset(24,32); memcpy(before,fake.pixels,fake.fb.size);
    struct neo_io io=model_io();
    CHECK(!neo_setup(&test_engine,&io,&fake.fb,fake.cursor+8,0x1ff000));
    CHECK(!fake.writes && !fake.reads && !fake.hides); /* setup issues no command */
    CHECK(!neo_dm9(&test_engine) && !memcmp(before,fake.pixels,fake.fb.size));
    model_reset(24,32); fake.corrupt=true;
    CHECK(!neo_setup(&test_engine,&io,&fake.fb,NULL,0));
    CHECK(neo_dm9(&test_engine)==-EIO && test_engine.quarantined && !test_engine.active);
    model_reset(24,32); fake.step=NEO_CHECK_US;
    CHECK(!neo_setup(&test_engine,&io,&fake.fb,NULL,0));
    CHECK(neo_dm9(&test_engine)==-ETIMEDOUT && !fake.launches && test_engine.quarantined);
    model_reset(24,32); fake.fb.bpp=32;
    CHECK(neo_setup(&test_engine,&io,&fake.fb,NULL,0)==-EOPNOTSUPP && !fake.writes);
    model_reset(16,32); fake.fb.red_pos=10; fake.fb.red_size=5;
    CHECK(!neo_geometry(&fake.fb));
    CHECK(!neo_chip(1) && !neo_chip(0x83) && neo_chip(5) && neo_chip(0x25) && neo_chip(6) && neo_chip(0x16));
    CHECK(neo_vram(5)==2560*1024 && neo_vram(0x16)==6144*1024 && !neo_vram(1));
}
int main(void)
{
    goldens(16); goldens(24); write_goldens(16); write_goldens(24);
    optional_goldens(16); optional_goldens(24);
    cursors(16); cursors(24); faults(); dm9_and_modes();
    puts("NeoMagic engine: register traces, pixel goldens, DM9, cursor, refusal and quarantine passed");
    return 0;
}
