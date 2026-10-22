/* f1-92: interrupt edges that are not a command's end, cancellation of an
 * issued command and the PIO repeat of a quarantined DMA request, on the
 * production ATA driver, cache, FAT and VFS. Included by ata_test.c after
 * ata_dma_test.inc. SPDX-License-Identifier: GPL-2.0-only
 *
 * T23 trial of the 1.0.0 alpha (image 8f59e221, three boots):
 *   [ata0] dma quarantine ... command=25 issued=1 bm=24 status=50 elapsed_ms=1..5 fallback=pio
 *   [ata0] fault unit=0 command=29 issued=1 status=58 error=00 elapsed_ms=1
 *   [storage] readonly reason=device_quarantined file=UNIFIED.IMG
 * fallback=pio proves the master had been started (an unstarted DMA command
 * prints fallback=blocked), so the replay below models the cause the code
 * allowed: the caller's request was cancelled while its command was issued. */

static bool trial_cancel;
static uint8_t trial_command;
static unsigned trial_hits;
static struct block_cache *trial_cache;
static struct cache_stream *trial_rival;
static struct blkdev *trial_dev;
static bool trial_cancelled(void *arg) { (void)arg; return trial_cancel; }
static uint64_t trial_now(void *arg) { (void)arg; return g_ticks; }
static void trial_pause(void *arg) { (void)arg; }
/* At the Command register write of the selected opcode: either the caller's
 * request is cancelled (DOS budget, VM stop, signal) or another reader
 * begins on the same cache, which decays the in-flight prefetch stream. */
static void trial_hook(struct fake *f, uint8_t command)
{
    (void)f;
    if (!trial_command || command != trial_command) return;
    trial_hits++;
    if (trial_cache) cache_stream_begin(trial_cache, trial_rival, trial_dev, 0);
    else trial_cancel = true;
}
static int trial_read(struct blkdev *b, uint64_t lba, unsigned n, void *buf, uint8_t cancel_at)
{
    struct vfs_request request = { .deadline = UINT64_MAX, .now = trial_now,
        .cancelled = trial_cancelled, .pause = trial_pause, .operation = "dos", .site = "dos" };
    struct fs_request_scope scope;
    trial_cancel = false; trial_command = cancel_at; trial_hits = 0; command_hook = trial_hook;
    fs_request_enter(&scope, &request);
    int e = b->read(b, lba, n, buf);
    fs_request_leave(&scope);
    command_hook = 0; trial_command = 0;
    return e;
}
static bool sectors_match(const uint8_t *buf, uint64_t lba, unsigned n)
{
    for (unsigned i = 0; i < n * 256; i++)
        if (fs_rd16(buf + i * 2) != sector_word(lba + i / 256, i % 256)) return false;
    return true;
}
/* DMA (UDMA5) plus READ/WRITE MULTIPLE of 16 sectors, LBA48: the T23 unit's
 * command set (25h/35h DMA, 29h/39h PIO multiple). */
static struct blkdev *trial_qualify(struct fake *f)
{
    dma_fresh(f, true);
    f->identify[0][47] = 0x8010;
    CHECK(ata_channel_discover(&f->channel) == 1);
    struct ata_device *d = &f->channel.devices[0];
    CHECK(d->dma_enabled && d->multiple_sectors == 16 && d->lba48);
    return &d->block;
}

static const char *f192_verdict(unsigned before) { return failures == before ? "PASS" : "FAIL"; }

