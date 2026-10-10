/* F1 storage probes use the production mount/VFS paths. No fixture data is
 * trusted from the selector. Names are UTF-8 hex chunks to preserve record
 * framing; the runner owns independent image hashes/fsck/mtools evidence.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/probe.h>
#include <ciuki/storage.h>
#include <ciuki/bootlog.h>
#include <ciuki/init.h>
#include <ciuki/sha256.h>
#ifndef FS_HOST
#include <ciuki/task.h>
#include <ciuki/ata.h>
#endif

static struct vfs_table probe_table;
static struct fat_entry entry;
static struct fat_entry lookup_entry;
static char walk_path[FS_PATH_BYTES];
static struct { uint32_t directory, cursor; unsigned length; } walk[32];

/* Mount facts are the immutable boot snapshot; live counters below belong
 * to the selected workload and may change after it opens the write gate. */
static void mount_records(const char *probe)
{
    for (unsigned d = 2; d < 26; d++) {
        const struct activation_entry *e = drivers_mount_get(d);
        if (!e) continue;
        rec_emit(probe, "DATA", "group=storage drive=%c disk=%u part=%u layout=%s mode=%s gate=%s writes=%llu qualified=%u reason=%s recovered=%u",
                 'A' + d, e->disk, e->partition, e->partition ? "mbr" : "superfloppy", e->readonly ? "ro" : "rw",
                 e->read_gate ? "read" : "closed", e->writes, e->qualified, e->reason,
                 !strncmp(e->reason, "dirty_recovered", sizeof("dirty_recovered")));
    }
}

