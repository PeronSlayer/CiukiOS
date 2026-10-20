/* F3-47: production INT 13h replay, synthetic immutable 12 MiB image.
 * No real DOS executable, ATA latency, or FPS claim. SPDX-License-Identifier: GPL-2.0-only */
static uint64_t disk_trace_clock(void)
{
    struct timespec t;
    CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static uint32_t disk_trace_random(uint32_t *seed)
{ *seed = *seed * 1664525u + 1013904223u; return *seed; }
static void disk_trace_read(struct dosvm *v, unsigned lba, unsigned sectors)
{
    while (sectors) {
        unsigned n = sectors > 127 ? 127 : sectors;
        uint8_t dap[16] = {16, 0}; vm_w16(dap + 2, n);
        vm_w16(dap + 6, 0x3000); vm_w32(dap + 8, lba);
        CHECK(!dosvm_write(v, 0x20000, dap, sizeof(dap)));
        struct v86_frame f = disk_frame(0x4200);
        CHECK(!dosvm_bios(v, &f, 0x13) && !(f.tf.eflags & V86_CF));
        lba += n; sectors -= n;
    }
}
static void disk_trace(const char *name, unsigned zone_mb, bool hot, bool fallback)
{
    unsigned saved_bytes = bundled_bytes;
    bundled_bytes = 12u << 20;
    struct dosvm *v; struct dosvm_config c = {.dpmi = true,
        .commitment = fallback ? VM_POOL_CAP + DOSVM_LOW_CAP : 0,
        .base = "C:\\GAMES\\CIUKIGAM.IMG"};
    CHECK(!dosvm_create(&c, &v));
    CHECK(v->memory.quota == VM_POOL_CAP);
    CHECK(v->info.commitment == (fallback ? c.commitment : DOSVM_RESIDENT_CAP));
    CHECK(v->read_capacity == (fallback ? DOSVM_READ_FALLBACK_BLOCKS : 192));
    unsigned cache_pages = 0;
    for (unsigned i = 0; i < v->read_capacity; i++)
        for (unsigned p = 0; p < DOSVM_READ_PAGES; p++) {
            CHECK(page_owner[v->read_cache[i].phys / PAGE_SIZE + p] == MM_VM_PRIVATE);
            cache_pages++;
        }
    CHECK(cache_pages == v->read_pages);
    printf("f3-47 accounting trace=%s commitment=%u resident=%u pool=%u cache=%u private=%u struct=%zu\n",
        name, v->info.commitment, v->info.resident, v->memory.quota, v->read_pages * PAGE_SIZE,
        v->info.resident - v->memory.pool.resident * PAGE_SIZE, sizeof(*v));
    g_current = v->task; /* no controller copy checkpoints in INT 13h */
    g_tsc_per_ms = 1000000; tsc_clock_hook = disk_trace_clock;
    uint32_t seed = 47, zone_used = 0;
    /* 384 immutable lumps, 1..64 KiB, distributed over the whole image.
     * A FIFO purgeable-cache model reserves 3 MiB for static game data.
     * It is a capacity model, not DOOM's exact zone allocator. */
    bool cached[384] = {0}; unsigned queue[384], head = 0, count = 0;
    uint32_t sizes[384], positions[384];
    for (unsigned i = 0; i < 384; i++) {
        sizes[i] = (1 + (disk_trace_random(&seed) >> 16) % 64) * 1024;
        positions[i] = (disk_trace_random(&seed) >> 8) % ((12u << 20) / 512 - sizes[i] / 512);
    }
    seed = 47;
    unsigned requests = 0, all_hit = 0, host_reads = 0; uint64_t hit_max = 0;
    for (unsigned i = 0; i < 4000; i++) {
        if (i == 2000) {
            memset(&v->perf.interval[v->perf.active_interval].disk, 0, sizeof(struct dosvm_disk_cost));
            requests = all_hit = 0; host_reads = disk_reads;
        }
        unsigned lump = (disk_trace_random(&seed) >> 8) % 384;
        g_ticks += (i & 1) ? 3 : 2; /* logical 400 requests/s */
        if (zone_mb && cached[lump]) continue;
        if (zone_mb) {
            unsigned limit = (zone_mb - 3) << 20;
            while (zone_used + sizes[lump] > limit && count) {
                unsigned old = queue[head]; head = (head + 1) % 384; count--;
                zone_used -= sizes[old]; cached[old] = false;
            }
            cached[lump] = true; zone_used += sizes[lump]; queue[(head + count++) % 384] = lump;
        }
        unsigned sectors = sizes[lump] / 512;
        unsigned lba = hot ? ((positions[lump] / 128) % 32) * 6 * 128 : positions[lump];
        /* A cached lump always refers to stable data, although this model
         * doesn't need the payload contents to simulate purging. */
        unsigned before = disk_reads; uint64_t begin = disk_trace_clock();
        disk_trace_read(v, lba, sectors);
        uint64_t ns = disk_trace_clock() - begin;
        if (i >= 2000) {
            requests++;
            if (disk_reads == before) { all_hit++; if (ns > hit_max) hit_max = ns; }
        }
    }
    struct dosvm_disk_cost *d = &v->perf.interval[v->perf.active_interval].disk;
    printf("f3-47 trace=%s warmup=2000 window_requests=2000 rate=400/s issued=%u request_hits=%u sector_hits=%u misses=%u host_reads=%u bytes=%llu regions=%u bins=%u,%u,%u,%u,%u hit_max_ns=%llu\n",
        name, requests, all_hit, d->hits, d->misses, disk_reads - host_reads,
        (unsigned long long)d->bytes, d->regions, d->latency[0], d->latency[1], d->latency[2],
        d->latency[3], d->latency[4], (unsigned long long)hit_max);
    CHECK(!requests || (uint64_t)d->hits * 100 >= (uint64_t)(d->hits + d->misses) * 95);
    if (!fallback) {
        CHECK((uint64_t)all_hit * 100 >= (uint64_t)requests * 95);
        CHECK(all_hit == requests && !d->misses && !d->reads && disk_reads == host_reads);
    }
    if (hot) CHECK(all_hit == requests && !d->misses && !d->reads);
    tsc_clock_hook = 0; g_tsc_per_ms = 0; g_current = &caller;
    destroy(v, true); empty(); bundled_bytes = saved_bytes;
}
static void test_disk_trace(void)
{
    disk_trace("uniform-default", 0, false, false);
    disk_trace("zone6-default", 6, false, false);
    disk_trace("zone16-default", 16, false, false);
    disk_trace("hot32-default", 0, true, false);
    disk_trace("zone6-constrained-fallback", 6, false, true);
    disk_trace("hot32-constrained-fallback", 0, true, true);
}
