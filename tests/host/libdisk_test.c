/* f9-02 host qualification of sdk/libdisk over the production lease code
 * (src/kernel/fs/disklease.c, mount.c, fat.c, cache.c) on file-backed disks.
 * Oracles: scripts/build_image.py and mkfs.fat 4.2 (tests/host/libdisk_oracle.py),
 * fsck.fat -n and mtools, the f1-22 corruption fixtures (fat_fixtures.py).
 * Usage: libdisk_test <work directory with the oracle files>.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/storage.h>
#include <ciuki/kernel.h>
#include <ciuki/bootlog.h>
#include <ciuki/vfs_hooks.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <ciuki/abi.h>
#pragma clang diagnostic pop
#include <libdisk.h>
#include "internal.h"
#include "fs/fake.h"
#include <stdio.h>
#include <stdarg.h>

int hf_create(const char *, uint64_t);
int hf_read(const char *, uint64_t, void *, size_t);
int hf_write(const char *, uint64_t, const void *, size_t);
long hf_size(const char *);
bool hf_data_beyond(const char *, uint64_t);
int hf_run(const char *, char *, size_t);
int hf_remove(const char *);

static unsigned checks;
/* The native archive dependency is strong; the host uses its file backend. */
const struct disk_backend *disk_native_backend(void) { return 0; }
static const char *work;
static void dump(void)
{
    const struct disk_error_info *e = disk_last_error();
    fprintf(stderr, "last libdisk error: code=%d err=%d disk=%u lba=%llu count=%llu reasons=0x%x what=%s\n",
            e->code, e->err, e->disk, (unsigned long long)e->lba, (unsigned long long)e->count, e->reasons, e->what);
}
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "LIBDISK line %d: %s\n", __LINE__, #x); dump(); exit(1); } } while (0)
#define OK(x) CHECK((x) == 0)

/* Kernel records, kept for assertions. */
static char records[256][200];
static unsigned nrecords;
void klog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(records[nrecords % 256], sizeof(records[0]), fmt, ap);
    va_end(ap);
    nrecords++;
    bootlog_capture("", 0);
}
void rec_emit(const char *probe, const char *event, const char *fmt, ...) { (void)probe; (void)event; (void)fmt; }
struct ciuki_boot_info g_boot;
static bool saw(const char *needle)
{
    for (unsigned i = 0; i < 256 && i < nrecords; i++) if (strstr(records[i], needle)) return true;
    return false;
}
static void forget_records(void) { nrecords = 0; memset(records, 0, sizeof(records)); }

/* Whole-disk model: LBA 0..2047 in one file, the partition area in another
 * so fsck.fat and mtools read the volume file directly. */
struct model {
    struct blkdev dev;
    struct fake head, vol;
    char head_path[512], vol_path[512];
    bool open;
};
static int m_io(struct blkdev *d, uint64_t lba, uint32_t n, void *buf, bool write)
{
    struct model *m = d->ctx;
    int e = blkdev_range(d, lba, n);
    if (e) return e;
    uint8_t *p = buf;
    while (n) {
        bool head = lba < DISK_PART_LBA;
        uint32_t k = head ? (uint32_t)(DISK_PART_LBA - lba < n ? DISK_PART_LBA - lba : n) : n;
        struct fake *f = head ? &m->head : &m->vol;
        uint64_t at = head ? lba : lba - DISK_PART_LBA;
        e = write ? f->dev.write(&f->dev, at, k, p) : f->dev.read(&f->dev, at, k, p);
        if (e) return e;
        lba += k; n -= k; p += (size_t)k * 512;
    }
    return 0;
}
static int m_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf) { return m_io(d, lba, n, buf, false); }
static int m_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf) { return m_io(d, lba, n, (void *)buf, true); }
static int m_flush(struct blkdev *d)
{
    struct model *m = d->ctx;
    int e = m->head.dev.flush(&m->head.dev);
    return e ? e : m->vol.dev.flush(&m->vol.dev);
}
static void model_close(struct model *m)
{
    if (!m->open) return;
    fake_close(&m->head); fake_close(&m->vol);
    hf_remove(m->head_path); hf_remove(m->vol_path);
    m->open = false;
}
static void model_open(struct model *m, const char *name, uint64_t capacity, const char *volume_source)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->head_path, sizeof(m->head_path), "%s/%s-head.img", work, name);
    snprintf(m->vol_path, sizeof(m->vol_path), "%s/%s-vol.img", work, name);
    OK(hf_create(m->head_path, DISK_PART_LBA * 512ull));
    if (volume_source) {
        char command[1200], out[256];
        snprintf(command, sizeof(command), "cp --sparse=always '%s' '%s'", volume_source, m->vol_path);
        OK(hf_run(command, out, sizeof(out)));
    } else OK(hf_create(m->vol_path, (capacity - DISK_PART_LBA) * 512));
    OK(fake_open(&m->head, m->head_path)); OK(fake_open(&m->vol, m->vol_path));
    m->dev = (struct blkdev){ .read = m_read, .write = m_write, .flush = m_flush,
        .capacity = DISK_PART_LBA + m->vol.dev.capacity, .sector_size = 512,
        .write_cache_state = BLKDEV_CACHE_ENABLED, .ctx = m };
    m->open = true;
}
/* A previous system: MBR with a FAT partition, an extended container with
 * one logical partition (one EBR), so erasure and fingerprints see a chain. */
static void model_old_table(struct model *m, uint8_t first_type)
{
    uint8_t s[512] = { 0 };
    s[446] = 0x80; s[450] = first_type; disk_wr32(s + 454, 63); disk_wr32(s + 458, 100000);
    s[466] = 0x05; disk_wr32(s + 470, 200000); disk_wr32(s + 474, 300000);
    s[510] = 0x55; s[511] = 0xaa;
    memcpy(s, "OLD BOOT CODE", 13);
    OK(fake_raw_write(&m->head, 0, s));
    uint8_t e[512] = { 0 };
    e[450] = 0x06; disk_wr32(e + 454, 63); disk_wr32(e + 458, 50000);
    e[510] = 0x55; e[511] = 0xaa;
    OK(fake_raw_write(&m->vol, 200000 - DISK_PART_LBA, e));
}

