/* EMU10K1/10K2 independent register/page walker and ACC3 DSP interpreter.
 * No QEMU or physical IO. Model constants intentionally use donor register
 * values rather than the production driver's symbolic definitions. */
static struct {
    struct audio_dma_device *d;
    uint32_t ptr, config_address, command, regs[128][64], code[2048];
    uint32_t hcfg, ipr, inte;
    uint16_t codec[64], iocfg, timer;
    uint8_t codec_address;
    uint64_t tick, output_frames, capture_frames;
    unsigned writes, data_writes, stops, generated;
    int stop_fault;
    bool codec_timeout, codec_reject, no_progress, dead_window, dead_irq;
    struct mm_pin pins[3];
    unsigned pin_count, unpins;
    int32_t last_left, last_right;
} emodel;
int mm_pin(struct mm_pin *p)
{
    if (!emu10k1_kind(audio_dma.kind)) return 0;
    CHECK(emodel.pin_count<3 && p->address_mask==emu10k1_dma_mask(audio_dma.kind));
    CHECK(p->generation && p->pages && p->first_pfn+p->pages<=HOST_PAGES);
    for (unsigned i=0;i<p->pages;i++) CHECK(allocated[p->first_pfn+i]);
    p->ticket=emodel.pin_count+1;
    emodel.pins[emodel.pin_count++]=*p; return 0;
}
int mm_unpin(struct mm_pin *p,uint32_t generation)
{
    if (p==&audio_dma.capture.pin) {
        CHECK(p->ticket==3 && generation==p->generation && !audio_dma.capture.running);
        CHECK(!audio_dma.state.quarantine && !emodel.regs[0x4a][0] && !emodel.regs[0x42][0]);
        emodel.unpins++; return 0;
    }
    CHECK(emu10k1_kind(audio_dma.kind) && p->ticket>=1 && p->ticket<=2);
    CHECK(generation==p->generation && !audio_dma.state.running && !audio_dma.irq_attached);
    CHECK(!audio_dma.state.quarantine && !(emodel.command&4));
    emodel.unpins++; return 0;
}
static uint32_t *emodel_register(void)
{
    unsigned reg=emodel.ptr>>16, voice=emodel.ptr&63;
    if (reg>=0x400) {
        unsigned base=emodel.d->kind==AUDIO_EMU10K2 ? 0x600 : 0x400;
        CHECK(reg>=base && reg-base<2048 && !voice);
        return &emodel.code[reg-base];
    }
    CHECK(reg<128); return &emodel.regs[reg][voice];
}
static uint32_t emodel_read(void *cookie,uint16_t port,unsigned width)
{
    (void)cookie;
    if (port==0xcfc) return emodel.command;
    unsigned off=port-emodel.d->base;
    if (off==4) return emodel.dead_window ? UINT32_MAX : *emodel_register();
    if (off==8) return emodel.dead_irq ? UINT32_MAX : emodel.ipr;
    if (off==12) return emodel.inte;
    if (off==0x14) return emodel.hcfg;
    if (off==0x18) { CHECK(width==2); return emodel.iocfg; }
    if (off==0x1e) return emodel.codec_timeout ? 0 : 0x80|emodel.codec_address;
    if (off==0x1c) { CHECK(width==2); return emodel.codec[emodel.codec_address/2]; }
    CHECK(false); return 0;
}
static void emodel_write(void *cookie,uint16_t port,uint32_t value,unsigned width)
{
    (void)cookie; emodel.writes++;
    if (port==0xcf8) { emodel.config_address=value; return; }
    if (port==0xcfc) {
        CHECK(width==2 && (emodel.config_address&255)==4);
        if (emodel.stop_fault!=2 || (value&4)) emodel.command=value;
        return;
    }
    unsigned off=port-emodel.d->base;
    if (off==0) { CHECK(width==4); emodel.ptr=value; return; }
    if (off==4) {
        emodel.data_writes++;
        unsigned reg=emodel.ptr>>16, voice=emodel.ptr&63;
        if (emodel.stop_fault==1 && voice==0 && reg<=1 && !(value&0xffff0000)) return;
        if (emodel.stop_fault==4 && reg==0x4a && !value) return;
        *emodel_register()=value;
        if (reg==0x4a && value) {
            unsigned idx=emodel.d->kind==AUDIO_EMU10K1 ? 0x64 : 0x63;
            emodel.regs[idx][0]=0;
        }
        return;
    }
    if (off==8) { emodel.ipr&=~value; return; }
    if (off==12) { emodel.inte=value; return; }
    if (off==0x14) { emodel.hcfg=value; return; }
    if (off==0x18) { CHECK(width==2); emodel.iocfg=value; return; }
    if (off==0x1a) { CHECK(width==2); emodel.timer=value; return; }
    if (off==0x1e) { CHECK(width==1 && value<128); emodel.codec_address=value; return; }
    if (off==0x1c) {
        CHECK(width==2);
        if (!emodel.codec_reject) emodel.codec[emodel.codec_address/2]=value|(emodel.codec_address==0x26 ? 15u : 0u);
        return;
    }
    CHECK(false);
}
/* Resolve the physical sample through the actual driver-generated table.
 * Wrong shifts, index bits, ring bounds or unpinned pages fail immediately. */
