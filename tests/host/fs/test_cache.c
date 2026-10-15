/* f4-04 T0: cache page layouts per device sector size, on the production
 * cache. Mixed 512/2048-byte devices, budget, on-demand conversion, the
 * minimum-cache rule for failed dirty blocks and write-back on conversion.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "cache.h"
#include <stdio.h>
static unsigned failures, checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); failures++; } } while (0)

struct mem {
    struct blkdev dev; uint8_t *bytes; unsigned id;
    uint64_t reads, writes, flushes, fail_write_lba, last_write_lba;
    bool fail_all;
};
static uint8_t pattern(const struct mem *m, uint64_t lba, unsigned i) { return (uint8_t)(lba * 31 + i * 7 + m->id); }
static int mem_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf) {
    struct mem *m = d->ctx; int e = blkdev_range(d, lba, n); if (e) return e;
    m->reads += n; memcpy(buf, m->bytes + lba * d->sector_size, (size_t)n * d->sector_size); return 0;
}
static int mem_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) {
    struct mem *m = d->ctx; int e = blkdev_range(d, lba, n); if (e) return e;
    if (m->fail_all || lba == m->fail_write_lba) return -FS_EIO;
    m->writes += n; m->last_write_lba = lba; memcpy(m->bytes + lba * d->sector_size, buf, (size_t)n * d->sector_size); return 0;
}
static int mem_flush(struct blkdev *d) { struct mem *m = d->ctx; m->flushes++; return 0; }
static void mem_init(struct mem *m, unsigned id, uint32_t sector, uint64_t sectors) {
    memset(m, 0, sizeof(*m)); m->id = id; m->fail_write_lba = UINT64_MAX;
    m->bytes = malloc((size_t)sector * sectors); if (!m->bytes) abort();
    for (uint64_t l = 0; l < sectors; l++) for (unsigned i = 0; i < sector; i++) m->bytes[l * sector + i] = pattern(m, l, i);
    m->dev = (struct blkdev){ .read = mem_read, .write = mem_write, .flush = mem_flush, .capacity = sectors,
        .sector_size = sector, .write_cache_state = BLKDEV_CACHE_ENABLED, .ctx = m };
}
static void mem_free(struct mem *m) { free(m->bytes); }
static bool good(struct mem *m, struct block_cache *c, uint64_t lba) {
    uint8_t buf[2048];
    if (cache_read(c, &m->dev, lba, buf)) return false;
    for (unsigned i = 0; i < m->dev.sector_size; i++) if (buf[i] != pattern(m, lba, i)) return false;
    return true;
}
/* Pure budget arithmetic of cache_init, evaluated for a descriptor density. */
static unsigned model_data(unsigned room, unsigned per_meta) {
    unsigned d = room;
    while (d && d + (d > 1 ? (d - 1 + per_meta - 1) / per_meta : 0) > room) d--;
    return d;
}

static void budget(void) {
    static const size_t sizes[] = { 8192, 12288, 65536, 131072, 1024 * 1024, CACHE_DEFAULT_BYTES, 32u * 1024 * 1024 };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        struct block_cache c; struct cache_stats s;
        CHECK(!cache_init(&c, sizes[i])); cache_stats(&c, &s);
        CHECK(s.pages <= sizes[i] / 4096);
        CHECK(s.pages >= sizes[i] / 4096 - 1);   /* a tiny cache may leave one page unused */
        CHECK(s.slots_512 == 8 * s.data_pages && !s.slots_2048 && c.blocks == s.slots_512);
        CHECK(s.workspace_pages == c.workspace_pages && s.data_pages + s.meta_pages + s.workspace_pages == s.pages);
        cache_destroy(&c);
    }
    /* Default budget: 8 MiB = 2048 pages, 128 workspace, 1920 for data and
     * descriptors. F1 had 7 slots in each of those 1920 pages (13440). The
     * i686 layout packs 11 page headers per descriptor page (static assert in
     * cache.c): 14080 slots. The host's 64-bit pointers pack 7: 13440, equal. */
    struct block_cache c; struct cache_stats s; CHECK(!cache_init(&c, 0)); cache_stats(&c, &s);
    CHECK(s.workspace_pages == 128 && s.pages == 2048 && s.slots_512 >= 13440);
    cache_destroy(&c);
    CHECK(8 * model_data(1920, 11) == 14080 && 8 * model_data(1920, 11) >= 7 * 1920);
    CHECK(8 * model_data(1920, 7) == 13440);
    printf("PASS budget: pages <= bytes/4096, 8 slots per data page, default 8 MiB keeps F1's 13440 FAT slots (i686 model 14080)\n");
}

