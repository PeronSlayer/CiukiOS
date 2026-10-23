/* f1-107: literal physical register models, separate from driver offset helpers. */
static void test_ac97_layout_matrix(void)
{
    static const struct { uint16_t vendor, device; unsigned bytes; } ids[]={
        {0x8086,0x2415,64},{0x8086,0x2425,64},{0x8086,0x2445,64},
        {0x8086,0x2485,64},{0x8086,0x24c5,128},{0x8086,0x24d5,128},
        {0x1039,0x7012,128},{0x10de,0x01b1,128},{0x10de,0x006a,128},
        {0x10de,0x008a,128},{0x10de,0x00da,128},{0x10de,0x00ea,128}
    };
    static struct ac97_bd bdl[32]; static struct audio_frame buffers[32][256];
    for(unsigned n=0;n<ARRAY_SIZE(ids);n++) {
        hardware_setup(&ac97); hw.any_pcm=hw.ich3=true;
        ac97.pci=(struct pci_func){.vendor=ids[n].vendor,.device=ids[n].device,.class_code=4,.subclass=1,
            .bar={0x101,0x201},.command=1,.irq_line=11,.irq_pin=2};
        CHECK(supported(&ac97.pci) && controller_layout(&ac97,0));
        CHECK(ac97.nam==0x100 && ac97.nabm==0x200 && ac97.nabm_bytes==ids[n].bytes);
        CHECK(controller_idle(&ac97));
        if(ac97.ich4 || ac97.nvidia) {
            unsigned channel=ac97.nvidia ? 7 : 6;
            hw.sibling_sr[channel]=0; CHECK(!controller_idle(&ac97));
            hw.sibling_sr[channel]=0xffff; CHECK(!controller_idle(&ac97));
            hw.sibling_sr[channel]=1; hw.sibling_cr[channel]=1; CHECK(!controller_idle(&ac97));
            hw.sibling_cr[channel]=0;
        }
        ac97.bdl=bdl; ac97.bdl_phys=0x90000;
        for(unsigned i=0;i<32;i++) { ac97.pcm[i]=buffers[i]; bdl[i].address=0xa0000+i*1024; }
        CHECK(!controller_activate(&ac97) && hw.starts==1 && !ac97.running);
        CHECK(trace_at(0x210,0x90000)<hw.trace_count); /* PCM-out BDBAR */
        CHECK(trace_at(0x200+(ac97.sis ? 0x18 : 0x16),0x1c)<hw.trace_count);
        for(unsigned i=0;i<8;i++)CHECK(!ac97_publish(&ac97,buffers[0]));
        CHECK((bdl[0].length & 0xffff)==(ac97.sis ? 1024u : 512u));
        ac97.ready=true; hw.ich3=false; CHECK(!ac97_start(&ac97));
        hw.remaining=ac97.sis ? 768 : 384; hw.sr=0;
        CHECK(!ac97_progress(&ac97) && ac97.fetched==64);
        hw.sr=AC97_BCIS; CHECK(ac97_irq(11,"ac97") && !hw.sr);
        CHECK(!ac97_irq(11,"ac97")); hw.sr=0xffff; CHECK(!ac97_irq(11,"ac97")); hw.sr=0;
        CHECK(!ac97_stop(&ac97) && !ac97_codec_restore(&ac97));
        struct ac97_device bad=ac97;
        bad.pci.bar[1]=0x10001; CHECK(!controller_layout(&bad,0));
        bad.pci.bar[1]=0x200; CHECK(!controller_layout(&bad,0));
    }
    struct pci_func p={.vendor=0x10b9,.device=0x5451,.class_code=4,.subclass=1}; CHECK(!supported(&p));
    p.vendor=0x10de; p.device=0x0059; CHECK(!supported(&p)); /* outside directed generation */
    memset(&ac97,0,sizeof(ac97));
    puts("f1-107 ICH0-5/SiS/nForce1-3: literal BDBAR/SR/PICB, units, silent proof, sibling refusal, own IRQ and stop PASS");
}
static void test_ac97_codec_matrix(void)
{
    static const uint32_t ids[]={0x41445340,0x41445348,0x41445360,0x41445361,0x41445363,
        0x41445370,0x41445372,0x41445374,0x41445375,0x414c4720,0x414c4721,0x414c4722,0x414c4723,
        0x414c4767,0x414c4781,0x414c4783,0x414c4795,0x43525937,0x83847600,0x83847604,
        0x83847605,0x83847608,0x83847609,0x83847644,0x83847650,0x83847652,0x83847656,
        0x83847658,0x83847666,0x434d4941,0x434d4961,0x49434551,0x49434552,
        0x574d4c00,0x574d4c03,0x574d4c04,0x574d4c05,0x574d4c09,0x12345678};
    CHECK(!strcmp(ac97_codec_lookup(0x414c4721)->name,"ALC650D"));
    CHECK(!strcmp(ac97_codec_lookup(0x414c4781)->name,"ALC658D"));
    CHECK(!strcmp(ac97_codec_lookup(0x43525937)->name,"CS4299"));
    CHECK(!strcmp(ac97_codec_lookup(0x43525938)->name,"Generic AC'97"));
    CHECK(!strcmp(ac97_codec_lookup(0x83847601)->name,"Generic AC'97"));
    CHECK(!ac97_codec_lookup(0x434d4941)->pcm_mask && !ac97_codec_lookup(0x434d4961)->master_mask);
    for(unsigned n=0;n<ARRAY_SIZE(ids);n++) for(unsigned vra=0;vra<2;vra++) {
        struct ac97_device d; hardware_setup(&d);
        hw.codec[0x7c/2]=ids[n]>>16; hw.codec[0x7e/2]=ids[n]; hw.codec[0]=0x10;
        hw.codec[0x28/2]=vra; hw.codec[0x2a/2]=vra; hw.codec[4/2]=0x8000;
        hw.codec_model=true;
        uint16_t original[128]; memcpy(original,hw.codec,sizeof(original));
        CHECK(!ac97_codec_prepare(&d) && d.codec_id==ids[n] && d.rate==48000);
        CHECK(d.codec.variable_rate==(vra!=0));
        CHECK(!ac97_set_mute(&d,true));
        if(d.codec.profile->master_mask) CHECK((hw.codec[1] & (d.codec.profile->master_mask & 0x8080))==(d.codec.profile->master_mask & 0x8080));
        CHECK(!ac97_set_mute(&d,false));
        if(ids[n]==0x41445360 || ids[n]==0x41445361) CHECK((hw.codec[4/2] & 0x3f3f)==0x0404);
        if(ids[n]==0x434d4941) CHECK(hw.codec[0x18/2]==0x8808);
        if(ids[n]==0x434d4961) CHECK(d.software_master && hw.codec[1]==0x8000);
        CHECK(!ac97_codec_restore(&d) && !d.codec.saved_count);
        for(unsigned reg=0;reg<128;reg++) {
            if(reg==0x26/2)continue; /* RO readiness is synthesized */
            CHECK(hw.codec[reg]==original[reg]);
        }
    }
    struct ac97_device d;
    hardware_setup(&d); hw.codec[0x7c/2]=0x574d; hw.codec[0x7e/2]=0x4c13;
    CHECK(ac97_codec_prepare(&d)==-EOPNOTSUPP && !strcmp(d.failure.access,"codec_route_unsupported") && !hw.starts);
    hardware_setup(&d); d.pci=(struct pci_func){.vendor=0x8086,.sub_vendor=0x1014,.sub_device=0x0534};
    CHECK(!ac97_codec_prepare(&d) && (hw.codec[0x26/2] & 0x8000)); CHECK(!ac97_codec_restore(&d));
    hardware_setup(&d); d.pci=(struct pci_func){.vendor=0x8086,.sub_vendor=0x1014,.sub_device=0x0222};
    CHECK(!ac97_codec_prepare(&d) && !(hw.codec[0x26/2] & 0x8000)); CHECK(!ac97_codec_restore(&d));
    hardware_setup(&d); hw.codec[0x7c/2]=0x414c; hw.codec[0x7e/2]=0x4760;
    d.pci.sub_vendor=0x1462; d.pci.sub_device=0x0131; hw.codec[0x7a/2]=2;
    CHECK(!ac97_codec_prepare(&d) && !(hw.codec[0x7a/2] & 2));
    CHECK(!ac97_codec_restore(&d) && hw.codec[0x7a/2]==2);
    hardware_setup(&d); hw.codec[0x7c/2]=0x4144; hw.codec[0x7e/2]=0x5370;
    hw.read_timeout_reg=0x76;
    CHECK(ac97_codec_prepare(&d)==-EIO && !hw.starts && !strcmp(d.failure.stage,"codec_quirks"));
    puts("f1-107 codecs: exact/masked names, VRA/fixed fixtures, mixer omissions/stereo mutes, board EAPD/jacks, full restore and refusal PASS");
}
struct codec_patch_model { uint16_t regs[64]; unsigned updates, fail; };
static int codec_patch_read(void *cookie,uint16_t reg,uint16_t *value)
{ *value=((struct codec_patch_model *)cookie)->regs[reg/2]; return 0; }
static int codec_patch_update(void *cookie,uint16_t reg,uint16_t value,uint16_t mask)
{
    struct codec_patch_model *m=cookie;
    m->regs[reg/2]=(m->regs[reg/2] & ~mask) | (value & mask);
    return ++m->updates==m->fail ? -EIO : 0; /* failed write has still landed */
}
static void test_ac97_partial_quirks(void)
{
    const uint32_t ids[]={0x41445370,0x414c4790,0x434d4961};
    for(unsigned id=0;id<ARRAY_SIZE(ids);id++) for(unsigned fail=1;fail<=5;fail++) {
        struct codec_patch_model m={0}; uint16_t old[64];
        for(unsigned i=0;i<64;i++)m.regs[i]=(uint16_t)(i*781+31);
        memcpy(old,m.regs,sizeof(old)); m.fail=fail;
        struct ac97_codec_policy s; struct pci_func p={0};
        ac97_codec_policy_init(&s,&p,ids[id],0,1);
        struct ac97_codec_ops io={&m,codec_patch_read,codec_patch_update};
        int e=ac97_codec_quirks(&s,&p,&io);
        if(m.updates>=fail)CHECK(e==-EIO); else CHECK(!e);
        m.fail=0;
        CHECK(!ac97_codec_unpatch(&s,&io) && !s.saved_count && !memcmp(old,m.regs,sizeof(old)));
    }
    puts("f1-107 codec rollback: each AD/Realtek/C-Media vendor write fails after landing, reverse restoration preserves unrelated bits PASS");
}
/* ALi model exposes a single 256-byte BAR and rejects Intel NAM/global/RUN
 * semantics. Expected offsets and bit fields are literal donor facts. */