static struct audio_frame emodel_sample(unsigned frame)
{
    uint32_t ptb=emodel.regs[0x40][0];
    CHECK(ptb && ptb/PAGE_SIZE<HOST_PAGES && allocated[ptb/PAGE_SIZE]);
    unsigned byte=frame*4, page=byte/PAGE_SIZE;
    uint32_t pte=((uint32_t *)P2V(ptb))[page];
    bool live=emodel.d->kind==AUDIO_EMU10K1;
    CHECK((pte&(live ? 0x1fff : 0xfff))==page);
    uint32_t phys=(live ? (pte&0xffffe000u)>>1 : pte&0xfffff000u)+(byte%PAGE_SIZE);
    CHECK(phys/PAGE_SIZE<HOST_PAGES && allocated[phys/PAGE_SIZE]);
    return *(struct audio_frame *)P2V(phys);
}
static void emodel_dsp(struct audio_frame sample)
{
    int32_t r[2048]={0};
    bool a=emodel.d->kind==AUDIO_EMU10K2;
    unsigned shift=a ? 12 : 10, mask=a ? 0x7ff : 0x3ff;
    r[0]=sample.left; r[1]=sample.right;
    r[a ? 0x40 : 0x10]=901; r[a ? 0x41 : 0x11]=-1703;
    for (unsigned i=0;i<(a ? 1024u : 512u);i++) {
        uint32_t low=emodel.code[i*2], high=emodel.code[i*2+1];
        CHECK((high>>(a ? 24 : 20))==6); /* generated ACC3 only */
        unsigned dst=(high>>shift)&mask;
        if (dst!=(a ? 0xc0u : 0x40u)) r[dst]=r[high&mask]+r[(low>>shift)&mask]+r[low&mask];
    }
    emodel.last_left=r[a ? 0x68 : 0x20]; emodel.last_right=r[a ? 0x69 : 0x21];
    CHECK(r[a ? 0x76 : 0x2a]==901 && r[a ? 0x77 : 0x2b]==-1703);
    CHECK(r[a ? 0x6a : 0x24]==0 && r[a ? 0x6b : 0x25]==0);
    if (a) CHECK(r[0x70]==emodel.last_left && r[0x71]==emodel.last_right);
    if (!(emodel.hcfg&1)) emodel.last_left=emodel.last_right=0;
}
static void emodel_advance(unsigned frames)
{
    bool active=(emodel.command&4) && !(emodel.hcfg&8) &&
        (emodel.regs[0][0]>>16)==0x4000 && !emodel.no_progress;
    for (unsigned i=0;i<frames && active;i++) {
        unsigned fetched=(emodel.regs[8][0]+8192-61)%8192;
        emodel_dsp(emodel_sample(fetched));
        emodel.regs[8][0]=(emodel.regs[8][0]+1)%8192;
        emodel.regs[8][1]=emodel.regs[8][0]; emodel.output_frames++;
    }
    if (active && (emodel.inte&4)) emodel.ipr|=0x200;
    if (emodel.stop_fault==3) emodel.regs[8][0]=(emodel.regs[8][0]+1)%8192;
}
static void emodel_capture_advance(unsigned frames)
{
    unsigned idx=emodel.d->kind==AUDIO_EMU10K1 ? 0x64 : 0x63;
    unsigned bytes=emodel.regs[0x4a][0]==0x1b ? 32768 : 8192;
    CHECK((emodel.command&4) && (emodel.regs[0x4a][0]==0x13 || emodel.regs[0x4a][0]==0x1b));
    CHECK(!emodel.regs[emodel.d->kind==AUDIO_EMU10K1 ? 0x52 : 0x53][0]);
    uint32_t phys=emodel.regs[0x46][0];
    for (unsigned i=0;i<frames;i++) {
        unsigned at=emodel.regs[idx][0]/4;
        CHECK(phys/PAGE_SIZE<HOST_PAGES && allocated[(phys+at*4)/PAGE_SIZE]);
        ((struct audio_frame *)P2V(phys))[at]=(struct audio_frame){901+(int)(emodel.capture_frames%100),-1703};
        emodel.regs[idx][0]=(emodel.regs[idx][0]+4)%bytes; emodel.capture_frames++;
    }
    emodel.ipr|=emodel.regs[idx][0]<bytes/2 ? 0x8000 : 0x4000;
}
static uint64_t emodel_now(void *cookie) { (void)cookie; return emodel.tick; }
static void emodel_sleep(void *cookie,uint32_t ms)
{
    (void)cookie; CHECK(host_flags&0x200); emodel.tick+=ms;
    emodel_advance(ms*48);
}
static void emodel_setup(enum audio_backend_kind kind)
{
    struct audio_dma_device *d=&audio_dma;
    memset(&emodel,0,sizeof(emodel)); memset(d,0,sizeof(*d));
    emodel.d=d; emodel.command=1; emodel.codec[0x7c/2]=0x8384; emodel.codec[0x7e/2]=0x7608;
    emodel.codec[0x26/2]=15; emodel.codec[2/2]=0x8000; emodel.codec[0x18/2]=0x8808;
    emodel.codec[0x1a/2]=0x0101; emodel.codec[0x1c/2]=0x8000; emodel.iocfg=0x44;
    d->kind=kind; d->name="emu10k1"; d->base=0x1000; d->bytes=32; d->ops=&emu10k1_ops;
    d->command=1; d->state.irq=11;
    d->pci=(struct pci_func){.vendor=0x1102,.device=kind==AUDIO_EMU10K1 ? 2 : 4,
        .sub_vendor=0x1102,.sub_device=kind==AUDIO_EMU10K1 ? 0x8027 : 0x0051,
        .class_code=4,.subclass=1,.revision=1,.command=1,.irq_line=11,.irq_pin=1,.bar={0x1001}};
    d->io=(struct ac97_io){&emodel,emodel_read,emodel_write,emodel_now,emodel_sleep};
    for (unsigned i=0;i<8;i++) d->claims[i]=-1;
    second_claim_next=12; second_claim_failure=-1;
    audio.error=0; audio.quarantined=false; audio.ready=audio.running=false;
    audio.master=(struct ciuki_mixer_master){.version=1,.size=32,.left=100,.right=100};
    audio.master_in_codec=false; audio.backend_name=d->name; audio.backend_pci=d->pci;
    dma_selected=true;
}
static void emodel_dispose_quarantine(void)
{
    /* Host fixture destruction only, after discarding the fake bus. Production
     * must retain these allocations permanently after an unproved stop. */
    for (unsigned i=0;i<8;i++) pmm_free(audio_dma.phys+i*PAGE_SIZE);
    for (unsigned i=0;i<audio_dma.desc_pages;i++) pmm_free(audio_dma.desc_phys+i*PAGE_SIZE);
    audio.commitment_pages=0;
    memset(backend_pins,0,sizeof(backend_pins));
}
static void test_emu10k1(void)
{
    unsigned baseline=pages_used;
    static struct audio_frame marker[256], captured[1024];
    for (unsigned i=0;i<256;i++) marker[i]=(struct audio_frame){1234,-2345};
    for (unsigned chip=0;chip<2;chip++) {
        enum audio_backend_kind kind=chip ? AUDIO_EMU10K2 : AUDIO_EMU10K1;
        emodel_setup(kind); CHECK(!backend_dma_init(&audio_dma));
        struct audio_dma_device *d=&audio_dma;
        CHECK(d->state.ready && !d->state.running && !(emodel.command&4));
        CHECK(pages_used-baseline==(chip ? 15u : 19u) && audio.commitment_pages==pages_used-baseline);
        CHECK(emodel.pin_count==2 && emodel.pins[0].pages==8 && emodel.pins[0].direction==MM_PIN_TO_DEVICE);
        CHECK(emodel.pins[1].pages==(chip ? 7u : 11u) && emodel.pins[1].direction==MM_PIN_BIDIRECTIONAL);
        CHECK(audio.caps.backend==(chip ? 11u : 10u) && audio.caps.output_rate==48000);
        unsigned entries=chip ? 4096 : 8192, shift=chip ? 0 : 1;
        uint32_t *pt=(uint32_t *)d->desc;
        uint32_t silent=d->desc_phys+(chip ? 4 : 8)*PAGE_SIZE;
        for (unsigned i=0;i<entries;i++) CHECK(pt[i]==(((i<8 ? d->phys+i*PAGE_SIZE : silent)<<shift)|i));
        CHECK(!(emodel.inte&0xfffff000) && !(emodel.hcfg&0x200)); /* no legacy/game port */
        CHECK(emodel.last_left==0 && emodel.last_right==0); /* activation proof is silent */
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start());
        for (unsigned i=1;i<=96;i++) {
            emodel_advance(256); emodel.tick+=5;
            CHECK(backend_irq(11,"emu10k1") && !backend_irq(11,"emu10k1"));
            CHECK(d->state.retired==i && d->state.generation_fetched==i*256u);
            CHECK(emodel.last_left==1234 && emodel.last_right==-2345);
            CHECK(!audio_backend_publish(marker));
        }
        CHECK(!audio_backend_master(&(struct ciuki_mixer_master){.mute=1}));
        CHECK(!(emodel.hcfg&1) && (emodel.codec[1]&0x8000));
        emodel_advance(8); emodel.tick++;
        CHECK(!emodel.last_left && !emodel.last_right);
        CHECK(!audio_backend_master(&(struct ciuki_mixer_master){0})); CHECK(emodel.hcfg&1);
        /* An unrelated MIDI cause is never acknowledged by this PCM owner. */
        emodel.ipr=0x80; CHECK(!backend_irq(11,"emu10k1") && emodel.ipr==0x80);
        emodel.ipr=0;
        CHECK(!audio_backend_stop() && !(emodel.command&4));
        CHECK(!d->state.running && !d->state.published && !d->pcm[0].left);
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start());
        /* Actual production mixer supplies both native PCM and the existing
         * virtual SB16 source hook, without any physical SB port operations. */
        struct audio_stream *stream; audio.ready=true;
        CHECK(!audio_stream_open(&stream));
        struct ciuki_audio_format format={.format=CIUKI_AUDIO_S16LE,.channels=2,.rate=48000};
        CHECK(!audio_stream_format(stream,&format));
        CHECK(audio_stream_enqueue(stream,marker,sizeof(marker))==sizeof(marker));
        vm_active_hook=vm_nonzero_hook=vm_attached_hook=true;
        audio_mix(marker,256); CHECK(marker[0].left==13234 && marker[0].right==9655);
        vm_active_hook=vm_nonzero_hook=vm_attached_hook=false; audio_stream_close(stream); audio.ready=false;
        for (unsigned i=0;i<256;i++) marker[i]=(struct audio_frame){1234,-2345};
        static const unsigned rates[]={8000,11025,16000,22050,24000,32000,44100,48000,12000};
        for (unsigned r=0;r<9;r++) {
            if (!chip && rates[r]==12000) {
                CHECK(emu10k1_capture_start(d,rates[r])==-EINVAL); continue;
            }
            CHECK(!emu10k1_capture_start(d,rates[r]));
            unsigned code=r==8 ? 6 : r==0 ? (chip ? 8 : 7) : r==1 ? (chip ? 7 : 6) : 7-r;
            CHECK(emodel.regs[0x42][0]==((chip ? 0x30u : 0x18u)|code));
            CHECK(emu10k1_capture_start(d,rates[r])==-EBUSY);
            CHECK(emu10k1_capture_read(d,captured,1024)==-EAGAIN);
            emodel_capture_advance(1024); emodel.tick+=1024000/rates[r];
            CHECK(emu10k1_capture_read(d,captured,1024)==1024);
            for (unsigned i=0;i<1024;i++) CHECK(captured[i].left==901+(int)(i%100) && captured[i].right==-1703);
            emodel_capture_advance(1024); emodel.tick+=1024000/rates[r];
            CHECK(emu10k1_capture_read(d,captured,1024)==1024);
            for (unsigned i=0;i<1024;i++) CHECK(captured[i].left==901+(int)((i+1024)%100) && captured[i].right==-1703);
            CHECK(!emu10k1_capture_stop(d) && !d->emu.capture_running && !(emodel.command&4));
            emodel.capture_frames=0;
        }
        CHECK(emu10k1_capture_start(d,12345)==-EINVAL);
        CHECK(!emu10k1_capture_start(d,48000));
        emodel_capture_advance(2048); emodel.tick+=43;
        CHECK(emu10k1_capture_read(d,captured,1024)==-EIO && d->emu.capture_failed);
        CHECK(!emu10k1_capture_stop(d));
        unsigned sleep_pages=pages_used; uint32_t sleep_phys=d->phys;
        CHECK(!audio_backend_suspend() && !audio_backend_resume());
        CHECK(d->state.ready && d->phys==sleep_phys && pages_used==sleep_pages);
        audio_backend_shutdown(); CHECK(!audio.quarantined && pages_used==baseline && !audio.commitment_pages);
        CHECK(emodel.unpins==2);
        CHECK(emodel.codec[0x1a/2]==0x0101 && emodel.codec[0x1c/2]==0x8000);
        if (chip) CHECK(emodel.iocfg==0x44);
        /* Runtime exhaustion and missed modulo wraps cannot retire or reuse
         * an unprepared period. A proved stop is required for recovery. */
        emodel_setup(kind); CHECK(!backend_dma_init(d));
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start());
        struct audio_frame canary=d->pcm[0];
        uint64_t frontier=d->state.published; d->state.published=0;
        CHECK(audio_backend_publish(marker)==-EIO && !memcmp(&canary,d->pcm,sizeof(canary)));
        d->state.published=frontier;
        emodel_advance(9*256); emodel.tick+=48;
        CHECK(backend_irq(11,"emu10k1") && d->inhibited && !(emodel.command&4));
        CHECK(!d->state.retired && audio_backend_start()==-EIO);
        CHECK(!audio_backend_stop() && !d->inhibited);
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start()); emodel.tick+=171;
        CHECK(audio_backend_progress()==-EIO && !d->state.retired);
        audio_backend_shutdown(); CHECK(!audio.quarantined && pages_used==baseline);
        emodel_setup(kind); CHECK(!backend_dma_init(d));
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start()); emodel.ipr=0x200000;
        CHECK(backend_irq(11,"emu10k1") && d->inhibited && d->state.dma_halts==1);
        audio_backend_shutdown(); CHECK(!audio.quarantined && pages_used==baseline);
        emodel_setup(kind); CHECK(!backend_dma_init(d));
        emodel.dead_irq=true;
        CHECK(!backend_irq(11,"emu10k1") && d->inhibited);
        emodel.dead_irq=false;
        audio_backend_shutdown(); CHECK(!audio.quarantined && pages_used==baseline);
        for (unsigned fault=1;fault<=4;fault++) {
            emodel_setup(kind); CHECK(!backend_dma_init(d));
            for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
            CHECK(!audio_backend_start());
            if (fault==4) CHECK(!emu10k1_capture_start(d,48000));
            emodel.stop_fault=fault; unsigned held=pages_used, releases=resource_releases;
            audio_backend_shutdown(); CHECK(d->state.quarantine && audio.quarantined && pages_used==held);
            CHECK(!emodel.unpins);
            CHECK(audio_backend_init()==-EIO && pages_used==held);
            CHECK(resource_releases==releases);
            for (unsigned i=0;i<8;i++) if (d->claims[i]>=0) CHECK(registry_get(d->claims[i])->state==RS_QUARANTINED);
            emodel.stop_fault=0;
            CHECK(audio_backend_publish(marker)==-EAGAIN && audio_backend_start()==-EIO);
            CHECK(emu10k1_capture_start(d,48000)==-EIO);
            audio_backend_shutdown(); CHECK(pages_used==held && resource_releases==releases);
            emodel_dispose_quarantine(); CHECK(pages_used==baseline);
        }
    }
    /* All allocation and claim failures roll back without a DMA start. */
    for (unsigned fail=0;fail<2;fail++) {
        emodel_setup(AUDIO_EMU10K1); alloc_fail=fail;
        CHECK(backend_dma_init(&audio_dma)==-ENOMEM); alloc_fail=-1;
        CHECK(pages_used==baseline && !emodel.output_frames && !audio.quarantined);
    }
    for (unsigned claim=12;claim<=15;claim++) {
        emodel_setup(AUDIO_EMU10K1); second_claim_failure=claim;
        CHECK(backend_dma_init(&audio_dma)==-EBUSY && pages_used==baseline && !emodel.output_frames);
    }
    for (unsigned fail=0;fail<5;fail++) {
        emodel_setup(AUDIO_EMU10K1);
        if (fail==0) emodel.codec_timeout=true;
        if (fail==1) emodel.codec_reject=true;
        if (fail==2) emodel.no_progress=true;
        if (fail==3) emodel.regs[0][7]=0x40000000;
        if (fail==4) emodel.regs[0x4b][0]=1;
        int e=backend_dma_init(&audio_dma); CHECK(e<0 && !audio_dma.state.ready);
        CHECK(!audio.quarantined);
        CHECK(pages_used==baseline);
        if (fail>=3) CHECK(!emodel.data_writes && !audio_dma.touched);
    }
    for (unsigned variant=0;variant<4;variant++) {
        emodel_setup(AUDIO_EMU10K2);
        if (variant<2) audio_dma.pci.sub_device=0x4001+variant;
        if (variant==2) audio_dma.pci.revision=4;
        if (variant==3) audio_dma.pci.sub_device=0x2001;
        CHECK(backend_dma_init(&audio_dma)==-ENODEV && !emodel.writes && pages_used==baseline);
        CHECK(strstr(audio.reason,"excluded"));
    }
    enum audio_backend_kind detected=AUDIO_ICH;
    struct pci_func p={.vendor=0x1102,.device=7,.class_code=4,.subclass=1};
    CHECK(!emu10k1_match(&p,&detected)); p.device=8; CHECK(!emu10k1_match(&p,&detected));
    p.device=2; CHECK(emu10k1_match(&p,&detected) && detected==AUDIO_EMU10K1);
    p.device=4; CHECK(emu10k1_match(&p,&detected) && detected==AUDIO_EMU10K2);
    p.subclass=2; CHECK(!emu10k1_match(&p,&detected));
    for (unsigned chip=0;chip<2;chip++) {
        emodel_setup(chip ? AUDIO_EMU10K2 : AUDIO_EMU10K1);
        struct audio_dma_device *d=&audio_dma;
        d->phys=chip ? 0xfffff000u : 0x80000000u; d->desc_phys=0x100000;
        d->desc_pages=emu10k1_aux_pages(d->kind); d->pcm=(void *)1; d->desc=(void *)1;
        CHECK(d->ops->prepare(d)==-EINVAL && !emodel.writes && !d->touched);
        d->phys=0x100001;
        CHECK(d->ops->prepare(d)==-EINVAL && !emodel.writes);
    }
    emodel_setup(AUDIO_EMU10K1); second_pci_count=1; second_pci[0]=audio_dma.pci;
    CHECK(audio_backend_init()==-ENODEV && !strcmp(audio.reason,"routing_unqualified"));
    CHECK(audio.backend_pci.vendor==0x1102 && !strcmp(audio.backend_name,"emu10k1"));
    CHECK(!dma_selected && !audio_dma.touched && pages_used==baseline); second_pci_count=0;
    dma_selected=false; memset(&audio_dma,0,sizeof(audio_dma)); memset(&ac97,0,sizeof(ac97));
    audio.backend_name=0; audio.backend_pci=(struct pci_func){0};
    audio.ready=audio.running=audio.quarantined=false; audio.error=0;
    CHECK(pages_used==baseline && !audio.commitment_pages);
    printf("EMU private state bytes: device=%zu, capture=%zu; no quota changes\n",sizeof(audio_dma),sizeof(audio_dma.emu));
    puts("EMU10K1/10K2: PTE walker, stereo DSP output, 3 wraps, native+virtual SB16 mix, ADC rates/copies/overrun, own IRQs, rollback and four stop-quarantine faults PASS");
}

