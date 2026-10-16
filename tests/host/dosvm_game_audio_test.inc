/* F4-13 production virtual devices/mixer plus the production physical AC97
 * cursor functions, with only RAM, time and register transport replaced.
 * Wolf sequence follows id's ID_SD.C. DOOM/DMX sequences are protocol
 * replays (DMX was not part of id's source release), not the game binary. */
#include <ciuki/ac97.h>
static struct ac97_cursor game_cursor;
static uint32_t game_read(void *cookie, uint16_t p, unsigned width)
{
    (void)cookie; (void)width;
    switch (p) {
    case 0x14: return game_cursor.civ;
    case 0x15: return game_cursor.lvi;
    case 0x16: return game_cursor.status;
    case 0x18: return game_cursor.remaining;
    case 0x1A: return game_cursor.piv;
    default: CHECK(false); return 0;
    }
}
static void game_write(void *cookie, uint16_t p, uint32_t value, unsigned width)
{ (void)cookie; CHECK(p==0x16 && width==2); game_cursor.status &= ~value; }
static uint64_t game_now(void *cookie) { (void)cookie; return g_ticks; }
#include "dosvm_audio_cursor.inc"

/* alOut's six address reads and 35 data reads are delay cycles; the
 * 100-read detection loop must see the 80 us timer without forcing IF. */
