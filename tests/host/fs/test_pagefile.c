/* f0-49: production pagefile adapter (fs/pagefile.c) on production FAT, VFS,
 * cache and f1-83 request pool over the file-backed fake device. Covers the
 * slot map oracle (mtools), in-place growth/shrink without relocation,
 * exclusivity and volume dependency, reserved records, page I/O failures,
 * and power cuts during page-out, growth and shrink (scan + fsck.fat).
 * SPDX-License-Identifier: GPL-2.0-only */
#include "pagefile.h"
#include "fat.h"
#include "fake.h"
#include "scan.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL pagefile:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#define MIB (1024u * 1024u)
static unsigned checks;
static struct block_cache cache;
static struct fat_volume vol;
static struct vfs space;
static struct pagefile pf;
static struct fake f;
static const char *image;
static uint8_t page[PAGER_BATCH * PAGER_PAGE];

static uint64_t now_us(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u; }
static void mount(void)
{
    memset(&cache, 0, sizeof(cache)); memset(&vol, 0, sizeof(vol));
    CHECK(!cache_init(&cache, 2 * MIB));
    CHECK(!fat_mount(&vol, &cache, &f.dev, 0, f.dev.capacity, true) && !vol.readonly);
    vfs_init(&space); CHECK(!vfs_attach(&space, 2, &vol));
}
static void unmount(void) { CHECK(!vfs_detach(&space, 2)); vfs_destroy(&space); cache_destroy(&cache); }
static void clean_scan(bool allow_lost)
{
    struct scan_result r = independent_scan(f.fd, 0), second = independent_scan(f.fd, 1);
    CHECK(!r.crosslinks && !r.corrupt && !second.crosslinks && !second.corrupt);
    if (!allow_lost) CHECK(!r.lost && !r.divergent);
}
static int fsck_status(void)
{
    fflush(stdout); pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        char path[64]; snprintf(path, sizeof(path), "/proc/self/fd/%d", f.fd);
        int null = open("/dev/null", 1); if (null >= 0) { dup2(null, 1); dup2(null, 2); }
        execlp("fsck.fat", "fsck.fat", "-n", path, (char *)0); _exit(127);
    }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status));
    return WEXITSTATUS(status);
}
static int page_io(bool write, const struct pager_map *m, uint32_t slot, void *buf, enum pager_class cls)
{
    uint64_t lba = 0; bool found = false;
    for (uint32_t r = 0; r < m->runs && !found; r++)
        if (slot >= m->run[r].first && slot - m->run[r].first < m->run[r].count) {
            lba = m->run[r].lba + (uint64_t)(slot - m->run[r].first) * 8; found = true;
        }
    CHECK(found);
    struct pager_io io = { .buffer = buf, .lba = lba, .bytes = PAGER_PAGE, .cls = cls, .write = write };
    int e = pagefile_backend.submit(&pf, &io);
    if (!e) e = pagefile_backend.wait(&pf, &io);
    return e;
}
static uint32_t mapped(const struct pager_map *m)
{ uint32_t n = 0; for (uint32_t r = 0; r < m->runs; r++) n += m->run[r].count; return n; }
static void fill(uint32_t slot, uint32_t salt) { for (uint32_t i = 0; i < PAGER_PAGE / 4; i++) ((uint32_t *)page)[i] = slot * 2654435761u ^ salt ^ i; }
static void open_pagefile(uint32_t *existing)
{
    CHECK(!pagefile_init(&pf, &space, 2, "C:/PAGEFILE.SWP", &f.dev));
    CHECK(!pagefile_backend.open(&pf, existing));
}
/* Independent oracle: mtools reads the file back from the image. */
static void oracle(const uint32_t *slots, unsigned count, const struct pager_map *m0, uint32_t salt)
{
    (void)m0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "MTOOLSRC=build/host/fs/mtoolsrc mcopy -n -i %s ::PAGEFILE.SWP build/host/fs/pagefile-oracle.bin", image);
    fflush(stdout); CHECK(!system(cmd));
    FILE *in = fopen("build/host/fs/pagefile-oracle.bin", "rb"); CHECK(in);
    for (unsigned i = 0; i < count; i++) {
        uint8_t got[PAGER_PAGE];
        CHECK(!fseek(in, (long)slots[i] * PAGER_PAGE, SEEK_SET) && fread(got, 1, PAGER_PAGE, in) == PAGER_PAGE);
        fill(slots[i], salt); CHECK(!memcmp(got, page, PAGER_PAGE));
    }
    fclose(in); unlink("build/host/fs/pagefile-oracle.bin");
}