/* libdisk host backend over the production lease functions. */
#define LOG_MAX 16384u
static struct harness {
    struct storage *s;
    struct storage_caller caller;
    struct vfs_table table;
    bool table_ready;
    uint32_t entropy[16]; unsigned entropy_next, entropy_count;
    bool logging;
    struct { uint64_t lba; uint32_t count; size_t offset; } log[LOG_MAX];
    unsigned logs;
    uint8_t *log_data; size_t log_bytes, log_cap;
    uint32_t fail_op; int fail_result; unsigned fail_after, op_calls;
    void (*before_publish)(void);
    int writer_error;
} H;
int __real_storage_writer_start(struct storage *);
int __wrap_storage_writer_start(struct storage *s)
{ return H.writer_error ? H.writer_error : __real_storage_writer_start(s); }
static void path_of(char *out, size_t cap, char letter, const char *rel)
{
    snprintf(out, cap, "%c:/%s", letter, rel);
}
static int b_open(void *ctx, unsigned disk, bool write)
{
    (void)ctx; (void)write;
    int e = storage_disk_open(H.s, disk);
    return e ? e : (int)disk;
}
static void b_close(void *ctx, int handle) { (void)ctx; (void)handle; }
static int b_pread(void *ctx, int h, void *buf, uint32_t bytes, uint64_t offset)
{
    (void)ctx;
    return storage_disk_io(H.s, (unsigned)h, &H.caller, false, offset, buf, bytes);
}
static int b_pwrite(void *ctx, int h, const void *buf, uint32_t bytes, uint64_t offset)
{
    (void)ctx;
    int r = storage_disk_io(H.s, (unsigned)h, &H.caller, true, offset, (void *)buf, bytes);
    if (r == (int)bytes && H.logging) {
        if (H.logs == LOG_MAX) abort();
        if (H.log_bytes + bytes > H.log_cap) {
            H.log_cap = (H.log_bytes + bytes) * 2;
            H.log_data = realloc(H.log_data, H.log_cap);
            if (!H.log_data) abort();
        }
        memcpy(H.log_data + H.log_bytes, buf, bytes);
        H.log[H.logs].lba = offset / 512; H.log[H.logs].count = bytes / 512; H.log[H.logs].offset = H.log_bytes;
        H.logs++; H.log_bytes += bytes;
    }
    return r;
}
static int b_devctl(void *ctx, int h, uint32_t op, void *record, uint32_t size)
{
    (void)ctx;
    if (op == H.fail_op && ++H.op_calls == H.fail_after) return H.fail_result;
    if (op == DISK_PUBLISH && H.before_publish) { void (*hook)(void) = H.before_publish; H.before_publish = 0; hook(); }
    return storage_disk_control(H.s, (unsigned)h, &H.caller, op, record, size);
}
static int b_mkdir(void *ctx, char letter, const char *rel)
{
    (void)ctx;
    char p[256]; path_of(p, sizeof(p), letter, rel);
    return vfs_mkdir(&H.table, p, 0);
}
static int b_write_file(void *ctx, char letter, const char *rel, const void *data, uint32_t bytes)
{
    (void)ctx;
    char p[256]; path_of(p, sizeof(p), letter, rel);
    int h = vfs_open(&H.table, p, VFS_READ | VFS_WRITE | VFS_CREATE | VFS_TRUNCATE, VFS_DENY_NONE, FAT_ATTR_ARCHIVE);
    if (h < 0) return h;
    size_t done = 0;
    int e = bytes ? vfs_write(&H.table, h, data, bytes, &done) : 0;
    if (!e && done != bytes) e = -FS_EIO;
    if (!e) e = vfs_commit(&H.table, h);
    int c = vfs_close(&H.table, h);
    return e ? e : c;
}
static int b_read_file(void *ctx, char letter, const char *rel, void *data, uint32_t capacity)
{
    (void)ctx;
    char p[256]; path_of(p, sizeof(p), letter, rel);
    int h = vfs_open(&H.table, p, VFS_READ, VFS_DENY_NONE, 0);
    if (h < 0) return h;
    size_t done = 0;
    int e = vfs_read(&H.table, h, data, capacity, &done);
    vfs_close(&H.table, h);
    return e ? e : (int)done;
}
static int b_entropy(void *ctx, void *buf, uint32_t bytes)
{
    (void)ctx;
    uint8_t *p = buf;
    for (uint32_t i = 0; i < bytes; i += 4) {
        uint32_t v = H.entropy_next < H.entropy_count ? H.entropy[H.entropy_next++] : 0x5eed0000u + H.entropy_next++;
        memcpy(p + i, &v, bytes - i < 4 ? bytes - i : 4);
    }
    return 0;
}
/* mkfs.fat --invariant: 1426325213 = 2015-03-14 09:26:53 UTC. */
#define INVARIANT_DATE 0x466e
#define INVARIANT_TIME 0x4b5a
#define INVARIANT_ID 0x1234abcdu
static void b_now(void *ctx, uint16_t *date, uint16_t *time) { (void)ctx; *date = INVARIANT_DATE; *time = INVARIANT_TIME; }
static const struct disk_backend backend = {
    0, b_open, b_close, b_pread, b_pwrite, b_devctl, b_mkdir, b_write_file, b_read_file, b_entropy, b_now
};

static void set_entropy(uint32_t signature, uint32_t volume_id)
{
    H.entropy[0] = signature; H.entropy[1] = volume_id; H.entropy_next = 0; H.entropy_count = 2;
}
static struct storage *start(struct model *m, unsigned slot)
{
    struct storage *s = storage_get();
    OK(storage_setup(s, 0));
    if (m) {
        (void)storage_add_disk(s, slot, &m->dev); /* a blank disk 0 has no table: recorded, not mounted */
        CHECK(s->raw[slot].dev == &m->dev);
        snprintf(s->raw[slot].model, sizeof(s->raw[slot].model), "CIUKI_TEST_DISK_%u", slot);
        snprintf(s->raw[slot].serial, sizeof(s->raw[slot].serial), "F902-%04u", slot);
        snprintf(s->raw[slot].firmware, sizeof(s->raw[slot].firmware), "1.0");
    }
    OK(vfs_table_init(&s->vfs, &H.table, 3)); H.table_ready = true;
    s->setup_mode=true;
    H.s = s; H.caller = (struct storage_caller){ .pid=42,.setup_capability=true,.administrator=true,.system=true };
    H.logging = false; H.logs = 0; H.log_bytes = 0;
    H.fail_op = 0; H.fail_after = 0; H.op_calls = 0; H.before_publish = 0;
    H.writer_error = 0;
    forget_records();
    return s;
}
static void stop(void)
{
    (void)disk_release(0); (void)disk_release(1);
    if (H.table_ready) vfs_table_destroy(&H.table);
    H.table_ready = false;
    storage_destroy(H.s);
}
static void read_file(const char *name, void *buf, size_t n)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", work, name);
    OK(hf_read(path, 0, buf, n));
}
static void sha(const void *data, size_t n, uint8_t out[32])
{
    struct disk_sha256 s; disk_sha256_init(&s); disk_sha256_update(&s, data, n); disk_sha256_final(&s, out);
}
static int ctl(unsigned disk, uint32_t op, void *record, uint32_t size)
{
    return storage_disk_control(H.s, disk, &H.caller, op, record, size);
}
static struct ciuki_disk_lease lease_state(unsigned disk)
{
    struct ciuki_disk_lease l = { .version = CIUKI_DISK_VERSION, .size = sizeof(l) };
    OK(ctl(disk, DISK_LEASE_STATE, &l, sizeof(l)));
    return l;
}
static int acquire(unsigned disk)
{
    struct ciuki_disk_info info = { .version = CIUKI_DISK_VERSION, .size = sizeof(info) };
    return ctl(disk, DISK_LEASE_ACQUIRE, &info, sizeof(info));
}
static int release(unsigned disk)
{
    struct ciuki_disk_request q = { CIUKI_DISK_VERSION, sizeof(q), 0, 0 };
    return ctl(disk, DISK_LEASE_RELEASE, &q, sizeof(q));
}