static int finish(const char *probe, int error)
{
    rec_emit(probe, "DATA", "group=metadata subcase=complete owner=vfs generation=%u errors=%u gate=%s timing_domain=%s",
             storage_get()->vfs.generation[2], error != 0, error ? "failed" : "complete",
             (g_boot.flags & CBI_F_SMBIOS_QEMU) ? "icount" : "hardware");
    rec_emit(probe, "END", "status=%s error=%d", error ? "FAIL" : "PASS", error);
    return error ? 1 : 0;
}
static void name_record(const char *probe, unsigned id, const char *field, const char *text)
{
    static const char digits[] = "0123456789abcdef";
    size_t length = strlen(text);
    for (unsigned offset = 0; offset < length; offset += 40) {
        unsigned n = (unsigned)length - offset; if (n > 40) n = 40;
        char hex[81];
        for (unsigned j = 0; j < n; j++) {
            uint8_t b = (uint8_t)text[offset + j]; hex[j*2] = digits[b>>4]; hex[j*2+1] = digits[b&15];
        }
        hex[n*2] = 0;
        rec_emit(probe, "DATA", "case=name id=%u field=%s offset=%u bytes=%u hex=%s", id, field, offset, n, hex);
    }
}
int storage_file_digest(struct vfs_table *table, const char *path, uint8_t digest[32], uint32_t *size)
{
    if (!table || !path || !digest || !size) return -FS_EINVAL;
    int h = vfs_open(table, path, VFS_READ, VFS_DENY_NONE, 0);
    if (h < 0) return h;
    uint8_t *buf = fs_page_alloc();
    if (!buf) { vfs_close(table, h); return -FS_ENOMEM; }
    struct sha256_ctx hash; sha256_init(&hash);
    int e = 0; *size = 0;
    for (;;) {
        size_t done;
        e = vfs_read(table, h, buf, 4096, &done);
        if (e || !done) break;
        if (done > UINT32_MAX - *size) { e = -FS_EFBIG; break; }
        sha256_update(&hash, buf, done); *size += (uint32_t)done; fs_service();
    }
    if (!e) sha256_final(&hash, digest);
    fs_page_free(buf);
    int close = vfs_close(table, h);
    return e ? e : close;
}
static int digest_record(const char *probe, unsigned id, struct vfs_table *table, const char *path)
{
    uint8_t digest[32]; uint32_t size; char hex[65];
    int e = storage_file_digest(table, path, digest, &size);
    if (e) return e;
    sha256_hex(digest, hex);
    if (path_equal(path, BOOTLOG_PATH))
        rec_emit(probe, "DATA", "case=file id=%u name_hex=2f53595354454d2f4c4f47532f424f4f542e4c4f47 size=%u sha256=%s", id, size, hex);
    else rec_emit(probe, "DATA", "case=file id=%u size=%u sha256=%s", id, size, hex);
    return 0;
}
static int list_volume(const char *probe, struct storage_volume *v, unsigned *id)
{
    struct sha256_ctx listing; sha256_init(&listing);
    unsigned depth = 0, entries = 0, name_errors = 0, alias_errors = 0, size_errors = 0;
    walk_path[0] = (char)('A' + v->drive); walk_path[1] = ':'; walk_path[2] = '/'; walk_path[3] = 0;
    walk[0].directory = v->fat.type == 32 ? v->fat.root : 0; walk[0].cursor = 0; walk[0].length = 3;
    for (;;) {
        int e = fat_next(&v->fat, walk[depth].directory, &walk[depth].cursor, &entry);
        if (e == -FS_ENOENT) { if (!depth) break; depth--; continue; }
        if (e) return e;
        if (path_equal(entry.alias, ".") || path_equal(entry.alias, "..")) continue;
        e = fat_lookup(&v->fat, walk[depth].directory, entry.name, &lookup_entry);
        if (e || (strlen(entry.name) != strlen(lookup_entry.name) || memcmp(entry.name, lookup_entry.name, strlen(entry.name)))) name_errors++;
        e = fat_lookup(&v->fat, walk[depth].directory, entry.alias, &lookup_entry);
        if (e || (strlen(entry.alias) != strlen(lookup_entry.alias) || memcmp(entry.alias, lookup_entry.alias, strlen(entry.alias)))) alias_errors++;
        unsigned length = walk[depth].length, name = (unsigned)strlen(entry.name);
        if (length + name + 2 > sizeof(walk_path) || ++entries > 16384) return -FS_ENAMETOOLONG;
        memcpy(walk_path + length, entry.name, name + 1);
        unsigned current = ++*id;
        name_record(probe, current, "path", walk_path); name_record(probe, current, "alias", entry.alias);
        rec_emit(probe, "DATA", "case=entry id=%u drive=%c size=%u attr=%u lfn_slots=%u",
                 current, 'A' + v->drive, entry.size, entry.attr, entry.lfn_count);
        /* Canonical list hash: path NUL, alias NUL, size LE32, attr byte,
         * content SHA256 (32 zero bytes for directories), in disk order. */
        uint8_t size[4], digest[32] = { 0 }; fs_wr32(size, entry.size);
        sha256_update(&listing, walk_path, strlen(walk_path) + 1);
        sha256_update(&listing, entry.alias, strlen(entry.alias) + 1);
        sha256_update(&listing, size, 4); sha256_update(&listing, &entry.attr, 1);
        if (!(entry.attr & FAT_ATTR_DIR)) {
            uint32_t bytes; char hex[65];
            e = storage_file_digest(&probe_table, walk_path, digest, &bytes);
            if (e) return e;
            if (bytes != entry.size) size_errors++;
            sha256_hex(digest, hex);
            rec_emit(probe, "DATA", "case=file id=%u size=%u sha256=%s", current, bytes, hex);
            int h = vfs_open(&probe_table, walk_path, VFS_READ, VFS_DENY_NONE, 0);
            if (h < 0) return h;
            uint64_t position; size_t done = 0; uint8_t pair[2];
            uint32_t boundary = (uint32_t)v->fat.spc * 512 - 1;
            e = vfs_seek(&probe_table, h, boundary, VFS_SEEK_SET, &position);
            if (!e) e = vfs_read(&probe_table, h, pair, sizeof(pair), &done);
            unsigned expected = entry.size > boundary ? entry.size - boundary : 0; if (expected > 2) expected = 2;
            if (!e && done != expected) e = -FS_EIO;
            if (!e) e = vfs_seek(&probe_table, h, entry.size, VFS_SEEK_SET, &position);
            size_t eof = 1;
            if (!e) e = vfs_read(&probe_table, h, pair, 1, &eof);
            vfs_close(&probe_table, h);
            if (e || eof) return e ? e : -FS_EIO;
            rec_emit(probe, "DATA", "case=boundary id=%u offset=%u bytes=%u eof_bytes=%u error=%d",
                     current, boundary, (unsigned)done, (unsigned)eof, e);
        }
        sha256_update(&listing, digest, 32);
        if (entry.attr & FAT_ATTR_DIR) {
            if (depth + 1 >= ARRAY_SIZE(walk)) return -FS_ELOOP;
            walk_path[length + name] = '/'; walk_path[length + name + 1] = 0;
            depth++; walk[depth].directory = entry.first; walk[depth].cursor = 0; walk[depth].length = length + name + 1;
        }
    }
    uint8_t digest[32]; char hex[65]; sha256_final(&listing, digest); sha256_hex(digest, hex);
    rec_emit(probe, "DATA", "case=list drive=%c entries=%u sha256=%s", 'A' + v->drive, entries, hex);
    rec_emit(probe, "DATA", "group=fat-read drive=%c fat_type=%u name_errors=%u alias_errors=%u size_errors=%u write_count=%llu chain_bounded=1",
             'A' + v->drive, v->fat.type, name_errors, alias_errors, size_errors, v->writes);
    return name_errors || alias_errors || size_errors ? -FS_EIO : 0;
}
int probe_fat_read(void)
{
    const char *probe = "fat-read"; rec_emit(probe, "BEGIN", 0);
    mount_records(probe);
    struct storage *s = storage_get();
    if (!s->ready || !s->vfs.volumes[2]) return finish(probe, -FS_ENOENT);
    int e = vfs_table_init(&s->vfs, &probe_table, 2); if (e) return finish(probe, e);
    e = vfs_stat(&probe_table, "C:/SYSTEM", &entry);
    if (!e && !(entry.attr & FAT_ATTR_DIR)) e = -FS_ENOTDIR;
    unsigned id = 0;
    for (unsigned disk = 1; disk < STORAGE_DISKS; disk++)
        rec_emit(probe, "DATA", "case=fixture disk=%u status=%s", disk, s->disks[disk] ? "present" : "absent");
    for (unsigned d = 2; d < 26 && !e; d++) {
        struct storage_volume *v = storage_volume(s, d); if (!v) continue;
        const struct activation_entry *mount = drivers_mount_get(d);
        if (!mount) { e = -FS_EIO; break; }
        rec_emit(probe, "DATA", "case=mount drive=%c disk=%u type=%u mode=%s reasons=%u writes=%llu read_gate=%u",
                 'A' + d, mount->disk, mount->type, mount->readonly ? "ro" : "rw", mount->reasons, mount->writes, mount->read_gate);
        if (v->error || !v->fat.readonly || v->writes) { e = v->error ? v->error : -FS_EIO; break; }
        e = list_volume(probe, v, &id);
        rec_emit(probe, "DATA", "case=lfn drive=%c orphan_observations=%u bad_checksum_observations=%u invalid_observations=%u handling=short_fallback",
                 'A' + d, v->fat.lfn_orphans, v->fat.lfn_bad_checksum, v->fat.lfn_invalid);
        if (!e && v->writes) e = -FS_EIO;
    }
    uint16_t ucs[256]; unsigned length;
    int invalid = path_validate_name("bad*name", ucs, &length);
    int unmappable = path_validate_name("bad\xf0\x9f\x90\xb6", ucs, &length);
    rec_emit(probe, "DATA", "case=invalid_name invalid=%d unmappable=%d writes=0", invalid, unmappable);
    if (invalid != -FS_EINVAL || unmappable != -FS_EILSEQ) e = -FS_EIO;
    vfs_table_destroy(&probe_table);
    return finish(probe, e);
}

