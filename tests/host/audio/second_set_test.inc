/* Synthetic register devices exercise production admission, playback,
 * boundaries and cleanup; no privileged instructions or QEMU. */
struct second_hw {
    struct audio_dma_device *d;
    uint32_t cfg[64], regs[64], pci_address, src_state;
    uint16_t codec[64], src[128];
    uint8_t mixer[256], mixer_index, fifo[8];
    unsigned fifo_head, fifo_count, ff, count_word, dma_address, dma_page, dma_mode;
    uint64_t tick;
    unsigned frame, starts, stops, codec_writes, src_writes, writes, dsp_bytes;
    bool active, mask, no_progress, stop_stuck, src_busy, codec_busy, zero_codec, reject_codec, window;
    unsigned window_reads, start_delay;
    uint64_t active_at;
    bool pending_start, zero_remaining;
    uint8_t reset;
};
static struct second_hw second;
static void second_push(struct second_hw *h,uint8_t value)
{ CHECK(h->fifo_count<8); h->fifo[(h->fifo_head+h->fifo_count++)%8]=value; }
static uint32_t second_read(void *cookie,uint16_t port,unsigned width)
{
    struct second_hw *h=cookie; struct audio_dma_device *d=h->d;
    if (port==0xcfc) return h->cfg[(h->pci_address&255)/4];
    if (port==0x4d0 || port==0x4d1) return d->kind==AUDIO_SB16 ? 0 : 255;
    if (port==0xc6) {
        unsigned words=h->count_word-(h->frame*2)%(h->count_word+1);
        unsigned v=(words>>(h->ff*8))&255; h->ff^=1; return v;
    }
    unsigned off=port-d->base;
    if (d->kind==AUDIO_SB16) {
        CHECK(width==1);
        if (off==5) return h->mixer[h->mixer_index];
        if (off==0xc) return h->codec_busy ? 0x80 : 0;
        if (off==0xe) return h->fifo_count ? 0x80 : 0;
        if (off==0xa) { CHECK(h->fifo_count); uint8_t v=h->fifo[h->fifo_head]; h->fifo_head=(h->fifo_head+1)%8; h->fifo_count--; return v; }
        if (off==0xf) { h->mixer[0x82]&=~2; return 255; }
        return 0;
    }
    if (d->kind==AUDIO_ES1370 || d->kind==AUDIO_ES1371) {
        if (off==4) return h->regs[1] | (h->codec_busy ? 0x700 : 0);
        if (off==0x3c) return h->regs[off/4] | (uint32_t)h->frame<<16;
        if (off==0x10 && d->kind==AUDIO_ES1371) {
            if (h->src_busy) return 1u<<23;
            if (h->window) return h->src_state | (h->window_reads++ ? 0x10000 : 0);
            return h->src_state;
        }
        if (off==0x14 && d->kind==AUDIO_ES1371) return h->regs[off/4] | (h->codec_busy ? 1u<<30 : 0);
        return h->regs[off/4];
    }
    if (off==0x80) return h->regs[off/4] | (h->codec_busy ? 1u<<24 : 0);
    if (h->pending_start && h->tick>=h->active_at) { h->active=true; h->pending_start=false; }
    if (off==d->channel) return (h->regs[off/4]&255) | (h->active ? 0x80 : 0);
    if (off==d->channel+4 && h->active) return d->desc_phys+(h->frame/AUDIO_QUANTUM+1)*8;
    if (off==d->channel+0xc) return (d->kind==AUDIO_VIA686 ? 0 : (h->frame/AUDIO_QUANTUM)<<24) | (h->zero_remaining ? 0 : (AUDIO_QUANTUM-h->frame%AUDIO_QUANTUM)*4);
    return h->regs[off/4];
}
static void second_write(void *cookie,uint16_t port,uint32_t value,unsigned width)
{
    struct second_hw *h=cookie; struct audio_dma_device *d=h->d; h->writes++;
    if (port==0xcf8) { CHECK(width==4); h->pci_address=value; return; }
    if (port>=0xcfc && port<=0xcff) {
        unsigned reg=(h->pci_address&255)+(port-0xcfc), shift=(reg&3)*8;
        uint32_t mask=width==4 ? UINT32_MAX : ((1u<<(width*8))-1)<<shift;
        h->cfg[reg/4]=(h->cfg[reg/4]&~mask)|(value<<shift);
        return;
    }
    if (port==0xd4) { if (!h->stop_stuck) h->mask=(value&4)!=0; return; }
    if (port==0xd8) { h->ff=0; return; }
    if (port==0xd6) { h->dma_mode=value; return; }
    if (port==0x8b) { h->dma_page=value; return; }
    if (port==0xc4 || port==0xc6) {
        unsigned *p=port==0xc4 ? &h->dma_address : &h->count_word;
        *p=(*p&~(255u<<(h->ff*8))) | value<<(h->ff*8); h->ff^=1; if (port==0xc6) h->frame=0; return;
    }
    unsigned off=port-d->base;
    if (d->kind==AUDIO_SB16) {
        CHECK(width==1);
        if (off==4) h->mixer_index=value;
        else if (off==5) h->mixer[h->mixer_index]=value;
        else if (off==6) {
            if (h->reset && !value) { h->fifo_count=h->fifo_head=0; second_push(h,0xaa); if (!h->stop_stuck) h->active=false; }
            h->reset=value;
        } else if (off==0xc) {
            h->dsp_bytes++;
            if (value==0xe1) { second_push(h,4); second_push(h,5); }
            if (value==0xb6) { h->active=true; h->starts++; CHECK(!d->state.ready || !d->proving); }
        }
        return;
    }
    if (d->kind==AUDIO_ES1370 || d->kind==AUDIO_ES1371) {
        if (off==0) {
            if (value&ES_DAC2_ENABLE) { h->starts++; h->active=true; CHECK(!d->state.ready || !d->proving); }
            else if (!h->stop_stuck) { if (h->active) h->stops++; h->active=false; }
            h->regs[0]=value | (h->stop_stuck && h->active ? ES_DAC2_ENABLE : 0); return;
        }
        if (off==0x3c) { h->regs[off/4]=value; h->frame=0; return; }
        if (off==0x20) { h->regs[off/4]=value; if (!(value&ES_P2_IRQ)) h->regs[1]&=~2u; return; }
        if (off==0x10 && d->kind==AUDIO_ES1370) { CHECK(width==2); h->codec[(value>>8)&63]=value&255; h->codec_writes++; return; }
        if (off==0x10 && d->kind==AUDIO_ES1371) {
            if (value&(1u<<24)) { h->src[(value>>25)&127]=value&0xffff; h->src_writes++; }
            h->src_state=value&ES_SRC_MASK; h->window=(value&0x10000)!=0; h->window_reads=0; return;
        }
        if (off==0x14 && d->kind==AUDIO_ES1371) {
            CHECK(h->window && h->window_reads>=2);
            unsigned reg=(value>>16)&0x7f;
            if (value&(1u<<23)) h->regs[off/4]=1u<<31 | (uint32_t)reg<<16 | (h->zero_codec ? 0 : h->codec[reg/2] | (reg==0x26 ? 15 : 0));
            else { if (!h->reject_codec) h->codec[reg/2]=value&0xffff; h->codec_writes++; h->regs[off/4]=0; }
            return;
        }
        h->regs[off/4]=value; return;
    }
    if (off==0x80) {
        unsigned reg=(value>>16)&0x7e;
        if (value&(1u<<23)) h->regs[off/4]=1u<<25 | reg<<16 | (h->zero_codec ? 0 : h->codec[reg/2] | (reg==0x26 ? 15 : 0));
        else { if (!h->reject_codec) h->codec[reg/2]=value&0xffff; h->codec_writes++; h->regs[off/4]=value; }
        return;
    }
    if (off==d->channel+1) {
        if (value&0x40) { if (!h->stop_stuck) { h->active=h->pending_start=false; h->stops++; } }
        else if (value&0x80) { h->pending_start=true; h->active_at=h->tick+h->start_delay; h->active=false; h->starts++; h->frame=0; CHECK(!d->state.ready || !d->proving); }
        return;
    }
    if (off==d->channel && width==1) { h->regs[off/4]&=~value; return; }
    if (width==1) { unsigned shift=(off&3)*8; h->regs[off/4]=(h->regs[off/4]&~(255u<<shift)) | value<<shift; }
    else h->regs[off/4]=value;
}
static uint64_t second_now(void *cookie) { return ((struct second_hw *)cookie)->tick; }
static void second_sleep(void *cookie,uint32_t ms)
{
    struct second_hw *h=cookie; h->tick+=ms;
    if (h->pending_start && h->tick>=h->active_at) { h->active=true; h->pending_start=false; }
    if (h->active && !h->no_progress && (h->d->kind!=AUDIO_SB16 || !h->mask)) h->frame=(h->frame+ms*44)%(32*256);
}
static void second_setup(enum audio_backend_kind kind,unsigned revision)
{
    struct audio_dma_device *d=&audio_dma;
    memset(d,0,sizeof(*d)); memset(&second,0,sizeof(second));
    second.d=d; second.mask=true; second.cfg[1]=1; second.cfg[0x40/4]=0xcc01;
    second.codec[0x7c/2]=0x4144; second.codec[0x7e/2]=0x5348;
    second.codec[0x26/2]=0xf; second.codec[2/2]=0x8000; second.codec[0x18/2]=0x8808;
    second.mixer[0x80]=2; second.mixer[0x81]=0x22;
    d->kind=kind; d->base=kind==AUDIO_SB16 ? 0x220 : 0x1000; d->bytes=kind==AUDIO_SB16 ? 16 : kind>=AUDIO_VIA686 ? 256 : 64;
    d->name=kind==AUDIO_SB16 ? "sb16" : kind>=AUDIO_VIA686 ? "via82xx" : "es137x";
    d->ops=kind==AUDIO_SB16 ? &sb16_ops : kind>=AUDIO_VIA686 ? &via82xx_ops : &es137x_ops;
    d->io=(struct ac97_io){&second,second_read,second_write,second_now,second_sleep};
    d->command=1; d->pci=(struct pci_func){.vendor=kind>=AUDIO_VIA686 ? 0x1106 : 0x1274,
        .device=kind==AUDIO_ES1370 ? 0x5000 : kind==AUDIO_ES1371 ? 0x1371 : kind==AUDIO_VIA686 ? 0x3058 : 0x3059,
        .class_code=4,.subclass=1,.revision=revision,.irq_line=11,.irq_pin=1,.command=1};
    if (kind==AUDIO_SB16) memset(&d->pci,0,sizeof(d->pci));
    d->state.irq=kind==AUDIO_SB16 ? 5 : 11;
    for (unsigned i=0;i<8;i++) d->claims[i]=-1;
    second_claim_next=12; second_claim_failure=-1; second_irq_adds=second_pic_masks=second_pic_unmasks=0;
    second_pic_masked=true; second_isa_handler=0;
    dma_selected=true; audio.backend_name=d->name; audio.backend_pci=d->pci;
    audio.master=(struct ciuki_mixer_master){.version=1,.size=32,.left=50,.right=50};
    audio.error=0; audio.quarantined=false; audio.ready=false; audio.running=false;
}
static void test_physical_rate(void)
{
    unsigned baseline=pages_used;
    static struct audio_frame source[1024], result[256];
    for (unsigned i=0;i<1024;i++) source[i]=(struct audio_frame){(int16_t)(i*16),(int16_t)(-((int)i)*16)};
    struct ciuki_audio_caps saved_caps=audio.caps;
    audio.caps.output_rate=44100; audio.ready=true; audio.error=0; audio.master_in_codec=false;
    audio.master=(struct ciuki_mixer_master){.left=100,.right=100};
    const unsigned rates[]={8000,44100,48000};
    for (unsigned r=0;r<3;r++) {
        struct audio_stream *stream; CHECK(!audio_stream_open(&stream));
        struct ciuki_audio_format f={.format=CIUKI_AUDIO_S16LE,.channels=2,.rate=rates[r]};
        CHECK(!audio_stream_format(stream,&f) && f.output_rate==44100 && f.rate==rates[r]);
        CHECK(audio_stream_enqueue(stream,source,sizeof(source))==sizeof(source));
        unsigned rendered=0;
        const unsigned batches[]={13,256,231};
        for (unsigned k=0;k<3;k++) {
            audio_mix(result,batches[k]);
            for (unsigned i=0;i<batches[k];i++) {
                uint64_t position=(uint64_t)(rendered+i)*rates[r];
                unsigned index=position/44100, phase=position%44100;
                int left=source[index].left+(int64_t)(source[index+1].left-source[index].left)*phase/44100;
                CHECK(result[i].left==left && result[i].right==-left);
            }
            rendered+=batches[k];
        }
        CHECK(stream->consumed==(uint64_t)500*rates[r]/44100 && stream->phase==(uint64_t)500*rates[r]%44100);
        audio_stream_close(stream);
    }
    /* Debt from the last short downsampled batch cannot extrapolate the
     * next queued sample with a phase greater than one output frame. */
    struct audio_stream *s; CHECK(!audio_stream_open(&s)); s->phase=47000;
    CHECK(audio_stream_enqueue(s,source,12)==12); audio_mix(result,1);
    CHECK(s->phase<44100 && result[0].left>=source[1].left && result[0].left<=source[2].left);
    audio_stream_close(s); audio.caps=saved_caps; audio.ready=false;
    CHECK(pages_used==baseline);
    puts("effective 44.1 kHz: native 8/44.1/48 kHz rational conversion, split batches, source-frame accounting and short-batch debt PASS");
}
static void route_identity(uint8_t *table, unsigned size)
{
    table[12]=second_pci[0].vendor; table[13]=second_pci[0].vendor>>8;
    table[14]=second_pci[0].device; table[15]=second_pci[0].device>>8;
    table[31]=0; unsigned sum=0; for(unsigned i=0;i<size;i++)sum+=table[i]; table[31]=(uint8_t)-sum;
}
static void test_audio_routes(void)
{
    second_setup(AUDIO_ES1370,0);
    uint8_t table[64]={0}; memcpy(table,"$PIR",4); table[5]=1; table[6]=64;
    table[8]=0; table[9]=8;
    struct pci_func card={.bus=0,.dev=5,.irq_line=11,.irq_pin=1,.vendor=0x1274};
    second_pci_count=1; second_pci[0]=(struct pci_func){.bus=0,.dev=1,.class_code=6,.subclass=1,.vendor=0x1106,.device=0x0686};
    table[32]=0; table[33]=5*8; table[34]=1; table[36]=8;
    unsigned checksum=0; for (unsigned i=0;i<64;i++) checksum+=table[i]; table[31]=(uint8_t)-checksum;
    route_identity(table,sizeof(table));
    second.cfg[0x54/4]=11u<<12; /* VIA link1: 55h upper nibble */
    CHECK(audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    table[31]++; CHECK(!audio_pir_route(&audio_dma.io,&card,table,sizeof(table))); table[31]--;
    CHECK(!audio_pir_route(&audio_dma.io,&card,table,48));
    second.cfg[0x54/4]=10u<<12; CHECK(!audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    second_pci[0].vendor=0x1039; second_pci[0].device=8;
    route_identity(table,sizeof(table));
    second.cfg[0x40/4]=11u<<8;
    CHECK(audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    second.cfg[0x40/4]|=0x80u<<8; CHECK(!audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    second_pci[0].vendor=0x10de; card.vendor=0x10de; card.device=0x006a;
    route_identity(table,sizeof(table));
    CHECK(audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    card.device=0xdead; CHECK(!audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    second_pci[0].vendor=0x8086; second_pci[0].device=0x7110;
    second.cfg[0x60/4]=11; uint32_t boot_flags=g_boot.flags; g_boot.flags|=CBI_F_SMBIOS_QEMU;
    CHECK(audio_pci_routed(&audio_dma.io,&card)); second.cfg[0x60/4]=0x8b;
    CHECK(!audio_pci_routed(&audio_dma.io,&card));
    g_boot.flags=boot_flags; second_pci_count=0; dma_selected=false;
    puts("audio firmware routes: bounded PIR size/checksum, VIA nibble, SiS disable bit, nForce pin bitmap, PIIX swizzle/disabled route PASS");
}
static void test_ich_variants(void)
{
    static struct ac97_bd bdl[32]; static struct audio_frame buffers[32][256];
    for (unsigned variant=0;variant<3;variant++) {
        hardware_setup(&ac97); hw.any_pcm=hw.ich3=true;
        ac97.pci=(struct pci_func){.vendor=variant ? 0x10de : 0x1039,
            .device=variant==0 ? 0x7012 : variant==1 ? 0x01b1 : 0x006a,
            .class_code=4,.subclass=1,.bar={0x101,0x201},.irq_line=11,.irq_pin=1};
        CHECK(supported(&ac97.pci) && controller_layout(&ac97,0));
        CHECK(ac97.nabm_bytes>=128 && ac97.pcm_unity==0x0808);
        ac97.bdl=bdl; ac97.bdl_phys=0x90000;
        for (unsigned i=0;i<32;i++) { ac97.pcm[i]=buffers[i]; bdl[i].address=0xa0000+i*1024; }
        CHECK(!controller_activate(&ac97) && hw.starts==1 && !ac97.running);
        CHECK(ac97.rate==48000 && ac97.codec_id && !ac97.ready);
        if (!variant) CHECK(ac97.sis && ac97.saved_sis_gate==0 && hw.sis_gate==1);
        for (unsigned i=0;i<8;i++) CHECK(!ac97_publish(&ac97,buffers[0]));
        CHECK((bdl[0].length&0xffff)==(variant ? 512u : 1024u));
        ac97.ready=true; hw.ich3=false; CHECK(!ac97_start(&ac97));
        hw.remaining=ac97_units(&ac97)-128; hw.sr=0; hw.civ=0; hw.piv=1;
        CHECK(!ac97_progress(&ac97) && ac97.generation_fetched==(variant ? 64u : 32u));
        hw.sr=AC97_BCIS; CHECK(ac97_irq(11,"ac97") && !hw.sr && ac97.irqs==1);
        CHECK(!ac97_irq(11,"ac97")); /* Linux's buggy_irq workaround is not adopted */
        bool swapped=false; for (unsigned i=0;i<hw.trace_count;i++)
            if (hw.trace[i].port==0x200+(variant ? 0x16 : 0x18) && hw.trace[i].value==AC97_BCIS) swapped=true;
        CHECK(swapped && !ac97_stop(&ac97));
        CHECK(!ac97_codec_restore(&ac97));
    }
    memset(&ac97,0,sizeof(ac97));
    puts("Intel-compatible variants: SiS swapped SR/PICB, byte counts/output gate; nForce/nForce2 silent proof and own-source IRQ acknowledgement PASS");
}
static void test_second_set(void)
{
    test_physical_rate(); test_audio_routes(); test_ich_variants();
    unsigned baseline=pages_used;
    static struct audio_frame marker[256]; for (unsigned i=0;i<256;i++) marker[i]=(struct audio_frame){1234,-2345};
    for (unsigned which=0;which<12;which++) {
        enum audio_backend_kind kind=which==0 ? AUDIO_ES1370 : which<=2 || (which>=8 && which<=10) ? AUDIO_ES1371 : which==3 ? AUDIO_SB16 : which==4 ? AUDIO_VIA686 : AUDIO_VIA8233;
        second_setup(kind,which==2 ? 7 : which==5 ? 0x30 : which==6 ? 0x40 : 0x50);
        if (which>=8 && which<=10) { audio_dma.pci.device=0x5880; audio_dma.pci.revision=which-6; }
        if (which==11) audio_dma.pci.revision=0x20;
        CHECK(!backend_dma_init(&audio_dma));
        struct audio_dma_device *d=&audio_dma;
        CHECK(d->state.ready && !d->state.running && second.starts==1 && !d->state.generation_fetched);
        CHECK(audio.caps.output_rate==(kind==AUDIO_ES1370 || kind==AUDIO_SB16 ? 44100 : 48000));
        CHECK(!second_pic_masks);
        CHECK(kind==AUDIO_SB16 ? second_pic_unmasks==1 && !second_pic_masked && second_isa_handler : second_irq_adds==1);
        unsigned pic_before=second_pic_masks+second_pic_unmasks;
        if (kind==AUDIO_ES1371) { CHECK(second.src_writes>=138 && second.src[0x75]==0x4000 && second.src[0x77]==0 && second.src[0x7e]==4096); }
        if (kind==AUDIO_ES1370) CHECK(second.codec[0x11]==12 && second.codec[0x16]==3 && second.codec[2]==6);
        CHECK(!audio_backend_master(&(struct ciuki_mixer_master){.mute=1}));
        CHECK(d->state.master&0x8000); CHECK(!audio_backend_master(&(struct ciuki_mixer_master){0}));
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker));
        CHECK(!audio_backend_start() && second.starts==2);
        if (kind==AUDIO_SB16) CHECK(second.dma_mode==0x59 && second.dma_page==((d->phys>>16)&0xfe) && second.dma_address==((d->phys>>1)&0xffff) && second.count_word==16383);
        if (kind==AUDIO_VIA8233) CHECK(second.regs[(d->channel+8)/4]==(d->channel==0x40 ? 0xff000021 : 0xff3fffff));
        for (unsigned i=1;i<97;i++) {
            second.frame=i*256%(32*256); second.tick+=6;
            CHECK(!audio_backend_progress() && d->state.retired==i && d->state.fetched==(uint64_t)i*256);
            CHECK(!audio_backend_publish(marker));
        }
        /* Publication cannot rewrite a current or prefetched period. */
        uint64_t published=d->state.published; d->state.published=d->state.retired;
        struct audio_frame canary=d->pcm[second.frame];
        CHECK(audio_backend_publish(marker)==-EIO && !memcmp(&canary,&d->pcm[second.frame],sizeof(canary)));
        d->state.published=published;
        CHECK(!audio_backend_stop() && !d->state.running && d->state.fetched==96*256);
        CHECK(!d->state.published && !d->pcm[0].left && !d->pcm[8191].right);
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker)); CHECK(!audio_backend_start());
        unsigned causes=kind==AUDIO_SB16 ? 2 : kind>=AUDIO_VIA686 ? 1 : 2;
        if (kind==AUDIO_SB16) second.mixer[0x82]=causes;
        else second.regs[(kind>=AUDIO_VIA686 ? d->channel : 4)/4]|=causes;
        second.frame=256; second.tick+=6;
        CHECK(backend_irq(d->state.irq,d->name) && d->state.irqs==1);
        CHECK(!backend_irq(d->state.irq,d->name));
        CHECK(!audio_backend_publish(marker));
        second.frame=9*256; second.tick+=40;
        if (kind==AUDIO_SB16) second.mixer[0x82]=2;
        else second.regs[(kind>=AUDIO_VIA686 ? d->channel : 4)/4]|=causes;
        CHECK(backend_irq(d->state.irq,d->name) && d->inhibited && !audio.error);
        CHECK(kind==AUDIO_SB16 ? second.mask : !second.active);
        audio_backend_shutdown(); CHECK(!audio.quarantined && pages_used==baseline && !audio.commitment_pages);
        CHECK(second_pic_masks+second_pic_unmasks==pic_before);
        if (kind==AUDIO_SB16) {
            CHECK(second.mask && !second.active && !(second.mixer[0x82]&2) && !second_isa_handler && !second_pic_masked);
            /* Reattach the stopped device on the still-open line. */
            audio.error=0;
            CHECK(!backend_dma_init(d));
            CHECK(second_pic_masks+second_pic_unmasks==pic_before && second_isa_handler);
            audio_backend_shutdown();
            CHECK(second_pic_masks+second_pic_unmasks==pic_before && pages_used==baseline);
        }
    }
    second_setup(AUDIO_VIA8233,0x30); second.start_delay=3;
    CHECK(!backend_dma_init(&audio_dma) && audio_dma.state.ready && !audio_dma.state.running);
    for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker)); CHECK(!audio_backend_start());
    second.zero_remaining=true; CHECK(audio_backend_progress()==-EAGAIN && !audio_dma.state.generation_fetched);
    second.zero_remaining=false; second.tick+=4; second.frame=256;
    CHECK(!audio_backend_progress() && audio_dma.state.retired==1);
    audio_backend_shutdown(); CHECK(pages_used==baseline && !audio.quarantined);
    for (unsigned slot=12;slot<16;slot++) {
        second_setup(AUDIO_VIA8233,0x30); second_claim_failure=slot;
        CHECK(backend_dma_init(&audio_dma)==-EBUSY && !second.starts && !audio.quarantined && pages_used==baseline);
    }
    second_setup(AUDIO_SB16,0); alloc_fail=0;
    CHECK(backend_dma_init(&audio_dma)==-ENOMEM && !second.starts && pages_used==baseline);
    alloc_fail=-1;
    second_setup(AUDIO_ES1371,0); second.src_busy=true;
    CHECK(backend_dma_init(&audio_dma)==-ETIMEDOUT && !second.starts && pages_used==baseline && !audio.quarantined);
    second_setup(AUDIO_ES1371,0); second.codec_busy=true;
    CHECK(backend_dma_init(&audio_dma)==-ETIMEDOUT && !second.starts && pages_used==baseline && !audio.quarantined);
    second_setup(AUDIO_VIA8233,0x30); second.codec_busy=true;
    CHECK(backend_dma_init(&audio_dma)==-ETIMEDOUT && !second.starts && pages_used==baseline && !audio.quarantined);
    second_setup(AUDIO_ES1371,0); second.reject_codec=true;
    CHECK(backend_dma_init(&audio_dma)==-EIO && !second.starts && pages_used==baseline && !audio.quarantined);
    second_setup(AUDIO_VIA8233,0x30); second.reject_codec=true;
    CHECK(backend_dma_init(&audio_dma)==-EIO && !second.starts && pages_used==baseline && !audio.quarantined);
    for (unsigned kind=AUDIO_ES1370;kind<=AUDIO_VIA8233;kind+=3) {
        second_setup(kind,0x30); second.active=true; second.regs[0]=kind==AUDIO_ES1370 ? ES_DAC2_ENABLE : 0;
        CHECK(backend_dma_init(&audio_dma)==-EBUSY && !second.writes && !audio_dma.touched && pages_used==baseline);
    }
    second_pci_count=1;
    second_pci[0]=(struct pci_func){.vendor=0x1039,.device=0x7012,.class_code=4,.subclass=1,.command=4,.bar={0x101,0x201}};
    CHECK(audio_backend_init()==-ENODEV && !strcmp(audio.reason,"firmware_state_unreadable") && !ac97.touched);
    CHECK(!dma_selected && pages_used==baseline); second_pci_count=0;
    /* No inferred firmware resource reassignment. */
    second_setup(AUDIO_SB16,0); second.mixer[0x81]=0x42;
    CHECK(backend_dma_init(&audio_dma)==-ENODEV && !second.dsp_bytes && !second.starts && pages_used==baseline);
    CHECK(!strcmp(audio.reason,"isa_resources_unqualified"));
    /* Silent progress is mandatory; a codec identity alone is insufficient. */
    second_setup(AUDIO_ES1370,0); second.no_progress=true;
    CHECK(backend_dma_init(&audio_dma)==-ETIMEDOUT && !audio_dma.state.ready && pages_used==baseline);
    for (unsigned kind=AUDIO_ES1371;kind<=AUDIO_VIA686;kind+=2) {
        second_setup(kind,0x20); second.zero_codec=true;
        CHECK(backend_dma_init(&audio_dma)<0 && !second.starts && !audio_dma.state.ready);
        /* A codec refusal has never enabled an engine; rollback releases. */
        CHECK(pages_used==baseline && !audio.commitment_pages);
    }
    /* Failed stop pins both data and descriptor storage, permanently. */
    for (unsigned kind=AUDIO_ES1370;kind<=AUDIO_VIA8233;kind++) {
        second_setup(kind,0x30); CHECK(!backend_dma_init(&audio_dma));
        for (unsigned i=0;i<8;i++) CHECK(!audio_backend_publish(marker)); CHECK(!audio_backend_start());
        second.stop_stuck=true; unsigned held=pages_used;
        unsigned masks_before=second_pic_masks, unmasks_before=second_pic_unmasks;
        audio_backend_shutdown(); CHECK(audio.quarantined && audio_dma.state.quarantine && pages_used==held);
        CHECK(strstr(audio_log,"failure stage=shutdown backend=") &&
              strstr(audio_log,"reason=stop_unproved") && strstr(audio_log,"fallback=quarantine"));
        if (kind==AUDIO_SB16) CHECK(second_pic_masked && second_pic_masks==masks_before+1 && second_pic_unmasks==unmasks_before);
        second.stop_stuck=false; audio_backend_shutdown(); CHECK(pages_used==held && audio_dma.state.quarantine);
        if (kind==AUDIO_SB16) CHECK(second_pic_masks==masks_before+1 && second_pic_unmasks==unmasks_before);
        /* Fixture disposal only: real quarantined storage is never freed. */
        for (unsigned i=0;i<8;i++) pmm_free(audio_dma.phys+i*PAGE_SIZE);
        if (audio_dma.desc_phys) pmm_free(audio_dma.desc_phys);
        audio.commitment_pages=0;
    }
    dma_selected=false; memset(&audio_dma,0,sizeof(audio_dma)); memset(&ac97,0,sizeof(ac97));
    audio.backend_pci=(struct pci_func){0}; audio.backend_name=0; audio.backend_touched=false;
    audio.error=0; audio.ready=audio.running=audio.quarantined=false; memset(&audio.caps,0,sizeof(audio.caps));
    CHECK(pages_used==baseline);
    puts("audio PIC: publish/start/progress/IRQ/mute/stop/clean shutdown+reattach=0 writes after init; SB16 unproved stop=1 quarantine mask PASS");
    puts("second audio set: synthetic ES1370/ES1371/CT5880, ISA SB16, VIA 686/8233/8233A/8235: progress, wrap, IRQ, rate, mute and quarantine PASS");
}
