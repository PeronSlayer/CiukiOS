/* Interim fd/clock controller: production namespace, loader and user syscalls.
 * Missing integration/payload fails the gate explicitly. No QEMU dispatch here.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/files_probe.h>
#include <ciuki/fat_native.h>
#include <ciuki/clock.h>
#include <ciuki/storage.h>
#include <ciuki/signal.h>
#include <ciuki/probe.h>
#include <ciuki/sha256.h>

/* Sparse, fresh FAT32 block boundary, following Microsoft's FAT specification:
 * https://www.cs.fsu.edu/~cop4610t/assignments/project3/spec/fatspec.pdf
 * Only touched sectors consume RAM. Fail the first write after the owning
 * entry's durability barrier; it never affects the canonical disk/controller. */
static struct file_fault_disk {
    struct blkdev dev;
    struct { uint64_t lba; uint8_t bytes[512]; } sectors[32];
    unsigned used, failures;
    uint32_t first_before;
    bool armed;
} fault_disk;
static struct block_cache fault_cache;
static struct fat_volume fault_volume;
static struct vfs fault_vfs;
static struct px_namespace fault_space;
static struct px_node fault_node;

static uint8_t *fault_sector(uint64_t lba, bool create)
{
    for (unsigned i = 0; i < fault_disk.used; i++)
        if (fault_disk.sectors[i].lba == lba) return fault_disk.sectors[i].bytes;
    if (!create || fault_disk.used == ARRAY_SIZE(fault_disk.sectors)) return 0;
    unsigned i = fault_disk.used++;
    fault_disk.sectors[i].lba = lba;
    memset(fault_disk.sectors[i].bytes, 0, 512);
    return fault_disk.sectors[i].bytes;
}
static int fault_read(struct blkdev *dev, uint64_t lba, uint32_t count, void *bytes)
{
    int err = blkdev_range(dev, lba, count);
    if (err) return err;
    for (unsigned i = 0; i < count; i++) {
        uint8_t *sector = fault_sector(lba + i, false);
        if (sector) memcpy((uint8_t *)bytes + i * 512, sector, 512);
        else memset((uint8_t *)bytes + i * 512, 0, 512);
    }
    return 0;
}
static int fault_write(struct blkdev *dev, uint64_t lba, uint32_t count, const void *bytes)
{
    int err = blkdev_range(dev, lba, count);
    if (err) return err;
    /* FAT replace publishes this entry only after update's durable barrier.
     * The next callback is the first issued write in its retirement suffix. */
    if (fault_disk.armed && fault_node.entry.first != fault_disk.first_before) {
        fault_disk.armed = false; fault_disk.failures++;
        dev->quarantined = true;
        return -FS_EIO;
    }
    for (unsigned i = 0; i < count; i++) {
        uint8_t *sector = fault_sector(lba + i, true);
        if (!sector) return -FS_EIO;
        memcpy(sector, (const uint8_t *)bytes + i * 512, 512);
    }
    return 0;
}
static int fault_flush(struct blkdev *dev) { return dev->quarantined ? -FS_EIO : 0; }

