/* APU/WaveCache/AC-link model, not an Intel DMA model. No physical evidence. */
static struct {
    struct audio_dma_device *d;
    uint32_t cfg[64], address;
    uint16_t apu[64][16], indexed[32], wave[512], codec[64], ports[128];
    unsigned index, apu_index, wave_index, codec_index, codec_value;
    unsigned clock, writes, gpio_writes, resets, irq_acks, run_writes;
    unsigned last_left, last_right, output_frames;
    uint64_t tick, phase[6];
    bool index_bad, mode_stuck, bme_stuck, codec_busy, reject_codec, no_progress, wave_bad;
} maestro_hw;
static uint32_t maestro_model_read(void *cookie, uint16_t port, unsigned width)
{
    (void)cookie;
    if (port==0xcfc) return maestro_hw.cfg[(maestro_hw.address&255)/4];
    if (port==0x4d0 || port==0x4d1) return 255;
    unsigned off=port-maestro_hw.d->base;
    if (off==0) {
        CHECK(width==2);
        if (maestro_hw.index==0) return maestro_hw.apu[maestro_hw.apu_index/16][maestro_hw.apu_index%16];
        if (maestro_hw.index==1) return maestro_hw.index_bad ? 0xffff : maestro_hw.apu_index;
        return maestro_hw.indexed[maestro_hw.index];
    }
    if (off==0x12) return maestro_hw.wave_bad ? 0xffff : maestro_hw.wave[maestro_hw.wave_index];
    if (off==0x30) return maestro_hw.codec_busy ? 1 : 0;
    if (off==0x32) return maestro_hw.codec[maestro_hw.codec_index/2] | (maestro_hw.codec_index==0x26 ? 15 : 0);
    unsigned value=maestro_hw.ports[off/2];
    return width==1 ? (value>>((off&1)*8))&255 : value;
}
static void maestro_model_write(void *cookie, uint16_t port, uint32_t value, unsigned width)
{
    (void)cookie;
    maestro_hw.writes++;
    if (port==0xcf8) { CHECK(width==4); maestro_hw.address=value; return; }
    if (port>=0xcfc && port<=0xcff) {
        unsigned reg=(maestro_hw.address&255)+port-0xcfc, shift=(reg&3)*8;
        uint32_t mask=width==4 ? UINT32_MAX : ((1u<<(width*8))-1)<<shift;
        if (reg==4 && maestro_hw.bme_stuck) value|=4;
        maestro_hw.cfg[reg/4]=(maestro_hw.cfg[reg/4]&~mask)|(value<<shift); return;
    }
    unsigned off=port-maestro_hw.d->base;
    if (off==0x60 || off==0x64 || off==0x68) maestro_hw.gpio_writes++;
    if (off==0x18 && (value&0xc000)) maestro_hw.resets++;
    if (off==2) { CHECK(width==2 && value<32); maestro_hw.index=value; return; }
    if (off==0) {
        CHECK(width==2);
        if (maestro_hw.index==1) { CHECK(value<1024); maestro_hw.apu_index=value; return; }
        if (maestro_hw.index==0) {
            unsigned voice=maestro_hw.apu_index/16, reg=maestro_hw.apu_index%16;
            if (!reg && (value&MAESTRO_MODE)) maestro_hw.run_writes++;
            if (!reg && maestro_hw.mode_stuck && (maestro_hw.apu[voice][reg]&MAESTRO_MODE)) value|=maestro_hw.apu[voice][reg]&MAESTRO_MODE;
            maestro_hw.apu[voice][reg]=value; return;
        }
        maestro_hw.indexed[maestro_hw.index]=value; return;
    }
    if (off==0x10) { CHECK(width==2 && value<512); maestro_hw.wave_index=value; return; }
    if (off==0x12) { CHECK(width==2); maestro_hw.wave[maestro_hw.wave_index]=value; return; }
    if (off==0x32) { CHECK(width==2); maestro_hw.codec_value=value; return; }
    if (off==0x30) {
        CHECK(width==1); maestro_hw.codec_index=value&0x7e;
        if (!(value&0x80) && !maestro_hw.reject_codec) maestro_hw.codec[(value&0x7e)/2]=maestro_hw.codec_value;
        return;
    }
    if (off==0x1a) { maestro_hw.ports[off/2]&=~value; maestro_hw.irq_acks++; return; }
    if (width==1) {
        unsigned shift=(off&1)*8;
        maestro_hw.ports[off/2]=(maestro_hw.ports[off/2]&~(255u<<shift))|(value<<shift);
    } else { CHECK(width==2); maestro_hw.ports[off/2]=value; }
}
static uint64_t maestro_model_now(void *cookie) { (void)cookie; return maestro_hw.tick; }
static void maestro_model_sleep(void *cookie, uint32_t ms)
{
    (void)cookie;
    CHECK(host_flags&0x200); /* codec waits and drain proof cannot sleep IRQ-off */
    maestro_hw.tick+=ms;
    if (maestro_hw.no_progress || !(maestro_hw.cfg[1]&4) || !(maestro_hw.ports[0x14/2]&0x100)) return;
    for (unsigned voice=0;voice<6;voice++) {
        uint16_t *apu=maestro_hw.apu[voice];
        unsigned mode=(apu[0]>>4)&15;
        if (mode!=2 && mode!=11 && mode!=9) continue;
        uint32_t step=(uint32_t)apu[3]<<8 | apu[2]>>8;
        maestro_hw.phase[voice]+=(uint64_t)ms*maestro_hw.clock*step;
        unsigned advance=maestro_hw.phase[voice]/65536000;
        maestro_hw.phase[voice]%=65536000;
        unsigned begin=(uint16_t)(apu[6]-apu[7]);
        for (unsigned i=0;i<advance;i++) {
            unsigned at=(uint16_t)(apu[5]-begin);
            if (mode==2 && voice==0) {
                struct audio_frame f=maestro_hw.d->pcm[at];
                maestro_hw.last_left=(uint16_t)f.left; maestro_hw.last_right=(uint16_t)f.right; maestro_hw.output_frames++;
            }
            if (mode==11) {
                int16_t *samples=P2V(maestro_hw.d->phys+MAESTRO_CAPTURE_OFFSET);
                samples[(voice-2)*MAESTRO_FRAMES+at]=(int16_t)((voice==2 ? 1000 : -1000)+(int)at);
            }
            apu[5]=begin+(at+1)%apu[7];
        }
    }
    if (maestro_hw.ports[0x18/2]&4) maestro_hw.ports[0x1a/2]|=4;
}
static void maestro_setup(enum audio_backend_kind kind)
{
    memset(&audio_dma,0,sizeof(audio_dma)); memset(&maestro,0,sizeof(maestro)); memset(&maestro_hw,0,sizeof(maestro_hw));
    struct audio_dma_device *d=&audio_dma; maestro_hw.d=d;
    maestro_hw.clock=48828; maestro_hw.cfg[1]=1;
    maestro_hw.codec[0x7c/2]=0x8384; maestro_hw.codec[0x7e/2]=0x7608;
    maestro_hw.codec[2/2]=0x8202; maestro_hw.codec[0x18/2]=0x8909; maestro_hw.codec[0x26/2]=0x800f;
    maestro_hw.codec[0x1a/2]=0x0101; maestro_hw.codec[0x1c/2]=0x8505;
    maestro_hw.ports[0x60/2]=0x409; maestro_hw.ports[0x64/2]=0xf9ff; maestro_hw.ports[0x68/2]=0x601;
    d->io=(struct ac97_io){0,maestro_model_read,maestro_model_write,maestro_model_now,maestro_model_sleep};
    d->kind=kind; d->ops=&maestro_ops; d->name="ess1978"; d->base=0x1000; d->bytes=256; d->command=1;
    d->pci=(struct pci_func){.vendor=kind==AUDIO_MAESTRO1 ? 0x1285 : 0x125d,
        .device=kind==AUDIO_MAESTRO1 ? 0x100 : kind==AUDIO_MAESTRO2 ? 0x1968 : 0x1978,
        .sub_vendor=0x0e11,.sub_device=0xb112,.class_code=4,.subclass=1,.irq_line=11,.irq_pin=1,.command=1};
    d->state.irq=11;
    for (unsigned i=0;i<8;i++) d->claims[i]=-1;
    second_claim_next=12; second_claim_failure=-1; second_irq_adds=0;
    resource_releases=resource_quarantines=irq_removals=0;
    dma_selected=true; audio.backend_name=d->name; audio.backend_pci=d->pci;
    audio.master=(struct ciuki_mixer_master){.version=1,.size=32,.left=50,.right=50};
    audio.error=0; audio.ready=audio.running=audio.quarantined=false;
}
static void maestro_publish_lead(void)
{
    struct audio_frame data[AUDIO_QUANTUM];
    for (unsigned i=0;i<AUDIO_QUANTUM;i++) data[i]=(struct audio_frame){1200,-2400};
    for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(data));
    CHECK(!audio_backend_start());
}
static void test_maestro(void)
{
    unsigned baseline=pages_used;
    struct audio_dma_device *d=&audio_dma;
    /* E500 uses the firmware PIIX link; no vendor-name route guessing. */
    maestro_setup(AUDIO_MAESTRO2E);
    uint8_t pir[48]={0}; memcpy(pir,"$PIR",4); pir[5]=1; pir[6]=48; pir[9]=8;
    pir[33]=5*8; pir[34]=0x60; pir[36]=8;
    pir[12]=0x86; pir[13]=0x80; pir[14]=0x10; pir[15]=0x71;
    unsigned sum=0; for (unsigned i=0;i<48;i++) sum+=pir[i]; pir[31]=(uint8_t)-sum;
    struct pci_func e500=d->pci; e500.dev=5;
    second_pci_count=1; second_pci[0]=(struct pci_func){.bus=0,.dev=1,.class_code=6,.subclass=1,.vendor=0x8086,.device=0x7110};
    maestro_hw.cfg[0x60/4]=11;
    CHECK(audio_pir_route(&d->io,&e500,pir,sizeof(pir)));
    maestro_hw.cfg[0x60/4]=0x8b; CHECK(!audio_pir_route(&d->io,&e500,pir,sizeof(pir)));
    pir[31]++; CHECK(!audio_pir_route(&d->io,&e500,pir,sizeof(pir)));
    second_pci_count=0;
    for (unsigned kind=AUDIO_MAESTRO1;kind<=AUDIO_MAESTRO2E;kind++) {
        maestro_setup(kind);
        enum audio_backend_kind found=0;
        CHECK(maestro_match(&d->pci,&found) && found==kind);
        struct pci_func bad=d->pci; bad.device=0x1998; CHECK(!maestro_match(&bad,&found));
        bad=d->pci; bad.subclass=0; CHECK(!maestro_match(&bad,&found));
        uint16_t saved_gpio[3]={maestro_hw.ports[0x60/2],maestro_hw.ports[0x64/2],maestro_hw.ports[0x68/2]};
        CHECK(!backend_dma_init(d));
        CHECK(d->state.ready && !d->state.running && !d->state.quarantine);
        CHECK(d->dma_pages==32 && pages_used==baseline+32 && audio.commitment_pages==32);
        CHECK(!(d->phys&(MAESTRO_ARENA_BYTES-1)) && d->phys+MAESTRO_ARENA_BYTES<=0x1000000);
        CHECK(audio.caps.backend==kind && audio.caps.output_rate==48000);
        CHECK(maestro.clock>=48800 && maestro.clock<=48840 && !(d->state.power&0x8000));
        CHECK(!maestro_hw.resets && !maestro_hw.gpio_writes);
        CHECK(maestro_hw.ports[0x60/2]==saved_gpio[0] && maestro_hw.ports[0x64/2]==saved_gpio[1] && maestro_hw.ports[0x68/2]==saved_gpio[2]);
        if (kind==AUDIO_MAESTRO2E) CHECK(maestro.e500 && (maestro_hw.ports[0xc0/2]&0x20));
        else CHECK(!maestro.e500 && !(maestro_hw.ports[0xc0/2]&0x20));
        maestro_publish_lead();
        CHECK(maestro_hw.apu[0][0]==0x402f && maestro_hw.apu[1][0]==0x402f);
        CHECK(maestro_hw.apu[0][10]==0x8f10 && maestro_hw.apu[1][10]==0x8f00);
        CHECK(maestro_hw.apu[0][4]==0x2000 && maestro_hw.apu[1][4]==0x6000);
        CHECK(maestro_hw.wave[0]==0xff2 && maestro_hw.wave[8]==0xff2 && maestro_hw.wave[0x1fc]==d->phys/4096);
        maestro_model_sleep(0,10); CHECK(!audio_backend_progress());
        CHECK(d->state.fetched>=478 && d->state.fetched<=482 && maestro_hw.last_left==1200 && maestro_hw.last_right==(uint16_t)-2400);
        CHECK(backend_irq(11,d->name)); unsigned acks=maestro_hw.irq_acks;
        maestro_hw.ports[0x1a/2]=0x42; CHECK(!backend_irq(11,d->name) && maestro_hw.irq_acks==acks);
        maestro_hw.ports[0x1a/2]=0x46; CHECK(backend_irq(11,d->name) && maestro_hw.ports[0x1a/2]==0x42);
        maestro_hw.ports[0x1a/2]=0;
        CHECK(!audio_backend_master(&(struct ciuki_mixer_master){.mute=1}) && maestro_hw.codec[1]&0x8000);
        CHECK(!audio_backend_master(&(struct ciuki_mixer_master){0}) && !(maestro_hw.codec[1]&0x8000));
        /* Continuous refill across several wraps, without fabricating an LVI. */
        struct audio_frame data[AUDIO_QUANTUM]; memset(data,0,sizeof(data));
        for (unsigned i=0;i<90;i++) {
            maestro_model_sleep(0,7); CHECK(!audio_backend_progress());
            while (d->state.published-d->state.retired<8) CHECK(!audio_backend_publish(data));
        }
        CHECK(d->state.fetched>MAESTRO_FRAMES*3 && !d->state.dma_halts);
        uint32_t token=0;
        unsigned writes=maestro_hw.writes;
        CHECK(maestro_capture_start(d,48000,4,&token)==-EINVAL && maestro_hw.writes==writes);
        CHECK(!maestro_capture_start(d,22050,4,&token) && token);
        CHECK(maestro_hw.apu[2][11]==4 && maestro_hw.apu[3][11]==5 && maestro_hw.apu[4][11]==0x14 && maestro_hw.apu[5][11]==0x15);
        CHECK(maestro_hw.apu[2][0]==0x40bf && maestro_hw.apu[4][0]==0x409f);
        struct audio_frame captured[AUDIO_QUANTUM];
        writes=maestro_hw.writes;
        CHECK(maestro_capture_read(d,token+1,captured,32)==-EIO && maestro_hw.writes==writes && maestro.capture_running);
        maestro_model_sleep(0,4); CHECK(!audio_backend_progress());
        int got=maestro_capture_read(d,token,captured,AUDIO_QUANTUM);
        CHECK(got>=86 && got<=90);
        for (int i=0;i<got;i++) CHECK(captured[i].left==1000+i && captured[i].right==-1000+i);
        CHECK(!maestro_capture_read(d,token,captured,8));
        CHECK(!maestro_capture_stop(d,token));
        CHECK(maestro_capture_read(d,token,captured,8)==-EIO);
        CHECK(d->state.running && (maestro_hw.apu[0][0]&MAESTRO_MODE)==0x20 && (maestro_hw.cfg[1]&4));
        for (unsigned voice=2;voice<6;voice++) CHECK(!(maestro_hw.apu[voice][0]&MAESTRO_MODE));
        CHECK(!audio_backend_stop());
        for (unsigned voice=0;voice<6;voice++) CHECK(!(maestro_hw.apu[voice][0]&MAESTRO_MODE));
        CHECK(!(maestro_hw.cfg[1]&4) && !maestro_hw.ports[0x14/2]);
        audio_backend_shutdown(); CHECK(pages_used==baseline && !audio.commitment_pages && !audio.quarantined);
        CHECK(maestro_hw.codec[1]==0x8202 && maestro_hw.codec[0x26/2]==0x800f);
        CHECK(maestro_hw.codec[0x1a/2]==0x0101 && maestro_hw.codec[0x1c/2]==0x8505);
    }
    /* Firmware sibling/ASSP/secondary link must never be stopped or reset. */
    for (unsigned fault=0;fault<5;fault++) {
        maestro_setup(AUDIO_MAESTRO2E);
        if (!fault) maestro_hw.apu[63][0]=0x402f;
        if (fault==1) maestro_hw.ports[0xa4/2]=1;
        if (fault==2) maestro_hw.ports[0x38/2]=0x20;
        if (fault==3) maestro_hw.index_bad=true;
        if (fault==4) d->command=4;
        CHECK(backend_dma_init(d)<0 && !d->touched && !d->state.ready && pages_used==baseline && !maestro_hw.run_writes);
        CHECK(!maestro_hw.gpio_writes && !maestro_hw.resets && !maestro_hw.irq_acks);
    }
    for (unsigned fault=0;fault<5;fault++) {
        maestro_setup(AUDIO_MAESTRO2E);
        if (!fault) maestro_hw.codec_busy=true;
        if (fault==1) maestro_hw.reject_codec=true;
        if (fault==2) maestro_hw.no_progress=true;
        if (fault==3) maestro_hw.wave_bad=true;
        if (fault==4) maestro_hw.clock=30000;
        CHECK(backend_dma_init(d)<0 && !d->state.ready && pages_used==baseline && !audio.quarantined);
        CHECK(maestro_hw.tick<=1000);
    }
    maestro_setup(AUDIO_MAESTRO2E); second_claim_failure=13;
    CHECK(backend_dma_init(d)==-EBUSY && !maestro_hw.writes && pages_used==baseline);
    /* Underrun immediately gates, then thread stop proves before clearing. */
    maestro_setup(AUDIO_MAESTRO2E); CHECK(!backend_dma_init(d)); maestro_publish_lead();
    maestro_model_sleep(0,45); CHECK(backend_irq(11,d->name) && d->inhibited && d->state.dma_halts==1);
    CHECK(!(maestro_hw.cfg[1]&4) && audio_backend_publish((struct audio_frame[AUDIO_QUANTUM]){{0}})==-EAGAIN);
    CHECK(!audio_backend_stop()); audio_backend_shutdown(); CHECK(pages_used==baseline);
    /* Capture rates, stale generations, exhausted input and missed wraps. */
    const unsigned rates[]={4000,8000,11025,22050,44100,47999};
    for (unsigned i=0;i<ARRAY_SIZE(rates);i++) {
        maestro_setup(AUDIO_MAESTRO2E); CHECK(!backend_dma_init(d)); maestro_publish_lead();
        uint32_t token=0; CHECK(!maestro_capture_start(d,rates[i],i&1 ? 4 : 0,&token));
        uint32_t step=(uint32_t)maestro_hw.apu[2][3]<<8 | maestro_hw.apu[2][2]>>8;
        CHECK(step!=0x10000 && maestro.capture_rate<=rates[i] && rates[i]-maestro.capture_rate<=1);
        struct audio_frame captured[AUDIO_QUANTUM];
        maestro_model_sleep(0,8); CHECK(maestro_capture_read(d,token,captured,32)>0);
        unsigned writes=maestro_hw.writes;
        CHECK(maestro_capture_stop(d,token+1)==-EIO && maestro_hw.writes==writes);
        CHECK(!maestro_capture_stop(d,token)); audio_backend_shutdown(); CHECK(pages_used==baseline);
    }
    for (unsigned fault=0;fault<2;fault++) {
        maestro_setup(AUDIO_MAESTRO2E); CHECK(!backend_dma_init(d)); maestro_publish_lead();
        uint32_t token=0; CHECK(!maestro_capture_start(d,47999,4,&token));
        struct audio_frame captured[AUDIO_QUANTUM];
        maestro_model_sleep(0,fault ? 180 : 166);
        CHECK(maestro_capture_read(d,token,captured,16)==-EIO && d->inhibited);
        CHECK(!audio_backend_stop()); audio_backend_shutdown(); CHECK(pages_used==baseline);
    }
    /* Actual service/mixer/transport path accepts DOS-source PCM and native
     * frames together, then recovers an underrun with no guest I/O access. */
    memset(&audio,0,sizeof(audio)); maestro_setup(AUDIO_MAESTRO2E); CHECK(!backend_dma_init(d));
    audio.ready=true; audio.master.left=audio.master.right=100;
    vm_attached_hook=vm_active_hook=vm_nonzero_hook=true;
    idle_since=non_silent_end=0; audio_service();
    CHECK(audio.running && d->state.published==AUDIO_VM_LEAD);
    struct audio_stream *native=0; CHECK(!audio_stream_open(&native));
    native->started=true;
    struct audio_frame data[AUDIO_QUANTUM]; for (unsigned i=0;i<AUDIO_QUANTUM;i++) data[i]=(struct audio_frame){1234,-1234};
    CHECK(audio_stream_enqueue(native,data,sizeof(data))==sizeof(data));
    maestro_model_sleep(0,6); g_ticks+=6; CHECK(backend_irq(11,d->name)); audio_service();
    unsigned slot=(d->state.published-1)%AUDIO_HW_SLOTS;
    CHECK(d->pcm[slot*AUDIO_QUANTUM].left==13234 && d->pcm[slot*AUDIO_QUANTUM].right==10766);
    maestro_model_sleep(0,55); g_ticks+=55; CHECK(backend_irq(11,d->name) && d->inhibited);
    audio_service(); CHECK(audio.running && !audio.error && !d->inhibited && audio.underruns==1);
    audio_stream_close(native);
    audio_backend_shutdown(); CHECK(pages_used==baseline && !audio.commitment_pages);
    vm_attached_hook=vm_active_hook=vm_nonzero_hook=false; idle_since=non_silent_end=0;
    /* Any uncertain owned APU or BME retains the complete arena and claims. */
    for (unsigned fault=0;fault<2;fault++) {
        maestro_setup(AUDIO_MAESTRO2E); CHECK(!backend_dma_init(d)); maestro_publish_lead();
        uint32_t token; CHECK(!maestro_capture_start(d,47999,0,&token));
        if (!fault) maestro_hw.mode_stuck=true; else maestro_hw.bme_stuck=true;
        unsigned held=pages_used, before=resource_releases;
        if (!fault) CHECK(maestro_capture_stop(d,token)==-EIO && d->state.quarantine && d->inhibited);
        audio_backend_shutdown();
        CHECK(d->state.quarantine && audio.quarantined && pages_used==held && resource_releases==before && resource_quarantines==3);
        uint64_t ticks=maestro_hw.tick; unsigned runs=maestro_hw.run_writes;
        CHECK(audio_backend_init()==-EIO && maestro_hw.tick==ticks);
        audio_backend_shutdown(); CHECK(pages_used==held && maestro_hw.run_writes==runs && !maestro_hw.resets);
        /* Test-fixture disposal only; production never releases quarantine. */
        for (unsigned i=0;i<32;i++) pmm_free(d->phys+i*PAGE_SIZE);
        audio.commitment_pages=0;
    }
    dma_selected=false; memset(d,0,sizeof(*d)); memset(&ac97,0,sizeof(ac97)); memset(&maestro,0,sizeof(maestro));
    audio.backend_name=0; audio.backend_pci=(struct pci_func){0}; audio.backend_touched=false;
    audio.ready=audio.running=audio.quarantined=false; audio.error=0; memset(&audio.caps,0,sizeof(audio.caps));
    CHECK(pages_used==baseline);
    puts("Maestro1/2/2E APU model: clock/rates/stereo/capture/wrap/IRQ/underrun, E500 route+GPIO preservation, stop and retained quarantine PASS");
}
