/* f9-01 host model: the 512-byte boot-image view over a file-backed
 * 2,048-byte optical device, the boot-CD identification, and the Setup-mode
 * read-only storage start with fixture disks (blank, occupied FAT, NTFS),
 * on the production fs/storage sources. Driven by scripts/test/cd_media_tests.py.
 * argv: iso options identity_address identity_bytes blank.img fat.img ntfs.img expected_media_record
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/storage.h>
#include <ciuki/setup_mode.h>
#include <ciuki/kernel.h>
#include "fs/fake.h"
#include "../../apps/setup/setup_info.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ciuki_boot_info g_boot;
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "CD line %d: %s\n", __LINE__, #x); failures++; } } while (0)
static char last_candidate[512], last_detach[128], last_missing[64];
static unsigned detach_records;

void klog(const char *fmt, ...)
{
    char line[512]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    if (!strncmp(line, "[setup] candidate ", 18)) snprintf(last_candidate, sizeof(last_candidate), "%s", line);
    if (!strncmp(line, "[cdview] detached ", 18)) { detach_records++; snprintf(last_detach, sizeof(last_detach), "%s", line); }
    if (!strcmp(line, "[setup] media missing")) snprintf(last_missing, sizeof(last_missing), "%s", line);
}
void rec_emit(const char *probe, const char *event, const char *fmt, ...) { (void)probe; (void)event; (void)fmt; }

/* ---- optical fixture: read-only 2,048-byte blocks with one patched block */
struct optical {
    struct blkdev dev;
    FILE *file;
    uint64_t patch_block;
    uint8_t patch[2048];
    bool patched;
    unsigned reads, blocks;
};
void fs_assert_io_unlocked(void);
static int optical_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buffer)
{
    struct optical *o = d->ctx;
    int e = blkdev_range(d, lba, n);
    if (e) return e;
    fs_assert_io_unlocked();   /* no namespace or cache lock across device I/O */
    o->reads++; o->blocks += n;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *out = (uint8_t *)buffer + (size_t)i * 2048;
        if (o->patched && lba + i == o->patch_block) { memcpy(out, o->patch, 2048); continue; }
        if (fseek(o->file, (long)((lba + i) * 2048), SEEK_SET) || fread(out, 2048, 1, o->file) != 1) return -FS_EIO;
    }
    return 0;
}
static void optical_open(struct optical *o, const char *path)
{
    memset(o, 0, sizeof(*o));
    o->file = fopen(path, "rb");
    CHECK(o->file);
    if (!o->file) exit(2);
    fseek(o->file, 0, SEEK_END);
    long size = ftell(o->file);
    o->dev = (struct blkdev){ .read = optical_read, .capacity = (uint64_t)size / 2048, .sector_size = 2048,
        .media_generation = 1, .ctx = o };
}
static void raw(struct optical *o, uint64_t block, uint8_t out[2048])
{
    CHECK(!fseek(o->file, (long)(block * 2048), SEEK_SET) && fread(out, 2048, 1, o->file) == 1);
}

static const char *options;
static struct cdview_identity identity;
static size_t options_size(void) { return strlen(options) + 1; }

