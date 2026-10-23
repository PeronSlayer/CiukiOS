/* Production add-in backends, independently modeled clocks/registers/media.
 * SPDX-License-Identifier: GPL-2.0-only */
static struct {
    uint8_t regs[0x4000];
    unsigned writes, drop, input_khz, fractional_us, counter_reads;
    bool torn_counter, autotiming;
} addin_model;
static uint32_t addin_test_read(void *ctx, unsigned reg, unsigned bytes)
{
    (void)ctx;CHECK(reg+bytes<=sizeof(addin_model.regs));
    if((reg==0x1120 || reg==0x1220) && (fs_rd32(addin_model.regs+0x1100)&0x4000)) {
        uint32_t counter=(0x3ffff000u-(uint32_t)(g_ticks*addin_model.input_khz))&0x3fffffff;
        if(addin_model.torn_counter && reg==0x1220) counter^=(++addin_model.counter_reads&1) ? 0x8000u : 0;
        return reg==0x1120 ? counter&0x7fff : counter>>15;
    }
    if(bytes==4) return fs_rd32(addin_model.regs+reg);
    if(bytes==2) return fs_rd16(addin_model.regs+reg);
    return addin_model.regs[reg];
}
static void addin_test_write(void *ctx, unsigned reg, unsigned bytes, uint32_t value)
{
    (void)ctx;CHECK(reg+bytes<=sizeof(addin_model.regs));
    if(++addin_model.writes==addin_model.drop) return;
    if(bytes==4) fs_wr32(addin_model.regs+reg,value);
    else if(bytes==2) fs_wr16(addin_model.regs+reg,(uint16_t)value);
    else addin_model.regs[reg]=(uint8_t)value;
}
static const struct ata_addin_ops addin_test_ops={addin_test_read,addin_test_write};
static int addin_test_delay(void *ctx, uint32_t us)
{
    struct fake *f=ctx; f->delays++;
    CHECK(us<=1000);
    addin_model.fractional_us+=us;
    g_ticks+=addin_model.fractional_us/1000;addin_model.fractional_us%=1000;
    return f->fail_delay ? -FS_EIO : 0;
}
static const struct ata_port_ops addin_test_ports={fake_in8,fake_out8,fake_in16,fake_out16,
    fake_now,fake_deadline,addin_test_delay,fake_insw,fake_outsw};
