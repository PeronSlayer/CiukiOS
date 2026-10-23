/* FIFO packet, failure and golden pixel tests. SPDX-License-Identifier: GPL-2.0-only */
#include "vmwaresvga_fixture.h"
static struct vmsvga_engine engine;
static int attach(void)
{
    struct vmsvga_io io=fake_io(); fake.engine=&engine;
    return vmsvga_engine_init(&engine,&io,&fake.fb,0xe8000000,sizeof(fake.fifo));
}
static void setup(unsigned bpp,bool extended)
{ fake_setup(bpp,extended); CHECK(!attach()); CHECK(engine.cpu_safe && engine.active && !fake.packets && !fake.syncs); }
static void submission_trace(unsigned first)
{
    const unsigned regs[]={SVGA_REG_CONFIG_DONE,SVGA_REG_SYNC,SVGA_REG_CONFIG_DONE,SVGA_REG_ENABLE};
    const unsigned values[]={1,1,0,1};
    CHECK(fake.trace_count==first+4);
    for (unsigned i=0;i<4;i++) CHECK(fake.trace[first+i].reg==regs[i] && fake.trace[first+i].value==values[i]);
}
static void golden(unsigned bpp,bool extended)
{
    setup(bpp,extended);
    uint8_t pixels[sizeof(fake.pixels)];
    for (unsigned i=0;i<sizeof(pixels);i++) pixels[i]=fake.pixels[i]=(uint8_t)(i*131+31);
    struct fb_device ref=fake.fb; ref.mapped=pixels;
    struct fb_rect r={2,3,20,18};
    uint32_t colour=swdib_pack(&ref,0xa731e2);
    swdib_fill(&ref,&r,colour); unsigned first=fake.trace_count;
    CHECK(!engine.ops.fill_rect(&engine,&r,colour)); submission_trace(first);
    uint32_t fill_packet[]={2,colour,2,3,20,18};
    CHECK(fake.last_words==6 && !memcmp(fake.last,fill_packet,sizeof(fill_packet)));
    CHECK(!memcmp(pixels,fake.pixels,sizeof(pixels)) && fake.traces && !fake.configured);
    const int pairs[][4]={{0,0,4,4},{4,4,0,0},{0,0,4,0},{4,0,0,0},{0,4,0,0},{0,0,0,4}};
    for (unsigned i=0;i<ARRAY_SIZE(pairs);i++) {
        r=(struct fb_rect){pairs[i][0],pairs[i][1],24,24};
        swdib_copy(&ref,&r,pairs[i][2],pairs[i][3]);
        first=fake.trace_count;
        CHECK(!engine.ops.copy_rect(&engine,&r,pairs[i][2],pairs[i][3])); submission_trace(first);
        uint32_t copy_packet[]={3,(uint32_t)r.x,(uint32_t)r.y,(uint32_t)pairs[i][2],(uint32_t)pairs[i][3],24,24};
        CHECK(fake.last_words==7 && !memcmp(fake.last,copy_packet,sizeof(copy_packet)));
        CHECK(!memcmp(pixels,fake.pixels,sizeof(pixels)) && engine.cpu_safe && !engine.pending);
    }
    CHECK(engine.fills==1 && engine.copies==6 && !engine.ops.pattern_rect && !engine.ops.write_rect && !engine.ops.offscreen);
    unsigned writes=fake.writes,packets=fake.packets;
    CHECK(!engine.ops.sync(&engine) && fake.writes==writes && fake.packets==packets);
    r=(struct fb_rect){-1,0,1,1}; CHECK(engine.ops.fill_rect(&engine,&r,0)==-EOPNOTSUPP);
    r=(struct fb_rect){INT32_MAX,0,INT32_MAX,1}; CHECK(engine.ops.copy_rect(&engine,&r,0,0)==-EOPNOTSUPP);
    CHECK(fake.writes==writes && fake.packets==packets);
    /* Force a legitimate empty ring near the end; packet crosses its end. */
    engine.next=sizeof(fake.fifo)-8; fake.fifo[SVGA_FIFO_NEXT_CMD]=fake.fifo[SVGA_FIFO_STOP]=engine.next;
    r=(struct fb_rect){0,0,1,1}; CHECK(!engine.ops.fill_rect(&engine,&r,7));
    CHECK(engine.next==engine.min+16 && fake.fifo[SVGA_FIFO_STOP]==engine.next);
    CHECK(!engine.ops.stop(&engine) && engine.cpu_safe && !engine.active);
}
static int pause_ok(void *ctx)
{ struct svga_fake *f=ctx; f->pauses++; CHECK(!engine.cpu_safe && engine.pending); return 0; }
static int pause_cancel(void *ctx) { (void)ctx; return -EINTR; }
static void faults(void)
{
    struct fb_rect r={0,0,4,4};
    setup(32,false); fake.delayed=150;
    engine.ops.set_pause(&engine,pause_ok,&fake);
    CHECK(!engine.ops.fill_rect(&engine,&r,1) && fake.pauses==1 && engine.cpu_safe);
    for (unsigned frozen=0;frozen<2;frozen++) {
        setup(32,false); fake.wedge=true; fake.frozen=frozen;
        engine.ops.set_pause(&engine,pause_ok,&fake);
        CHECK(engine.ops.copy_rect(&engine,&r,1,1)==-ETIMEDOUT);
        CHECK(engine.quarantined && !engine.cpu_safe && !engine.active && !engine.ops.capabilities(&engine));
        CHECK(fake.busy_reads<=2001 && fake.pauses>=1 && fake.packets==0);
        unsigned packets=engine.packets;
        CHECK(engine.ops.fill_rect(&engine,&r,2)==-EIO && engine.packets==packets);
        CHECK(engine.ops.stop(&engine)==-ETIMEDOUT && !engine.cpu_safe);
        fake.wedge=false; CHECK(!engine.ops.sync(&engine) && engine.cpu_safe && engine.quarantined);
        CHECK(fake.packets==1 && engine.packets==packets); /* never replay submitted COPY */
    }
    setup(32,false); fake.wedge=true; engine.ops.set_pause(&engine,pause_cancel,0);
    CHECK(engine.ops.fill_rect(&engine,&r,9)==-EINTR && !engine.cpu_safe && engine.quarantined);
    engine.ops.set_pause(&engine,0,0); fake.wedge=false; CHECK(!engine.ops.stop(&engine));
    setup(32,false); fake.fifo[SVGA_FIFO_STOP]=3;
    CHECK(engine.ops.fill_rect(&engine,&r,0)==-EIO && !engine.cpu_safe && engine.quarantined && !fake.packets);
    CHECK(engine.ops.sync(&engine)==-EIO && !engine.cpu_safe);
    setup(32,false); fake.revoked=true;
    unsigned writes=fake.writes; CHECK(engine.ops.fill_rect(&engine,&r,0)==-EIO && !engine.cpu_safe && fake.writes==writes);
}
static bool cancelled(void *ctx) { (void)ctx; return true; }
static void cursors(bool extended)
{
    setup(32,extended); CHECK(engine.ops.capabilities(&engine)==3);
    struct accel_cursor shape={.foreground=0x123456,.background=0xabcdef};
    shape.mask[0]=0x80000001; shape.image[0]=0x80000000;
    unsigned first=fake.trace_count;
    CHECK(!engine.ops.cursor_set(&engine,&shape) && !engine.cursor_visible);
    if (!extended) {
        CHECK(fake.trace[first].reg==SVGA_REG_CURSOR_ON && !fake.trace[first].value); first++;
    }
    submission_trace(first);
    CHECK(fake.cursor_mask[0]==0x7f && fake.cursor_mask[3]==0xfe && fake.cursor_mask[4]==255);
    CHECK(fake.cursor_rgb[0]==0x123456 && fake.cursor_rgb[31]==0xabcdef && !fake.cursor_rgb[1]);
    first=fake.trace_count; CHECK(!engine.ops.cursor_move(&engine,-31,INT32_MAX));
    if (!extended) {
        CHECK(fake.trace_count==first+2 && fake.trace[first].reg==SVGA_REG_CURSOR_X &&
            fake.trace[first].value==(uint32_t)-31 && fake.trace[first+1].reg==SVGA_REG_CURSOR_Y &&
            fake.trace[first+1].value==INT32_MAX);
    } else CHECK(fake.fifo[SVGA_FIFO_CURSOR_X]==(uint32_t)-31 && fake.fifo[SVGA_FIFO_CURSOR_Y]==INT32_MAX);
    CHECK(engine.cursor_x==-31 && engine.cursor_y==INT32_MAX);
    CHECK(!engine.ops.cursor_show(&engine,true) && engine.cursor_visible);
    CHECK(!engine.ops.cursor_set(&engine,&shape) && !engine.cursor_visible); /* hide before replacement */
    CHECK(!vmsvga_engine_cursor_test(&engine,100,0,0) && !engine.cursor_visible);
    CHECK(vmsvga_engine_cursor_test(&engine,100,cancelled,0)==-EINTR && !engine.cursor_visible);
    g_ticks=100; CHECK(vmsvga_engine_cursor_test(&engine,100,0,0)==-ETIMEDOUT);
    CHECK(!engine.ops.cursor_show(&engine,true)); vmsvga_engine_quarantine(&engine,"test");
    CHECK(!engine.cursor_visible && engine.cpu_safe && !engine.ops.capabilities(&engine));
    CHECK(engine.ops.cursor_show(&engine,true)==-EIO);
    setup(32,extended); CHECK(!engine.ops.cursor_set(&engine,&shape));
    CHECK(!engine.ops.cursor_show(&engine,true)); fake.suppress_hide=true;
    CHECK(engine.ops.cursor_set(&engine,&shape)==-EIO && engine.quarantined && !engine.cpu_safe);
    CHECK(engine.ops.stop(&engine)==-EIO && !engine.cpu_safe);
    fake.suppress_hide=false; CHECK(!engine.ops.stop(&engine) && engine.cpu_safe);
    setup(16,extended); CHECK(engine.ops.capabilities(&engine)==1 && engine.ops.cursor_set(&engine,&shape)==-EOPNOTSUPP);
    fake_setup(32,extended); fake.regs[SVGA_REG_CAPABILITIES]&=~SVGA_CAP_CURSOR;
    CHECK(!attach() && engine.ops.capabilities(&engine)==1 && engine.ops.cursor_set(&engine,&shape)==-EOPNOTSUPP);
}
static void refusals(void)
{
    const unsigned regs[]={SVGA_REG_ID,SVGA_REG_ENABLE,SVGA_REG_WIDTH,SVGA_REG_HEIGHT,
        SVGA_REG_BITS_PER_PIXEL,SVGA_REG_BYTES_PER_LINE,SVGA_REG_FB_START,SVGA_REG_FB_OFFSET,
        SVGA_REG_RED_MASK,SVGA_REG_GREEN_MASK,SVGA_REG_BLUE_MASK,SVGA_REG_MEM_START,SVGA_REG_MEM_SIZE,
        SVGA_REG_PSEUDOCOLOR,SVGA_REG_FB_SIZE,SVGA_REG_CONFIG_DONE,SVGA_REG_BUSY};
    for (unsigned i=0;i<ARRAY_SIZE(regs);i++) {
        fake_setup(32,false); memset(&engine,0,sizeof(engine)); fake.regs[regs[i]]^=1;
        if (regs[i]==SVGA_REG_FB_SIZE) fake.regs[regs[i]]=0;
        CHECK(attach()==-EOPNOTSUPP && !fake.writes && !fake.fifo_writes && !fake.packets && engine.cpu_safe);
    }
    fake_setup(32,true); fake.regs[SVGA_REG_MEM_REGS]=4;
    CHECK(attach()==-EOPNOTSUPP && !fake.writes && !fake.fifo_writes);
    fake_setup(32,true); fake.regs[SVGA_REG_MEM_REGS]=UINT32_MAX;
    CHECK(attach()==-EOPNOTSUPP && !fake.writes && !fake.fifo_writes);
    /* Actual inspected VBox capabilities have no legacy fill. */
    fake_setup(32,true); fake.regs[SVGA_REG_CAPABILITIES]&=~VMSVGA_CAP_FILL;
    CHECK(attach()==-EOPNOTSUPP && !strcmp(engine.reason,"native_fill_unavailable") && !fake.fifo_writes);
    fake_setup(32,false); fake.regs[SVGA_REG_CAPABILITIES]&=~SVGA_CAP_RECT_COPY;
    CHECK(attach()==-EOPNOTSUPP && !fake.fifo_writes);
}
int main(void)
{
    for (unsigned ext=0;ext<2;ext++) { golden(16,ext); golden(24,ext); golden(32,ext); cursors(ext); }
    faults(); refusals(); puts("vmwaresvga engine: PASS (packets, pixels, cursor, refusal, quarantine)"); return 0;
}