static void test_parsers(void)
{
    static const char good[] = "safe=0 serial=1 setup=1\n";
    CHECK(setup_option_token(good, sizeof(good), "setup=1"));
    CHECK(setup_option_token("setup=1", 7, "setup=1"));
    CHECK(setup_option_token("safe=0\r\nsetup=1\r\n", 17, "setup=1"));
    CHECK(!setup_option_token("setup=10", 8, "setup=1"));
    CHECK(!setup_option_token("xsetup=1", 8, "setup=1"));
    CHECK(!setup_option_token("setup=0 safe=1", 14, "setup=1"));
    CHECK(!setup_option_token("setup=1", 6, "setup=1"));       /* bounded by size */
    CHECK(setup_mode_parse(0, good, sizeof(good)));
    CHECK(!setup_mode_parse(CBI_F_TEST_REQUEST, good, sizeof(good)));
    CHECK(setup_mode_parse(CBI_F_SAFE_MODE | CBI_F_SERIAL_LOG, good, sizeof(good)));
    CHECK(!setup_mode_parse(0, "safe=0 serial=1\n", 17));
    struct ciuki_e820 map[4] = {
        { 0, 0x9f000, CBI_E820_RAM, 1 }, { 0x9f000, 0x61000, 2, 1 },
        { 0x100000, 0x7ee0000, CBI_E820_RAM, 1 }, { 0x7fe0000, 0x20000, 2, 1 } };
    uint64_t kib = setup_usable_kib(map, 4);                     /* a QEMU -m 128 shape */
    CHECK(kib == (0x9f000 >> 10) + (0x7ee0000 >> 10) && kib >= SETUP_MIN_USABLE_KIB);
    map[2].length = 0x3ee0000;                                    /* 64 MiB class */
    CHECK(setup_usable_kib(map, 4) < SETUP_MIN_USABLE_KIB);
    map[2].length = (uint64_t)SETUP_MIN_USABLE_KIB * 1024 - 0x9f000;
    CHECK(setup_usable_kib(map, 4) == SETUP_MIN_USABLE_KIB);     /* boundary: not refused */
    map[2].length -= 1024;
    CHECK(setup_usable_kib(map, 4) == SETUP_MIN_USABLE_KIB - 1);
    struct storage_start_policy normal = storage_start_policy(false, false);
    struct storage_start_policy probe = storage_start_policy(true, false);
    struct storage_start_policy setup = storage_start_policy(false, true);
    CHECK(normal.writer && normal.enable_write && normal.bootlog && normal.pagefile && normal.optical_poller && normal.disk0_system);
    CHECK(!probe.writer && !probe.enable_write && !probe.bootlog && !probe.pagefile && !probe.optical_poller && probe.disk0_system);
    CHECK(!setup.writer && !setup.enable_write && !setup.bootlog && !setup.pagefile && setup.optical_poller && !setup.disk0_system);
}

static struct cdview_boot_record record;

static void test_probe(const char *iso)
{
    struct optical o; optical_open(&o, iso);
    uint8_t block[2048]; const char *why = 0;
    CHECK(!cdview_probe(&o.dev, 0, block, &record, &why) && !strcmp(why, "ok"));
    CHECK(record.signature == CDVIEW_SETUP_SIGNATURE && record.part_start == 2048 && record.media == 4);
    CHECK(record.system_type == 0x0c && record.sector_count == 1 && record.load_segment == 0 && record.platform == 0);
    CHECK(record.image_sectors == record.part_start + record.part_count && record.image_sectors % 4 == 0);
    /* Corrupt one field at a time; each is refused with its reason. */
    struct { uint64_t block; unsigned offset; uint8_t value; const char *reason; } cases[] = {
        { 17, 0, 1, "boot_record" }, { 17, 8, 'X', "boot_record" },
        { record.catalog_block, 0x1c, 0, "validation_entry" }, { record.catalog_block, 0x1f, 0, "validation_entry" },
        { record.catalog_block, 1, 1, "validation_entry" }, { record.catalog_block, 32, 0, "initial_entry" },
        { record.catalog_block, 33, 2, "initial_entry" }, { record.catalog_block, 34, 0xc0, "initial_entry" },
        { record.catalog_block, 36, 0x0b, "initial_entry" }, { record.catalog_block, 38, 4, "initial_entry" },
        { record.load_rba, 440, 0x42, "signature" }, { record.load_rba, 450, 0x07, "image_mbr" },
        { record.load_rba, 454, 0x01, "partition" }, { record.load_rba, 511, 0, "image_mbr" } };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        raw(&o, cases[i].block, o.patch);
        o.patch[cases[i].offset] ^= cases[i].value ? cases[i].value : 0xff;
        o.patch_block = cases[i].block; o.patched = true;
        struct cdview_boot_record r;
        int e = cdview_probe(&o.dev, 0, block, &r, &why);
        CHECK(e == -FS_EINVAL && !strcmp(why, cases[i].reason));
        if (e != -FS_EINVAL || strcmp(why, cases[i].reason)) fprintf(stderr, "case %u reason %s\n", i, why);
    }
    o.patched = false;
    CHECK(cdview_probe(&o.dev, 1, block, &(struct cdview_boot_record){0}, &why) && !strcmp(why, "boot_record"));
    fclose(o.file);
}