static void wolf_al_out(struct dosvm *v, uint8_t reg, uint8_t value)
{
    a_out(v,0x388,reg); for (unsigned i=0;i<6;i++) a_in(v,0x388);
    a_out(v,0x389,value); for (unsigned i=0;i<35;i++) a_in(v,0x388);
}
static void game_detect(struct dosvm *v, bool wolf)
{
    if (wolf) {
        wolf_al_out(v,4,0x60); wolf_al_out(v,4,0x80);
        uint8_t before=a_in(v,0x388);
        wolf_al_out(v,2,0xFF); wolf_al_out(v,4,0x21);
        for (unsigned i=0;i<100;i++) a_in(v,0x388);
        uint8_t after=a_in(v,0x388);
        wolf_al_out(v,4,0x60); wolf_al_out(v,4,0x80);
        CHECK(!(before&0xE0) && (after&0xE0)==0xC0);
        /* SDL_CheckSB resets and delays through AdLib status reads. */
        a_out(v,SB+6,1); for(unsigned i=0;i<9;i++) a_in(v,0x388);
        a_out(v,SB+6,0); for(unsigned i=0;i<100;i++) a_in(v,0x388);
        CHECK(dsp_read(v)==0xAA);
    } else {
        dsp_reset(v); dsp(v,0xE1); CHECK(dsp_read(v)==4 && dsp_read(v)==5);
        CHECK(mixer_r(v,0x80)==4 && mixer_r(v,0x81)==0x22);
    }
}
static void wolf_segment(struct dosvm *v)
{
    dma8(v,1,0x20000,511,false);
    dsp(v,0x14); dsp(v,0xFF); dsp(v,1);
}
static void game_irq(void *arg)
{
    struct dosvm *v=arg;
    if (!(v->audio.sb.causes&1)) return;
    CHECK(v86_irq_pending(&v->monitor)==7 && dosvm_pic_ack(&v->monitor)==0x0F);
    a_in(v,SB+0xE); /* SDL_SBService / DMX acknowledges the DSP before EOI */
    a_out(v,0x20,0x20);
    if (!v->audio.sb.x.auto_init && !v->audio.sb.x.active) wolf_segment(v);
}
static void game_audio_replay(bool wolf, unsigned rate, bool sb16)
{
    vq_begin(); audio.caps.output_rate=rate; audio.error=0; audio.halts=audio.fifo=0;
    struct dosvm *v=audio_vm_create(); game_detect(v,wolf);
    dsp(v,0xD1);
    if (wolf) {
        dsp(v,0x40); dsp(v,114); /* 256 - floor(1000000/7000), SDL_StartSB */
        mixer_w(v,0x26,0xBB); CHECK(mixer_r(v,0x26)==0xBB);
        mixer_w(v,0x26,0xFF); CHECK(mixer_r(v,0x26)==0xFF);
        mixer_w(v,0x0E,0); mixer_w(v,0x04,0xFF);
    } else { mixer_w(v,0x22,0xFF); mixer_w(v,0x04,0xFF); mixer_w(v,0x26,0xFF); }
    vq_time(1200); vq_run(2);
    uint8_t bytes[4096]; for (unsigned i=0;i<sizeof(bytes);i++) bytes[i]=(uint8_t)(32+(i%64)*3);
    CHECK(!dosvm_write(v,0x20000,bytes,sizeof(bytes)));
    if (wolf) wolf_segment(v);
    else {
        dma8(v,1,0x20000,4095,true);
        if (sb16) {
            dsp(v,0x41); dsp(v,0x2B); dsp(v,0x11); /* 11025 Hz */
            dsp(v,0xC6); dsp(v,0); dsp(v,0xFF); dsp(v,1);
        } else {
            dsp(v,0x40); dsp(v,165); /* legacy DMX-style ~11025 Hz */
            dsp(v,0x48); dsp(v,0xFF); dsp(v,1); dsp(v,0x1C);
        }
    }
    opl_note(v,0); vq_service=game_irq; vq_arg=v;
    vq_run(8);
    CHECK(ainfo(v).active && vstats(v).pcm.frames && vstats(v).opl.frames);
    /* A live native backend at the handoff observed on physical ICH3.
     * Before F4-13, zero PICB was committed and the reload failed EIO;
     * that clears ready and later game VMs have no audio consumer. */
    struct ac97_device backend={.io={.read=game_read,.write=game_write,.now=game_now},
        .running=true,.published=160,.retired=152,.last_progress=g_ticks,
        .generation_fetched=152*AUDIO_QUANTUM+200,.fetched=152*AUDIO_QUANTUM+200};
    game_cursor=(struct ac97_cursor){.civ=24,.piv=25,.lvi=31,.remaining=0};
    CHECK(ac97_progress(&backend)==-EAGAIN && backend.retired==152);
    game_cursor.remaining=512;
    CHECK(ac97_progress(&backend)==-EAGAIN);
    game_cursor.civ=25; game_cursor.piv=26; game_cursor.remaining=448;
    CHECK(!ac97_progress(&backend) && backend.retired==153);
    vq_run(420);
    struct audio_vm_stats st=vstats(v); struct dosvm_audio_info info=ainfo(v);
    struct audio_vm_diagnostic diag; audio_vm_diagnostic(dosvm_key(v),&diag);
    CHECK(diag.ready && diag.attached && diag.rate==rate && diag.published==430);
    CHECK(st.pcm.frames>10000 && st.opl.frames>10000 && st.pcm.peak[0]>500 && st.opl.peak[0]>100);
    CHECK(info.services>20 && !info.dma_faults && !info.unsupported && !info.forced);
    CHECK(!st.starved && !st.revoked && !info.overrun_frames && !info.lost_events);
    dsp_reset(v); destroy(v,true); vq_end(); empty();
    printf("game audio replay: %s %s physical_rate=%u PCM+FM, actual publications, cursor handoff PASS\n",
           wolf?"Wolf3D":"DOOM-style DMX",sb16?"SB16":"legacy",rate);
}
static void test_audio_records(void)
{
    vq_begin(); g_boot.flags=CBI_F_VERBOSE;
    struct dosvm *v=audio_vm_create(); unsigned first=vm_audio_logs;
    static const char env[]="COMSPEC=COMMAND.COM\0BLASTER=A220 I7 D1 H5 T6\0\0";
    CHECK(!dosvm_write(v,0x16300,env,sizeof(env)));
    /* Queueing on the device path never prints. */
    dsp_reset(v); dsp(v,0xE1); CHECK(dsp_read(v)==4 && dsp_read(v)==5);
    CHECK(vm_audio_logs==first);
    dosvm_audio_log(v); CHECK(vm_audio_logs==first+1 && strstr(last_vm_audio_log,"attach=0"));
    dosvm_audio_log(v); CHECK(vm_audio_logs==first+1);
    v->dpmi.depth=1; v->dpmi.rec[0].flags=DPMI_RF_HWINT;
    ms(100); dosvm_audio_log(v); CHECK(vm_audio_logs==first+1);
    v->dpmi.depth=0;
    dosvm_audio_log(v); CHECK(strstr(last_vm_audio_log,"reset=1 reply=aa"));
    ms(100); dosvm_audio_log(v); CHECK(strstr(last_vm_audio_log,"version=4.05"));
    ms(100); dosvm_audio_log(v); CHECK(strstr(last_vm_audio_log,"blaster=A220_I7_D1_H5_T6 source=guest_env"));
    for(unsigned i=0;i<32;i++) { dsp_reset(v); dma8(v,1,0x20000,511,false); }
    CHECK(v->audio.diag.count<=16 && v->audio.diag.used[DOSVM_AUDIO_RESET]==4);
    while(v->audio.diag.count) { ms(100); dosvm_audio_log(v); }
    while(v->audio.diag.state_count) { ms(100);dosvm_audio_log(v); }
    unsigned quiet=vm_audio_logs;
    ms(2000);dosvm_audio_log(v);CHECK(vm_audio_logs==quiet);
    g_boot.flags|=CBI_F_DIAG_VERBOSE;
    ms(100);dosvm_audio_log(v);CHECK(strstr(last_vm_audio_log,"level_db="));
    ms(100); dosvm_audio_log(v); CHECK(strstr(last_vm_audio_log,"buffers_ps=") && strstr(last_vm_audio_log,"starved=0"));
    ms(100); dosvm_audio_log(v); CHECK(strstr(last_vm_audio_log,"pcm=") && strstr(last_vm_audio_log,"fm="));
    CHECK(strstr(last_vm_audio_log,"backend_halts=") && !strstr(last_vm_audio_log," halt="));
    unsigned logged=vm_audio_logs;
    g_boot.flags=0; ms(3000); dosvm_audio_log(v); CHECK(vm_audio_logs==logged);
    destroy(v,true); vq_end(); empty();
    /* A failed backend is explicit, and queue discard is observable. */
    g_boot.flags=CBI_F_VERBOSE; audio.ready=false; audio.error=-EIO;
    v=audio_vm_create(); dosvm_audio_log(v); CHECK(strstr(last_vm_audio_log,"attach=-19"));
    dsp_reset(v); dsp(v,0x10); dsp(v,255);
    CHECK(!ainfo(v).consumer && ainfo(v).discarded_events==1);
    destroy(v,true); audio.error=0; g_boot.flags=0; empty();
}
/* T23 programming evidence: DMA1, 063b60h, 4096 bytes, 11025 Hz and
 * 256-unit DSP blocks. Fresh playback is a control; paused auto-init
 * reprogramming exposes the boundary-defer deadlock. Protocol replay only. */
