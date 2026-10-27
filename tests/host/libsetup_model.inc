/* f9-04 model adapted from f9-02 host qualification of sdk/libdisk over the production lease code
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
#include "../../sdk/libdisk/internal.h"
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