/* 1. Primitive oracles: FIPS 180-4 example vectors, CRC-32 check value. */
static void test_primitives(void)
{
    static const uint8_t abc[32] = { 0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
    static const uint8_t two[32] = { 0x24,0x8d,0x6a,0x61,0xd2,0x06,0x38,0xb8,0xe5,0xc0,0x26,0x93,0x0c,0x3e,0x60,0x39,
        0xa3,0x3c,0xe4,0x59,0x64,0xff,0x21,0x67,0xf6,0xec,0xed,0xd4,0x19,0xdb,0x06,0xc1 };
    uint8_t out[32];
    sha("abc", 3, out); CHECK(!memcmp(out, abc, 32));
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha(m, strlen(m), out); CHECK(!memcmp(out, two, 32));
    struct disk_sha256 s; disk_sha256_init(&s);
    for (size_t i = 0; i < strlen(m); i++) disk_sha256_update(&s, m + i, 1);
    disk_sha256_final(&s, out); CHECK(!memcmp(out, two, 32));
    CHECK(disk_crc32("123456789", 9) == 0xcbf43926u);
    printf("PASS libdisk primitives: SHA-256 FIPS 180-4 vectors, CRC-32 check value\n");
}

/* 2. Geometry and bytes against build_image.py + mkfs.fat for each size. */
static const struct { const char *name; uint64_t capacity; } sizes[] = {
    { "1g", 2097152 }, { "4g", 8388608 }, { "40g", 83886080 }, { "200g", 419430400 } };
static void test_geometry(void)
{
    for (unsigned i = 0; i < 4; i++) {
        char name[64];
        uint8_t oracle[12 * 512], mbr[512], b[512];
        snprintf(name, sizeof(name), "oracle-%s.bin", sizes[i].name); read_file(name, oracle, sizeof(oracle));
        snprintf(name, sizeof(name), "mbr-%s.bin", sizes[i].name); read_file(name, mbr, sizeof(mbr));
        const uint8_t *boot = oracle, *fsinfo = oracle + 512, *fat1 = oracle + 9 * 512, *fat2 = oracle + 10 * 512, *root = oracle + 11 * 512;
        struct disk_geometry g;
        OK(disk_geometry_whole(sizes[i].capacity, &g));
        uint64_t end = sizes[i].capacity < DISK_LBA28 ? sizes[i].capacity : DISK_LBA28;
        CHECK(g.part_sectors == end - DISK_PART_LBA && disk_rd32(boot + 32) == g.part_sectors);
        CHECK(g.fat_sectors == disk_rd32(boot + 36));
        CHECK(g.clusters == (g.part_sectors - DISK_RESERVED - 2 * g.fat_sectors) / 8);
        CHECK(g.clusters >= 65525 && (uint64_t)g.fat_sectors * 128 >= (uint64_t)g.clusters + 2); /* ciukldr disk.inc */
        CHECK(g.fat_spec_sectors >= g.fat_sectors); /* the spec formula over-allocates slightly */
        disk_fmt_boot(b, &g, INVARIANT_ID); CHECK(!memcmp(b, boot, 512)); CHECK(!memcmp(b, oracle + 6 * 512, 512));
        disk_fmt_boot(b, &g, 0x0badf00du);  /* random volume id: identical outside bytes 67..70 */
        CHECK(!memcmp(b, boot, 67) && !memcmp(b + 71, boot + 71, 512 - 71) && disk_rd32(b + 67) == 0x0badf00du);
        disk_fmt_fsinfo(b, &g); CHECK(!memcmp(b, fsinfo, 512)); CHECK(!memcmp(b, oracle + 7 * 512, 512));
        for (unsigned s = 2; s < 6; s++) { uint8_t z[512] = { 0 }; CHECK(!memcmp(oracle + s * 512, z, 512)); }
        disk_fmt_blackbox(b); CHECK(!memcmp(b, oracle + 8 * 512, 512));
        disk_fmt_fat_first(b, true); CHECK(!memcmp(b, fat1, 512) && !memcmp(b, fat2, 512));
        disk_fmt_fat_first(b, false); CHECK(disk_rd32(b + 4) == (disk_rd32(fat1 + 4) & ~DISK_FAT32_CLEAN));
        disk_fmt_root_first(b, INVARIANT_DATE, INVARIANT_TIME); CHECK(!memcmp(b, root, 512));
        uint8_t table[64];
        disk_fmt_table(table, DISK_PART_LBA, g.part_sectors);
        disk_fmt_mbr(b, disk_canonical_mbr_code, 0x4b554943u, table); CHECK(!memcmp(b, mbr, 512));
        disk_fmt_mbr(b, disk_canonical_mbr_code, 0x12345678u, table);
        CHECK(!memcmp(b, mbr, 440) && !memcmp(b + 444, mbr + 444, 68) && disk_rd32(b + 440) == 0x12345678u);
        /* The same through the planner on a disk model of that capacity. */
        struct model m;
        model_open(&m, "plan", sizes[i].capacity, 0);
        start(&m, 0);
        set_entropy(0xc1a0f902u, INVARIANT_ID);
        struct disk_payload pl = { 0 };
        disk_payload_add_file(&pl, 100000);
        static struct disk_plan plan;
        OK(disk_plan_whole(0, &pl, &plan));
        CHECK(!memcmp(&plan.geo, &g, sizeof(g)) && !memcmp(plan.table, mbr + 446, 64));
        CHECK(plan.disk_signature == 0xc1a0f902u && plan.volume_id == INVARIANT_ID && plan.fits);
        CHECK(plan.unallocated_sectors == sizes[i].capacity - end && plan.id.sectors == sizes[i].capacity);
        CHECK(plan.partition_bytes == (uint64_t)g.part_sectors * 512 && plan.data_bytes == (uint64_t)g.clusters * 4096);
        CHECK(plan.required_bytes == plan.payload.bytes + plan.overhead_bytes + plan.reserve_bytes);
        uint64_t five = plan.partition_bytes / 20;
        CHECK(plan.reserve_bytes == (32ull << 20) + (five > (16ull << 20) ? five : (16ull << 20)));
        CHECK(H.s->raw[0].lease.writes == 0 && m.head.writes == 0 && m.vol.writes == 0); /* the plan writes nothing */
        CHECK(!saw("[disk] lease"));
        stop(); model_close(&m);
        printf("PASS libdisk geometry %s: part=%u fat=%u (spec formula %u) clusters=%u heads=%u; BPB, FSInfo, backups, "
               "blackbox, FATs, root and MBR byte-identical to build_image.py/mkfs.fat\n",
               sizes[i].name, g.part_sectors, g.fat_sectors, g.fat_spec_sectors, g.clusters, g.heads);
    }
    /* mkfs.fat's FAT formula may leave the last cluster without an entry;
     * the planner never produces such a volume (sweep of 2^20 sizes). */
    for (uint64_t cap = DISK_MIN_SECTORS; cap < DISK_MIN_SECTORS + (1u << 20); cap += 1) {
        struct disk_geometry g;
        if (disk_geometry_whole(cap, &g)) { CHECK(0); }
        if ((uint64_t)g.fat_sectors * 128 < (uint64_t)g.clusters + 2) CHECK(0);
    }
    checks += 1u << 20;
    printf("PASS libdisk geometry sweep: 1048576 capacities from 1 GiB satisfy the loader FAT rule\n");
}

/* 3. Refusals: disk != 0, small, GPT, overlay, 2048-byte sectors, no flush. */
static void refuse(const char *label, uint64_t capacity, unsigned slot, uint8_t type, uint32_t sector_size,
                   enum blkdev_cache_state cache, bool flush, enum disk_refusal want)
{
    struct model m;
    model_open(&m, "refuse", capacity, 0);
    if (type) model_old_table(&m, type);
    if (sector_size != 512) { m.dev.sector_size = sector_size; m.dev.capacity /= sector_size / 512; }
    m.dev.write_cache_state = cache;
    if (!flush) m.dev.flush = 0;
    start(&m, slot);
    struct disk_entry e;
    OK(disk_probe(slot, &e));
    CHECK(e.present && !e.installable && e.refusal == want);
    static struct disk_plan plan;
    CHECK(disk_plan_whole(slot, 0, &plan) == DISK_EINTERNAL);
    CHECK(!strcmp(disk_last_error()->what, disk_refusal_text(want)));
    CHECK(m.head.writes == 0 && m.vol.writes == 0);
    stop(); model_close(&m);
    printf("PASS libdisk refusal %s: %s\n", label, disk_refusal_text(want));
}
static void test_refusals(void)
{
    refuse("disk1", 2097152, 1, 0, 512, BLKDEV_CACHE_ENABLED, true, DISK_REFUSE_NOT_DISK0);
    refuse("512MiB", 1048576, 0, 0, 512, BLKDEV_CACHE_ENABLED, true, DISK_REFUSE_TOO_SMALL);
    refuse("gpt", 2097152, 0, 0xee, 512, BLKDEV_CACHE_ENABLED, true, DISK_REFUSE_GPT);
    refuse("overlay", 2097152, 0, 0x55, 512, BLKDEV_CACHE_ENABLED, true, DISK_REFUSE_OVERLAY);
    refuse("2048-byte", 2097152, 0, 0, 2048, BLKDEV_CACHE_ENABLED, true, DISK_REFUSE_SECTOR_SIZE);
    refuse("no-flush", 2097152, 0, 0, 512, BLKDEV_CACHE_ENABLED, false, DISK_REFUSE_DURABILITY);
    refuse("cache-unknown", 2097152, 0, 0, 512, BLKDEV_CACHE_UNKNOWN, true, DISK_REFUSE_DURABILITY);
    /* An empty slot is absent; the 2048-byte node moves no data. */
    struct model m;
    model_open(&m, "refuse", 2097152, 0);
    m.dev.sector_size = 2048; m.dev.capacity /= 4;
    start(&m, 0);
    struct disk_entry e;
    OK(disk_probe(2, &e)); CHECK(!e.present && e.refusal == DISK_REFUSE_ABSENT);
    uint8_t buf[512];
    CHECK(storage_disk_io(H.s, 0, &H.caller, false, 0, buf, 512) == -EINVAL);
    CHECK(storage_disk_open(H.s, 2) == -ENXIO && storage_disk_open(H.s, 4) == -ENXIO);
    stop(); model_close(&m);
    printf("PASS libdisk absent slot ENXIO, 2048-byte sectors move no data\n");
}

/* 4. Enumeration of an occupied disk: table, chain, fingerprint, volumes. */
static void test_enumeration(void)
{
    struct model m;
    model_open(&m, "enum", 2097152, 0);
    model_old_table(&m, 0x0c);
    start(&m, 0);
    static struct disk_list list;
    OK(disk_enumerate(&list));
    struct disk_entry *e = &list.disk[0];
    CHECK(list.count == 4 && e->present && e->installable && e->table_present && !e->table_error && !e->stub_mbr);
    CHECK(e->partitions == 3 && e->ebrs == 1);
    CHECK(e->partition[0].type == 0x0c && e->partition[0].start == 63 && e->partition[0].bootable);
    CHECK(e->partition[1].type == 0x05 && e->partition[1].extended);
    CHECK(e->partition[2].type == 0x06 && e->partition[2].logical && e->partition[2].start == 200000 + 63);
    uint8_t mbr[512], ebr[512], fp[32];
    OK(fake_raw_read(&m.head, 0, mbr)); OK(fake_raw_read(&m.vol, 200000 - DISK_PART_LBA, ebr));
    struct disk_sha256 s; disk_sha256_init(&s); disk_sha256_update(&s, mbr, 512); disk_sha256_update(&s, ebr, 512);
    disk_sha256_final(&s, fp);
    CHECK(!memcmp(fp, e->id.table_fingerprint, 32));
    CHECK(!strcmp(e->id.model, "CIUKI_TEST_DISK_0") && !strcmp(e->id.serial, "F902-0000") && e->id.sectors == 2097152);
    for (unsigned d = 1; d < 4; d++) CHECK(!list.disk[d].present);
    /* A looping EBR chain stops the walk; the disk stays erasable. */
    disk_wr32(ebr + 470, 0); ebr[466] = 0x05; /* link back to the container start (itself) */
    OK(fake_raw_write(&m.vol, 200000 - DISK_PART_LBA, ebr));
    OK(disk_probe(0, e)); CHECK(e->table_error && e->installable && e->ebrs == 1);
    stop(); model_close(&m);
    printf("PASS libdisk enumeration: primary/extended/logical entries, EBR walk, fingerprint = SHA-256(MBR||EBR), loop refusal\n");
}

/* 5. The whole installation on a zeroed 1 GiB disk. */
static struct model *compare_model;
static uint32_t compare_data_sector;
static void compare_fresh_volume(void)
{
    /* Before DISK_PUBLISH: P1 + P2's raw output equals build_image.py's fresh
     * volume (mkfs.fat + initialize_blackbox) except FAT[1]'s clean bit. */
    char oracle[600];
    snprintf(oracle, sizeof(oracle), "%s/volume-1g.img", work);
    static uint8_t a[128 * 512], b[128 * 512];
    for (uint32_t lba = 0; lba < compare_data_sector + 8; lba += 128) {
        uint32_t n = compare_data_sector + 8 - lba < 128 ? compare_data_sector + 8 - lba : 128;
        OK(hf_read(compare_model->vol_path, (uint64_t)lba * 512, a, n * 512));
        OK(hf_read(oracle, (uint64_t)lba * 512, b, n * 512));
        for (uint32_t k = 0; k < n; k++) {
            uint32_t sector = lba + k;
            bool fat_first = sector == DISK_RESERVED || sector == DISK_RESERVED + (compare_data_sector - DISK_RESERVED) / 2;
            if (fat_first) {
                CHECK(disk_rd32(a + k * 512 + 4) == 0x07ffffffu && disk_rd32(b + k * 512 + 4) == 0x0fffffffu);
                a[k * 512 + 7] |= 0x08;
            }
            CHECK(!memcmp(a + k * 512, b + k * 512, 512));
        }
    }
    CHECK(!hf_data_beyond(compare_model->vol_path, ((uint64_t)compare_data_sector + 8) * 512));
    CHECK(!hf_data_beyond(oracle, ((uint64_t)compare_data_sector + 8) * 512));
    printf("PASS libdisk P1+P2 raw volume: %u sectors byte-identical to the build_image.py/mkfs.fat volume "
           "(FAT[1] clean bit clear by contract), nothing written beyond the root cluster\n", compare_data_sector + 8);
}
struct progress_log { unsigned calls, phase; uint64_t last_done, last_total; bool monotonic; unsigned cancel_after; };
static void on_progress(void *ctx, const struct disk_progress *p)
{
    struct progress_log *l = ctx;
    if (l->phase != p->phase) { l->phase = p->phase; l->last_done = 0; }
    if (p->done < l->last_done || p->done > p->total) l->monotonic = false;
    l->last_done = p->done; l->last_total = p->total; l->calls++;
}
static bool on_cancel(void *ctx)
{
    struct progress_log *l = ctx;
    return l->cancel_after && l->calls >= l->cancel_after;
}
static uint8_t loader[1023 * 512];
static uint32_t loader_bytes;
static void check_plan_against_log(const struct disk_plan *plan)
{
    /* Phases write their ranges in plan order (P1, P2, P5); the P5 loader
     * and MBR overwrite P1 sectors, so the log is consumed in time order. */
    uint64_t planned = 0, logged = 0;
    unsigned cursor = 0;
    for (unsigned i = 0; i < H.logs; i++) logged += H.log[i].count;
    for (unsigned r = 0; r < plan->ranges; r++) {
        const struct disk_range *range = &plan->range[r];
        planned += range->count;
        struct disk_sha256 s; disk_sha256_init(&s);
        uint64_t next = range->lba;
        while (next < range->lba + range->count) {
            CHECK(cursor < H.logs && H.log[cursor].lba == next); /* contiguous, in order, once */
            CHECK(H.log[cursor].lba + H.log[cursor].count <= range->lba + range->count);
            disk_sha256_update(&s, H.log_data + H.log[cursor].offset, (size_t)H.log[cursor].count * 512);
            next += H.log[cursor++].count;
        }
        uint8_t out[32]; disk_sha256_final(&s, out);
        CHECK(!memcmp(out, range->sha256, 32));
    }
    CHECK(planned == logged && cursor == H.logs);
    printf("PASS libdisk dry run: %u planned ranges, %llu sectors, equal to the raw writes hash by hash\n",
           plan->ranges, (unsigned long long)planned);
}
static void fsck_expect(const char *path, int status, const char *only)
{
    char command[800], out[4096];
    snprintf(command, sizeof(command), "fsck.fat -n '%s'", path);
    int r = hf_run(command, out, sizeof(out));
    if (r != status || (only && !strstr(out, only))) fprintf(stderr, "%s", out);
    CHECK(r == status);
    if (only) {
        /* Every line is the banner, the summary, "Leaving..." or the expected finding. */
        for (char *line = strtok(out, "\n"); line; line = strtok(0, "\n")) {
            bool known = !strncmp(line, "fsck.fat ", 9) || strstr(line, " files, ") || strstr(line, "Leaving filesystem unchanged") ||
                         strstr(line, only) || strstr(line, "Automatically removing dirty bit");
            if (!known) fprintf(stderr, "unexpected fsck line: %s\n", line);
            CHECK(known);
        }
    }
}
static void mtools_expect(const char *vol, const char *path, const char *text)
{
    char command[900], out[4096];
    snprintf(command, sizeof(command), "MTOOLSRC='%s/../mtoolsrc' MTOOLS_SKIP_CHECK=1 mtype -i '%s' '::%s'", work, vol, path);
    CHECK(hf_run(command, out, sizeof(out)) == 0);
    if (strcmp(out, text)) fprintf(stderr, "mtype %s: [%s]\n", path, out);
    CHECK(!strcmp(out, text));
}
struct install {
    struct model m;
    struct disk_plan plan;
    char letter;
    struct progress_log progress;
};
static const char boot_cfg[] = "safe=0 serial=1\n";
static const char installed_state[] = "phase=installed release=f9-02-host firstboot=pending\n";
static const char fixture_text[] = "Ciuki fixture payload for the consistency check.\n";
static void install(struct install *x, const char *name, bool fixture_file, bool check)
{
    memset(x, 0, sizeof(*x));
    model_open(&x->m, name, 2097152, 0);
    start(&x->m, 0);
    set_entropy(0xc1a0f902u, INVARIANT_ID);
    struct disk_payload pl = { 0 };
    for (unsigned i = 0; i < 100; i++) disk_payload_add_file(&pl, 1000u * i);
    disk_payload_add_directory(&pl);
    OK(disk_plan_whole(0, &pl, &x->plan));
    OK(disk_plan_publish(&x->plan, loader, loader_bytes));
    struct disk_callbacks cb = { on_progress, on_cancel, &x->progress };
    x->progress.monotonic = true;
    H.logging = check;
    OK(disk_execute_p1(&x->plan, &cb));
    CHECK(x->progress.last_done == x->progress.last_total && x->progress.last_total == 2 * DISK_PART_LBA);
    if (check) {
        uint8_t s[512], want[512];
        OK(fake_raw_read(&x->m.head, 0, s));
        disk_fmt_mbr(want, disk_stub_mbr_code, x->plan.disk_signature, x->plan.table);
        CHECK(!memcmp(s, want, 512));
        CHECK(saw("[disk] lease disk=0 pid=42 state=held reason=acquire"));
        struct ciuki_disk_lease l = lease_state(0);
        CHECK(l.state == CIUKI_DISK_LEASE_HELD && l.holder == 42 && !strcmp(l.phase, "p1 partition"));
        compare_model = &x->m; compare_data_sector = x->plan.geo.data_sector;
        H.before_publish = compare_fresh_volume;
    }
    OK(disk_execute_p2(&x->plan, &cb, &x->letter));
    CHECK(x->letter == 'D' && x->progress.monotonic);
    if (check) {
        CHECK(!H.before_publish);
        CHECK(saw("[disk] publish disk=0 pid=42 drive=D reasons=0x5"));
        struct ciuki_disk_lease l = lease_state(0);
        CHECK(l.state == CIUKI_DISK_LEASE_PUBLISHED && l.drive == 3 && (l.reasons & CIUKI_FAT_RO_DIRTY));
        /* P2 state: fsck finds only the deliberately clear clean bit. */
        char copy[600], command[1400], out[256];
        snprintf(copy, sizeof(copy), "%s/p2-copy.img", work);
        snprintf(command, sizeof(command), "cp --sparse=always '%s' '%s'", x->m.vol_path, copy);
        OK(hf_run(command, out, sizeof(out)));
        fsck_expect(copy, 1, "Dirty bit is set");
        mtools_expect(copy, "/SYSTEM/SETUP.STATE", "phase=format\n");
        uint8_t fat[512];
        for (unsigned c = 0; c < 2; c++) {
            uint64_t at = ((uint64_t)DISK_RESERVED + c * x->plan.geo.fat_sectors) * 512;
            OK(hf_read(copy, at, fat, 512)); fat[7] |= 0x08; OK(hf_write(copy, at, fat, 512));
        }
        fsck_expect(copy, 0, 0);
        hf_remove(copy);
        /* Holder-only writes on the published volume; raw writes inside it refused. */
        CHECK(!storage_disk_write_denied(H.s, 3, 42) && storage_disk_write_denied(H.s, 3, 43));
        uint8_t z[512] = { 0 };
        CHECK(storage_disk_io(H.s, 0, &H.caller, true, 4096ull * 512, z, 512) == -EBUSY);
        CHECK(storage_disk_io(H.s, 0, &H.caller, true, 1023ull * 512, z, 512) == 512); /* the gap is outside */
        struct disk_check chk;
        OK(disk_consistency_check('d', &chk));
        CHECK(chk.reasons == 0 && chk.fat_type == 32 && chk.clusters == x->plan.geo.clusters && chk.lost_clusters == 0);
        CHECK(!storage_disk_write_denied(H.s, 3, 42)); /* writable again for the holder */
    }
    /* P3/P4 stand-in: files through the VFS, then raw verification. */
    OK(b_mkdir(0, x->letter, "PROGRAMS"));
    static uint8_t payload[20000];
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 7 + 3);
    OK(b_write_file(0, x->letter, "PROGRAMS/DATA.BIN", payload, sizeof(payload)));
    if (fixture_file) OK(b_write_file(0, x->letter, "Ciuki long fixture.txt", fixture_text, sizeof(fixture_text) - 1));
    OK(disk_flush(0));
    struct fat_entry entry;
    OK(vfs_stat(&H.table, "D:/PROGRAMS/DATA.BIN", &entry));
    static uint8_t raw[5 * 4096];
    uint64_t first = x->plan.geo.part_start + x->plan.geo.data_sector + (uint64_t)(entry.first - 2) * 8;
    OK(disk_read_raw(0, first, 8, raw));                 /* the first cluster, uncached */
    CHECK(!memcmp(raw, payload, 4096));
    struct disk_publish pub = { loader, loader_bytes, boot_cfg, installed_state };
    OK(disk_execute_p5(&x->plan, &pub, &cb));
    CHECK(x->progress.last_done == x->progress.last_total && x->progress.monotonic);
}
static void test_install(void)
{
    static struct install x;
    install(&x, "install", false, true);
    check_plan_against_log(&x.plan);
    /* Final disk: canonical MBR (build_image.py bytes, our signature), the
     * loader with its CRC, the zero gap, both FAT[1] clean, fsck clean. */
    uint8_t s[512], mbr[512];
    read_file("mbr-1g.bin", mbr, sizeof(mbr));
    OK(fake_raw_read(&x.m.head, 0, s));
    CHECK(!memcmp(s, mbr, 440) && disk_rd32(s + 440) == 0xc1a0f902u && !memcmp(s + 444, mbr + 444, 68));
    for (uint32_t i = 0; i < loader_bytes / 512; i++) { OK(fake_raw_read(&x.m.head, 1 + i, s)); CHECK(!memcmp(s, loader + i * 512, 512)); }
    CHECK(disk_crc32(loader + 16, loader_bytes - 16) == disk_rd32(loader + 8));
    uint8_t zero[512] = { 0 };
    for (uint32_t lba = 1 + loader_bytes / 512; lba < DISK_PART_LBA; lba++) { OK(fake_raw_read(&x.m.head, lba, s)); CHECK(!memcmp(s, zero, 512)); }
    for (unsigned c = 0; c < 2; c++) {
        OK(fake_raw_read(&x.m.vol, DISK_RESERVED + c * x.plan.geo.fat_sectors, s));
        CHECK(disk_rd32(s + 4) == 0x0fffffffu);
    }
    uint8_t boot[512], backup[512];
    OK(fake_raw_read(&x.m.vol, 0, boot)); OK(fake_raw_read(&x.m.vol, 6, backup));
    CHECK(!memcmp(boot, backup, 512) && disk_rd16(boot + 14) == DISK_RESERVED && boot[13] == 8);
    fsck_expect(x.m.vol_path, 0, 0);
    mtools_expect(x.m.vol_path, "/SYSTEM/BOOT.CFG", boot_cfg);
    mtools_expect(x.m.vol_path, "/SYSTEM/SETUP.STATE", installed_state);
    char command[900], out[4096];
    snprintf(command, sizeof(command), "MTOOLSRC='%s/../mtoolsrc' MTOOLS_SKIP_CHECK=1 mdir -i '%s' ::/", work, x.m.vol_path);
    CHECK(hf_run(command, out, sizeof(out)) == 0 && strstr(out, "Volume in drive : is CIUKIOS"));
    CHECK(saw("[disk] recheck disk=0 drive=D seal=1 reasons=0x0"));
    struct ciuki_disk_lease l = lease_state(0);
    CHECK(l.state == CIUKI_DISK_LEASE_PUBLISHED && !strcmp(l.phase, "p5 publish") && l.flushes >= 6);
    CHECK(storage_disk_writes(H.s, 0) >= l.writes && l.writes > 0);
    /* Sealed: read-only for everyone; release keeps it mounted. */
    CHECK(H.s->volumes[3].fat.readonly);
    OK(disk_release(0));
    CHECK(saw("[disk] lease disk=0 pid=42 state=released reason=release"));
    CHECK(storage_disk_write_denied(H.s, 3, 42));
    stop(); model_close(&x.m);
    printf("PASS libdisk install 1 GiB: P1 stub MBR, P2 format+publish (fsck: only the clean bit), P5 loader, "
           "BOOT.CFG, SETUP.STATE, sealed clean volume (fsck.fat -n 0, mtools), canonical MBR last\n");
}