static void dmx_start(struct dosvm *v, unsigned command)
{
    if (command == 0xC6) {
        dsp(v,0x41); dsp(v,0x2B); dsp(v,0x11);
        dsp(v,command); dsp(v,0); dsp(v,255); dsp(v,0);
    } else {
        dsp(v,0x40); dsp(v,165); /* 10989 Hz, legacy TC quantization */
        if (command == 0x1C) { dsp(v,0x48); dsp(v,255); dsp(v,0); dsp(v,command); }
        else { dsp(v,command); dsp(v,255); dsp(v,0); }
    }
}
static void test_dmx_restart(void)
{
    static const unsigned commands[]={0x14,0x1C,0xC6};
    for (unsigned r=0;r<2;r++) for (unsigned c=0;c<ARRAY_SIZE(commands);c++)
        for (unsigned restart=0;restart<3;restart++) {
            if (restart && commands[c]==0x14) continue; /* single-cycle tails defer by design */
            vq_begin(); audio.caps.output_rate=r?44100:48000;
            audio.halts=1; /* cumulative native halt must not gate either source */
            struct dosvm *v=audio_vm_create(); game_detect(v,false);
            uint8_t bytes[4096];
            for (unsigned i=0;i<sizeof(bytes);i++) bytes[i]=(i&32)?224:32;
            CHECK(!dosvm_write(v,0x63B60,bytes,sizeof(bytes)));
            dma8(v,1,0x63B60,4095,commands[c]!=0x14);
            if (restart) {
                dmx_start(v,0xC6);
                if (restart==1) { dsp(v,0xD0); CHECK(ainfo(v).paused); }
            }
            dmx_start(v,commands[c]);
            opl_note(v,0);
            vq_service=game_irq; vq_arg=v;
            vq_time(1200); vq_run(24);
            struct dosvm_audio_info info=ainfo(v); struct audio_vm_stats st=vstats(v);
            printf("T23 DMX replay: cmd=%02x restart=%s output=%u units=%u IRQ=%u/%u PCM=%llu FM=%llu\n",
                   commands[c],restart==1?"paused":restart==2?"running":"fresh",audio_output_rate(),info.units,info.irq_edges,info.services,
                   (unsigned long long)st.pcm.frames,(unsigned long long)st.opl.frames);
            CHECK(info.units>=256 && info.irq_edges && info.services && !info.dma_faults);
            CHECK(st.pcm.frames && st.opl.frames && first_sound(0)<vq_n);
            CHECK(!info.paused && !v->audio.sb.next_valid);
            dsp_reset(v); destroy(v,true); vq_end(); empty();
        }
    audio.halts=0;
}
static void test_vm_audio_levels(void)
{
    vq_begin();
    struct dosvm *v=audio_vm_create(); struct dosvm_key key=dosvm_key(v);
    uint8_t bytes[4096]; memset(bytes,224,sizeof(bytes));
    CHECK(!dosvm_write(v,0x63B60,bytes,sizeof(bytes)));
    dma8(v,1,0x63B60,4095,true); dmx_start(v,0xC6); opl_note(v,0);
    vq_service=game_irq; vq_arg=v; vq_time(1200); vq_run(420);
    struct audio_vm_diagnostic d, observed;
    audio_vm_diagnostic(key,&d);
    CHECK(d.level_at && d.pcm_peak>500 && d.opl_peak && d.mix_peak && d.ring_peak);
    unsigned wakes=audio_wakes; uint32_t units=ainfo(v).units;
    audio_vm_diagnostic(key,&observed);
    CHECK(d.published==observed.published && d.level_at==observed.level_at && d.pcm_peak==observed.pcm_peak &&
          d.opl_peak==observed.opl_peak && d.mix_peak==observed.mix_peak && d.ring_peak==observed.ring_peak &&
          wakes==audio_wakes && units==ainfo(v).units);
    CHECK(sb_level_db(0)==-96 && sb_level_db(32768)==0 && sb_level_db(16384)==-6 && sb_level_db(1)==-90);
    /* Pause cannot mute FM, even with a past native transport halt. */
    dsp(v,0xD0); audio.halts=1; vq_run(420); audio_vm_diagnostic(key,&d);
    CHECK(ainfo(v).paused && !d.pcm_peak && d.opl_peak && d.ring_peak && d.halts==1);
    dsp(v,0xD4);
    /* Global digital mute: source taps remain audible, final PCM is zero. */
    audio.master.mute=1; vq_run(420); audio_vm_diagnostic(key,&d);
    CHECK(d.master_muted && d.pcm_peak && d.opl_peak && !d.mix_peak && !d.ring_peak);
    audio.master.mute=0;
    CHECK(!audio_vm_set_gain(key,100,100,true));
    vq_run(420); audio_vm_diagnostic(key,&d);
    CHECK(d.muted && !d.pcm_peak && !d.opl_peak && !d.mix_peak && !d.ring_peak);
    CHECK(!audio_vm_set_gain(key,0,0,false));
    vq_run(420); audio_vm_diagnostic(key,&d);
    CHECK(!d.muted && !d.gain[0] && !d.gain[1] && !d.ring_peak && audio_vm_playing());
    CHECK(!audio_vm_set_gain(key,100,100,false));
    audio.master.left=audio.master.right=25; audio.master_in_codec=true;
    vq_run(420); audio_vm_diagnostic(key,&d);
    CHECK(d.master_in_codec && d.master[0]==25 && d.pcm_peak && d.mix_peak && d.ring_peak);
    /* Mixed, unaccepted samples must not appear in a published window. */
    CHECK(!audio_vm_set_gain(key,100,100,true)); vq_run(420); audio_vm_diagnostic(key,&d);
    CHECK(!d.ring_peak);
    CHECK(!audio_vm_set_gain(key,100,100,false)); vq_time(1200);
    audio_mix(vq_out,AUDIO_QUANTUM); /* no fake publication */
    audio_vm_diagnostic(key,&observed);
    CHECK(observed.published==d.published && observed.level_at==d.level_at && !observed.ring_peak);
    audio_vm_published();
    dsp_reset(v); destroy(v,true); vq_end(); empty();
    audio.master_in_codec=false; audio.halts=0;
    puts("VM audio levels: published one-second PCM/FM/output peaks, DSP pause independence, source/master mute, codec master and read-only snapshots PASS");
}
static void test_sb_state_records(void)
{
    vq_begin(); g_boot.flags=CBI_F_VERBOSE | CBI_F_DIAG_VERBOSE;
    struct dosvm *v=audio_vm_create(); unsigned logs=vm_audio_logs;
    dsp_reset(v); dma8(v,1,0x63B60,4095,true); dmx_start(v,0xC6);
    CHECK(v->audio.diag.sb_state==DOSVM_SB_RUNNING);
    a_out(v,0x0A,5); CHECK(v->audio.diag.sb_state==DOSVM_SB_DMA_WAIT);
    a_out(v,0x0A,1); CHECK(v->audio.diag.sb_state==DOSVM_SB_RUNNING);
    a_out(v,0x08,4); CHECK(v->audio.diag.sb_state==DOSVM_SB_DMA_WAIT);
    a_out(v,0x08,0); CHECK(v->audio.diag.sb_state==DOSVM_SB_RUNNING);
    dsp(v,0xD0); CHECK(v->audio.diag.sb_state==DOSVM_SB_PAUSED);
    dsp(v,0xD4); CHECK(v->audio.diag.sb_state==DOSVM_SB_RUNNING);
    a_out(v,0x0B,0x55); ms(1); /* channel 1 memory-write is invalid for playback */
    CHECK(v->audio.diag.sb_state==DOSVM_SB_ERROR && ainfo(v).last_error==DOSVM_AUDIO_DMA_MODE);
    CHECK(vm_audio_logs==logs); /* no device/tick formatting or printing */
    v->audio.diag.environment=true;
    bool pause=false, fault=false, dma=false, level=false;
    for(unsigned i=0;i<60;i++) {
        g_ticks+=100; dosvm_audio_log(v); /* worker time, no new guest transitions */
        pause|=strstr(last_vm_audio_log,"state=paused reason=dsp cmd=d0")!=0;
        fault|=strstr(last_vm_audio_log,"state=error reason=fault")!=0;
        dma|=strstr(last_vm_audio_log,"state=dma_wait reason=dma_io")!=0;
        level|=strstr(last_vm_audio_log,"level_db=")!=0;
    }
    CHECK(pause && fault && dma && level && !v->audio.diag.state_count);
    dosvm_tick(); /* settle worker-only clock jumps while the failed DSP is inactive */
    /* Verify DSP IRQ-boundary and ACK transitions with production PIC
     * arbitration; no guest ISR execution is claimed by this replay. */
    dsp_reset(v); dma8(v,1,0x63B60,4095,true); dmx_start(v,0xC6); ms(24);
    CHECK(v->audio.diag.sb_state==DOSVM_SB_IRQ_WAIT && v86_irq_pending(&v->monitor)==7);
    game_irq(v); CHECK(v->audio.diag.sb_state==DOSVM_SB_RUNNING && ainfo(v).services);
    /* State recording has no lifetime quota; overflow is explicit and
     * periodic level records still win over a saturated state queue. */
    dsp_reset(v); dma8(v,1,0x63B60,4095,true); dmx_start(v,0xC6);
    for(unsigned i=0;i<32;i++) { dsp(v,0xD0); dsp(v,0xD4); }
    CHECK(v->audio.diag.state_count==16 && v->audio.diag.dropped);
    while(v->audio.diag.count) { g_ticks+=100; dosvm_audio_log(v); }
    if(g_ticks<v->audio.diag.next_level) g_ticks=v->audio.diag.next_level;
    else g_ticks+=100;
    dosvm_audio_log(v);
    CHECK(strstr(last_vm_audio_log,"level_db="));
    dsp_reset(v); destroy(v,true); vq_end(); empty(); g_boot.flags=0;
    puts("SB state records: pause/resume, mask/controller waits, local DMA failure, bounded queue and periodic priority PASS");
}
static void test_game_audio(void)
{
    test_dmx_restart();
    test_vm_audio_levels(); test_sb_state_records();
    for(unsigned i=0;i<2;i++) {
        unsigned rate=i?44100:48000;
        game_audio_replay(true,rate,false);
        game_audio_replay(false,rate,false);
        game_audio_replay(false,rate,true);
    }
    test_audio_records(); audio.caps.output_rate=48000;
}
