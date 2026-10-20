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
#ifndef STORAGE_NAMESPACE_LIBRARY
bool supervisor_output(struct task *t,unsigned stream,const void *b,uint32_t n)
{ (void)t;(void)stream;(void)b;(void)n; return false; }
void proc_launch_volume_read(uint32_t requested,size_t done) { (void)requested;(void)done; }
void serial_write(const char *s,size_t n) { (void)s;(void)n; abort(); }
void console_write(const char *s,size_t n) { (void)s;(void)n; abort(); }
#endif
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

#ifndef STORAGE_NAMESPACE_LIBRARY
static void latency_tests(void);
#endif
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
    vfs_lock(space->vfs,0);
    int err = px_resolve_locked(space, cwd, path, false, &result);
    vfs_unlock(space->vfs);
    return err;
}
static int namespace_stat(struct px_node *node,struct ciuki_stat *out)
{
    vfs_lock(node->space->vfs,0); int e=px_stat_locked(node,out);
    vfs_unlock(node->space->vfs); return e;
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
static void removable_namespace_tests(void)
{
    struct px_namespace *space;struct storage *s=namespace_start(&space);
    OK(storage_add_removable(s,&media[2].dev));CHECK(s->vfs.volumes[4] && s->vfs.drive_flags[4]==VFS_DRIVE_REMOVABLE);
    int fd=file_open(&supervisor,"/mnt/e/SHORT.BIN",O_RDONLY,0);CHECK(fd>=3);
    struct px_node *node=file_fd(&supervisor,fd)->node;px_retain(node);
    struct vfs_page_identity *page;
    OK(file_mapping_identity(file_fd(&supervisor,fd),0,&page));
    uint64_t old_generation=page->key.generation;
    int legacy=vfs_open(&table,"E:/SHORT.BIN",VFS_READ,VFS_DENY_NONE,0);CHECK(legacy>=0);
    unsigned writes=media[2].writes,flushes=media[2].flushes;
    CHECK(storage_remove_removable(s,&media[2].dev,false)==-FS_EBUSY);
    CHECK(media[2].writes==writes && media[2].flushes==flushes);
    OK(storage_remove_removable(s,&media[2].dev,true));CHECK(!s->vfs.volumes[4]);
    CHECK(!page->valid && !page->file->valid && s->volumes[4].fat.completion.incomplete);
    CHECK(s->volumes[4].fat.completion.error==-FS_EIO);
    vfs_page_put(&s->vfs,page);
    struct ciuki_stat stat;CHECK(namespace_stat(node,&stat)==-EIO);
    uint8_t bytes[512];size_t done;CHECK(vfs_read(&table,legacy,bytes,sizeof(bytes),&done)<0);
    CHECK(resolve_cwd(space,node,".")==-ENOENT);
    OK(file_close(&supervisor,fd));OK(vfs_close(&table,legacy));
    CHECK(media[2].writes==writes && media[2].flushes==flushes);
    media[2].dev.quarantined=false;
    OK(storage_add_removable(s,&media[2].dev));CHECK(s->vfs.volumes[5] && !s->vfs.volumes[4]);
    CHECK(namespace_stat(node,&stat)==-EIO); /* expired mount root is retained separately */
    fd=file_open(&supervisor,"/mnt/f/SHORT.BIN",O_RDONLY,0);CHECK(fd>=3);
    OK(file_mapping_identity(file_fd(&supervisor,fd),0,&page));
    CHECK(page->valid && page->key.generation!=old_generation);
    OK(file_close(&supervisor,fd));
    CHECK(storage_remove_removable(s,&media[2].dev,false)==-FS_EBUSY);
    vfs_page_put(&s->vfs,page);
    OK(storage_remove_removable(s,&media[2].dev,false));CHECK(!s->vfs.volumes[5]);
    px_release(node);
    namespace_stop(s,true);
    puts("PASS USB removable native/legacy namespace: E: onward, busy preflight, surprise expiry, stale reads/stat/cwd, no teardown writes, fresh F: mount and safe detach");
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
#ifndef STORAGE_NAMESPACE_LIBRARY
/* f1-79: production native/legacy paths and the actual media devctl boundary. */
static _Thread_local void *media_user;
int copy_from_user(void *out,uint32_t address,uint32_t bytes)
{ if (address!=1 || !media_user) abort(); memcpy(out,media_user,bytes); return 0; }
int copy_to_user(uint32_t address,const void *in,uint32_t bytes)
{ if (address!=1 || !media_user) abort(); memcpy(media_user,in,bytes); return 0; }
static struct process *latency_owners[2];
struct process *proc_at_slot(unsigned slot) { return !slot ? &supervisor : slot<3 ? latency_owners[slot-1] : 0; }
int optical_eject(struct storage *s,unsigned d,uint32_t g) { (void)s;(void)d;(void)g; return -FS_ENOENT; }
int usb_storage_eject(struct storage *s,unsigned d,uint32_t g) { (void)s;(void)d;(void)g; return -FS_ENOENT; }
/* f5-12B2: the CD-audio devctls (media_cd.c) are exercised by ata_test; here only the dispatch edge. */
int media_cd_devctl(struct process *p,struct storage *s,uint32_t op,uint32_t a,uint32_t b)
{ (void)p;(void)s;(void)a;(void)b;(void)op; return -ENOTTY; }
#include "../../../src/kernel/drivers/media_dev.c"

#define LAT_ASSERT(x) do { if (!(x)) { fprintf(stderr,"latency line %d: %s\n",__LINE__,#x); abort(); } } while (0)
struct latency_run {
    struct process slow,fast;
    struct vfs_table slow_table,fast_table;
    int media_fd;
    bool native;
    unsigned done,files,b_calls,c_calls,b_max,c_max,blockers_seen;
};
static void latency_record(uint32_t start,unsigned *count,unsigned *max)
{
    unsigned ms=fs_now_ms()-start; (*count)++; if (ms>*max) *max=ms;
    LAT_ASSERT(ms<50);
}
static int native_next(struct file_description *d,struct ciuki_dirent *entry)
{
    vfs_lock(d->node->space->vfs,0); int e=px_getdents_locked(d,entry);
    vfs_unlock(d->node->space->vfs); return e;
}
static int native_read(struct file_description *d,void *buf,size_t *done)
{
    vfs_lock(d->node->space->vfs,0); int e=file_io_locked(d,buf,512,false,false,0,done);
    vfs_unlock(d->node->space->vfs); return e;
}
static int native_stat(struct process *p,const char *path,struct ciuki_stat *st)
{
    struct px_node *cwd=p->cwd; struct px_path r;
    vfs_lock(cwd->space->vfs,0); int e=px_resolve_locked(cwd->space,cwd,path,false,&r);
    if (!e) e=px_stat_locked(r.node,st);
    vfs_unlock(cwd->space->vfs); return e;
}
static void *latency_slow(void *arg)
{
    struct latency_run *r=arg; uint8_t bytes[512]; size_t done;
    if (r->native) {
        int dir=file_open(&r->slow,"/mnt/e/Scan",O_RDONLY|O_DIRECTORY,0); LAT_ASSERT(dir>=0);
        struct ciuki_dirent ent; int e;
        while ((e=native_next(file_fd(&r->slow,dir),&ent))>0) {
            if (ent.d_name[0]=='.') continue;
            char path[128]; snprintf(path,sizeof(path),"/mnt/e/Scan/%s",ent.d_name);
            struct ciuki_stat st; LAT_ASSERT(!native_stat(&r->slow,path,&st) && st.st_size==512);
            int fd=file_open(&r->slow,path,O_RDONLY,0); LAT_ASSERT(fd>=0);
            LAT_ASSERT(!native_read(file_fd(&r->slow,fd),bytes,&done) && done==512 && bytes[0]==0x79);
            LAT_ASSERT(!file_close(&r->slow,fd)); r->files++;
        }
        LAT_ASSERT(!e && !file_close(&r->slow,dir));
    } else {
        struct vfs_find find; struct fat_entry ent;
        int e=vfs_find_first(&r->slow_table,"E:/Scan/*",0,&find,&ent);
        while (!e) {
            if (ent.name[0]!='.') {
                char path[128]; snprintf(path,sizeof(path),"E:/Scan/%s",ent.name);
                struct fat_entry st; LAT_ASSERT(!vfs_stat(&r->slow_table,path,&st) && st.size==512);
                int fd=vfs_open(&r->slow_table,path,VFS_READ,0,0); LAT_ASSERT(fd>=0);
                LAT_ASSERT(!vfs_read(&r->slow_table,fd,bytes,512,&done) && done==512 && bytes[0]==0x79);
                LAT_ASSERT(!vfs_close(&r->slow_table,fd)); r->files++;
            }
            e=vfs_find_next(&r->slow_table,&find,&ent);
        }
        LAT_ASSERT(e==-FS_ENOENT);
    }
    __atomic_store_n(&r->done,1,__ATOMIC_RELEASE); return 0;
}
static void *latency_fast(void *arg)
{
    struct latency_run *r=arg; uint8_t bytes[512]; size_t done;
    while (!__atomic_load_n(&media[2].in_read,__ATOMIC_ACQUIRE)) { struct timespec t={0,1000000}; nanosleep(&t,0); }
    do {
        uint32_t start=fs_now_ms();
        if (r->native) {
            int dir=file_open(&r->fast,"/Fixture dir",O_RDONLY|O_DIRECTORY,0); LAT_ASSERT(dir>=0);
            latency_record(start,&r->b_calls,&r->b_max);
            struct ciuki_dirent ent; int e;
            do { start=fs_now_ms(); e=native_next(file_fd(&r->fast,dir),&ent); latency_record(start,&r->b_calls,&r->b_max); } while(e>0);
            LAT_ASSERT(!e); start=fs_now_ms(); LAT_ASSERT(!file_close(&r->fast,dir)); latency_record(start,&r->b_calls,&r->b_max);
            start=fs_now_ms(); int fd=file_open(&r->fast,"/SHORT.BIN",O_RDONLY,0); LAT_ASSERT(fd>=0); latency_record(start,&r->b_calls,&r->b_max);
            start=fs_now_ms(); LAT_ASSERT(!native_read(file_fd(&r->fast,fd),bytes,&done) && done==512); latency_record(start,&r->b_calls,&r->b_max);
            start=fs_now_ms(); LAT_ASSERT(!file_close(&r->fast,fd)); latency_record(start,&r->b_calls,&r->b_max);
        } else {
            struct vfs_find find; struct fat_entry ent; int e=vfs_find_first(&r->fast_table,"C:/Fixture dir/*",0,&find,&ent);
            latency_record(start,&r->b_calls,&r->b_max);
            while (!e) { start=fs_now_ms(); e=vfs_find_next(&r->fast_table,&find,&ent); latency_record(start,&r->b_calls,&r->b_max); }
            LAT_ASSERT(e==-FS_ENOENT);
            start=fs_now_ms(); int fd=vfs_open(&r->fast_table,"C:/SHORT.BIN",VFS_READ,0,0); LAT_ASSERT(fd>=0); latency_record(start,&r->b_calls,&r->b_max);
            start=fs_now_ms(); LAT_ASSERT(!vfs_read(&r->fast_table,fd,bytes,512,&done) && done==512); latency_record(start,&r->b_calls,&r->b_max);
            start=fs_now_ms(); LAT_ASSERT(!vfs_close(&r->fast_table,fd)); latency_record(start,&r->b_calls,&r->b_max);
        }
        struct timespec t={0,1000000}; nanosleep(&t,0);
    } while (!__atomic_load_n(&r->done,__ATOMIC_ACQUIRE));
    return 0;
}
static void *latency_media(void *arg)
{
    struct latency_run *r=arg;
    while (!__atomic_load_n(&media[2].in_read,__ATOMIC_ACQUIRE)) { struct timespec t={0,1000000}; nanosleep(&t,0); }
    do {
        struct ciuki_media_volume v={.version=CIUKI_MEDIA_VERSION,.size=sizeof(v),.drive=4};
        media_user=&v; uint32_t start=fs_now_ms();
        LAT_ASSERT(!media_devctl(&supervisor,r->media_fd,MEDIA_GET_VOLUME,1,sizeof(v)));
        latency_record(start,&r->c_calls,&r->c_max);
        LAT_ASSERT(v.flags==CIUKI_MEDIA_REMOVABLE && v.generation);
        media_user=&v; LAT_ASSERT(media_devctl(&supervisor,r->media_fd,MEDIA_CD_TOC,1,sizeof(v))==-ENOTTY); /* f5-12B2 dispatch edge */
        v=(struct ciuki_media_volume){.version=CIUKI_MEDIA_VERSION,.size=sizeof(v),.drive=3};
        start=fs_now_ms(); LAT_ASSERT(!media_devctl(&supervisor,r->media_fd,MEDIA_GET_VOLUME,1,sizeof(v)));
        latency_record(start,&r->c_calls,&r->c_max); LAT_ASSERT(v.flags==CIUKI_MEDIA_OPTICAL);
        struct ciuki_media_state st={.version=CIUKI_MEDIA_VERSION,.size=sizeof(st)};
        media_user=&st; start=fs_now_ms();
        LAT_ASSERT(!media_devctl(&supervisor,r->media_fd,MEDIA_GET_STATE,1,sizeof(st)));
        latency_record(start,&r->c_calls,&r->c_max); LAT_ASSERT(st.removable_mask&(1u<<4));
        union { struct ciuki_media_blockers out; struct ciuki_media_remove in; } blockers;
        memset(&blockers,0,sizeof(blockers));
        blockers.in=(struct ciuki_media_remove){.version=CIUKI_MEDIA_VERSION,.size=sizeof(blockers.out),.drive=4,.generation=media[2].dev.media_generation};
        media_user=&blockers; start=fs_now_ms();
        LAT_ASSERT(!media_devctl(&supervisor,r->media_fd,MEDIA_GET_BLOCKERS,1,sizeof(blockers.out)));
        latency_record(start,&r->c_calls,&r->c_max); r->blockers_seen+=blockers.out.count;
        struct timespec t={0,1000000}; nanosleep(&t,0);
    } while (!__atomic_load_n(&r->done,__ATOMIC_ACQUIRE));
    return 0;
}
static unsigned observed_volume_waits,observed_namespace_waits;
void vfs_wait_observer(char drive,uint32_t ms,char holder,const char *reason)
{
    LAT_ASSERT(ms>100);
    if (!strcmp(reason,"volume")) { LAT_ASSERT(drive=='E' && holder=='E'); __atomic_add_fetch(&observed_volume_waits,1,__ATOMIC_RELAXED); }
    else { LAT_ASSERT(!strcmp(reason,"namespace")); __atomic_add_fetch(&observed_namespace_waits,1,__ATOMIC_RELAXED); }
}
struct reservation_waiter { struct vfs *v; unsigned queued,acquired; bool volume; };
static void *reservation_wait(void *arg)
{
    struct reservation_waiter *w=arg;
    if (w->volume) {
        vfs_lock(w->v,0); __atomic_store_n(&w->queued,1,__ATOMIC_RELEASE);
        vfs_reserve_locked(w->v,vfs_drive_mask(4));
    } else {
        __atomic_store_n(&w->queued,1,__ATOMIC_RELEASE); vfs_lock(w->v,0);
    }
    __atomic_store_n(&w->acquired,1,__ATOMIC_RELEASE); vfs_unlock(w->v); return 0;
}
static void reservation_tests(struct vfs *v)
{
    for (unsigned volume=0;volume<2;volume++) {
        struct reservation_waiter w={.v=v,.volume=volume}; pthread_t thread;
        vfs_lock(v,vfs_drive_mask(4));
        struct vfs_io_state mask={0}; if (volume) mask=vfs_io_begin_locked(v);
        LAT_ASSERT(!pthread_create(&thread,0,reservation_wait,&w));
        while (!__atomic_load_n(&w.queued,__ATOMIC_ACQUIRE)) { struct timespec t={0,1000000}; nanosleep(&t,0); }
        struct timespec t={0,150000000}; nanosleep(&t,0);
        if (volume) vfs_io_end_locked(v,mask);
        vfs_unlock(v);
        if (volume) {
            vfs_lock(v,vfs_drive_mask(4)); LAT_ASSERT(__atomic_load_n(&w.acquired,__ATOMIC_ACQUIRE)); vfs_unlock(v);
        }
        LAT_ASSERT(!pthread_join(thread,0));
    }
    LAT_ASSERT(observed_volume_waits && observed_namespace_waits);
    puts("PASS reservation FIFO: queued volume waiter precedes immediate reacquisition; namespace/volume waits >100ms observed");
}

struct relative_change { int result; };
static void *relative_chdir(void *arg)
{ ((struct relative_change *)arg)->result=vfs_chdir(&table,"Scan"); return 0; }
static void relative_drive_test(struct storage *s)
{
    OK(vfs_set_drive(&table,4)); cache_invalidate(&s->cache,&s->volumes[4].io,true);
    media[2].read_delay_ms=25;
    struct relative_change change={.result=-1}; pthread_t thread;
    CHECK(!pthread_create(&thread,0,relative_chdir,&change));
    uint32_t start=fs_now_ms();
    while (!__atomic_load_n(&media[2].in_read,__ATOMIC_ACQUIRE)) {
        CHECK(fs_now_ms()-start<2000); struct timespec t={0,1000000}; nanosleep(&t,0);
    }
    start=fs_now_ms(); OK(vfs_set_drive(&table,2)); CHECK(fs_now_ms()-start<50);
    CHECK(!pthread_join(thread,0) && !change.result);
    CHECK(table.drive==2 && !strcmp(table.cwd[4],"/Scan") && !strcmp(table.cwd[2],"/"));
    media[2].read_delay_ms=0;
    puts("PASS relative path identity: changing default E: to C: during slow lookup preserves the operation's E: cwd target");
}

/* Actual cache writeback blocked in a device callback while shutdown starts. */
static pthread_mutex_t writer_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t writer_changed=PTHREAD_COND_INITIALIZER;
static bool writer_entered,writer_release;
void fs_assert_io_unlocked(void);
static int blocked_writer_error(struct blkdev *d,uint64_t lba,uint32_t n,const void *buf)
{
    (void)d;(void)lba;(void)n;(void)buf; fs_assert_io_unlocked();
    pthread_mutex_lock(&writer_mutex); writer_entered=true; pthread_cond_broadcast(&writer_changed);
    while (!writer_release) pthread_cond_wait(&writer_changed,&writer_mutex);
    pthread_mutex_unlock(&writer_mutex); return -FS_EIO;
}
struct writer_call { struct storage *s; int result; bool shutdown,done; };
static void *writer_call_run(void *arg)
{
    struct writer_call *c=arg;
    c->result=c->shutdown ? storage_shutdown(c->s) : storage_writeback(c->s,fs_now_ms()+6000);
    __atomic_store_n(&c->done,true,__ATOMIC_RELEASE);
    return 0;
}

static void healthy_writer_reservation_test(void)
{
    struct px_namespace *space; struct storage *s=namespace_start(&space); OK(storage_enable_write(s,2));
    /* Keep the real C: reservation while the service drains a batch. This
     * is also the reservation a cache_write throttler must retain. */
    vfs_lock(&s->vfs,vfs_drive_mask(2)); struct vfs_io_state io=vfs_io_begin_locked(&s->vfs);
    uint8_t b[512];
    for (unsigned i=0;i<100;i++) {
        OK(cache_read(&s->cache,&s->volumes[2].io,i,b));
        OK(cache_write(&s->cache,&s->volumes[2].io,i,b));
    }
    writeback_progress_record[0]=0;
    struct writer_call c={.s=s}; pthread_t w; CHECK(!pthread_create(&w,0,writer_call_run,&c));
    uint32_t start=fs_now_ms();
    while (!__atomic_load_n(&c.done,__ATOMIC_ACQUIRE)) {
        CHECK(fs_now_ms()-start<2000); struct timespec t={0,1000000}; nanosleep(&t,0);
    }
    CHECK(!pthread_join(w,0) && !c.result && !s->writer_running);
    CHECK(strstr(writeback_progress_record,"dirty_bytes=18432 budget_bytes=1048576 throttle_bytes=2097152"));
    CHECK(strstr(writeback_progress_record,"flushed_bytes=32768") && strstr(writeback_progress_record,"cache_sync_evictions=0"));
    CHECK(strstr(writeback_progress_record,"writer_waits=0 writer_wait_ms=0"));
    vfs_io_end_locked(&s->vfs,io); vfs_unlock(&s->vfs);
    namespace_stop(s,true);
    puts("PASS F1-82 service: healthy C reservation retained across writeback, protected progress record includes budgets/flush/fallback/wait counters");
}
static void writer_shutdown_test(void)
{
    struct px_namespace *space; struct storage *s=namespace_start(&space);
    uint8_t sector[512]; OK(cache_read(&s->cache,&s->volumes[2].io,0,sector));
    OK(cache_write(&s->cache,&s->volumes[2].io,0,sector));
    int (*saved)(struct blkdev *,uint64_t,uint32_t,const void *)=media[0].dev.write;
    media[0].dev.write=blocked_writer_error; writer_entered=writer_release=false;
    struct writer_call write={.s=s},shutdown={.s=s,.shutdown=true}; pthread_t a,b;
    CHECK(!pthread_create(&a,0,writer_call_run,&write));
    pthread_mutex_lock(&writer_mutex);
    while (!writer_entered) pthread_cond_wait(&writer_changed,&writer_mutex);
    pthread_mutex_unlock(&writer_mutex);
    CHECK(!pthread_create(&b,0,writer_call_run,&shutdown));
    uint32_t start=fs_now_ms();
    for (;;) {
        uint32_t call=fs_now_ms(); vfs_lock(&s->vfs,0); CHECK(fs_now_ms()-call<50);
        bool waiting=s->writer_quiescing;
        CHECK(s->writer_running && !s->stopped); vfs_unlock(&s->vfs);
        if (waiting) break;
        CHECK(fs_now_ms()-start<2000); struct timespec t={0,1000000}; nanosleep(&t,0);
    }
    pthread_mutex_lock(&writer_mutex); writer_release=true; pthread_cond_broadcast(&writer_changed); pthread_mutex_unlock(&writer_mutex);
    CHECK(!pthread_join(a,0) && !pthread_join(b,0)); media[0].dev.write=saved;
    CHECK(write.result==-FS_EIO && shutdown.result==-FS_EIO && !s->writer_error);
    CHECK(cache_error(&s->cache,&s->volumes[2].io)==-FS_EIO);
    CHECK(s->stopped && !s->writer_running && !s->writer_quiescing);
    namespace_stop(s,true);
    puts("PASS shutdown/writeback race: namespace remains available, writer drains before shutdown preflight, delayed EIO remains sticky");
}

static int forbidden_media_check(struct blkdev *d) { (void)d; abort(); }
static void latency_tests(void)
{
    struct px_namespace *space; struct storage *s=namespace_start(&space);
    /* Seed a dedicated directory on the existing disposable FAT16 fixture.
     * No OS image, whole-image copy or variant is involved. */
    struct fat_volume seed; OK(fat_mount(&seed,&s->cache,&media[2].dev,0,media[2].dev.capacity,true));
    OK(vfs_attach(&s->vfs,3,&seed)); OK(vfs_mkdir(&table,"D:/Scan",0));
    uint8_t data[512]; memset(data,0x79,sizeof(data)); size_t done;
    for (unsigned i=0;i<64;i++) {
        char path[64]; snprintf(path,sizeof(path),"D:/Scan/File%02u.bin",i);
        int fd=vfs_open(&table,path,VFS_CREATE|VFS_WRITE,0,0); CHECK(fd>=0);
        OK(vfs_write(&table,fd,data,sizeof(data),&done)); CHECK(done==sizeof(data)); OK(vfs_close(&table,fd));
    }
    OK(vfs_detach(&s->vfs,3)); cache_invalidate(&s->cache,&media[2].dev,true);
    media[2].dev.media_generation=79;
    OK(storage_add_removable(s,&media[2].dev)); CHECK(s->vfs.volumes[4]);
    /* Snapshot queries must never call even the generation callback: it may
     * issue sleepable readiness commands. This published optical model traps
     * any such callback while sharing the real media marshalling path. */
    struct blkdev optical={.media_generation=79,.sector_size=2048,.media_check=forbidden_media_check};
    s->optical[0]=(struct fs_ro_volume){.ops=&iso9660_ops,.cache=&s->cache,.dev=&optical,.mounted=true,.mount_generation=79};
    OK(vfs_attach_ro(&s->vfs,3,&s->optical[0]));
    reservation_tests(&s->vfs); relative_drive_test(s);
    int control=file_open(&supervisor,"/dev/media",O_RDONLY,0); CHECK(control>=0);
    for (unsigned native=0;native<2;native++) {
        struct latency_run r={.native=native,.media_fd=control};
        r.slow=(struct process){.state=PROC_LIVE,.cwd=space->root,.fds=calloc(CIUKI_OPEN_MAX,sizeof(struct proc_fd))};
        r.fast=(struct process){.state=PROC_LIVE,.cwd=space->root,.fds=calloc(CIUKI_OPEN_MAX,sizeof(struct proc_fd))};
        latency_owners[0]=&r.slow; latency_owners[1]=&r.fast;
        CHECK(r.slow.fds && r.fast.fds); px_retain(r.slow.cwd); px_retain(r.fast.cwd);
        OK(vfs_table_init(&s->vfs,&r.slow_table,4)); OK(vfs_table_init(&s->vfs,&r.fast_table,2));
        cache_invalidate(&s->cache,&s->volumes[4].io,true); cache_invalidate(&s->cache,&s->volumes[2].io,true);
        media[2].read_delay_ms=25; media[2].assert_io_unlocked=true; media[0].assert_io_unlocked=true;
        pthread_t a,b,c; CHECK(!pthread_create(&a,0,latency_slow,&r)); CHECK(!pthread_create(&b,0,latency_fast,&r)); CHECK(!pthread_create(&c,0,latency_media,&r));
        CHECK(!pthread_join(a,0) && !pthread_join(b,0) && !pthread_join(c,0));
        CHECK(r.files==64 && r.b_calls>100 && r.c_calls>100); CHECK(!native || r.blockers_seen);
        printf("PASS two-volume latency %s: files=%u delay=25ms B_calls=%u B_max=%ums C_calls=%u C_max=%ums limit<50ms\n",native ? "native" : "legacy",r.files,r.b_calls,r.b_max,r.c_calls,r.c_max);
        media[2].read_delay_ms=0; vfs_table_destroy(&r.slow_table); vfs_table_destroy(&r.fast_table);
        latency_owners[0]=latency_owners[1]=0;
        px_release(r.slow.cwd); px_release(r.fast.cwd); free(r.slow.fds); free(r.fast.fds);
    }
    OK(file_close(&supervisor,control));
    CHECK(storage_remove_removable(s,&media[2].dev,false)==-FS_EBUSY); /* retained E:/Scan cwd */
    OK(vfs_chdir(&table,"E:/"));
    OK(storage_remove_removable(s,&media[2].dev,false));
    OK(vfs_detach(&s->vfs,3));
    media[2].dev.media_generation=0;
    namespace_stop(s,true); fake_reset(&media[2]);
}

int main(int argc, char **argv)
{
    setvbuf(stdout,0,_IOLBF,0);
    CHECK(argc == 4);
    for (unsigned i = 0; i < 3; i++) OK(fake_open(&media[i], argv[i + 1]));
    baseline(); media[2].undo_enabled = true;
    latency_tests(); healthy_writer_reservation_test(); writer_shutdown_test(); cwd_tests(); busy_tests(); removable_namespace_tests(); detach_tests(); namespace_probe_tests();
    for (unsigned i = 0; i < 3; i++) fake_close(&media[i]);
    printf("STORAGE NAMESPACE RESULT checks=%u records=%u failures=0 ASan/UBSan=enabled\n", checks, records);
    return 0;
}
#endif