static void attach_on_demand(void) {
    struct block_cache c; struct cache_stats before, after;
    struct mem fat, cd; mem_init(&fat, 1, 512, 4096); mem_init(&cd, 2, 2048, 512);
    CHECK(!cache_init(&c, 1024 * 1024));
    for (uint64_t l = 0; l < 64; l++) CHECK(good(&fat, &c, l));
    cache_stats(&c, &before);
    CHECK(before.slots_2048 == 0);            /* nothing allocated for an absent optical device */
    CHECK(!cache_attach(&c, &cd.dev)); cache_stats(&c, &after);
    unsigned pages = before.data_pages / 8;
    CHECK(after.slots_2048 == pages * 2 * 1 && after.slots_512 == before.slots_512 - pages * 8);
    CHECK(after.pages == before.pages && after.data_pages == before.data_pages && after.meta_pages == before.meta_pages);
    CHECK(c.blocks == after.slots_512 + after.slots_2048);
    CHECK(!cache_attach(&c, &cd.dev)); cache_stats(&c, &before);
    CHECK(before.slots_2048 == after.slots_2048);   /* idempotent */
    for (uint64_t l = 0; l < 64; l++) CHECK(good(&fat, &c, l) && good(&fat, &c, l));
    uint64_t reads = cd.reads;
    for (uint64_t l = 0; l < 8; l++) CHECK(good(&cd, &c, l));
    CHECK(cd.reads == reads + 8);
    for (uint64_t l = 0; l < 8; l++) CHECK(good(&cd, &c, l));
    CHECK(cd.reads == reads + 8);            /* second pass: cache hits */
    /* A new media generation never serves old blocks. */
    cd.dev.media_generation++; for (unsigned i = 0; i < 2048; i++) cd.bytes[i] ^= 0xa5;
    uint8_t buf[2048]; CHECK(!cache_read(&c, &cd.dev, 0, buf) && buf[0] == (uint8_t)(pattern(&cd, 0, 0) ^ 0xa5));
    /* No 2048-byte device reaches the 512-byte device's blocks, and vice versa. */
    CHECK(c.hits > 0 && cd.writes == 0 && cd.flushes == 0);
    cache_destroy(&c); mem_free(&fat); mem_free(&cd);
    printf("PASS attach on demand: optical slots appear only on attach, within the same page budget, generations and hits correct\n");
}

static void mixed_pressure(void) {
    struct block_cache c; struct mem fat, cd, fat2;
    mem_init(&fat, 3, 512, 2000); mem_init(&fat2, 4, 512, 2000); mem_init(&cd, 5, 2048, 700);
    CHECK(!cache_init(&c, 64 * 1024));            /* 16 pages: 1 workspace, 14 data + 1 descriptor */
    CHECK(!cache_attach(&c, &cd.dev));
    uint32_t seed = 12345;
    for (unsigned i = 0; i < 20000; i++) {
        seed = seed * 1664525u + 1013904223u;
        struct mem *m = (seed >> 24) % 3 == 0 ? &cd : (seed >> 24) % 3 == 1 ? &fat : &fat2;
        CHECK(good(m, &c, (seed >> 8) % m->dev.capacity));
    }
    struct cache_stats s; cache_stats(&c, &s);
    CHECK(s.slots_2048 >= 2 && s.slots_512 >= 8 && s.misses > 0 && s.hits > 0);
    cache_destroy(&c); mem_free(&fat); mem_free(&fat2); mem_free(&cd);
    printf("PASS mixed devices: 20000 randomized 512/2048-byte reads, every byte verified under eviction\n");
}

