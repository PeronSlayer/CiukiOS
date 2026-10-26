/* The real packet engine, scalar reference, alias windows and fbdev presenter
 * interleave on a UP host model. The checkpoint is the scheduling boundary. */
static uint64_t background_start3d, background_phase_start3d;
static unsigned background_cpu3d, background_frames3d, background_phases3d;
static bool background_total3d;
static int background_checkpoint3d(void *ctx)
{
    (void)ctx;
    FULL_CHECK(!locks && !drawing && !accel_inflight && !count3d);
    FULL_CHECK(!m.reg[0x48c40/4] && !m.reg[0x850c/4] && !m.reg[0x48584/4]);
    /* A complete real framebuffer presentation, while qualification owns its
     * generation pin and scratch. The desktop's retained grant is untouched. */
    uint32_t pixels[96*64];
    for (unsigned i=0;i<96*64;i++) pixels[i]=0x123456;
    struct fb_surface source={pixels,96,64,96*4,sizeof(pixels)};
    struct fb_rect target={0,0,96,64};
    FULL_CHECK(!fbdev_present(&source,&target)); background_frames3d++;
    FULL_CHECK(device.mapped[0]==0x56 && device.mapped[1]==0x34 && device.mapped[2]==0x12);
    m.us+=background_cpu3d;
    if (m.us-background_start3d>=SAVAGE3D_PROBE_TOTAL_US) {
        background_total3d=true; return -ETIMEDOUT;
    }
    return m.us-background_phase_start3d>=SAVAGE3D_PROBE_PHASE_US ? -ETIMEDOUT : 0;
}
static void background_phase3d(void *ctx,const char *name,unsigned index,bool begin,int error)
{
    (void)ctx; (void)name; (void)index; (void)error;
    FULL_CHECK(!locks);
    if (begin) { background_phase_start3d=m.us; background_phases3d++; }
}
static void background3d(unsigned slow)
{
    prepare(32); m.reg[0x48578/4]=0;
    memset(ram3d,0x7a,sizeof(ram3d));
    draw3d=copy3d=writes3d=memory3d=count3d=corruption3d=0;
    dropping3d=bad_restore3d=slow_guard3d=false;
    gpu3d=(struct savage3d_device){.engine=&test_engine,.heap={0x800000,sizeof(ram3d)},.generation=42,
        .mode_width=640,.mode_height=480,.texture_bytes=480*1024};
    gpu3d.memory=(struct savage3d_memory){0,memory_read3d,memory_write3d,lock3d,unlock3d};
    gpu3d.qualified=true; test_engine.render3d_qualified=true; gpu3d.fullsize_testing=true;
    struct render3d_host host={0,allocate3d,free3d,clock3d,0};
    fullsize_resources3d(&scratch3d,&host,640,480,true);
    fullsize_resources3d(&reference3d,&host,640,480,false);
    gpu3d.fullsize_testing=false; gpu3d.consumer=&scratch3d;
    test_engine.io.write32=write_engine3d;
    m.exact_clock=m.fifo_model=true; m.fifo_drain=8;
    test_engine.io.read32=read_pressure3d; pressure_reads3d=0;
    background_cpu3d=slow==1 ? 10000 : slow==2 ? SAVAGE3D_PROBE_TOTAL_US : 0;
    read_delay3d=slow==0 ? 7 : 0;
    background_frames3d=background_phases3d=0; background_total3d=false;
    background_start3d=background_phase_start3d=m.us;
    gpu3d.probe_checkpoint=background_checkpoint3d; gpu3d.probe_phase=background_phase3d;
    FULL_CHECK(!fbdev_render3d_prepare(42));
    int e=savage3d_fullsize_probe(&gpu3d,&reference3d);
    fbdev_render3d_prepared();
    FULL_CHECK(e==-ETIMEDOUT && !gpu3d.lost && !test_engine.quarantined && test_engine.cpu_safe &&
        !test_engine.event_pending && !locks && !drawing && !accel_inflight && background_frames3d && background_phases3d);
    FULL_CHECK(!gpu3d.fullsize_qualified && background_total3d==(slow==2));
    FULL_CHECK(!slow ? draw3d!=0 : draw3d==0);
    read_delay3d=0; gpu3d.probe_checkpoint=0; gpu3d.probe_phase=0;
    FULL_CHECK(!render3d_close(&scratch3d) && !render3d_close(&reference3d) && !allocations3d);
    finish();
    printf("background model=%s elapsed_us=%llu desktop_frames=%u phases=%u restored=1 quarantine=0 PASS\n",
        slow==0 ? "slow-bus" : slow==1 ? "slow-cpu" : "total-budget",(unsigned long long)(m.us-background_start3d),
        background_frames3d,background_phases3d);
}
static void background_suite3d(void)
{ background3d(0); background3d(1); background3d(2); }
