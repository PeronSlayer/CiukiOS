/* f4-04 T0: the production NTFS read-only path on the f4-03 fixtures.
 * Disk image (MBR, type 07h) -> storage_add_disk -> counted read-only view ->
 * shared block cache -> ntfs_fs adapter -> ntfs_ro library -> fs_ro views ->
 * VFS. Modes: tree, behavior <kind>, refusal, lifecycle, faults.
 * The disk descriptor is read-only and counts every write/flush request.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _POSIX_C_SOURCE 200809L
#include <ciuki/storage.h>
#include <ciuki/sha256.h>
#include <ciuki/kernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

struct ciuki_boot_info g_boot;
void klog(const char *fmt, ...) { (void)fmt; }

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while (0)
#define OK(x) CHECK((x)==0)

struct disk {
    struct blkdev dev; int fd;
    uint64_t reads, writes, flushes, read_number, fail_read_at;
    bool fail_all_reads;
};
static struct disk disk;
static int disk_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf)
{
    struct disk *x = d->ctx; int e = blkdev_range(d, lba, n); if (e) return e;
    for (uint32_t i = 0; i < n; i++) {
        x->reads++; x->read_number++;
        if (x->fail_all_reads || x->read_number == x->fail_read_at) return -FS_EIO;
        if (pread(x->fd, (uint8_t *)buf + i * 512, 512, (off_t)((lba + i) * 512)) != 512) return -FS_EIO;
    }
    return 0;
}
static int disk_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf)
{ (void)lba; (void)buf; ((struct disk *)d->ctx)->writes += n; return -FS_EROFS; }
static int disk_flush(struct blkdev *d) { ((struct disk *)d->ctx)->flushes++; return -FS_EROFS; }

static struct storage *storage;
static struct vfs_table table;
static const char *image_path;
static struct storage *start(void)
{
    memset(&disk, 0, sizeof(disk));
    disk.fd = open(image_path, O_RDONLY); if (disk.fd < 0) { perror(image_path); exit(2); }
    struct stat st; if (fstat(disk.fd, &st)) exit(2);
    disk.dev = (struct blkdev){ .read = disk_read, .write = disk_write, .flush = disk_flush,
        .capacity = (uint64_t)st.st_size / 512, .sector_size = 512, .write_cache_state = BLKDEV_CACHE_ENABLED, .ctx = &disk };
    storage = storage_get();
    OK(storage_setup(storage, 0));
    OK(storage_add_disk(storage, 1, &disk.dev));
    return storage;
}
static void stop(void)
{
    if (table.vfs) vfs_table_destroy(&table);
    storage_destroy(storage); close(disk.fd);
}
static struct storage_ntfs *volume(void) { return &storage->ntfs[0]; }
static void tables(void) { OK(vfs_table_init(&storage->vfs, &table, 2)); }
static void zero_writes(void)
{
    struct storage_ntfs *n = volume();
    CHECK(!disk.writes && !disk.flushes && !n->writes && !n->flushes);
}

/* ---- tree ---------------------------------------------------------- */
static int digest(const char *path, uint64_t *size, char hex[65], unsigned chunk)
{
    int h = vfs_open(&table, path, VFS_READ, VFS_DENY_NONE, 0); if (h < 0) return h;
    struct sha256_ctx ctx; sha256_init(&ctx); *size = 0;
    uint8_t *buf = malloc(chunk); int e = 0; size_t n;
    for (;;) { e = vfs_read(&table, h, buf, chunk, &n); if (e || !n) break; sha256_update(&ctx, buf, n); *size += n; }
    free(buf); vfs_close(&table, h);
    uint8_t d[32]; sha256_final(&ctx, d); sha256_hex(d, hex); return e;
}
static void walk(struct fs_ro_volume *ro, unsigned drive, uint64_t dir, const char *prefix, unsigned depth)
{
    CHECK(depth < 8); if (depth >= 8) return;
    uint32_t cursor = 0; struct fs_ro_node n; int e;
    while (!(e = fs_ro_next(ro, dir, &cursor, &n))) {
        char path[FS_PATH_BYTES]; snprintf(path, sizeof(path), "%s/%s", prefix, n.name);
        char full[FS_PATH_BYTES + 3]; snprintf(full, sizeof(full), "%c:%s", 'A' + drive, path);
        if (n.attr & FS_RO_DIRECTORY) {
            printf("D\t%s\t%u\t%s\n", path, n.attr, n.alias);
            walk(ro, drive, n.id, path, depth + 1);
        } else if (n.status) {
            printf("S\t%s\t%d\n", path, n.status);
            int h = vfs_open(&table, full, VFS_READ, VFS_DENY_NONE, 0); CHECK(h == n.status);
            struct fat_entry st; CHECK(vfs_stat(&table, full, &st) == n.status);
        } else if (n.size > UINT32_MAX) printf("L\t%s\t%llu\n", path, (unsigned long long)n.size);
        else {
            uint64_t size; char hex[65], again[65];
            e = digest(full, &size, hex, 517); CHECK(!e);
            uint64_t size2; CHECK(!digest(full, &size2, again, 4096) && !strcmp(hex, again) && size == size2);
            CHECK(size == n.size);
            printf("F\t%s\t%llu\t%s\t%u\t%s\n", path, (unsigned long long)size, hex, n.attr, n.alias);
        }
    }
    if (e != -FS_ENOENT) printf("E\t%s\t%d\n", prefix[0] ? prefix : "/", e);
}
static int mode_tree(void)
{
    start(); struct storage_ntfs *n = volume();
    CHECK(n->present);
    printf("M\t%d\t%s\t%u\t%u\n", n->error, n->ro.diagnostic ? n->ro.diagnostic : "", n->drive, n->ro.names_omitted);
    if (n->error) { stop(); return failures ? 1 : 0; }
    tables();
    struct fs_ro_node root; OK(n->ro.ops->root(&n->ro, &root));
    walk(&n->ro, n->drive, root.id, "", 0);
    zero_writes();
    printf("C\treads=%llu writes=%llu flushes=%llu io_writes=%llu io_flushes=%llu omitted=%u\n",
           (unsigned long long)disk.reads, (unsigned long long)disk.writes, (unsigned long long)disk.flushes,
           (unsigned long long)n->writes, (unsigned long long)n->flushes, n->ro.names_omitted);
    stop(); return failures ? 1 : 0;
}

