/* Extend, without editing, the frozen 2D model with a small 3D FIFO parser.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "savage2d_fixture.inc"
static uint32_t stream[32];
static unsigned stream_words;
static bool drop_event;
static void write3d(void *ctx,uint32_t offset,uint32_t value)
{
    struct model *p=ctx;
    if (offset<SAVAGE_BCI || offset>=SAVAGE_BCI+32*4) { model_write(ctx,offset,value); return; }
    CHECK(offset==SAVAGE_BCI+stream_words*4);
    CHECK(stream_words<32);
    stream[stream_words++]=value;
    if ((value&0xffff0000u)==0x98000000u && stream_words==24) {
        CHECK(stream[0]==0x800300c8 && stream[22]==0xc0030000 && (value&65535));
        if (!drop_event) p->reg[SAVAGE_EVENT_STATUS/4]=value&65535;
    }
}
static void transport(unsigned variant)
{
    prepare(32); struct savage2d *s=&test_engine;
    s->io.write32=write3d; stream_words=0; drop_event=false;
    uint32_t words[22]={0x800300c8};
    CHECK(savage2d_render3d_packet(s,words,22)==-EOPNOTSUPP && !stream_words);
    s->render3d_qualified=true;
    CHECK(savage2d_render3d_packet(s,words,31)==-EINVAL && !stream_words);
    s->chip=SAVAGE_4;
    CHECK(savage2d_render3d_packet(s,words,22)==-EOPNOTSUPP && !stream_words);
    s->chip=SAVAGE_SUPER;
    if (variant==1) m.reg[SAVAGE_EVENT_STATUS/4]=65535;
    if (variant==2) { m.stuck=true; m.frozen=true; }
    if (variant==3) m.dead=true;
    if (variant==4) { drop_event=true; m.reg[SAVAGE_EVENT_STATUS/4]=0; m.frozen=true; }
    if (variant==5) { drop_event=true; m.event_dead=true; }
    if (variant==6) { m.fifo_model=true; m.fifo_used=1; m.fifo_drain=1; }
    int e=savage2d_render3d_packet(s,words,22);
    bool good=variant==0 || variant==1 || variant==6;
    CHECK((!e)==good && stream_words==(variant==5 ? 0u : 24u));
    CHECK(s->quarantined==!good && s->cpu_safe==(good || variant==5));
    if (good) {
        CHECK(!s->event_pending && s->event_tag==(variant==1 ? 1 : 2));
        s->io.write32=model_write;
        struct fb_rect r={0,0,1,1}; CHECK(!s->ops.fill_rect(s,&r,0xabcdef));
    } else {
        unsigned before=stream_words;
        CHECK(savage2d_render3d_packet(s,words,22)==-EIO && stream_words==before);
    }
    finish();
}
static unsigned batch_words,batch_variant,pressure_reads;
static uint32_t batch_read(void *ctx,uint32_t reg)
{
    if (reg==SAVAGE_STATUS && batch_variant>=4) {
        m.fifo_drain=(++pressure_reads%8)==0 ? 1 : 0;
        return model_read(ctx,reg);
    }
    if (reg==SAVAGE_STATUS && m.fifo_used==32) {
        if (batch_variant==2) return UINT32_MAX;
        m.fifo_drain=batch_variant==1 || (++pressure_reads&1) ? 0 : 8;
    }
    return model_read(ctx,reg);
}
static void batch_write(void *ctx,uint32_t reg,uint32_t value)
{
    (void)ctx;
    CHECK(reg>=SAVAGE_BCI && reg<SAVAGE_BCI+128 && !(reg&3));
    m.fifo_used++; if (m.fifo_used>m.fifo_peak) m.fifo_peak=m.fifo_used;
    CHECK(m.fifo_used<=32); batch_words++;
    if (batch_words==7169) CHECK(value==0xc0030000u);
    if (batch_words==7170) {
        CHECK((value&0xffff0000u)==0x98000000u && (value&65535));
        if (batch_variant!=3) m.reg[SAVAGE_EVENT_STATUS/4]=value&65535;
    }
}
static void batch_transport(unsigned variant)
{
    prepare(32); batch_words=pressure_reads=0; batch_variant=variant;
    m.exact_clock=m.fifo_model=true; m.fifo_used=m.fifo_peak=0; m.fifo_drain=8;
    m.frozen=variant==1 || variant==3 || variant==4;
    test_engine.io.read32=batch_read; test_engine.io.write32=batch_write;
    test_engine.render3d_qualified=true;
    static uint32_t words[7168]; memset(words,0,sizeof(words));
    for (unsigned at=0;at<7168;at+=28) words[at]=0x80030008u;
    CHECK(savage2d_render3d_batch(&test_engine,words,7167)==-EINVAL && !batch_words);
    CHECK(savage2d_render3d_batch(&test_engine,words,7169)==-EINVAL && !batch_words);
    static uint32_t too_many[257*22];
    for (unsigned at=0;at<ARRAY_SIZE(too_many);at+=22) too_many[at]=0x800300c8u;
    CHECK(savage2d_render3d_batch(&test_engine,too_many,ARRAY_SIZE(too_many))==-EINVAL && !batch_words);
    uint64_t started=m.us;
    int e=savage2d_render3d_batch(&test_engine,words,7168);
    CHECK((e==0)==(variant==0) && m.fifo_peak<=32);
    if (!variant) CHECK(batch_words==7170 && test_engine.cpu_safe && !test_engine.event_pending && test_engine.fifo_waits>0);
    else {
        CHECK(test_engine.quarantined && !test_engine.active && test_engine.fifo_waits<5000);
        if (variant>=4) {
            CHECK(batch_words>32 && batch_words<7168 && pressure_reads<=SLICED_POLLS);
            CHECK(!strcmp(test_engine.reason,variant==4 ? "render3d_poll_limit" : "render3d_deadline"));
            if (variant==5) CHECK(m.us-started<=WAIT_US+POLL_US);
        }
        unsigned before=batch_words;
        CHECK(savage2d_render3d_batch(&test_engine,words,7168)==-EIO && batch_words==before);
    }
    finish();
}
int main(void)
{
    for (unsigned i=0;i<7;i++) transport(i);
    for (unsigned i=0;i<6;i++) batch_transport(i);
    printf("shared 2D/3D FIFO, both-engine wait/event, 16-bit wrap, frozen clock, dead MMIO, quarantine and 2D return: %s\n",failures ? "FAIL" : "PASS");
    fflush(stdout);
    return failures ? 1 : 0;
}