static void test_f192_early_edges(void)
{
    unsigned before = failures;
    uint8_t *buf = malloc(64 * 512); CHECK(buf);
    /* 1. Edge between the DMA command and Start: a late PIC edge (BM clear)
     * or the drive's early INTRQ (BM Interrupt set), with every status a
     * live drive can show before its data: stale DRDY (the T23 50h),
     * BSY, DRQ/DMARQ. Each must start the master and finish normally. */
    static const uint8_t live[] = { 0x50, 0x40, 0xd0, 0x58 };
    for (unsigned ext = 0; ext < 2; ext++) for (unsigned write = 0; write < 2; write++)
        for (unsigned bmirq = 0; bmirq < 2; bmirq++) for (unsigned i = 0; i < ARRAY_SIZE(live); i++) {
            struct fake f; struct blkdev *b = dma_qualify(&f, ext != 0);
            struct ata_device *d = &f.channel.devices[0];
            f.noncontiguous = (i & 1) != 0;
            f.late_edge = true; f.late_edge_status = live[i];
            f.late_edge_bm = bmirq != 0; /* early INTRQ of the drive, or a late PIC edge */
            unsigned n = 3 + i, reads = f.data_reads, writes = f.data_writes;
            capture_logs = true; captured_logs[0] = 0;
            if (write) fill(buf, 0x40, n); else memset(buf, 0x5a, n * 512);
            CHECK((write ? b->write(b, 0x40, n, buf) : b->read(b, 0x40, n, buf)) == 0);
            if (!write) CHECK(sectors_match(buf, 0x40, n));
            CHECK(f.dma_starts == 1 && f.data_reads == reads && f.data_writes == writes && d->dma_requests == 1);
            CHECK(d->dma_enabled && !d->dma_quarantined && !d->dma_errors && !f.channel.stopped && !b->quarantined);
            CHECK(d->early_irqs == 1 && strstr(captured_logs, "[ata0] early_irq unit=0 phase=before_start command="));
            CHECK(!strstr(captured_logs, "quarantine") && !strstr(captured_logs, "fault"));
            capture_logs = false;
        }
    /* The same edge with a real abort status stays a failure: ERR is the
     * drive's DMA abort (quarantine + PIO repeat), DF is containment. */
    {
        struct fake f; struct blkdev *b = dma_qualify(&f, false);
        f.late_edge = true; f.late_edge_status = 0x51;
        capture_logs = true; captured_logs[0] = 0;
        CHECK(!b->read(b, 0, 1, buf) && !f.dma_starts && f.channel.devices[0].dma_quarantined);
        CHECK(f.channel.devices[0].dma_retries == 1 && !f.channel.stopped && sectors_match(buf, 0, 1));
        CHECK(strstr(captured_logs, "fallback=pio cause=device_error\n") && !f.channel.devices[0].early_irqs);
        capture_logs = false;
        b = dma_qualify(&f, false);
        f.late_edge = true; f.late_edge_status = 0x70;
        CHECK(b->read(b, 0, 1, buf) == -FS_EIO && !f.dma_starts && f.channel.stopped && b->quarantined);
        CHECK(!f.channel.devices[0].dma_retries);
    }
    /* 2. A stray edge after Start while the transfer is live (BSY or DRQ
     * with Active): the late edge of an earlier command, or an early INTRQ
     * that also set the BM Interrupt bit. The master must not be stopped
     * (that aborts the transfer and used to quarantine the channel). */
    for (unsigned variant = 0; variant < 4; variant++) {
        struct fake f; struct blkdev *b = dma_qualify(&f, variant & 1);
        struct ata_device *d = &f.channel.devices[0];
        f.dma_deferred = f.stray_after_start = true;
        f.live_status = variant & 2 ? 0xd0 : 0x58;
        f.stray_bm_irq = variant == 3; /* early INTRQ while BSY: Interrupt with Active, see ata_dma_irq */
        capture_logs = true; captured_logs[0] = 0;
        memset(buf, 0x5a, 8 * 512);
        CHECK(!b->read(b, 0x77, 8, buf) && sectors_match(buf, 0x77, 8));
        CHECK(f.dma_starts == 1 && !f.dma_aborts && f.dma_completions == 1 && d->dma_requests == 1);
        CHECK(d->dma_enabled && !d->dma_quarantined && !f.channel.stopped);
        CHECK(d->early_irqs == 1 && strstr(captured_logs, "phase=dma_active"));
        capture_logs = false;
        fill(buf, 0x91, 8);
        f.dma_deferred = f.stray_after_start = true;
        CHECK(!b->write(b, 0x91, 8, buf) && f.dma_starts == 2 && !f.dma_aborts && d->early_irqs == 2);
    }
    /* A drive that ends the command but whose master never latches the
     * Interrupt: bounded grace (not the 30 s deadline), then the usual
     * DMA quarantine, cause bm_no_interrupt, and the PIO repeat. */
    {
        struct fake f; struct blkdev *b = dma_qualify(&f, false);
        f.fault = DMA_NO_BM_IRQ;
        uint64_t start = g_ticks;
        capture_logs = true; captured_logs[0] = 0;
        CHECK(!b->read(b, 0, 1, buf) && f.channel.devices[0].dma_quarantined && !f.channel.stopped);
        CHECK(g_ticks - start < 1000 && strstr(captured_logs, "cause=bm_no_interrupt\n"));
        CHECK(strstr(captured_logs, "[ata0] dma retry unit=0") && strstr(captured_logs, "result=0 count=1\n"));
        capture_logs = false;
    }
    /* 3. PIO READ MULTIPLE (and the single-sector protocol) with an edge
     * while the drive is still BSY, including short final blocks. Status
     * 58h after the command is the data phase, never a fault. */
    static const unsigned counts[] = { 1, 15, 16, 17, 33, 128 };
    for (unsigned ext = 0; ext < 2; ext++) for (unsigned multiple = 0; multiple < 2; multiple++)
        for (unsigned i = 0; i < ARRAY_SIZE(counts); i++) {
            struct fake f; fresh(&f, ext != 0);
            if (multiple) f.identify[0][47] = 0x8010;
            CHECK(ata_channel_discover(&f.channel) == 1);
            struct blkdev *b = &f.channel.devices[0].block;
            uint8_t *data = malloc(128 * 512); CHECK(data);
            unsigned n = counts[i], reads = f.data_reads;
            f.pio_early_edge = f.immediate = true;
            CHECK(!b->read(b, 0x1234, n, data) && sectors_match(data, 0x1234, n));
            CHECK(f.command == (multiple ? (ext ? 0x29 : 0xc4) : (ext ? 0x24 : 0x20)));
            CHECK(f.channel.devices[0].early_irqs == 1 && !f.channel.stopped && !b->quarantined);
            CHECK(f.data_reads - reads == n * 256 && !f.pio_early_edge && !f.busy_reads);
            free(data);
        }
    free(buf);
    printf("%s f1-92 ATA edges: before Start (late PIC/early INTRQ, DRDY 50h/BSY/DRQ, LBA28/48, read/write) starts the master; "
         "stray edge on a live transfer keeps it running; ERR/DF edges stay failures; missing BM Interrupt has a bounded grace; "
         "PIO/MULTIPLE edge while BSY with short final blocks\n", f192_verdict(before));
}

