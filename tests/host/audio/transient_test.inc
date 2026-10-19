/* F4-32: production service and independent publication with uncertain
 * progress. The zero-at-IRQ mode is deliberately adversarial: upstream
 * QEMU fetches the next descriptor before raising completion IRQ. */
static void test_transient_publish(void)
{
    late_setup(); vm_attached_hook=vm_active_hook=vm_nonzero_hook=true;
    audio_service();
    mock_advance(512); g_ticks+=6; hw.tick+=6;
    CHECK(!ac97_progress(&ac97) && ac97.retired==1);
    uint64_t fetched=ac97.fetched, output=audio.output;
    hw.zero_until=hw.tick+1;
    audio_service();
    printf("F4-32 transient publish: published=%llu retired=%llu fetched=%llu output_delta=%llu deadlines=%llu\n",
        (unsigned long long)ac97.published,(unsigned long long)ac97.retired,
        (unsigned long long)ac97.fetched,(unsigned long long)(audio.output-output),
        (unsigned long long)audio.deadlines);
    CHECK(ac97.published==AUDIO_VM_LEAD+1 && ac97.retired==1 && ac97.fetched==fetched);
    CHECK(audio.output-output==AUDIO_QUANTUM && !quantum_pending && !audio.deadlines);
    late_cleanup();
}
static void test_transient_wait(void)
{
    late_setup(); vm_attached_hook=vm_active_hook=true; audio_service();
    hw.zero_until=hw.tick+1; audio.pending=false; audio_service();
    uint64_t before=g_ticks;
    audio_worker_wait();
    printf("F4-32 transient rearm: blocked_ms=%llu\n",(unsigned long long)(g_ticks-before));
    CHECK(g_ticks-before==1);
    hw.tick++; audio.pending=false; audio_service();
    before=g_ticks; audio_worker_wait(); CHECK(g_ticks-before==4);
    late_cleanup();
}
static void test_transient_bursts(void)
{
    for(unsigned transitional=0;transitional<2;transitional++) {
        late_setup(); vm_attached_hook=vm_active_hook=vm_nonzero_hook=true; audio_service();
        unsigned irqs=0;
        uint64_t next_callback=g_ticks;
        for(unsigned step=1;step<=100;step++) {
            /* Exactly 480 frames per 10 ms callback. Completions keep their
             * IOC IRQ; services between callbacks cannot invent movement. */
            unsigned left=480;
            next_callback+=10;
            hw.tick+=next_callback-g_ticks; g_ticks=next_callback;
            while(left) {
                unsigned n=hw.remaining/2; if(n>left) n=left;
                mock_advance(n*2); left-=n;
                CHECK(!(hw.sr&AC97_DCH));
                if(hw.sr&AC97_BCIS) {
                    if(transitional) hw.zero_until=hw.tick+1;
                    CHECK(ac97_irq(11,"ac97")); irqs++;
                }
            }
            audio.pending=false; audio_service();
            /* A real blocking wake on the next 1 ms time step reloads PICB. */
            if(hw.tick<hw.zero_until) {
                uint64_t at=g_ticks; audio_worker_wait(); hw.tick+=g_ticks-at;
                audio.pending=false; audio_service();
            }
            CHECK(!audio.error && audio.running && !audio.halts && !audio.deadlines && !audio.underruns);
        }
        printf("F4-32 burst replay: transitional=%u callbacks=100 irqs=%u published=%llu retired=%llu halts=%llu deadlines=%llu\n",
            transitional,irqs,(unsigned long long)ac97.published,(unsigned long long)ac97.retired,
            (unsigned long long)audio.halts,(unsigned long long)audio.deadlines);
        CHECK(irqs==187 && ac97.published-ac97.retired==AUDIO_VM_LEAD);
        late_cleanup();
    }
}
static void test_capacity_burst(void)
{
    for(unsigned frames=1024;frames<=2048;frames*=2) {
    late_setup(); vm_attached_hook=vm_active_hook=vm_nonzero_hook=true; audio_service();
    CHECK(ac97.published==8 && AUDIO_VM_LEAD==8);
    /* One callback, no worker turn between completions. The 1024-frame
     * request at 21 ms used to drain the entire four-period frontier.
     * The 2048-frame request drains the new frontier at ceil(42.667 ms). */
    unsigned elapsed=frames==1024 ? 21 : 43;
    mock_advance(frames*2); hw.tick+=elapsed; g_ticks+=elapsed;
    CHECK(ac97_irq(11,"ac97"));
    bool exhausted=frames==AUDIO_VM_LEAD*AUDIO_QUANTUM;
    CHECK(ac97_progress(&ac97)==(exhausted ? -EIO : 0));
    CHECK(ac97.tail_drained==exhausted);
    CHECK(ac97.published==AUDIO_VM_LEAD && ac97.retired==frames/AUDIO_QUANTUM-exhausted);
    CHECK(ac97.dma_halts==exhausted && !ac97.deadline_misses);
    printf("F4-32 capacity replay: requested_frames=%u elapsed_ms=%u published=%llu retired=%llu halts=%llu deadlines=%llu\n",
        frames,elapsed,
        (unsigned long long)ac97.published,(unsigned long long)ac97.retired,
        (unsigned long long)ac97.dma_halts,(unsigned long long)ac97.deadline_misses);
    uint64_t output=audio.output;
    audio_service();
    CHECK(audio.running && !audio.error && audio.underruns==exhausted && audio.halts==exhausted);
    CHECK(!audio.deadlines && !ac97.deadline_misses && !quantum_pending);
    CHECK(hw.starts==1u+exhausted && hw.resets==exhausted);
    /* At 43 ms the measured gap also triggers the existing 134 ms cushion. */
    unsigned lead=exhausted ? audio_gap_lead(AUDIO_GAP_BUDGET_US,AUDIO_RATE) : AUDIO_VM_LEAD;
    CHECK(ac97.published-ac97.retired==lead && audio.lead_periods==lead);
    CHECK(audio.output==output+(exhausted ? lead : frames/AUDIO_QUANTUM)*AUDIO_QUANTUM);
    if(exhausted) {
        /* Recovery primes silence without consuming another source quantum;
         * vm_recovery_test.c checks the matching production VM re-anchor. */
        for(unsigned i=0;i<lead;i++) for(unsigned j=0;j<AUDIO_QUANTUM;j++)
            CHECK(!ac97.pcm[i][j].left && !ac97.pcm[i][j].right);
        mock_advance(AUDIO_QUANTUM*2); hw.tick+=6; g_ticks+=6; audio_service();
        CHECK(!audio.error && audio.running && audio.underruns==1 && !audio.deadlines);
        CHECK(ac97.pcm[lead][0].left==12000);
    }
    printf("F4-32 capacity service: requested_frames=%u halts=%llu underruns=%llu deadlines=%llu starts=%u PASS\n",
        frames,(unsigned long long)audio.halts,(unsigned long long)audio.underruns,
        (unsigned long long)audio.deadlines,hw.starts);
    late_cleanup();
    }
}
static int transient_dma_cursor(struct audio_dma_device *d, unsigned *frame)
{ (void)d; (void)frame; return -EAGAIN; }
static void test_transient_dma(void)
{
    const enum audio_backend_kind kinds[]={AUDIO_ES1370,AUDIO_ES1371,AUDIO_SB16,AUDIO_VIA686,AUDIO_VIA8233};
    unsigned pages=pages_used;
    for(unsigned i=0;i<ARRAY_SIZE(kinds);i++) {
        memset(&audio,0,sizeof(audio)); second_setup(kinds[i],0x30);
        CHECK(!backend_dma_init(&audio_dma)); audio.ready=true;
        vm_attached_hook=vm_active_hook=vm_nonzero_hook=true; audio_service();
        second.frame=256; second.tick+=10; g_ticks+=10;
        CHECK(!audio_dma_progress(&audio_dma) && audio_dma.state.retired==1);
        const struct audio_dma_ops *original=audio_dma.ops;
        struct audio_dma_ops uncertain=*original; uncertain.cursor=transient_dma_cursor; audio_dma.ops=&uncertain;
        uint64_t fetched=audio_dma.state.fetched;
        audio.pending=false; audio_service();
        CHECK(!audio.error && audio_dma.state.published==AUDIO_VM_LEAD+1 && audio_dma.state.retired==1);
        CHECK(audio_dma.state.fetched==fetched && !audio.deadlines);
        uint64_t at=g_ticks; audio_worker_wait(); CHECK(g_ticks-at==1); second.tick++;
        /* Protect the last coherent current and prefetch slots, even when
         * their starts lie before last_frame inside the current period. */
        struct audio_frame block[AUDIO_QUANTUM]={{123,456}};
        uint64_t published=audio_dma.state.published;
        audio_dma.state.published=2;
        struct audio_frame before=audio_dma.pcm[512];
        CHECK(audio_dma_publish(&audio_dma,block)==-EAGAIN && !memcmp(&before,&audio_dma.pcm[512],sizeof(before)));
        audio_dma.state.published=published;
        second.tick+=2;
        CHECK(audio_dma_progress(&audio_dma)==-EIO && audio_dma.state.deadline_misses==1);
        CHECK(audio_dma.state.retired==1 && audio_dma.state.fetched==fetched);
        audio_dma.ops=original; audio_backend_shutdown();
        CHECK(pages_used==pages && !audio.commitment_pages);
        dma_selected=false; vm_attached_hook=vm_active_hook=vm_nonzero_hook=false;
        idle_since=non_silent_end=0;
    }
    puts("F4-32 cyclic DMA: ES1370/1371, SB16, VIA686/8233 independent publication, ownership guards, 1 ms block, 2 ms retry expiry PASS");
}
static void test_vm_lead_budget(void)
{
    /* Preserve the missed-service budget and cover a 1024-frame capacity
     * callback with four periods left. Nominal source + transport <=50 ms. */
    CHECK(AUDIO_VM_LEAD*256>=480+256+256);
    CHECK((AUDIO_VM_LEAD*256+304)*1000<=50*48000);
    late_setup(); vm_attached_hook=vm_active_hook=vm_nonzero_hook=true; audio_service();
    mock_advance((480+256)*2); hw.tick+=15; g_ticks+=15;
    CHECK(!(hw.sr&AC97_DCH) && ac97.published==AUDIO_VM_LEAD);
    CHECK((AUDIO_VM_LEAD*256-(480+256))>=AUDIO_QUANTUM);
    audio_service(); CHECK(!audio.error && !audio.underruns && !audio.deadlines);
    CHECK(ac97.published-ac97.retired==AUDIO_VM_LEAD);
    late_cleanup();
    puts("F4-32 VM lead: missed-service budget plus capacity callback margin; 8 periods / 2352 frames / 49.0 ms PASS");
}
static void test_transient_transport(void)
{
    const char *which=getenv("CIUKI_F432_CASE");
    if(!which || !strcmp(which,"burst")) test_transient_bursts();
    if(!which || !strcmp(which,"publish")) test_transient_publish();
    if(!which || !strcmp(which,"wait")) test_transient_wait();
    if(!which || !strcmp(which,"dma")) test_transient_dma();
    if(!which || !strcmp(which,"lead")) test_vm_lead_budget();
    if(!which || !strcmp(which,"capacity")) test_capacity_burst();
}
