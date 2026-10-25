/* fx-launch: replay of the program starts of T23 trial 2ca3b0b6 (image of
 * main 60d029c9, 1.0.1 alpha) on the production block cache, with the
 * trial's cache geometry and a PATA timing model taken from earlier verbose
 * trials. It measures what one launch costs in the cache: physical commands,
 * sectors and descriptor visits of the sector index, and converts them into
 * modelled T23 milliseconds.
 *
 * Trial facts used (legacy/local/physical/2ca3b0b6, BOOT.LOG):
 *   [mem] ... owner=cache_data resident=7161 from tick 30848 to the end of the
 *   20-minute session (C4 ceiling min(32 MiB,U/8), U=126816 NORMAL pages);
 *   [desktop] launch ... spawn_ms= for freecell 101, mines 101, lua 74,
 *   netsurf 1277 then 496, music 68, viewer 437, gfxdemo 339, serial 667,
 *   control 721 then 165; [dosvm] image verify bytes=27684864 blocks=423
 *   (background reads during the first five starts); CIUKIST.LOG lists the
 *   CDPLAYER.EXE start with ntdll, kernel32, user32, gdi32, advapi32, winmm.
 * Loaded bytes are the PT_LOAD file sizes of the 60d029c9 build.
 *
 * T23 model (Pentium III-M 1.13 GHz, PC133 SDRAM, PATA UDMA2):
 *   device: 0.10 ms per command + 31 MB/s, from [launch-io] dma_ms of trials
 *     4bcd1d43 (27 commands/1,699,840 B/56 ms, 39/1,671,168 B/59 ms) and
 *     82fadce3 (94/2,052,096 B/77 ms, 21/1,084,416 B/36 ms);
 *   CPU: 0.15 us per index descriptor visited (one L2 miss: 57,288 i686
 *     descriptors of 56 B, 3.2 MB, against a 512 KiB L2), 0.2 us per index
 *     lookup, and each loaded byte copied four times (cache -> run buffer ->
 *     snapshot page -> elf_load buffer -> user page) at 300 MB/s.
 * Before the sized index the model gives the uncontended starts (freecell,
 * mines, lua, NetSurf #1) within 25 % of the trial's spawn_ms; the later
 * starts ran with idle_pm 0-9 and are slower than any cache model.
 *
 * The same source builds against the cache before fx-launch (no index
 * statistics): visits are then the service-call upper bound (scan_service
 * calls fs_service once per 16 descriptors), and the visit bound fails.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "cache.h"
#include <stdio.h>
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static unsigned failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL launch-replay line %u: %s\n", __LINE__, #x); failures++; } } while (0)

static uint32_t clock_ms = 1000;
uint32_t fs_host_clock_ms(void) { return clock_ms; }
static uint64_t service_calls;
void fs_host_service(void) { service_calls++; }

#define DEVICE_COMMAND_US 100.0
#define DEVICE_US_PER_BYTE (1.0 / 31.0)       /* 31 MB/s */
#define VISIT_US 0.15
#define LOOKUP_US 0.2
#define COPY_US_PER_BYTE (4.0 / 300.0)        /* four copies at 300 MB/s */

struct disk { struct blkdev dev; uint64_t commands, sectors; double device_us; };
static int disk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf) {
    struct disk *m = d->ctx;
    int e = blkdev_range(d, lba, n); if (e) return e;
    m->commands++; m->sectors += n; m->device_us += DEVICE_COMMAND_US + n * 512.0 * DEVICE_US_PER_BYTE;
    for (uint32_t i = 0; i < n; i++) memset((uint8_t *)buf + (size_t)i * 512, (int)((lba + i) * 37u + 11u), 512);
    return 0;
}
static int disk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) {
    (void)d; (void)lba; (void)n; (void)buf; return 0;
}
static void disk_init(struct disk *m) {
    memset(m, 0, sizeof(*m));
    m->dev = (struct blkdev){ .read = disk_read, .write = disk_write, .capacity = 1u << 22, .sector_size = 512,
        .write_cache_state = BLKDEV_CACHE_DISABLED, .ctx = m };
}

static struct block_cache cache;
static struct disk c_drive, e_drive;   /* the trial's C: and E: volumes */

static uint64_t visits_now(void) {
#ifdef CACHE_HASH_INLINE
    struct cache_stats s; cache_stats(&cache, &s); return s.lookup_probes;
#else
    return service_calls * 16;   /* pre-fx-launch tree: upper bound */
#endif
}
static uint64_t lookups_now(void) {
#ifdef CACHE_HASH_INLINE
    struct cache_stats s; cache_stats(&cache, &s); return s.lookups;
#else
    return 0;
#endif
}

/* One file read the way read_span() does it: one FAT entry lookup per 4 KiB
 * cluster (a cached FAT sector) and the data in runs of at most 128 sectors. */