static void dirty_writeback_on_conversion(void) {
    struct block_cache c; struct mem fat, cd; mem_init(&fat, 6, 512, 512); mem_init(&cd, 7, 2048, 64);
    CHECK(!cache_init(&c, 8192));                 /* minimum: one data page */
    uint8_t sector[512]; memset(sector, 0x5a, sizeof(sector));
    for (uint64_t l = 0; l < 3; l++) { sector[0] = (uint8_t)l; CHECK(!cache_write(&c, &fat.dev, 100 + l, sector)); }
    CHECK(!fat.writes);
    CHECK(!cache_attach(&c, &cd.dev));           /* converts both halves; healthy dirty data is written first */
    CHECK(fat.writes == 3);
    for (uint64_t l = 0; l < 3; l++) CHECK(fat.bytes[(100 + l) * 512] == (uint8_t)l && fat.bytes[(100 + l) * 512 + 1] == 0x5a);
    struct cache_stats s; cache_stats(&c, &s);
    CHECK(s.slots_512 == 0 && s.slots_2048 == 2);
    CHECK(good(&cd, &c, 1) && good(&cd, &c, 2) && good(&cd, &c, 1) && good(&fat, &c, 7));  /* 512 uses 2048 slots */
    cache_destroy(&c); mem_free(&fat); mem_free(&cd);
    printf("PASS conversion writes healthy dirty blocks back before dropping them\n");
}

/* A failed dirty FAT block pins one slot only; every other device keeps
 * making progress in a minimum cache (the f4-02 rule), in either attach order. */
static void minimum_cache_rule(void) {
    struct block_cache c; struct mem a, b, cd; struct cache_stats s;
    mem_init(&a, 8, 512, 256); mem_init(&b, 9, 512, 256); mem_init(&cd, 10, 2048, 64);
    uint8_t sector[512] = { 0 };
    /* 1. FAT only: a sticky write error leaves seven of eight slots. */
    CHECK(!cache_init(&c, 8192)); a.fail_write_lba = 200;
    CHECK(!cache_write(&c, &a.dev, 200, sector)); CHECK(cache_barrier(&c, &a.dev) == -FS_EIO);
    for (unsigned i = 0; i < 100; i++) CHECK(good(&b, &c, i % 64));
    CHECK(cache_error(&c, &a.dev) == -FS_EIO && !cache_error(&c, &b.dev));
    cache_destroy(&c);
    /* 2. Optical attached first: the page is two 2048-byte slots; a failed
     * FAT block pins one, FAT peer and optical share the other. */
    mem_free(&a); mem_free(&b); mem_init(&a, 8, 512, 256); mem_init(&b, 9, 512, 256);
    CHECK(!cache_init(&c, 8192)); CHECK(!cache_attach(&c, &cd.dev)); cache_stats(&c, &s);
    CHECK(s.slots_512 == 0 && s.slots_2048 == 2);
    a.fail_write_lba = 201; CHECK(!cache_write(&c, &a.dev, 201, sector));
    CHECK(cache_barrier(&c, &a.dev) == -FS_EIO);
    for (unsigned i = 0; i < 60; i++) { CHECK(good(&b, &c, i % 32)); CHECK(good(&cd, &c, i % 16)); }
    cache_destroy(&c);
    /* 3. FAT block already failed, optical attaches later: the pinned half
     * stays 512-byte, the other half converts, nobody is starved. */
    mem_free(&a); mem_free(&b); mem_init(&a, 8, 512, 256); mem_init(&b, 9, 512, 256);
    CHECK(!cache_init(&c, 8192)); a.fail_write_lba = 202;
    CHECK(!cache_write(&c, &a.dev, 202, sector)); CHECK(cache_barrier(&c, &a.dev) == -FS_EIO);
    CHECK(!cache_attach(&c, &cd.dev)); cache_stats(&c, &s);
    CHECK(s.slots_2048 == 1 && s.slots_512 == 4);
    for (unsigned i = 0; i < 60; i++) { CHECK(good(&b, &c, i % 32)); CHECK(good(&cd, &c, i % 16)); }
    CHECK(cache_error(&c, &a.dev) == -FS_EIO);
    cache_destroy(&c);
    /* 4. Failed dirty blocks in every slot: nothing can be converted, and the
     * attach fails visibly instead of dropping unwritable data. */
    mem_free(&a); mem_free(&b); mem_init(&a, 8, 512, 256); mem_init(&b, 9, 512, 256);
    CHECK(!cache_init(&c, 8192)); a.fail_all = true;
    for (unsigned i = 0; i < 8; i++) CHECK(!cache_write(&c, &a.dev, 20 + i, sector));
    CHECK(cache_barrier(&c, &a.dev) == -FS_EIO);
    CHECK(cache_attach(&c, &cd.dev) == -FS_EIO); cache_stats(&c, &s);
    CHECK(s.slots_512 == 8 && s.slots_2048 == 0);
    uint8_t buf[2048]; CHECK(!cache_read(&c, &cd.dev, 0, buf)); /* raw read needs no available cache slot */
    cache_destroy(&c);
    mem_free(&a); mem_free(&b); mem_free(&cd);
    printf("PASS minimum cache: failed dirty FAT block never monopolizes it, both attach orders\n");
}

