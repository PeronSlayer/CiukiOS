/* Production logger on a bounded byte-addressed VFS model. Real FAT reopen
 * and qualification checks remain in fs/test_storage.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/bootlog.h>
#include "../../src/kernel/fs/vfs.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL bootlog %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "../../src/kernel/core/bootlog.c"

static struct vfs fs;
static struct fat_volume volume;
static unsigned char disk[BOOTLOG_LIMIT];
static uint32_t disk_size, disk_position;
static unsigned calls, turns, commits, truncations;
static int fail_seek, fail_write, fail_truncate,fail_commit;
static bool short_write;
static bool sync_waiting, storage_turn, service_sync,produce_during_sync;
static void storage_call(void)
{
    /* VFS work must never happen while the producer's IRQ-equivalent lock
     * is held. Every write is one bounded worker batch. */
    CHECK(!pthread_mutex_trylock(&ram_lock)); CHECK(!pthread_mutex_unlock(&ram_lock));
    CHECK(!sync_waiting || storage_turn); /* a binder never performs VFS I/O */
    calls++;
}
int vfs_table_init(struct vfs *v, struct vfs_table *t, unsigned drive)
{ storage_call(); CHECK(v == &fs && drive == 2); t->vfs = v; return 0; }
void vfs_table_destroy(struct vfs_table *t) { storage_call(); CHECK(t->vfs == &fs); }
int vfs_open(struct vfs_table *t, const char *path, unsigned mode, enum vfs_share share, uint8_t attr)
{ storage_call(); CHECK(t->vfs == &fs && !strcmp(path, BOOTLOG_PATH)); CHECK(mode == (VFS_READ|VFS_WRITE|VFS_CREATE) && share == VFS_DENY_WRITE && !attr); return 1; }
int vfs_seek(struct vfs_table *t, int h, int64_t offset, enum vfs_whence whence, uint64_t *at)
{ (void)t; storage_call(); CHECK(h == 1 && whence == VFS_SEEK_SET && offset >= 0 && offset <= BOOTLOG_LIMIT); if (fail_seek) return -FS_EIO; *at = disk_position = (uint32_t)offset; return 0; }
int vfs_truncate(struct vfs_table *t, int h, uint64_t size)
{ (void)t; storage_call(); CHECK(h == 1 && (size == 0 || size == BOOTLOG_PROTECTED)); if (fail_truncate) return -FS_EIO; if (size > disk_size) memset(disk + disk_size, 0, (size_t)size - disk_size); disk_size = (uint32_t)size; truncations++; return 0; }
int vfs_write(struct vfs_table *t, int h, const void *data, size_t size, size_t *done)
{
    (void)t; storage_call(); CHECK(h == 1 && size && size <= 4096 && disk_position + size <= BOOTLOG_LIMIT);
    *done = 0; if (fail_write) return -FS_EIO;
    if (short_write) size--;
    if (disk_position > disk_size) memset(disk + disk_size, 0, disk_position - disk_size);
    memcpy(disk + disk_position, data, size); disk_position += (uint32_t)size;
    if (disk_position > disk_size) disk_size = disk_position;
    *done = size; return 0;
}
int vfs_commit(struct vfs_table *t, int h) { (void)t; storage_call(); CHECK(h == 1); commits++; return fail_commit ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *t, int h) { (void)t; storage_call(); CHECK(h == 1); return 0; }
static struct bootlog_stats snapshot(void) { struct bootlog_stats s; bootlog_snapshot(&s); return s; }
static void drain(void)
{
    while (snapshot().queued) {
        uint64_t writes = snapshot().writes;
        CHECK(!bootlog_drain()); CHECK(snapshot().writes == writes + 1);
        turns++;
    }
}
static void reset_model(void)
{
    bootlog_reset(); memset(&fs, 0, sizeof(fs)); memset(&volume, 0, sizeof(volume));
    fs.volumes[2] = &volume;
    calls = turns = commits = truncations = disk_position = 0;
    fail_seek = fail_write = fail_truncate = fail_commit = 0; short_write = false;
    sync_waiting=storage_turn=service_sync=produce_during_sync=false;
}
static void retention(void)
{
    reset_model(); disk_size = 9; memcpy(disk, "OLD BOOT\n", 9);
    static char text[4096]; memset(text, 'B', sizeof(text));
    /* Periodic traffic may wrap in RAM before mount; boot bytes survive. */
    for (unsigned i = 0; i < 5; i++) bootlog_capture_ring(text, sizeof(text));
    bootlog_capture("FIRST BOOT\n", 11);
    CHECK(!calls && snapshot().boot_queued == 11 && snapshot().ring_queued == BOOTLOG_RAM);
    CHECK(snapshot().dropped == 3 * sizeof(text));
    CHECK(bootlog_activate(&fs, false, 0) == -FS_EROFS && !calls && disk_size == 9);
    volume.readonly = true; CHECK(bootlog_activate(&fs, true, 1) == -FS_EROFS && !calls);
    volume.readonly = false;
    CHECK(!bootlog_activate(&fs, true, 17) && disk_size == 0);
    drain();
    CHECK(!memcmp(disk, "FIRST BOOT\n", 11));
    CHECK(disk[11] == 0 && disk[BOOTLOG_PROTECTED] == 'B');
    CHECK(snapshot().boot_size == 11 && snapshot().first_write_sequence == 18);
    /* Fill the protected region after runtime output has already started. */
    for (unsigned i = 0; i < BOOTLOG_PROTECTED / sizeof(text) - 1; i++) {
        bootlog_capture(text, sizeof(text)); drain();
    }
    bootlog_capture(text, sizeof(text) - 11); drain();
    CHECK(snapshot().boot_size == BOOTLOG_PROTECTED && !memcmp(disk, "FIRST BOOT\n", 11));
    /* Reach the exact 1 MiB boundary, wrap twice, and verify the prefix. */
    memset(text, 'R', sizeof(text));
    for (unsigned i = 2; i < BOOTLOG_RING / sizeof(text); i++) { bootlog_capture_ring(text, sizeof(text)); drain(); }
    CHECK(snapshot().size == BOOTLOG_LIMIT && !snapshot().rotations);
    for (unsigned wrap = 1; wrap <= 2; wrap++) {
        bootlog_capture("NEW RUNTIME\n", 12); drain();
        CHECK(snapshot().rotations == wrap && snapshot().size == BOOTLOG_PROTECTED + 12);
        CHECK(!memcmp(disk + BOOTLOG_PROTECTED, "NEW RUNTIME\n", 12));
        CHECK(!memcmp(disk, "FIRST BOOT\n", 11));
        for (unsigned i = 11; i < BOOTLOG_PROTECTED; i++) CHECK(disk[i] == 'B');
        if (wrap == 1) {
            for (unsigned i = 0; i < BOOTLOG_RING / sizeof(text) - 1; i++) { bootlog_capture_ring(text, sizeof(text)); drain(); }
            bootlog_capture_ring(text, sizeof(text) - 12); drain();
            CHECK(snapshot().size == BOOTLOG_LIMIT);
        }
    }
    CHECK(!bootlog_shutdown() && commits == 1 && !snapshot().active);
    unsigned before = calls; bootlog_capture("ignored", 7); bootlog_capture_ring("ignored", 7);
    CHECK(!bootlog_drain() && calls == before && !snapshot().queued);
    /* Reopening without activation preserves the file; a qualified next
     * boot replaces it rather than protecting stale records indefinitely. */
    reset_model(); CHECK(!memcmp(disk, "FIRST BOOT\n", 11));
    bootlog_capture("NEXT BOOT\n", 10); CHECK(!bootlog_activate(&fs, true, 20));
    CHECK(!bootlog_shutdown() && disk_size == 10 && !memcmp(disk, "NEXT BOOT\n", 10));
    puts("bootlog: protected pre-mount bytes, periodic routing, 1MiB boundary/two wraps, zero padding, fresh qualified boot/shutdown PASS");
}
static void errors(void)
{
    for (unsigned fault = 0; fault < 3; fault++) {
        reset_model(); CHECK(!bootlog_activate(&fs, true, 1));
        bootlog_capture("failure\n", 8);
        fail_seek = fault == 0; fail_write = fault == 1; short_write = fault == 2;
        CHECK(bootlog_drain() == -FS_EIO && !snapshot().active);
        unsigned before = calls;
        bootlog_capture("disabled\n", 9); CHECK(bootlog_drain() == -FS_EIO && calls == before);
        CHECK(bootlog_shutdown() == -FS_EIO);
    }
    reset_model(); fail_truncate = 1;
    CHECK(bootlog_activate(&fs, true, 1) == -FS_EIO && !snapshot().active);
    CHECK(bootlog_shutdown() == -FS_EIO);
    puts("bootlog: sticky seek/write/short-write/truncation errors, no recursion or retry I/O PASS");
}
static unsigned sync_pauses;
static bool cancel_sync(void *ctx) { (void)ctx;return sync_pauses>=3; }
static void pause_sync(void *ctx) {
    (void)ctx;sync_pauses++;
    if (produce_during_sync) {
        static char periodic[4096];memset(periodic,'P',sizeof(periodic));
        bootlog_capture_ring(periodic,sizeof(periodic));
    }
    if (service_sync) {
        storage_turn=true;
        int e=bootlog_drain(); CHECK(!e || e==-FS_EBUSY || e==-FS_EIO);
        storage_turn=false;
    }
}
static int sync_records(void) {
    sync_waiting=true;
    int e=bootlog_sync(cancel_sync,pause_sync,0);
    sync_waiting=false; return e;
}
static void synchronous_records(void)
{
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    bootlog_capture("[usb-storage] stage=inquiry addr=1\n",33);
    service_sync=true;sync_pauses=0;CHECK(!sync_records() && sync_pauses==1);
    CHECK(!snapshot().queued && commits==1 && !memcmp(disk,"[usb-storage] stage=inquiry addr=1\n",33));
    /* A running writer or a VFS client cannot make sync spin or steal locks. */
    bootlog_capture("next stage\n",11);
    draining=true;sync_pauses=0;
    CHECK(sync_records()==-FS_EAGAIN && sync_pauses==3 && draining);
    draining=false;fs_lock_take(&fs.lock);sync_pauses=0;
    CHECK(sync_records()==-FS_EAGAIN && sync_pauses==3);
    fs_lock_drop(&fs.lock);CHECK(!snapshot().error);
    CHECK(!bootlog_shutdown());
    puts("bootlog: USB watermark wait, storage-only drain/commit, cancellable writer/VFS contention without lock theft PASS");
}
static bool never_cancel(void *ctx) { (void)ctx;return false; }
static void sync_bounds(void)
{
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    static char bytes[8192];memset(bytes,'Z',sizeof(bytes));bootlog_capture(bytes,sizeof(bytes));
    service_sync=true;sync_pauses=0;CHECK(!sync_records() && sync_pauses==2 && commits==1);
    /* Already-written bytes still need a commit and a visible storage turn. */
    bootlog_capture("written",7);CHECK(!bootlog_drain());
    CHECK(!snapshot().queued);sync_pauses=0;CHECK(!sync_records() && sync_pauses==1 && commits==2);
    bootlog_capture("next",4);service_sync=false;sync_pauses=0;
    unsigned before=calls;
    CHECK(bootlog_sync(never_cancel,pause_sync,NULL)==-FS_EAGAIN);
    CHECK(sync_pauses==BOOTLOG_SYNC_TIMEOUT_MS && calls==before && snapshot().active && !snapshot().error);
    service_sync=true;sync_pauses=0;CHECK(!sync_records());
    bootlog_capture("bad commit",10);fail_commit=1;sync_pauses=0;
    CHECK(sync_records()==-FS_EIO && sync_pauses==1 && snapshot().error==-FS_EIO);
    CHECK(bootlog_shutdown()==-FS_EIO);
    puts("bootlog: bounded 4KiB turns, commit-only watermark, independent deadline, recoverable wait timeout and sticky commit error PASS");
    reset_model();CHECK(!bootlog_activate(&fs,true,1));bootlog_capture("stage",5);
    service_sync=produce_during_sync=true;sync_pauses=0;
    CHECK(!sync_records() && sync_pauses==1 && commits==1 && snapshot().ring_queued==4096);
    produce_during_sync=false;CHECK(!bootlog_shutdown());
    puts("bootlog: concurrent periodic output cannot extend a bind's captured watermark PASS");
}
static unsigned overlap_turns;
static void overlap_storage(void *ctx)
{
    (void)ctx;overlap_turns++;storage_turn=true;
    CHECK(!bootlog_drain());storage_turn=false;
    if (overlap_turns<=2) CHECK(commits==overlap_turns && snapshot().queued);
}
static void second_waiter(void *ctx)
{
    (void)ctx;static char newest[20000];memset(newest,'C',sizeof(newest));
    bootlog_capture(newest,sizeof(newest));
    CHECK(bootlog_try_sync()==-FS_EAGAIN); /* cannot displace older waiters */
    CHECK(!bootlog_sync(never_cancel,overlap_storage,NULL));
}
static void first_waiter(void *ctx)
{
    (void)ctx;static char newer[4096];memset(newer,'B',sizeof(newer));
    bootlog_capture(newer,sizeof(newer));
    CHECK(!bootlog_sync(never_cancel,second_waiter,NULL));
}
static void overlapping_watermarks(void)
{
    reset_model();CHECK(!bootlog_activate(&fs,true,1));bootlog_capture("first",5);
    overlap_turns=0;sync_waiting=true;
    CHECK(!bootlog_sync(never_cancel,first_waiter,NULL));sync_waiting=false;
    CHECK(commits==3 && overlap_turns==6 && !snapshot().queued);
    for(unsigned i=0;i<SYNC_WAITERS;i++)CHECK(!sync_waiters[i]);
    CHECK(!bootlog_shutdown());
    puts("bootlog: three overlapping bind watermarks commit at 4KiB/8KiB/final; later binders cannot extend earlier waits PASS");
}
static void nonblocking_records(void)
{
    reset_model();bootlog_capture("stage",5);
    CHECK(bootlog_try_sync()==-FS_EAGAIN && !calls);
    CHECK(!bootlog_activate(&fs,true,1));unsigned before=calls;
    CHECK(bootlog_try_sync()==-FS_EAGAIN && calls==before && !commits);
    CHECK(!bootlog_drain() && commits==1 && !snapshot().queued);
    before=calls;CHECK(!bootlog_try_sync() && calls==before);
    /* A watermark with no queued bytes still schedules a commit-only turn. */
    bootlog_capture("written",7);CHECK(!bootlog_drain());
    CHECK(!snapshot().queued);before=calls;
    CHECK(bootlog_try_sync()==-FS_EAGAIN && snapshot().queued==1 && calls==before);
    CHECK(!bootlog_drain() && commits==2 && !bootlog_try_sync());
    bootlog_capture("bad sink",8);CHECK(bootlog_try_sync()==-FS_EAGAIN);
    fail_commit=1;CHECK(bootlog_drain()==-FS_EIO);before=calls;
    CHECK(bootlog_try_sync()==-FS_EIO && calls==before);
    CHECK(bootlog_shutdown()==-FS_EIO);
    puts("bootlog: nonblocking stage watermark, no producer I/O, commit-only turn, inactive/failed sink status PASS");
}
int main(void) { retention(); errors(); synchronous_records();sync_bounds();overlapping_watermarks();nonblocking_records(); return 0; }