/* ---- 1: activation, slot map, growth, exclusivity, reservations ---- */
static uint32_t max_extend_us, max_trim_us, max_map_us;
static void lifecycle(void)
{
    CHECK(!fake_open(&f, image)); mount();
    uint32_t existing = 9; open_pagefile(&existing);
    CHECK(!existing && pf.cluster_bytes == 4096 && pf.open);
    uint64_t free0, capacity; CHECK(!pagefile_backend.space(&pf, &free0, &capacity));
    uint64_t reserve = capacity / 100 * PAGER_RESERVE_PERCENT > PAGER_RESERVE_FLOOR ? capacity / 100 * PAGER_RESERVE_PERCENT : PAGER_RESERVE_FLOOR;
    CHECK(free0 >= reserve + PAGER_EXTENT_BYTES);
    /* Bounded steps, measured: each call is one volume reservation. */
    uint32_t size = 0;
    while (size < PAGER_EXTENT_BYTES) {
        uint64_t t = now_us(); CHECK(!vfs_pagefile_extend(&pf.table, pf.handle, size + PAGER_GROW_STEP));
        uint32_t us = (uint32_t)(now_us() - t); if (us > max_extend_us) max_extend_us = us;
        size += PAGER_GROW_STEP;
    }
    struct pager_map m0; CHECK(!pagefile_backend.grow(&pf, 0, &m0)); /* already allocated: maps only */
    CHECK(m0.runs >= 1 && mapped(&m0) == PAGER_EXTENT_SLOTS);
    uint64_t free1; CHECK(!pagefile_backend.space(&pf, &free1, &capacity) && free1 == free0 - PAGER_EXTENT_BYTES);
    /* Exclusive: no reader, no delete, no detach while the store is open. */
    struct vfs_table other; CHECK(!vfs_table_init(&space, &other, 2));
    int denied = vfs_open(&other, "C:/PAGEFILE.SWP", VFS_READ, VFS_DENY_NONE, 0);
    CHECK(denied == -FS_EACCES || denied == -VFS_ESHARE);
    CHECK(vfs_delete(&other, "C:/PAGEFILE.SWP") < 0);
    CHECK(vfs_detach(&space, 2) == -FS_EBUSY);
    struct fat_entry entry; CHECK(!vfs_stat(&other, "C:/PAGEFILE.SWP", &entry));
    CHECK(entry.size == PAGER_EXTENT_BYTES && (entry.attr & (FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM)) == (FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM));
    /* Reserved records survive an exhausted pool. */
    struct blk_request *ordinary[BLKREQ_CAPACITY]; unsigned n = 0;
    while (n < BLKREQ_CAPACITY && (ordinary[n] = blk_request_prepare(&f.dev, BLK_READ, 0, 512, page, BLK_DEMAND, 0, 0, 1000))) n++;
    CHECK(n == BLKREQ_CAPACITY - PAGER_IN_REQUESTS - PAGER_OUT_REQUESTS);
    struct blk_request *in[PAGER_IN_REQUESTS + 1];
    for (unsigned i = 0; i < PAGER_IN_REQUESTS; i++) CHECK((in[i] = blk_request_prepare_reserved(&pf.in, &f.dev, BLK_READ, 0, 512, page, BLK_DEMAND, 0, 0, 1000)));
    CHECK(!blk_request_prepare_reserved(&pf.in, &f.dev, BLK_READ, 0, 512, page, BLK_DEMAND, 0, 0, 1000));
    struct blk_request *out = blk_request_prepare_reserved(&pf.out, &f.dev, BLK_WRITE, 0, 512, page, BLK_BACKGROUND, 0, 0, 1000);
    CHECK(out && blk_request_unreserve(&pf.out) == -FS_EBUSY);
    for (unsigned i = 0; i < PAGER_IN_REQUESTS; i++) { CHECK(blk_request_cancel(in[i])); CHECK(!blk_request_release(in[i])); }
    CHECK(blk_request_cancel(out) && !blk_request_release(out));
    for (unsigned i = 0; i < n; i++) { CHECK(blk_request_cancel(ordinary[i])); CHECK(!blk_request_release(ordinary[i])); }
    printf("TRACE M4-FS reservations ordinary=%u reserved_in=%u reserved_out=%u pool=%u exhausted_pool_page_io=ok\n",
           n, PAGER_IN_REQUESTS, PAGER_OUT_REQUESTS, BLKREQ_CAPACITY);
    /* Page I/O through reserved records only; fake asserts no namespace or
     * cache lock is held across the transport. */
    uint32_t probe[] = { 0, 1, 7, 8, 1023, 2048, PAGER_EXTENT_SLOTS - 1 };
    unsigned writes = f.writes;
    for (unsigned i = 0; i < 7; i++) { fill(probe[i], 0xa5); CHECK(!page_io(true, &m0, probe[i], page, PAGER_MANDATORY)); }
    CHECK(f.writes == writes + 7 * 8 && !f.flushes == !f.flushes); /* data only, no flush */
    uint8_t back[PAGER_PAGE];
    for (unsigned i = 0; i < 7; i++) { CHECK(!page_io(false, &m0, probe[i], back, PAGER_DEMAND)); fill(probe[i], 0xa5); CHECK(!memcmp(back, page, PAGER_PAGE)); }
    /* Growth keeps extent 0 in place; mapping is resumable and bounded. */
    struct pager_map m1, m2, again;
    CHECK(!pagefile_backend.grow(&pf, 1, &m1) && mapped(&m1) == PAGER_EXTENT_SLOTS);
    CHECK(!pagefile_backend.grow(&pf, 2, &m2) && mapped(&m2) == PAGER_EXTENT_SLOTS);
    memset(&pf.cursor, 0, sizeof(pf.cursor));
    CHECK(!pagefile_backend.grow(&pf, 0, &again) && !memcmp(&again, &m0, sizeof(m0)));
    {
        struct vfs_pagefile_run runs[16]; unsigned used = 0; uint32_t got = 0;
        struct vfs_pagefile_cursor c = { 0 };
        uint64_t t = now_us();
        CHECK(!vfs_pagefile_map(&pf.table, pf.handle, &c, 2 * (PAGER_EXTENT_BYTES / 4096), PAGER_EXTENT_BYTES / 4096, runs, 16, &used, &got, 0));
        max_map_us = (uint32_t)(now_us() - t);
        CHECK(got <= PAGEFILE_MAP_WORK); /* skip + map bounded per call */
    }
    for (unsigned i = 0; i < 7; i++) { CHECK(!page_io(false, &m0, probe[i], back, PAGER_DEMAND)); fill(probe[i], 0xa5); CHECK(!memcmp(back, page, PAGER_PAGE)); }
    /* Read/write failures surface as contained errors. */
    uint64_t lba = m0.run[0].lba;
    f.fail_read = lba; CHECK(page_io(false, &m0, 0, back, PAGER_DEMAND) == -PAGER_EIO); f.fail_read = UINT64_MAX;
    f.fail_write = lba + 3; fill(0, 1); CHECK(page_io(true, &m0, 0, page, PAGER_MANDATORY) == -PAGER_EIO); f.fail_write = UINT64_MAX;
    fill(0, 0xa5); CHECK(!page_io(true, &m0, 0, page, PAGER_MANDATORY));
    f.dev.quarantined = true; CHECK(page_io(false, &m0, 0, back, PAGER_DEMAND) == -PAGER_EIO); f.dev.quarantined = false;
    puts("TRACE M4-FS page_io read_failure=EIO write_failure=EIO quarantined=EIO no_flush=1 transport_unlocked=1");
    /* In-place trailing shrink, one extent per bounded call. */
    for (uint32_t size = 3 * PAGER_EXTENT_BYTES; size > PAGER_EXTENT_BYTES; size -= PAGER_SHRINK_STEP) {
        uint64_t t = now_us(); CHECK(!vfs_pagefile_trim(&pf.table, pf.handle, size - PAGER_SHRINK_STEP));
        uint32_t us = (uint32_t)(now_us() - t); if (us > max_trim_us) max_trim_us = us;
    }
    CHECK(!pagefile_backend.shrink(&pf, 1)); /* already there: no-op */
    uint64_t free2; CHECK(!pagefile_backend.space(&pf, &free2, &capacity) && free2 == free1);
    memset(&pf.cursor, 0, sizeof(pf.cursor));
    CHECK(!pagefile_backend.grow(&pf, 0, &again) && !memcmp(&again, &m0, sizeof(m0)));
    /* Shutdown dependency: detach closes the handle; the volume can then
     * detach; live slots stay readable through their runs; reattach maps
     * the same runs. */
    CHECK(!pagefile_detach(&pf) && !pf.open);
    vfs_table_destroy(&other);
    unmount();
    CHECK(!page_io(false, &m0, 7, back, PAGER_DEMAND)); fill(7, 0xa5); CHECK(!memcmp(back, page, PAGER_PAGE));
    CHECK(page_io(true, &m0, 7, page, PAGER_MANDATORY) == -PAGER_EIO); /* no page-out while detached */
    clean_scan(false); CHECK(!fsck_status());
    oracle(probe, 7, &m0, 0xa5);
    mount(); pf.vfs = &space; pf.volume = &vol;
    CHECK(!pagefile_reattach(&pf) && pf.open && !pf.detached);
    CHECK(!pagefile_backend.grow(&pf, 0, &again) && !memcmp(&again, &m0, sizeof(m0)));
    printf("TRACE M4-FS exclusive open=%d delete=refused detach=EBUSY\n", denied);
    printf("TRACE M4-FS slot_map runs=%u slots=%u oracle=mtools(7 pages) grow=in-place extent0_unchanged shrink=in-place free_restored detach/reattach=same runs\n",
           m0.runs, mapped(&m0));
    printf("TRACE M4-FS R1 volume_reservation max_extend_step_us=%u (%u KiB) max_trim_step_us=%u (%u KiB) max_map_call_us=%u (host wall time)\n",
           max_extend_us, PAGER_GROW_STEP / 1024, max_trim_us, PAGER_SHRINK_STEP / 1024, max_map_us);
    pagefile_close(&pf);
    unmount(); clean_scan(false); CHECK(!fsck_status());
    fake_close(&f);
}

