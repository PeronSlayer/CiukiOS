/* Per-family PCI register models plus real ATA transaction fixtures.
 * SPDX-License-Identifier: GPL-2.0-only */
_Static_assert(ATA_CHIPSET_ABI == 1u && ATA_TIMING_WRITES == 12u, "ATA boot contract v1");
_Static_assert(sizeof(struct ata_timing_write) == 3u, "bounded byte-register plan entry");
static struct {
    uint8_t regs[4][256];
    unsigned writes, drop_write;
    uint8_t peer_status, peer_command;
    bool pll_model, pll_fail;
} chip_model;
static unsigned chip_slot(const struct pci_func *p)
{
    for (unsigned i = 0; i < npci; i++) if (p == &pci[i]) return i;
    CHECK(false); return 0;
}
static uint8_t chip_read(void *ctx, const struct pci_func *p, unsigned reg)
{
    (void)ctx; CHECK(reg < 256);
    uint8_t value=chip_model.regs[chip_slot(p)][reg];
    if(reg==0x5b && chip_model.pll_model && !chip_model.pll_fail) value|=0x80;
    return value;
}
static void chip_write(void *ctx, const struct pci_func *p, unsigned reg, uint8_t v)
{
    (void)ctx; CHECK(reg < 256); chip_model.writes++;
    if (chip_model.writes != chip_model.drop_write) chip_model.regs[chip_slot(p)][reg] = v;
}
static const struct ata_config_ops chip_ops = {chip_read,chip_write};
static uint8_t chip_bm_read(void *ctx, uint16_t port)
{
    struct fake *f = ctx;
    unsigned peer = f->channel.dma.base ^ 8u;
    if (port == peer) return chip_model.peer_command;
    if (port == peer + 2) return chip_model.peer_status;
    return fake_in8(ctx,port);
}
static const struct ata_dma_ops chip_bm_ops = {chip_bm_read,fake_out8,dma_out32,dma_physical};
static void chip_fixture(struct fake *f, enum ata_family family, unsigned ch)
{
    dma_fresh(f,false);
    memset(&chip_model,0,sizeof(chip_model)); memset(pci,0,sizeof(pci)); npci=1;
    pci[0]=(struct pci_func){.bus=0,.dev=7,.fn=1,.class_code=1,.subclass=1,.command=1,
        .prog_if=0x80,.bar={0,0,0,0,0xc001}};
    f->channel.index=ch; f->channel.dma.base=(uint16_t)(0xc000+8*ch);
    f->channel.dma.ops=&chip_bm_ops;
    pci[1]=(struct pci_func){.class_code=6,.subclass=1,.dev=7};
    uint8_t *r=chip_model.regs[0];
    switch(family) {
    case ATA_CHIP_PIIX:
        pci[0].vendor=0x8086; pci[0].device=0x244b; r[0x41]=r[0x43]=0x80; r[0x54]=0xf0;
        r[0x4a]=0xcc; r[0x4b]=0xcc; r[0x55]=0x0a; break;
    case ATA_CHIP_VIA:
        npci=2; pci[0].vendor=pci[1].vendor=0x1106; pci[0].device=0x0571;
        pci[1].device=0x0686; pci[1].revision=0x40; r[0x40]=3; r[0x53]=r[0x51]=0x10; break;
    case ATA_CHIP_SIS:
        npci=2; pci[0].vendor=pci[1].vendor=0x1039; pci[0].device=0x5513;
        pci[1].device=0x0735; r[0x4a]=6; r[0x49]=1; break;
    case ATA_CHIP_ALI:
        pci[0].vendor=0x10b9; pci[0].device=0x5229; pci[0].revision=0xc5;
        r[0x4a]=0x20; r[0x4b]=9; r[0x53]=1; r[0x54]=r[0x55]=0x55; break;
    case ATA_CHIP_NV:
        pci[0].vendor=0x10de; pci[0].device=0x0065; r[0x50]=3;
        r[0x60]=r[0x61]=r[0x62]=r[0x63]=0xc7; break;
    default: CHECK(false);
    }
    ata_chipset_init(&f->channel.chipset,&pci[0],ch,&chip_ops,0);
    f->channel.pci=&pci[0];
    CHECK(f->channel.chipset.maximum>=0 && f->channel.chipset.family==family);
}
static void chip_set_cable(enum ata_family family, unsigned ch, bool cable)
{
    uint8_t *r=chip_model.regs[0];
    if (family==ATA_CHIP_PIIX) r[0x54]=cable ? 0xf0 : 0;
    if (family==ATA_CHIP_VIA) r[0x53-2*ch]=cable ? 0x10 : 0;
    if (family==ATA_CHIP_SIS) r[0x48]=cable ? 0 : (uint8_t)(0x10u<<ch);
    if (family==ATA_CHIP_ALI) r[0x4a]=(uint8_t)(0x20|(cable ? 0 : 1u<<ch));
    if (family==ATA_CHIP_NV) r[0x63-2*ch]=r[0x62-2*ch]=cable ? 0xc7 : 0xc0;
}
static void test_chip_matrix(enum ata_family family)
{
    unsigned cases=0;
    for (unsigned ch=0;ch<2;ch++) for (unsigned u=0;u<2;u++)
    for (unsigned drive=0;drive<=6;drive++) for (unsigned cable=0;cable<4;cable++) {
        struct fake f; chip_fixture(&f,family,ch);
        chip_set_cable(family,ch,!!(cable&1));
        ata_chipset_init(&f.channel.chipset,&pci[0],ch,&chip_ops,0);
        struct ata_chipset *s=&f.channel.chipset;
        uint8_t id[512]={0}; fs_wr16(id+98,0x100); fs_wr16(id+106,4);
        fs_wr16(id+176,(uint16_t)((1u<<(drive+1))-1)); fs_wr16(id+186,cable&2 ? 0x6000 : 0x4000);
        struct ata_timing_plan plan;
        int mode=ata_chipset_plan(s,u,id,&plan), expected=(int)drive;
        if (expected>s->maximum) expected=s->maximum;
        if (cable!=3 && expected>2) expected=2;
        CHECK(mode==expected && plan.count && !chip_model.writes);
        uint8_t before[256], predicted[256]; memcpy(before,chip_model.regs[0],256); memcpy(predicted,before,256);
        for(unsigned i=0;i<plan.count;i++) {
            struct ata_timing_write *w=&plan.writes[i];
            predicted[w->reg]=(uint8_t)((predicted[w->reg]&~w->mask)|w->value);
        }
        kmutex_lock(&f.channel.mutex); CHECK(!ata_chipset_program(&f.channel,&plan)); kmutex_unlock(&f.channel.mutex);
        CHECK(!memcmp(predicted,chip_model.regs[0],256));
        unsigned reg=0,mask=255,bits=0;
        if(family==ATA_CHIP_PIIX) {
            unsigned dn=2*ch+u; reg=0x4a+dn/2; mask=3u<<((dn&1)*4);
            bits=(mode==0 ? 0 : (mode&1) ? 1u : 2u)<<((dn&1)*4);
            CHECK(chip_model.regs[0][0x48]&(1u<<dn));
            CHECK((chip_model.regs[0][0x54]&(1u<<dn))==(mode==3 || mode==4 ? 1u<<dn : 0));
            CHECK((chip_model.regs[0][0x55]&(0x10u<<dn))==(mode==5 ? 0x10u<<dn : 0));
        } else if(family==ATA_CHIP_VIA) {
            static const unsigned timing[]={0xe7,0xe6,0xe4,0xe3,0xe1,0xe0};
            reg=0x53-2*ch-u;mask=0xef;bits=timing[mode];
        } else if(family==ATA_CHIP_SIS) {
            static const unsigned timing[]={0x8b,0x87,0x85,0x83,0x82,0x81};
            reg=0x41+4*ch+2*u;mask=0x8f;bits=timing[mode];
        } else if(family==ATA_CHIP_ALI) {
            static const unsigned timing[]={0xc,0xb,0xa,9,8,0xf,0xd};
            reg=0x56+ch;mask=0xfu<<(4*u);bits=timing[mode]<<(4*u);
        } else {
            static const unsigned timing[]={0xc2,0xc1,0xc0,0xc4,0xc5,0xc6,0xc7};
            reg=0x63-2*ch-u;bits=timing[mode];
        }
        CHECK((chip_model.regs[0][reg]&mask)==bits);
        for(unsigned w=0;w<3;w++) {
            static const unsigned fields[]={98,106,176};
            unsigned old=fs_rd16(id+fields[w]);fs_wr16(id+fields[w],0);
            CHECK(ata_chipset_plan(s,u,id,&plan)<0 && !plan.count);fs_wr16(id+fields[w],(uint16_t)old);
        }
        cases++;
    }
    printf("ATA chipset family=%u cable/IDENTIFY/peer-mask/clock matrix: PASS (%u cases)\n",family,cases);
}
static void test_chip_errata(enum ata_family family)
{
    struct fake f;chip_fixture(&f,family,0);
    struct ata_chipset *s=&f.channel.chipset;
    uint8_t *r=chip_model.regs[0];
    if(family==ATA_CHIP_PIIX) {
        pci[0].device=0x7111;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum==2&&!s->cable80);
        pci[0].device=0x7010;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum<0);
        pci[0].device=0x244b;npci=2;pci[1]=(struct pci_func){.vendor=0x8086,.device=0x84cb,.revision=0};
        ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum<0&&!strcmp(s->reason,"intel_450nx_erratum"));
        pci[1].revision=4;chip_model.regs[1][0x42]=0x40;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum<0);
        pci[1].revision=5;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum==5);
    } else if(family==ATA_CHIP_VIA) {
        pci[1].revision=0;r[0x50]=8;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum<0);
        r[0x50]=0;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum==2);
        pci[1].revision=0x10;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum<0);
        r[0x50]=r[0x52]=8;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum==4&&!s->cable80);
        pci[1].device=0x0586;pci[1].revision=0x40;r[0x43]=0xa0;
        ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum<0);
        r[0x43]=0x20;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum==2);
        uint8_t id[512]={0};fs_wr16(id+98,0x100);fs_wr16(id+106,4);fs_wr16(id+176,7);
        memset(id+54,' ',40);const char *bad="TS64GSSD25-M";
        for(unsigned i=0;i<strlen(bad);i++)id[54+(i^1)]=(uint8_t)bad[i];
        struct ata_timing_plan p;CHECK(ata_chipset_plan(s,0,id,&p)<0&&!strcmp(s->reason,"via_ssd_erratum"));
    } else if(family==ATA_CHIP_SIS) {
        pci[1].device=0x0630;pci[1].revision=0x2f;r[0x52]=4;
        ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum==4&&s->layout==1);
        pci[1].revision=0x30;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum==5);
        pci[1].device=0x5598;r[0x52]=8;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum==2&&s->layout==0);
        pci[1].device=0x0965;r[0x50]=8;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum<0);
        r[0x50]=0;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum==6&&s->layout==3);
        npci=1;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum<0);
    } else if(family==ATA_CHIP_ALI) {
        for(unsigned rev=0x20;rev<=0xc5;rev++) {
            pci[0].revision=(uint8_t)rev;ata_chipset_init(s,&pci[0],0,&chip_ops,0);
            CHECK(s->maximum==(rev<0xc2 ? -1 : rev<=0xc3 ? 4 : rev==0xc4 ? 5 : 6));
        }
        pci[0].revision=0xc2;npci=3;pci[1]=(struct pci_func){.vendor=0x10b9,.device=0x1541,.class_code=6};
        pci[2]=(struct pci_func){.vendor=0x10b9,.device=0x1533,.class_code=6};
        ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum<0);
        chip_model.regs[2][0x79]=4;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum==4&&s->no_lba48_dma);
    } else if(family==ATA_CHIP_NV) {
        pci[0].device=0x01bc;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum==5);
        r[0x50]=0;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum<0);
        pci[0].device=0xffff;ata_chipset_init(s,&pci[0],0,&chip_ops,0);CHECK(s->maximum<0);
    }
    CHECK(!chip_model.writes);registry_init();
    printf("ATA chipset family=%u revision/firmware/errata refusals: PASS\n",family);
}
static void test_chip_resources(void)
{
    struct pci_func p={.vendor=0x1106,.device=0x0571,.class_code=1,.subclass=1,.prog_if=0x85,.command=1,
        .irq_pin=1,.irq_line=11,.bar={0xd001,0xd101,0xd201,0xd301,0xc001}};
    struct ata_resources r;
    for(unsigned ch=0;ch<2;ch++) {
        CHECK(!ata_pci_resources(&p,ch,true,&r));CHECK(r.native_irq&&r.base==0xd000+ch*0x200&&r.control==0xd102+ch*0x200&&r.irq==11);
        CHECK(ata_pci_resources(&p,ch,false,&r)<0);
    }
    const uint32_t bad[]={0,1,0xd000,0xd005,0x10001,0xfffffff9};
    for(unsigned i=0;i<ARRAY_SIZE(bad);i++) {uint32_t old=p.bar[0];p.bar[0]=bad[i];CHECK(ata_pci_resources(&p,0,true,&r)<0);p.bar[0]=old;}
    p.bar[1]=p.bar[0];CHECK(ata_pci_resources(&p,0,true,&r)<0);p.bar[1]=0xd101;
    p.bar[2]=p.bar[0];CHECK(ata_pci_resources(&p,0,true,&r)<0);p.bar[2]=0xd201;
    p.bar[4]=p.bar[0];CHECK(ata_pci_resources(&p,0,true,&r)<0);p.bar[4]=0xc001;
    p.vendor=0x8086;p.device=0x248a;CHECK(ata_pci_resources(&p,0,true,&r)<0);
    p.prog_if=0x84;CHECK(ata_pci_resources(&p,0,false,&r)<0);
    p.prog_if=0x80;CHECK(!ata_pci_resources(&p,0,false,&r)&&!r.native_irq&&r.base==0x1f0&&r.control==0x3f6&&r.irq==14);
    p.prog_if=0x81;p.vendor=0x1106;p.device=0x0571;CHECK(!ata_pci_resources(&p,1,false,&r)&&r.base==0x170);
    p.command=0;CHECK(ata_pci_resources(&p,1,false,&r)<0);
    registry_init();npci=1;pci[0]=p;struct fake f;fresh(&f,false);CHECK(ata_bind_resources(&f.channel,0)<0);
    pci[0].command=1;ata_test_route=true;CHECK(!ata_bind_resources(&f.channel,0)&&f.channel.native_irq&&f.channel.base==0xd000);
    npci=2;pci[1]=pci[0];CHECK(ata_bind_resources(&f.channel,0)==-FS_EBUSY);
    CHECK(!f.writes);registry_init();
    puts("ATA native/mixed/legacy BAR, alias, decode, route, ambiguity, ICH3 resource matrix: PASS");
}
static void test_chip_layouts(enum ata_family family)
{
    if(family!=ATA_CHIP_VIA&&family!=ATA_CHIP_SIS) return;
    for(unsigned layout=0;layout<3;layout++)for(unsigned ch=0;ch<2;ch++)for(unsigned u=0;u<2;u++) {
        struct fake f;chip_fixture(&f,family,ch);
        struct ata_chipset *s=&f.channel.chipset;
        struct ata_timing_plan plan;
        uint8_t *r=chip_model.regs[0];
        if(family==ATA_CHIP_VIA) {
            pci[1].device=layout==2 ? 0x3177 : 0x0686;pci[1].revision=layout==0 ? 0 : layout==1 ? 0x10 : 0;
            r[0x50]=r[0x52]=layout==1 ? 8 : 0;
            ata_chipset_init(s,&pci[0],ch,&chip_ops,0);
            static const unsigned bits[3][7]={
                {0xe2,0xe1,0xe0,0,0,0,0},
                {0xee,0xec,0xea,0xe9,0xe8,0,0},
                {0xe7,0xe7,0xe6,0xe4,0xe2,0xe1,0xe0}};
            unsigned ceiling=layout==0 ? 2 : layout==1 ? 4 : 6;
            CHECK(s->maximum==(int)ceiling);
            for(unsigned m=0;m<=ceiling;m++) {
                plan.count=0;CHECK(ata_via_plan(s,u,m,&plan));
                CHECK(plan.count==1&&plan.writes[0].reg==0x53-2*ch-u&&plan.writes[0].value==bits[layout][m]);
            }
        } else {
            pci[1].device=layout==0 ? 0x5598 : layout==1 ? 0x0630 : 0x0965;
            r[0x50]=r[0x52]=layout==0 ? 8 : layout==1 ? 4 : 0;
            ata_chipset_init(s,&pci[0],ch,&chip_ops,0);
            CHECK(s->maximum==(layout==0 ? 2 : layout==1 ? 4 : 6));
            if(layout<2) {
                static const unsigned bits[2][5]={{0xe0,0xc0,0xa0,0,0},{0xf0,0xd0,0xb0,0xa0,0x90}};
                for(unsigned m=0;m<=(layout==0 ? 2u : 4u);m++) {
                    plan.count=0;CHECK(ata_sis_plan(s,u,m,&plan));
                    CHECK(plan.writes[0].reg==0x41+4*ch+2*u&&plan.writes[0].value==bits[layout][m]);
                }
            } else for(unsigned remap=0;remap<2;remap++)for(unsigned clock=0;clock<2;clock++) {
                fs_wr32(r+0x54,remap ? 0x40000000 : 0);
                unsigned reg=(remap ? 0x70 : 0x40)+8*ch+4*u;r[reg]=(uint8_t)(clock ? 8 : 0);
                uint8_t id[512]={0};fs_wr16(id+98,0x100);fs_wr16(id+106,4);fs_wr16(id+176,0x7f);fs_wr16(id+186,0x6000);
                int mode=ata_chipset_plan(s,u,id,&plan);CHECK(mode==(clock ? 6 : 5));
                CHECK(plan.count==2&&plan.writes[0].reg==reg&&plan.writes[0].value==0x14);
                CHECK(plan.writes[1].value==(clock ? 2 : 1));
            }
        }
    }
    printf("ATA chipset family=%u additional timing-layout/clock encodings: PASS\n",family);
}
static bool chip_corrupt_verify;
static bool chip_corrupt_mode;
static void chip_command_fault(struct fake *f,uint8_t command)
{
    if(command==0xef)chip_corrupt_verify=true;
    if(command==0xec&&chip_corrupt_verify) {
        if(chip_corrupt_mode) f->identify[0][88]&=255;
        else f->identify[0][27]^=1;
    }
}
static bool chip_peer_irq(unsigned irq,const char *owner) { (void)irq;(void)owner;return false; }
static void test_chip_native_irq(void)
{
    struct fake f;chip_fixture(&f,ATA_CHIP_PIIX,0);
    struct ata_channel *c=&channels[0];*c=f.channel;c->native_irq=true;c->irq=11;c->native=true;
    CHECK(!claim_channel(c,0));CHECK(!registry_activate(c->claims[2],c->generations[2]));
    int peer=registry_claim(RES_IRQ,11,12,"chip-peer",true);CHECK(peer>=0);
    gen_t gen=registry_get((unsigned)peer)->generation;CHECK(!registry_activate(peer,gen));
    g_cpu_tsc=true;g_tsc_per_ms=1000;
    CHECK(!irq_chain_add(11,ata_pci_interrupt,owners[0]));CHECK(!irq_chain_add(11,chip_peer_irq,"chip-peer"));
    c->native_irq_attached=true;
    unsigned before=masks+unmasks,acks=f.irq_acks;
    f.bm_status=0x60;CHECK(!ata_pci_interrupt(11,owners[0])&&f.irq_acks==acks);
    f.bm_status|=ATA_BM_IRQ;f.pending=true;
    CHECK(ata_pci_interrupt(11,owners[0])&&f.irq_acks==acks+1&&!(f.bm_status&ATA_BM_IRQ));
    CHECK(masks+unmasks==before&&registry_valid(peer,gen));
    CHECK(ata_channel_idle(c));CHECK(ata_channel_idle(c));
    CHECK(registry_valid(peer,gen)&&masks+unmasks==before);
    struct irq_chain_stats stats;CHECK(irq_chain_snapshot(11,&stats)&&stats.participants==1);
    CHECK(!irq_chain_remove(11,chip_peer_irq,"chip-peer"));
    memset(channels,0,sizeof(channels));registry_init();
    puts("ATA native IRQ: source-only acknowledgement, shared peer survives detach, no PIC masking: PASS");
}
static void test_chip_transactions(void)
{
    CHECK(!held&&!owned_queues&&!actor.preempt_holds);
    for(unsigned state=0;state<11;state++) {
        struct fake f;chip_fixture(&f,ATA_CHIP_PIIX,0);uint8_t id[512]={0};
        fs_wr16(id+98,0x100);fs_wr16(id+106,4);fs_wr16(id+176,0x3f);fs_wr16(id+186,0x6000);
        struct ata_timing_plan p;CHECK(ata_chipset_plan(&f.channel.chipset,0,id,&p)==5);
        /* The missing-owner case must never acquire the mutex. Clearing
         * owner alone would leave this stack fixture in sync.c's lists. */
        if(state!=8)kmutex_lock(&f.channel.mutex);
        if(state==0)f.channel.active=&f.channel.devices[0];
        if(state==1)f.channel.dma.armed=true;
        if(state==2)f.channel.dma.started=true;
        if(state==3)f.channel.quiescing=true;
        if(state==4)chip_model.peer_status=ATA_BM_ACTIVE;
        if(state==5)chip_model.peer_command=1;
        if(state==6)f.bm_status|=ATA_BM_ACTIVE;
        if(state==7)f.channel.modes_published=true;
        if(state==9)f.status=ATA_BSY;
        if(state==10)f.status=ATA_DRQ|ATA_DRDY;
        CHECK(ata_chipset_program(&f.channel,&p)==-FS_EBUSY&&!chip_model.writes);
        if(state!=8)kmutex_unlock(&f.channel.mutex);
        CHECK(!f.channel.mutex.owner&&!f.channel.mutex.held_next&&
              !f.channel.mutex.wait.owner&&!f.channel.mutex.wait.owned_next);
        CHECK(!held&&!owned_queues&&!actor.preempt_holds);
    }
    puts("ATA chipset refusal fixtures: 11 cases, balanced mutex/queue/preemption lifetime: PASS");
    for(unsigned family=ATA_CHIP_PIIX;family<=ATA_CHIP_NV;family++)for(unsigned unit=0;unit<2;unit++) {
        struct fake f;chip_fixture(&f,(enum ata_family)family,0);
        if(unit) {
            f.kind[0]=ABSENT;f.kind[1]=DISK;
            memcpy(f.identify[1],f.identify[0],sizeof(f.identify[0]));
        }
        CHECK(ata_channel_discover(&f.channel)==1);
        CHECK(f.channel.devices[unit].dma_enabled&&f.channel.devices[unit].udma_mode==5&&f.commands==4);
    }
    for(unsigned failure=0;failure<5;failure++) {
        struct fake f;chip_fixture(&f,ATA_CHIP_PIIX,0);
        if(failure==1)chip_model.drop_write=2;
        if(failure==2)f.fault=DMA_MODE_ERROR;
        if(failure>=3){chip_corrupt_verify=false;chip_corrupt_mode=failure==4;command_hook=chip_command_fault;}
        int result=ata_channel_discover(&f.channel);command_hook=0;
        if(!failure) {
            CHECK(result==1&&f.channel.devices[0].dma_enabled&&f.channel.devices[0].udma_mode==5);
            CHECK(f.commands==4&&!f.dma_starts);
        } else CHECK(result<0&&f.channel.stopped&&!f.channel.devices[0].dma_enabled);
    }
    for(unsigned fallback=0;fallback<3;fallback++) {
        struct fake f;chip_fixture(&f,ATA_CHIP_PIIX,0);
        if(fallback==0) {
            pci[0].device=0x7010;ata_chipset_init(&f.channel.chipset,&pci[0],0,&chip_ops,0);
        } else if(fallback==1) f.identify[0][49]&=~0x100u;
        else {
            chip_set_cable(ATA_CHIP_PIIX,0,false);ata_chipset_init(&f.channel.chipset,&pci[0],0,&chip_ops,0);
        }
        CHECK(ata_channel_discover(&f.channel)==1&&!f.channel.stopped);
        CHECK(f.channel.devices[0].dma_enabled==(fallback==2));
        CHECK(fallback==2 ? f.channel.devices[0].udma_mode==2 : !chip_model.writes&&f.commands==2);
    }
    /* Real channel control path restores VIA Device/Head after IEN writes. */
    struct fake f;chip_fixture(&f,ATA_CHIP_VIA,0);reg_write(&f.channel,6,0xb0);
    device_control(&f.channel,0);CHECK(f.selected==1&&f.head==0xb0);
    /* Quarantine must gate INTRQ without selecting a device in an
     * uncertain live command. The modeled IEN loss remains uncorrected. */
    f.channel.stopped=true;device_control(&f.channel,ATA_NIEN);CHECK(f.selected==0&&f.head==0xa0);
    registry_init();
    test_chip_native_irq();
    puts("ATA boot transaction: live-I/O refusal, PCI readback, SET FEATURES/IDENTIFY identity, VIA selection: PASS");
}
static void test_ata_chipset(const char *name)
{
    const char *names[]={"piix","via","sis","ali","nv"};
    for(unsigned i=0;i<ARRAY_SIZE(names);i++)if(!strcmp(name,names[i])) {
        test_chip_matrix((enum ata_family)(i+1));test_chip_errata((enum ata_family)(i+1));
        test_chip_layouts((enum ata_family)(i+1));return;
    }
    if(!strcmp(name,"resources"))test_chip_resources();
    else if(!strcmp(name,"transaction"))test_chip_transactions();
    else CHECK(false);
}
