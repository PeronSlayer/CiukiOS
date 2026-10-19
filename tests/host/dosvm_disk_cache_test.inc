/* F3-47 production LRU/overlay/lifetime/ledger regression coverage.
 * SPDX-License-Identifier: GPL-2.0-only */
static void cache_changed_image(uint32_t lba, void *data)
{ memset(data, (lba + 17) % 251, 512); }
/* Feed real creation charges into the separate production PMM/ledger model.
 * The transport fake keeps worker backing on malloc; model that charge in
 * stacks/heap explicitly. Static VM metadata is already in the kernel image. */
static void disk_memory_charge(void)
{
    struct dosvm *v;
    struct dosvm_config cfg = {.dpmi = true, .base = "C:\\GAMES\\CIUKIGAM.IMG"};
    CHECK(!dosvm_create(&cfg, &v));
    CHECK(v->info.commitment == DOSVM_RESIDENT_CAP && v->memory.quota == VM_POOL_CAP);
    CHECK(v->read_pages == DOSVM_READ_BLOCKS * DOSVM_READ_PAGES);
    unsigned owners[MM_OWNERS] = {0}, pages = 0;
    for (unsigned i = 0; i < HOST_PAGES; i++) if (allocated[i]) {
        CHECK(page_owner[i] < MM_OWNERS);
        owners[page_owner[i]]++; pages++;
    }
    struct dosvm_ledger snapshot; dosvm_snapshot(&snapshot);
    CHECK(pages == snapshot.pages && owners[MM_VM_PRIVATE] >= v->read_pages);
    CHECK(owners[MM_VM_POOL] == VM_POOL_PAGES);
    for (unsigned i = 0; i < MM_OWNERS; i++) printf("%u ", owners[i]);
    printf("%u %u %zu %u\n", v->read_pages, KSTACK_SIZE / PAGE_SIZE,
        1 + (sizeof(struct task) + PAGE_SIZE - 1) / PAGE_SIZE, v->info.resident);
    destroy(v, true); empty();
}
static void test_disk_cache(void)
{
    struct dosvm *v; struct dosvm_config cfg = {.dpmi = true,
        .commitment = VM_POOL_CAP + DOSVM_LOW_CAP}; /* constrained fallback */
    CHECK(!dosvm_create(&cfg, &v));
    CHECK(v->read_capacity == 32 && v->read_pages == 512);
    CHECK(v->info.resident <= v->info.commitment);
    for (unsigned i = 0; i < v->read_capacity; i++)
        for (unsigned p = 0; p < 16; p++) CHECK(page_owner[v->read_cache[i].phys / PAGE_SIZE + p] == MM_VM_PRIVATE);
    struct dosvm_ledger ledger; dosvm_snapshot(&ledger);
    CHECK(ledger.pages == DOSVM_PAGES + v->memory.quota / PAGE_SIZE + v->as.tables + 1 + v->audio.q.page_count + v->read_pages);
    uint8_t data[512], sector[512]; unsigned reads = disk_reads;
    for (unsigned i = 0; i < 32; i++) CHECK(!dosvm_disk_read(v, i * 128, sector));
    CHECK(disk_reads == reads + 32);
    /* A full cache span stays resident; promote block zero before eviction. */
    for (unsigned i = 0; i < 32; i++) CHECK(!dosvm_disk_read(v, i * 128 + 1, sector));
    CHECK(disk_reads == reads + 32);
    CHECK(!dosvm_disk_read(v, 0, sector));
    CHECK(!dosvm_disk_read(v, 32 * 128, sector) && disk_reads == reads + 33);
    CHECK(!dosvm_disk_read(v, 0, sector) && disk_reads == reads + 33);
    CHECK(!dosvm_disk_read(v, 128, sector) && disk_reads == reads + 34);
    /* Coherent new and rewritten overlay sectors, including failed writes. */
    memset(data, 0xD4, sizeof(data));
    CHECK(!dosvm_disk_write(v, 2, data)); reads = disk_reads;
    CHECK(!dosvm_disk_read(v, 2, sector) && !memcmp(data, sector, 512) && disk_reads == reads);
    memset(data, 0xA8, sizeof(data)); short_write = true;
    CHECK(dosvm_disk_write(v, 2, data) == -V86_EIO); short_write = false;
    CHECK(!dosvm_disk_read(v, 2, sector) && sector[0] == 0xA8 && sector[511] == 0xD4);
    /* Evicted base data must still merge the authoritative overlay. */
    for (unsigned i = 1; i <= 33; i++) CHECK(!dosvm_disk_read(v, i * 128, sector));
    CHECK(!dosvm_disk_read(v, 2, sector) && sector[0] == 0xA8 && sector[511] == 0xD4);
    /* Failed block read / fallback never publishes an incomplete block. */
    fail_read_ahead = true; reads = disk_reads;
    CHECK(!dosvm_disk_read(v, 40 * 128, sector) && disk_reads == reads + 2);
    fail_read_ahead = false; short_read = true;
    CHECK(dosvm_disk_read(v, 40 * 128, sector) == -V86_EIO); short_read = false;
    CHECK(!dosvm_disk_read(v, 40 * 128, sector));
    uint32_t resident = v->info.resident;
    CHECK(!dosvm_disk_close(v, true) && !v->read_pages && v->info.resident == resident - (2u << 20));
    /* Reopen a different image in the same VM; no old generation survives. */
    disk_base_sector = cache_changed_image;
    CHECK(!dosvm_disk_open(v, "C:\\GAMES\\CIUKIGAM.IMG")); reads = disk_reads;
    disk_base_sector = 0;
    CHECK(!dosvm_disk_read(v, 2, sector) && sector[0] == 19 && disk_reads == reads + 1);
    CHECK(!dosvm_disk_read(v, DOSVM_SECTORS - 1, sector));
    struct dosvm_disk_cost *d = &v->perf.interval[v->perf.active_interval].disk;
    CHECK(d->touched[255] & 0x80); /* final 64 KiB region of the geometry */
    destroy(v, true); empty();
    /* The cache is part of the existing low-memory commitment too. */
    v = create(); CHECK(v->read_capacity == 32 && v->read_pages == 512);
    destroy(v, true); empty();
    printf("f3-47 disk: LRU span/eviction, overlay merge/partial-write invalidation, reopen, failed reads, MM_VM_PRIVATE ownership/teardown PASS\n");
}
static void test_disk_cache_sizing(void)
{
    /* Isolate exact disk-allocation boundaries from creation overhead. */
    static struct dosvm v;
    const unsigned sectors[] = {361, 128 * 32, 128 * 32 + 1, 128 * 192,
                                128 * 256, DOSVM_SECTORS};
    const unsigned blocks[] = {3, 32, 33, 192, 256, 256};
    for (unsigned i = 0; i < ARRAY_SIZE(sectors); i++) {
        memset(&v, 0, sizeof(v));
        v.info.commitment = DOSVM_RESIDENT_CAP;
        v.info.resident = sizeof(v);
        v.base_sectors = sectors[i];
        unsigned before = used_pages;
        CHECK(!read_cache_open(&v) && v.read_capacity == blocks[i]);
        CHECK(v.read_pages == blocks[i] * DOSVM_READ_PAGES);
        CHECK(v.info.resident == sizeof(v) + blocks[i] * 65536u);
        CHECK(used_pages == before + v.read_pages);
        for (unsigned n = 0; n < v.read_capacity; n++)
            for (unsigned p = 0; p < DOSVM_READ_PAGES; p++)
                CHECK(page_owner[v.read_cache[n].phys / PAGE_SIZE + p] == MM_VM_PRIVATE);
        read_cache_close(&v);
        CHECK(used_pages == before && v.info.resident == sizeof(v) && !v.read_pages);
    }
    /* Exact boundaries include the not-yet-created worker's charge. */
    const unsigned worker = KSTACK_SIZE + PAGE_SIZE + sizeof(struct task);
    v.base_sectors = 128 * 256;
    v.info.resident = v.info.commitment - (16u << 20) - worker;
    CHECK(!read_cache_open(&v) && v.read_capacity == 256);
    read_cache_close(&v);
    v.info.resident++;
    CHECK(!read_cache_open(&v) && v.read_capacity == 32);
    read_cache_close(&v);
    v.info.resident = v.info.commitment - (2u << 20) - worker;
    CHECK(!read_cache_open(&v) && v.read_capacity == 32);
    read_cache_close(&v);
    v.info.resident++;
    CHECK(read_cache_open(&v) == -ENOMEM && !v.read_capacity && !v.read_pages);
    v.info.resident = v.info.commitment + 1;
    CHECK(read_cache_open(&v) == -ENOMEM && !v.read_capacity && !v.read_pages);
    /* Failure at the first/middle/last allocation of a full 16 MiB cache. */
    const unsigned failures[] = {1, 128, 256};
    for (unsigned i = 0; i < ARRAY_SIZE(failures); i++) {
        v.info.resident = sizeof(v);
        contiguous_calls = 0; fail_contiguous_at = failures[i];
        CHECK(read_cache_open(&v) == -ENOMEM && !v.read_capacity && !v.read_pages);
        CHECK(v.info.resident == sizeof(v));
        fail_contiguous_at = 0; empty();
    }
    /* The largest cache must work through default production creation,
     * including the eager pool and the final non-pool residency check. */
    struct dosvm *full;
    struct dosvm_config cfg = {.dpmi = true, .base = "C:\\GAMES\\CIUKIGAM.IMG"};
    CHECK(!dosvm_create(&cfg, &full));
    CHECK(full->memory.quota == VM_POOL_CAP && full->info.commitment == DOSVM_RESIDENT_CAP);
    CHECK(full->read_capacity == 256 && full->read_pages == 4096);
    struct dosvm_ledger ledger; dosvm_snapshot(&ledger);
    CHECK(ledger.resident == full->info.resident);
    CHECK(ledger.pages == DOSVM_PAGES + VM_POOL_PAGES + full->as.tables + 1 +
          full->audio.q.page_count + 4096);
    printf("f3-47 accounting maximum-cache: commitment=%u resident=%u pool=%u cache=%u private=%u struct=%zu\n",
        full->info.commitment, full->info.resident, full->memory.quota,
        full->read_pages * PAGE_SIZE, full->info.resident - full->memory.quota,
        sizeof(*full));
    uint8_t sector[512]; unsigned reads = disk_reads;
    for (unsigned i = 0; i < 256; i++) CHECK(!dosvm_disk_read(full, i * 128, sector));
    CHECK(disk_reads == reads + 256);
    for (unsigned i = 0; i < 256; i++) CHECK(!dosvm_disk_read(full, i * 128 + 1, sector));
    CHECK(disk_reads == reads + 256);
    CHECK(!dosvm_disk_read(full, 0, sector));
    CHECK(!dosvm_disk_read(full, 256 * 128, sector) && disk_reads == reads + 257);
    CHECK(!dosvm_disk_read(full, 0, sector) && disk_reads == reads + 257);
    CHECK(!dosvm_disk_read(full, 128, sector) && disk_reads == reads + 258);
    destroy(full, true); empty();
    puts("f3-47 sizing: image rounding/16MiB ceiling, commitment/worker headroom, 2MiB fallback, full-size ownership/unwind PASS");
}
static void test_default_disk_admission(void)
{
    empty();
    const unsigned required = (DOSVM_RESIDENT_CAP + DOSVM_HEADROOM) / PAGE_SIZE;
    const unsigned free = pmm_free_count(); CHECK(free >= required);
    /* Model the constrained free-RAM envelope on a 128 MiB host: exactly
     * one default admission fits; one page less refuses before construction. */
    withheld_pages = free - required + 1;
    unsigned before = allocation_calls;
    struct dosvm *v = (void *)1;
    struct dosvm_config cfg = {.dpmi = true, .base = "C:\\GAMES\\CIUKIGAM.IMG"};
    CHECK(dosvm_create(&cfg, &v) == -ENOMEM && !v && allocation_calls == before);
    empty(); withheld_pages--;
    CHECK(!dosvm_create(&cfg, &v));
    CHECK(v->info.commitment == DOSVM_RESIDENT_CAP && v->memory.quota == VM_POOL_CAP);
    CHECK(v->read_pages == DOSVM_READ_BLOCKS * DOSVM_READ_PAGES);
    CHECK(v->info.resident <= v->info.commitment);
    char expected[96];
    snprintf(expected, sizeof(expected), " request=%u headroom=%u admitted=1", DOSVM_RESIDENT_CAP, DOSVM_HEADROOM);
    CHECK(strstr(last_record, expected)); /* vm_start uses the diagnostic sink */
    struct dosvm *second = (void *)1; before = allocation_calls;
    CHECK(dosvm_create(&cfg, &second) == -ENOMEM && !second && allocation_calls == before);
    CHECK(dosvm_valid(v->info.key) && v->memory.quota == VM_POOL_CAP);
    destroy(v, true); empty(); withheld_pages = 0;
    puts("f3-47 admission: default request/headroom boundary, pre-creation refusal, full-cache start, outstanding second-VM refusal PASS");
}
static uint64_t disk_latency_step;
static uint64_t disk_latency_clock(void)
{ uint64_t now = fake_tsc; fake_tsc += disk_latency_step; return now; }
static void test_disk_latency(void)
{
    struct dosvm *v = create();
    static const unsigned us[] = {0, 99, 100, 999, 1000, 9999, 10000, 50000, 50001};
    g_tsc_per_ms = 1000; tsc_clock_hook = disk_latency_clock;
    for (unsigned i = 0; i < ARRAY_SIZE(us); i++) {
        disk_latency_step = us[i];
        struct v86_frame f = disk_frame(0);
        CHECK(!dosvm_bios(v, &f, 0x13));
    }
    tsc_clock_hook = 0;
    struct dosvm_disk_cost *d = &v->perf.interval[v->perf.active_interval].disk;
    CHECK(d->latency[0] == 2 && d->latency[1] == 2 && d->latency[2] == 2 && d->latency[3] == 2 && d->latency[4] == 1);
    /* Full-width wire counters fit; emit one interval and verify its exact
     * region set/counters are cleared without touching the producer bank. */
    struct dosvm_disk_cost widest; memset(&widest, 0xff, sizeof(widest)); widest.regions = 2048;
    struct dosvm_perf_sample sample = {0};
    uint32_t generation = v->info.key.generation;
    v->info.key.generation = UINT32_MAX;
    perf_live(v, &sample, UINT32_MAX, &widest);
    v->info.key.generation = generation;
    g_ticks += DOSVM_PERF_INTERVAL; dosvm_perf_report(v);
    CHECK(v->perf.sample_count == 1 && v->perf.active_interval == 1);
    for (unsigned i = 0; i < 5; i++) CHECK(!v->perf.interval[0].disk.latency[i]);
    g_tsc_per_ms = 0; fake_tsc = 0;
    destroy(v, true); empty();
    /* Allocation failure at each end and the middle unwinds the ledger. */
    const unsigned failures[] = {1, 16, 32};
    for (unsigned i = 0; i < ARRAY_SIZE(failures); i++) {
        contiguous_calls = 0; fail_contiguous_at = failures[i];
        CHECK(dosvm_create(0, &v) == -ENOMEM && !v);
        fail_contiguous_at = 0; empty();
    }
    puts("f3-47 latency: exact 100/1000/10000/50000us boundaries, interval reset, full-width records, allocation unwind PASS");
}

