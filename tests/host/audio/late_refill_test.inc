/* F4-15: run the production service/driver against an LVI-bounded DMA fake.
 * SPDX-License-Identifier: GPL-2.0-only */
static void test_t23_cursor_replay(void)
{
    struct ac97_device d; hardware_setup(&d);
    d.running=true; d.retired=209; d.published=217;
    d.fetched=d.generation_fetched=209*AUDIO_QUANTUM+32;
    hw.cr=1; hw.civ=hw.piv=8; hw.lvi=24; hw.remaining=0;
    hw.sr=AC97_DCH|AC97_CELV|AC97_LVBCI|AC97_BCIS;
    uint64_t fetched=d.fetched;
    CHECK(((hw.civ-(d.retired&31))&31)==23);
    CHECK(ac97_progress(&d)==-EIO && !d.tail_drained && d.retired==209 && d.fetched==fetched);
    /* Same fragments, a possible native idle-tail window: not evidence
     * that the damaged physical record actually had published=233. */
    d.published=233; hw.lvi=8;
    CHECK(hw.piv!=((hw.civ+1)&31)); /* the former PIV comparison rejects this */
    CHECK(ac97_progress(&d)==-EIO && d.tail_drained && d.retired==232);
    CHECK(d.generation_fetched==233*AUDIO_QUANTUM && d.fetched==d.generation_fetched);
    CHECK(d.dma_halts==1);
    CHECK(ac97_progress(&d)==-EIO && d.tail_drained && d.dma_halts==1);
    /* W1C IRQ causes may already have been acknowledged; CELV is enough. */
    hw.sr=AC97_DCH|AC97_CELV; hw.piv=9;
    CHECK(ac97_progress(&d)==-EIO && d.tail_drained && d.fetched==233*AUDIO_QUANTUM);
    /* A wrap in an eight-period window is legal when its delta fits. */
    hardware_setup(&d); d.running=true; d.retired=223; d.published=231;
    d.fetched=d.generation_fetched=223*AUDIO_QUANTUM+200;
    hw.cr=1; hw.civ=hw.piv=0; hw.lvi=6; hw.remaining=448; hw.sr=0;
    CHECK(!ac97_progress(&d) && d.retired==224 && d.fetched==224*AUDIO_QUANTUM+32);
    puts("audio T23 replay: impossible 8-period movement rejected, legal deep-tail exhaustion and CIV/PIV handoff/wrap PASS");
}
static void late_setup(void)
{
    static struct ac97_bd bdl[32]; static struct audio_frame buffers[32][AUDIO_QUANTUM];
    CHECK(!audio.commitment_pages);
    memset(&audio,0,sizeof(audio)); memset(bdl,0,sizeof(bdl)); memset(buffers,0xa5,sizeof(buffers));
    hardware_setup(&ac97); hw.any_pcm=true; ac97.bdl=bdl; ac97.ready=true;
    for(unsigned i=0;i<32;i++) ac97.pcm[i]=buffers[i];
    for(unsigned i=0;i<12;i++) ac97.claims[i]=-1;
    audio.ready=true; audio.master=(struct ciuki_mixer_master){.left=100,.right=100};
    idle_since=non_silent_end=0; first_playback=playback_logged=quantum_pending=quantum_mixed=false;
    transport_next=g_ticks+5000;
    vm_active_hook=vm_attached_hook=vm_nonzero_hook=false;
    g_boot.flags=CBI_F_VERBOSE; g_ticks=hw.tick=1000;
}
static void late_start(void)
{
    vm_active_hook=true;
    for(unsigned i=0;i<AUDIO_LEAD;i++) { g_ticks++; hw.tick++; audio_service(); }
    CHECK(audio.running && ac97.running && ac97.published==AUDIO_LEAD && !audio.error);
    vm_active_hook=false;
}
static struct audio_stream *late_stream(void)
{
    struct audio_stream *s; CHECK(!audio_stream_open(&s));
    struct audio_frame frames[AUDIO_RING];
    for(unsigned i=0;i<AUDIO_RING;i++) frames[i]=(struct audio_frame){1000+i/AUDIO_QUANTUM,-1000};
    CHECK(audio_stream_enqueue(s,frames,sizeof(frames))==(int)sizeof(frames));
    return s;
}
static void late_cleanup(void)
{
    CHECK(!audio.commitment_pages);
    memset(&ac97,0,sizeof(ac97)); audio.ready=audio.running=false;
    vm_active_hook=vm_attached_hook=vm_nonzero_hook=false;
    idle_since=non_silent_end=0; quantum_pending=quantum_mixed=false; g_boot.flags=0;
}
static void test_late_refill(void)
{
    late_setup(); struct audio_stream *s=late_stream(); late_start();
    CHECK(!s->consumed);
    mock_advance(5*512+64); g_ticks+=27; hw.tick+=27;
    audio_service();
    CHECK(!audio.error && ac97.retired==5 && ac97.published==13 && hw.lvi==12);
    CHECK(s->consumed==5*AUDIO_QUANTUM && audio.output==13*AUDIO_QUANTUM);
    CHECK(audio.played_output==5*AUDIO_QUANTUM+32 && !quantum_pending);
    for(unsigned i=0;i<5;i++) CHECK(ac97.pcm[8+i][0].left==1000+(int)i);
    audio_service(); CHECK(s->consumed==5*AUDIO_QUANTUM); /* full lead, no extra mixing */
    audio_stream_close(s); late_cleanup();
    /* Long-running service with varying lateness: 32-entry wraps preserve
     * the same eight-period lead without mistaking modulo CIV for a count. */
    late_setup(); late_start(); vm_active_hook=vm_nonzero_hook=true;
    for(unsigned i=0;i<100;i++) {
        unsigned periods=1+i%6;
        mock_advance(periods*512); g_ticks+=periods*6; hw.tick+=periods*6;
        audio_service();
        CHECK(!audio.error && audio.running && ac97.published-ac97.retired==AUDIO_LEAD);
        CHECK(hw.civ==(ac97.retired&31) && hw.lvi==((ac97.published-1)&31));
    }
    CHECK(ac97.retired>300 && !audio.halts && !audio.deadlines);
    late_cleanup();
    puts("audio late refill: 5-period source catch-up, exact consumption, variable delays across 10 CIV wraps PASS");
}
static void test_exhausted_recovery(void)
{
    /* Both a live source and an inactive source with nonzero PCM still
     * queued recover immediately; the latter used to stall after one period. */
    for(unsigned inactive=0;inactive<2;inactive++) {
        late_setup(); struct audio_stream *s=late_stream(); late_start();
        mock_advance(2*512); g_ticks+=11; hw.tick+=11; audio_service();
        CHECK(s->consumed==2*AUDIO_QUANTUM && ac97.published==10);
        if(inactive) s->started=false;
        mock_advance(10*512); g_ticks+=100; hw.tick+=100;
        CHECK(hw.civ==9 && hw.lvi==9 && !hw.remaining && hw.sr==(AC97_DCH|AC97_CELV|AC97_LVBCI|AC97_BCIS));
        CHECK(hw.silence_words==2*512 && hw.completed_count==10);
        CHECK(hw.completed[8]==1000 && hw.completed[9]==1001);
        CHECK(ac97_irq(11,"ac97") && hw.sr==(AC97_DCH|AC97_CELV));
        unsigned logs=audio_log_count;
        uint64_t output=audio.output;
        audio_service();
        CHECK(audio.ready && !audio.error && audio.running && ac97.running && hw.starts==2 && hw.resets==1);
        CHECK(audio_log_count>=logs+3 && audio.halts==1 && ac97.retired==0 && ac97.published==audio.lead_periods);
        CHECK(ac97.fetched==10*AUDIO_QUANTUM && ac97.frame_base==output && !ac97.generation_fetched);
        CHECK(audio.output==output+audio.lead_periods*AUDIO_QUANTUM && s->consumed==2*AUDIO_QUANTUM);
        for(unsigned i=0;i<audio.lead_periods;i++) for(unsigned j=0;j<AUDIO_QUANTUM;j++)
            CHECK(!ac97.pcm[i][j].left && !ac97.pcm[i][j].right);
        mock_advance(512); g_ticks+=6; hw.tick+=6; audio_service();
        CHECK(!audio.error && ac97.fetched==11*AUDIO_QUANTUM && audio.played_output==output+AUDIO_QUANTUM);
        if(!inactive) CHECK(s->consumed==3*AUDIO_QUANTUM && ac97.pcm[audio.lead_periods][0].left==1002);
        else CHECK(s->consumed==2*AUDIO_QUANTUM && idle_since);
        audio_stream_close(s); late_cleanup();
    }
    puts("audio exhausted recovery: DCH/CELV after IRQ W1C, silence prime, no PCM replay, monotonic fetched and resumed sources PASS");
}
static void test_publication_retry(void)
{
    for(unsigned inactive=0;inactive<2;inactive++) {
    late_setup(); struct audio_stream *s=late_stream(); late_start();
    /* Worker sees the running LVI, whose successor is prefetched. Mixing
     * happens before publication refuses that slot: retain exactly one block. */
    mock_advance(7*512); g_ticks+=38; hw.tick+=38;
    struct audio_frame before=ac97.pcm[8][0]; unsigned writes=hw.lvi_writes;
    audio_service();
    CHECK(!audio.error && quantum_pending && quantum_mixed && s->consumed==AUDIO_QUANTUM);
    CHECK(ac97.published==8 && hw.lvi_writes==writes && !memcmp(&before,&ac97.pcm[8][0],sizeof(before)));
    audio_service(); CHECK(s->consumed==AUDIO_QUANTUM && quantum_pending && ac97.published==8);
    /* The final fetch halts. Recovery publishes the retained PCM once,
     * then silence; no second mix and no loss of its output position. */
    if(inactive) s->started=false;
    mock_advance(512); g_ticks+=6; hw.tick+=6; audio_service();
    CHECK(!audio.error && audio.running && !quantum_pending && ac97.published==AUDIO_LEAD);
    CHECK(s->consumed==AUDIO_QUANTUM && ac97.pcm[0][0].left==1000);
    CHECK(ac97.frame_base==8*AUDIO_QUANTUM && s->last_output==9*AUDIO_QUANTUM);
    for(unsigned i=1;i<AUDIO_LEAD;i++) CHECK(!ac97.pcm[i][0].left && !ac97.pcm[i][0].right);
    mock_advance(512); g_ticks+=6; hw.tick+=6; audio_service();
    CHECK(hw.completed[8]==1000 && !audio.error);
    if(inactive) CHECK(s->consumed==AUDIO_QUANTUM);
    else CHECK(s->consumed==2*AUDIO_QUANTUM && ac97.pcm[8][0].left==1001);
    audio_stream_close(s); late_cleanup();
    }
    puts("audio publication retry: CIV/PIV payload retention, single source consumption and pending PCM across DCH reset PASS");
}
static void test_recovery_failure_record(void)
{
    for(unsigned fault=0;fault<7;fault++) {
        late_setup(); late_start(); vm_active_hook=vm_nonzero_hook=true;
        /* A validated window which crosses CIV wrap, with counters far
         * above 32. Invalid/recoverable variants share the capture fields. */
        ac97.published=233; ac97.retired=209;
        ac97.fetched=ac97.generation_fetched=209*AUDIO_QUANTUM;
        audio.output=non_silent_end=233*AUDIO_QUANTUM;
        hw.civ=hw.lvi=8; hw.piv=8; hw.remaining=0; hw.sr=AC97_DCH|AC97_CELV;
        if(fault==0) hw.sr=AC97_DCH; /* software/unexpected halt */
        if(fault==1) hw.remaining=32; /* partially fetched LVI */
        if(fault==2) hw.lvi=9; /* publication mismatch */
        if(fault==3) hw.cr=0; /* RUN was cleared outside the service */
        if(fault==4) hw.run_clear_stuck=true;
        if(fault==5) hw.reset_stuck=true;
        if(fault==6) { ac97.bdl_phys=0x4000; hw.bdbar_stuck=true; } /* post-reset readback fails */
        unsigned logs=backend_failure_logs;
        audio_service();
        if(fault<4) {
            CHECK(!audio.error && audio.ready && audio.running && hw.starts==2 && hw.resets==1);
            CHECK(backend_failure_logs==logs && audio.underruns==1);
        } else {
            CHECK(audio.error==-EIO && !audio.ready && backend_failure_logs==logs+1);
            CHECK(strstr(audio_log,"op=recover_stop civ=8 lvi=8 piv=8 picb=0 sr=0003 published=233 retired=232 running=1"));
            CHECK(ac97.quarantine && hw.starts==1);
            if(fault==4) CHECK(!hw.resets && ac97.published==233);
            else CHECK(hw.resets==1 && hw.civ==0);
        }
        late_cleanup();
    }
    puts("audio failure snapshot: unexpected/partial halt, LVI/RUN mismatch, failed pause/reset/readback, one complete pre-rollback record PASS");
}
static void test_wide_failure_record(void)
{
    late_setup(); late_start();
    ac97.published=UINT64_MAX; ac97.retired=UINT64_MAX-8;
    hw.civ=hw.lvi=hw.piv=255; hw.remaining=65535; hw.sr=0xffff;
    first_playback=true; playback_start=0; g_ticks=UINT64_MAX-1;
    audio_service();
    CHECK(audio.error==-EIO && strstr(audio_log,"civ=255 lvi=255 piv=255 picb=65535 sr=ffff"));
    CHECK(strstr(audio_log,"published=18446744073709551615 retired=18446744073709551607 running=1"));
    CHECK(strstr(audio_log,"elapsed_ms=18446744073709551614") && strlen(audio_log)<254);
    late_cleanup();
    puts("audio failure record: maximum-width indices/counters fit the production 256-byte log buffer PASS");
}