/* ---- behavior ------------------------------------------------------ */
static const char *volume_path(char out[FS_PATH_BYTES + 3], const char *p)
{ snprintf(out, FS_PATH_BYTES + 3, "%c:%s", 'A' + volume()->drive, p); return out; }
static int read_all(const char *path, uint8_t *out, size_t cap, size_t *got)
{
    int h = vfs_open(&table, path, VFS_READ, VFS_DENY_NONE, 0); if (h < 0) return h;
    int e = vfs_read(&table, h, out, cap, got); vfs_close(&table, h); return e;
}
static void mutations(const char *file)
{
    char p[FS_PATH_BYTES + 3]; volume_path(p, file);
    struct fat_times times = { 0 }; size_t done; uint64_t pos; uint8_t b[1] = { 0 };
    int h = vfs_open(&table, p, VFS_READ, VFS_DENY_NONE, 0); CHECK(h >= 0);
    CHECK(vfs_open(&table, p, VFS_WRITE, VFS_DENY_NONE, 0) == -FS_EROFS);
    CHECK(vfs_open(&table, p, VFS_READ | VFS_CREATE, VFS_DENY_NONE, 0) == -FS_EROFS);
    CHECK(vfs_open(&table, p, VFS_WRITE | VFS_TRUNCATE, VFS_DENY_NONE, 0) == -FS_EROFS);
    char missing[FS_PATH_BYTES + 3]; volume_path(missing, "/does-not-exist");
    CHECK(vfs_open(&table, missing, VFS_READ | VFS_CREATE, VFS_DENY_NONE, 0) == -FS_EROFS);
    CHECK(vfs_write(&table, h, b, 1, &done) == -FS_EROFS && !done);
    CHECK(vfs_truncate(&table, h, 0) == -FS_EROFS);
    CHECK(vfs_attrib(&table, p, 0) == -FS_EROFS && vfs_times(&table, p, &times) == -FS_EROFS);
    CHECK(vfs_rename(&table, p, missing) == -FS_EROFS && vfs_delete(&table, p) == -FS_EROFS);
    CHECK(vfs_mkdir(&table, missing, 0) == -FS_EROFS && vfs_rmdir(&table, p) == -FS_EROFS);
    CHECK(!vfs_seek(&table, h, 0, VFS_SEEK_SET, &pos) && !vfs_commit(&table, h));
    uint64_t free_bytes; uint32_t unit;
    CHECK(!vfs_free_space(&table, volume()->drive, &free_bytes, &unit) && !free_bytes && unit == 512);
    CHECK(!vfs_close(&table, h));
}
static void expect_file(const char *path, const char *bytes, size_t length)
{
    char p[FS_PATH_BYTES + 3]; uint8_t data[4096]; size_t got = 0; volume_path(p, path);
    CHECK(!read_all(p, data, sizeof(data), &got) && got == length && !memcmp(data, bytes, length));
}
static unsigned count_find(const char *pattern, uint8_t mask)
{
    char p[FS_PATH_BYTES + 3]; struct vfs_find find; struct fat_entry e; unsigned count = 0;
    int r = vfs_find_first(&table, volume_path(p, pattern), mask, &find, &e);
    while (!r) { count++; r = vfs_find_next(&table, &find, &e); }
    CHECK(r == -FS_ENOENT); return count;
}
static void check_names(void)
{
    char p[FS_PATH_BYTES + 3]; struct fat_entry st;
    expect_file("/\xe1\xbe\x80.txt", "greek collation\n", 16);
    /* U+1F88 folds to the stored U+1F80 only in NTFS's $UpCase table, not in the
     * VFS case map: the driver-collation fallback finds the same node. */
    expect_file("/\xe1\xbe\x88.TXT", "greek collation\n", 16);
    CHECK(vfs_stat(&table, volume_path(p, "/\xe1\xbe\x88-missing.txt"), &st) == -FS_ENOENT);
    expect_file("/plain ascii.txt", "ascii\n", 6); expect_file("/PLAIN ASCII.TXT", "ascii\n", 6);
    /* A name outside the BMP with a DOS alias is shown by the alias; one
     * without is omitted and counted. */
    expect_file("/SMILE~1.TXT", "alias names the file\n", 21);
    CHECK(vfs_stat(&table, volume_path(p, "/\xf0\x9f\x98\x80 plain.txt"), &st) < 0);
    CHECK(count_find("/*.*", 0) == 3);        /* directories need the directory mask bit */
    CHECK(count_find("/*.*", 16) == 4);
    CHECK(volume()->ro.names_omitted == 1);
    CHECK(count_find("/*.TXT", 0) == 3 && count_find("/S*.*", 0) == 1);
    /* Directories: nested resolution, per-directory views, generated aliases. */
    expect_file("/Sub dir/inner.txt", "inside a directory\n", 19);
    expect_file("/sub DIR/MIXED case.txt", "mixed case\n", 11);
    expect_file("\\Sub dir\\deeper\\leaf.txt", "two levels down\n", 16);
    expect_file("/SUBDIR~1/inner.txt", "inside a directory\n", 19);   /* alias of a directory with a space */
    expect_file("/Sub dir/../Sub dir/./deeper/../inner.txt", "inside a directory\n", 19);
    CHECK(!vfs_stat(&table, volume_path(p, "/Sub dir"), &st) && (st.attr & FS_RO_DIRECTORY) && !st.size);
    CHECK(vfs_open(&table, volume_path(p, "/Sub dir"), VFS_READ, VFS_DENY_NONE, 0) == -FS_EISDIR);
    CHECK(vfs_stat(&table, volume_path(p, "/Sub dir/missing.txt"), &st) == -FS_ENOENT);
    CHECK(vfs_stat(&table, volume_path(p, "/Sub dir/inner.txt/x"), &st) == -FS_ENOTDIR);
    CHECK(count_find("/Sub dir/*.*", 0) == 2 && count_find("/Sub dir/*.*", 16) == 3 && count_find("/Sub dir/deeper/*.*", 16) == 1);
    CHECK(count_find("/Sub dir/m*.txt", 0) == 1);
    OK(vfs_chdir(&table, volume_path(p, "/Sub dir"))); OK(vfs_set_drive(&table, volume()->drive));
    char cwd[FS_PATH_BYTES]; OK(vfs_getcwd(&table, volume()->drive, cwd)); CHECK(!strcmp(cwd, "/Sub dir"));
    CHECK(!vfs_stat(&table, "inner.txt", &st) && st.size == 19);
    CHECK(!vfs_stat(&table, "deeper\\leaf.txt", &st) && st.size == 16);
    CHECK(!vfs_stat(&table, "..\\plain ascii.txt", &st) && st.size == 6);
    OK(vfs_chdir(&table, "deeper")); OK(vfs_getcwd(&table, volume()->drive, cwd)); CHECK(!strcmp(cwd, "/Sub dir/deeper"));
    OK(vfs_chdir(&table, "..\\..")); OK(vfs_getcwd(&table, volume()->drive, cwd)); CHECK(!strcmp(cwd, "/"));
    CHECK(vfs_chdir(&table, "plain ascii.txt") == -FS_ENOTDIR && vfs_chdir(&table, "nowhere") == -FS_ENOENT);
    /* Walking by interface ids agrees with names: parents identify directories. */
    struct fs_ro_node root, sub, inner; OK(volume()->ro.ops->root(&volume()->ro, &root));
    OK(fs_ro_lookup(&volume()->ro, root.id, "Sub dir", &sub)); CHECK(sub.parent == root.id && (sub.attr & FS_RO_DIRECTORY));
    OK(fs_ro_lookup(&volume()->ro, sub.id, "INNER.TXT", &inner)); CHECK(inner.parent == sub.id && inner.size == 19);
    struct fs_ro_node meta; OK(volume()->ro.ops->metadata(&volume()->ro, inner.id, &meta)); CHECK(meta.size == 19 && meta.id == inner.id);
    CHECK(volume()->ro.ops->metadata(&volume()->ro, inner.id ^ (1ull << 48), &meta) == -FS_EIO);   /* stale sequence */
}
static void check_attributes(void)
{
    char p[FS_PATH_BYTES + 3]; struct fat_entry st;
    OK(vfs_stat(&table, volume_path(p, "/resident.txt"), &st)); CHECK((st.attr & 7) == (FS_RO_READONLY | 2));
    OK(vfs_stat(&table, volume_path(p, "/empty.bin"), &st)); CHECK((st.attr & 7) == (FS_RO_READONLY | 4));
    OK(vfs_stat(&table, volume_path(p, "/allocated.bin"), &st)); CHECK((st.attr & 7) == FS_RO_READONLY);
    /* DOS masks: hidden and system files appear only when asked for. */
    CHECK(count_find("/*.*", 0) == 3);   /* allocated, Long filename, the 240-unit name */
    CHECK(count_find("/*.*", 2) == 4);
    CHECK(count_find("/*.*", 4) == 4);
    CHECK(count_find("/*.*", 6) == 5);
    expect_file("/resident.txt", "Ciuki NTFS resident\n", 20);   /* hidden files stay readable */
}
static void check_clean(void)
{
    char p[FS_PATH_BYTES + 3], q[FS_PATH_BYTES + 3];
    CHECK(volume()->ro.mounted && !volume()->error && !strcmp(volume()->ro.diagnostic, "ntfs"));
    expect_file("/resident.txt", "Ciuki NTFS resident\n", 20);
    expect_file("/Long filename with spaces.txt", "one node, two names\n", 20);
    expect_file("/LONGFI~1.TXT", "one node, two names\n", 20);     /* DOS alias of the same node */
    expect_file("/long FILENAME with SPACES.TXT", "one node, two names\n", 20);
    expect_file("/Resident.TXT", "Ciuki NTFS resident\n", 20);      /* case folding */
    expect_file("\\resident.txt", "Ciuki NTFS resident\n", 20);
    struct fat_entry a, b;
    OK(vfs_stat(&table, volume_path(p, "/LONGFI~1.TXT"), &a)); OK(vfs_stat(&table, volume_path(q, "/long filename with spaces.txt"), &b));
    CHECK(a.size == 20 && b.size == 20 && !strcmp(a.name, b.name) && !strcmp(a.alias, "LONGFI~1.TXT"));
    CHECK((a.attr & FS_RO_READONLY) && !(a.attr & FS_RO_DIRECTORY));
    struct fat_entry e; OK(vfs_stat(&table, volume_path(p, "/empty.bin"), &e)); CHECK(!e.size);
    uint8_t data[4096]; size_t got = 99; CHECK(!read_all(p, data, sizeof(data), &got) && !got);
    /* 240-unit name, up to the VFS limit. */
    char longname[300]; strcpy(longname, "/"); memset(longname + 1, 'n', 240); strcpy(longname + 241, ".txt");
    expect_file(longname, "long component\n", 15);
    /* allocated.bin: bytes(range(256))*257, read across cluster boundaries. */
    int h = vfs_open(&table, volume_path(p, "/allocated.bin"), VFS_READ, VFS_DENY_NONE, 0); CHECK(h >= 0);
    uint64_t pos; static uint8_t big[70000]; size_t total = 0;
    for (;;) { size_t n; int r = vfs_read(&table, h, big + total, 1000, &n); CHECK(!r); if (!n) break; total += n; }
    CHECK(total == 65792); for (size_t i = 0; i < total; i++) if (big[i] != (uint8_t)i) { CHECK(0); break; }
    OK(vfs_seek(&table, h, 4093, VFS_SEEK_SET, &pos)); size_t n; OK(vfs_read(&table, h, data, 10, &n));
    CHECK(n == 10 && !memcmp(data, big + 4093, 10));
    OK(vfs_seek(&table, h, -1, VFS_SEEK_END, &pos)); CHECK(pos == 65791);
    OK(vfs_seek(&table, h, 0, VFS_SEEK_END, &pos)); OK(vfs_read(&table, h, data, 10, &n)); CHECK(!n);
    OK(vfs_seek(&table, h, 1 << 20, VFS_SEEK_SET, &pos)); OK(vfs_read(&table, h, data, 10, &n)); CHECK(!n);
    OK(vfs_close(&table, h));
    /* Alternate stream syntax, system files and non-BMP names. */
    struct fat_entry st;
    CHECK(vfs_stat(&table, volume_path(p, "/resident.txt:stream"), &st) == -FS_EOPNOTSUPP);
    CHECK(vfs_open(&table, volume_path(p, "/resident.txt:$DATA"), VFS_READ, VFS_DENY_NONE, 0) == -FS_EOPNOTSUPP);
    CHECK(vfs_stat(&table, volume_path(p, "/$MFT"), &st) == -FS_ENOENT);
    CHECK(vfs_stat(&table, volume_path(p, "/$Extend"), &st) == -FS_ENOENT);
    CHECK(vfs_stat(&table, volume_path(p, "/no-such-file"), &st) == -FS_ENOENT);
    CHECK(vfs_stat(&table, volume_path(p, "/resident.txt/child"), &st) == -FS_ENOTDIR);
    CHECK(vfs_stat(&table, volume_path(p, "/caff\xc3\xa8-\xe7\x8a\xac-\xf0\x9f\x98\x80.txt"), &st) < 0);
    CHECK(volume()->ro.names_omitted == 1);   /* the one name outside the BMP; counted once */
    /* Enumeration hides system files and honors attribute masks. */
    struct vfs_find find; unsigned count = 0;
    int r = vfs_find_first(&table, volume_path(p, "/*.*"), 0, &find, &e);
    while (!r) { count++; CHECK(!(e.attr & FS_RO_DIRECTORY)); r = vfs_find_next(&table, &find, &e); }
    CHECK(r == -FS_ENOENT && count == 5);     /* six files minus the non-BMP name */
    count = 0; r = vfs_find_first(&table, volume_path(p, "/L*.TXT"), 0, &find, &e);
    while (!r) { count++; CHECK(!strcmp(e.alias, "LONGFI~1.TXT")); r = vfs_find_next(&table, &find, &e); }
    CHECK(r == -FS_ENOENT && count == 1);
    count = 0; r = vfs_find_first(&table, volume_path(p, "/LONGFI~1.TXT"), 0, &find, &e);
    while (!r) { count++; r = vfs_find_next(&table, &find, &e); }
    CHECK(count == 1);
    /* Sharing: bidirectional, per open description. */
    volume_path(p, "/resident.txt");
    int one = vfs_open(&table, p, VFS_READ, VFS_DENY_READ, 0); CHECK(one >= 0);
    CHECK(vfs_open(&table, p, VFS_READ, VFS_DENY_NONE, 0) == -FS_EACCES);
    CHECK(!vfs_close(&table, one));
    one = vfs_open(&table, p, VFS_READ, VFS_DENY_NONE, 0); CHECK(one >= 0);
    CHECK(vfs_open(&table, p, VFS_READ, VFS_DENY_READ, 0) == -FS_EACCES);
    int two = vfs_open(&table, p, VFS_READ, VFS_DENY_WRITE, 0); CHECK(two >= 0);
    CHECK(!vfs_close(&table, one) && !vfs_close(&table, two));
    /* cwd and relative names. */
    OK(vfs_chdir(&table, volume_path(p, "/"))); OK(vfs_set_drive(&table, volume()->drive));
    char cwd[FS_PATH_BYTES]; OK(vfs_getcwd(&table, volume()->drive, cwd)); CHECK(!strcmp(cwd, "/"));
    CHECK(!vfs_stat(&table, "resident.txt", &st) && st.size == 20);
    CHECK(vfs_chdir(&table, volume_path(p, "/resident.txt")) == -FS_ENOTDIR);
    mutations("/resident.txt");
}
static void check_unsupported(const char *file, int expected, const char *neighbour, int neighbour_error)
{
    char p[FS_PATH_BYTES + 3]; struct fat_entry st; uint8_t data[64]; size_t got;
    CHECK(volume()->ro.mounted && !volume()->error);
    CHECK(vfs_stat(&table, volume_path(p, file), &st) == expected);
    CHECK(vfs_open(&table, p, VFS_READ, VFS_DENY_NONE, 0) == expected);
    CHECK(vfs_open(&table, p, VFS_WRITE, VFS_DENY_NONE, 0) == expected || vfs_open(&table, p, VFS_WRITE, VFS_DENY_NONE, 0) == -FS_EROFS);
    /* Still listed, with the same status through the interface. */
    uint32_t cursor = 0; struct fs_ro_node n; bool listed = false; int e;
    struct fs_ro_node root; OK(volume()->ro.ops->root(&volume()->ro, &root));
    while (!(e = fs_ro_next(&volume()->ro, root.id, &cursor, &n))) if (!strcmp(n.name, file + 1)) { listed = true; CHECK(n.status == expected); }
    CHECK(listed && e == -FS_ENOENT);
    if (neighbour) CHECK(read_all(volume_path(p, neighbour), data, sizeof(data), &got) == neighbour_error);
}
static void check_sparse(void)
{
    char p[FS_PATH_BYTES + 3]; struct fat_entry st;
    /* The 32-bit legacy API refuses sizes above 4 GiB explicitly; the driver
     * reads the sparse hole as zeros at 64-bit offsets. */
    CHECK(vfs_stat(&table, volume_path(p, "/huge.bin"), &st) == -FS_EFBIG);
    CHECK(vfs_open(&table, p, VFS_READ, VFS_DENY_NONE, 0) == -FS_EFBIG);
    struct fs_ro_node root, node; OK(volume()->ro.ops->root(&volume()->ro, &root));
    OK(fs_ro_lookup(&volume()->ro, root.id, "huge.bin", &node));
    CHECK(node.size == (1ull << 32) + 65537);
    uint8_t buf[100]; size_t got; memset(buf, 0xee, sizeof(buf));
    OK(fs_ro_read(&volume()->ro, node.id, (1ull << 32) + 17, buf, 33, &got));
    uint8_t zero[33] = { 0 }; CHECK(got == 33 && !memcmp(buf, zero, 33));
    OK(fs_ro_read(&volume()->ro, node.id, 0, buf, 19, &got));
    CHECK(got == 19 && !memcmp(buf, "initialized prefix\n", 19));
    OK(fs_ro_read(&volume()->ro, node.id, node.size - 5, buf, 100, &got)); CHECK(got == 5);
    OK(fs_ro_read(&volume()->ro, node.id, node.size, buf, 100, &got)); CHECK(!got);
    OK(fs_ro_read(&volume()->ro, node.id, UINT64_MAX, buf, 100, &got)); CHECK(!got);
    /* A large read assembled from many 1 MiB library calls stays zero. */
    uint8_t *window = malloc(3u << 20); memset(window, 0xee, 3u << 20);
    OK(fs_ro_read(&volume()->ro, node.id, 1ull << 30, window, 3u << 20, &got));
    bool zeros = got == (3u << 20); for (size_t i = 0; zeros && i < got; i++) zeros = !window[i]; CHECK(zeros);
    free(window);
}
static void check_large(void)
{
    char p[FS_PATH_BYTES + 3]; struct fat_entry st, e; struct vfs_find find;
    /* 2048 entries exceed the 256-entry VFS directory view: explicit ENOSPC,
     * never a truncated listing. The root stays usable. */
    CHECK(vfs_find_first(&table, volume_path(p, "/many/*.*"), 16, &find, &e) == -FS_ENOSPC);
    CHECK(vfs_stat(&table, volume_path(p, "/many/entry-00000.txt"), &st) == -FS_ENOSPC);
    CHECK(!vfs_stat(&table, volume_path(p, "/many"), &st) && (st.attr & FS_RO_DIRECTORY));
    CHECK(vfs_find_first(&table, volume_path(p, "/many/*.*"), 16, &find, &e) == -FS_ENOSPC);
}
static void check_damaged(const char *file, int expected)
{
    char p[FS_PATH_BYTES + 3]; struct fat_entry st;
    CHECK(volume()->ro.mounted && !volume()->error);
    CHECK(vfs_stat(&table, volume_path(p, file), &st) == expected);
    CHECK(vfs_open(&table, p, VFS_READ, VFS_DENY_NONE, 0) == expected);
    CHECK(volume()->ro.mounted);                /* a damaged node never unmounts the volume */
}
static int mode_behavior(const char *kind)
{
    start(); struct storage_ntfs *n = volume(); CHECK(n->present);
    if (n->error) { fprintf(stderr, "mount refused %d %s\n", n->error, n->ro.diagnostic); stop(); return 1; }
    tables();
    if (!strcmp(kind, "clean")) check_clean();
    else if (!strcmp(kind, "compressed")) check_unsupported("/compressed.bin", -FS_EOPNOTSUPP, 0, 0);
    else if (!strcmp(kind, "encrypted") || !strcmp(kind, "reparse")) check_unsupported("/allocated.bin", -FS_EOPNOTSUPP, "/resident.txt", 0);
    else if (!strcmp(kind, "runlist")) check_unsupported("/allocated.bin", -FS_EIO, "/resident.txt", 0);
    else if (!strcmp(kind, "attribute-list")) check_damaged("/fragment.bin", -FS_EIO);
    else if (!strcmp(kind, "names")) check_names();
    else if (!strcmp(kind, "attributes")) check_attributes();
    else if (!strcmp(kind, "sparse")) check_sparse();
    else if (!strcmp(kind, "large")) check_large();
    else if (!strcmp(kind, "index-cycle")) {
        char p[FS_PATH_BYTES + 3]; struct fat_entry st;
        CHECK(vfs_stat(&table, volume_path(p, "/many/entry-00000.txt"), &st) == -FS_EIO && n->ro.mounted);
    } else { fprintf(stderr, "unknown kind %s\n", kind); stop(); return 2; }
    zero_writes();
    stop(); printf("PASS behavior %s checks=%u failures=%u\n", kind, checks, failures);
    return failures ? 1 : 0;
}