/* 6. Lease and capability rules. */
static void test_authority(void)
{
    struct model m;
    model_open(&m, "auth", 2097152, 0);
    start(&m, 0);
    uint8_t buf[512] = { 0 };
    struct storage_caller plain = { .pid=42 },
        other = { .pid=43,.setup_capability=true,.administrator=true,.system=true }, plain43 = { .pid=43 };
    struct ciuki_disk_request q = { CIUKI_DISK_VERSION, sizeof(q), 0, 0 };
    struct ciuki_disk_info info = { .version = CIUKI_DISK_VERSION, .size = sizeof(info) };
    struct ciuki_disk_lease l = { .version = CIUKI_DISK_VERSION, .size = sizeof(l) };
    /* No capability: reads and queries only. */
    CHECK(storage_disk_io(H.s, 0, &plain, false, 0, buf, 512) == -EPERM);
    CHECK(storage_disk_io(H.s, 0, &plain, true, 0, buf, 512) == -EPERM);
    CHECK(storage_disk_flush(H.s, 0, &plain) == -EPERM);
    CHECK(storage_disk_control(H.s, 0, &plain, DISK_LEASE_ACQUIRE, &info, sizeof(info)) == -EPERM);
    CHECK(storage_disk_control(H.s, 0, &plain, DISK_FLUSH, &q, sizeof(q)) == -EPERM);
    OK(storage_disk_control(H.s, 0, &plain, DISK_GET_INFO, &info, sizeof(info)));
    CHECK(info.flags == (CIUKI_DISK_PRESENT | CIUKI_DISK_FLUSH_CACHE) && info.sectors == 2097152 && info.write_cache == CIUKI_DISK_CACHE_ENABLED);
    OK(storage_disk_control(H.s, 0, &plain, DISK_LEASE_STATE, &l, sizeof(l)));
    /* Capability without the lease. */
    CHECK(storage_disk_io(H.s, 0, &H.caller, true, 0, buf, 512) == -EACCES);
    CHECK(storage_disk_flush(H.s, 0, &H.caller) == -EACCES);
    CHECK(release(0) == -EACCES);
    /* Record validation. */
    struct ciuki_disk_request bad = { 2, sizeof(bad), 0, 0 };
    CHECK(ctl(0, DISK_LEASE_RELEASE, &bad, sizeof(bad)) == -EOPNOTSUPP);
    bad.version = 1; bad.size = 8; CHECK(ctl(0, DISK_LEASE_RELEASE, &bad, sizeof(bad)) == -EINVAL);
    CHECK(ctl(0, DISK_LEASE_RELEASE, &q, 8) == -EINVAL);
    /* 0x4409 is DISK_PARTITION_ACQUIRE (G29c); the first unused disk control is 0x440a. */
    CHECK(ctl(0, 0x440a, &q, sizeof(q)) == -ENOTTY && ctl(0, 0x4301, &q, sizeof(q)) == -ENOTTY);
    /* Holder 42. */
    OK(acquire(0));
    CHECK(saw("[disk] lease disk=0 pid=42 state=held reason=acquire"));
    CHECK(storage_disk_io(H.s, 0, &H.caller, true, 4096, buf, 512) == 512);
    CHECK(storage_disk_io(H.s, 0, &H.caller, true, 4096, buf, 1000) == -EINVAL); /* whole sectors only */
    CHECK(storage_disk_io(H.s, 0, &H.caller, true, 100, buf, 512) == -EINVAL);
    CHECK(storage_disk_io(H.s, 0, &H.caller, true, 2097152ull * 512, buf, 512) == -EINVAL); /* past the end */
    OK(storage_disk_flush(H.s, 0, &H.caller));
    /* Another capable process: refused; the same descriptor in a process
     * without the capability (inherited or passed) is read-only. */
    CHECK(storage_disk_io(H.s, 0, &other, true, 4096, buf, 512) == -EACCES);
    CHECK(storage_disk_control(H.s, 0, &other, DISK_LEASE_ACQUIRE, &info, sizeof(info)) == -EBUSY);
    CHECK(saw("[disk] lease disk=0 pid=43 state=refused reason=leased"));
    CHECK(storage_disk_control(H.s, 0, &other, DISK_LEASE_RELEASE, &q, sizeof(q)) == -EACCES);
    CHECK(storage_disk_io(H.s, 0, &plain43, true, 4096, buf, 512) == -EPERM);
    CHECK(storage_disk_io(H.s, 0, &plain43, false, 4096, buf, 512) == -EPERM);
    CHECK(storage_disk_io(H.s, 0, &plain, true, 4096, buf, 512) == -EPERM); /* the holder pid without capability */
    /* Death of the holder revokes the lease, with no I/O. */
    unsigned writes = m.head.writes + m.vol.writes;
    storage_disk_exit(H.s, 42);
    CHECK(saw("[disk] lease disk=0 pid=42 state=revoked reason=process_exit"));
    CHECK(lease_state(0).state == CIUKI_DISK_LEASE_REVOKED && m.head.writes + m.vol.writes == writes);
    CHECK(storage_disk_io(H.s, 0, &H.caller, true, 4096, buf, 512) == -EACCES);
    OK(storage_disk_control(H.s, 0, &other, DISK_LEASE_ACQUIRE, &info, sizeof(info)));
    CHECK(lease_state(0).holder == 43 && lease_state(0).generation == 2);
    OK(storage_disk_control(H.s, 0, &other, DISK_LEASE_RELEASE, &q, sizeof(q)));
    CHECK(saw("[disk] lease disk=0 pid=43 state=released reason=release"));
    /* libdisk: a session whose process lost the capability cannot flush. */
    set_entropy(0xc1a0f902u, INVARIANT_ID);
    static struct disk_plan plan;
    OK(disk_plan_whole(0, 0, &plan));
    OK(disk_execute_p1(&plan, 0));
    H.caller.setup_capability = false;
    CHECK(disk_flush(0) == DISK_ELEASE && disk_last_error()->err == EPERM);
    CHECK(disk_read_raw(0, 0, 1, buf) == 0 && !memcmp(buf, disk_stub_mbr_code, 440));
    H.caller.setup_capability = true;
    /* DISK_PUBLISH on an unformatted partition fails and keeps the lease. */
    struct ciuki_disk_lease pub = { .version = CIUKI_DISK_VERSION, .size = sizeof(pub) };
    CHECK(ctl(0, DISK_PUBLISH, &pub, sizeof(pub)) == -EINVAL);
    CHECK(saw("reason=mount") && lease_state(0).state == CIUKI_DISK_LEASE_HELD && !strcmp(lease_state(0).reason, "mount"));
    char letter = 0;
    OK(disk_execute_p2(&plan, 0, &letter));
    CHECK(letter == 'D');
    struct ciuki_disk_recheck rq = { .version = CIUKI_DISK_VERSION, .size = sizeof(rq), .flags = 2 };
    CHECK(ctl(0, DISK_RECHECK, &rq, sizeof(rq)) == -EINVAL);
    stop(); model_close(&m);
    printf("PASS libdisk authority: no capability EPERM (write, flush, lease), no lease EACCES, inherited descriptor read-only, "
           "death revokes, second lease EBUSY, failed publish keeps the lease, record validation\n");
}

