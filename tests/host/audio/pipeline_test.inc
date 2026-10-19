/* f4-31: production transport service, syscalls and integer resampler. */
static void pipeline_step(unsigned frames, unsigned ms)
{
    mock_advance(frames*2); g_ticks+=ms; hw.tick+=ms;
    if(hw.sr & (AC97_BCIS|AC97_LVBCI)) CHECK(ac97_irq(11,"ac97"));
    audio_service(); CHECK(!audio.error);
}
static void test_pipeline_lifecycle(void)
{
    late_setup(); vm_attached_hook=vm_active_hook=vm_nonzero_hook=true;
    audio_service(); CHECK(audio.running && ac97.published==AUDIO_VM_LEAD);
    pipeline_step(256,6); CHECK(ac97.pcm[AUDIO_VM_LEAD][0].left==12000);
    vm_active_hook=vm_nonzero_hook=false;
    for(unsigned i=0;i<100 && audio.running;i++) pipeline_step(256,6);
    CHECK(!audio.running && audio.ready && !audio.error && vm_attached_hook);
    hw.completed_count=0; /* inspect the new generation's DAC-model output */
    /* Real copied, nonblocking write on the stopped transport, paused VM
     * still attached. RUN must be true before the syscall returns. */
    int fd=file_open(client,"/dev/audio",O_WRONLY|O_NONBLOCK,0); CHECK(fd>=0);
    struct audio_frame input[256];
    for(unsigned i=0;i<256;i++) input[i]=(struct audio_frame){1234,-1234};
    CHECK(!ua_write(client->memory,USER,input,sizeof(input)));
    uint64_t wrote_at=g_ticks;
    CHECK(call(CIUKI_SYS_WRITE,fd,USER,sizeof(input),0,0)==sizeof(input));
    CHECK(audio.running && ac97.running && g_ticks-wrote_at<5);
    struct audio_stream *s=file_fd(client,fd)->audio;
    for(unsigned i=0;i<7;i++) CHECK(audio_stream_enqueue(s,input,sizeof(input))==sizeof(input));
    pipeline_step(256,6); CHECK(ac97.pcm[AUDIO_VM_LEAD][0].left==1234);
    /* Round cumulative 48 kHz frame time up to the next millisecond.
     * Rounding every period to 6 ms would invent 6 ms of delay at lead 8. */
    while(hw.completed_count<AUDIO_VM_LEAD+1) {
        /* Keep the source running while the first queued period reaches
         * the DAC; the longer transport lead outlasts one source ring. */
        CHECK(audio_stream_enqueue(s,input,sizeof(input))==sizeof(input));
        unsigned elapsed=((hw.completed_count+1)*AUDIO_QUANTUM*1000+AUDIO_RATE-1)/AUDIO_RATE;
        pipeline_step(AUDIO_QUANTUM,elapsed-(g_ticks-wrote_at));
    }
    CHECK(hw.completed[AUDIO_VM_LEAD]==1234 && g_ticks-wrote_at<=50);
    printf("audio latency stopped VM -> native: first DAC-model period completed in %llu ms, lead=%u PASS\n",
        (unsigned long long)(g_ticks-wrote_at),AUDIO_VM_LEAD);
    /* Resume VM while more native data plays: actual mixer sums both. */
    CHECK(audio_stream_enqueue(s,input,sizeof(input))==sizeof(input));
    vm_active_hook=vm_nonzero_hook=true;
    pipeline_step(256,6);
    CHECK(ac97.pcm[(ac97.published-1)&31][0].left==13234);
    CHECK(ac97.pcm[(ac97.published-1)&31][0].right==10766);
    /* Exhaustion before worker service: write-side DMA observation must
     * recover even though the software running flag is still true. */
    mock_advance(10*512); g_ticks+=40; hw.tick+=40;
    CHECK(audio.running && (hw.sr&AC97_DCH));
    unsigned starts=hw.starts;
    CHECK(call(CIUKI_SYS_WRITE,fd,USER,sizeof(input),0,0)==sizeof(input));
    CHECK(audio.running && !(hw.sr&AC97_DCH) && hw.starts==starts+1 && audio.underruns==1);
    CHECK(!file_close(client,fd));
    late_cleanup();
    /* Native-only default lead, with conservative 6 ms host time steps. */
    late_setup();
    fd=file_open(client,"/dev/audio",O_WRONLY|O_NONBLOCK,0); CHECK(fd>=0);
    wrote_at=g_ticks;
    CHECK(call(CIUKI_SYS_WRITE,fd,USER,sizeof(input),0,0)==sizeof(input));
    CHECK(audio.running && ac97.published==AUDIO_LEAD);
    s=file_fd(client,fd)->audio;
    for(unsigned i=0;i<7;i++) CHECK(audio_stream_enqueue(s,input,sizeof(input))==sizeof(input));
    while(hw.completed_count<AUDIO_LEAD+1) pipeline_step(256,6);
    CHECK(hw.completed[AUDIO_LEAD]==1234 && g_ticks-wrote_at<=60);
    printf("audio latency native-only: first DAC-model period completed in %llu ms, lead=%u PASS\n",
        (unsigned long long)(g_ticks-wrote_at),AUDIO_LEAD);
    CHECK(!file_close(client,fd)); late_cleanup();
}
static void test_pipeline_gap(void)
{
    CHECK(audio_gap_lead(AUDIO_GAP_BUDGET_US,48000)==28);
    CHECK(audio_gap_lead(AUDIO_GAP_BUDGET_US,44100)==26);
    late_setup(); late_start(); vm_active_hook=vm_nonzero_hook=true;
    /* Reproduce a full-ring exhaustion after the measured T23 gap. The
     * worker grows its lead, proves stop, and restarts in the same turn. */
    mock_advance(113*96); g_ticks+=113; hw.tick+=113;
    audio_service();
    CHECK(audio.running && audio.ready && !audio.error && audio.underruns==1);
    CHECK(audio.max_service_gap_us==113000 && audio.lead_periods==28);
    CHECK(ac97.published==28 && hw.starts==2 && hw.resets==1);
    unsigned starts=hw.starts;
    /* A repeated 113 ms gap fits the adapted lead with margin. */
    pipeline_step(113*48,113);
    CHECK(hw.starts==starts && audio.underruns==1 && audio.running);
    printf("audio T23 gap replay: 113000 us -> lead=%u (%u us), repeat without halt PASS\n",
        audio.lead_periods,audio.lead_periods*256000u/48);
    late_cleanup();
}
static void test_cubic_cost(void)
{
    late_setup(); struct audio_stream *s=late_stream();
    s->rate=44100; s->draining=true; s->drain_goal=UINT64_MAX;
    struct audio_frame whole[256],split[256];
    audio_mix(whole,256); audio_stream_reset(s); s->rate=44100;
    struct audio_frame input[2048];
    for(unsigned i=0;i<2048;i++) input[i]=(struct audio_frame){1000+i/256,-1000};
    CHECK(audio_stream_enqueue(s,input,sizeof(input))==sizeof(input));
    audio_mix(split,17); audio_mix(split+17,239);
    CHECK(!memcmp(whole,split,sizeof(whole)));
    CHECK(cubic(-10000,0,10000,0,24000)==6250); /* curvature, unlike linear */
    CHECK(cubic(12000,12000,12000,12000,12345)==12000);
    uint32_t seed=0x4f31;
    for(unsigned n=0;n<10000;n++) {
        int16_t p[4]; for(unsigned i=0;i<4;i++) { seed=1664525u*seed+1013904223u; p[i]=(int16_t)(seed>>16); }
        unsigned phase=seed%48000, t=phase*1024/48000;
        int64_t v=(-(int64_t)p[0]+3*(int64_t)p[1]-3*(int64_t)p[2]+p[3])*t/1024;
        v=(v+2*(int64_t)p[0]-5*(int64_t)p[1]+4*(int64_t)p[2]-p[3])*t/1024;
        v=(v-p[0]+p[2])*t/1024;
        CHECK(cubic(p[0],p[1],p[2],p[3],phase)==p[1]+v/2);
    }
    for(unsigned linear=0;linear<2;linear++) {
        audio.linear_resampling=linear;
        uint64_t begin=audio_cpu_ns();
        for(unsigned n=0;n<2000;n++) { s->queued=AUDIO_RING; audio_mix(whole,256); }
        uint64_t cost=(audio_cpu_ns()-begin)/2000;
        CHECK(cost<106666); /* regression only: host is not a Pentium III */
        printf("audio resampler %s: ns_per_period=%llu CPU_budget_ns=106666 PASS\n",
            linear ? "linear" : "cubic",(unsigned long long)cost);
    }
    audio_stream_close(s); late_cleanup();
}
static void test_pipeline_backends(void)
{
    const enum audio_backend_kind kinds[]={AUDIO_ES1370,AUDIO_ES1371,AUDIO_SB16,AUDIO_VIA686,AUDIO_VIA8233};
    unsigned baseline=pages_used;
    for(unsigned i=0;i<ARRAY_SIZE(kinds);i++) {
        memset(&audio,0,sizeof(audio)); second_setup(kinds[i],0x30);
        CHECK(!backend_dma_init(&audio_dma)); audio.ready=true;
        vm_attached_hook=vm_active_hook=true; audio_service();
        CHECK(audio.running && audio_dma.state.published==AUDIO_VM_LEAD);
        /* Cyclic IRQ inhibits immediately; worker recovers without killing
         * native fd state, and without any PIC mutation. */
        unsigned pic=second_pic_masks+second_pic_unmasks;
        second.frame=(AUDIO_VM_LEAD+1)*AUDIO_QUANTUM;
        second.tick+=(AUDIO_VM_LEAD+1)*6; g_ticks+=(AUDIO_VM_LEAD+1)*6;
        if(kinds[i]==AUDIO_SB16) second.mixer[0x82]=2;
        else second.regs[(kinds[i]>=AUDIO_VIA686 ? audio_dma.channel : 4)/4]|=2;
        CHECK(backend_irq(audio_dma.state.irq,audio_dma.name) && audio_dma.inhibited && !audio.error);
        audio_service(); CHECK(audio.running && !audio.error && audio.underruns==1);
        CHECK(!audio_dma.inhibited && second_pic_masks+second_pic_unmasks==pic);
        audio_backend_shutdown(); CHECK(pages_used==baseline && !audio.commitment_pages);
        vm_active_hook=vm_attached_hook=false; idle_since=non_silent_end=0;
    }
    dma_selected=false; memset(&audio,0,sizeof(audio)); memset(&audio_dma,0,sizeof(audio_dma));
    puts("audio lifecycle recovery: ES1370/1371, physical SB16, VIA686/8233 no PIC writes PASS");
}
static void test_pipeline_budget(void)
{
    late_setup(); late_start(); vm_active_hook=vm_nonzero_hook=true;
    mock_advance(6*512); g_ticks+=32; hw.tick+=32;
    uint64_t published=ac97.published;
    modeled_mix_us=700;
    audio_service();
    CHECK(!audio.error && audio.running && ac97.published==published+2 && audio.pending);
    CHECK(audio.max_service_us==1400 && !audio.deadlines);
    audio_service(); audio_service();
    CHECK(ac97.published-ac97.retired==AUDIO_LEAD && audio.max_service_us==1400);
    modeled_mix_us=0; late_cleanup();
    /* Low-power policy measures a period before selecting linear. */
    late_setup(); struct audio_stream *s=late_stream(); s->rate=44100; late_start();
    modeled_mix_us=120; mock_advance(512); g_ticks+=6; hw.tick+=6; audio_service();
    CHECK(audio.linear_resampling && strstr(audio_log,"resample-linear"));
    modeled_mix_us=0; audio_stream_close(s); late_cleanup();
    puts("audio service budget: 700 us periods yield after 1.4 ms; remaining work wakes; slow conversion selects linear PASS");
}
static void test_pipeline(void)
{
    test_pipeline_lifecycle(); test_pipeline_gap(); test_cubic_cost(); test_pipeline_backends(); test_pipeline_budget();
}