#define WORK_SEED 0x00c1a009u
#define WORK_ROOT "C:/F109"
static const char *const work_files[] = { WORK_ROOT "/EMPTY.BIN", WORK_ROOT "/CLUSTER.BIN", WORK_ROOT "/FOURM.BIN" };
#define WORK_MARKER WORK_ROOT "/DONE.BIN"
static uint8_t pattern(unsigned i) { return (uint8_t)(i * 37 + WORK_SEED); }
static int expected_file(struct vfs_table *t, const char *path, uint32_t bytes, bool patched)
{
    uint8_t actual[32], expected[32], buf[512]; uint32_t size;
    int e = storage_file_digest(t, path, actual, &size); if (e) return e;
    struct sha256_ctx hash; sha256_init(&hash);
    for (uint32_t at = 0; at < bytes; at += sizeof(buf)) {
        unsigned n = bytes - at; if (n > sizeof(buf)) n = sizeof(buf);
        memset(buf, 0, n);
        if (!at && patched) for (unsigned i = 0; i < n && i < 64; i++) buf[i] = pattern(i);
        sha256_update(&hash, buf, n); fs_service();
    }
    sha256_final(&hash, expected);
    return size == bytes && !memcmp(actual, expected, 32) ? 0 : -FS_EIO;
}
static int make_file(struct vfs_table *t, const char *path, uint32_t bytes)
{
    int h = vfs_open(t, path, VFS_READ | VFS_WRITE | VFS_CREATE | VFS_EXCLUSIVE, VFS_DENY_NONE, 0);
    if (h < 0) return h;
    /* f1-01 replaces whole files atomically. Zero extension exercises the
     * 4 MiB allocation/data/publication path with one page of working RAM. */
    int e = vfs_truncate(t, h, bytes);
    if (!e) e = vfs_commit(t, h);
    vfs_close(t, h);
    if (!e) e = expected_file(t, path, bytes, false);
    return e;
}
static int patch_file(struct vfs_table *t, const char *path, uint32_t bytes)
{
    int h = vfs_open(t, path, VFS_READ | VFS_WRITE, VFS_DENY_NONE, 0); if (h < 0) return h;
    uint8_t buf[64]; for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = pattern(i);
    size_t n = bytes < sizeof(buf) ? bytes : sizeof(buf), done;
    int e = vfs_write(t, h, buf, n, &done);
    if (!e && done != n) e = -FS_EIO;
    if (!e) e = vfs_commit(t, h);
    vfs_close(t, h);
    if (!e) e = expected_file(t, path, bytes, true);
    return e;
}
static int marker(struct vfs_table *t, bool write, const uint32_t sizes[3])
{
    uint8_t bytes[116] = { 'F','1','0','9','W','0','1',0 };
    for (unsigned i = 0; i < 3; i++) {
        fs_wr32(bytes + 8 + i*36, sizes[i]); uint32_t size;
        int e = expected_file(t, work_files[i], sizes[i], true); if (e) return e;
        e = storage_file_digest(t, work_files[i], bytes + 12 + i*36, &size); if (e) return e;
    }
    int h = vfs_open(t, WORK_MARKER, write ? VFS_WRITE | VFS_CREATE | VFS_EXCLUSIVE : VFS_READ, VFS_DENY_NONE, 0);
    if (h < 0) return h;
    size_t done; int e;
    if (write) {
        e = vfs_write(t, h, bytes, sizeof(bytes), &done);
        if (!e) e = vfs_commit(t, h);
    } else {
        uint8_t readback[117]; e = vfs_read(t, h, readback, sizeof(readback), &done);
        if (!e && (done != sizeof(bytes) || memcmp(bytes, readback, sizeof(bytes)))) e = -FS_EIO;
    }
    if (!e && done != sizeof(bytes)) e = -FS_EIO;
    vfs_close(t, h); return e;
}
int storage_write_workload(struct storage *s, struct vfs_table *t, bool *reboot)
{
    if (!s || !t || !reboot || !s->vfs.volumes[2]) return -FS_EINVAL;
    const uint32_t sizes[] = { 0, (uint32_t)s->vfs.volumes[2]->spc * 512, 4u*1024u*1024u };
    int e = vfs_stat(t, WORK_MARKER, &entry);
    if (!e) { *reboot = true; return marker(t, false, sizes); }
    if (e != -FS_ENOENT) return e;
    *reboot = false;
    e = vfs_stat(t, WORK_ROOT, &entry);
    if (!e) return -FS_EUCLEAN; /* never erase an interrupted workload */
    if (e != -FS_ENOENT) return e;
    if ((e = storage_enable_write(s, 2)) || (e = vfs_mkdir(t, WORK_ROOT, 0))) return e;
    for (unsigned i = 0; i < 3; i++) {
        if ((e = make_file(t, work_files[i], sizes[i]))) return e;
        rec_emit("fat-write", "DATA", "case=operation file=%u op=create_read bytes=%u result=%d", i, sizes[i], e);
        if ((e = patch_file(t, work_files[i], sizes[i]))) return e;
        rec_emit("fat-write", "DATA", "case=operation file=%u op=overwrite_read bytes=%u result=%d", i, sizes[i], e);
        int h = vfs_open(t, work_files[i], VFS_WRITE, VFS_DENY_NONE, 0); if (h < 0) return h;
        e = vfs_truncate(t, h, sizes[i]/2); vfs_close(t, h); if (e) return e;
        if ((e = expected_file(t, work_files[i], sizes[i]/2, true))) return e;
        if ((e = vfs_rename(t, work_files[i], WORK_ROOT "/RENAMED.BIN"))) return e;
        if ((e = expected_file(t, WORK_ROOT "/RENAMED.BIN", sizes[i]/2, true))) return e;
        if ((e = vfs_delete(t, WORK_ROOT "/RENAMED.BIN"))) return e;
        rec_emit("fat-write", "DATA", "case=operation file=%u op=truncate_rename_delete result=%d", i, e);
        if ((e = make_file(t, work_files[i], sizes[i])) || (e = patch_file(t, work_files[i], sizes[i]))) return e;
    }
    if ((e = vfs_mkdir(t, WORK_ROOT "/GROW", 0))) return e;
    unsigned count = s->vfs.volumes[2]->spc * 16u / 3u + 10u;
    char name[96], previous[40] = { 0 };
    for (unsigned i = 0; i < count; i++) {
        ksnprintf(name, sizeof(name), WORK_ROOT "/GROW/Collision example %03u.txt", i);
        int h = vfs_open(t, name, VFS_WRITE | VFS_CREATE | VFS_EXCLUSIVE, VFS_DENY_NONE, 0);
        if (h < 0) return h;
        vfs_close(t, h);
        if ((e = vfs_stat(t, name, &entry))) return e;
        if (i && path_equal(previous, entry.alias)) return -FS_EIO;
        memcpy(previous, entry.alias, sizeof(previous));
        name_record("fat-write", i, "alias", entry.alias);
    }
    if ((e = vfs_stat(t, WORK_ROOT "/GROW", &entry))) return e;
    uint32_t next;
    if ((e = fat_get_cluster(s->vfs.volumes[2], entry.first, &next))) return e;
    if (next < 2 || next >= s->vfs.volumes[2]->clusters + 2) return -FS_EIO;
    rec_emit("fat-write", "DATA", "case=directory entries=%u first=%u next=%u aliases=distinct", count, entry.first, next);
    return marker(t, true, sizes);
}
int probe_fat_write(void)
{
    const char *probe = "fat-write"; rec_emit(probe, "BEGIN", 0);
    mount_records(probe);
    struct storage *s = storage_get();
    if (!s->ready || !s->vfs.volumes[2]) return finish(probe, -FS_ENOENT);
    int e = vfs_table_init(&s->vfs, &probe_table, 2); if (e) return finish(probe, e);
    bool reboot = false;
    rec_emit(probe, "DATA", "case=workload workload=zero_patch_v1 seed=%u root=F109", WORK_SEED);
    e = storage_write_workload(s, &probe_table, &reboot);
    for (unsigned i = 0; i < 3 && !e; i++) {
        name_record(probe, i, "path", work_files[i]); e = digest_record(probe, i, &probe_table, work_files[i]);
    }
    vfs_table_destroy(&probe_table);
    if (!e) e = storage_sync();
    rec_emit(probe, "DATA", "case=%s flush_result=%d checker=host_required", reboot ? "cold_reboot" : "workload", e);
    if (!e && !reboot) rec_emit(probe, "ARM", "action=cold_reboot overlay=reuse marker=F109/DONE.BIN");
    return finish(probe, e);
}

