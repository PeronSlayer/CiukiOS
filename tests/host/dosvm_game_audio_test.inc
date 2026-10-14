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
    ms(2000); dosvm_audio_log(v); CHECK(strstr(last_vm_audio_log,"buffers_ps=") && strstr(last_vm_audio_log,"starved=0"));
    ms(100); dosvm_audio_log(v); CHECK(strstr(last_vm_audio_log,"pcm=") && strstr(last_vm_audio_log,"fm="));
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
static void test_game_audio(void)
{
    for(unsigned i=0;i<2;i++) {
        unsigned rate=i?44100:48000;
        game_audio_replay(true,rate,false);
        game_audio_replay(false,rate,false);
        game_audio_replay(false,rate,true);
    }
    test_audio_records(); audio.caps.output_rate=48000;
}