/* 7. EBUSY preflight on an occupied disk 0 (C: from build_image.py's volume). */
static void occupied(struct model *m)
{
    char oracle[600];
    snprintf(oracle, sizeof(oracle), "%s/volume-1g.img", work);
    model_open(m, "busy", 0, oracle);
    uint8_t mbr[512];
    read_file("mbr-1g.bin", mbr, sizeof(mbr));
    OK(fake_raw_write(&m->head, 0, mbr));
    start(m, 0);
    CHECK(H.s->volumes[2].present && H.s->volumes[2].fat.mounted && H.s->volumes[2].fat.readonly);
}
static void expect_busy(const char *reason)
{
    char line[160];
    bool mode=H.s->setup_mode;
    H.s->setup_mode=true; /* probe the Setup lease against this storage state */
    int admitted=acquire(0);
    H.s->setup_mode=mode;
    CHECK(admitted == -EBUSY);
    snprintf(line, sizeof(line), "state=refused reason=%s", reason);
    CHECK(saw(line));
    CHECK(!strcmp(lease_state(0).reason, reason));
    CHECK(lease_state(0).state == (strcmp(reason, "leased") ? CIUKI_DISK_LEASE_NONE : CIUKI_DISK_LEASE_HELD));
}
static void test_busy(void)
{
    struct model m;
    /* Read-only and idle: leased; its volume is detached. */
    occupied(&m);
    struct ciuki_disk_info info = { .version = CIUKI_DISK_VERSION, .size = sizeof(info) };
    OK(ctl(0, DISK_GET_INFO, &info, sizeof(info)));
    CHECK(info.volume_count == 1 && info.volumes[0].drive == 2 && info.volumes[0].fs == 32 &&
          (info.volumes[0].flags & CIUKI_DISK_VOLUME_READONLY) && !strcmp(info.volumes[0].label, "CIUKIOS"));
    CHECK(vfs_mkdir(&H.table, "C:/X", 0) == -FS_EROFS); /* discovered read-only */
    OK(acquire(0));
    OK(ctl(0, DISK_GET_INFO, &info, sizeof(info)));
    CHECK(info.volume_count == 0 && (info.flags & CIUKI_DISK_LEASED) && !vfs_drive_present(&H.s->vfs, 2));
    CHECK(m.vol.writes == 0); /* detaching the read-only volume wrote nothing */
    expect_busy("leased");
    stop(); model_close(&m);
    /* Open handle on a read-only volume (seeded, then cleanly shut down). */
    occupied(&m);
    OK(fat_enable_write(&H.s->volumes[2].fat)); OK(vfs_mkdir(&H.table, "C:/SYSTEM", 0));
    OK(b_write_file(0, 'C', "SYSTEM/NOTE.TXT", "note", 4));
    vfs_table_destroy(&H.table); H.table_ready = false;
    OK(storage_shutdown(H.s)); storage_destroy(H.s);
    start(&m, 0);
    CHECK(H.s->volumes[2].fat.readonly && !(H.s->volumes[2].fat.ro_reasons & CIUKI_FAT_RO_DIRTY));
    int h = vfs_open(&H.table, "C:/SYSTEM/NOTE.TXT", VFS_READ, VFS_DENY_NONE, 0);
    CHECK(h >= 0);
    expect_busy("open");
    OK(vfs_close(&H.table, h));
    /* Writable volume (the ordinary boot state of C:). */
    H.s->setup_mode=false;
    OK(storage_enable_write(H.s, 2));
    expect_busy("writable");
    /* Delayed write-back data of that volume with no open handle (vfs_close
     * commits, so one free data sector is dirtied in the cache directly). */
    struct fat_volume *c = &H.s->volumes[2].fat;
    uint8_t zero[512] = { 0 };
    OK(cache_write(&H.s->cache, &H.s->volumes[2].io, c->data_sector + 4000ull * 8, zero));
    CHECK(cache_dirty_bytes(&H.s->cache, &H.s->volumes[2].io) == 512);
    expect_busy("dirty");
    OK(cache_barrier(&H.s->cache, &H.s->volumes[2].io));
    CHECK(cache_dirty_bytes(&H.s->cache, &H.s->volumes[2].io) == 0);
    expect_busy("writable");
    /* The boot log sink on C:. */
    OK(bootlog_activate(&H.s->vfs, true, H.s->sequence));
    expect_busy("disk_log");
    (void)bootlog_shutdown();
    /* The pagefile on C:. */
    int e = storage_pagefile_bind(H.s, 2, "C:/SYSTEM/PAGEFILE.SWP");
    if (!e) {
        expect_busy("pagefile");
        printf("PASS libdisk busy pagefile\n");
    } else CHECK(e == -FS_EOPNOTSUPP);
    stop(); model_close(&m);
    printf("PASS libdisk EBUSY preflight: second lease, open handle, writable, dirty pages, disk log%s; idle read-only volume detached with no write\n",
           e ? "" : ", pagefile");
}