static void test_f192_cancelled_inflight(void)
{
    unsigned before = failures;
    uint8_t *buf = malloc(300 * 512); CHECK(buf);
    struct fake f; struct blkdev *b = trial_qualify(&f);
    struct ata_device *d = &f.channel.devices[0];
    capture_logs = true; captured_logs[0] = 0;
    /* Trial step 1: the request is cancelled while READ DMA EXT (25h) is
     * issued. The command runs to its device end; nothing is quarantined. */
    memset(buf, 0x5a, 32 * 512);
    unsigned commands = f.commands;
    int e = trial_read(b, 0x2000, 32, buf, 0x25);
    CHECK(trial_hits == 1 && e != 0 && e != -FS_EIO && f.commands == commands + 1);
    CHECK(sectors_match(buf, 0x2000, 32)); /* the completed transfer was published */
    CHECK(d->dma_enabled && !d->dma_quarantined && !d->dma_errors && !f.channel.stopped && !b->quarantined);
    CHECK(d->interrupted_inflight == 1 && strstr(captured_logs, "[ata0] interrupted_inflight unit=0 command=25 result=0"));
    CHECK(!strstr(captured_logs, "dma quarantine"));
    /* Trial step 2: the same on READ MULTIPLE EXT (29h, the T23's PIO
     * command), cancelled while the drive holds DRQ (58h) with a short
     * final block (17 = 16 + 1). */
    d->dma_enabled = false;
    captured_logs[0] = 0; commands = f.commands;
    e = trial_read(b, 0x3000, 17, buf, 0x29);
    CHECK(trial_hits == 1 && e != 0 && e != -FS_EIO && f.commands == commands + 1);
    CHECK(sectors_match(buf, 0x3000, 17) && f.channel.devices[0].block.stats.pio >= 1);
    CHECK(!f.channel.stopped && !b->quarantined && d->interrupted_inflight == 2);
    CHECK(strstr(captured_logs, "interrupted_inflight unit=0 command=29 result=0") && !strstr(captured_logs, "fault"));
    d->dma_enabled = true;
    /* A request already cancelled before its next command issues nothing
     * more: EAGAIN at the command boundary, no port write, no quarantine. */
    struct vfs_request request = { .deadline = UINT64_MAX, .now = trial_now,
        .cancelled = trial_cancelled, .pause = trial_pause };
    struct fs_request_scope scope;
    fs_request_enter(&scope, &request);
    trial_cancel = true; commands = f.commands; unsigned writes = f.writes;
    CHECK(d->block.request_read(&d->block, 0, 8, buf) == -FS_EAGAIN && f.commands == commands && f.writes == writes);
    /* Cancelled while the first of three commands is issued: that command
     * completes, the remaining two are never issued. */
    trial_cancel = false; trial_command = 0x25; trial_hits = 0; command_hook = trial_hook;
    CHECK(d->block.request_read(&d->block, 0, 300, buf) == -FS_EAGAIN && f.commands == commands + 1);
    command_hook = 0; trial_command = 0; trial_cancel = false;
    fs_request_leave(&scope);
    CHECK(!f.channel.stopped && d->dma_enabled && sectors_match(buf, 0, 128));
    /* Trial step 3: write-back and flush still work. */
    fill(buf, 0x4000, 24);
    CHECK(!b->write(b, 0x4000, 24, buf) && !b->flush(b) && d->dma_enabled && !f.channel.stopped);
    capture_logs = false;
    free(buf);
    printf("%s f1-92 ATA T23 replay (driver): request cancelled during READ DMA EXT and during READ MULTIPLE EXT at DRQ completes "
         "on the device, no DMA/channel quarantine, interrupted_inflight records; cancellation honoured only between commands\n",
         f192_verdict(before));
}

