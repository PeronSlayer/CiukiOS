/* fx-4b: production background verifier and verified stamp (dosvm_image.c)
 * over the production FAT/VFS on a file-backed outer volume. The test drives
 * the verifier's steps itself (no scheduler); the launch side waits through
 * dosvm_image_lookup exactly as dosvm_disk.c base_identity does.
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
static unsigned block_allocs, block_frees;
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
static char last_record[256];
static unsigned records, mismatch_records, stamp_errors;
void klog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(last_record, sizeof(last_record), fmt, ap); va_end(ap);
    assert(n > 0 && n < (int)sizeof(last_record));
    records++;
    if (strstr(last_record, "result=mismatch")) mismatch_records++;
    if (strstr(last_record, "stamp write")) stamp_errors++;
    printf("%s\n", last_record);
}
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
uint32_t pmm_alloc_contig(uint32_t pages)
{ assert(pages == 16 && block_allocs == block_frees); block_allocs++; return 0x1000000u; }
void pmm_free(uint32_t phys) { assert(phys >= 0x1000000u && phys < 0x1010000u); if (phys == 0x1000000u) block_frees++; }
struct storage *storage_get(void) { return &storage; }
/* The session digest cache of dosvm_disk.c, reduced to its contract. */
static struct vfs_file_version digest_version;
static char digest_sha[65];
static unsigned digest_publishes;
bool dosvm_game_digest_find(const struct vfs_file_version *v, char out[65])
{
    if (!digest_sha[0] || !vfs_file_version_equal(v, &digest_version)) return false;
    memcpy(out, digest_sha, 65); return true;
}
void dosvm_game_digest_publish(struct vfs_table *t, int h, const struct vfs_file_version *v, const char sha[65])
{ (void)t; (void)h; digest_version = *v; memcpy(digest_sha, sha, 65); digest_publishes++; }

static uint32_t image_size;
static char good_sha[65];
static void mount_volume(void)
{
    OK(cache_init(&storage.cache, 256 * 1024));
    struct fat_volume *outer = &storage.volumes[2].fat;
    OK(fat_mount(outer, &storage.cache, &disk.dev, 0, disk.dev.capacity, false));
    OK(fat_enable_write(outer));
    vfs_init(&storage.vfs); OK(vfs_attach(&storage.vfs, 2, outer)); storage.ready = true;
    OK(vfs_table_init(&storage.vfs, &table, 2));
}
static void reboot(void)
{
    vfs_table_destroy(&table); OK(vfs_detach(&storage.vfs, 2)); vfs_destroy(&storage.vfs);
    cache_destroy(&storage.cache); storage.volumes[2].fat.dirty_recovered = false;
    mount_volume();
    digest_sha[0] = 0;                 /* the session cache dies with the boot */
    dosvm_image_test_mode();
}
static void hex(const uint8_t raw[32], char out[65]) { sha256_hex(raw, out); }
static void file_sha(const char *path, char out[65], uint32_t *size)
{
    int fd = vfs_open(&table, path, VFS_READ, VFS_DENY_WRITE, 0); assert(fd >= 0);
    static uint8_t buffer[65536]; struct sha256_ctx c; sha256_init(&c);
    uint32_t total = 0;
    for (;;) { size_t got = 0; OK(vfs_read(&table, fd, buffer, sizeof(buffer), &got)); if (!got) break; sha256_update(&c, buffer, got); total += (uint32_t)got; }
    OK(vfs_close(&table, fd)); uint8_t raw[32]; sha256_final(&c, raw); hex(raw, out); if (size) *size = total;
}
static void write_at(const char *path, uint64_t at, const void *data, size_t n, bool truncate)
{
    int fd = vfs_open(&table, path, VFS_WRITE | (truncate ? VFS_CREATE | VFS_TRUNCATE : 0), VFS_DENY_ALL, 0); assert(fd >= 0);
    uint64_t position; size_t done = 0;
    if (!truncate) { OK(vfs_seek(&table, fd, (int64_t)at, VFS_SEEK_SET, &position)); assert(position == at); }
    OK(vfs_write(&table, fd, data, n, &done)); assert(done == n); OK(vfs_commit(&table, fd)); OK(vfs_close(&table, fd));
}
static void set_sidecar(const char sha[65])
{ char text[65]; memcpy(text, sha, 64); text[64] = '\n'; write_at("C:\\SYSTEM\\DOS\\UNIFIED.SHA", 0, text, 65, true); }
static void set_mtime(uint16_t date, uint16_t time)
{
    struct fat_entry e; OK(vfs_stat(&table, "C:\\SYSTEM\\DOS\\UNIFIED.IMG", &e));
    struct fat_times times = e.times; times.write_date = date; times.write_time = time;
    OK(vfs_times(&table, "C:\\SYSTEM\\DOS\\UNIFIED.IMG", &times));
}
static struct vfs_file_version launch_version(int *fd)
{
    *fd = vfs_open(&table, "C:\\SYSTEM\\DOS\\UNIFIED.IMG", VFS_READ, VFS_DENY_WRITE, 0); assert(*fd >= 0);
    struct vfs_file_version v; OK(vfs_handle_version(&table, *fd, &v)); return v;
}
static unsigned run_to_end(void) { unsigned steps = 0; while (dosvm_image_step()) steps++; return steps; }
static unsigned expected_blocks(void) { return (image_size + 65535u) / 65536u; }
static bool stamp_present(void) { struct fat_entry e; return !vfs_stat(&table, DOSVM_IMAGE_STAMP, &e) && e.size == 128; }
static int cancel_calls, cancel_at;
static bool cancel_after(void *arg) { (void)arg; return ++cancel_calls >= cancel_at; }

