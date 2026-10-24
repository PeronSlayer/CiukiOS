/* fx-23 (T23 trial of image f441c5bd): foreground file access while the
 * background check of C:\SYSTEM\DOS\UNIFIED.IMG runs, over the production
 * cache/FAT/VFS on a file-backed volume. The trial: the storage writer
 * (P_DEVICE) used 10-23 % CPU during the check and 24-40 % afterwards with
 * 1,200-1,500 device-budget overruns per 2 s; Files waited 19.4 s for its
 * window. Three properties are measured here:
 *  1. a Files-style listing (find, open, read) during the check is served
 *     from the cache: the 27.7 MB scan through a smaller cache must not
 *     evict the foreground working set (drop-behind);
 *  2. the check's reads queue no read-ahead, so the storage writer never
 *     reads on its behalf at device priority;
 *  3. ordinary cache traffic (fills of foreground misses) does not wake the
 *     storage writer: it sleeps on its own event between 10 ms polls.
 * SPDX-License-Identifier: GPL-2.0-only */
#define CIUKI_CPU_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef WIFEXITED
#undef WEXITSTATUS
#undef WIFSIGNALED
#undef WTERMSIG
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t flags) { (void)flags; }
static uint8_t block_memory[16 * 4096] __attribute__((aligned(4096)));
#define P2V(p) ((void *)(block_memory + ((p) - 0x1000000u)))
#include "../../../src/kernel/vm/dosvm_image.c"
#include "../fs/fake.h"
#include <assert.h>
#include <stdarg.h>