/* ---- refusal ------------------------------------------------------- */
static int mode_refusal(void)
{
    start(); struct storage_ntfs *n = volume();
    CHECK(n->present && n->error && !n->ro.mounted && !storage->vfs.ro_volumes[n->drive] && !vfs_drive_present(&storage->vfs, n->drive));
    CHECK(!n->ro.state && n->ro.diagnostic);
    printf("R\t%d\t%s\n", n->error, n->ro.diagnostic ? n->ro.diagnostic : "");
    extern unsigned fs_host_contig_live; CHECK(!fs_host_contig_live);
    zero_writes();
    stop(); return failures ? 1 : 0;
}

/* ---- lifecycle ----------------------------------------------------- */
static uint64_t tree_hash_all(void)
{
    struct fs_ro_node root, n; uint32_t cursor = 0; int e; uint64_t sum = 0;
    OK(volume()->ro.ops->root(&volume()->ro, &root));
    while (!(e = fs_ro_next(&volume()->ro, root.id, &cursor, &n))) sum = sum * 1099511628211ull + n.id + n.size;
    CHECK(e == -FS_ENOENT); return sum;
}
static int mode_lifecycle(void)
{
    extern unsigned fs_host_contig_live;
    start(); struct storage_ntfs *n = volume(); CHECK(n->present && !n->error && n->ro.mounted);
    unsigned drive = n->drive; CHECK(drive == 3 && fs_host_contig_live == 1);
    tables(); char p[FS_PATH_BYTES + 3];
    uint32_t generation = storage->vfs.generation[drive]; uint64_t before = tree_hash_all();
    /* Open files and a cwd pin refuse unmount; closing releases it. */
    int h = vfs_open(&table, volume_path(p, "/resident.txt"), VFS_READ, VFS_DENY_NONE, 0); CHECK(h >= 0);
    CHECK(storage_ntfs_unmount(storage, 0) == -FS_EBUSY && n->ro.mounted && vfs_drive_present(&storage->vfs, drive));
    uint8_t data[64]; size_t got; OK(vfs_read(&table, h, data, 20, &got)); CHECK(got == 20);
    OK(vfs_close(&table, h));
    OK(vfs_chdir(&table, volume_path(p, "/")));
    OK(storage_ntfs_unmount(storage, 0));
    CHECK(!n->ro.mounted && !vfs_drive_present(&storage->vfs, drive) && storage->vfs.generation[drive] > generation);
    CHECK(fs_host_contig_live == 0 && !n->ro.state);
    struct fat_entry st; CHECK(vfs_stat(&table, volume_path(p, "/resident.txt"), &st) < 0);
    CHECK(!strcmp(table.cwd[drive], "/"));
    /* Remount: the same letter, a new VFS generation, identical content. */
    uint64_t reads = disk.reads; generation = storage->vfs.generation[drive];
    CHECK(storage_ntfs_mount(storage, 0) == (int)drive && fs_host_contig_live == 1);
    CHECK(storage->vfs.generation[drive] > generation && disk.reads > reads);   /* cache was invalidated */
    CHECK(tree_hash_all() == before);
    expect_file("/resident.txt", "Ciuki NTFS resident\n", 20);
    CHECK(storage_ntfs_mount(storage, 0) == -FS_EBUSY);
    /* A stale handle never reads from the next mount of the same volume. */
    h = vfs_open(&table, volume_path(p, "/resident.txt"), VFS_READ, VFS_DENY_NONE, 0); CHECK(h >= 0);
    struct fat_entry find_entry; struct vfs_find find;
    CHECK(!vfs_find_first(&table, volume_path(p, "/*.*"), 0, &find, &find_entry));
    OK(vfs_chdir(&table, volume_path(p, "/")));
    disk.dev.quarantined = true;                       /* media loss: quarantine of the disk */
    CHECK(vfs_read(&table, h, data, 20, &got) == -FS_EIO && !got);
    CHECK(!vfs_drive_present(&storage->vfs, drive) && !n->ro.mounted && fs_host_contig_live == 0);
    CHECK(vfs_find_next(&table, &find, &find_entry) == -FS_EIO && !strcmp(table.cwd[drive], "/"));
    CHECK(storage_ntfs_mount(storage, 0) < 0);         /* refused while quarantined */
    disk.dev.quarantined = false;
    CHECK(storage_ntfs_mount(storage, 0) == (int)drive);
    CHECK(vfs_read(&table, h, data, 20, &got) == -FS_EIO && !got);   /* old handle, new mount */
    CHECK(vfs_seek(&table, h, 0, VFS_SEEK_SET, &(uint64_t){ 0 }) == -FS_EIO);
    CHECK(vfs_find_next(&table, &find, &find_entry) == -FS_EIO);
    expect_file("/resident.txt", "Ciuki NTFS resident\n", 20);
    OK(vfs_close(&table, h));
    /* Counters: not one block write or flush reached the disk or the view. */
    zero_writes();
    OK(storage_ntfs_unmount(storage, 0)); CHECK(fs_host_contig_live == 0);
    CHECK(storage_ntfs_unmount(storage, 0) == 0);      /* idempotent */
    zero_writes();
    stop(); CHECK(fs_host_contig_live == 0);
    printf("PASS lifecycle checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}

/* ---- failure injection ---------------------------------------------- */
static void open_disk(void)
{
    memset(&disk, 0, sizeof(disk));
    disk.fd = open(image_path, O_RDONLY); if (disk.fd < 0) { perror(image_path); exit(2); }
    struct stat st; if (fstat(disk.fd, &st)) exit(2);
    disk.dev = (struct blkdev){ .read = disk_read, .write = disk_write, .flush = disk_flush,
        .capacity = (uint64_t)st.st_size / 512, .sector_size = 512, .write_cache_state = BLKDEV_CACHE_ENABLED, .ctx = &disk };
}
static unsigned list_root(struct fs_ro_node *root, unsigned *statuses, int *error)
{
    struct storage_ntfs *n = volume(); struct fs_ro_node node; uint32_t cursor = 0; unsigned count = 0;
    int e; *statuses = 0;
    e = n->ro.ops->root(&n->ro, root); if (e) { *error = e; return 0; }
    while (!(e = fs_ro_next(&n->ro, root->id, &cursor, &node))) { count++; *statuses += node.status != 0; }
    *error = e == -FS_ENOENT ? 0 : e; return count;
}
static int mode_faults(void)
{
    extern unsigned fs_host_contig_fail, fs_host_contig_live;
    /* Allocation failure: explicit ENOMEM, no letter, nothing leaked. */
    fs_host_contig_fail = 1; start(); struct storage_ntfs *n = volume();
    CHECK(n->present && n->error == -FS_ENOMEM && !n->ro.mounted && !strcmp(n->ro.diagnostic, "ntfs_workspace") && !fs_host_contig_live);
    CHECK(!vfs_drive_present(&storage->vfs, n->drive));
    CHECK(storage_ntfs_mount(storage, 0) == (int)n->drive && fs_host_contig_live == 1);
    stop(); CHECK(!fs_host_contig_live);
    /* Count the device reads of a good mount, then fail each one in turn. */
    start(); uint64_t mount_reads = disk.reads; CHECK(volume()->ro.mounted); stop();
    CHECK(mount_reads > 20);
    for (uint64_t at = 1; at <= mount_reads + 1; at++) {
        open_disk(); disk.fail_read_at = at;
        storage = storage_get(); OK(storage_setup(storage, 0));
        int e = storage_add_disk(storage, 1, &disk.dev); n = volume();
        bool failed = e < 0 || (n->present && n->error);
        CHECK(failed == (at <= mount_reads));
        if (failed) {
            CHECK(!fs_host_contig_live && !n->ro.mounted && (!n->present || !vfs_drive_present(&storage->vfs, n->drive)));
            if (n->present) {
                CHECK(n->error == -FS_EIO);
                disk.fail_read_at = 0; CHECK(storage_ntfs_mount(storage, 0) == (int)n->drive && fs_host_contig_live == 1);
            }
        } else CHECK(n->present && n->ro.mounted && fs_host_contig_live == 1);
        CHECK(!disk.writes && !disk.flushes && !n->writes && !n->flushes);
        storage_destroy(storage); close(disk.fd); CHECK(!fs_host_contig_live);
    }
    /* A device error while a directory view is built is returned as that
     * error, never recorded as a per-file status; the next attempt succeeds. */
    start(); n = volume(); CHECK(n->ro.mounted); tables();
    struct fs_ro_node root; unsigned statuses, good; int e;
    cache_invalidate(&storage->cache, &n->io, false);   /* the view must come from the disk */
    uint64_t before = disk.reads; good = list_root(&root, &statuses, &e);
    uint64_t view_reads = disk.reads - before; CHECK(!e && !statuses && good == 5 && view_reads > 10);
    unsigned failed_views = 0;
    for (uint64_t k = 1; k <= view_reads + 1; k++) {
        OK(storage_ntfs_unmount(storage, 0)); CHECK(storage_ntfs_mount(storage, 0) == (int)n->drive);
        cache_invalidate(&storage->cache, &n->io, false);
        disk.fail_read_at = disk.read_number + k;
        unsigned count = list_root(&root, &statuses, &e);
        disk.fail_read_at = 0;
        CHECK(!statuses);
        if (e) { failed_views++; CHECK(e == -FS_EIO && !count); count = list_root(&root, &statuses, &e); }
        CHECK(!e && count == 5 && !statuses);
    }
    CHECK(failed_views >= 10);
    CHECK(!disk.writes && !disk.flushes && !n->writes && !n->flushes);
    stop(); CHECK(!fs_host_contig_live);
    printf("PASS faults mount_reads=%llu view_reads=%llu failed_views=%u checks=%u failures=%u\n",
           (unsigned long long)mount_reads, (unsigned long long)view_reads, failed_views, checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: test_ntfs image tree|behavior kind|refusal|lifecycle|faults\n"); return 2; }
    image_path = argv[1];
    if (!strcmp(argv[2], "tree")) return mode_tree();
    if (!strcmp(argv[2], "behavior") && argc == 4) return mode_behavior(argv[3]);
    if (!strcmp(argv[2], "refusal")) return mode_refusal();
    if (!strcmp(argv[2], "lifecycle")) return mode_lifecycle();
    if (!strcmp(argv[2], "faults")) return mode_faults();
    return 2;
}