static uint64_t provider_test_clock(void *arg) { (void)arg; return (uint64_t)g_ticks*1000; }
static unsigned provider_test_work;
static void provider_test_pause(void *arg) { (void)arg; g_ticks++; provider_test_work++; }
static void test_disk_provider_deadline(void) {
    struct dosvm *v; struct dosvm_config cfg={.dpmi=true}; CHECK(!dosvm_create(&cfg,&v));
    struct blkdev provider; dosvm_disk_provider_init(v,&provider);
    struct vfs_wait_info info={0};
    struct vfs_request r={.deadline=provider_test_clock(0)+2000000,.now=provider_test_clock,
        .pause=provider_test_pause,.owner=49,.wait=&info};
    host_storage_request=&r; v->disk_busy=true; uint8_t sector[512]; memset(sector,0xa5,sizeof(sector));
    unsigned before=disk_reads;
    CHECK(provider.read(&provider,0,1,sector)==-VFS_ETIMEOUT);
    CHECK(provider_test_clock(0)==r.deadline && info.reason==VFS_WAIT_PROVIDER && info.elapsed_us==2000000);
    CHECK(info.outcome==-VFS_ETIMEOUT && sector[0]==0xa5 && sector[511]==0xa5 && disk_reads==before && provider_test_work==2000);
    v->disk_busy=false; host_storage_request=0;
    CHECK(!provider.read(&provider,0,1,sector)); destroy(v,true); empty();
    puts("H49-08 provider: PASS production disk gate, exact 2000 ms deadline, successor and unrelated work");
}