/* ---- 2: power cuts during page-out, growth and shrink ---- */
static struct pager_map base_map;
static void cut_reopen(uint32_t allowed_low, uint32_t allowed_high, unsigned *lost)
{
    fake_power_loss(&f);
    struct scan_result r = independent_scan(f.fd, 0);
    clean_scan(true);
    if (r.lost) (*lost)++;
    int status = fsck_status(); CHECK(status == 0 || status == 1);
    mount(); clean_scan(false); /* mount repairs owned excess/lost clusters */
    uint32_t existing; open_pagefile(&existing);
    CHECK(existing >= allowed_low && existing <= allowed_high);
    struct pager_map m; CHECK(!pagefile_backend.grow(&pf, 0, &m) && !memcmp(&m, &base_map, sizeof(m)));
    pagefile_close(&pf); unmount();
}
static void abandon(void)
{
    /* Power is gone: drop volatile state without flushing anything. */
    if (pf.in.live) CHECK(!blk_request_unreserve(&pf.in));
    if (pf.out.live) CHECK(!blk_request_unreserve(&pf.out));
    memset(&pf, 0, sizeof(pf)); memset(&space, 0, sizeof(space));
    cache_destroy(&cache);
}
static void power_cuts(void)
{
    CHECK(!fake_open(&f, image)); mount();
    uint32_t existing; open_pagefile(&existing); CHECK(existing == 1);
    CHECK(!pagefile_backend.grow(&pf, 0, &base_map));
    pagefile_close(&pf); unmount();
    unsigned trials = 0, lost = 0;
    /* a) page-out of one 8-page batch: data writes only, never FAT. */
    f.undo_enabled = true; fake_reset(&f);
    mount(); open_pagefile(&existing);
    static uint8_t batch[PAGER_BATCH * PAGER_PAGE];
    for (unsigned k = 0; k < PAGER_BATCH; k++) { fill(k, 9); memcpy(batch + k * PAGER_PAGE, page, PAGER_PAGE); }
    unsigned events = f.events;
    struct pager_io io = { .buffer = batch, .lba = base_map.run[0].lba, .bytes = sizeof(batch), .cls = PAGER_MANDATORY, .write = true };
    CHECK(!pagefile_backend.submit(&pf, &io) && !pagefile_backend.wait(&pf, &io));
    unsigned span = f.events - events;
    CHECK(span == PAGER_BATCH * 8);
    pagefile_close(&pf); unmount(); fake_reset(&f);
    for (unsigned cut = 1; cut <= span; cut++) {
        mount(); open_pagefile(&existing);
        f.cut_at = f.events + cut;
        CHECK(!pagefile_backend.submit(&pf, &io) && pagefile_backend.wait(&pf, &io) == -PAGER_EIO && f.cut);
        abandon();
        struct scan_result r = independent_scan(f.fd, 0); CHECK(!r.lost && !r.divergent);
        cut_reopen(1, 1, &lost); fake_reset(&f); trials++;
    }
    printf("TRACE M4-FS power_cut pageout cuts=%u lost=0 divergent=0 fsck=0/1(dirty flag of the open mount) reopen=existing extent\n", span);
    /* b) one growth step (256 KiB publication group) at every event. */
    mount(); open_pagefile(&existing);
    events = f.events;
    CHECK(!vfs_pagefile_extend(&pf.table, pf.handle, PAGER_EXTENT_BYTES + PAGER_GROW_STEP));
    span = f.events - events;
    pagefile_close(&pf); unmount(); fake_reset(&f);
    unsigned growth_cuts = 0;
    for (unsigned cut = 1; cut <= span; cut++) {
        mount(); open_pagefile(&existing);
        f.cut_at = f.events + cut;
        CHECK(vfs_pagefile_extend(&pf.table, pf.handle, PAGER_EXTENT_BYTES + PAGER_GROW_STEP) < 0 && f.cut);
        abandon();
        cut_reopen(1, 1, &lost); fake_reset(&f); trials++; growth_cuts++;
    }
    printf("TRACE M4-FS power_cut growth_step cuts=%u (all events) fsck=0/1 crosslinks=0 corrupt=0 extent0_runs=unchanged partial_tail=released\n", growth_cuts);
    /* c) trailing shrink of a second extent at every event. */
    f.undo_enabled = false;
    mount(); open_pagefile(&existing);
    struct pager_map m1; CHECK(!pagefile_backend.grow(&pf, 1, &m1));
    pagefile_close(&pf); unmount();
    f.undo_enabled = true; fake_reset(&f);
    mount(); open_pagefile(&existing); CHECK(existing == 2);
    events = f.events;
    CHECK(!pagefile_backend.shrink(&pf, 1));
    span = f.events - events;
    pagefile_close(&pf); unmount(); fake_reset(&f);
    unsigned shrink_cuts = 0;
    for (unsigned cut = 1; cut <= span; cut++) {
        mount(); open_pagefile(&existing);
        f.cut_at = f.events + cut;
        CHECK(pagefile_backend.shrink(&pf, 1) < 0 && f.cut);
        abandon();
        cut_reopen(1, 2, &lost); fake_reset(&f); trials++; shrink_cuts++;
    }
    CHECK(span > 16); /* sixteen bounded trim publications per extent */
    printf("TRACE M4-FS power_cut shrink cuts=%u (all events) fsck=0/1 extent0_runs=unchanged lost_repaired_at_mount=%u total_trials=%u\n",
           shrink_cuts, lost, trials);
    f.undo_enabled = false; fake_reset(&f);
    fake_close(&f);
}