/* 8. Revalidation after the dry run. */
static void test_revalidation(void)
{
    for (unsigned which = 0; which < 3; which++) {
        struct model m;
        model_open(&m, "reval", 2097152, 0);
        model_old_table(&m, 0x0c);
        start(&m, 0);
        set_entropy(0xc1a0f902u, INVARIANT_ID);
        static struct disk_plan plan;
        OK(disk_plan_whole(0, 0, &plan));
        const char *want = which == 0 ? "partition table" : which == 1 ? "capacity" : "identity";
        if (which == 0) {
            uint8_t s[512]; OK(fake_raw_read(&m.head, 0, s)); s[450] = 0x0b; OK(fake_raw_write(&m.head, 0, s));
        } else if (which == 1) m.dev.capacity -= 8;
        else H.s->raw[0].serial[0] = 'X';
        unsigned writes = m.head.writes + m.vol.writes;
        CHECK(disk_execute_p1(&plan, 0) == DISK_ECHANGED);
        CHECK(!strcmp(disk_last_error()->what, want));
        CHECK(m.head.writes + m.vol.writes == writes && H.s->raw[0].lease.writes == 0);
        CHECK(lease_state(0).state == CIUKI_DISK_LEASE_RELEASED);
        stop(); model_close(&m);
    }
    printf("PASS libdisk revalidation: changed table fingerprint, capacity and identity refuse DISK_ECHANGED before any write\n");
}