/* FAT12 1.44 MB layout, as in tests/host/fs/test_t23_holds.c. */
#define TRIAL_SECTORS 2880u
static void trial_format(uint8_t *disk)
{
    memset(disk, 0, TRIAL_SECTORS * 512);
    disk[0] = 0xeb; disk[1] = 0x3c; disk[2] = 0x90; fs_wr16(disk + 11, 512); disk[13] = 1;
    fs_wr16(disk + 14, 1); disk[16] = 2; fs_wr16(disk + 17, 224); fs_wr16(disk + 19, TRIAL_SECTORS);
    disk[21] = 0xf0; fs_wr16(disk + 22, 9); fs_wr16(disk + 510, 0xaa55);
    for (unsigned i = 0; i < 2; i++) { uint8_t *fat = disk + (1 + i * 9) * 512; fat[0] = 0xf0; fat[1] = fat[2] = 0xff; }
}
/* Issue one prefetch of `count` sectors at `lba` on stream `s`, the way a
 * sequential reader arms it (two sequential demand turns, then queue). */
static bool trial_prefetch(struct block_cache *cache, struct cache_stream *s, struct blkdev *b, uint64_t lba)
{
    uint64_t offset = lba * 512;
    for (unsigned i = 0; i < 2; i++) {
        cache_stream_begin(cache, s, b, offset);
        (void)cache_stream_end(s, offset, 512, true);
        offset += 512;
    }
    cache_stream_queue(s, lba + 2, 32);
    return cache_prefetch_tick(cache);
}