static uint8_t run_buffer[BLKDEV_READ_BYTES];
static void read_file(struct disk *m, uint64_t first, uint32_t bytes) {
    uint64_t fat = 64 + first / 1024;
    uint32_t sectors = (bytes + 511) / 512;
    for (uint32_t at = 0; at < sectors;) {
        uint32_t n = sectors - at < BLKDEV_READ_SECTORS ? sectors - at : BLKDEV_READ_SECTORS;
        for (uint32_t c = at / 8; c <= (at + n - 1) / 8; c++) CHECK(!cache_read(&cache, &m->dev, fat + c / 128, run_buffer));
        CHECK(!cache_read_run(&cache, &m->dev, first + at, n, run_buffer));
        at += n;
    }
}

/* fx-23 background reads (the DOS image check): drop-behind in the cache. */
static uint64_t background_now(void *arg) { (void)arg; return 0; }
static const struct vfs_request background_request = { .deadline = UINT64_MAX / 2, .now = background_now,
    .operation = "verify", .site = "vfs", .background = true };
static uint32_t verify_done;
static void verify_blocks(unsigned blocks) {
    struct fs_request_scope scope; fs_request_enter(&scope, &background_request);
    for (unsigned i = 0; i < blocks && verify_done < 423; i++, verify_done++)
        CHECK(!cache_read_run(&cache, &c_drive.dev, 2000000 + (uint64_t)verify_done * 128, 128, run_buffer));
    fs_request_leave(&scope);
}
/* The music player streaming an MP3 from E: between two starts. */
static uint64_t mp3_at;
static void music_stream(uint32_t bytes) {
    for (uint32_t done = 0; done < bytes; done += 4096, mp3_at += 8) read_file(&e_drive, 100000 + mp3_at, 4096);
}

struct launch {
    const char *name; uint64_t lba; uint32_t bytes;
};
/* Image placement: one 8 MiB region of C: per program, contiguous files. */
#define IMAGE(i) (1000000u + (i) * 16384u)
static const struct launch freecell = { "freecell", IMAGE(0), 439786 };
static const struct launch mines = { "mines", IMAGE(1), 477830 };
static const struct launch lua = { "lua", IMAGE(2), 324578 };
static const struct launch netsurf = { "netsurf", IMAGE(3), 4591547 };
static const struct launch music = { "music", IMAGE(4), 565690 };
static const struct launch viewer = { "viewer", IMAGE(5), 571834 };
static const struct launch gfxdemo = { "gfxdemo", IMAGE(6), 525686 };
static const struct launch serial = { "serial", IMAGE(7), 478606 };
static const struct launch control = { "control", IMAGE(8), 1277646 };
/* CDPLAYER.EXE: a whole-file snapshot of the program and each DLL it maps. */
static const struct launch cdplayer[] = {
    { "CDPLAYER.EXE", IMAGE(9), 36864 }, { "ntdll.dll", IMAGE(10), 114688 },
    { "kernel32.dll", IMAGE(11), 172032 }, { "user32.dll", IMAGE(12), 1466368 },
    { "gdi32.dll", IMAGE(13), 319488 }, { "advapi32.dll", IMAGE(14), 176128 },
    { "winmm.dll", IMAGE(15), 974848 },
};

struct result { uint64_t commands, sectors, visits, lookups; double device_us, model_ms; };
static struct result measure(const struct launch *const *images, unsigned count) {
    uint64_t commands = c_drive.commands, sectors = c_drive.sectors, visits = visits_now(), lookups = lookups_now();
    double device = c_drive.device_us; uint64_t bytes = 0;
    for (unsigned i = 0; i < count; i++) {
        /* ELF/PE header run, the loaded ranges, the section table at the end. */
        read_file(&c_drive, images[i]->lba, images[i]->bytes);
        read_file(&c_drive, images[i]->lba + 15000, 1024);
        bytes += images[i]->bytes;
    }
    struct result r = { c_drive.commands - commands, c_drive.sectors - sectors, visits_now() - visits,
        lookups_now() - lookups, c_drive.device_us - device, 0 };
    r.model_ms = (r.device_us + r.visits * VISIT_US + r.lookups * LOOKUP_US + bytes * COPY_US_PER_BYTE) / 1000.0;
    return r;
}
static double worst_visits_per_sector;
/* trial_ms: the trial's desktop spawn_ms for this start (-1: not recorded). */
static struct result launch(const struct launch *l, const char *label, int trial_ms, double bound_ms) {
    const struct launch *one[] = { l };
    struct result r = measure(one, 1);
    double per_sector = (double)r.visits / ((l->bytes + 1024) / 512.0);
    if (per_sector > worst_visits_per_sector) worst_visits_per_sector = per_sector;
    printf("| %-14s | %5d | %5llu | %6llu | %8llu | %6.1f | %7.1f |\n", label, trial_ms,
        (unsigned long long)r.commands, (unsigned long long)r.sectors, (unsigned long long)r.visits, per_sector, r.model_ms);
    /* Sized index: chains of one or two, three to four lookups per cold
     * sector; the device address enters the hash, so runs vary (40 runs:
     * worst 6.5). Before: about 600. */
    CHECK(per_sector <= 10.0);
    CHECK(r.model_ms <= bound_ms);
    return r;
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);
    disk_init(&c_drive); disk_init(&e_drive);
    /* Production sizing: cache_initial_bytes/cache_adaptive_enable with the
     * T23 ledger (zone 2 usable 126816, free 113623, high 7926 pages). */
    uint32_t initial = cache_initial_bytes(126816, 113623, 7926);
    CHECK(initial == CACHE_DEFAULT_BYTES);
    CHECK(!cache_init(&cache, initial) && !cache_adaptive_enable(&cache, 126816));
    struct cache_stats s; cache_stats(&cache, &s);
    CHECK(s.ceiling_pages == CACHE_CEILING_BYTES / 4096);