/* Also called by T0 against the identical production FAT/cache/fd path. */
bool f2_files_post_commit_fault(void)
{
    memset(&fault_disk, 0, sizeof(fault_disk));
    fault_disk.dev = (struct blkdev){ .read = fault_read, .write = fault_write, .flush = fault_flush,
        .capacity = 66039, .sector_size = 512, .write_cache_state = BLKDEV_CACHE_DISABLED };
    uint8_t *b = fault_sector(0, true);
    b[0] = 0xeb; b[2] = 0x90; fs_wr16(b + 11, 512); b[13] = 1;
    fs_wr16(b + 14, 2); b[16] = 1; b[21] = 0xf8;
    fs_wr32(b + 32, 66039); fs_wr32(b + 36, 512); fs_wr32(b + 44, 2);
    fs_wr16(b + 48, 1); fs_wr16(b + 510, 0xaa55);
    b = fault_sector(1, true);
    fs_wr32(b, 0x41615252); fs_wr32(b + 484, 0x61417272);
    fs_wr32(b + 488, UINT32_MAX); fs_wr32(b + 492, UINT32_MAX); fs_wr32(b + 508, 0xaa550000);
    b = fault_sector(2, true);
    fs_wr32(b, 0x0ffffff8); fs_wr32(b + 4, 0x0fffffff); fs_wr32(b + 8, 0x0fffffff);
    /* Mount scans 65,525 clusters: the cache's 1/16 workspace needs 16 pages.
     * This temporary 1 MiB cache is released before the durable shutdown. */
    int err = cache_init(&fault_cache, 1024 * 1024);
    if (err) return false;
    err = fat_mount(&fault_volume, &fault_cache, &fault_disk.dev, 0, fault_disk.dev.capacity, true);
    vfs_init(&fault_vfs);
    fault_space = (struct px_namespace){ .vfs = &fault_vfs };
    memset(&fault_node, 0, sizeof(fault_node));
    fault_node.space = &fault_space; fault_node.volume = &fault_volume; fault_node.kind = PX_FILE;
    fault_node.linked = true;
    if (!err) err = fat_create(&fault_volume, 2, "fault.bin", 0, &fault_node.entry);
    struct file_description d = { .node = &fault_node, .flags = O_RDWR };
    size_t done = 0;
    if (!err) err = file_io_locked(&d, "old", 3, true, false, 0, &done);
    uint32_t old_first = fault_node.entry.first;
    if (!err && done != 3) err = -EIO;
    if (!err) {
        fault_disk.first_before = old_first;
        fault_disk.armed = true;
        err = file_io_locked(&d, "new", 3, true, true, 0, &done);
    }
    int sync = file_sync_locked(&d), again = file_sync_locked(&d);
    bool pass = err == -EIO && done == 3 && fault_disk.failures == 1 &&
        fault_node.entry.first != old_first && d.position == 3 && fault_volume.readonly &&
        sync == -EIO && again == -EIO && fault_disk.dev.quarantined;
    rec_emit("fd-table", "DATA", "case=post-commit-fault owner=synthetic_block issued_errors=%u count=%u error=%d fsync=%d sticky=%d committed=%u quarantined=%u",
             fault_disk.failures, (unsigned)done, err, sync, again, fault_node.entry.first != old_first, fault_disk.dev.quarantined);
    /* Destroy discards only this disposable device's dirty/error state. */
    cache_destroy(&fault_cache);
    return pass;
}

#ifdef CIUKI_FILES_PAYLOAD_BIN
__asm__(".pushsection .rodata.files_payload,\"a\"\n"
        ".balign 4\n"
        "files_payload_start:\n"
        ".incbin \"" CIUKI_FILES_PAYLOAD_BIN "\"\n"
        "files_payload_end:\n"
        ".popsection\n");