struct ali_model {
    uint32_t config_address, scr, dma, fifo[3], interface, interrupts;
    uint16_t command, command_value, result, codec[64];
    uint8_t cr[11], sr[11], civ, piv, lvi;
    uint16_t remaining;
    uint32_t bdbar;
    uint64_t tick, completion_at;
    bool busy, ready_stuck, completion_stuck, stop_stuck, reset_stuck, start_stuck, no_progress;
    unsigned completion, starts, resets, writes, nam_accesses;
    struct { unsigned off, width; uint32_t value; } trace[512];
    unsigned traces;
};
static struct ali_model ali_hw;
static uint32_t ali_model_read(void *cookie,uint16_t port,unsigned width)
{
    struct ali_model *h=cookie;
    if(port==0xcfc) { CHECK((h->config_address & 255)==4 && width==4); return h->command; }
    CHECK(port>=0x1000 && port<0x1100);
    unsigned off=port-0x1000;
    if(off==0) { CHECK(width==4); return h->scr; }
    if(off==8) { CHECK(width==4); return h->dma; }
    if(off==0xc || off==0x1c || off==0x2c) { CHECK(width==4); return h->fifo[off==0xc ? 0 : off==0x1c ? 1 : 2]; }
    if(off==0x10) { CHECK(width==4); return h->interface; }
    if(off==0x18) { CHECK(width==4); return h->interrupts; }
    if(off==0x34) { CHECK(width==4); return h->ready_stuck ? 0 : 0x80; }
    if(off==0x38) { CHECK(width==1); return 8 | ((!h->completion_stuck && h->tick>=h->completion_at) ? h->completion : 0); }
    if(off==0x3c) { CHECK(width==4); return h->busy ? 0x80000000u : 0; }
    if(off==0x24) { CHECK(width==2); return h->result; }
    if(off>=0x40 && off<=0xeb) {
        unsigned i=(off-0x40)/16, reg=off & 15;
        if(reg==11) { CHECK(width==1); return h->cr[i]; }
        if(reg==6) { CHECK(width==1); return h->sr[i]; }
        if(i==1) {
            if(reg==0) { CHECK(width==4); return h->bdbar; }
            if(reg==4) { CHECK(width==1); return h->civ; }
            if(reg==5) { CHECK(width==1); return h->lvi; }
            if(reg==8) { CHECK(width==2); return h->remaining; }
            if(reg==10) { CHECK(width==1); return h->piv; }
        }
    }
    CHECK(!"unsupported ALi read offset/width"); return UINT32_MAX;
}
static void ali_model_write(void *cookie,uint16_t port,uint32_t value,unsigned width)
{
    struct ali_model *h=cookie;
    if(port==0xcf8) { CHECK(width==4); h->config_address=value; return; }
    if(port==0xcfc) { CHECK((h->config_address & 255)==4 && width==2); h->command=value; return; }
    CHECK(port>=0x1000 && port<0x1100);
    unsigned off=port-0x1000; h->writes++;
    if(h->traces<ARRAY_SIZE(h->trace)) { unsigned i=h->traces++; h->trace[i].off=off; h->trace[i].width=width; h->trace[i].value=value; }
    if(off==0) { CHECK(width==4); h->scr=value; h->resets++; return; }
    if(off==0xc || off==0x1c || off==0x2c) { CHECK(width==4); h->fifo[off==0xc ? 0 : off==0x1c ? 1 : 2]=value; return; }
    if(off==0x10) { CHECK(width==4); h->interface=value; return; }
    if(off==0x14) { CHECK(width==4 && !value); return; }
    if(off==0x18) { CHECK(width==4 && value==(1u<<17)); h->interrupts&=~value; return; }
    if(off==0x20) { CHECK(width==2); h->command_value=value; return; }
    if(off==0x22) {
        CHECK(width==2 && !(value & ~255u)); unsigned reg=value & 0x7e;
        h->completion_at=h->tick+2;
        if(value & 0x80) { h->result=h->codec[reg/2]; if(reg==0x26)h->result|=15; h->completion=2; }
        else { if(reg)h->codec[reg/2]=h->command_value; h->completion=1; }
        return;
    }
    if(off==8) {
        CHECK(width==4 && !(value & ~0x07ff07ffu));
        h->dma=value;
        if((value & 0x20002)==2) {
            CHECK(h->command & 4); h->starts++;
            if(!h->start_stuck) { h->sr[1]=0; h->remaining=512; h->piv=1; }
        }
        if((value & (1u<<17)) && !h->stop_stuck)h->sr[1]|=1;
        return;
    }
    if(off>=0x40 && off<=0xeb) {
        unsigned i=(off-0x40)/16, reg=off & 15;
        if(reg==11) {
            CHECK(width==1 && !(value & 1)); /* Intel RUN is forbidden */
            if(value & 2) { CHECK(h->sr[i] & 1); h->cr[i]=h->reset_stuck ? 2 : 0; if(i==1) {h->civ=h->piv=h->lvi=0;h->remaining=0;} }
            else h->cr[i]=value;
            return;
        }
        if(reg==6) { CHECK(width==1 && !(value & ~0x1cu)); h->sr[i]&=~value; return; }
        if(i==1 && reg==0) { CHECK(width==4); h->bdbar=value; return; }
        if(i==1 && reg==5) { CHECK(width==1); h->lvi=value; return; }
    }
    CHECK(!"unsupported ALi write offset/width");
}
static uint64_t ali_model_now(void *cookie) { return ((struct ali_model *)cookie)->tick; }
static void ali_model_sleep(void *cookie,uint32_t ms)
{
    struct ali_model *h=cookie; h->tick+=ms;
    if(!h->no_progress && !(h->sr[1] & 1) && (h->dma & 0x20002)==2 && h->remaining>ms*96)h->remaining-=ms*96;
}
static void ali_setup(void)
{
    memset(&ac97,0,sizeof(ac97)); memset(&ali_hw,0,sizeof(ali_hw));
    ac97.pci=(struct pci_func){.vendor=0x10b9,.device=0x5455,.class_code=4,.subclass=1,.bar={0x1001},.command=1,.irq_line=11,.irq_pin=1};
    ac97.command=1; ali_hw.command=1;
    CHECK(supported(&ac97.pci) && controller_layout(&ac97,0));
    ac97.io=(struct ac97_io){&ali_hw,ali_model_read,ali_model_write,ali_model_now,ali_model_sleep};
    for(unsigned i=0;i<11;i++)ali_hw.sr[i]=1;
    ali_hw.codec[0x7c/2]=0x4144; ali_hw.codec[0x7e/2]=0x5348;
    ali_hw.codec[0x26/2]=15; ali_hw.codec[0x28/2]=1; ali_hw.codec[0x2c/2]=48000;
    ali_hw.codec[1]=0x8000; ali_hw.codec[0x18/2]=0x8808;
    static struct ac97_bd bdl[32]; static struct audio_frame buffers[32][256];
    memset(bdl,0,sizeof(bdl)); memset(buffers,0,sizeof(buffers));
    ac97.bdl=bdl; ac97.bdl_phys=0x90000;
    for(unsigned i=0;i<32;i++) { ac97.pcm[i]=buffers[i]; bdl[i].address=0xa0000+i*1024; }
}
static void test_ali_layout(void)
{
    ali_setup(); CHECK(ac97.ali && !ac97.nam_bytes && ac97.nabm_bytes==256 && controller_idle(&ac97));
    ali_hw.sr[10]=0; CHECK(!controller_idle(&ac97)); ali_hw.sr[10]=1;
    ali_hw.dma=1<<9; CHECK(!controller_idle(&ac97)); ali_hw.dma=0;
    CHECK(!controller_activate(&ac97) && ali_hw.starts==1 && !ac97.running && ac97.rate==48000);
    CHECK(ali_hw.interface==2 && ali_hw.fifo[0]==0x83838383 && !ac97.ready);
    for(unsigned i=0;i<8;i++)CHECK(!ac97_publish(&ac97,ac97.pcm[0]));
    ac97.ready=true; CHECK(!ac97_start(&ac97));
    CHECK(ali_hw.cr[1]==0x10 && (ali_hw.dma & 0x20002)==2 && (ac97.bdl[0].length & 65535)==512);
    ali_hw.remaining=384; CHECK(!ac97_progress(&ac97) && ac97.fetched==64);
    ali_hw.interrupts=(1u<<17) | (1u<<18); ali_hw.sr[1]=AC97_BCIS;
    CHECK(ac97_irq(11,"ac97") && ali_hw.interrupts==(1u<<18) && !ali_hw.sr[1]);
    CHECK(!ac97_irq(11,"ac97"));
    CHECK(!ac97_stop(&ac97) && !ac97_codec_restore(&ac97));
    ali_setup(); ali_hw.busy=true; uint16_t value; unsigned writes=ali_hw.writes;
    CHECK(ac97_codec_read(&ac97,2,&value,1000)==-ETIMEDOUT && ali_hw.tick==100 && ali_hw.writes==writes);
    ali_setup(); ali_hw.completion_stuck=true;
    CHECK(ac97_codec_read(&ac97,2,&value,1000)==-ETIMEDOUT && ali_hw.tick==100);
    ali_setup(); ali_hw.ready_stuck=true;
    CHECK(ac97_controller_reset(&ac97)==-ETIMEDOUT && ali_hw.tick==1000 && !ali_hw.starts);
    ali_setup(); ali_hw.no_progress=true;
    CHECK(controller_activate(&ac97)==-ETIMEDOUT && !ac97.ready && !strcmp(ac97.failure.stage,"dma_start"));
    ali_setup(); CHECK(!controller_activate(&ac97));
    ac97.ready=true; for(unsigned i=0;i<8;i++)CHECK(!ac97_publish(&ac97,ac97.pcm[0])); CHECK(!ac97_start(&ac97));
    ali_hw.stop_stuck=true; uint32_t address=ac97.bdl[0].address; unsigned resets=ali_hw.resets;
    CHECK(ac97_stop(&ac97)==-EIO && ac97.quarantine && ac97.bdl[0].address==address && ali_hw.resets==resets);
    unsigned starts=ali_hw.starts; CHECK(ac97_start(&ac97)==-EIO && ali_hw.starts==starts);
    ali_setup(); ali_hw.reset_stuck=true;
    CHECK(controller_activate(&ac97)==-EIO && !ali_hw.starts && ac97.quarantine);
    ali_setup(); ac97.pci.bar[0]=0x1003; CHECK(!controller_layout(&ac97,0));
    memset(&ac97,0,sizeof(ac97));
    puts("f1-107 ALi M5455: single BAR, command/completion deadlines, FIFO/DMACR, literal offsets/widths, sibling refusal, IRQ isolation, stop quarantine PASS");
}
static void test_ac97_via_qualification(void)
{
    static const unsigned revisions[]={0x20,0x30,0x40,0x50,0x60};
    for(unsigned i=0;i<ARRAY_SIZE(revisions);i++) {
        second_setup(AUDIO_VIA8233,revisions[i]); enum audio_backend_kind kind;
        CHECK(via82xx_match(&audio_dma.pci,&kind) && kind==AUDIO_VIA8233);
        unsigned before=pages_used;
        CHECK(!backend_dma_init(&audio_dma) && audio_dma.channel==(revisions[i]==0x40 ? 0x40u : 0));
        CHECK(audio_dma.state.rate==48000 && audio_dma.codec.profile && !audio_dma.codec.variable_rate);
        audio_backend_shutdown(); CHECK(pages_used==before);
    }
    second_setup(AUDIO_VIA8233,0x70); enum audio_backend_kind kind;
    CHECK(!via82xx_match(&audio_dma.pci,&kind));
    second_setup(AUDIO_VIA8233,0x60); second.regs[0x60/4]=0x80;
    CHECK(via_prepare(&audio_dma)==-EBUSY && !audio_dma.touched);
    second_setup(AUDIO_VIA8233,0x60); second.codec[0x7c/2]=0x414c; second.codec[0x7e/2]=0x4760;
    CHECK(!backend_dma_init(&audio_dma) && !strcmp(audio_dma.codec.profile->name,"ALC655")); audio_backend_shutdown();
    const uint32_t codec_ids[]={0x41445370,0x41445374,0x414c4790,0x434d4941,0x434d4961};
    for(unsigned i=0;i<ARRAY_SIZE(codec_ids);i++) {
        second_setup(AUDIO_VIA8233,0x60); second.codec[0x7c/2]=codec_ids[i]>>16; second.codec[0x7e/2]=codec_ids[i];
        second.codec[0]=0x10; second.codec[4/2]=0x8000;
        uint16_t old[64]; memcpy(old,second.codec,sizeof(old));
        CHECK(!backend_dma_init(&audio_dma) && audio_dma.codec_saved_count==(codec_ids[i]==0x434d4961 ? 4u : 5u));
        CHECK(!audio_backend_master(&audio.master));
        audio_backend_shutdown(); CHECK(!memcmp(old,second.codec,sizeof(old)));
    }
    second_setup(AUDIO_VIA8233,0x60); second.codec_busy=true;
    CHECK(backend_dma_init(&audio_dma)==-ETIMEDOUT && !audio_dma.state.ready && !second.starts);
    second_setup(AUDIO_VIA8233,0x60); second.zero_codec=true;
    CHECK(backend_dma_init(&audio_dma)==-EIO && !audio_dma.state.ready && !second.starts);
    memset(&audio_dma,0,sizeof(audio_dma)); dma_selected=false;
    puts("f1-107 VIA686/8233/A/8235/8237: revision admission, DXS/multichannel stereo, shared codec, sibling/busy/identity refusal PASS");
}
static void test_ac97_route_qualification(void)
{
    second_setup(AUDIO_VIA8233,0x60);
    uint8_t table[48]={0}; memcpy(table,"$PIR",4); table[5]=1; table[6]=48; table[9]=8;
    table[33]=5*8; table[34]=1; table[36]=8;
    struct pci_func card={.vendor=0x1106,.device=0x3059,.dev=5,.irq_pin=1,.irq_line=11};
    second_pci_count=1;
    second_pci[0]=(struct pci_func){.vendor=0x1106,.device=0x3227,.dev=1,.class_code=6,.subclass=1};
    route_identity(table,sizeof(table)); second.cfg[0x54/4]=11u<<12;
    CHECK(audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    table[34]=4; route_identity(table,sizeof(table)); second.cfg[0x54/4]=11u<<28;
    CHECK(audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    table[34]=1; second_pci[0].vendor=0x10b9; second_pci[0].device=0x1563;
    card.vendor=0x10b9; card.device=0x5455; route_identity(table,sizeof(table));
    second.cfg[0x48/4]=9; CHECK(audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    second.cfg[0x48/4]=11; CHECK(!audio_pir_route(&audio_dma.io,&card,table,sizeof(table))); /* 11 encodes IRQ12 */
    table[34]=16; second.cfg[0x4c/4]=9u<<28; route_identity(table,sizeof(table));
    CHECK(audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    table[34]=17; route_identity(table,sizeof(table)); CHECK(!audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    table[34]=1; second_pci[0].vendor=0x10de; second_pci[0].device=0x00d0;
    card.vendor=0x10de; card.device=0x00da; route_identity(table,sizeof(table));
    CHECK(audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    table[36]=4; route_identity(table,sizeof(table)); CHECK(!audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    table[36]=8; route_identity(table,sizeof(table));
    table[14]^=1; table[31]^=1; CHECK(!audio_pir_route(&audio_dma.io,&card,table,sizeof(table)));
    second_pci[0]=(struct pci_func){.vendor=0x8086,.device=0x24d0,.dev=31,.class_code=6,.subclass=1};
    card=(struct pci_func){.vendor=0x8086,.device=0x24d5,.dev=31,.fn=5,.irq_pin=2,.irq_line=11};
    ac97.io=audio_dma.io; second.cfg[0x60/4]=11u<<8; qualification_elcr[1]=8;
    CHECK(routed(&card)); second.cfg[0x60/4]=0x8bu<<8; CHECK(!routed(&card));
    second.cfg[0x60/4]=11u<<8; qualification_elcr[1]=0; CHECK(!routed(&card));
    qualification_elcr[1]=8; card.irq_pin=1; CHECK(!routed(&card)); qualification_elcr[1]=0;
    second_pci_count=0; dma_selected=false; memset(&ac97,0,sizeof(ac97));
    puts("f1-107 routes: ICH5 PIRQB/ELCR, VT8237 A/D nibbles, ALi encoded IRQs/link bounds, nForce3 firmware bitmap/identity refusal PASS");
}
static void test_ac97_qualification(void)
{
    unsigned before=pages_used;
    test_ac97_layout_matrix(); test_ac97_codec_matrix(); test_ac97_partial_quirks(); test_ali_layout(); test_ac97_via_qualification(); test_ac97_route_qualification();
    CHECK(pages_used==before);
}