static void failed_victim_read(void) {
    struct block_cache c;struct mem bad,good_disk;
    mem_init(&bad,13,512,32);mem_init(&good_disk,14,512,32);
    CHECK(!cache_init(&c,8192));uint8_t sector[512]={0};
    for (unsigned i=0;i<4;i++) CHECK(!cache_write(&c,&bad.dev,i,sector));
    for (unsigned i=0;i<4;i++) CHECK(good(&good_disk,&c,i));
    bad.fail_all=true;
    /* The oldest victim fails its writeback; this same read still succeeds. */
    CHECK(good(&good_disk,&c,10));CHECK(cache_error(&c,&bad.dev)==-FS_EIO);
    for (unsigned i=11;i<20;i++) CHECK(good(&good_disk,&c,i));
    cache_destroy(&c);bad.fail_all=false;
    CHECK(!cache_init(&c,8192));
    for (unsigned i=0;i<8;i++) CHECK(!cache_write(&c,&bad.dev,i,sector));
    bad.fail_all=true;CHECK(cache_barrier(&c,&bad.dev)==-FS_EIO);
    CHECK(good(&good_disk,&c,21));
    uint8_t bytes[1024];CHECK(!cache_read_run(&c,&good_disk.dev,22,2,bytes));
    CHECK(bytes[0]==pattern(&good_disk,22,0) && bytes[512]==pattern(&good_disk,23,0));
    cache_destroy(&c);mem_free(&bad);mem_free(&good_disk);
    puts("PASS read isolation: first failed dirty eviction pins its device, current and later reads use healthy slots");
}
static void explicit_errors(void) {
    struct block_cache c; struct mem cd; mem_init(&cd, 11, 2048, 8);
    CHECK(!cache_init(&c, 8192));
    struct blkdev odd = cd.dev; odd.sector_size = 4096;
    CHECK(cache_attach(&c, &odd) == -FS_EINVAL && cache_attach(&c, 0) == -FS_EINVAL);
    CHECK(cache_init(&c, 4096) == -FS_EINVAL);
    mem_free(&cd);
    printf("PASS invalid attach and cache sizes are explicit errors\n");
}

int main(void) {
    budget(); attach_on_demand(); mixed_pressure(); dirty_writeback_on_conversion(); minimum_cache_rule(); failed_victim_read(); explicit_errors();
    printf("RESULT cache layout checks=%u failures=%u ASan/UBSan=enabled\n", checks, failures);
    return failures ? 1 : 0;
}