/* Small synchronous fixture at the block-driver boundary. Only sectors
 * touched by the cache test exist; no command can reach a real device. */
struct cache_fixture {
    struct blkdev dev;
    uint8_t stable[32][512], pending[32][512];
    bool dirty[32], fail_write, fail_flush;
    uint32_t barriers, writes, persisted, trace_id;
};
static struct cache_fixture cache_fixture;
static int fixture_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf)
{
    struct cache_fixture *f = d->ctx;
    int e = blkdev_range(d, lba, n); if (e) return e;
    for (unsigned i = 0; i < n; i++) memcpy((uint8_t *)buf+i*512, f->dirty[lba+i] ? f->pending[lba+i] : f->stable[lba+i], 512);
    return 0;
}
static int fixture_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf)
{
    struct cache_fixture *f = d->ctx;
    int e = blkdev_range(d, lba, n); if (e) return e;
    if (f->fail_write) return -FS_EIO;
    for (unsigned i = 0; i < n; i++) {
        memcpy(f->pending[lba+i], (const uint8_t *)buf+i*512, 512); f->dirty[lba+i] = true; f->writes++;
        rec_emit("cache", "DATA", "case=trace id=%u barrier=%u action=write lba=%llu", ++f->trace_id, f->barriers+1, lba+i);
    }
    return 0;
}
static int fixture_flush(struct blkdev *d)
{
    struct cache_fixture *f = d->ctx;
    if (f->fail_flush) return -FS_EIO;
    f->barriers++;
    for (unsigned i = 32; i; i--) if (f->dirty[i-1]) {
        memcpy(f->stable[i-1], f->pending[i-1], 512); f->dirty[i-1] = false; f->persisted++;
        rec_emit("cache", "DATA", "case=trace id=%u barrier=%u action=persist lba=%u", ++f->trace_id, f->barriers, i-1);
    }
    rec_emit("cache", "DATA", "case=trace id=%u barrier=%u action=flush", ++f->trace_id, f->barriers);
    return 0;
}
static void fixture_init(void)
{
    memset(&cache_fixture, 0, sizeof(cache_fixture));
    cache_fixture.dev = (struct blkdev){ .read=fixture_read, .write=fixture_write, .flush=fixture_flush,
        .capacity=32, .sector_size=512, .write_cache_state=BLKDEV_CACHE_ENABLED, .ctx=&cache_fixture };
}
static int coherence(struct storage *s)
{
    int e = storage_enable_write(s, 2); if (e) return e;
    int a = vfs_open(&probe_table, "C:/F109CACH.BIN", VFS_READ | VFS_WRITE | VFS_CREATE | VFS_EXCLUSIVE, VFS_DENY_NONE, 0);
    if (a < 0) return a;
    int b = vfs_open(&probe_table, "C:/F109CACH.BIN", VFS_READ, VFS_DENY_NONE, 0);
    if (b < 0) { vfs_close(&probe_table, a); return b; }
    int dup = vfs_dup(&probe_table, a, &probe_table, -1); size_t n; char bytes[4]; uint64_t pos = 0;
    e = dup < 0 ? dup : vfs_write(&probe_table, a, "sync", 4, &n);
    if (!e && n != 4) e = -FS_EIO;
    if (!e) e = vfs_seek(&probe_table, dup, 0, VFS_SEEK_CUR, &pos);
    if (!e) e = vfs_read(&probe_table, b, bytes, 4, &n);
    if (!e && (pos != 4 || n != 4 || memcmp(bytes, "sync", 4))) e = -FS_EIO;
    rec_emit("cache", "DATA", "case=coherence shared_position=%llu independent_bytes=%u result=%d", pos, (unsigned)n, e);
    vfs_close(&probe_table, a); vfs_close(&probe_table, b); if (dup >= 0) vfs_close(&probe_table, dup);
    if (!e) e = vfs_delete(&probe_table, "C:/F109CACH.BIN");
    return e;
}
/* Caller supplies a fresh synthetic device. A real device is never faulted
 * by this helper. The test exercises delayed cache errors through unmount. */