static void test_emu10k1_public(void)
{
    unsigned baseline=pages_used;
    for (unsigned chip=0;chip<2;chip++) {
        emodel_setup(chip ? AUDIO_EMU10K2 : AUDIO_EMU10K1);
        struct audio_dma_device *d=&audio_dma;
        CHECK(!backend_dma_init(d)); audio.ready=true;
        uint32_t token=0;
        CHECK(!emu10k1_capture_start(d,48000));
        CHECK(audio_capture_open(d,&token)==-EBUSY && d->emu.capture_running);
        CHECK(!emu10k1_capture_stop(d));
        uint16_t select=emodel.codec[0x1a/2],gain=emodel.codec[0x1c/2];
        CHECK(!audio_capture_open(d,&token) && token && emodel.regs[0x4a][0]==0x1b);
        CHECK(emu10k1_capture_start(d,48000)==-EBUSY);
        CHECK(emu10k1_capture_stop(d)==-EBUSY && d->capture.running);
        CHECK(!audio_capture_source(d,token,CIUKI_CAPTURE_MIC) && !emodel.codec[0x1a/2]);
        CHECK(!audio_capture_source(d,token,CIUKI_CAPTURE_CD) && emodel.codec[0x1a/2]==0x0101);
        CHECK(!audio_capture_source(d,token,CIUKI_CAPTURE_LINE));
        static struct audio_frame frames[128],marker[AUDIO_QUANTUM];
        for (unsigned i=0;i<8;i++) CHECK(!audio_dma_publish(d,marker));
        CHECK(!audio_dma_start(d));
        CHECK(audio_capture_read(d,token,frames,128)==-EAGAIN);
        emodel_capture_advance(600); emodel.tick+=12;
        CHECK(audio_capture_read(d,token,frames,128)==128);
        for (unsigned i=0;i<128;i++) CHECK(frames[i].left==901+(int)(i%100) && frames[i].right==-1703);
        uint32_t pitch=emodel.regs[0][0],timer=emodel.inte,hcfg=emodel.hcfg,command=emodel.command;
        CHECK(!audio_capture_close(d,token));
        CHECK(d->state.running && emodel.regs[0][0]==pitch && emodel.inte==timer && emodel.hcfg==hcfg && emodel.command==command);
        CHECK(emodel.codec[0x1a/2]==select && emodel.codec[0x1c/2]==gain);
        CHECK(emodel.unpins==1 && emodel.pin_count==3);
        audio_backend_shutdown(); CHECK(pages_used==baseline && emodel.unpins==3);
    }
    puts("EMU10K1/10K2 public capture: independent ADC start/stop/copy/source/restore/pins/playback preservation PASS");
}