/* ---- 3: small clusters and a full volume ---- */
static void small_and_full(const char *small, const char *full)
{
    image = small;
    CHECK(!fake_open(&f, image)); mount();
    uint32_t existing; open_pagefile(&existing);
    CHECK(pf.cluster_bytes == 512);
    struct pager_map m; CHECK(!pagefile_backend.grow(&pf, 0, &m));
    uint32_t slots = mapped(&m);
    CHECK(slots <= PAGER_EXTENT_SLOTS && slots + m.runs >= PAGER_EXTENT_SLOTS - 1);
    uint32_t probe[] = { m.run[0].first, m.run[m.runs - 1].first + m.run[m.runs - 1].count - 1 };
    for (unsigned i = 0; i < 2; i++) { fill(probe[i], 0x3c); CHECK(!page_io(true, &m, probe[i], page, PAGER_MANDATORY)); }
    pagefile_close(&pf); unmount(); clean_scan(false); CHECK(!fsck_status());
    oracle(probe, 2, &m, 0x3c);
    fake_close(&f);
    printf("TRACE M4-FS small_clusters cluster_bytes=512 runs=%u usable_slots=%u failed_straddling=%u oracle=ok\n",
           m.runs, slots, PAGER_EXTENT_SLOTS - slots);
    /* A volume too small for one extent: growth fails cleanly, the partial
     * tail is released on the next open, the volume stays consistent. */
    image = full;
    CHECK(!fake_open(&f, image)); mount();
    open_pagefile(&existing);
    uint64_t before, capacity; CHECK(!pagefile_backend.space(&pf, &before, &capacity));
    CHECK(pagefile_backend.grow(&pf, 0, &m) == -FS_ENOSPC);
    pagefile_close(&pf);
    open_pagefile(&existing); CHECK(!existing);
    uint64_t after; CHECK(!pagefile_backend.space(&pf, &after, &capacity) && after == before);
    pagefile_close(&pf);
    struct vfs_table t; CHECK(!vfs_table_init(&space, &t, 2) && !vfs_delete(&t, "C:/PAGEFILE.SWP")); vfs_table_destroy(&t);
    unmount(); clean_scan(false); CHECK(!fsck_status());
    fake_close(&f);
    puts("TRACE M4-FS full_volume grow=ENOSPC partial_tail=released_on_open free_space=restored fsck=clean");
}

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    image = argv[1];
    lifecycle();
    power_cuts();
    small_and_full(argv[2], argv[3]);
    CHECK(!blk_request_pool_host_cleanup());
    printf("PASS M4-FS pagefile adapter checks=%u\n", checks);
    return 0;
}