extern const uint8_t files_payload_start[], files_payload_end[];
static int payload_read(void *cookie, uint32_t off, void *dst, uint32_t bytes)
{
    (void)cookie;
    uint32_t length = (uint32_t)(files_payload_end - files_payload_start);
    if (off > length || bytes > length - off) return -EIO;
    memcpy(dst, files_payload_start + off, bytes); return 0;
}
static bool observed(const char *operation, int64_t expected, int64_t actual)
{
    rec_emit("fd-table", "DATA", "operation=%s expected=%lld observed=%lld", operation, expected, actual);
    return expected == actual;
}
static void record_slots(const struct process *p)
{
    unsigned slots = 0;
    for (unsigned i = 0; i < CIUKI_OPEN_MAX; i++)
        slots += p->fds[i].object != 0;
    rec_emit("fd-table", "DATA", "operation=fd-slots slots=%u limit=%u", slots, CIUKI_OPEN_MAX);
}
static unsigned record_growth(const char *bytes, unsigned size)
{
    unsigned errors = 0;
    for (unsigned i = 8; i < size; i++) errors += bytes[i] != 0;
    rec_emit("fd-table", "DATA", "operation=growth size=%u zero_errors=%u", size, errors);
    return errors;
}
static void record_seek(int64_t position, int error, uint32_t before, uint32_t after)
{
    rec_emit("fd-table", "DATA", "operation=seek expected=%llu observed=%lld error=%d free_before=%u free_after=%u",
             (uint64_t)UINT32_MAX + 10, position, error, before, after);
}
static void record_directory_digest(struct sha256_ctx *names)
{
    uint8_t digest[32]; char hex[65];
    sha256_final(names, digest); sha256_hex(digest, hex);
    rec_emit("fd-table", "DATA", "operation=directory-digest encoding=names_nul sha256=%s", hex);
}
static int kernel_io(struct process *p, int fd, void *b, unsigned bytes, bool wr, bool positioned, uint64_t off)
{
    struct file_description *d = file_fd(p, fd);
    if (!d) return -EBADF;
    size_t done;
    fs_lock_take(&d->node->space->vfs->lock);
    int err = file_io_locked(d, b, bytes, wr, positioned, off, &done);
    fs_lock_drop(&d->node->space->vfs->lock);
    return done ? (int)done : err;
}
static int namespace_op(struct process *p, unsigned op, const char *a, const char *b)
{
    struct px_node *cwd = p->cwd;
    fs_lock_take(&cwd->space->vfs->lock);
    int err = op == 0 ? px_mkdir_locked(cwd->space, cwd, a, 0777) :
              op == 1 ? px_rename_locked(cwd->space, cwd, a, b) :
                        px_remove_locked(cwd->space, cwd, a, op == 2);
    fs_lock_drop(&cwd->space->vfs->lock); return err;
}
static bool kernel_cases(struct process *p)
{
    bool pass = true;
    int result = namespace_op(p, 0, "/tmp", 0);
    if (result && result != -EEXIST) return observed("tmp", 0, result);
    int a = file_open(p, "/tmp/f2-kernel-a.bin", O_CREAT | O_EXCL | O_RDWR | O_APPEND, 0666);
    if (a < 0) return observed("create", 0, a);
    pass &= observed("exclusive", -EEXIST, file_open(p, "/tmp/f2-kernel-a.bin", O_CREAT | O_EXCL | O_RDWR, 0666));
    int b = file_open(p, "/tmp/f2-kernel-a.bin", O_RDWR | O_APPEND, 0);
    if (b < 0) return observed("second-open", 0, b);
    pass &= observed("append-a", 4, kernel_io(p, a, "ABCD", 4, true, false, 0));
    pass &= observed("append-b", 4, kernel_io(p, b, "EFGH", 4, true, false, 0));
    pass &= observed("pwrite", 1, kernel_io(p, a, "x", 1, true, true, 2));
    pass &= observed("offset", 4, (int64_t)file_fd(p, a)->position);
    char bytes[64];
    pass &= observed("pread", 8, kernel_io(p, b, bytes, 8, false, true, 0));
    pass &= !memcmp(bytes, "ABxDEFGH", 8);
    uint8_t digest[32]; char hex[65]; sha256(bytes, 8, digest); sha256_hex(digest, hex);
    rec_emit("fd-table", "DATA", "operation=content size=8 sha256=%s", hex);
    int d = file_dup(p, a, 0, false, 0);
    pass &= d >= 0 && file_fd(p, d) == file_fd(p, a);
    pass &= observed("dup2-self", a, file_dup(p, a, a, true, 0));
    pass &= observed("dup2-replace", b, file_dup(p, a, b, true, 0));
    pass &= observed("cloexec", 0, file_fcntl(p, d, F_SETFD, FD_CLOEXEC));
    pass &= observed("cloexec-read", FD_CLOEXEC, file_fcntl(p, d, F_GETFD, 0));
    pass &= observed("status", 0, file_fcntl(p, a, F_SETFL, 0));
    pass &= observed("status-shared", O_RDWR, file_fcntl(p, b, F_GETFL, 0));
    struct px_namespace *s = ((struct px_node *)p->cwd)->space;
    fs_lock_take(&s->vfs->lock);
    int64_t position = 0;
    struct fat_volume *seek_volume = file_fd(p, a)->node->volume;
    uint32_t free_before = seek_volume->free_clusters;
    int seek_result = file_seek_locked(file_fd(p, a), (int64_t)UINT32_MAX + 10, SEEK_SET, &position);
    pass &= !seek_result;
    pass &= position == (int64_t)UINT32_MAX + 10;
    record_seek(position, seek_result, free_before, seek_volume->free_clusters);
    fs_lock_drop(&s->vfs->lock);
    pass &= observed("efbig", -EFBIG, kernel_io(p, a, "x", 1, true, false, 0));
    fs_lock_take(&s->vfs->lock);
    pass &= !file_truncate_locked(file_fd(p, a), sizeof(bytes));
    pass &= !file_sync_locked(file_fd(p, a));
    fs_lock_drop(&s->vfs->lock);
    pass &= observed("grown-read", sizeof(bytes), kernel_io(p, a, bytes, sizeof(bytes), false, true, 0));
    pass &= !record_growth(bytes, sizeof(bytes));
    struct process *child = 0;
    int err = proc_prepare(p, &child);
    if (!err) {
        struct ciuki_spawn_fd inherit = { a, 17 };
        err = proc_inherit(child, p, &inherit, 1);
        pass &= !err && child->fds[17].object == p->fds[a].object;
        proc_discard(child);
    } else pass = false;
    pass &= observed("inherit", 0, err);
    struct ciuki_spawn_fd denied = { d, 16 };
    pass &= observed("inherit-cloexec", -EBADF, proc_fd_validate(p, &denied, 1));
    static struct vfs_table legacy;
    err = vfs_table_init(s->vfs, &legacy, 2);
    if (!err) {
        int share = vfs_open(&legacy, "C:/tmp/f2-share.bin", VFS_READ | VFS_WRITE | VFS_CREATE,
                              VFS_DENY_WRITE, 0);
        pass &= share >= 0;
        if (share >= 0) {
            pass &= observed("share-denied", -EACCES, file_open(p, "/tmp/f2-share.bin", O_WRONLY, 0));
            vfs_close(&legacy, share);
            pass &= !namespace_op(p, 3, "/tmp/f2-share.bin", 0);
        }
        vfs_table_destroy(&legacy);
    } else pass = false;
    const char *invalid[] = { "/bad:colon", "/bad\\slash", "/bad\xf0\x9f\x90\xb6", "missing/../x" };
    const int errors[] = { -EINVAL, -EINVAL, -EILSEQ, -ENOENT };
    struct storage_volume *volume = storage_volume(storage_get(), 2);
    uint64_t writes_before = volume ? volume->writes : 0;
    for (unsigned i = 0; i < ARRAY_SIZE(invalid); i++)
        pass &= observed("invalid-path", errors[i], file_open(p, invalid[i], O_CREAT | O_RDWR, 0666));
    if (volume) pass &= observed("validation-writes", 0, (int64_t)(volume->writes - writes_before));
    pass &= observed("mkdir-a", 0, namespace_op(p, 0, "/tmp/F2Move", 0));
    pass &= observed("mkdir-b", 0, namespace_op(p, 0, "/tmp/F2Dest", 0));
    pass &= observed("mkdir-child", 0, namespace_op(p, 0, "/tmp/F2Move/Child", 0));
    pass &= observed("file-over-dir", -EISDIR, namespace_op(p, 1, "/tmp/f2-kernel-a.bin", "/tmp/F2Move"));
    pass &= observed("dir-over-file", -ENOTDIR, namespace_op(p, 1, "/tmp/F2Move", "/tmp/f2-kernel-a.bin"));
    pass &= observed("descendant", -EINVAL, namespace_op(p, 1, "/tmp/F2Move", "/tmp/F2Move/inside"));
    pass &= observed("chdir", 0, px_chdir(p, "/tmp/F2Move"));
    pass &= observed("pinned-rmdir", -EBUSY, namespace_op(p, 2, "/tmp/F2Move", 0));
    pass &= observed("identical", 0, namespace_op(p, 1, "/tmp/F2Move", "/tmp/F2Move"));
    pass &= observed("case-rename", 0, namespace_op(p, 1, "/tmp/F2Move", "/tmp/f2move"));
    pass &= observed("cross-directory", 0, namespace_op(p, 1, "/tmp/f2move", "/tmp/F2Dest/Moved"));
    char cwd[CIUKI_PATH_MAX];
    fs_lock_take(&s->vfs->lock); err = px_getcwd_locked(p->cwd, cwd); fs_lock_drop(&s->vfs->lock);
    pass &= !err && !strncmp(cwd, "/tmp/F2Dest/Moved", sizeof("/tmp/F2Dest/Moved"));
    rec_emit("fd-table", "DATA", "operation=cwd expected=/tmp/F2Dest/Moved observed=%s", err ? "error" : cwd);
    pass &= !px_chdir(p, "/");
    if (s->vfs->volumes[3]) {
        pass &= observed("exdev", -EXDEV, namespace_op(p, 1, "/tmp/f2-kernel-a.bin", "/mnt/d/f2-cross.bin"));
        if (s->vfs->volumes[3]->readonly)
            pass &= observed("readonly", -EROFS, file_open(p, "/mnt/d/f2-denied.bin", O_CREAT | O_WRONLY, 0666));
        else { rec_emit("fd-table", "ERROR", "case=readonly status=not_run reason=missing_readonly_volume"); pass = false; }
    }
    else { rec_emit("fd-table", "ERROR", "case=exdev status=not_run reason=missing_second_volume"); pass = false; }
    int dir = file_open(p, "/tmp/F2Dest", O_DIRECTORY, 0);
    if (dir < 0) pass = false;
    else {
        struct ciuki_dirent ent; uint64_t cookie = 0; int n;
        struct sha256_ctx names; sha256_init(&names);
        fs_lock_take(&s->vfs->lock);
        while ((n = px_getdents_locked(file_fd(p, dir), &ent)) > 0) {
            pass &= n == 792 && ent.d_reclen == 792 && ent.d_off == (int64_t)++cookie && ent.d_namlen == strlen(ent.d_name);
            sha256_update(&names, ent.d_name, strlen(ent.d_name) + 1);
        }
        pass &= !n && !file_seek_locked(file_fd(p, dir), 0, SEEK_SET, &position);
        pass &= px_getdents_locked(file_fd(p, dir), &ent) == 792 && ent.d_off == 1 && !strncmp(ent.d_name, ".", 2);
        fs_lock_drop(&s->vfs->lock);
        rec_emit("fd-table", "DATA", "operation=getdents records=%llu record_bytes=792 rewind_cookie=%lld", cookie, ent.d_off);
        record_directory_digest(&names);
        file_close(p, dir);
    }
    int replaced = file_open(p, "/tmp/f2-replaced.bin", O_CREAT | O_EXCL | O_RDWR, 0666);
    if (replaced < 0) pass = false;
    else {
        pass &= observed("replace", 0, namespace_op(p, 1, "/tmp/f2-kernel-a.bin", "/tmp/f2-replaced.bin"));
        struct ciuki_stat st; fs_lock_take(&s->vfs->lock);
        pass &= !px_stat_locked(file_fd(p, replaced)->node, &st) && st.st_nlink == 0;
        fs_lock_drop(&s->vfs->lock); file_close(p, replaced);
    }
    pass &= observed("unlink-open", 0, namespace_op(p, 3, "/tmp/f2-replaced.bin", 0));
    struct ciuki_stat st; fs_lock_take(&s->vfs->lock);
    pass &= !px_stat_locked(file_fd(p, a)->node, &st) && !st.st_nlink;
    fs_lock_drop(&s->vfs->lock);
    rec_emit("fd-table", "DATA", "operation=fstat-unlinked inode=%llu links=%u size=%lld", st.st_ino, st.st_nlink, st.st_size);
    while ((result = file_dup(p, a, 0, false, 0)) >= 0) { }
    record_slots(p);
    pass &= observed("emfile", -EMFILE, result);
    pass &= !file_close(p, 73) && observed("fd-reuse", 73, file_dup(p, a, 0, false, 0));
    for (int i = 0; i < CIUKI_OPEN_MAX; i++) if (p->fds[i].object) pass &= !file_close(p, i);
    pass &= !namespace_op(p, 2, "/tmp/F2Dest/Moved/Child", 0);
    pass &= !namespace_op(p, 2, "/tmp/F2Dest/Moved", 0) && !namespace_op(p, 2, "/tmp/F2Dest", 0);
    return pass;
}
static int64_t milliseconds(const struct ciuki_timespec *t)
{
    return t->tv_sec * 1000 + t->tv_nsec / 1000000;
}
static bool durable_cases(struct process *owner)
{
    static const char path[] = "/tmp/f2-durable.bin", content[] = "CiukiOS F2 durable\n";
    int fd = file_open(owner, path, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0) return observed("durable-create", 0, fd);
    bool pass = observed("durable-write", sizeof(content) - 1,
                         kernel_io(owner, fd, (void *)content, sizeof(content) - 1, true, false, 0));
    fs_lock_take(&file_fd(owner, fd)->node->space->vfs->lock);
    int err = file_sync_locked(file_fd(owner, fd));
    fs_lock_drop(&file_fd(owner, fd)->node->space->vfs->lock);
    pass = observed("durable-fsync", 0, err) && pass;
    pass = !file_close(owner, fd) && pass;
    fd = file_open(owner, path, O_RDONLY, 0);
    if (fd < 0) return false;
    char bytes[sizeof(content) - 1] = { 0 };
    pass = observed("durable-reopen", sizeof(bytes), kernel_io(owner, fd, bytes, sizeof(bytes), false, false, 0)) && pass;
    pass = !memcmp(bytes, content, sizeof(bytes)) && pass;
    uint8_t digest[32]; char hex[65];
    sha256(bytes, sizeof(bytes), digest); sha256_hex(digest, hex);
    rec_emit("fd-table", "DATA", "case=durable-file name_hex=2f746d702f66322d64757261626c652e62696e size=%u sha256=%s",
             (unsigned)sizeof(bytes), hex);
    return !file_close(owner, fd) && pass;
}
static bool user_cases(struct process *owner)
{
    struct proc_strings *args = proc_strings_new();
    if (!args) return false;
    int err = proc_strings_add(args, "files", sizeof("files"), false);
    struct ciuki_file image = { .bytes = (uint32_t)(files_payload_end - files_payload_start), .read = payload_read };
    struct process *p = 0;
    if (!err) err = proc_spawn_file(owner, &image, args, 0, 0, 0, 0, &p);
    proc_strings_free(args);
    if (err < 0) return observed("payload-spawn", 0, err);
    struct files_result r = { 0 };
    uint64_t deadline = g_ticks + 30000, signal_at = 0; bool sent = false;
    while (g_ticks < deadline && p->state == PROC_LIVE) {
        if (ua_read(p->memory, &r, FILES_RESULT, sizeof(r))) break;
        if (r.stage && !signal_at) signal_at = g_ticks + 10;
        if (!sent && signal_at && g_ticks >= signal_at) {
            err = proc_signal_thread_kill(p, r.tid, SIGUSR1); sent = !err;
        }
        if (r.done) break;
        task_sleep_ms(1);
    }
    struct clock_seed seed; file_clock_snapshot(&seed);
    struct ciuki_utsname uts = { 0 };
    bool uname_read = p->state == PROC_LIVE && !ua_read(p->memory, &uts, FILES_RESULT + 1280, sizeof(uts));
    int64_t offset = milliseconds(&r.realtime) - milliseconds(&r.monotonic);
    int64_t expected = seed.utc * 1000 - (int64_t)seed.tick;
    int64_t busy = milliseconds(&r.busy_after) - milliseconds(&r.busy_before);
    int64_t sleeping = milliseconds(&r.sleep_cpu_after) - milliseconds(&r.sleep_cpu_before);
    int64_t elapsed = milliseconds(&r.sleep_end) - milliseconds(&r.sleep_begin);
    int64_t remain = milliseconds(&r.remaining);
    bool pass = uname_read && !strncmp(uts.sysname, "CiukiOS", sizeof("CiukiOS")) &&
        !strncmp(uts.nodename, "ciuki", sizeof("ciuki")) &&
        !strncmp(uts.machine, "i686", sizeof("i686")) &&
        uts.abi_version == CIUKI_ABI_VERSION && uts.realtime_source == seed.source &&
        r.done && !r.errors && r.reads == 10000 && !r.decreases && r.io_cases == 1 &&
        offset >= expected - 1 && offset <= expected + 1 && busy > 0 && sleeping >= 0 && sleeping <= 2 &&
        !r.sleep_result && elapsed >= 20 && sent && r.interrupted_result == -EINTR && remain >= 0 && remain <= 20;
    rec_emit("fd-table", "DATA", "case=user checks=%u errors=%u io_cases=%u reads=%u decreases=%u", r.checks, r.errors, r.io_cases, r.reads, r.decreases);
    rec_emit("fd-table", "DATA", "case=clock-source realtime_source=%u source=%s qualified=%u valid=%u utc=%lld sample=%llu", seed.source, seed.source ? "rtc" : "build", seed.qualified, seed.valid, seed.utc, seed.tick);
    rec_emit("fd-table", "DATA", "case=clock-uname abi_version=%u realtime_source=%u clock_source=%u matched=%u",
             uts.abi_version, uts.realtime_source, seed.source, uname_read && uts.realtime_source == seed.source);
    rec_emit("fd-table", "DATA", "case=clock-offset expected_ms=%lld observed_ms=%lld resolution_ns=1000000", expected, offset);
    rec_emit("fd-table", "DATA", "case=cpu-clock busy_ms=%lld sleeping_ms=%lld sleep_ms=%lld scheduling_delay_ms=%lld", busy, sleeping, elapsed, elapsed - 20);
    rec_emit("fd-table", "DATA", "case=sleep-interrupt result=%d remaining_ms=%lld handlers=%u sent=%u", r.interrupted_result, remain, r.signal_entries, sent);
    if (p->state == PROC_LIVE && r.done) {
        uint32_t one = 1, va = FILES_RESULT + offsetof(struct files_result, release);
        ua_write(p->memory, va, &one, sizeof(one));
        struct ww_key key; if (!ww_bind(p->memory, va, &key)) ww_wake(&key, UINT32_MAX);
    } else proc_stop(p, 1, 0);
    deadline = g_ticks + 1000;
    while (p->state != PROC_ZOMBIE && g_ticks < deadline) { task_sleep_ms(1); proc_collect(); }
    if (p->state == PROC_ZOMBIE) { pass &= !p->status; proc_reap(owner, p); }
    else pass = false;
    return pass;
}
#endif