static void test_view(const char *iso)
{
    struct optical o; optical_open(&o, iso);
    struct cdview v = { 0 };
    CHECK(cdview_bind(&v, &o.dev, record.load_rba, o.dev.capacity * 4 + 1) == -FS_EINVAL);
    CHECK(!cdview_bind(&v, &o.dev, record.load_rba, record.image_sectors));
    CHECK(v.dev.sector_size == 512 && v.dev.capacity == record.image_sectors && !v.dev.write && !v.dev.flush);
    uint8_t *image = malloc(160 * 512), *got = malloc(160 * 512);
    CHECK(image && got);
    /* The reference bytes: the image at its load RBA in the ISO file. */
    CHECK(!fseek(o.file, (long)record.load_rba * 2048, SEEK_SET) && fread(image, 512, 160, o.file) == 160);
    for (unsigned s = 0; s < 16; s++) {                 /* translation: block = rba + s/4, slice s%4 */
        memset(got, 0xa5, 512);
        CHECK(!v.dev.read(&v.dev, s, 1, got) && !memcmp(got, image + s * 512, 512));
    }
    static const struct { unsigned lba, count; } runs[] = { {3, 2}, {1, 9}, {2, 4}, {4, 4}, {7, 130}, {0, 160}, {5, 155} };
    for (unsigned i = 0; i < sizeof(runs) / sizeof(*runs); i++) {
        memset(got, 0x5a, runs[i].count * 512);
        unsigned before = o.blocks;
        CHECK(!v.dev.read(&v.dev, runs[i].lba, runs[i].count, got) && !memcmp(got, image + runs[i].lba * 512, runs[i].count * 512));
        unsigned first = runs[i].lba / 4, last = (runs[i].lba + runs[i].count - 1) / 4;
        CHECK(o.blocks - before == last - first + 1);   /* whole blocks, bounded runs */
    }
    /* The last sectors of the image and the bounds. */
    uint8_t tail[1024], ref[1024];
    CHECK(!fseek(o.file, (long)(record.load_rba * 2048 + (record.image_sectors - 2) * 512), SEEK_SET) && fread(ref, 512, 2, o.file) == 2);
    CHECK(!v.dev.read(&v.dev, record.image_sectors - 2, 2, tail) && !memcmp(tail, ref, sizeof(ref)));
    CHECK(v.dev.read(&v.dev, record.image_sectors, 1, tail) == -FS_EINVAL);
    CHECK(v.dev.read(&v.dev, record.image_sectors - 1, 2, tail) == -FS_EINVAL);
    CHECK(v.dev.read(&v.dev, 0, 0, tail) == -FS_EINVAL);
    CHECK(v.dev.read(&v.dev, 0, 1, 0) == -FS_EINVAL);
    /* FAT32 through the view, read-only by construction. */
    struct block_cache cache; struct blkpart part; struct fat_volume fat = { 0 };
    CHECK(!cache_init(&cache, 256 * 1024));
    struct partition p = { .start = record.part_start, .count = record.part_count, .type = 0x0c };
    CHECK(!blkpart_init(&part, &v.dev, &p) && !part.block.write);
    CHECK(!fat_mount(&fat, &cache, &part.block, 0, p.count, false) && fat.readonly && fat.type == 32);
    struct fat_entry system, cfg; size_t done = 0; char text[160] = { 0 };
    CHECK(!fat_lookup(&fat, fat.root, "SYSTEM", &system) && !fat_lookup(&fat, system.first, "BOOT.CFG", &cfg));
    CHECK(cfg.size == strlen(options) && !fat_read(&fat, &cfg, 0, text, cfg.size, &done) && done == cfg.size && !strcmp(text, options));
    CHECK(fat_enable_write(&fat) == -FS_EROFS && fat.readonly);
    CHECK(!fat_unmount(&fat));
    cache_invalidate(&cache, &part.block, true);
    cache_destroy(&cache);
    /* A media change quarantines the view for good. */
    o.dev.media_generation = 2;
    CHECK(v.dev.read(&v.dev, 0, 1, got) == -FS_EQUARANTINED && v.detached && v.dev.quarantined);
    CHECK(detach_records == 1 && !strcmp(last_detach, "[cdview] detached generation=2"));
    o.dev.media_generation = 1;
    CHECK(v.dev.read(&v.dev, 0, 1, got) == -FS_EQUARANTINED && detach_records == 1);
    cdview_release(&v);
    CHECK(!v.bound && !v.bounce);
    free(image); free(got);
    fclose(o.file);
}

static struct storage *setup_storage(void)
{
    struct storage *s = storage_get();
    CHECK(!storage_setup(s, 0));
    s->setup_mode = true;
    return s;
}