int storage_cache_fault(struct block_cache *c, struct blkdev *d, void (*inject)(void *, bool), void *ctx, bool flush)
{
    uint8_t buf[512] = { 0 };
    int e = cache_read(c, d, 0, buf); if (e) return e;
    e = cache_write(c, d, 0, buf); if (e) return e;
    inject(ctx, flush);
    int delayed = cache_writeback_tick(c, fs_now_ms() + 5001);
    int barrier_error = cache_barrier(c, d);
    struct fat_volume vol = { .cache=c, .dev=d, .type=16, .mounted=true, .writable_session=true };
    int unmount = fat_unmount(&vol);
    rec_emit("cache", "DATA", "case=ata_error fault=%s delayed=%d flush=%d unmount=%d mounted=%u quarantined=%u",
             flush ? "flush" : "write", delayed, barrier_error, unmount, vol.mounted, d->quarantined);
    bool ok = delayed == -FS_EIO && barrier_error == delayed && unmount == delayed && vol.mounted;
    cache_invalidate(c, d, true); /* synthetic power loss, never real recovery */
    return ok ? 0 : -FS_EIO;
}
#ifndef FS_HOST
struct storage_ata_fixture {
    struct ata_channel channel;
    uint8_t selected, status, command, control;
    unsigned word;
    bool armed, flush_fault;
};
static struct storage_ata_fixture ata_fixture;
static uint8_t storage_ata_in8(void *ctx, uint16_t port)
{
    struct storage_ata_fixture *f = ctx;
    unsigned reg = port - f->channel.base;
    if (port == f->channel.control || reg == 7) return f->selected ? 0 : f->status;
    if (reg == 1) return 4;
    if (reg == 2 || reg == 3) return 1;
    return 0;
}
static void storage_ata_out8(void *ctx, uint16_t port, uint8_t value)
{
    struct storage_ata_fixture *f = ctx;
    unsigned reg = port - f->channel.base;
    if (port == f->channel.control) { f->control = value; if (value & 4) f->status = 0x40; return; }
    if (reg == 6) { f->selected = value >> 4 & 1; f->status = 0x40; return; }
    if (reg != 7) return;
    f->command = value; f->word = 0; f->status = value == 0xe7 ? 0x40 : 0x48;
    if (f->armed && value == (f->flush_fault ? 0xe7 : 0x30)) f->status = 0x41;
    /* The first normal WRITE DRQ has no IRQ; errors and flushes do. */
    if (!(f->control & 2) && (value != 0x30 || f->status == 0x41)) ata_channel_irq(&f->channel);
}
static uint16_t storage_ata_in16(void *ctx, uint16_t port)
{
    (void)port; struct storage_ata_fixture *f = ctx; unsigned w = f->word++;
    uint16_t value = 0;
    if (f->command == 0xec) {
        if (w == 49) value = 1u << 9;
        if (w == 60) value = 1024;
        if (w == 82 || w == 85) value = 1u << 5;
        if (w == 83) value = 0x5000;
        if (w == 87) value = 0x4000;
    }
    if (f->word == 256) f->status = 0x40;
    return value;
}
static void storage_ata_out16(void *ctx, uint16_t port, uint16_t value)
{
    (void)port; (void)value; struct storage_ata_fixture *f = ctx;
    if (++f->word == 256) { f->status = 0x40; if (!(f->control & 2)) ata_channel_irq(&f->channel); }
}
static uint64_t storage_ata_now(void *ctx) { (void)ctx; return deadline_after_ms(0); }
static uint64_t storage_ata_deadline(void *ctx, uint64_t d) { (void)ctx; return d; }
static int storage_ata_delay(void *ctx, uint32_t us) { (void)ctx; (void)us; return 0; }
static void storage_ata_inject(void *ctx, bool flush)
{
    struct storage_ata_fixture *f = ctx; f->armed = true; f->flush_fault = flush;
}
static int cache_ata_faults(struct block_cache *c)
{
    static const struct ata_port_ops ops = { storage_ata_in8, storage_ata_out8, storage_ata_in16,
        storage_ata_out16, storage_ata_now, storage_ata_deadline, storage_ata_delay };
    uint64_t real = ata_command_count();
    for (unsigned mode = 0; mode < 2; mode++) {
        struct storage_ata_fixture *f = &ata_fixture; memset(f, 0, sizeof(*f));
        int e = ata_channel_setup(&f->channel, 0, &ops, f); if (e) return e;
        if (ata_channel_discover(&f->channel) != 1) return -FS_EIO;
        struct ata_device *dev = &f->channel.devices[0];
        e = storage_cache_fault(c, &dev->block, storage_ata_inject, f, !!mode);
        uint64_t issued = f->channel.commands; uint8_t bytes[512];
        int next = dev->block.read(&dev->block, 0, 1, bytes);
        uint64_t later = f->channel.commands - issued;
        rec_emit("cache", "DATA", "case=ata_quarantine owner=synthetic_ata generation=%u further_commands=%llu real_commands=%llu next=%d",
                 dev->generation, later, ata_command_count()-real, next);
        if (e || !dev->block.quarantined || later || next != -FS_EQUARANTINED || ata_command_count() != real) return -FS_EIO;
    }
    return 0;
}
#endif
int probe_cache(void)
{
    rec_emit("cache", "BEGIN", 0);
    mount_records("cache");
    struct storage *s = storage_get();
    if (!s->ready || !s->vfs.volumes[2]) return finish("cache", -FS_ENOENT);
    /* Isolated tiny cache solely to force deterministic eviction; real
     * volumes retain their one shared system cache. */
    static struct block_cache c;
    int e = cache_init(&c, 8192); if (e) return finish("cache", e);
    fixture_init(); struct blkdev *d = &cache_fixture.dev;
    uint8_t buf[512]; memset(buf, 0x59, sizeof(buf));
    uint32_t start = fs_now_ms();
    e = cache_write(&c, d, 3, buf);
    if (!e) e = cache_writeback_tick(&c, start + 4999);
    if (!e && cache_fixture.writes) e = -FS_EIO;
    /* The host tests supply exact boundary time; the guest waits real PIT
     * ticks, then observes the same production hook. */
#ifndef FS_HOST
    task_sleep_ms(5000);
    uint32_t now = fs_now_ms();
#else
    uint32_t now = start + 5001;
#endif
    if (!e) e = cache_writeback_tick(&c, now);
    if (!e && (cache_fixture.persisted != 1 || memcmp(cache_fixture.stable[3], buf, 512))) e = -FS_EIO;
    rec_emit("cache", "DATA", "case=writeback dirty_age_ms=%u writes=%u persisted=%u result=%d",
             now-start, cache_fixture.writes, cache_fixture.persisted, e);
    for (unsigned i = 0; i < c.blocks + 1 && !e; i++) e = cache_write(&c, d, i, buf);
    unsigned evicted = cache_fixture.writes;
    if (!e && evicted <= 1) e = -FS_EIO;
    if (!e) e = cache_barrier(&c, d);
    rec_emit("cache", "DATA", "case=eviction blocks=%u issued=%u barriers=%u result=%d", c.blocks, evicted, cache_fixture.barriers, e);
    for (unsigned fault = 0; fault < 2 && !e; fault++) {
        cache_invalidate(&c, d, true); fixture_init();
        e = cache_write(&c, d, 0, buf); if (e) break;
        cache_fixture.fail_write = !fault; cache_fixture.fail_flush = !!fault;
        int delayed = cache_writeback_tick(&c, fs_now_ms()+5001);
        int flush = cache_barrier(&c, d);
        struct fat_volume fake = { .cache=&c, .dev=d, .mounted=true, .writable_session=true };
        int unmount = fat_unmount(&fake);
        rec_emit("cache", "DATA", "case=error layer=block_driver fault=%s delayed=%d flush=%d unmount=%d mounted=%u",
                 fault ? "flush" : "write", delayed, flush, unmount, fake.mounted);
        if (delayed != -FS_EIO || flush != delayed || unmount != delayed || !fake.mounted) e = -FS_EIO;
    }
    cache_invalidate(&c, d, true);
#ifndef FS_HOST
    if (!e) e = cache_ata_faults(&c);
#endif
    cache_destroy(&c);
    /* Mount a separate, read-only forwarding view of the boot fixture with
     * an unqualified FLUSH. Never writes the backing volume. */
    static struct fat_volume unsupported;
    struct storage_volume *v = storage_volume(s, 2);
    struct blkdev noflush = v->io; noflush.flush = 0; noflush.write_cache_state = BLKDEV_CACHE_ENABLED;
    if (!e) e = fat_mount(&unsupported, &s->cache, &noflush, 0, noflush.capacity, false);
    int upgrade = e ? e : fat_enable_write(&unsupported);
    rec_emit("cache", "DATA", "case=unsupported_flush mode=%s upgrade=%d reasons=%u", unsupported.readonly ? "ro" : "rw", upgrade, unsupported.ro_reasons);
    if (!e && (upgrade != -FS_EROFS || !unsupported.readonly)) e = -FS_EIO;
    cache_invalidate(&s->cache, &noflush, true);
    if (!e) e = vfs_table_init(&s->vfs, &probe_table, 2);
    if (!e) { e = coherence(s); vfs_table_destroy(&probe_table); }
    if (!e) e = storage_sync();
    return finish("cache", e);
}