int main(int argc, char **argv)
{
    assert(argc == 2 && !fake_open(&disk, argv[1]));
    mount_volume();
    file_sha("C:\\SYSTEM\\DOS\\UNIFIED.IMG", good_sha, &image_size);
    dosvm_image_test_mode();
    uint32_t state, progress, error;

    /* 1. First boot: nothing verified; the desktop frame arms the check. */
    dosvm_image_query(&state, &progress, &error); assert(state == CIUKI_DOSVM_IMAGE_UNCHECKED);
    dosvm_image_desktop_presented();
    dosvm_image_query(&state, &progress, &error); assert(state == CIUKI_DOSVM_IMAGE_VERIFYING && !progress);
    assert(dosvm_image_step());                         /* begin: no stamp, hash */
    unsigned reads = disk.reads, steps = 0, mid = 0;
    while (dosvm_image_step()) {
        steps++;
        dosvm_image_query(&state, &progress, &error);
        assert(state == CIUKI_DOSVM_IMAGE_VERIFYING && progress < 1000);
        if (progress > 400 && progress < 600) mid = progress;
    }
    dosvm_image_query(&state, &progress, &error);
    assert(state == CIUKI_DOSVM_IMAGE_VERIFIED && progress == 1000 && !error && mid);
    assert(iv.blocks == expected_blocks() && steps == expected_blocks() - 1);
    assert(!strcmp(iv.sha, good_sha) && !strcmp(digest_sha, good_sha) && stamp_present());
    assert(strstr(last_record, "result=verified source=hash") && block_allocs == block_frees);
    unsigned hash_reads = disk.reads - reads;
    printf("fx-4b first boot: hashed %u bytes in %u blocks (%u disk reads), stamp written, mid progress %u/1000\n",
           image_size, iv.blocks, hash_reads, mid);

    /* 2. Later boot, unchanged identity and sidecar: the stamp is reused. */
    reboot(); dosvm_image_desktop_presented();
    reads = disk.reads;
    assert(!dosvm_image_step());                        /* begin finishes from the stamp */
    dosvm_image_query(&state, &progress, &error);
    assert(state == CIUKI_DOSVM_IMAGE_VERIFIED && progress == 1000 && !iv.blocks);
    assert(!strcmp(digest_sha, good_sha) && strstr(last_record, "result=verified source=stamp"));
    unsigned stamp_reads = disk.reads - reads;
    assert(stamp_reads * 8 < hash_reads);              /* metadata only, cold cache */
    /* A launch on a fresh boot before any background check: the stamp. */
    reboot();
    int fd; struct vfs_file_version v = launch_version(&fd);
    char sha[65]; const char *source = 0;
    OK(dosvm_image_lookup(&table, &v, sha, &source, 0, 0));
    assert(!strcmp(source, "stamp") && !strcmp(sha, good_sha)); OK(vfs_close(&table, fd));
    printf("fx-4b stamp reuse: no hash, %u disk reads (cold metadata) instead of %u\n", stamp_reads, hash_reads);

    /* 3. Identity changes force a new hash: write time, then a pending
     * removal marker, then a volume recovered dirty, then the sidecar. */
    static const char *const causes[] = {"write time", "pending removal", "dirty volume", "sidecar"};
    for (unsigned cause = 0; cause < 4; cause++) {
        reboot();
        if (cause == 0) set_mtime(0x5b57, 0x6a20);
        if (cause == 1) write_at("C:\\SYSTEM\\REMOVE2.PND", 0, "", 0, true);
        if (cause == 2) storage.volumes[2].fat.dirty_recovered = true;
        if (cause == 3) { char other[65]; memset(other, 'a', 64); other[64] = 0; set_sidecar(other); }
        dosvm_image_desktop_presented();
        assert(dosvm_image_step()); run_to_end();
        dosvm_image_query(&state, &progress, &error);
        assert(iv.blocks == expected_blocks() && !strcmp(iv.sha, good_sha));
        assert(state == (cause == 3 ? CIUKI_DOSVM_IMAGE_MISMATCH : CIUKI_DOSVM_IMAGE_VERIFIED));
        printf("fx-4b identity change (%s): re-hashed %u blocks, state=%u\n", causes[cause], iv.blocks, state);
        if (cause == 1) OK(vfs_delete(&table, "C:\\SYSTEM\\REMOVE2.PND"));
        if (cause == 3) set_sidecar(good_sha);
    }
    /* The restored sidecar: the mismatch was never stamped; the stamp written
     * on the dirty boot names this identity and sidecar again. */
    reboot(); dosvm_image_desktop_presented();
    if (dosvm_image_step()) run_to_end();
    dosvm_image_query(&state, &progress, &error); assert(state == CIUKI_DOSVM_IMAGE_VERIFIED);

    /* 4. A launch during the background check reuses the in-flight result:
     * the hash continues from where it is, it is never started twice. */
    reboot(); set_mtime(0x5b58, 0x6a20);
    dosvm_image_desktop_presented();
    assert(dosvm_image_step());
    for (unsigned i = 0; i < 4; i++) assert(dosvm_image_step());
    dosvm_image_query(&state, &progress, &error);
    assert(state == CIUKI_DOSVM_IMAGE_VERIFYING && progress && progress < 1000);
    unsigned before = iv.blocks;
    v = launch_version(&fd);
    cancel_calls = 0; cancel_at = 3;                   /* the owner ends during the wait */
    assert(dosvm_image_lookup(&table, &v, sha, &source, cancel_after, 0) == -EINTR);
    assert(iv.busy && iv.blocks > before && !iv.waiters);
    unsigned resumed = iv.blocks;
    cancel_at = 1 << 30;
    OK(dosvm_image_lookup(&table, &v, sha, &source, cancel_after, 0));
    assert(!strcmp(source, "background") && !strcmp(sha, good_sha) && !iv.busy);
    assert(iv.blocks == expected_blocks() && resumed < expected_blocks());
    OK(vfs_close(&table, fd));
    dosvm_image_query(&state, &progress, &error); assert(state == CIUKI_DOSVM_IMAGE_VERIFIED && stamp_present());
    printf("fx-4b launch during verify: joined at block %u, owner-ended wait EINTR at block %u, completed %u blocks once\n",
           before, resumed, iv.blocks);

    /* 5. Mismatch: changed bytes (and identity) without a new sidecar. The
     * result names the actual bytes, is reported, never stamped; a launch
     * receives it and refuses at the sidecar check (dosvm_disk.c). */
    reboot();
    uint8_t flip = 0x5a; write_at("C:\\SYSTEM\\DOS\\UNIFIED.IMG", 4096, &flip, 1, false);
    set_mtime(0x5b59, 0x6a20);
    char bad_sha[65]; file_sha("C:\\SYSTEM\\DOS\\UNIFIED.IMG", bad_sha, 0); assert(strcmp(bad_sha, good_sha));
    unsigned mismatches = mismatch_records;
    dosvm_image_desktop_presented();
    v = launch_version(&fd);
    OK(dosvm_image_lookup(&table, &v, sha, &source, 0, 0)); /* joins the armed check */
    assert(!strcmp(source, "background") && !strcmp(sha, bad_sha));
    dosvm_image_query(&state, &progress, &error);
    assert(state == CIUKI_DOSVM_IMAGE_MISMATCH && mismatch_records == mismatches + 1);
    assert(strstr(last_record, "result=mismatch") && strstr(last_record, good_sha) && strstr(last_record, bad_sha));
    OK(vfs_close(&table, fd));
    reboot(); dosvm_image_desktop_presented();         /* never stamped: hashes again */
    assert(dosvm_image_step()); run_to_end();
    dosvm_image_query(&state, &progress, &error);
    assert(state == CIUKI_DOSVM_IMAGE_MISMATCH && iv.blocks == expected_blocks());
    printf("fx-4b mismatch: expected %.16s..., actual %.16s..., reported, not stamped, re-hashed next boot\n", good_sha, bad_sha);

    /* 6. Game removal holds: the running check releases the image (deny
     * write) at once; release re-arms it. */
    flip = 0; write_at("C:\\SYSTEM\\DOS\\UNIFIED.IMG", 4096, &flip, 1, false);
    file_sha("C:\\SYSTEM\\DOS\\UNIFIED.IMG", bad_sha, 0); set_sidecar(bad_sha);
    reboot(); dosvm_image_desktop_presented();
    assert(dosvm_image_step()); assert(dosvm_image_step());
    dosvm_image_hold();
    dosvm_image_query(&state, &progress, &error);
    assert(state == CIUKI_DOSVM_IMAGE_UNCHECKED && !iv.busy && iv.fd < 0 && !iv.table_ready);
    int writer = vfs_open(&table, "C:\\SYSTEM\\DOS\\UNIFIED.IMG", VFS_READ | VFS_WRITE, VFS_DENY_ALL, 0);
    assert(writer >= 0); OK(vfs_close(&table, writer));
    assert(!dosvm_image_step());                       /* held: nothing starts */
    dosvm_image_release();
    dosvm_image_query(&state, &progress, &error); assert(state == CIUKI_DOSVM_IMAGE_VERIFYING);
    assert(dosvm_image_step()); run_to_end();
    dosvm_image_query(&state, &progress, &error); assert(state == CIUKI_DOSVM_IMAGE_VERIFIED);

    /* 7. fx-23 (T23 trial f441c5bd): a DOS launch right after the check
     * must not hash 27.7 MB again on the launching thread inside CREATE.
     * (a) The check finished on a volume recovered dirty at this mount: the
     * stamp lapses, but this session's own result names the same version. */
    reboot(); storage.volumes[2].fat.dirty_recovered = true;
    dosvm_image_desktop_presented();
    assert(dosvm_image_step()); run_to_end();
    dosvm_image_query(&state, &progress, &error); assert(state == CIUKI_DOSVM_IMAGE_VERIFIED);
    char current_sha[65]; memcpy(current_sha, iv.sha, 65);  /* section 6 changed byte 4096 and the sidecar */
    digest_sha[0] = 0;                                  /* the launch's session cache missed */
    v = launch_version(&fd); reads = disk.reads; unsigned hashed = iv.blocks;
    OK(dosvm_image_lookup(&table, &v, sha, &source, 0, 0));
    assert(!strcmp(source, "background") && !strcmp(sha, current_sha) && iv.blocks == hashed);
    assert(disk.reads - reads < 8);                      /* no second hash anywhere */
    OK(vfs_close(&table, fd));
    printf("fx-23 launch after the check on a recovered volume: source=%s, %u disk reads, no hash\n", source, disk.reads - reads);
    /* (b) The image identity changed after the check (and the stamp no
     * longer matches): the launch waits for one fresh check on the verifier
     * (progress in IMAGE_QUERY) instead of hashing synchronously itself. */
    set_mtime(0x5b5a, 0x6a20);
    v = launch_version(&fd);
    OK(dosvm_image_lookup(&table, &v, sha, &source, 0, 0));
    assert(!strcmp(source, "background") && !strcmp(sha, current_sha) && iv.blocks == expected_blocks());
    assert(vfs_file_version_equal(&iv.version, &v));
    OK(vfs_close(&table, fd));
    dosvm_image_query(&state, &progress, &error); assert(state == CIUKI_DOSVM_IMAGE_VERIFIED);
    printf("fx-23 launch after an identity change: re-armed background check, %u blocks on the verifier, source=%s\n",
           iv.blocks, source);
    storage.volumes[2].fat.dirty_recovered = false;

    assert(block_allocs == block_frees && !stamp_errors && records && digest_publishes);
    vfs_table_destroy(&table); OK(vfs_detach(&storage.vfs, 2)); vfs_destroy(&storage.vfs);
    cache_destroy(&storage.cache); fake_close(&disk); assert(!fs_host_contig_live);
    puts("PASS fx-4b image verify: background hash with progress, stamp reuse, 4 identity changes re-hash, launch joins in-flight check (EINTR on owner exit), mismatch reported and never stamped, removal hold/release; fx-23: finished result reused, re-armed check instead of a launch hash");
    return 0;
}