int probe_f2_fd_table(void)
{
    rec_emit("fd-table", "BEGIN", 0);
#ifndef CIUKI_FILES_PAYLOAD_BIN
    rec_emit("fd-table", "ERROR", "status=not_run reason=files_payload_not_wired");
    rec_emit("fd-table", "END", "status=FAIL reason=not_run");
    return 1;
#else
    if (!fat_native_ready || !fat_native_ready() || !fat_rename_replace) {
        rec_emit("fd-table", "ERROR", "status=not_run reason=fat_native_integration_absent");
        rec_emit("fd-table", "END", "status=FAIL reason=not_run"); return 1;
    }
    struct storage *storage = storage_get(); struct px_namespace *space;
    int err = storage_enable_write(storage, 2);
    if (!err) err = files_attach(&storage->vfs, &space);
    if (err) { rec_emit("fd-table", "END", "status=FAIL reason=storage error=%d", err); return 1; }
    struct file_ledger before, after; files_snapshot(space, &before);
    struct process *owner = 0;
    err = proc_prepare(proc_supervisor(), &owner);
    bool pass = !err;
    if (owner) {
        if (owner->cwd && owner->cwd_release) owner->cwd_release(owner->cwd);
        owner->cwd = space->root; px_retain(owner->cwd);
        owner->cwd_retain = px_retain; owner->cwd_release = px_release;
        proc_publish(owner, 0);
        pass = kernel_cases(owner);
        pass &= user_cases(owner);
        pass &= durable_cases(owner);
        proc_stop(owner, 0, 0); proc_collect();
    }
    files_snapshot(space, &after);
    pass &= before.descriptions == after.descriptions && before.pins == after.pins;
    rec_emit("fd-table", "DATA", "case=ledger descriptions_before=%u descriptions_after=%u pins_before=%u pins_after=%u", before.descriptions, after.descriptions, before.pins, after.pins);
    pass &= f2_files_post_commit_fault();
    err = storage_sync();
    pass = !err && pass;
    rec_emit("fd-table", "DATA", "case=durable-checker flush_result=%d checker=host_required", err);
    rec_emit("fd-table", "DATA", "case=stack task=%s size=%u high_water=%u",
             g_current->name, KSTACK_SIZE, task_stack_high_water(g_current));
    if (pass) rec_emit("fd-table", "ARM", "action=durable_shutdown checker=host_required");
    rec_emit("fd-table", "END", pass ? "status=PASS" : "status=FAIL reason=files_contract");
    return pass ? 0 : 1;
#endif
}
CIUKI_F2_PROBE("fd-table", probe_f2_fd_table);
