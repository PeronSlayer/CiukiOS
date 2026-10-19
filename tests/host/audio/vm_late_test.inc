/* F4-35: production SB16 C6/00, DMA1, VM source and OPL, fake output
 * clock. Protocol fixture, not execution of proprietary DOOM/DMX. */
static struct audio_frame late_out[AUDIO_QUANTUM];
static int64_t late_sums[AUDIO_QUANTUM][2];
extern unsigned audio_vm_probe_jitter(struct dosvm_key, bool);
static void late_setup(void)
{
    audio.ready = audio.running = true; audio.caps.output_rate = AUDIO_RATE;
    audio.master_in_codec = audio.master_in_hardware = false;
    audio.master = (struct ciuki_mixer_master){.version=1,.size=32,.left=100,.right=100};
    audio.lead_periods = AUDIO_VM_LEAD;
    audio.output = 8192; audio.played_output = audio.output - 7 * AUDIO_QUANTUM;
}
static struct dosvm *late_start(bool single)
{
    late_setup();
    struct dosvm *v = audio_vm_create(); dsp_reset(v);
    uint8_t bytes[128];
    for (unsigned i=0;i<128;i++) bytes[i] = 0x90 + i % 64;
    CHECK(!dosvm_write(v,0x30000,bytes,sizeof(bytes)));
    dma8(v,1,0x30000,127,true);
    dsp(v,0x41); dsp(v,0x2B); dsp(v,0x11); /* 11025 Hz */
    dsp(v,0xC6); dsp(v,0); dsp(v,63); dsp(v,0);
    if (single) dsp(v,0xDA);
    return v;
}
static struct audio_vm_stats late_stats(struct dosvm *v, const char *name)
{
    struct audio_vm_stats s; CHECK(audio_vm_stats(dosvm_key(v),&s));
    printf("F4-35 %s: hist=%llu,%llu,%llu,%llu dropped=%llu pcm=%llu delay=%u reanchors=%u lead=%d/%d/%d lag=%d/%d drop=%u/%u/%u\n",
        name,(unsigned long long)s.late_hist[0],(unsigned long long)s.late_hist[1],
        (unsigned long long)s.late_hist[2],(unsigned long long)s.late_hist[3],
        (unsigned long long)s.late_frames,(unsigned long long)s.pcm_frames,s.delay_frames,s.reanchors,
        s.lead,s.lead_min,s.lead_max,s.lag,s.lag_min,s.drop_runs,s.drop_last,s.drop_max);
    return s;
}
static void late_finish(struct dosvm *v)
{ dsp_reset(v); destroy(v,true); audio.ready=audio.running=false; empty(); }
static void test_vm_late_block(unsigned age_ms)
{
    struct dosvm *v = late_start(true);
    uint64_t began=v->audio.clock;
    ms(age_ms+7);
    /* First source frame is the completion of DMA unit 1: ceil(48000/11025).
     * Select the output cursor so the initial source target is exactly late. */
    uint64_t target=began+5+age_ms*48;
    uint64_t queued=AUDIO_VM_LATENCY+target-v->audio.clock;
    CHECK(queued<=(AUDIO_VM_LEAD+1)*AUDIO_QUANTUM);
    audio.played_output=audio.output-queued;
    unsigned heard=0;
    for (unsigned q=0;q<3;q++) {
        memset(late_sums,0,sizeof(late_sums));
        audio_vm_mix(late_sums,AUDIO_QUANTUM);
        for (unsigned k=0;k<AUDIO_QUANTUM;k++) if (late_sums[k][0]) {
            /* An independent rational ramp oracle includes the attack and
             * the last held sample; no source frame may disappear. */
            if (age_ms==5) {
                unsigned idx=heard*11025/48000, phase=heard*11025%48000;
                int expected=(16+(int)idx)*256;
                if (idx<63) expected+=(int)(256*phase/48000);
                CHECK(late_sums[k][0]==expected && late_sums[k][1]==expected);
            }
            heard++;
        }
        audio.output+=AUDIO_QUANTUM; audio.played_output+=AUDIO_QUANTUM;
        ms(5); /* source has stopped; only output clock progresses */
    }
    struct audio_vm_stats s=late_stats(v,age_ms==5 ? "late5" : "late100");
    CHECK(s.log_count==1 && s.log[0].frames==64);
    if (age_ms==100) CHECK(!heard && s.late_frames==64 && s.log[0].dropped==64 && s.drop_runs && s.drop_max==64 &&
        s.drop_lag==331 && s.drop_lag_min==331 && s.drop_lead==2352 && s.drop_lead_min==2352 && s.drop_lead_max==2352);
    else CHECK(heard==279 && s.pcm_frames==64 && !s.late_frames && !s.log[0].dropped && s.delay_frames==240 && s.reanchors==1);
    /* Exercise the protected cadence supplements through the real logger,
     * including the drop-time snapshot, with its 256-byte host limit. */
    unsigned logs=vm_audio_logs;
    v->audio.diag.cadence=v->audio.sb.cadence; v->audio.diag.cadence_pending=true;
    dosvm_audio_log(v);
    CHECK(vm_audio_logs==logs+(age_ms==100 ? 4 : 3));
    CHECK(strstr(last_vm_audio_log,age_ms==100 ? "cadence_drop runs=1 last=64 max=64 lag=331" : "cadence_source late=0 delay=240"));
    late_finish(v);
}
static void test_vm_late_jitter(bool gate)
{
    struct dosvm *v=late_start(false);
    if (gate) audio_vm_probe_jitter(dosvm_key(v),true);
    /* DOOM's nominal 140 Hz PIT, production mode-3 clock. */
    device_out(v,0x43,0x36); device_out(v,0x40,8522&255); device_out(v,0x40,8522>>8);
    /* Same 1 ms timer and guest service seam as the f3-42/f3-44 replay.
     * The refill callback instead arrives in the repeating 20/1/7/13 ms
     * schedule. The fake output advances independently at 48 kHz. */
    uint64_t began=v->audio.clock, pit0=v->pit.edges;
    unsigned generated=0, next=gate ? 1 : 20, schedule=0;
    const unsigned gaps[]={20,1,7,13};
    for (unsigned t=1;t<=2000;t++) {
        g_current = t%31<3 ? &caller : v->task;
        ms(1);
        if (t%7) { a_in(v,SB+0xE); v->monitor.pic[0].irr=0; }
        if (t<next) continue;
        uint64_t played=8192-7*AUDIO_QUANTUM+t*48;
        /* No initial source rendering until callback 1. Afterwards retain
         * all publication slots; every callback catches up in order. */
        if (!generated) audio.output=played+7*AUDIO_QUANTUM;
        audio.played_output=played;
        while (audio.output<played+8*AUDIO_QUANTUM) {
            audio_mix(late_out,AUDIO_QUANTUM); generated+=AUDIO_QUANTUM;
        }
        next+=gate ? 1 : gaps[++schedule%4];
    }
    uint64_t active_clock=v->audio.clock-began, pit_edges=v->pit.edges-pit0;
    unsigned active_units=v->audio.sb.units, active_blocks=v->audio.sb.boundaries, active_edges=v->audio.sb.irq_edges;
    CHECK(active_clock==96000 && active_units==22050 && active_blocks==344 && pit_edges>=279 && pit_edges<=280);
    unsigned gate_gap=gate ? audio_vm_probe_jitter(dosvm_key(v),false) : 0;
    CHECK(gate_gap<=20);
    dsp(v,0xDA); ms(7); a_in(v,SB+0xE);
    for (unsigned q=0;q<16;q++) {
        audio.played_output=audio.output-7*AUDIO_QUANTUM;
        audio_mix(late_out,AUDIO_QUANTUM); ms(5);
    }
    struct audio_vm_stats s=late_stats(v,gate ? "probe_gate20" : "jitter20");
    printf("F4-35 clocks: active_ms=2000 dma_units=%u blocks=%u irq_edges=%u pit_edges=%llu vm_frames=%llu output_frames=%u gate_gap_ms=%u\n",
        active_units,active_blocks,active_edges,
        (unsigned long long)pit_edges,(unsigned long long)active_clock,generated,gate_gap);
    CHECK(!s.late_frames && s.delay_frames<=AUDIO_VM_LATE_BUDGET && s.lead_max<=(int32_t)(AUDIO_VM_LATENCY+AUDIO_VM_LATE_BUDGET+256));
    CHECK(s.blocks==345 && s.pcm_frames==22080);
    for (unsigned i=0;i<AUDIO_VM_LOG;i++) CHECK(!s.log[i].dropped && !(s.log[i].flags&AUDIO_VM_BLOCK_DROPPED));
    CHECK(!v->audio.q.overrun_frames);
    g_current=&caller; late_finish(v);
}
static void test_vm_late_budget(void)
{
    struct dosvm *v=late_start(true);
    for (unsigned block=0;block<2;block++) {
        uint64_t began=v->audio.clock;
        if (block) { dsp(v,0xC6); dsp(v,0); dsp(v,63); dsp(v,0); dsp(v,0xDA); }
        ms(block ? 31 : 26);
        uint64_t target=began+5+(block ? 912+240 : 912);
        audio.output+=4*AUDIO_QUANTUM;
        audio.played_output=audio.output-(AUDIO_VM_LATENCY+target-v->audio.clock);
        for (unsigned q=0;q<3;q++) {
            memset(late_sums,0,sizeof(late_sums)); audio_vm_mix(late_sums,AUDIO_QUANTUM);
            audio.output+=AUDIO_QUANTUM; audio.played_output+=AUDIO_QUANTUM; ms(5);
        }
    }
    struct audio_vm_stats s=late_stats(v,"budget_exhausted");
    CHECK(s.delay_frames==912 && s.late_frames==55 && s.log_count==2 &&
          !s.log[0].dropped && s.log[1].dropped==55 && s.pcm_frames==73);
    late_finish(v);
}
static void test_vm_late(void)
{
    test_vm_late_block(5); test_vm_late_block(100);
    test_vm_late_budget();
    test_vm_late_jitter(false); test_vm_late_jitter(true);
}