static void test_f192_storage_replay(void)
{
    for (unsigned pio = 0; pio < 2; pio++) {
        unsigned before = failures;
        static uint8_t disk[TRIAL_SECTORS * 512];
        static uint8_t data[96 * 1024], check[96 * 1024];
        trial_format(disk);
        struct fake f; struct blkdev *b = trial_qualify(&f);
        struct ata_device *d = &f.channel.devices[0];
        f.media_bytes = disk; f.media_sectors = TRIAL_SECTORS; b->capacity = TRIAL_SECTORS;
        if (pio) d->dma_enabled = false; /* the trial's later commands ran on 29h */
        static struct block_cache cache; static struct fat_volume vol; static struct vfs space;
        static struct vfs_table table;
        CHECK(!cache_init(&cache, 1024 * 1024) && cache.writeback_buffer);
        CHECK(!fat_mount(&vol, &cache, b, 0, TRIAL_SECTORS, true));
        vfs_init(&space); CHECK(!vfs_attach(&space, 2, &vol) && !vfs_table_init(&space, &table, 2));
        for (unsigned i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 7 + 3);
        size_t done;
        int h = vfs_open(&table, "C:/UNIFIED.IMG", VFS_CREATE | VFS_READ | VFS_WRITE, VFS_DENY_NONE, 0);
        CHECK(h >= 0 && !vfs_write(&table, h, data, sizeof(data), &done) && done == sizeof(data));
        CHECK(!vfs_commit(&table, h));
        /* Two cancelled prefetches, as in each trial boot: a competing reader
         * begins while the speculative command is issued (cache_stream_begin
         * decays every other stream with outstanding work). */
        static struct cache_stream stream, rival;
        memset(&stream, 0, sizeof(stream)); memset(&rival, 0, sizeof(rival));
        capture_logs = true; captured_logs[0] = 0;
        unsigned decayed = 0;
        for (unsigned round = 0; round < 2; round++) {
            cache_invalidate(&cache, b, false);
            trial_cache = &cache; trial_rival = &rival; trial_dev = b;
            trial_command = d->dma_enabled ? 0x25 : 0x29; trial_hits = 0; command_hook = trial_hook;
            unsigned commands = f.commands;
            CHECK(trial_prefetch(&cache, &stream, b, 300 + round * 100));
            command_hook = 0; trial_command = 0; trial_cache = 0;
            CHECK(trial_hits == 1 && f.commands == commands + 1);
            decayed += trial_hits;
            (void)cache_stream_end(&rival, 0, 0, true); /* the competing reader's turn ends */
        }
        CHECK(decayed == 2 && d->interrupted_inflight == 2);
        CHECK(!f.channel.stopped && !b->quarantined && !d->dma_quarantined && d->dma_enabled == !pio);
        /* Write-back after the trial sequence: a DOS save and the image. */
        memset(data, 0x44, 4096);
        int save = vfs_open(&table, "C:/DOSVM.SAV", VFS_CREATE | VFS_WRITE, VFS_DENY_NONE, 0);
        CHECK(save >= 0 && !vfs_write(&table, save, data, 4096, &done) && done == 4096);
        CHECK(!vfs_commit(&table, save) && !vfs_close(&table, save));
        uint64_t pos;
        CHECK(!vfs_seek(&table, h, 4096, VFS_SEEK_SET, &pos) && !vfs_write(&table, h, data, 4096, &done));
        CHECK(!vfs_commit(&table, h));
        CHECK(!vol.readonly && !strstr(captured_logs, "readonly") && !strstr(captured_logs, "quarantine"));
        CHECK(!strstr(captured_logs, "fault") && strstr(captured_logs, "interrupted_inflight unit=0"));
        if (failures != before) printf("f1-92 storage replay records:\n%s", captured_logs);
        capture_logs = false;
        cache_invalidate(&cache, b, false);
        CHECK(!vfs_seek(&table, h, 0, VFS_SEEK_SET, &pos) && !vfs_read(&table, h, check, sizeof(check), &done));
        CHECK(done == sizeof(check));
        for (unsigned i = 0; i < sizeof(check); i++)
            if (check[i] != (i >= 4096 && i < 8192 ? 0x44 : (uint8_t)(i * 7 + 3))) { CHECK(false); break; }
        CHECK(!vfs_close(&table, h));
        cache_stream_close(&stream); cache_stream_close(&rival);
        vfs_table_destroy(&table); CHECK(!vfs_detach(&space, 2)); vfs_destroy(&space); cache_destroy(&cache);
        printf("%s f1-92 ATA T23 replay (storage, %s): two prefetches cancelled in flight by a competing reader, "
               "write-back and commit of UNIFIED.IMG/DOSVM.SAV, no read-only transition (interrupted_inflight=%u)\n",
               f192_verdict(before), pio ? "PIO READ MULTIPLE EXT 29h" : "READ DMA EXT 25h", d->interrupted_inflight);
    }
}

static void test_f192(void)
{
    test_f192_early_edges();
    test_f192_cancelled_inflight();
    test_f192_storage_replay();
}
