/* C-Media native register model, independent from the other PCI engines.
 * Counts are stereo frames; native mixer aliases never touch ISA SB ports. */
struct cmi_model {
    uint8_t regs[256], mixer[256], index;
    uint32_t cfg[64], cfg_address;
    unsigned frame[2], starts[2], resets[2], writes, legacy_writes, bad_count;
    unsigned stuck, busy_stuck, moving_stopped, no_progress, reject_reg;
    bool reject_mixer, disconnected;
    uint64_t tick;
};
static struct cmi_model cmi_hw;
static unsigned cmi_pins, cmi_unpins;
static int cmi_pin_failure=-1;
/* Observe the actual pin requests from production admission/teardown. Other
 * engines keep this fixture's original weak/no-ledger behavior. */
int mm_pin(struct mm_pin *pin)
{
    if (audio_dma.kind!=AUDIO_CMIPCI) return 0;
    CHECK(pin->pages==8 && pin->alignment==4 && !pin->boundary && pin->address_mask==UINT32_MAX);
    CHECK(pin->generation==audio_dma.generations[1] && !audio_dma.touched);
    CHECK(pin->first_pfn==(pin->direction==MM_PIN_FROM_DEVICE ? audio_dma.cmi.phys : audio_dma.phys)/PAGE_SIZE);
    if ((int)cmi_pins==cmi_pin_failure) return -ENOMEM;
    pin->ticket=++cmi_pins; return 0;
}
int mm_unpin(struct mm_pin *pin,uint32_t generation)
{
    CHECK(pin->ticket && generation==pin->generation && !audio_dma.state.quarantine &&
          !audio_dma.state.running && !audio_dma.cmi.running && !audio_dma.irq_attached);
    CHECK(audio_dma.phys && audio_dma.cmi.phys); cmi_unpins++; return 0;
}
static uint32_t cmi_model_reg(struct cmi_model *h,unsigned off,unsigned width)
{
    uint32_t value=0;
    for (unsigned i=0;i<width;i++) value|=(uint32_t)h->regs[off+i]<<(8*i);
    return value;
}
static void cmi_model_store(struct cmi_model *h,unsigned off,unsigned value,unsigned width)
{ for (unsigned i=0;i<width;i++) h->regs[off+i]=value>>(8*i); }
static uint32_t cmi_model_read(void *cookie,uint16_t port,unsigned width)
{
    struct cmi_model *h=cookie;
    if (port==0xcfc) return h->cfg[(h->cfg_address&255)/4];
    if (port==0x4d0 || port==0x4d1) return 255;
    CHECK(port>=audio_dma.base && port+width<=audio_dma.base+256);
    unsigned off=port-audio_dma.base;
    if (h->disconnected) return width==1 ? 255 : width==2 ? 65535 : UINT32_MAX;
    if (off==0x22) { CHECK(width==1); return h->mixer[h->index]; }
    if (off==0x84 || off==0x8c) {
        CHECK(width==2); unsigned ch=off==0x8c;
        if (h->bad_count&(1u<<ch)) return 65535;
        return 8191-h->frame[ch];
    }
    if (off==0x10) {
        uint32_t value=cmi_model_reg(h,off,4);
        for (unsigned ch=0;ch<2;ch++) if ((cmi_model_reg(h,0,4)&(0x10000u<<ch)) || (h->busy_stuck&(1u<<ch))) value|=4u<<ch;
        if (value&3) value|=0x80000000u;
        return value;
    }
    return cmi_model_reg(h,off,width);
}
static void cmi_model_write(void *cookie,uint16_t port,uint32_t value,unsigned width)
{
    struct cmi_model *h=cookie;
    if (port==0xcf8) { h->cfg_address=value; return; }
    if (port>=0xcfc && port<=0xcff) {
        unsigned reg=(h->cfg_address&255)+port-0xcfc, shift=(reg&3)*8;
        uint32_t mask=width==4 ? UINT32_MAX : ((1u<<(width*8))-1)<<shift;
        h->cfg[reg/4]=(h->cfg[reg/4]&~mask)|(value<<shift); return;
    }
    if (port<audio_dma.base || port+width>audio_dma.base+256) { h->legacy_writes++; CHECK(false); }
    unsigned off=port-audio_dma.base; h->writes++;
    if (h->reject_reg==off+1) return;
    if (off==0x23) { CHECK(width==1 && value!=0x3f && value!=0x40); h->index=value; return; }
    if (off==0x22) { CHECK(width==1); if (!h->reject_mixer) h->mixer[h->index]=value; return; }
    if (off==0) {
        CHECK(width==4);
        unsigned old=cmi_model_reg(h,0,4);
        for (unsigned ch=0;ch<2;ch++) {
            unsigned enable=0x10000u<<ch, reset=0x40000u<<ch;
            if ((h->stuck&(1u<<ch)) && (old&enable)) value|=enable;
            if ((value&enable) && !(old&enable)) {
                CHECK(audio_dma.claims[0]>=0 && audio_dma.claims[1]>=0 && audio_dma.claims[4]>=0 && audio_dma.claims[5]>=0);
                CHECK((h->cfg[1]&5)==5 && (cmi_model_reg(h,4,4)&0x1e)==0x10);
                CHECK(cmi_model_reg(h,0x80+ch*8,4)==(ch ? audio_dma.cmi.phys : audio_dma.phys));
                CHECK(cmi_model_reg(h,0x84+ch*8,2)==8191 && cmi_model_reg(h,0x86+ch*8,2)==255);
                CHECK(!audio_dma.state.quarantine); h->starts[ch]++;
            }
            if (value&reset) {
                CHECK(!(old&enable) && !(h->busy_stuck&(1u<<ch)) && !audio_dma.state.quarantine);
                h->resets[ch]++; h->frame[ch]=0;
            }
        }
    }
    if (off==4) CHECK(!(value&0xe));
    if (off==0x18) CHECK(!(value&0x80000));
    if (off==0xc) {
        unsigned status=cmi_model_reg(h,0x10,4);
        if (!(value&0x10000)) status&=~1u;
        if (!(value&0x20000)) status&=~2u;
        cmi_model_store(h,0x10,status,4);
        value=(value&0xffffff)|(cmi_model_reg(h,0xc,4)&0xff000000);
    }
    if (off==8) value=(value&0xffffff)|(cmi_model_reg(h,8,4)&0xff000000);
    cmi_model_store(h,off,value,width);
    if (off==0x84 || off==0x8c) h->frame[off==0x8c]=0;
}
static uint64_t cmi_model_now(void *cookie) { return ((struct cmi_model *)cookie)->tick; }
static void cmi_model_sleep(void *cookie,uint32_t ms)
{
    struct cmi_model *h=cookie; h->tick+=ms;
    for (unsigned ch=0;ch<2;ch++) {
        bool running=(cmi_model_reg(h,0,4)&(0x10000u<<ch))!=0;
        if ((!running || (h->no_progress&(1u<<ch))) && !(h->moving_stopped&(1u<<ch))) continue;
        unsigned fn=cmi_model_reg(h,4,4), code=(fn>>(10+ch*3))&7;
        static const unsigned rate[]={5512,11025,22050,44100,8000,16000,32000,48000};
        unsigned frames=ms*rate[code]/1000;
        for (unsigned i=0;ch && i<frames;i++) {
            unsigned at=(h->frame[ch]+i)%8192;
            audio_dma.cmi.pcm[at]=(struct audio_frame){(int16_t)at,(int16_t)-(int)at};
        }
        h->frame[ch]=(h->frame[ch]+frames)%8192;
    }
}
static void cmi_setup(unsigned device,unsigned signature)
{
    CHECK(cmi_pins==cmi_unpins); cmi_pins=cmi_unpins=0; cmi_pin_failure=-1;
    memset(&audio_dma,0,sizeof(audio_dma)); memset(&cmi_hw,0,sizeof(cmi_hw));
    struct audio_dma_device *d=&audio_dma;
    d->kind=AUDIO_CMIPCI; d->base=0x1000; d->bytes=256; d->name="cmipci"; d->ops=&cmipci_ops;
    d->command=1; d->state.irq=11;
    d->pci=(struct pci_func){.vendor=0x13f6,.device=device,.class_code=4,.subclass=1,.command=1,.irq_line=11,.irq_pin=1};
    d->io=(struct ac97_io){&cmi_hw,cmi_model_read,cmi_model_write,cmi_model_now,cmi_model_sleep};
    for (unsigned i=0;i<8;i++) d->claims[i]=-1;
    cmi_hw.cfg[1]=1; cmi_model_store(&cmi_hw,0xc,signature,4);
    for (unsigned i=0;i<256;i++) cmi_hw.mixer[i]=(i*3)&0xf8;
    cmi_hw.regs[0x24]=0xc0; cmi_hw.regs[0x25]=0xf0;
    second_pci_count=0; second_claim_next=12; second_claim_failure=-1;
    dma_selected=true; audio.backend_name=d->name; audio.backend_pci=d->pci;
    audio.error=0; audio.ready=audio.running=audio.quarantined=false;
    audio.master=(struct ciuki_mixer_master){.left=20,.right=20};
}
static void cmi_dispose_quarantine(void)
{
    /* Only the fixture disposes memory after discarding its fake device. */
    for (unsigned i=0;i<8;i++) { pmm_free(audio_dma.phys+i*PAGE_SIZE); pmm_free(audio_dma.cmi.phys+i*PAGE_SIZE); }
    audio.commitment_pages=0;
    cmi_pins=cmi_unpins=0; memset(backend_pins,0,sizeof(backend_pins));
}
static void test_cmipci(void)
{
    unsigned baseline=pages_used;
    struct audio_dma_device *d=&audio_dma;
    static struct audio_frame marker[256], captured[256];
    for (unsigned i=0;i<256;i++) marker[i]=(struct audio_frame){1234,-2345};
    unsigned devices[]={0x100,0x101,0x111,0x112,0x111,0x112,0x111,0x111};
    for (unsigned k=0;k<ARRAY_SIZE(devices);k++) {
        cmi_setup(devices[k],k==5 ? 0x08000000 : k>=2 && k<6 ? 0x05000000 : 0);
        if (k==6) cmi_model_store(&cmi_hw,8,0x01000000,4);
        if (k==4) d->pci.vendor=0x10b9;
        enum audio_backend_kind kind=0;
        CHECK(cmipci_match(&d->pci,&kind) && kind==AUDIO_CMIPCI);
        uint8_t saved[256]; memcpy(saved,cmi_hw.mixer,256);
        CHECK(!backend_dma_init(d) && d->state.ready && !d->state.running && !d->cmi.running);
        CHECK(d->cmi.model==(k<2 ? 0 : k==5 ? 55 : k==6 ? 37 : k==7 ? 33 : 39));
        CHECK(pages_used==baseline+16 && audio.commitment_pages==16 && cmi_pins==2 && !cmi_unpins && audio.caps.backend==12 && audio.caps.output_rate==48000);
        CHECK(cmi_hw.starts[0]==1 && cmi_hw.starts[1]==1 && !cmi_hw.legacy_writes);
        CHECK(cmi_hw.mixer[0x30]==0xf8 && cmi_hw.mixer[0x32]==0xf8 && cmi_hw.mixer[0x3c]==0);
        CHECK(!audio_backend_master(&(struct ciuki_mixer_master){.mute=1}) && cmi_hw.regs[0x24]==0xc0);
        CHECK(!audio_backend_master(&(struct ciuki_mixer_master){.left=20,.right=80}) && cmi_hw.regs[0x24]==0x80);
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start());
        for (unsigned i=1;i<=96;i++) {
            cmi_hw.frame[0]=i*256%8192; cmi_hw.tick+=6;
            CHECK(!audio_backend_progress() && d->state.retired==i && d->state.fetched==(uint64_t)i*256);
            CHECK(!audio_backend_publish(marker));
        }
        unsigned writes=cmi_hw.writes;
        CHECK(cmipci_capture_start(d,47999)==-EINVAL && cmi_hw.writes==writes);
        const unsigned rates[]={5512,11025,22050,44100,8000,16000,32000,48000};
        for (unsigned r=0;r<8;r++) {
            unsigned resets=cmi_hw.resets[0];
            CHECK(!cmipci_capture_start(d,rates[r]));
            CHECK(((cmi_model_reg(&cmi_hw,4,4)>>13)&7)==r && d->state.running);
            CHECK(cmipci_capture_start(d,rates[r])==-EBUSY);
            CHECK(cmipci_capture_read(d,captured)==-EAGAIN);
            for (unsigned i=0;i<96;i++) {
                cmi_model_sleep(&cmi_hw,(256000u+rates[r]-1)/rates[r]);
                CHECK(!cmipci_capture_read(d,captured));
                for (unsigned j=0;j<256;j++) {
                    int expected=(i*256+j)%8192;
                    CHECK(captured[j].left==expected && captured[j].right==-expected);
                }
            }
            CHECK(!cmipci_capture_stop(d) && d->state.running && cmi_hw.resets[0]==resets);
        }
        CHECK(!cmipci_capture_start(d,48000));
        unsigned capture_resets=cmi_hw.resets[1];
        CHECK(!audio_backend_stop() && d->cmi.running && cmi_hw.resets[1]==capture_resets);
        /* Channel B IRQ is ours; UART-only/foreign sources are not. */
        cmi_model_store(&cmi_hw,0x10,0x10000,4); CHECK(!backend_irq(11,"cmipci"));
        cmi_model_store(&cmi_hw,0x10,2,4); CHECK(backend_irq(11,"cmipci") && d->cmi.irqs==1);
        CHECK(!backend_irq(11,"cmipci"));
        memset(captured,0x5a,sizeof(captured)); cmi_model_sleep(&cmi_hw,171);
        CHECK(cmipci_capture_read(d,captured)==-EIO && d->cmi.failed && captured[0].left==0x5a5a);
        unsigned sleep_pages=pages_used; uint32_t sleep_phys=d->phys;
        CHECK(!audio_backend_suspend() && !audio_backend_resume());
        CHECK(d->state.ready && d->phys==sleep_phys && pages_used==sleep_pages);
        audio_backend_shutdown();
        CHECK(!audio.quarantined && pages_used==baseline && !audio.commitment_pages && cmi_unpins==2 && !(cmi_hw.cfg[1]&4));
        CHECK(!memcmp(saved,cmi_hw.mixer,256) && cmi_hw.regs[0x24]==0xc0 && cmi_hw.regs[0x25]==0xf0);
        writes=cmi_hw.writes;
        CHECK(!cmipci_capture_stop(d) && cmipci_capture_start(d,48000)==-EIO &&
              audio_backend_master(&(struct ciuki_mixer_master){0})==-EIO && cmi_hw.writes==writes);
    }
    /* Failures before first mutation: firmware siblings/decoders, unreadable,
     * newer silicon and claim/allocation admission failures. */
    for (unsigned fault=0;fault<9;fault++) {
        cmi_setup(0x111,0);
        if (fault==0) cmi_model_store(&cmi_hw,0,0x20000,4);
        if (fault==1) cmi_hw.busy_stuck=1;
        if (fault==2) cmi_model_store(&cmi_hw,4,8,4);
        if (fault==3) cmi_model_store(&cmi_hw,4,4,4);
        if (fault==4) cmi_model_store(&cmi_hw,4,2,4);
        if (fault==5) cmi_model_store(&cmi_hw,0x18,0x80000,4);
        if (fault==6) cmi_model_store(&cmi_hw,0xc,0x10000000,4);
        if (fault==7) cmi_model_store(&cmi_hw,0xc,0x20000000,4);
        if (fault==8) cmi_hw.disconnected=true;
        CHECK(backend_dma_init(d)<0 && !d->touched && !cmi_hw.writes && pages_used==baseline && !audio.quarantined);
    }
    for (unsigned slot=12;slot<=15;slot++) {
        cmi_setup(0x111,0); second_claim_failure=slot;
        CHECK(backend_dma_init(d)==-EBUSY && !d->touched && pages_used==baseline);
    }
    for (unsigned allocation=0;allocation<2;allocation++) {
        cmi_setup(0x111,0); alloc_fail=allocation;
        CHECK(backend_dma_init(d)==-ENOMEM && !d->touched && pages_used==baseline); alloc_fail=-1;
    }
    for (unsigned pin=0;pin<2;pin++) {
        cmi_setup(0x111,0); cmi_pin_failure=pin;
        CHECK(backend_dma_init(d)==-ENOMEM && !d->touched && pages_used==baseline && cmi_pins==cmi_unpins);
    }
    for (unsigned ch=0;ch<2;ch++) {
        cmi_setup(0x111,0); cmi_hw.no_progress=1u<<ch;
        CHECK(backend_dma_init(d)==-ETIMEDOUT && pages_used==baseline && !audio.quarantined);
    }
    cmi_setup(0x111,0); cmi_hw.reject_mixer=true;
    CHECK(backend_dma_init(d)==-EIO && pages_used==baseline && !audio.quarantined);
    cmi_setup(0x111,0); CHECK(!backend_dma_init(d));
    CHECK(!cmipci_capture_start(d,48000)); cmi_hw.bad_count=2;
    memset(captured,0x5a,sizeof(captured));
    CHECK(cmipci_capture_read(d,captured)==-EIO && captured[0].left==0x5a5a);
    cmi_hw.bad_count=0; audio_backend_shutdown(); CHECK(pages_used==baseline);
    cmi_setup(0x111,0); CHECK(!backend_dma_init(d) && !cmipci_capture_start(d,48000));
    /* Polling a stalled cursor must not extend its progress deadline. */
    for (unsigned i=0;i<100;i++) { cmi_hw.tick++; CHECK(cmipci_capture_read(d,captured)==-EAGAIN); }
    cmi_hw.tick++; CHECK(cmipci_capture_read(d,captured)==-EIO);
    audio_backend_shutdown(); CHECK(pages_used==baseline);
    cmi_setup(0x111,0); CHECK(!backend_dma_init(d) && !cmipci_capture_start(d,48000));
    /* Frequent valid observations still detect unread data being overtaken. */
    for (unsigned i=0;i<239;i++) { cmi_model_sleep(&cmi_hw,6); CHECK(!cmipci_capture_read(d,captured)); }
    cmi_model_sleep(&cmi_hw,6);
    CHECK(cmipci_capture_read(d,captured)==-EIO && d->cmi.overruns==1);
    audio_backend_shutdown(); CHECK(pages_used==baseline);
    /* Exact matcher and discovery refusals never activate the ISA fallback. */
    cmi_setup(0x111,0);
    enum audio_backend_kind bad_kind=0;
    d->pci.device=0x113; CHECK(!cmipci_match(&d->pci,&bad_kind));
    d->pci.device=0x111; d->pci.subclass=3; CHECK(!cmipci_match(&d->pci,&bad_kind));
    d->pci.subclass=1; second_pci[0]=d->pci; second_pci_count=1;
    CHECK(audio_backend_init()==-ENODEV && !strcmp(audio.reason,"invalid_bars") && !dma_selected && !cmi_hw.writes);
    second_pci[0].bar[0]=0x1001; second_pci[0].command=0;
    CHECK(audio_backend_init()==-ENODEV && !strcmp(audio.reason,"firmware_state_unreadable") && !dma_selected);
    second_pci[0].command=1;
    CHECK(audio_backend_init()==-ENODEV && !strcmp(audio.reason,"routing_unqualified") && !dma_selected);
    second_pci_count=0;
    /* Each stop proof component matters. Failure pins both rings and every
     * claim, forbids resets/restoration and remains permanent after recovery. */
    for (unsigned fault=0;fault<4;fault++) {
        cmi_setup(0x111,0x04000000); CHECK(!backend_dma_init(d));
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start() && !cmipci_capture_start(d,48000));
        if (fault<2) cmi_hw.stuck=1u<<fault;
        if (fault==2) cmi_hw.busy_stuck=2;
        if (fault==3) cmi_hw.moving_stopped=2;
        unsigned resets=cmi_hw.resets[0]+cmi_hw.resets[1];
        audio_backend_shutdown();
        CHECK(audio.quarantined && d->state.quarantine && d->phys && d->cmi.phys && pages_used==baseline+16 && cmi_pins==2 && !cmi_unpins);
        CHECK(cmi_hw.resets[0]+cmi_hw.resets[1]<=resets+1);
        for (unsigned i=0;i<8;i++) if (d->claims[i]>=0) CHECK(registry_get(d->claims[i])->state==RS_QUARANTINED);
        cmi_hw.stuck=cmi_hw.busy_stuck=cmi_hw.moving_stopped=0;
        resets=cmi_hw.resets[0]+cmi_hw.resets[1]; audio_backend_shutdown();
        CHECK(d->state.quarantine && pages_used==baseline+16 && cmi_hw.resets[0]+cmi_hw.resets[1]==resets);
        CHECK(cmipci_capture_start(d,48000)==-EIO && audio_backend_publish(marker)==-EAGAIN);
        cmi_dispose_quarantine();
    }
    dma_selected=false; memset(d,0,sizeof(*d)); memset(&ac97,0,sizeof(ac97));
    audio.backend_pci=(struct pci_func){0}; audio.backend_name=0; audio.backend_touched=false;
    audio.error=0; audio.ready=audio.running=audio.quarantined=false; memset(&audio.caps,0,sizeof(audio.caps));
    CHECK(pages_used==baseline);
    puts("CMI8338/8738: native PCM/mixer, 8 capture rates, duplex, wrap, decoder isolation, stop and permanent quarantine PASS");
}