#ifdef CACHE_HASH_INLINE
    CHECK(s.hash_buckets >= s.ceiling_pages * CACHE_PAGE_SLOTS / 2 && s.hash_pages > 0);
    printf("index: %u buckets in %u pages for a %u-page ceiling\n", s.hash_buckets, s.hash_pages, s.ceiling_pages);
#endif
    /* C4 growth to the ceiling (100 ms windows, one hit and one miss each),
     * as on the trial, where the cache reached its ceiling 30 s after boot. */
    uint8_t sector[512];
    for (unsigned w = 0; w < 400; w++) {
        CHECK(!cache_read(&cache, &c_drive.dev, 3000000 + w, sector) && !cache_read(&cache, &c_drive.dev, 3000000 + w, sector));
        clock_ms += 100; cache_adaptive_tick(&cache, clock_ms, 113623, 7926, false);
    }
    cache_stats(&cache, &s);
    CHECK(s.pages >= s.ceiling_pages - 1);
    printf("cache: %u pages (%u data, %u slots of 512 B); trial: 7161 data pages, 57288 slots\n",
        s.pages, s.data_pages, s.slots_512);
    /* Boot-time contents: desktop, wallpaper, fonts, icons; full as on the trial. */
    read_file(&c_drive, 3100000, 40u * 1024 * 1024);

    printf("| start          | trial | cmds  | sectors | visits   | /sector | T23 ms |\n");
    printf("|----------------|-------|-------|---------|----------|---------|--------|\n");
    verify_blocks(80); launch(&freecell, "freecell", 101, 40);
    verify_blocks(80); launch(&mines, "mines", 101, 40);
    verify_blocks(80); launch(&lua, "lua", 74, 30);
    verify_blocks(80); struct result cold = launch(&netsurf, "netsurf", 1277, 350);
    verify_blocks(80); struct result warm = launch(&netsurf, "netsurf again", 496, 100);
    CHECK(cold.commands > 0 && warm.commands == 0);   /* drop-behind kept NetSurf resident */
    verify_blocks(80); CHECK(verify_done == 423);
    launch(&music, "music", -1, 45);
    music_stream(2u << 20); launch(&music, "music again", 68, 30);
    music_stream(2u << 20); launch(&viewer, "viewer", 437, 45);
    music_stream(2u << 20); launch(&gfxdemo, "gfxdemo", 339, 45);
    music_stream(2u << 20);
    {
        const struct launch *images[ARRAY_SIZE(cdplayer)];
        for (unsigned i = 0; i < ARRAY_SIZE(cdplayer); i++) images[i] = &cdplayer[i];
        struct result r = measure(images, ARRAY_SIZE(cdplayer));
        double per_sector = (double)r.visits / r.sectors;
        printf("| %-14s | %5s | %5llu | %6llu | %8llu | %6.1f | %7.1f |\n", "CDPLAYER+DLLs", "-",
            (unsigned long long)r.commands, (unsigned long long)r.sectors, (unsigned long long)r.visits, per_sector, r.model_ms);
        CHECK(per_sector <= 10.0 && r.model_ms <= 230);
    }
    music_stream(2u << 20); launch(&serial, "serial", 667, 40);
    music_stream(2u << 20); launch(&control, "control", 721, 90);
    launch(&control, "control again", 165, 40);
    cache_stats(&cache, &s);
    printf("worst index visits per sector read: %.1f; preemption points: %llu\n", worst_visits_per_sector,
        (unsigned long long)service_calls);
    cache_destroy(&cache);
    if (failures) { printf("FAIL launch replay: %u checks\n", failures); return 1; }
    puts("PASS launch replay: T23 trial 2ca3b0b6 starts on the production cache, sized sector index");
    return 0;
}