#define OK(x) assert((x) == 0)
volatile uint64_t g_ticks;
struct task *g_current;
struct ciuki_boot_info g_boot;
static struct storage storage;
static struct fake disk;
static struct vfs_table table;
void klog(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n'); }
void kpreempt_point(void) { g_ticks++; }
uint64_t deadline_after_ms(uint32_t ms) { return g_ticks + ms; }
void task_sleep_ms(uint32_t ms) { g_ticks += ms; }
struct task *task_create_kernel(const char *n, void (*fn)(void *), void *a, enum task_prio p)
{ (void)n; (void)fn; (void)a; (void)p; assert(!"test mode never creates the task"); return 0; }
void task_start(struct task *t) { (void)t; assert(!"no task"); }
void kwait_init(struct kwait *q) { memset(q, 0, sizeof(*q)); }
void kwait_wake_all(struct kwait *q) { (void)q; }
void kwait_set_owner(struct kwait *q, struct task *t) { q->owner = t; }
bool kwait_wait_until(struct kwait *q, kwait_cond_fn fn, void *arg, uint64_t until)
{ (void)q; (void)until; assert(!"test mode never sleeps"); return fn(arg); }
uint32_t pmm_alloc_contig(uint32_t pages) { assert(pages == 16); return 0x1000000u; }
void pmm_free(uint32_t phys) { (void)phys; }
struct storage *storage_get(void) { return &storage; }
static char digest_sha[65];
bool dosvm_game_digest_find(const struct vfs_file_version *v, char out[65]) { (void)v; (void)out; return false; }
void dosvm_game_digest_publish(struct vfs_table *t, int h, const struct vfs_file_version *v, const char sha[65])
{ (void)t; (void)h; (void)v; memcpy(digest_sha, sha, 65); }

static unsigned listing(unsigned *files)
{
    unsigned before = disk.reads; struct vfs_find find; struct fat_entry entry;
    static uint8_t buffer[16384]; *files = 0;
    int e = vfs_find_first(&table, "C:\\FILES\\*.DAT", 0, &find, &entry);
    while (!e) {
        char path[64]; snprintf(path, sizeof(path), "C:\\FILES\\%s", entry.name);
        int fd = vfs_open(&table, path, VFS_READ, VFS_DENY_WRITE, 0); assert(fd >= 0);
        size_t got = 0; OK(vfs_read(&table, fd, buffer, sizeof(buffer), &got)); assert(got == entry.size);
        OK(vfs_close(&table, fd)); (*files)++;
        e = vfs_find_next(&table, &find, &entry);
    }
    assert(e == -FS_ENOENT);
    return disk.reads - before;
}
static uint64_t now_ms(void) { return fs_now_ms(); }
static volatile int writer_stop; static unsigned writer_passes;
static void *writer_main(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&writer_stop, __ATOMIC_ACQUIRE)) {
        cache_writer_wait(&storage.cache, 10);
        __atomic_add_fetch(&writer_passes, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static unsigned holder_ops[3];
static int hold_work(void) { struct timespec t = {0, 5000000}; nanosleep(&t, NULL); return 0; }
static void *holder_main(void *arg)
{
    unsigned k = (unsigned)(uintptr_t)arg;
    while (!__atomic_load_n(&writer_stop, __ATOMIC_ACQUIRE)) {
        vfs_lock(&storage.vfs, vfs_drive_mask(2));          /* a ticket, as every C: operation */
        (void)VFS_IO(&storage.vfs, hold_work());            /* its work, namespace released */
        vfs_unlock(&storage.vfs);
        holder_ops[k]++;
    }
    return NULL;
}
static pthread_t publisher;
static int published, stamp_written;
static void *publisher_main(void *arg)
{
    (void)arg;
    int fd = vfs_open(&table, "C:\\SYSTEM\\DOS\\UNIFIED.IMG", VFS_READ, VFS_DENY_WRITE, 0); assert(fd >= 0);
    struct vfs_file_version v; OK(vfs_handle_version(&table, fd, &v));
    dosvm_image_publish(&table, &v, digest_sha);
    struct fat_entry e; stamp_written = !vfs_stat(&table, DOSVM_IMAGE_STAMP, &e) && e.size == 128;
    OK(vfs_close(&table, fd));
    __atomic_store_n(&published, 1, __ATOMIC_RELEASE);
    return NULL;
}
int main(int argc, char **argv)
{
    assert(argc == 2 && !fake_open(&disk, argv[1]));
    OK(cache_init(&storage.cache, 2u * 1024u * 1024u));
    OK(cache_adaptive_enable(&storage.cache, 65536));
    struct fat_volume *outer = &storage.volumes[2].fat;
    OK(fat_mount(outer, &storage.cache, &disk.dev, 0, disk.dev.capacity, false));
    OK(fat_enable_write(outer));
    vfs_init(&storage.vfs); OK(vfs_attach(&storage.vfs, 2, outer)); storage.ready = true;
    OK(vfs_table_init(&storage.vfs, &table, 2));
    struct cache_stats stats; cache_stats(&storage.cache, &stats);
    struct fat_entry image; OK(vfs_stat(&table, "C:\\SYSTEM\\DOS\\UNIFIED.IMG", &image));
    assert(image.size > 2u * (stats.data_pages * 4096u));       /* the image is larger than the cache */

    /* 1+2. Files lists and reads its folder (the working set), then lists it
     * again every 40 blocks (2.5 MiB, more than the cache) while the check runs. The storage writer's
     * read-ahead service runs after every block, as on the T23. */
    unsigned files = 0, warm = listing(&files);
    assert(files == 40 && warm && !listing(&files));               /* warm: served from the cache */
    uint64_t prefetch = storage.cache.prefetch_commands;
    dosvm_image_test_mode(); dosvm_image_desktop_presented();
    assert(dosvm_image_step());
    unsigned blocks = 0, listings = 0, listing_reads = 0, served = 0;
    while (dosvm_image_step()) {
        while (cache_prefetch_tick(&storage.cache)) served++;      /* the writer's turn */
        if (!(++blocks % 40)) { listing_reads += listing(&files); listings++; }   /* every 2.5 MiB */
    }
    uint32_t state, progress, error; dosvm_image_query(&state, &progress, &error);
    assert(state == CIUKI_DOSVM_IMAGE_VERIFIED && listings >= 2);
    printf("fx-23 check of %u bytes in %u blocks: %u Files listings read %u sectors from disk; "
           "writer read-ahead commands for the check %llu (served turns %u)\n",
           image.size, iv.blocks, listings, listing_reads,
           (unsigned long long)(storage.cache.prefetch_commands - prefetch), served);
    assert(listing_reads == 0);                                     /* working set kept */
    assert(storage.cache.prefetch_commands == prefetch && !served); /* no read-ahead at device priority */
    assert(!listing(&files));

    /* 3. Foreground misses (5,000 cold sectors) with the writer asleep on
     * its own event: it must wake about once per 10 ms poll, not per fill. */
    pthread_t writer; OK(pthread_create(&writer, NULL, writer_main, NULL));
    struct timespec settle = {0, 30000000}; nanosleep(&settle, NULL);
    unsigned passes = __atomic_load_n(&writer_passes, __ATOMIC_RELAXED), misses = 0;
    uint64_t began = now_ms(), cold = storage.cache.misses;
    uint8_t sector[512];
    uint64_t first = (uint64_t)outer->data_sector + 64u;
    for (uint64_t lba = first; misses < 5000 && lba < disk.dev.capacity; lba += 3) {
        OK(cache_read(&storage.cache, &disk.dev, lba, sector));
        misses = (unsigned)(storage.cache.misses - cold);
    }
    uint64_t elapsed = now_ms() - began;
    unsigned woke = __atomic_load_n(&writer_passes, __ATOMIC_RELAXED) - passes;
    __atomic_store_n(&writer_stop, 1, __ATOMIC_RELEASE); /* its next poll ends it */
    OK(pthread_join(writer, NULL));
    printf("fx-23 writer coupling: %u foreground fills in %llu ms woke the storage writer %u times\n",
           misses, (unsigned long long)elapsed, woke);
    assert(misses >= 5000 && woke <= elapsed / 10u + 10u);

    /* 4. The 50 s CREATE (see the fx-23 report, "50 s CREATE: cause"): a
     * FAT transaction step (here the verified stamp's open/truncate, write
     * and commit, as dosvm_image_publish does in CREATE) waits in
     * transaction_guard_wait until C: has no reservation, no queued ticket
     * and no in-flight reader. Three tasks doing back-to-back short C:
     * operations (5 ms each, the desktop threads of the trial) leave no such
     * instant, so the step waited as long as they ran. With the fx-23 gate,
     * tickets issued after the refused step wait for it. */
    writer_stop = 0;
    pthread_t holders[3];
    for (uintptr_t i = 0; i < 3; i++) OK(pthread_create(&holders[i], NULL, holder_main, (void *)i));
    struct timespec ramp = {0, 50000000}; nanosleep(&ramp, NULL);
    OK(pthread_create(&publisher, NULL, publisher_main, NULL));
    uint64_t start = now_ms();
    while (!__atomic_load_n(&published, __ATOMIC_ACQUIRE) && now_ms() - start < 4000) {
        struct timespec t = {0, 1000000}; nanosleep(&t, NULL);
    }
    uint64_t waited = now_ms() - start;
    bool finished = __atomic_load_n(&published, __ATOMIC_ACQUIRE);
    __atomic_store_n(&writer_stop, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 3; i++) OK(pthread_join(holders[i], NULL));
    OK(pthread_join(publisher, NULL));
    printf("fx-23 guarded step under a stream of C: operations: %s after %llu ms (%u, %u, %u operations meanwhile)\n",
           finished ? "published" : "still waiting", (unsigned long long)waited, holder_ops[0], holder_ops[1], holder_ops[2]);
    assert(finished && waited < 2000);
    assert(stamp_written && holder_ops[0] && holder_ops[1] && holder_ops[2]);

    vfs_table_destroy(&table); OK(vfs_detach(&storage.vfs, 2)); vfs_destroy(&storage.vfs);
    cache_destroy(&storage.cache); fake_close(&disk);
    puts("PASS fx-23 contention: Files served from the cache during the check, no check read-ahead on the writer, writer not woken per fill, guarded step not starved by C: operations");
    return 0;
}
