/* Native namespace over the production F1 storage harness. Only process,
 * scheduler and memory services are faked; storage/path/fd code is linked.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main storage_fixture_main
#include "test_storage.c"
#undef main
#undef CLOCKS_PER_SEC
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
#include <ciuki/files.h>
#include <ciuki/clock.h>
#include <ciuki/desktop.h>
#include <ciuki/supervisor.h>
#include <ciuki/signal.h>

static struct process supervisor = { .state = PROC_LIVE };
struct task *g_current;
struct desktop_ledger desktop_objects;
void *kmalloc(size_t bytes) { return malloc(bytes); }
void *kzalloc(size_t bytes) { return calloc(1, bytes); }
void kfree(void *p) { free(p); }
void panic(const char *fmt, ...) { fprintf(stderr, "unexpected panic: %s\n", fmt); abort(); }
struct process *proc_supervisor(void) { return &supervisor; }
struct proc_thread *proc_thread_for(const struct task *task) { (void)task; return 0; }
bool proc_signal_caught(struct proc_thread *thread) { (void)thread; return false; }
void task_sleep_ms(uint32_t ms) { (void)ms; abort(); }
void proc_set_file_ops(const struct ciuki_file_ops *ops) { (void)ops; }
void supervisor_set_io_ops(const struct supervisor_io_ops *ops) { (void)ops; }
uint32_t pmm_alloc(void) { abort(); }
void pmm_free(uint32_t page) { (void)page; abort(); }
static void *host_page(uint32_t page) { (void)page; abort(); }
#define P2V(page) host_page(page)
#define V2P(ptr) ((void)(ptr), 0u)
bool clock_fat_utc(uint16_t date, uint16_t time, int64_t *seconds)
{ (void)date; (void)time; *seconds = 0; return false; }
int file_clock_now(uint32_t clock, const struct process *p, struct ciuki_timespec *out)
{ (void)clock; (void)p; memset(out, 0, sizeof(*out)); return 0; }
#include "../../../src/kernel/proc/posixpath.c"
#include "../../../src/kernel/proc/fdtable.c"
#include "../../../src/kernel/proc/devnodes.c"

static void namespace_close(void)
{
    for (int fd = 0; fd < CIUKI_OPEN_MAX; fd++)
        if (supervisor.fds[fd].object) OK(file_close(&supervisor, fd));
    px_release(supervisor.cwd); supervisor.cwd = 0;
    kfree(supervisor.fds); supervisor.fds = 0;
}

static struct storage *namespace_start(struct px_namespace **space)
{
    struct storage *s = start();
    OK(files_bootstrap(&s->vfs));
    OK(files_attach(&s->vfs, space));
    return s;
}
static void namespace_stop(struct storage *s, bool restore)
{
    namespace_close(); CHECK(!file_description_count());
    stop(s, false);
    if (restore) reset();
    else bootlog_reset(); /* cold boot loses logger RAM, retaining only disk */
}
static int resolve_cwd(struct px_namespace *space, struct px_node *cwd, const char *path)
{
    struct px_path result;
    fs_lock_take(&space->vfs->lock);
    int err = px_resolve_locked(space, cwd, path, false, &result);
    fs_lock_drop(&space->vfs->lock);
    return err;
}
static void cwd_tests(void)
{
    struct px_namespace *space;
    struct storage *s = namespace_start(&space);
    CHECK(file_open(&supervisor, "/mnt", O_RDONLY | O_DIRECTORY, 0) >= 3);
    CHECK(file_open(&supervisor, "/dev", O_RDONLY | O_DIRECTORY, 0) >= 3);
    OK(storage_add_disk(s, 1, &media[2].dev));
    struct process peer = { .state = PROC_LIVE, .cwd = space->root };
    px_retain(peer.cwd);
    OK(px_chdir(&peer, "/Fixture dir"));
    CHECK(vfs_rmdir(&table, "C:/Fixture dir") == -FS_EBUSY);
    struct process other = { .state = PROC_LIVE, .cwd = space->root };
    px_retain(other.cwd);
    OK(px_chdir(&other, "/mnt/d/Fixture dir"));
    struct px_node *synthetic[] = { space->mnt, space->dev, space->null, space->console };
    for (unsigned i = 0; i < ARRAY_SIZE(synthetic); i++) px_retain(synthetic[i]);
    uint32_t c_generation = s->vfs.generation[2], d_generation = s->vfs.generation[3];
    int result = storage_sync();
    printf("namespace root cwd: refs=%u storage_sync()=%d stopped=%u attached=%u mounted=%u\n",
           space->root->refs, result, s->stopped,
           s->vfs.volumes[2] != 0, s->volumes[2].fat.mounted);
    OK(result); CHECK(s->stopped && !s->vfs.volumes[2] && !s->vfs.volumes[3]);
    CHECK(!s->volumes[2].fat.mounted && !s->volumes[3].fat.mounted);
    CHECK(s->vfs.generation[2] == c_generation + 1 && s->vfs.generation[3] == d_generation + 1);
    unsigned reads = media[0].reads, writes = media[0].writes;
    char cwd[CIUKI_PATH_MAX];
    CHECK(px_getcwd_locked(supervisor.cwd, cwd) == -ENOENT);
    CHECK(px_getcwd_locked(peer.cwd, cwd) == -ENOENT);
    CHECK(px_getcwd_locked(other.cwd, cwd) == -ENOENT);
    CHECK(px_getcwd_locked(space->mnt, cwd) == -ENOENT);
    CHECK(px_getcwd_locked(space->dev, cwd) == -ENOENT);
    const char *paths[] = { ".", "..", "../SHORT.BIN", "Nested.bin", "/", "/dev/null" };
    for (unsigned i = 0; i < ARRAY_SIZE(paths); i++) {
        CHECK(resolve_cwd(space, peer.cwd, paths[i]) == -ENOENT);
        CHECK(resolve_cwd(space, other.cwd, paths[i]) == -ENOENT);
    }
    CHECK(file_open(&supervisor, "new.bin", O_CREAT | O_RDWR, 0666) == -ENOENT);
    CHECK(media[0].reads == reads && media[0].writes == writes);
    OK(storage_sync()); /* shutdown remains idempotent with retained cwd pins */
    px_release(peer.cwd); px_release(other.cwd);
    for (unsigned i = 0; i < ARRAY_SIZE(synthetic); i++) px_release(synthetic[i]);
    namespace_stop(s, true);
    puts("PASS namespace cwd: supervisor/live processes/synthetic pins and stdio permit shutdown; stale cwd/getcwd ENOENT, mutation pins preserved");
}
static void busy_tests(void)
{
    const char *paths[] = { "/SHORT.BIN", "/", "/Fixture dir", "/mnt/d/SHORT.BIN", "/mnt/d" };
    for (unsigned i = 0; i < ARRAY_SIZE(paths); i++) {
        struct px_namespace *space;
        struct storage *s = namespace_start(&space);
        OK(storage_add_disk(s, 1, &media[2].dev));
        OK(storage_enable_write(s, 2));
        int fd = file_open(&supervisor, paths[i], O_RDONLY, 0); CHECK(fd >= 3);
        int copy = file_dup(&supervisor, fd, 0, false, 0); CHECK(copy > fd);
        unsigned drive = file_fd(&supervisor, fd)->node->drive;
        CHECK(vfs_detach(&s->vfs, drive) == -FS_EBUSY);
        uint32_t c_generation = s->vfs.generation[2], d_generation = s->vfs.generation[3];
        unsigned writes = media[0].writes, flushes = media[0].flushes;
        int result = storage_sync();
        CHECK(result == -FS_EBUSY && !s->stopped);
        CHECK(s->vfs.volumes[2] && s->vfs.volumes[3]);
        CHECK(s->volumes[2].fat.mounted && s->volumes[3].fat.mounted);
        CHECK(s->vfs.generation[2] == c_generation && s->vfs.generation[3] == d_generation);
        CHECK(media[0].writes == writes && media[0].flushes == flushes);
        OK(storage_writeback(s, fs_now_ms() + 5001));
        int newfd = file_open(&supervisor, "/Retry.bin", O_CREAT | O_RDWR, 0666); CHECK(newfd >= 3);
        OK(file_close(&supervisor, newfd));
        OK(file_close(&supervisor, fd));
        CHECK(storage_sync() == -FS_EBUSY && !s->stopped);
        /* Model file_description_close's final decrement before it can
         * acquire vfs.lock. The listed description still owns node cleanup. */
        struct file_description *closing = file_fd(&supervisor, copy);
        CHECK(closing->references == 1); closing->references = 0;
        CHECK(storage_sync() == -FS_EBUSY && !s->stopped);
        closing->references = 1;
        OK(file_close(&supervisor, copy));
        int retry = storage_sync(); OK(retry);
        CHECK(s->stopped && !s->vfs.volumes[2] && !s->vfs.volumes[3]);
        printf("PASS namespace native open: path=%s storage_sync()=%d stopped_before_close=0 retry=%d\n", paths[i], result, retry);
        namespace_stop(s, true);
    }
    for (unsigned drive = 2; drive <= 3; drive++) {
        struct px_namespace *space;
        struct storage *s = namespace_start(&space);
        OK(storage_add_disk(s, 1, &media[2].dev));
        int h = vfs_open(&table, drive == 2 ? "C:/SHORT.BIN" : "D:/SHORT.BIN", VFS_READ, VFS_DENY_NONE, 0);
        CHECK(h >= 0);
        int copy = vfs_dup(&table, h, &table, -1); CHECK(copy >= 0);
        CHECK(storage_sync() == -FS_EBUSY && !s->stopped);
        CHECK(s->vfs.volumes[2] && s->vfs.volumes[3]);
        OK(vfs_close(&table, h)); CHECK(storage_sync() == -FS_EBUSY && !s->stopped);
        OK(vfs_close(&table, copy)); OK(storage_sync());
        namespace_stop(s, true);
    }
    puts("PASS namespace legacy opens: all volumes retained on EBUSY, duplicate final close permits retry");
    struct px_namespace *space;
    struct storage *s = namespace_start(&space);
    OK(storage_enable_write(s, 2));
    OK(bootlog_activate(&s->vfs, true, s->sequence));
    bootlog_capture("busy shutdown log\n", 18);
    int fd = file_open(&supervisor, "/SHORT.BIN", O_RDONLY, 0); CHECK(fd >= 3);
    CHECK(storage_sync() == -FS_EBUSY && !s->stopped && s->vfs.volumes[2]);
    struct bootlog_stats log; bootlog_snapshot(&log);
    CHECK(!log.active && !log.error && log.writes);
    OK(file_close(&supervisor, fd)); OK(storage_sync());
    namespace_stop(s, true);
    puts("PASS namespace boot logger: sink drained/closed on EBUSY, storage remains usable and retry succeeds");
}
static void detach_tests(void)
{
    struct px_namespace *space;
    struct storage *s = namespace_start(&space);
    OK(storage_add_disk(s, 1, &media[2].dev));
    OK(px_chdir(&supervisor, "/mnt/d/Fixture dir"));
    struct px_node *old = supervisor.cwd;
    OK(vfs_chdir(&table, "D:/Fixture dir"));
    OK(vfs_detach(&s->vfs, 3));
    CHECK(!strcmp(table.cwd[3], "/") && !table.cwd_cluster[3]);
    CHECK(resolve_cwd(space, old, ".") == -ENOENT);
    CHECK(resolve_cwd(space, old, "..") == -ENOENT);
    OK(resolve_cwd(space, old, "/")); /* absolute paths can select a live root */
    char cwd[CIUKI_PATH_MAX]; CHECK(px_getcwd_locked(old, cwd) == -ENOENT);
    struct storage_volume *v = &s->volumes[3];
    OK(fat_mount(&v->fat, &s->cache, &v->io, 0, v->io.capacity, true));
    OK(vfs_attach(&s->vfs, 3, &v->fat));
    CHECK(resolve_cwd(space, old, "Nested.bin") == -ENOENT);
    CHECK(px_getcwd_locked(old, cwd) == -ENOENT);
    uint64_t ino = old->ino;
    OK(px_chdir(&supervisor, "/mnt/d/Fixture dir"));
    CHECK(((struct px_node *)supervisor.cwd)->ino != ino);
    OK(resolve_cwd(space, supervisor.cwd, "Nested.bin"));
    OK(px_chdir(&supervisor, "/"));
    OK(storage_sync()); namespace_stop(s, true);
    puts("PASS namespace detach: cwd expires, legacy cwd resets, absolute chdir recovers; reattach never revives old identities");
}
static void namespace_probe_tests(void)
{
    int (*probes[])(void) = { probe_fat_write, probe_cache, probe_bootlog };
    const char *names[] = { "fat-write", "cache", "bootlog" };
    for (unsigned i = 0; i < ARRAY_SIZE(probes); i++) {
        /* Workload and cold reopen both keep the boot namespace attached. */
        unsigned boots = i == 1 ? 1 : 2;
        for (unsigned boot = 0; boot < boots; boot++) {
            struct px_namespace *space;
            struct storage *s = namespace_start(&space);
            unsigned writes = media[0].writes;
            int result = probes[i](); OK(result);
            CHECK(s->stopped && !s->vfs.volumes[2] && !s->volumes[2].fat.mounted);
            if (boot) CHECK(media[0].writes == writes);
            printf("PASS namespace production probe: %s boot=%u result=%d stopped=1 attached=0\n", names[i], boot, result);
            namespace_stop(s, boot + 1 == boots);
        }
    }
}
int main(int argc, char **argv)
{
    CHECK(argc == 4);
    for (unsigned i = 0; i < 3; i++) OK(fake_open(&media[i], argv[i + 1]));
    baseline(); media[2].undo_enabled = true;
    cwd_tests(); busy_tests(); detach_tests(); namespace_probe_tests();
    for (unsigned i = 0; i < 3; i++) fake_close(&media[i]);
    printf("STORAGE NAMESPACE RESULT checks=%u records=%u failures=0 ASan/UBSan=enabled\n", checks, records);
    return 0;
}