/* 9. Failure injection and cancel leave the documented incomplete state. */
static void stub_present(struct model *m, const struct disk_plan *plan)
{
    uint8_t s[512], want[512];
    OK(fake_raw_read(&m->head, 0, s));
    disk_fmt_mbr(want, disk_stub_mbr_code, plan->disk_signature, plan->table);
    CHECK(!memcmp(s, want, 512));
}
static void test_failures(void)
{
    static struct disk_plan plan;
    struct model m;
    /* Write error inside the first FAT during P2. */
    model_open(&m, "fail", 2097152, 0);
    start(&m, 0); set_entropy(0xc1a0f902u, INVARIANT_ID);
    OK(disk_plan_whole(0, 0, &plan)); OK(disk_execute_p1(&plan, 0));
    m.vol.fail_write = DISK_RESERVED + 300;
    CHECK(disk_execute_p2(&plan, 0, 0) == DISK_EIO);
    const struct disk_error_info *e = disk_last_error();
    CHECK(!strcmp(e->what, "write") && e->err == EIO && e->lba <= DISK_PART_LBA + DISK_RESERVED + 300 &&
          DISK_PART_LBA + DISK_RESERVED + 300 < e->lba + e->count && e->count <= 128);
    stub_present(&m, &plan);
    CHECK(lease_state(0).state == CIUKI_DISK_LEASE_HELD && saw("[disk] write disk=0"));
    stop(); model_close(&m);
    /* Writer allocation is part of publication, before any writable mount. */
    model_open(&m, "fail-writer", 2097152, 0);
    start(&m, 0); set_entropy(0xc1a0f902u, INVARIANT_ID);
    OK(disk_plan_whole(0, 0, &plan)); OK(disk_execute_p1(&plan, 0));
    H.writer_error = -FS_ENOMEM;
    CHECK(disk_execute_p2(&plan, 0, 0) == DISK_ENOMEM);
    CHECK(lease_state(0).state == CIUKI_DISK_LEASE_HELD && !strcmp(lease_state(0).reason, "writer"));
    CHECK(!vfs_drive_present(&H.s->vfs, 3) && !H.s->volumes[3].present);
    stub_present(&m, &plan);
    H.writer_error = 0;
    char letter;
    OK(disk_execute_p2(&plan, 0, &letter)); CHECK(letter == 'D');
    stop(); model_close(&m);
    /* Flush error at the end of P2 (the third device flush of the volume). */
    model_open(&m, "fail", 2097152, 0);
    start(&m, 0); set_entropy(0xc1a0f902u, INVARIANT_ID);
    OK(disk_plan_whole(0, 0, &plan)); OK(disk_execute_p1(&plan, 0));
    m.vol.fail_flush_call = m.vol.flush_calls + 2;
    CHECK(disk_execute_p2(&plan, 0, 0) == DISK_EFLUSH && !strcmp(disk_last_error()->what, "flush"));
    stub_present(&m, &plan);
    CHECK(lease_state(0).state == CIUKI_DISK_LEASE_HELD);
    stop(); model_close(&m);
    /* Flush error after the final MBR: the write is not confirmed and a
     * power cut loses it (write-back device model). */
    static struct install x;
    memset(&x, 0, sizeof(x));
    model_open(&x.m, "fail", 2097152, 0);
    start(&x.m, 0); set_entropy(0xc1a0f902u, INVARIANT_ID);
    OK(disk_plan_whole(0, 0, &x.plan)); OK(disk_execute_p1(&x.plan, 0)); OK(disk_execute_p2(&x.plan, 0, &x.letter));
    x.m.head.reorder = true;
    H.fail_op = DISK_FLUSH; H.fail_after = 3; H.fail_result = -EIO; H.op_calls = 0;
    struct disk_publish pub = { loader, loader_bytes, boot_cfg, installed_state };
    CHECK(disk_execute_p5(&x.plan, &pub, 0) == DISK_EFLUSH);
    fake_power_loss(&x.m.head);
    stub_present(&x.m, &x.plan);
    stop(); model_close(&x.m);
    /* Cancel between requests of P2. */
    model_open(&m, "fail", 2097152, 0);
    start(&m, 0); set_entropy(0xc1a0f902u, INVARIANT_ID);
    OK(disk_plan_whole(0, 0, &plan)); OK(disk_execute_p1(&plan, 0));
    struct progress_log log = { .monotonic = true, .cancel_after = 3 };
    struct disk_callbacks cb = { on_progress, on_cancel, &log };
    CHECK(disk_execute_p2(&plan, &cb, 0) == DISK_ECANCELLED && log.calls == 3);
    stub_present(&m, &plan);
    CHECK(lease_state(0).state == CIUKI_DISK_LEASE_HELD);
    /* A flush the device cannot confirm is EIO at the node. */
    m.vol.fail_flush = true;
    CHECK(storage_disk_flush(H.s, 0, &H.caller) == -EIO && saw("[disk] flush disk=0 result="));
    stop(); model_close(&m);
    printf("PASS libdisk failure injection: P2 write error (LBA range reported), writer failure publishes nothing (retry succeeds), P2 flush error, unconfirmed final MBR, "
           "cancel: stub MBR and lease kept in every case\n");
}