static void test_identify(const char *iso)
{
    struct optical o; optical_open(&o, iso);
    struct storage *s = storage_get();
    /* Outside Setup mode the call is refused. */
    CHECK(!storage_setup(s, 0));
    CHECK(storage_setup_identify(s, 3, &o.dev, 0, options, options_size(), &identity) == -FS_EINVAL);
    storage_destroy(s);
    /* Wrong disk signature. */
    s = setup_storage();
    raw(&o, record.load_rba, o.patch); o.patch[440] ^= 1; o.patch_block = record.load_rba; o.patched = true;
    CHECK(storage_setup_identify(s, 3, &o.dev, 0, options, options_size(), &identity) == -FS_EINVAL);
    CHECK(strstr(last_candidate, "reason=signature") && !s->volumes[2].present && !s->boot_cd.bound);
    storage_setup_finish(s);
    CHECK(!strcmp(last_missing, "[setup] media missing") && s->next_drive == 4);
    storage_destroy(s);
    o.patched = false;
    /* BOOT.CFG different from the loader's options. */
    s = setup_storage();
    CHECK(storage_setup_identify(s, 3, &o.dev, 0, "safe=0 serial=1\n", 17, &identity) == -FS_EINVAL);
    CHECK(strstr(last_candidate, "reason=boot_cfg_differs") && !s->volumes[2].present && !s->boot_cd.bound);
    storage_destroy(s);
    /* Another kernel build. */
    s = setup_storage();
    char other[64]; memcpy(other, identity.bytes, identity.length); other[identity.length - 2] ^= 1;
    struct cdview_identity wrong = { identity.address, other, identity.length };
    CHECK(storage_setup_identify(s, 3, &o.dev, 0, options, options_size(), &wrong) == -FS_EINVAL);
    CHECK(strstr(last_candidate, "reason=kernel_identity") && !s->boot_cd.bound);
    wrong.address += 0x100000;
    CHECK(storage_setup_identify(s, 3, &o.dev, 0, options, options_size(), &wrong) == -FS_EINVAL);
    CHECK(strstr(last_candidate, "reason=kernel_identity"));
    storage_destroy(s);
    fclose(o.file);
}

static uint64_t fake_writes(struct fake *f) { return f->writes + f->flushes; }