static unsigned cut_index, cut_barrier;
static void cut_trace(struct storage_volume *v, char action, uint64_t lba, int result)
{
    if (action == 'B' && !result) cut_barrier++;
    rec_emit("mount-crash", "DATA", "case=cut index=%u barrier=%u action=%s lba=%llu result=%d durable=%u",
             ++cut_index, cut_barrier, action == 'B' ? "flush" : "write", lba, result,
             !result && (action == 'B' || v->io.write_cache_state == BLKDEV_CACHE_DISABLED));
}
int probe_mount_crash(void)
{
    const char *probe = "mount-crash"; rec_emit(probe, "BEGIN", 0);
    mount_records(probe);
    struct storage *s = storage_get();
    if (!s->ready) return finish(probe, -FS_ENOENT);
    int result = 0; unsigned fixtures = 0; bool cut_reboot = false, cut_selected = false, recovered = false;
    for (unsigned disk = 1; disk < STORAGE_DISKS; disk++) {
        if (!s->disks[disk]) continue;
        fixtures++;
        rec_emit(probe, "DATA", "case=partition disk=%u error=%d", disk, s->disk_errors[disk]);
        for (unsigned d = 3; d < 26; d++) {
            struct storage_volume *v = storage_volume(s, d); if (!v || v->disk != disk) continue;
            int refusal = v->error;
            if (!refusal && v->read_gate && (v->fat.ro_reasons & FAT_RO_DIRTY))
                refusal = storage_enable_write(s, d);
            if (!refusal) {
                int open = vfs_table_init(&s->vfs, &probe_table, d);
                if (!open) {
                    char path[] = "D:/F109REF.BIN"; path[0] = (char)('A' + d);
                    refusal = vfs_open(&probe_table, path, VFS_WRITE | VFS_CREATE, VFS_DENY_NONE, 0);
                    if (refusal >= 0) { vfs_close(&probe_table, refusal); refusal = 0; }
                    vfs_table_destroy(&probe_table);
                } else refusal = open;
            }
            rec_emit(probe, "DATA", "case=mount drive=%c error=%d reasons=%u mode=%s lost=%u write_refusal=%d writes=%llu recovered=%u refusals=%llu reason=%s",
                     'A' + d, v->error, v->fat.ro_reasons, v->fat.readonly ? "ro" : "rw", v->fat.lost_clusters, refusal, v->writes,
                     v->fat.dirty_recovered, v->refused, v->fat.dirty_recovered ? "dirty_recovered" : "mount");
            if (v->fat.dirty_recovered) {
                recovered = true;
                if (v->error || v->fat.readonly || v->fat.ro_reasons || v->fat.lost_clusters || refusal || v->refused || !v->writes)
                    result = -FS_EIO;
            } else if (v->error ? v->fat.mounted : (!v->fat.readonly || refusal != -FS_EROFS || v->writes ||
                !(v->fat.ro_reasons & ~(FAT_RO_REQUEST | FAT_RO_DURABILITY)))) result = -FS_EIO;
        }
    }
    if (!fixtures) rec_emit(probe, "DATA", "case=corrupt_fixtures status=not_run reason=absent");
    /* Crash selection is an on-disk marker the runner puts in its overlay;
     * no new selector grammar or privileged host command is invented. */
    if (!s->vfs.volumes[2]) return finish(probe, result ? result : -FS_ENOENT);
    int e = vfs_table_init(&s->vfs, &probe_table, 2); if (e) return finish(probe, e);
    e = vfs_stat(&probe_table, "C:/F109CUT.ARM", &entry);
    if (!e) {
        cut_selected = true;
        struct storage_volume *v = storage_volume(s, 2);
        if (v->fat.dirty_recovered || (v->fat.ro_reasons & ~(FAT_RO_REQUEST | FAT_RO_DURABILITY))) {
            cut_reboot = true;
            bool recover = v->read_gate && (v->fat.ro_reasons & FAT_RO_DIRTY);
            int enabled = recover ? storage_enable_write(s, 2) : 0;
            /* The mount scan has already inspected owners; the independent
             * host checker remains mandatory after power-cut/export. */
            rec_emit(probe, "DATA", "case=crash_reboot reasons=%u lost=%u scan_corrupt=%u checker=host_required writes=%llu recovered=%u mode=%s reason=%s",
                     v->fat.ro_reasons, v->fat.lost_clusters, !!(v->fat.ro_reasons & FAT_RO_CORRUPT), v->writes,
                     v->fat.dirty_recovered, v->fat.readonly ? "ro" : "rw", v->fat.dirty_recovered ? "dirty_recovered" : "mount");
            /* Opening the existing marker for writing proves the gate without
             * changing the interrupted directory sector or orphan LFN. */
            int refusal = vfs_open(&probe_table, "C:/F109CUT.ARM", VFS_WRITE, VFS_DENY_NONE, 0);
            if (refusal >= 0) { vfs_close(&probe_table, refusal); refusal = 0; }
            rec_emit(probe, "DATA", "case=crash_refusal drive=C write_refusal=%d writes=%llu refusals=%llu",
                     refusal, v->writes, v->refused);
            if (v->fat.dirty_recovered) {
                recovered = true;
                if (enabled || v->fat.readonly || v->fat.ro_reasons || v->fat.lost_clusters || refusal || v->refused || !v->writes)
                    result = -FS_EIO;
            } else if (refusal != -FS_EROFS || v->writes) result = -FS_EIO;
            if (v->fat.ro_reasons & FAT_RO_CORRUPT) result = -FS_EUCLEAN;
        } else {
            e = storage_enable_write(s, 2);
            if (!e) {
                cut_index = cut_barrier = 0; v->trace = cut_trace;
                rec_emit(probe, "ARM", "action=crash_cut marker=F109CUT.ARM workload=replace_rename bytes=8192");
                e = make_file(&probe_table, "C:/F109CUT.BIN", 8192);
                if (!e) e = patch_file(&probe_table, "C:/F109CUT.BIN", 8192);
                if (!e) e = vfs_rename(&probe_table, "C:/F109CUT.BIN", "C:/F109END.BIN");
                v->trace = 0;
                rec_emit(probe, "DATA", "case=cut_missed result=%d status=not_run", e);
                result = e ? e : -FS_EIO;
            } else result = e;
        }
    } else if (e == -FS_ENOENT) rec_emit(probe, "DATA", "case=crash_cut status=not_run reason=marker_absent");
    else result = e;
    vfs_table_destroy(&probe_table);
    /* Export after this barrier/unmount sees the recovered clean flag. */
    if (recovered && !result) result = storage_sync();
    /* Missing fixture/cut evidence must never become a complete PASS. */
    if (!fixtures && !cut_reboot && !result) result = -FS_ENOENT;
    rec_emit(probe, "DATA", "case=coverage fixtures=%u cut_selected=%u cut_reboot=%u checker=host_required", fixtures, cut_selected, cut_reboot);
    return finish(probe, result);
}
static int log_marker(struct vfs_table *t, bool write)
{
    uint8_t bytes[44] = { 'F','1','0','9','L','O','G','1' }; uint32_t size;
    int e = storage_file_digest(t, BOOTLOG_PATH, bytes + 12, &size); if (e) return e;
    fs_wr32(bytes + 8, size);
    int h = vfs_open(t, "C:/F109LOG.OK", write ? VFS_WRITE | VFS_CREATE | VFS_EXCLUSIVE : VFS_READ, VFS_DENY_NONE, 0);
    if (h < 0) return h;
    size_t done;
    if (write) {
        e = vfs_write(t, h, bytes, sizeof(bytes), &done);
        if (!e) e = vfs_commit(t, h);
    } else {
        uint8_t actual[45]; e = vfs_read(t, h, actual, sizeof(actual), &done);
        if (!e && (done != sizeof(bytes) || memcmp(bytes, actual, sizeof(bytes)))) e = -FS_EIO;
    }
    if (!e && done != sizeof(bytes)) e = -FS_EIO;
    vfs_close(t, h); return e;
}
int probe_bootlog(void)
{
    const char *probe = "bootlog"; rec_emit(probe, "BEGIN", 0);
    mount_records(probe);
    struct storage *s = storage_get(); struct bootlog_stats before, after;
    bootlog_snapshot(&before);
    rec_emit(probe, "DATA", "group=bootlog case=before prequalification_writes=%llu storage_calls=%llu queued=%u limit=%u",
             before.writes, before.storage_calls, before.queued, BOOTLOG_LIMIT);
    if (before.writes || before.storage_calls || !s->ready) return finish(probe, -FS_EIO);
    struct storage_volume *v = storage_volume(s, 2);
    if (!v || v->error || !v->read_gate || !blkdev_durable(blkpart_device(&v->part))) {
        int unavailable = bootlog_activate(&s->vfs, false, 0);
        bootlog_snapshot(&after);
        rec_emit(probe, "DATA", "group=bootlog case=readonly disk_log=unavailable result=%d write_count=%llu storage_calls=%llu",
                 unavailable, after.writes, after.storage_calls);
        return finish(probe, unavailable == -FS_EROFS && !after.storage_calls ? 0 : -FS_EIO);
    }
    klog("[bootlog] ordinary output capture check");
    bootlog_snapshot(&after);
    bool connected = after.captured > before.captured;
    rec_emit(probe, "DATA", "case=capture source=klog connected=%u captured=%llu", connected, after.captured);
    if (!connected) return finish(probe, -FS_EOPNOTSUPP);
    int e = vfs_table_init(&s->vfs, &probe_table, 2); if (e) return finish(probe, e);
    int found = vfs_stat(&probe_table, "C:/F109LOG.OK", &entry);
    if (found && found != -FS_ENOENT) { vfs_table_destroy(&probe_table); return finish(probe, found); }
    bool reboot = !found;
    if (reboot) {
        /* The runner compares the exact prior log digest, not a regenerated
         * expected log containing a different boot's activation records. */
        e = log_marker(&probe_table, false);
        if (!e) e = digest_record(probe, 0, &probe_table, BOOTLOG_PATH);
        rec_emit(probe, "DATA", "case=cold_reboot writes=%llu result=%d", v->writes, e);
    } else {
        e = storage_enable_write(s, 2);
        if (!e) e = bootlog_activate(&s->vfs, true, s->sequence);
        if (!e) {
            bootlog_capture("f1-09 boot log durable record\n", sizeof("f1-09 boot log durable record\n") - 1);
            klog("[bootlog] klog qualification record");
            e = bootlog_shutdown();
        }
        if (!e) e = digest_record(probe, 0, &probe_table, BOOTLOG_PATH);
        if (!e) e = log_marker(&probe_table, true);
    }
    bootlog_snapshot(&after);
    uint32_t log_bytes = after.size;
    if (!e && reboot) {
        e = vfs_stat(&probe_table, BOOTLOG_PATH, &entry);
        if (!e) log_bytes = entry.size;
    }
    rec_emit(probe, "DATA", "case=qualification qualified_seq=%u first_log_write_seq=%u size=%u writes=%llu flush_result=%d",
             after.qualification_sequence, after.first_write_sequence, log_bytes, after.writes, e);
    rec_emit(probe, "DATA", "group=bootlog first_write_after_gate=%u log_bytes=%u reopen_errors=%u durable_flush=%u",
             !reboot && after.first_write_sequence > after.qualification_sequence,
             log_bytes, e != 0, !e);
    vfs_table_destroy(&probe_table);
    if (!e) e = storage_sync();
    if (!e && !reboot) rec_emit(probe, "ARM", "action=cold_reboot overlay=reuse marker=F109LOG.OK");
    /* No panic is invoked here. F0 panic evidence must be collected on its
     * own boot; storage_init suppresses writer/sink for every test selector. */
    rec_emit(probe, "DATA", "case=panic status=not_run required_probe=f0:panic writer_quiesced=%u", s->stopped);
    return finish(probe, e);
}
CIUKI_F1_PROBE("fat-read", probe_fat_read);
CIUKI_F1_PROBE("fat-write", probe_fat_write);
CIUKI_F1_PROBE("cache", probe_cache);
CIUKI_F1_PROBE("mount-crash", probe_mount_crash);