static void test_cache_disabled(void)
{
    struct model m;
    static struct disk_plan plan;
    model_open(&m, "no-cache", 2097152, 0);
    m.dev.write_cache_state = BLKDEV_CACHE_DISABLED; m.dev.flush = 0;
    start(&m, 0); set_entropy(0xc1a0f902u, INVARIANT_ID);
    OK(disk_plan_whole(0, 0, &plan)); OK(disk_execute_p1(&plan, 0));
    char letter;
    OK(disk_execute_p2(&plan, 0, &letter)); CHECK(letter == 'D');
    struct disk_publish pub = { loader, loader_bytes, boot_cfg, installed_state };
    OK(disk_execute_p5(&plan, &pub, 0));
    fsck_expect(m.vol_path, 0, 0);
    CHECK(!m.head.flush_calls && !m.vol.flush_calls);
    stop(); model_close(&m);
    puts("PASS libdisk cache-disabled device without FLUSH: P1/P2/P5, fsck clean, no unqualified flush command");
}

/* 10. The kernel consistency check on the f1-22 corruption fixtures. */
static void test_consistency(void)
{
    static const struct { const char *defect; int rc; uint32_t must; } cases[] = {
        { "dirty", 0, CIUKI_FAT_RO_DIRTY },
        { "error-flag", DISK_ECONSISTENCY, CIUKI_FAT_RO_IO_FLAG },
        /* A different valid EOC in copy 2 is fat.c's interrupted-EOF case
         * (f1-26): repairable DIRTY, which the directive does not fail. */
        { "fat-divergence", 0, CIUKI_FAT_RO_DIRTY },
        { "fat0-divergence", DISK_ECONSISTENCY, CIUKI_FAT_RO_COPIES },
        { "lost-cluster", DISK_ECONSISTENCY, CIUKI_FAT_RO_LOST },
        { "chain-corruption", DISK_ECONSISTENCY, CIUKI_FAT_RO_CORRUPT },
        { "torn-sector", DISK_ECONSISTENCY, CIUKI_FAT_RO_CORRUPT },
        { "bad-bpb", DISK_ECONSISTENCY, 0 } };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        static struct install x;
        install(&x, "consist", true, false);
        struct disk_check chk;
        OK(disk_consistency_check('D', &chk));
        CHECK(chk.reasons == 0);
        if (!strcmp(cases[i].defect, "fat0-divergence")) {
            /* Copy 2's FAT[0] media byte F0: reserved entries differ. */
            uint8_t fat[512];
            uint64_t at = ((uint64_t)DISK_RESERVED + x.plan.geo.fat_sectors) * 512;
            OK(hf_read(x.m.vol_path, at, fat, 512)); fat[0] = 0xf0; OK(hf_write(x.m.vol_path, at, fat, 512));
        } else if (!strcmp(cases[i].defect, "lost-cluster")) {
            /* Cluster 50000 allocated (end of chain) in both FATs, owned by no file. */
            for (unsigned c = 0; c < 2; c++) {
                uint8_t fat[512];
                uint64_t at = ((uint64_t)DISK_RESERVED + c * x.plan.geo.fat_sectors + 50000 / 128) * 512;
                OK(hf_read(x.m.vol_path, at, fat, 512)); disk_wr32(fat + (50000 % 128) * 4, 0x0fffffffu);
                OK(hf_write(x.m.vol_path, at, fat, 512));
            }
        } else {
            char command[1400], out[512];
            snprintf(command, sizeof(command), "python3 tests/host/libdisk_fixture.py '%s' %s", x.m.vol_path, cases[i].defect);
            int r = hf_run(command, out, sizeof(out));
            if (r) fprintf(stderr, "%s", out);
            CHECK(r == 0);
        }
        memset(&chk, 0, sizeof(chk));
        int rc = disk_consistency_check('D', &chk);
        uint32_t reasons = rc ? disk_last_error()->reasons : chk.reasons;
        printf("CHECK f1-22 %-16s rc=%d err=%d reasons=0x%x lost=%u\n", cases[i].defect, rc,
               rc ? disk_last_error()->err : 0, reasons, chk.lost_clusters);
        CHECK(rc == cases[i].rc);
        CHECK((reasons & cases[i].must) == cases[i].must);
        if (rc == DISK_ECONSISTENCY && strcmp(cases[i].defect, "bad-bpb"))
            CHECK(!disk_last_error()->err && H.s->volumes[3].fat.readonly && lease_state(0).state == CIUKI_DISK_LEASE_PUBLISHED);
        if (!strcmp(cases[i].defect, "bad-bpb")) {
            CHECK(disk_last_error()->err == EINVAL && lease_state(0).state == CIUKI_DISK_LEASE_HELD);
            CHECK(saw("reason=recheck_mount"));
        }
        stop(); model_close(&x.m);
    }
    printf("PASS libdisk consistency check: f1-22 fixtures dirty, error-flag, fat-divergence, chain-corruption, torn-sector, bad-bpb; FAT[0] copy divergence, lost cluster\n");
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: libdisk_test WORKDIR\n"); return 2; }
    work = argv[1];
    disk_set_backend(&backend);
    char path[600];
    snprintf(path, sizeof(path), "%s/loader.bin", work);
    long sectors = hf_size(path);
    CHECK(sectors > 0 && sectors <= 1023);
    loader_bytes = (uint32_t)sectors * 512;
    OK(hf_read(path, 0, loader, loader_bytes));
    test_primitives();
    test_geometry();
    test_refusals();
    test_enumeration();
    test_install();
    test_authority();
    test_busy();
    test_revalidation();
    test_failures();
    test_cache_disabled();
    test_consistency();
    free(H.log_data);
    printf("PASS libdisk host suite: %u checks\n", checks);
    return 0;
}