static void test_setup_storage(const char *iso, const char *blank, const char *fat_disk, const char *ntfs_disk, const char *expected)
{
    struct optical o; optical_open(&o, iso);
    struct fake disks[3];
    CHECK(!fake_open(&disks[0], blank) && !fake_open(&disks[1], fat_disk) && !fake_open(&disks[2], ntfs_disk));
    struct storage *s = setup_storage();
    CHECK(storage_setup_identify(s, 3, &o.dev, 0, options, options_size(), &identity) == 2);
    CHECK(strstr(last_candidate, "result=0 reason=ok"));
    CHECK(s->boot_cd.bound && s->volumes[2].present && s->volumes[2].optical && s->volumes[2].disk == 3);
    CHECK(s->vfs.volumes[2] == &s->volumes[2].fat && s->volumes[2].fat.readonly);
    CHECK(s->vfs.ro_volumes[3] == &s->optical[3]);                     /* D: is the ISO tree */
    storage_setup_finish(s);
    CHECK(s->next_drive == 4);
    for (unsigned i = 0; i < 3; i++) storage_add_disk(s, i, &disks[i].dev);
    CHECK(s->disk_errors[0] != 0);                     /* blank disk 0: no table, nothing bound */
    CHECK(s->volumes[2].optical && s->volumes[2].disk == 3); /* disk 0 never becomes C: in Setup mode */
    CHECK(s->volumes[4].present && s->volumes[4].disk == 1 && s->volumes[4].fat.readonly && s->vfs.volumes[4]);
    unsigned ntfs = 0;
    for (unsigned i = 0; i < STORAGE_NTFS; i++) if (s->ntfs[i].present) { ntfs++; CHECK(s->ntfs[i].drive == 5 && s->ntfs[i].disk == 2); }
    CHECK(ntfs == 1);
    for (unsigned d = 2; d < 26; d++) if (s->volumes[d].present)
        CHECK(storage_enable_write(s, d) == -FS_EROFS && s->volumes[d].fat.readonly);
    /* Reads through the VFS: C:, D:, E:; a write attempt on C: and E: fails. */
    struct vfs_table table;
    CHECK(!vfs_table_init(&s->vfs, &table, 2));
    char buffer[200] = { 0 }; size_t done = 0;
    int h = vfs_open(&table, "C:/SYSTEM/BOOT.CFG", VFS_READ, VFS_DENY_NONE, 0);
    CHECK(h >= 0 && !vfs_read(&table, h, buffer, sizeof(buffer) - 1, &done) && !strcmp(buffer, options));
    if (h >= 0) vfs_close(&table, h);
    char release[4096]; done = 0;
    h = vfs_open(&table, "C:/SYSTEM/RELEASE.JSON", VFS_READ, VFS_DENY_NONE, 0);
    CHECK(h >= 0 && !vfs_read(&table, h, release, sizeof(release), &done) && done > 0 && done < sizeof(release));
    if (h >= 0) vfs_close(&table, h);
    struct setup_release r; char line[256];
    CHECK(!setup_release_parse(release, done, &r));
    CHECK(setup_media_record(line, sizeof(line), &r, r.payload_files) > 0 && !strcmp(line, expected));
    CHECK(setup_media_record(line, sizeof(line), &r, r.payload_files + 1) > 0 && strstr(line, "[setup] media bad "));
    memset(buffer, 0, sizeof(buffer)); done = 0;
    h = vfs_open(&table, "D:/SETUPINF.TXT", VFS_READ, VFS_DENY_NONE, 0);
    CHECK(h >= 0 && !vfs_read(&table, h, buffer, 40, &done) && !strncmp(buffer, "; CiukiOS Setup answer file sample", 34));
    if (h >= 0) vfs_close(&table, h);
    h = vfs_open(&table, "E:/OCCUPIED.TXT", VFS_READ, VFS_DENY_NONE, 0);
    CHECK(h >= 0);
    if (h >= 0) vfs_close(&table, h);
    CHECK(vfs_open(&table, "C:/SYSTEM/NEW.TXT", VFS_READ | VFS_WRITE | VFS_CREATE, VFS_DENY_NONE, 0) < 0);
    CHECK(vfs_open(&table, "E:/NEW.TXT", VFS_READ | VFS_WRITE | VFS_CREATE, VFS_DENY_NONE, 0) < 0);
    h = vfs_open(&table, "E:/OCCUPIED.TXT", VFS_READ | VFS_WRITE, VFS_DENY_NONE, 0);
    if (h >= 0) { size_t wrote = 0; CHECK(vfs_write(&table, h, "x", 1, &wrote) != 0 || wrote == 0); vfs_close(&table, h); }
    vfs_table_destroy(&table);
    (void)storage_writeback(s, fs_now_ms() + 60000);   /* nothing may become dirty */
    CHECK(!storage_shutdown(s));
    for (unsigned i = 0; i < 3; i++) CHECK(fake_writes(&disks[i]) == 0);
    for (unsigned d = 2; d < 26; d++) if (s->volumes[d].present) CHECK(s->volumes[d].writes == 0 && s->volumes[d].flushes == 0);
    for (unsigned i = 0; i < STORAGE_NTFS; i++) if (s->ntfs[i].present) CHECK(s->ntfs[i].writes == 0 && s->ntfs[i].flushes == 0);
    printf("[setup] writes disk=0 count=%llu\n[setup] writes disk=1 count=%llu\n[setup] writes disk=2 count=%llu\n",
           (unsigned long long)fake_writes(&disks[0]), (unsigned long long)fake_writes(&disks[1]), (unsigned long long)fake_writes(&disks[2]));
    storage_destroy(s);
    for (unsigned i = 0; i < 3; i++) fake_close(&disks[i]);
    fclose(o.file);
}

int main(int argc, char **argv)
{
    if (argc != 9) { fprintf(stderr, "usage: %s iso options identity_address identity_bytes blank fat ntfs expected\n", argv[0]); return 2; }
    options = argv[2];
    static char expected[256], identity_bytes[128];
    snprintf(identity_bytes, sizeof(identity_bytes), "%s\n", argv[4]);
    identity = (struct cdview_identity){ (uint32_t)strtoul(argv[3], 0, 0), identity_bytes, (uint32_t)strlen(identity_bytes) };
    snprintf(expected, sizeof(expected), "%s\n", argv[8]);
    test_parsers();
    test_probe(argv[1]);
    test_view(argv[1]);
    test_identify(argv[1]);
    test_setup_storage(argv[1], argv[5], argv[6], argv[7], expected);
    printf("cd_media_test: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
