/* Production DMA through synthetic task file/BM ports, IRQ and PTEs.
 * Included by ata_test.c. SPDX-License-Identifier: GPL-2.0-only */
static _Alignas(4) struct ata_prd dma_table[ATA_DMA_PRDS];
static _Alignas(4096) uint8_t dma_bounce[ATA_DMA_BYTES];
static _Alignas(4096) uint8_t scatter_data[3*4096];
static bool scatter_mapping;
static const uint32_t scatter_phys[3]={0x210000,0x350000,0x280000};
static void dma_out32(void *ctx, uint16_t port, uint32_t value)
{
    struct fake *f = ctx;
    CHECK(port == f->channel.dma.base + 4 && !(f->bm_command & 1));
    f->bm_table = value;
}
static bool dma_physical(void *ctx, const void *buf, uint32_t bytes, bool writable, uint32_t *phys)
{
    struct fake *f = ctx;
    (void)bytes; (void)writable;
    if (scatter_mapping) {
        uintptr_t address=(uintptr_t)buf,base=(uintptr_t)scatter_data;
        if (address<base || address>=base+sizeof(scatter_data) || bytes>4096-(address&4095u)) return false;
        *phys=scatter_phys[(address-base)/4096]+(address&4095u); return true;
    }
    if (f->noncontiguous) return false;
    f->direct_buffer = (void *)buf;
    *phys = f->direct_phys;
    return true;
}
static const struct ata_dma_ops dma_fake_ops = { fake_in8, fake_out8, dma_out32, dma_physical };
static void dma_bus_start(struct fake *f)
{
    struct ata_dma *m = &f->channel.dma;
    CHECK(f->dma_command && m->armed && f->bm_table == m->prd_phys);
    CHECK(!!(f->bm_command & 8) == !f->writing);
    uint32_t bytes = 0, phys = m->bounced ? m->bounce_phys : f->direct_phys;
    uint8_t *data = m->bounced ? m->bounce : scatter_mapping ? scatter_data : f->direct_buffer;
    for (unsigned i = 0; i < ATA_DMA_PRDS; i++) {
        struct ata_prd *prd = &m->prd[i];
        uint32_t count = prd->count ? prd->count : 65536;
        uint32_t expected=scatter_mapping && !m->bounced ? scatter_phys[bytes/4096]+bytes%4096 : phys+bytes;
        CHECK(prd->address == expected && !(prd->address & 1) && !(count & 1));
        CHECK((prd->address & 65535) + count <= 65536 && !(prd->flags & 0x7fff));
        bytes += count;
        if (prd->flags & 0x8000) break;
        CHECK(i + 1 < ATA_DMA_PRDS);
    }
    CHECK(bytes == f->blocks * 512 && bytes == m->bytes && bytes <= ATA_DMA_BYTES);
    f->bm_status |= 1;
    if (f->media_bytes) {
        CHECK(f->lba + f->blocks <= f->media_sectors);
        if (f->writing) memcpy(f->media_bytes + f->lba * 512, data, bytes);
        else memcpy(data, f->media_bytes + f->lba * 512, bytes);
    } else {
        for (uint32_t i = 0; i < bytes; i += 2) {
            uint16_t word = sector_word(f->lba + i / 512, (i % 512) / 2);
            if (f->writing) CHECK(fs_rd16(data + i) == word);
            else fs_wr16(data + i, word);
        }
    }
    g_ticks += 2; /* deterministic hardware transfer time */
    f->status = f->fault == ERR ? 0x41 : f->fault == DF ? 0x60 :
                f->fault == BUSY ? 0x80 : f->fault == EXTRA_DRQ ? 0x48 : 0x40;
    f->bm_status = (f->bm_status & 0xe0) | (f->fault == DMA_ERROR || f->fault == DMA_ERROR_NO_IRQ ? 2 : 4);
    if (f->fault == DMA_ACTIVE || f->fault == DMA_STUCK_MASTER) f->bm_status |= 1;
    if (f->fault == DMA_NO_BM_IRQ) f->bm_status &= ~4u;
    f->dma_completions++;
    if (f->fault == DMA_ERROR_NO_IRQ) return; /* SFF masters need not interrupt for a PCI error */
    interrupt(f);
}
static void dma_fresh(struct fake *f, bool ext)
{
    fresh(f, ext);
    f->bm_status = 0x60; f->direct_phys = 0x20ffe0;
    CHECK(!ata_dma_setup(&f->channel, 0xc000, &dma_fake_ops, f,
                         dma_table, 0x100000, dma_bounce, 0x10ffe0));
    f->identify[0][49] |= 0x100; f->identify[0][53] |= 4;
    f->identify[0][88] = 0x203f; f->identify[0][93] = 0x6000;
}
static struct blkdev *dma_qualify(struct fake *f, bool ext)
{
    dma_fresh(f, ext);
    CHECK(ata_channel_discover(&f->channel) == 1);
    CHECK(f->channel.devices[0].dma_enabled && f->channel.devices[0].udma_mode == 5);
    CHECK(f->commands == 3 && !f->dma_starts);
    return &f->channel.devices[0].block;
}
static void test_dma_prds_modes(void)
{
    struct ata_prd prd[3];
    const uint32_t starts[] = { 0, 2, 0xfffe, 0x10000, 0xffff0000, 0xfffffe00 };
    const uint32_t sizes[] = { 2, 512, 65534, 65536, 65538, 131072 };
    for (unsigned s = 0; s < ARRAY_SIZE(starts); s++) for (unsigned n = 0; n < ARRAY_SIZE(sizes); n++) {
        uint32_t phys = starts[s], bytes = sizes[n];
        int count = ata_dma_prd(prd, 3, phys, bytes);
        if ((uint64_t)phys + bytes > (1ull << 32)) { CHECK(count == -FS_EINVAL); continue; }
        CHECK(count >= 1 && count <= 3);
        uint32_t total = 0;
        for (int i = 0; i < count; i++) {
            uint32_t take = prd[i].count ? prd[i].count : 65536;
            CHECK(prd[i].address == phys + total && !(prd[i].count & 1));
            CHECK((prd[i].address & 65535) + take <= 65536);
            CHECK(prd[i].flags == (i + 1 == count ? 0x8000 : 0)); total += take;
        }
        CHECK(total == bytes);
    }
    CHECK(ata_dma_prd(prd, 1, 2, 65536) == -FS_EINVAL);
    CHECK(ata_dma_prd(prd, 3, 1, 512) == -FS_EINVAL);
    CHECK(ata_dma_prd(prd, 3, 0, 511) == -FS_EINVAL);
    CHECK(ata_dma_prd(prd, 3, 0, 0) == -FS_EINVAL);
    CHECK(ata_dma_prd(prd, 3, 0, ATA_DMA_BYTES + 2) == -FS_EINVAL);
    uint8_t data[512] = { 0 };
    fs_wr16(data + 49 * 2, 0x100); fs_wr16(data + 53 * 2, 4); fs_wr16(data + 93 * 2, 0x6000);
    for (unsigned mode = 0; mode <= 6; mode++) {
        fs_wr16(data + 88 * 2, (uint16_t)((1u << (mode + 8)) | ((1u << (mode + 1)) - 1)));
        CHECK(ata_dma_mode(data) == (int)mode);
        fs_wr16(data + 93 * 2, 0x4000);
        CHECK(ata_dma_mode(data) == (mode <= 2 ? (int)mode : -1));
        fs_wr16(data + 93 * 2, 0x2000);
        CHECK(ata_dma_mode(data) == (mode <= 2 ? (int)mode : -1));
        fs_wr16(data + 93 * 2, 0x6000);
    }
    const uint16_t bad[] = { 0x003f, 0x603f, 0x203e, 0x801f, 0x201f, 0xffff };
    for (unsigned i = 0; i < ARRAY_SIZE(bad); i++) { fs_wr16(data + 88 * 2, bad[i]); CHECK(ata_dma_mode(data) == -1); }
    fs_wr16(data + 88 * 2, 0x0407); fs_wr16(data + 53 * 2, 0); CHECK(ata_dma_mode(data) == -1);
    fs_wr16(data + 53 * 2, 4); fs_wr16(data + 49 * 2, 0); CHECK(ata_dma_mode(data) == -1);
    fs_wr16(data + 49 * 2, 0x100); fs_wr16(data + 63 * 2, 0x100); CHECK(ata_dma_mode(data) == -1);
    struct pci_func p = { .class_code = 1, .subclass = 1, .prog_if = 0x80, .command = 1, .bar = { 0, 0, 0, 0, 0xc001 } };
    CHECK(ata_dma_pci_base(&p, 0) == 0xc000 && ata_dma_pci_base(&p, 1) == 0xc008);
    p.prog_if = 0x85; CHECK(!ata_dma_pci_base(&p, 0) && !ata_dma_pci_base(&p, 1));
    p.prog_if = 0x82; CHECK(ata_dma_pci_base(&p, 0) == 0xc000);
    p.prog_if = 0; CHECK(!ata_dma_pci_base(&p, 0)); p.prog_if = 0x80;
    const uint32_t bars[] = { 0, 1, 0xc000, 0xc009, 0x10001, 0xffffffff };
    for (unsigned i = 0; i < ARRAY_SIZE(bars); i++) { p.bar[4] = bars[i]; CHECK(!ata_dma_pci_base(&p, 0)); }
    p.bar[4] = 0xfff1; CHECK(ata_dma_pci_base(&p, 1) == 0xfff8);
    p.command = 0; CHECK(!ata_dma_pci_base(&p, 0)); p.command = 1;
    p.subclass = 6; CHECK(!ata_dma_pci_base(&p, 0));
    /* Real PTE walker: contiguous stacks, writable reads and MMIO rejection. */
    dma_mapping_base = 0xf0000000; dma_mapping_phys = 0x200000;
    uint32_t phys;
    CHECK(bm_physical(0, (void *)(uintptr_t)(dma_mapping_base + 17), 8192, true, &phys) && phys == 0x200011);
    dma_mapping_fragmented = true;
    CHECK(!bm_physical(0, (void *)(uintptr_t)dma_mapping_base, 8192, true, &phys)); dma_mapping_fragmented = false;
    dma_mapping_readonly = true;
    CHECK(!bm_physical(0, (void *)(uintptr_t)dma_mapping_base, 512, true, &phys));
    CHECK(bm_physical(0, (void *)(uintptr_t)dma_mapping_base, 512, false, &phys)); dma_mapping_readonly = false;
    dma_mapping_uncached = true;
    CHECK(!bm_physical(0, (void *)(uintptr_t)dma_mapping_base, 512, false, &phys)); dma_mapping_uncached = false;
    CHECK(!bm_physical(0, (void *)(uintptr_t)0xffffff00, 512, false, &phys));
    CHECK(!bm_physical(0, (void *)(uintptr_t)0x400000, 512, false, &phys));
    puts("ata DMA: PRD boundaries/EOT/count-zero/32-bit overflow; selected mode/cable/PCI/PTE admission: PASS");
}
static void test_dma_attachment(void)
{
    struct pci_func p = { .bus = 0, .dev = 7, .fn = 1, .class_code = 1, .subclass = 1,
        .prog_if = 0x80, .command = 1, .bar = { 0, 0, 0, 0, 0xc001 } };
    for (unsigned variant = 0; variant < 8; variant++) {
        native_dma_ports = false; registry_init();
        npci = 1; pci[0] = p;
        struct fake f; fresh(&f, false);
        native_dma_ports = true; native_dma_config_fail = variant == 6;
        native_dma_allocs = native_dma_frees = native_dma_config_writes = 0;
        native_dma_alloc_fail = variant == 4 ? 1 : variant == 5 ? 2 : 0;
        native_dma_status[0] = native_dma_status[1] = variant == 3 ? 0x80 : 0x60;
        native_dma_command[0] = native_dma_command[1] = 0;
        native_dma_pci_command = 0x141; /* preserve I/O, parity, SERR */
        if (variant == 1) { npci = 2; pci[1] = p; }
        if (variant == 2) CHECK(registry_claim(RES_PORT, 0xc000, 0xc008, "conflict", false) >= 0);
        if (variant == 7) native_dma_status[0] |= 1;
        ata_dma_attach(&f.channel, variant == 3 ? 1 : 0);
        bool ok = variant == 0;
        CHECK(!!f.channel.dma.ops == ok);
        CHECK(f.channel.stopped == (variant == 7));
        CHECK(!(native_dma_command[0] & 1));
        CHECK(native_dma_config_writes == ((ok || variant == 6) ? 1u : 0u));
        CHECK(native_dma_frees == (variant == 5 ? 1u : variant == 6 ? 33u : 0u));
        if (ok) {
            CHECK(f.channel.dma.base == 0xc000 && f.channel.dma.claim >= 0);
            CHECK(native_dma_pci_command == 0x145 && native_dma_config_address == 0x80003904);
            CHECK(f.channel.dma.prd == (struct ata_prd *)native_dma_table && f.channel.dma.bounce == native_dma_bounce);
            CHECK(!registry_activate(f.channel.dma.claim, f.channel.dma.generation));
            f.channel.native = true;
            ata_dma_revoke(&f.channel);
            CHECK(native_dma_pci_command == 0x141 && native_dma_config_writes == 2);
            f.channel.native = false;
        }
        if (variant >= 3) CHECK(registry_get(registry_count() - 1)->state == RS_QUARANTINED);
        native_dma_ports = false;
    }
    struct fake f; fresh(&f, false);
    CHECK(ata_dma_setup(&f.channel, 0xc000, &dma_fake_ops, &f, dma_table, 0x10fff0, dma_bounce, 0x200000) == -FS_EINVAL);
    CHECK(ata_dma_setup(&f.channel, 0xc000, &dma_fake_ops, &f, dma_table, 0x100002, dma_bounce, 0x200000) == -FS_EINVAL);
    CHECK(ata_dma_setup(&f.channel, 0xc000, &dma_fake_ops, &f, dma_table, 0x200000, dma_bounce, 0x200000) == -FS_EINVAL);
    CHECK(ata_dma_setup(&f.channel, 0xc001, &dma_fake_ops, &f, dma_table, 0x100000, dma_bounce, 0x200000) == -FS_EINVAL);
    CHECK(ata_dma_setup(&f.channel, 0xc000, &dma_fake_ops, &f, dma_table, 0x100000, dma_bounce, 0xffff0000) == -FS_EINVAL);
    registry_init();
    puts("ata DMA: PCI attach/claims/ambiguity/simplex/busy/allocation failures/16-bit command readback/emergency master revocation: PASS");
}
static void test_dma(void)
{
    test_dma_prds_modes();
    test_dma_attachment();
    {
        struct fake f; struct blkdev *b=dma_qualify(&f,false); uint8_t data[512];
        uint64_t initial_pio=b->stats.pio;
        capture_logs=true; captured_logs[0]=0;
        CHECK(!b->read(b,0,1,data) && b->stats.dma==1 && b->stats.pio==initial_pio);
        char expected[128];
        snprintf(expected,sizeof(expected),"[ata0] dma first-completion unit=0 elapsed_us=%u\n",
            (unsigned)(f.channel.devices[0].elapsed_ms*1000));
        CHECK(!strcmp(captured_logs,expected));
        captured_logs[0]=0;
        CHECK(!b->read(b,0,1,data) && b->stats.dma==2 && !captured_logs[0]);
        f.fault=DMA_ERROR;
        CHECK(b->read(b,0,1,data)==-FS_EIO && b->stats.dma==2 && b->stats.pio==initial_pio);
        CHECK(strstr(captured_logs,"[ata0] dma quarantine unit=0"));
        CHECK(strstr(captured_logs,"[ata0] dma fallback unit=0") && strstr(captured_logs,"mode=pio\n"));
        captured_logs[0]=0; f.fault=GOOD;
        CHECK(!b->read(b,0,1,data) && b->stats.dma==2 && b->stats.pio==initial_pio+1 && !captured_logs[0]);
        capture_logs=false;
        b=dma_qualify(&f,false); f.fault=DMA_ACTIVE;
        capture_logs=true; captured_logs[0]=0;
        CHECK(b->read(b,0,1,data)==-FS_EIO && !b->stats.dma);
        CHECK(!strstr(captured_logs,"first-completion")); capture_logs=false;
        puts("PASS f1-86 ATA: first validated DMA completion once/channel, exact elapsed, failed DMA excluded, quarantine/fallback and later PIO");
    }
    for (unsigned fail=0;fail<2;fail++) {
        struct fake f; struct blkdev *b=dma_qualify(&f,false);
        unsigned pinned=dma_pin_live;
        scatter_mapping=true; dma_pin_fail=fail ? 2 : 0;
        memset(scatter_data,0,sizeof(scatter_data));
        CHECK(!b->read(b,0,sizeof(scatter_data)/512,scatter_data));
        CHECK(f.channel.dma.bounced==!!fail && !f.channel.dma.pin_count && dma_pin_live==pinned);
        for (unsigned i=0;i<sizeof(scatter_data);i+=2)
            CHECK(fs_rd16(scatter_data+i)==sector_word(i/512,(i%512)/2));
        CHECK(!b->write(b,0,sizeof(scatter_data)/512,scatter_data));
        CHECK(dma_pin_live==pinned && !f.channel.dma.pin_count);
        scatter_mapping=false; dma_pin_fail=0;
    }
    puts("ata DMA: scattered page vectors, direct pin retirement and partial pin failure bounce rollback: PASS");
    uint8_t *buf = malloc(513u * 512 + 1); CHECK(buf);
    const unsigned counts[] = { 1, 2, 127, 128, 129, 255, 256, 257, 513 };
    for (unsigned ext = 0; ext < 2; ext++) for (unsigned bounce = 0; bounce < 3; bounce++)
        for (unsigned i = 0; i < ARRAY_SIZE(counts); i++) {
            struct fake f; struct blkdev *b = dma_qualify(&f, ext);
            uint64_t lba = ext ? 0x123456789ull : 0x12345;
            unsigned n = counts[i], before = f.data_reads, delivered = f.delivered;
            f.noncontiguous = bounce == 1;
            if (bounce == 2) f.direct_phys |= 1;
            f.immediate = (i & 1) != 0;
            memset(buf, 0, n * 512);
            CHECK(!b->read(b, lba, n, buf));
            for (unsigned sector = 0; sector < n; sector++) for (unsigned word = 0; word < 256; word++)
                CHECK(fs_rd16(buf + sector * 512 + word * 2) == sector_word(lba + sector, word));
            CHECK(f.command == (ext ? 0x25 : 0xc8) && f.data_reads == before);
            CHECK(f.dma_starts == (n + 127) / 128 && f.delivered - delivered == f.dma_starts);
            CHECK(f.channel.devices[0].dma_bytes == n * 512 && f.channel.devices[0].dma_bounces == (bounce ? f.dma_starts : 0));
            CHECK(!b->write(b, lba, n, buf));
            CHECK(f.command == (ext ? 0x35 : 0xca) && !f.data_writes);
            CHECK(!(f.bm_command & 1) && !(f.bm_status & 7) && !f.channel.dma.armed);
            CHECK(f.channel.devices[0].dma_requests == f.dma_starts && f.channel.devices[0].dma_bytes == 2 * n * 512);
            CHECK(!b->flush(b) && f.command == (ext ? 0xea : 0xe7));
        }
    const enum fault faults[] = { ERR, DF, BUSY, EXTRA_DRQ, NO_IRQ, DMA_ERROR, DMA_ACTIVE, DMA_NO_BM_IRQ, DMA_STUCK_MASTER, DMA_ERROR_NO_IRQ };
    for (unsigned write = 0; write < 2; write++) for (unsigned i = 0; i < ARRAY_SIZE(faults); i++) {
        struct fake f; struct blkdev *b = dma_qualify(&f, false);
        f.noncontiguous = true; f.fault = faults[i]; unsigned before = f.commands;
        if (write) fill(buf, 0, 1); else memset(buf, 0x5a, 512);
        CHECK((write ? b->write(b, 0, 1, buf) : b->read(b, 0, 1, buf)) == -FS_EIO);
        struct ata_device *d = &f.channel.devices[0];
        CHECK(d->dma_quarantined && !d->dma_enabled && d->dma_errors == 1 && !d->dma_requests);
        CHECK(f.commands == before + 1 && !f.channel.dma.armed && !(f.bm_command & 1));
        if (!write) CHECK(buf[0] == 0x5a && buf[511] == 0x5a); /* failed bounce never published */
        bool unsafe = faults[i] == BUSY || faults[i] == DF || faults[i] == EXTRA_DRQ || faults[i] == DMA_STUCK_MASTER;
        CHECK(f.channel.stopped == unsafe && b->quarantined == unsafe);
        before = f.commands;
        f.fault = GOOD;
        if (unsafe) CHECK(b->read(b, 0, 1, buf) == -FS_EQUARANTINED && f.commands == before);
        else {
            unsigned starts = f.dma_starts;
            CHECK(!b->read(b, 0, 1, buf) && f.command == 0x20 && f.commands == before + 1 && f.dma_starts == starts);
        }
    }
    struct fake f; struct blkdev *b = dma_qualify(&f, false);
    f.early_dma_irq = f.immediate = true;
    CHECK(b->read(b, 0, 1, buf) == -FS_EIO && !f.dma_starts && f.channel.devices[0].dma_quarantined);
    b = dma_qualify(&f, false);
    f.early_dma_irq = f.early_dma_ready = f.immediate = f.noncontiguous = true;
    memset(buf, 0x5a, 512);
    CHECK(b->read(b, 0, 1, buf) == -FS_EIO && !f.dma_starts && !f.channel.devices[0].dma_requests);
    CHECK(buf[0] == 0x5a && buf[511] == 0x5a); /* stale ready IRQ cannot publish an unstarted buffer */
    CHECK(f.channel.stopped && b->quarantined); /* the drive command's completion is uncertain */
    dma_qualify(&f, false); f.noncontiguous = true; on_schedule = stale_schedule;
    CHECK(f.channel.devices[0].block.read(&f.channel.devices[0].block, 0, 1, buf) == -FS_EIO);
    CHECK(f.channel.devices[0].dma_quarantined && !f.channel.stopped); /* unmatched generation times out */
    dma_qualify(&f, false); on_schedule = quiesce_schedule;
    CHECK(f.channel.devices[0].block.read(&f.channel.devices[0].block, 0, 1, buf) == -FS_EIO);
    CHECK(f.channel.stopped && f.channel.quiescing && !(f.bm_command & 1));
    dma_fresh(&f, false); f.fault = DMA_MODE_ERROR;
    CHECK(ata_channel_discover(&f.channel) == 1 && !f.channel.devices[0].dma_enabled && f.channel.devices[0].dma_quarantined);
    f.fault = GOOD; CHECK(!f.channel.devices[0].block.read(&f.channel.devices[0].block, 0, 1, buf) && f.command == 0x20);
    for (unsigned variant = 0; variant < 4; variant++) {
        dma_fresh(&f, false);
        if (variant == 0) f.identify[0][88] &= 255; /* no retained firmware selection */
        if (variant == 1) f.identify[0][93] = 0x4000;
        if (variant == 2) f.identify[0][49] &= ~0x100u;
        if (variant == 3) { f.kind[0] = PACKET; f.identify[0][0] = 0x8500; }
        CHECK(ata_channel_discover(&f.channel) == 1 && !f.channel.devices[0].dma_enabled && f.commands == (variant == 3 ? 1u : 2u));
    }
    dma_fresh(&f, false); f.bm_status = 0;
    CHECK(ata_channel_discover(&f.channel) == 1 && f.channel.devices[0].dma_enabled);
    for (unsigned mode = 0; mode <= 6; mode++) {
        dma_fresh(&f, false);
        f.identify[0][88] = (uint16_t)((1u << (mode + 8)) | 0x7f); /* support exceeds firmware choice */
        f.reset_loses_udma = true;
        CHECK(ata_channel_discover(&f.channel) == 1 && f.channel.devices[0].dma_enabled &&
              f.channel.devices[0].udma_mode == mode); /* never choose the advertised maximum instead */
    }
    dma_fresh(&f, false); f.reset_loses_udma = true;
    CHECK(ata_channel_discover(&f.channel) == 1 && f.channel.devices[0].dma_enabled &&
          f.channel.devices[0].udma_mode == 5 && f.channel.devices[0].firmware_udma == 5);
    dma_fresh(&f, false); f.selected = 1; f.absent_ff = true;
    CHECK(ata_channel_discover(&f.channel) == 1 && f.channel.devices[0].dma_enabled && f.commands == 3);
    dma_fresh(&f, false); f.kind[1] = DISK;
    memcpy(f.identify[1], f.identify[0], sizeof(f.identify[0]));
    f.identify[1][88] = 0x080f; /* peer firmware selected UDMA3 */
    CHECK(ata_channel_discover(&f.channel) == 2 && f.channel.devices[1].udma_mode == 3);
    f.fault = ERR;
    CHECK(f.channel.devices[0].block.read(&f.channel.devices[0].block, 0, 1, buf) == -FS_EIO && !f.channel.stopped);
    CHECK(f.channel.devices[1].dma_enabled && !f.channel.devices[1].block.quarantined);
    f.fault = GOOD;
    CHECK(!f.channel.devices[1].block.read(&f.channel.devices[1].block, 0, 1, buf) && f.selected == 1 && f.command == 0xc8);
    dma_qualify(&f, false); unsigned before = f.writes;
    CHECK(f.channel.devices[0].block.read(&f.channel.devices[0].block, 0, 0, buf) == -FS_EINVAL && f.writes == before);
    CHECK(ata_channel_idle(&f.channel) && !(f.bm_command & 1));
    ata_channel_irq(&f.channel); CHECK(!f.channel.active && !f.channel.dma.armed); /* late edge */
    dma_qualify(&f, false);
    CHECK(read_fault(&f.channel.devices[0], buf) && f.command == 0x20 && f.channel.devices[0].dma_enabled);
    CHECK(!f.dma_starts); /* the existing fault probe selects/restores only its PIO section */
    /* The existing writer must retain EIO even though an idle DMA fault
     * leaves the block device available for future independent PIO I/O. */
    b = dma_qualify(&f, false);
    struct block_cache cache; CHECK(!cache_init(&cache, 8192));
    fill(buf, 0, 1); CHECK(!cache_write(&cache, b, 0, buf));
    f.fault = DMA_ERROR;
    CHECK(cache_barrier(&cache, b) == -FS_EIO && !b->quarantined && f.channel.devices[0].dma_quarantined);
    before = f.commands; f.fault = GOOD;
    CHECK(cache_barrier(&cache, b) == -FS_EIO && f.commands == before && cache_error(&cache, b) == -FS_EIO);
    cache_destroy(&cache);
    free(buf);
    puts("ata DMA: LBA28/48 read/write/chunks/direct/bounce/one IRQ, faults/timeouts/quiescence/quarantine/no retry/PIO fallback/ATAPI PIO: PASS");
}

/* The existing FAT/cache throughput suite can use this boundary instead of
 * memcpy. Link ata_test.c with main renamed; no modeled storage path. */
int ata_host_dma_io(uint8_t *media, uint32_t sectors, uint64_t lba, uint32_t count, void *buf, bool write)
{
    struct fake f; struct blkdev *b = dma_qualify(&f, false);
    f.media_bytes = media; f.media_sectors = sectors;
    f.noncontiguous = true; b->capacity = sectors;
    int e = write ? b->write(b, lba, count, buf) : b->read(b, lba, count, buf);
    CHECK(f.dma_starts == (count + 127) / 128 && f.channel.devices[0].dma_enabled && !f.data_writes);
    return failures ? -FS_EIO : e;
}