static void addin_fixture(struct fake *f, unsigned vendor, unsigned id, unsigned rev, unsigned ch)
{
    chip_fixture(f,ATA_CHIP_PIIX,ch);
    memset(&addin_model,0,sizeof(addin_model));addin_model.input_khz=16666;
    memset(&chip_model,0,sizeof(chip_model));
    chip_model.pll_model=vendor==0x1103;
    pci[0]=(struct pci_func){.vendor=(uint16_t)vendor,.device=(uint16_t)id,.revision=(uint8_t)rev,
        .class_code=1,.subclass=4,.prog_if=0x85,.command=7,.irq_pin=1,.irq_line=11,
        .bar={0xd001,0xd101,0xd201,0xd301,0xc001,0xf0000000u}};
    uint8_t *r=chip_model.regs[0];r[0x50]=r[0x54]=0x34;
    fs_wr32(r+0x40,0x00c8a731);fs_wr32(r+0x44,0x00c8a731);
    fs_wr32(addin_model.regs+0x90,0xabcde080);
    addin_model.regs[0x1104]=addin_model.regs[0x1204]=2;
    f->channel.ops=&addin_test_ports;f->channel.addin_ops=&addin_test_ops;
    f->channel.addin_ctx=0;f->channel.pci=&pci[0];f->channel.index=2+ch;
    ata_chipset_init(&f->channel.chipset,&pci[0],ch,&chip_ops,0);
}
static void addin_prepare_ok(struct fake *f)
{
    kmutex_lock(&f->channel.mutex);
    CHECK(!ata_addin_prepare(&f->channel));
    kmutex_unlock(&f->channel.mutex);
    CHECK(f->channel.chipset.prepared);
}
static void addin_cable(struct fake *f, bool cable)
{
    struct ata_chipset *s=&f->channel.chipset;
    if(s->family==ATA_CHIP_PDC_NEW) fs_wr32(addin_model.regs+0x1108+0x100*s->channel,cable ? 0 : 1u<<26);
    else if(s->family==ATA_CHIP_PDC_OLD) fs_wr16(chip_model.regs[0]+0x50,cable ? 0 : (uint16_t)(1u<<(10+s->channel)));
    else chip_model.regs[0][0x5a]=cable ? 0 : 2u>>s->channel;
    if(s->family==ATA_CHIP_PDC_OLD) s->cable80=cable && pci[0].device!=0x4d33;
}
static void test_addin_modes(bool promise)
{
    static const struct {unsigned vendor,id,rev,ceiling;} chips[]={
        {0x105a,0x4d33,1,2},{0x105a,0x4d38,1,4},{0x105a,0x0d30,1,5},{0x105a,0x4d30,1,5},
        {0x105a,0x4d68,1,5},{0x105a,0x4d69,1,6},{0x105a,0x6269,1,6},
        {0x105a,0x1275,1,6},{0x105a,0x5275,1,6},{0x105a,0x7275,1,6},
        {0x1103,4,0,4},{0x1103,4,1,4},{0x1103,4,2,4},{0x1103,4,3,4},{0x1103,4,4,4},
        {0x1103,4,5,6},{0x1103,4,6,6},{0x1103,5,1,6},{0x1103,5,2,6},{0x1103,8,1,5},{0x1103,9,1,6}};
    unsigned cases=0;
    for(unsigned chip=0;chip<ARRAY_SIZE(chips);chip++) {
        if((chips[chip].vendor==0x105a)!=promise) continue;
        for(unsigned ch=0;ch<(chips[chip].id==4 && chips[chip].rev<3 ? 1u : 2u);ch++)
        for(unsigned wire=0;wire<4;wire++) {
            struct fake f;addin_fixture(&f,chips[chip].vendor,chips[chip].id,chips[chip].rev,ch);
            addin_cable(&f,wire&1);addin_prepare_ok(&f);
            for(unsigned unit=0;unit<2;unit++) for(unsigned drive=0;drive<=6;drive++) {
                uint8_t id[512]={0};fs_wr16(id+98,0x100);fs_wr16(id+106,4);
                fs_wr16(id+176,(uint16_t)((1u<<(drive+1))-1));fs_wr16(id+186,wire&2 ? 0x6000 : 0x4000);
                struct ata_timing_plan plan;int mode=ata_chipset_plan(&f.channel.chipset,unit,id,&plan);
                int expected=(int)drive;
                if(expected>(int)chips[chip].ceiling) expected=(int)chips[chip].ceiling;
                if(wire!=3 && expected>2) expected=2;
                if(f.channel.chipset.family==ATA_CHIP_PDC_NEW && unit && expected>5) expected=5;
                CHECK(mode==expected);
                uint8_t before[256];memcpy(before,chip_model.regs[0],256);
                kmutex_lock(&f.channel.mutex);CHECK(!ata_chipset_program(&f.channel,&plan));kmutex_unlock(&f.channel.mutex);
                if(f.channel.chipset.family==ATA_CHIP_PDC_OLD) {
                    static const unsigned bp[]={0x60,0x40,0x20,0x40,0x20,0x20}, cp[]={3,2,1,2,1,1};
                    unsigned reg=0x60+8*ch+4*unit;
                    CHECK((chip_model.regs[0][reg+1]&0xe0)==bp[mode]);
                    CHECK((chip_model.regs[0][reg+2]&15)==cp[mode]);
                    CHECK((before[reg+1]&31)==(chip_model.regs[0][reg+1]&31));
                } else if(f.channel.chipset.family==ATA_CHIP_PDC_NEW) {
                    static const unsigned ct[]={0xd50f4a,0xd00a3a,0xcd072a,0xcd051a,0xcd031a,0xcb021a,0xcb011a};
                    struct ata_device *d=&f.channel.devices[unit];d->udma_mode=(uint8_t)mode;
                    kmutex_lock(&f.channel.mutex);CHECK(!ata_addin_post_mode(d));kmutex_unlock(&f.channel.mutex);
                    if(f.channel.chipset.clock_mhz==133) CHECK(fs_rd32(addin_model.regs+0x1110+0x100*ch+8*unit)==(0xac000000u|ct[mode]));
                    CHECK(!memcmp(before,chip_model.regs[0],256));
                } else {
                    unsigned reg=0x40+4*(unit+(f.channel.chipset.family==ATA_CHIP_HPT36 ? 0 : 2*ch));
                    uint32_t mask=f.channel.chipset.family==ATA_CHIP_HPT36 ? 0xf0070000u : 0x303c0000u;
                    CHECK((fs_rd32(before+reg)&~mask)==(fs_rd32(chip_model.regs[0]+reg)&~mask));
                    CHECK((fs_rd32(chip_model.regs[0]+reg)&0x30000000u)==0x10000000u);
                    if(mode==2) CHECK((fs_rd32(chip_model.regs[0]+reg)&mask)==
                        (f.channel.chipset.family==ATA_CHIP_HPT36 ? 0x10020000u :
                         f.channel.chipset.clock_mhz==33 ? 0x10080000u : f.channel.chipset.clock_mhz==50 ? 0x100c0000u : 0x10100000u));
                }
                cases++;
            }
            if(f.channel.chipset.family==ATA_CHIP_PDC_NEW)
                CHECK(fs_rd16(addin_model.regs+0x1202)==(chips[chip].id==0x4d68 ? 0x0d58 : 0x0d76));
            registry_init();
        }
    }
    printf("ATA add-in %s per-chip/channel/unit/cable/IDENTIFY/timing/PLL matrix: PASS (%u cases)\n",promise ? "Promise" : "HighPoint",cases);
}
static void test_addin_clocks(void)
{
    const unsigned selections[]={5,7,9}, clocks[]={25,33,40};
    for(unsigned i=0;i<3;i++) {
        struct fake f;addin_fixture(&f,0x1103,4,1,0);
        fs_wr32(chip_model.regs[0]+0x40,0xc0000000u|(selections[i]<<8));
        addin_prepare_ok(&f);CHECK(f.channel.chipset.clock_mhz==clocks[i]);registry_init();
        CHECK(!(fs_rd32(chip_model.regs[0]+0x40)&0xc0000000u));
    }
    const unsigned counts[]={128,160,200,264};
    for(unsigned rev=3;rev<=4;rev++) for(unsigned i=0;i<4;i++) for(unsigned bios=0;bios<2;bios++) {
        struct fake f;addin_fixture(&f,0x1103,4,rev,0);
        fs_wr32(addin_model.regs+0x90,bios ? 0xabcde000u|counts[i] : 0);
        fs_wr16(chip_model.regs[0]+0x78,(uint16_t)counts[i]);
        addin_prepare_ok(&f);
        CHECK(f.channel.chipset.clock_mhz==(i==0 ? 33u : 50u));
        CHECK(f.channel.chipset.maximum==(i==0 ? 4 : 5));registry_init();
    }
    struct fake f;addin_fixture(&f,0x1103,4,1,0);
    fs_wr32(chip_model.regs[0]+0x40,0);addin_prepare_ok(&f);
    CHECK(f.channel.chipset.maximum<0&&!chip_model.writes&&!strcmp(f.channel.chipset.reason,"highpoint_unknown_clock"));
    puts("ATA HighPoint 25/33/40 clocks, 33/40/50/66 PCI, BIOS/sampled frequency and PIO fallback: PASS");
    registry_init();
}
static void test_addin_resources(void)
{
    struct fake f;addin_fixture(&f,0x105a,0x4d38,1,0);
    struct ata_resources r;
    for(unsigned subclass=1;subclass<=0x80;subclass++) {
        pci[0].subclass=(uint8_t)subclass;
        CHECK((!ata_pci_resources(&pci[0],1,true,&r))==(subclass==1 || subclass==4 || subclass==0x80));
    }
    pci[0].subclass=4;CHECK(!ata_pci_resources(&pci[0],1,true,&r)&&r.base==0xd200&&r.control==0xd302&&r.bm==0xc008&&r.native_irq);
    CHECK(ata_pci_resources(&pci[0],0,false,&r)<0);
    pci[0].bar[4]=0xd001;CHECK(ata_pci_resources(&pci[0],0,true,&r)<0);
    pci[0].bar[4]=0xc001;pci[0].bar[2]=pci[0].bar[0];CHECK(ata_pci_resources(&pci[0],0,true,&r)<0);
    addin_fixture(&f,0x1103,4,2,0);CHECK(!ata_pci_resources(&pci[0],0,true,&r));CHECK(ata_pci_resources(&pci[0],1,true,&r)<0);
    pci[0].revision=7;CHECK(ata_pci_resources(&pci[0],0,true,&r)<0);
    addin_fixture(&f,0x105a,0x4d69,1,0);
    CHECK(!ata_pci_resources(&pci[0],0,true,&r)&&r.mmio==0xf0000000u&&r.base==0x17c0&&r.control==0x1fda&&r.bm==0x1000);
    uint32_t bad[]={0,1,0xf0000004u,0xf0001000u,0xffffc000u};
    for(unsigned i=0;i<ARRAY_SIZE(bad);i++){pci[0].bar[5]=bad[i];CHECK(ata_pci_resources(&pci[0],0,true,&r)<0);}
    pci[0].bar[5]=0xf0000000u;pci[0].command=1;CHECK(ata_pci_resources(&pci[0],0,true,&r)<0);
    pci[0].device=0x3373;CHECK(!ata_addin_match(&pci[0]));
    /* Real sparse task-file and MMIO bus-master access widths. */
    struct ata_channel *c=&f.channel;c->mmio=native_addin_mmio;c->base=0x17c0;c->control=0x1fda;
    memset(native_addin_mmio,0,sizeof(native_addin_mmio));
    const unsigned offsets[]={0,5,10,15,16,21,26,31};
    for(unsigned i=1;i<8;i++){native_out8(c,c->base+i,(uint8_t)(i+0x30));CHECK(c->mmio[c->base+offsets[i]]==i+0x30);}
    native_out16(c,c->base,0x1234);CHECK(native_in16(c,c->base)==0x1234);
    bm_write32(c,0x1004,0x1234000);CHECK(fs_rd32(native_addin_mmio+0x1004)==0x1234000);
    bm_write(c,0x1000,8);CHECK(bm_read(c,0x1000)==8);
    pci[0].device=0x4d69;CHECK(!ata_setup_install_allowed(&c->devices[0]));
    puts("ATA add-in native I/O/MMIO/class/IRQ/alias/revision/Setup gate: PASS");
    registry_init();
}
static void test_addin_bindings(void)
{
    struct fake f;addin_fixture(&f,0x105a,0x4d38,1,0);
    struct pci_func addin=pci[0];
    pci[0]=(struct pci_func){.vendor=0x8086,.device=0x244b,.class_code=1,.subclass=1,.command=1,.prog_if=0x80,.bar={0,0,0,0,0xb001}};
    pci[1]=addin;npci=2;ata_test_route=true;
    memset(channels,0,sizeof(channels));registry_init();npci=2;
    for(unsigned i=0;i<4;i++) {
        struct ata_channel *c=&channels[i];CHECK(!ata_channel_setup(c,i,&fake_ops,&f));
        CHECK(!ata_bind_resources(c,i));
        CHECK(c->pci==&pci[i<2 ? 0 : 1] && c->chipset.channel==i%2);
        CHECK(!addin_claim(c));CHECK(!claim_channel(c,i));
    }
    CHECK(channels[2].addin_claim==channels[3].addin_claim);
    CHECK(channels[2].peer_claims[0]==channels[3].claims[0]);
    CHECK(!activate_once(channels[2].addin_claim,channels[2].addin_generation));
    CHECK(!activate_once(channels[3].addin_claim,channels[3].addin_generation));
    memset(channels,0,sizeof(channels));registry_init();npci=2;
    /* The second task file conflicts: reject BEFORE touching clocks/ports. */
    CHECK(registry_claim(RES_PORT,0xd200,0xd208,"peer-owner",false)>=0);
    struct ata_channel *c=&channels[2];CHECK(!ata_channel_setup(c,2,&fake_ops,&f));
    CHECK(!ata_bind_resources(c,2));CHECK(addin_claim(c)<0);
    CHECK(!addin_model.writes&&!chip_model.writes);
    memset(channels,0,sizeof(channels));registry_init();npci=2;
    pci[1].device=0x4d69;
    for(unsigned i=2;i<4;i++) {
        c=&channels[i];CHECK(!ata_channel_setup(c,i,&fake_ops,&f));
        CHECK(!ata_bind_resources(c,i));CHECK(!addin_claim(c));CHECK(!claim_channel(c,i));
    }
    CHECK(channels[2].mmio==native_addin_mmio&&channels[3].mmio==native_addin_mmio);
    CHECK(channels[2].claims[0]==channels[3].claims[1]);
    memset(channels,0,sizeof(channels));registry_init();ata_test_route=false;
    puts("ATA add-in coexistence, local channel indices, shared claims/map, peer conflict before clock writes: PASS");
}
static void test_addin_raid(void)
{
    uint64_t lbas[54];unsigned n=ata_raid_locations(4096,lbas,54);CHECK(n>40);
    uint8_t data[512]={0};CHECK(!ata_raid_signature(data));
    memcpy(data,"Promise Technology, Inc.",23);CHECK(ata_raid_signature(data));
    memcpy(data,"FreeBSD ATA driver RAID ",24);CHECK(ata_raid_signature(data));
    memset(data,0,sizeof(data));fs_wr32(data+32,0x5a7816f0);CHECK(ata_raid_signature(data));
    fs_wr32(data+32,0x5a7816fd);CHECK(ata_raid_signature(data));
    memset(data,0,sizeof(data));fs_wr32(data,0x5a7816f3);CHECK(ata_raid_signature(data));
    for(unsigned loc=0;loc<n;loc++) {
        struct fake f;addin_fixture(&f,0x105a,0x4d38,1,0);addin_prepare_ok(&f);
        /* A bounded 2 MiB fake disk contains real metadata at every candidate. */
        uint8_t *media=calloc(4096,512);CHECK(media!=0);if(!media) return;
        memcpy(media+lbas[loc]*512,"Promise Technology, Inc.",23);
        f.media_bytes=media;f.media_sectors=4096;
        f.identify[0][60]=4096;f.identify[0][61]=0;
        CHECK(ata_channel_discover(&f.channel)==0);
        struct ata_device *d=&f.channel.devices[0];
        CHECK(d->raid_checked&&d->raid_member&&d->block.quarantined&&!d->block.write&&!d->block.request_write&&!d->block.flush&&!d->block.emergency_write);
        unsigned commands=f.commands;
        CHECK(ata_write(&d->block,0,1,data)==-FS_EQUARANTINED&&ata_flush(&d->block)==-FS_EQUARANTINED&&commands==f.commands&&!f.data_writes);
        free(media);registry_init();
    }
    struct fake f;addin_fixture(&f,0x105a,0x4d38,1,0);
    CHECK(ata_channel_discover(&f.channel)==1);
    CHECK(f.channel.devices[0].raid_checked&&!f.channel.devices[0].raid_member&&!f.channel.stopped);
    CHECK(!f.channel.devices[0].block.read(&f.channel.devices[0].block,1,1,data));
    registry_init();
    for(unsigned copy=0;copy<4;copy++) {
        addin_fixture(&f,0x105a,0x4d38,1,0);
        f.identify[0][60]=0;f.identify[0][61]=0x1000;
        f.raid_fixture=true;f.raid_lba=268435377ull+14*copy;
        memcpy(f.raid_sector,"Promise Technology, Inc.",23);
        CHECK(ata_channel_discover(&f.channel)==0&&f.channel.devices[0].raid_member&&!f.data_writes);
        registry_init();
    }
    for(unsigned format=0;format<4;format++) {
        addin_fixture(&f,0x1103,4,5,0);
        f.raid_fixture=true;f.raid_lba=format<2 ? 9 : 1048576-11;
        fs_wr32(f.raid_sector+(format<2 ? 32 : 0),format&1 ? 0x5a7816fdu : format<2 ? 0x5a7816f0u : 0x5a7816f3u);
        CHECK(ata_channel_discover(&f.channel)==0&&f.channel.devices[0].raid_member&&!f.data_writes);
        registry_init();
    }
    puts("ATA RAID signatures, all Promise locations/copies, single-disk write/flush refusal, plain disk publication: PASS");
    registry_init();
}
static void test_addin_shared_quiesce(void)
{
    struct fake first,second;addin_fixture(&first,0x105a,0x4d38,1,0);
    addin_fixture(&second,0x105a,0x4d38,1,1);
    memset(channels,0,sizeof(channels));registry_init();
    int claim=registry_claim(RES_PORT,0xc000,0xc040,"ata2",false);CHECK(claim>=0);
    gen_t gen=registry_get((unsigned)claim)->generation;
    channels[2]=first.channel;channels[3]=second.channel;
    for(unsigned i=2;i<4;i++) {
        channels[i].native=true;channels[i].native_irq=true;
        channels[i].dma.claim=claim;channels[i].dma.generation=gen;
        for(unsigned k=0;k<3;k++) channels[i].claims[k]=-1;
    }
    channels[3].active=&second.channel.devices[0];
    CHECK(!ata_registry_idle(claim,gen));CHECK(channels[2].quiescing&&channels[3].quiescing);
    channels[3].active=0;CHECK(ata_registry_idle(claim,gen));
    addin_model.regs[0x1d]=4;
    CHECK(ata_addin_irq_pending(&channels[2])&&!ata_addin_irq_pending(&channels[3]));
    memset(channels,0,sizeof(channels));registry_init();
    addin_fixture(&first,0x105a,0x4d69,1,0);
    addin_fixture(&second,0x105a,0x4d69,1,1);
    claim=registry_claim(RES_MMIO,0xf0000000u,0xf0004000u,"ata2",false);
    CHECK(claim>=0);gen=registry_get((unsigned)claim)->generation;
    channels[2]=first.channel;channels[3]=second.channel;addin_mmio_revokes=0;
    for(unsigned i=2;i<4;i++) {
        channels[i].native=true;channels[i].native_irq=true;
        channels[i].addin_claim=claim;channels[i].addin_generation=gen;
        channels[i].mmio=native_addin_mmio;channels[i].mmio_phys=0xf0000000u;
        for(unsigned k=0;k<3;k++) channels[i].claims[k]=-1;
    }
    CHECK(ata_registry_idle(claim,gen)&&addin_mmio_revokes==1&&!channels[2].mmio&&!channels[3].mmio);
    CHECK(ata_registry_idle(claim,gen)&&addin_mmio_revokes==1);
    CHECK(native_in8(&channels[2],channels[2].base)==0xff&&bm_read(&channels[2],0x1002)==0xff);
    native_out8(&channels[2],channels[2].base+7,0xec);bm_write(&channels[2],0x1000,1);
    memset(channels,0,sizeof(channels));registry_init();
    puts("ATA shared master resource: all channels quiesced, busy peer retains claim, Promise IRQ source: PASS");
}
static void test_addin_faults(void)
{
    for(unsigned fault=0;fault<4;fault++) {
        struct fake f;addin_fixture(&f,0x105a,0x4d69,1,0);
        if(fault==0) addin_model.drop=1;
        if(fault==1) addin_model.input_khz=1000;
        if(fault==2) addin_model.torn_counter=true;
        if(fault==3) f.fail_delay=true;
        CHECK(ata_channel_discover(&f.channel)<0&&f.channel.stopped&&f.channel.devices[0].block.quarantined);
        CHECK(!f.commands&&!f.dma_starts);registry_init();
    }
    struct fake f;addin_fixture(&f,0x1103,4,5,0);chip_model.pll_fail=true;
    CHECK(ata_channel_discover(&f.channel)<0&&f.channel.stopped&&!f.commands);
    addin_fixture(&f,0x1103,4,5,0);chip_model.peer_command=1;
    CHECK(ata_channel_discover(&f.channel)<0&&!chip_model.writes&&!addin_model.writes);
    addin_fixture(&f,0x105a,0x4d38,1,0);addin_prepare_ok(&f);
    CHECK(ata_channel_discover(&f.channel)==1);
    struct ata_device *d=&f.channel.devices[0];uint8_t bytes[512];
    d->udma_mode=4;CHECK(!ata_addin_dma_begin(d,512,true));CHECK((addin_model.regs[0x11]&2)==2);
    CHECK(ata_addin_dma_end(&f.channel)&&!(addin_model.regs[0x11]&2));
    d->lba48=true;CHECK(!ata_addin_dma_begin(d,65536,false));
    CHECK(fs_rd32(addin_model.regs+0x20)==0x05008000u);CHECK(ata_addin_dma_end(&f.channel));
    f.bm_status|=1;CHECK(!ata_addin_dma_end(&f.channel));f.bm_status&=~1u;
    d->dma_enabled=false;f.fault=ERR;
    CHECK(d->block.read(&d->block,0,1,bytes)==-FS_EIO&&f.channel.stopped&&d->block.quarantined);
    puts("ATA add-in PLL timeout/readback/torn-counter/delay/peer-active failures, DMA clock/count/stop, quarantine: PASS");
    registry_init();
}
static void addin_autotiming(struct fake *f, uint8_t command)
{
    if(f->channel.chipset.family!=ATA_CHIP_PDC_NEW || f->channel.chipset.clock_mhz!=133) return;
    unsigned reg=0x110c+0x100*f->channel.chipset.channel+8*f->selected;
    if(command==0xef) {
        fs_wr32(addin_model.regs+reg,0xdeadbeef);fs_wr32(addin_model.regs+reg+4,0xdeadbeef);
        addin_model.autotiming=true;
    } else if(command==0xec && addin_model.autotiming) {
        CHECK((fs_rd32(addin_model.regs+reg)&65535)==0x2bfb);
        CHECK(fs_rd32(addin_model.regs+reg+4)==0xaccb021au);
        addin_model.autotiming=false;
    }
}
static void addin_metadata_fault(struct fake *f, uint8_t command)
{
    if(command==0x20 || command==0x24) f->fault=ERR;
}
static void test_addin_transactions(void)
{
    static const unsigned ids[]={0x4d33,0x4d38,0x0d30,0x4d30,0x4d68,0x4d69,0x6269,0x1275,0x5275,0x7275,4,4,4,4,4,5,5,8,9};
    static const unsigned rev[]={1,1,1,1,1,1,1,1,1,1,0,2,3,5,6,1,2,1,1};
    unsigned cases=0;
    for(unsigned i=0;i<ARRAY_SIZE(ids);i++) for(unsigned ext=0;ext<2;ext++) {
        struct fake f;addin_fixture(&f,i<10 ? 0x105a : 0x1103,ids[i],rev[i],0);
        if(ext) {f.identify[0][83]|=0x2400;f.identify[0][100]=0;f.identify[0][101]=16;f.identify[0][102]=0;}
        command_hook=addin_autotiming;
        CHECK(ata_channel_discover(&f.channel)==1);
        command_hook=0;
        struct ata_device *d=&f.channel.devices[0];
        CHECK(d->dma_enabled==!(ext && ids[i]==0x4d33));
        uint8_t data[512];fill(data,1,1);
        CHECK(!d->block.write(&d->block,1,1,data));memset(data,0,sizeof(data));
        CHECK(!d->block.read(&d->block,1,1,data));CHECK(!d->block.flush(&d->block));
        CHECK(d->raid_checked&&!f.channel.stopped);
        CHECK(!fs_rd32(addin_model.regs+0x20) && !(addin_model.regs[0x11]&2));
        cases++;registry_init();
    }
    struct fake f;addin_fixture(&f,0x105a,0x4d38,1,0);
    f.identify[0][49]&=~0x100u;
    CHECK(ata_channel_discover(&f.channel)==1&&!f.channel.devices[0].dma_enabled&&!f.channel.stopped);
    addin_fixture(&f,0x105a,0x4d38,1,0);command_hook=addin_metadata_fault;
    CHECK(ata_channel_discover(&f.channel)<0&&f.channel.stopped&&!f.channel.devices[0].raid_checked);
    CHECK(!f.channel.devices[0].block.write&&!f.data_writes);command_hook=0;registry_init();
    addin_fixture(&f,0x1103,8,1,0);pci[0].fn=1;
    f.channel.chipset.pci_mhz=33;addin_prepare_ok(&f);
    CHECK(f.channel.chipset.clock_mhz==50&&!(chip_model.regs[0][0x53]&0x80));
    registry_init();
    printf("ATA add-in boot/SET FEATURES/autotiming/verify/PIO/UDMA28/48/write/read/flush/metadata fault: PASS (%u cases)\n",cases);
}
static void test_ata_addin(const char *name)
{
    if(!strcmp(name,"promise")) test_addin_modes(true);
    else if(!strcmp(name,"highpoint")) {test_addin_modes(false);test_addin_clocks();}
    else if(!strcmp(name,"resources")) {test_addin_resources();test_addin_bindings();test_addin_shared_quiesce();}
    else if(!strcmp(name,"raid")) test_addin_raid();
    else if(!strcmp(name,"faults")) test_addin_faults();
    else if(!strcmp(name,"transactions")) test_addin_transactions();
    else CHECK(false);
}
