/* DS-XG register/DMA-bank model. Numeric bus protocol is independent of the
 * driver's register names; actual production backend/mixer code is exercised. */
struct ymf_model {
    uint32_t regs[0x8000 / 4], cfg[64], address;
    uint16_t codec[64];
    uint64_t tick;
    unsigned frame, selected, starts, uploads, writes, codec_reads, resets, maps, revokes;
    unsigned rendered, samples_checked, stop_irqs;
    unsigned pin_calls, unpin_calls, pinned_mask;
    int pin_failure;
    int16_t expected_left, expected_right;
    bool active, no_progress, stop_stuck, bank_race, busy, reject_codec;
    bool map_failure, base_reject, bme_stuck, gone, irq_during_stop;
    int corrupt_ram;
};
static struct ymf_model ym;
int mm_pin(struct mm_pin *pin)
{
    if (audio_dma.kind==AUDIO_YMFPCI) {
        unsigned at=ym.pin_calls++;
        CHECK(at<2 && pin==&backend_pins[at] && !pin->ticket);
        CHECK(pin->address_mask==UINT32_MAX && pin->generation==audio_dma.generations[1]);
        CHECK(pin->pages==(at ? 1 : 8) && pin->alignment==(at ? 256 : 4));
        CHECK(pin->direction==(at ? MM_PIN_BIDIRECTIONAL : MM_PIN_TO_DEVICE));
        if (ym.pin_failure==(int)at) return -ENOMEM;
        ym.pinned_mask|=1u<<at;
    }
    pin->ticket=1; return 0;
}
int mm_unpin(struct mm_pin *pin,uint32_t generation)
{
    CHECK(pin->ticket && pin->generation==generation);
    if (audio_dma.kind==AUDIO_YMFPCI) {
        unsigned at=pin==&backend_pins[0] ? 0 : 1;
        CHECK(!ym.active && !audio_dma.state.quarantine && (ym.pinned_mask&(1u<<at)));
        ym.pinned_mask&=~(1u<<at); ym.unpin_calls++;
    }
    pin->ticket=0; return 0;
}
void *vmm_map_mmio(uint32_t phys, uint32_t bytes, bool uncached)
{
    CHECK(phys==0xe1000000 && bytes==32768 && uncached);
    CHECK(audio_resources[audio_dma.claims[0]].state==RS_ACTIVE);
    ym.maps++; return ym.map_failure ? 0 : ym.regs;
}
void vmm_revoke_mmio(void *ptr, uint32_t bytes)
{ CHECK(ptr==ym.regs && bytes==32768 && !ym.active); ym.revokes++; }
static uint32_t ym_io_read(void *cookie,uint16_t port,unsigned width)
{
    struct ymf_model *h=cookie;
    if (port==0x4d0 || port==0x4d1) return 255;
    CHECK(port==0xcfc && width==4);
    return h->cfg[(h->address&255)/4];
}
static void ym_io_write(void *cookie,uint16_t port,uint32_t value,unsigned width)
{
    struct ymf_model *h=cookie;
    if (port==0xcf8) { CHECK(width==4); h->address=value; return; }
    CHECK(port>=0xcfc && port<=0xcff);
    unsigned off=(h->address&255)+port-0xcfc, shift=(off&3)*8;
    uint32_t mask=width==4 ? UINT32_MAX : ((1u<<(width*8))-1)<<shift;
    if (off==4 && h->bme_stuck) value|=4;
    if (off==0x48 && (value&3)==3) h->resets++;
    h->cfg[off/4]=(h->cfg[off/4]&~mask)|(value<<shift);
}
static uint32_t ym_read(void *cookie,unsigned off,unsigned width)
{
    struct ymf_model *h=cookie;
    CHECK(off<32768 && !(off&(width-1)) && (width==2 || width==4));
    if (h->gone) return width==2 ? 0xffff : UINT32_MAX;
    if (off==0x66) return h->busy ? 0x8000 : h->regs[0x64/4]>>16;
    if (off==0x64) { h->codec_reads++; return h->regs[0x64/4]&0xffff; }
    if (off==0x104) {
        if (h->bank_race) h->selected^=1;
        return h->selected;
    }
    if ((int)off==h->corrupt_ram) return h->regs[off/4]^1;
    uint32_t value=h->regs[off/4]>>((off&3)*8);
    return width==2 ? value&0xffff : value;
}
static void ym_write(void *cookie,unsigned off,uint32_t value,unsigned width)
{
    struct ymf_model *h=cookie;
    CHECK(off<32768 && !(off&(width-1))); h->writes++;
    CHECK(audio_dma.claims[0]>=0 && audio_dma.claims[1]>=0 && audio_dma.claims[4]>=0 && audio_dma.claims[5]>=0);
    if (off>=0x1000) {
        CHECK(width==4 && !h->active && !(h->cfg[1]&4) && !h->regs[0x114/4]);
        CHECK((off>=0x1000 && off<0x1080) || (off>=0x4000 && off<0x7000));
        h->uploads++;
    }
    if (off==0x60) {
        CHECK(width==4); unsigned reg=value>>16;
        if (!h->reject_codec) h->codec[reg/2]=(value&0xffff) | (reg==0x26 ? 15 : 0);
        return;
    }
    if (off==0x62) {
        CHECK(width==2 && (value&0x8000)); unsigned reg=value&0x7f;
        h->regs[0x64/4]=(uint32_t)reg<<16 | h->codec[reg/2]; return;
    }
    if (off==0x100) { CHECK(width==4 && value==0x80000000); h->regs[off/4]&=~value; return; }
    if (off==0x108 && value==3) {
        CHECK(h->regs[0x114/4]==1 && (h->cfg[1]&6)==6);
        CHECK(h->regs[0x158/4]==audio_dma.desc_phys && h->regs[0x164/4]==audio_dma.desc_phys+0x600);
        CHECK(h->regs[0x14c/4]==256 && !h->regs[0x150/4] && !h->regs[0x154/4]);
        if (!h->active) {
            h->starts++;
            const uint32_t *table=P2V(h->regs[0x158/4]);
            const uint32_t *bank=P2V(table[1]); h->frame=bank[18];
        }
        h->active=true; h->regs[0x100/4]|=2;
    }
    if (off==0x108 && !value && !h->stop_stuck) { h->active=false; h->regs[0x100/4]&=~2u; }
    if (off==0x114 && !value && h->stop_stuck) return;
    if (off==0x158 && h->base_reject && value) return;
    if (width==2) {
        unsigned shift=(off&3)*8;
        h->regs[off/4]=(h->regs[off/4]&~(0xffffu<<shift))|(value<<shift);
    } else { CHECK(width==4); h->regs[off/4]=value; }
}
static uint64_t ym_now(void *cookie) { return ((struct ymf_model *)cookie)->tick; }
static void ym_step(unsigned frames)
{
    if (!ym.active || ym.no_progress) return;
    CHECK((ym.cfg[1]&4) && ym.regs[0x114/4]==1);
    const uint32_t *table=P2V(ym.regs[0x158/4]);
    CHECK(table[0]==64 && table[1] && table[2]);
    for (unsigned i=3;i<=64;i++) CHECK(!table[i]);
    uint32_t *banks[2];
    unsigned next=ym.selected^1;
    for (unsigned voice=0;voice<2;voice++) {
        banks[voice]=(uint32_t *)P2V(table[voice+1])+next*30;
        const uint32_t *b=banks[voice];
        CHECK(b[0]==(0x10000u|voice) && b[2]==audio_dma.phys && b[4]==8192);
        CHECK(b[6]==0x10000000 && b[20]==0x10000000 && b[7]==0x40000000 && b[21]==0x40000000);
        CHECK(b[8]==0x40000000 && b[22]==0x40000000 && b[14]==0x40000000);
        CHECK(b[9+voice]==0x40000000 && b[23+voice]==0x40000000);
        CHECK(!b[10-voice] && !b[24-voice] && !b[11] && !b[12] && !b[13]);
    }
    for (unsigned i=0;i<frames;i++) {
        const struct audio_frame *pcm=P2V(banks[0][2]);
        struct audio_frame sample=pcm[(ym.frame+i)%8192];
        if (!audio_dma.proving && ym.rendered<2048) {
            CHECK(sample.left==ym.expected_left && sample.right==ym.expected_right); ym.samples_checked++;
        }
        ym.rendered++;
    }
    ym.frame=(ym.frame+frames)%8192;
    banks[0][18]=banks[1][18]=ym.frame;
    ym.selected=next; ym.regs[0x100/4]|=0x80000000;
}
static void ym_sleep(void *cookie,uint32_t ms)
{
    struct ymf_model *h=cookie; h->tick+=ms;
    if (h->active) ym_step(ms*48);
    if (h->irq_during_stop && audio_dma.inhibited && audio_dma.irq_attached) {
        h->regs[0x100/4]|=0x80000000; h->stop_irqs++;
        CHECK(backend_irq(11,"ymfpci") && !(h->regs[0x100/4]&0x80000000));
        CHECK(!h->active); /* a completion during stop must never rearm MODE */
    }
}
static void ym_setup(unsigned id,unsigned revision)
{
    memset(&ym,0,sizeof(ym)); ym.corrupt_ram=-1; ym.tick=g_ticks;
    ym.pin_failure=-1; memset(backend_pins,0,sizeof(backend_pins));
    ym.expected_left=1234; ym.expected_right=-2345;
    memset(&audio_dma,0,sizeof(audio_dma)); memset(&ac97,0,sizeof(ac97));
    struct audio_dma_device *d=&audio_dma;
    d->kind=AUDIO_YMFPCI; d->name="ymfpci"; d->ops=&ymfpci_ops;
    d->pci=(struct pci_func){.vendor=0x1073,.device=id,.revision=revision,.class_code=4,.subclass=1,
        .irq_line=11,.irq_pin=1,.command=2,.bar={0xe1000000}};
    d->command=2; d->bytes=32768; d->state.irq=11; d->mmio_phys=0xe1000000;
    d->io=(struct ac97_io){&ym,ym_io_read,ym_io_write,ym_now,ym_sleep};
    d->mmio_read=ym_read; d->mmio_write=ym_write;
    for (unsigned i=0;i<8;i++) d->claims[i]=-1;
    ym.cfg[1]=2; ym.cfg[0x40/4]=0x907f; ym.codec[0x7c/2]=0x594d; ym.codec[0x7e/2]=0x4800;
    ym.codec[0x26/2]=15; ym.codec[2/2]=0x8000; ym.codec[0x18/2]=0x8808;
    ym.regs[0x140/4]=30; ym.regs[0x144/4]=ym.regs[0x148/4]=4;
    second_claim_next=12; second_claim_failure=-1; second_irq_adds=0; second_pci_count=0;
    audio.error=audio.init_error=0; audio.ready=audio.running=audio.quarantined=false;
    audio.master=(struct ciuki_mixer_master){.version=1,.size=32,.left=100,.right=100};
    audio.master_in_hardware=audio.master_in_codec=false;
    audio.backend_name=d->name; audio.backend_pci=d->pci; audio.backend_touched=false;
    dma_selected=true;
}
static void ym_dispose_quarantine(void)
{
    /* Fixture disposal after asserting permanent retention, never production. */
    for (unsigned i=0;i<8;i++) pmm_free(audio_dma.phys+i*PAGE_SIZE);
    pmm_free(audio_dma.desc_phys); audio.commitment_pages=0;
    memset(backend_pins,0,sizeof(backend_pins));
}
static void test_ymfpci(void)
{
    unsigned baseline=pages_used;
    static const unsigned ids[]={4,10,13,12,16,18};
    static struct audio_frame marker[256];
    for (unsigned i=0;i<256;i++) marker[i]=(struct audio_frame){1234,-2345};
    for (unsigned k=0;k<6;k++) {
        ym_setup(ids[k],k==4 ? 1 : 3);
        enum audio_backend_kind kind=0;
        CHECK(ymfpci_match(&audio_dma.pci,&kind) && kind==AUDIO_YMFPCI);
        CHECK(!backend_dma_init(&audio_dma));
        CHECK(audio_dma.state.ready && !audio_dma.state.running && ym.starts==1 && ym.uploads==3104);
        CHECK(ym.maps==1 && !ym.revokes && pages_used==baseline+9 && audio.commitment_pages==9);
        CHECK(ym.pin_calls==2 && ym.pinned_mask==3 && !ym.unpin_calls);
        CHECK(audio.caps.backend==13 && audio.caps.output_rate==48000 && audio.caps.codec_id==0x594d4800);
        CHECK(!ym.regs[0x158/4] && !ym.regs[0x164/4] && !(ym.cfg[1]&4));
        const uint32_t *firmware=k<2 ? CntrlInst : CntrlInst1E;
        CHECK(!memcmp(ym.regs+0x4000/4,firmware,12288) && !memcmp(ym.regs+0x1000/4,DspInst,128));
        CHECK((ym.cfg[0x40/4]&0x801f)==0x8000 && !ym.regs[0x18/4] && !ym.regs[0x34/4]);
        CHECK(k!=4 || ym.codec_reads>=600);
        CHECK(!audio_backend_master(&(struct ciuki_mixer_master){.mute=1}));
        CHECK(!ym.regs[0x84/4] && (ym.codec[1]&0x8000) && (ym.codec[12]&0x8000));
        CHECK(!audio_backend_master(&(struct ciuki_mixer_master){0}));
        CHECK(ym.regs[0x84/4]==0x3fff3fff && !ym.codec[1] && ym.codec[12]==0x0808);
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        ym.rendered=0; ym.frame=0;
        CHECK(!audio_backend_start() && ym.starts==2);
        for (unsigned i=1;i<=96;i++) {
            ym_step(256); ym.tick+=5;
            CHECK(backend_irq(11,"ymfpci") && !backend_irq(11,"ymfpci"));
            CHECK(audio_dma.state.retired==i && audio_dma.state.fetched==(uint64_t)i*256);
            CHECK(!audio_backend_publish(marker));
        }
        CHECK(ym.samples_checked==2048);
        /* Bank handoff cannot fabricate a retirement or rewrite the current slot. */
        uint64_t fetched=audio_dma.state.fetched;
        ym.bank_race=true; CHECK(audio_backend_progress()==-EAGAIN && audio_dma.state.fetched==fetched);
        ym.bank_race=false;
        uint64_t pub=audio_dma.state.published; audio_dma.state.published=audio_dma.state.retired;
        CHECK(audio_backend_publish(marker)==-EIO); audio_dma.state.published=pub;
        ym.irq_during_stop=true;
        CHECK(!audio_backend_stop() && !ym.active && !ym.regs[0x114/4] && !(ym.cfg[1]&4));
        CHECK(ym.stop_irqs && !audio_dma.state.published && !audio_dma.pcm[8191].right);
        audio_backend_shutdown();
        CHECK(!audio.quarantined && pages_used==baseline && !audio.commitment_pages && ym.revokes==1);
        CHECK(!ym.pinned_mask && ym.unpin_calls==2);
        CHECK(ym.codec[1]==0x8000 && ym.codec[12]==0x8808 && (ym.cfg[0x40/4]&0xffff)==0x907f);
        unsigned writes=ym.writes; audio_backend_shutdown();
        CHECK(ym.writes==writes && ym.revokes==1 && pages_used==baseline);
    }
    puts("Yamaha DS-XG: six IDs, exact DS-1/DS-1E upload, stereo banks, wrap, IRQ, mute, stop/release PASS");
    /* Native and DOS sources meet at the production mixer. Neither changes
     * the DSP's 48 kHz source clock or receives access to physical SB ports. */
    ym_setup(18,3); CHECK(!backend_dma_init(&audio_dma)); audio.ready=true;
    audio.master.left=50; audio.master.right=25;
    vm_attached_hook=vm_active_hook=vm_nonzero_hook=true;
    struct audio_stream *stream; CHECK(!audio_stream_open(&stream));
    for (unsigned i=0;i<8;i++) CHECK(audio_stream_enqueue(stream,marker,sizeof(marker))==sizeof(marker));
    static struct audio_frame mixed[256];
    for (unsigned period=0;period<8;period++) {
        audio_mix(mixed,256);
        for (unsigned i=0;i<256;i++) CHECK(mixed[i].left==6617 && mixed[i].right==2413);
        CHECK(!audio_backend_publish(mixed));
    }
    ym.expected_left=6617; ym.expected_right=2413; ym.rendered=ym.frame=0;
    CHECK(!audio_backend_start()); ym_step(256); ym.tick+=6;
    CHECK(!audio_backend_progress() && ym.samples_checked==256);
    CHECK(stream->consumed==2048 && stream->rate==48000 && !stream->queued);
    CHECK(!audio_backend_master(&(struct ciuki_mixer_master){.mute=1}));
    audio.master.mute=1; audio_mix(mixed,256);
    for (unsigned i=0;i<256;i++) CHECK(!mixed[i].left && !mixed[i].right);
    audio_stream_close(stream); vm_attached_hook=vm_active_hook=vm_nonzero_hook=false;
    audio_backend_shutdown(); CHECK(!audio.quarantined && pages_used==baseline && !audio.commitment_pages);
    puts("Yamaha DS-XG: concurrent native + virtual DOS source, single software master, copied DMA and explicit mute PASS");
    ym_setup(4,3); CHECK(!backend_dma_init(&audio_dma)); ym.rendered=2048;
    for (unsigned cycle=0;cycle<100;cycle++) {
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start()); ym_step(256); ym.tick+=6;
        CHECK(!audio_backend_progress() && audio_dma.state.retired==1);
        CHECK(!audio_backend_stop() && !ym.active && !audio_dma.inhibited);
    }
    audio_backend_shutdown(); CHECK(pages_used==baseline && !audio.quarantined);
    puts("Yamaha DS-XG: 100 start/play/stop cycles and idempotent clean teardown PASS");
    for (unsigned fault=0;fault<4;fault++) {
        ym_setup(4,3); CHECK(!backend_dma_init(&audio_dma));
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        ym.rendered=2048; CHECK(!audio_backend_start());
        if (fault==0) { ym_step(9*256); ym.tick+=48; }
        if (fault==1) {
            volatile uint32_t *b=ymf_bank(&audio_dma,0,ym.selected); b[18]=8192;
            ym.regs[0x100/4]|=0x80000000;
        }
        if (fault==2) { ym.tick+=171; ym.regs[0x100/4]|=0x80000000; }
        if (fault==3) {
            ym.bank_race=true;
            CHECK(audio_backend_progress()==-EAGAIN);
            ym.tick+=3; ym.regs[0x100/4]|=0x80000000;
        }
        uint64_t fetched=audio_dma.state.fetched;
        CHECK(backend_irq(11,"ymfpci") && audio_dma.inhibited && !ym.active);
        CHECK(audio_dma.state.fetched==fetched && audio_backend_publish(marker)==-EAGAIN);
        ym.bank_race=false; audio_backend_shutdown();
        CHECK(!audio.quarantined && pages_used==baseline && !ym.pinned_mask);
    }
    puts("Yamaha DS-XG: overrun, invalid bank position, missed wrap and bounded bank retry inhibit without fabricated retirement PASS");
    /* Unsupported identities and resource metadata must never touch hardware. */
    ym_setup(0x20,0); enum audio_backend_kind kind=0;
    CHECK(!ymfpci_match(&audio_dma.pci,&kind));
    for (unsigned i=0;i<8;i++) {
        ym_setup(4,0);
        static const uint32_t invalid[]={0,UINT32_MAX,0xe1000001,0xe1000004,0xe1000008,0xe1001000};
        if (i<6) audio_dma.pci.bar[0]=invalid[i];
        if (i==6) audio_dma.pci.header_type=1;
        if (i==7) audio_dma.command=4;
        CHECK(ymfpci_resources(&audio_dma)==-ENODEV && !ym.writes && !ym.maps);
    }
    ym_setup(4,0); CHECK(ymfpci_resources(&audio_dma)==-ENODEV && !strcmp(audio_dma.reason,"routing_unqualified"));
    uint32_t boot_flags=g_boot.flags; g_boot.flags&=~CBI_F_SMBIOS_QEMU;
    uint8_t *pir=P2V(0xf0000); memset(pir,0,48); memcpy(pir,"$PIR",4);
    pir[5]=1; pir[6]=48; pir[9]=8; pir[33]=audio_dma.pci.dev*8; pir[34]=0x60; pir[36]=8;
    pir[12]=0x86; pir[13]=0x80; pir[14]=0x10; pir[15]=0x71;
    unsigned checksum=0; for (unsigned i=0;i<48;i++) checksum+=pir[i]; pir[31]=(uint8_t)-checksum;
    second_pci_count=1; second_pci[0]=(struct pci_func){.bus=0,.dev=1,.class_code=6,.subclass=1,.vendor=0x8086,.device=0x7110};
    ym.cfg[0x60/4]=11;
    CHECK(!ymfpci_resources(&audio_dma) && audio_dma.mmio_phys==0xe1000000);
    ym.cfg[0x60/4]=0x8b; CHECK(ymfpci_resources(&audio_dma)==-ENODEV);
    memset(pir,0,48); second_pci_count=0; g_boot.flags=boot_flags;
    for (unsigned fault=0;fault<9;fault++) {
        ym_setup(4,0);
        if (fault==0) ym.regs[0x108/4]=3;
        if (fault==1) ym.regs[0x150/4]=1;
        if (fault==2) ym.regs[0x154/4]=1;
        if (fault==3) ym.regs[0x10/4]=1;
        if (fault==4) ym.cfg[0x40/4]=0x001f;
        if (fault==5) ym.cfg[0x54/4]=3;
        if (fault==6) ym.regs[0x70/4]=0x10;
        if (fault==7) ym.regs[0x04/4]=1;
        if (fault==8) ym.regs[0x50/4]=1u<<16; /* GPIO interrupt enable at 52h */
        CHECK(backend_dma_init(&audio_dma)==-EBUSY && !ym.writes && !ym.resets && !audio_dma.touched);
        CHECK(pages_used==baseline && !audio.quarantined && ym.revokes==1);
    }
    /* Claims, allocation and mapping refusal precede the first device write. */
    for (unsigned slot=12;slot<16;slot++) {
        ym_setup(4,0); second_claim_failure=slot;
        CHECK(backend_dma_init(&audio_dma)==-EBUSY && !ym.writes && pages_used==baseline);
    }
    for (int at=0;at<2;at++) {
        ym_setup(4,0); alloc_fail=at;
        CHECK(backend_dma_init(&audio_dma)==-ENOMEM && !ym.writes && pages_used==baseline); alloc_fail=-1;
    }
    ym_setup(4,0); ym.map_failure=true;
    CHECK(backend_dma_init(&audio_dma)==-ENOMEM && !ym.writes && pages_used==baseline && !ym.revokes);
    for (int fault=0;fault<2;fault++) {
        ym_setup(4,0); ym.pin_failure=fault;
        CHECK(backend_dma_init(&audio_dma)==-ENOMEM && !ym.writes && pages_used==baseline && !ym.pinned_mask);
    }
    for (unsigned fault=0;fault<8;fault++) {
        ym_setup(4,0);
        if (fault==0) ym.corrupt_ram=0x1000;
        if (fault==1) ym.corrupt_ram=0x6ffc;
        if (fault==2) ym.regs[0x140/4]=31;
        if (fault==3) ym.no_progress=true;
        if (fault==4) ym.codec[0x7c/2]=0;
        if (fault==5) ym.base_reject=true;
        if (fault==6) ym.busy=true;
        if (fault==7) ym.reject_codec=true;
        CHECK(backend_dma_init(&audio_dma)<0 && !audio_dma.state.ready && !audio.quarantined);
        CHECK(pages_used==baseline && !audio.commitment_pages && ym.revokes==1);
    }
    puts("Yamaha DS-XG: active-peer/legacy, BAR/route, allocation/claim, firmware corruption/size, codec and silent-proof refusals PASS");
    for (unsigned fault=0;fault<3;fault++) {
        ym_setup(18,3); CHECK(!backend_dma_init(&audio_dma));
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        ym.rendered=2048; CHECK(!audio_backend_start());
        if (fault==0) ym.stop_stuck=true;
        if (fault==1) ym.bme_stuck=true;
        if (fault==2) ym.gone=true;
        unsigned held=pages_used, resets=ym.resets, starts=ym.starts;
        audio_backend_shutdown();
        CHECK(audio.quarantined && audio_dma.state.quarantine && pages_used==held && !ym.revokes);
        CHECK(ym.pinned_mask==3 && !ym.unpin_calls);
        for (unsigned i=0;i<8;i++) if (audio_dma.claims[i]>=0)
            CHECK(audio_resources[audio_dma.claims[i]].state==RS_QUARANTINED);
        ym.stop_stuck=ym.bme_stuck=ym.gone=false;
        audio_backend_shutdown();
        CHECK(ym.resets==resets && ym.starts==starts && pages_used==held && !ym.revokes);
        CHECK(audio_backend_init()==-EIO && backend_dma_init(&audio_dma)==-EIO);
        CHECK(audio_backend_start()==-EIO && audio_backend_publish(marker)==-EAGAIN);
        CHECK(audio_backend_master(&(struct ciuki_mixer_master){0})==-EIO);
        unsigned writes=ym.writes;
        CHECK(audio_backend_stop()==-EIO && audio_backend_progress()==-EIO && audio_dma_prove(&audio_dma)==-EIO);
        CHECK(ym.writes==writes && ((uint32_t *)audio_dma.desc)[0]==64 && ym.pinned_mask==3 && audio_dma.pcm[0].left==1234);
        ym_dispose_quarantine();
    }
    dma_selected=false; memset(&audio_dma,0,sizeof(audio_dma)); memset(&ac97,0,sizeof(ac97));
    audio.backend_name=0; audio.backend_pci=(struct pci_func){0}; audio.backend_touched=false;
    audio.error=0; audio.ready=audio.running=audio.quarantined=false; memset(&audio.caps,0,sizeof(audio.caps));
    CHECK(pages_used==baseline);
    puts("Yamaha DS-XG: stop/bus-master/device-loss faults retain DMA+MMIO+claims; repeated teardown and reactivation refused PASS");
}
