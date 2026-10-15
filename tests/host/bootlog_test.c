/* Production logger on a bounded byte-addressed VFS model. Real FAT reopen
 * and qualification checks remain in fs/test_storage.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ciuki/bootlog.h>
#include "../../src/kernel/fs/vfs.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL bootlog %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static uint32_t model_now;
static uint32_t model_now_ms(void) { return model_now; }
#define fs_now_ms model_now_ms
#include "../../src/kernel/core/bootlog.c"

static char old_records[65536];
static size_t old_length;
const char *blackbox_replay(size_t *n) { *n=old_length;return old_records; }
static struct vfs fs;
static struct fat_volume volume;
static unsigned char disk[BOOTLOG_LIMIT];
static uint32_t disk_size, disk_position;
static unsigned calls, turns, commits, truncations, publications;
static uint32_t published_size;
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
int vfs_log_rotate(struct vfs_table *t, const char *a, const char *b, const char *c, uint32_t capacity)
{
    storage_call(); CHECK(t->vfs==&fs && !strcmp(a,BOOTLOG_PATH) &&
        !strcmp(b,BOOTLOG_PREVIOUS) && !strcmp(c,BOOTLOG_OLDEST) && capacity==BOOTLOG_LIMIT);
    return 0; /* Names/chain recovery are tested against real FAT fixtures. */
}
int vfs_seek(struct vfs_table *t, int h, int64_t offset, enum vfs_whence whence, uint64_t *at)
{ (void)t; storage_call(); CHECK(h == 1 && whence == VFS_SEEK_SET && offset >= 0 && offset <= BOOTLOG_LIMIT); if (fail_seek) return -FS_EIO; *at = disk_position = (uint32_t)offset; return 0; }
int vfs_truncate(struct vfs_table *t, int h, uint64_t size)
{ (void)t; storage_call(); CHECK(h == 1 && (size == 0 || size == BOOTLOG_PROTECTED)); if (fail_truncate) return -FS_EIO; if (size > disk_size) memset(disk + disk_size, 0, (size_t)size - disk_size); disk_size = (uint32_t)size; truncations++; return 0; }
int vfs_log_write(struct vfs_table *t, int h, const void *data, size_t size, uint32_t logical_size, size_t *done)
{
    (void)t; storage_call(); CHECK(h == 1 && size && size == BOOTLOG_BATCH && disk_position + size <= BOOTLOG_LIMIT);
    *done = 0; if (fail_write) return -FS_EIO;
    if (short_write) size--;
    if (disk_position > disk_size) memset(disk + disk_size, 0, disk_position - disk_size);
    memcpy(disk + disk_position, data, size); disk_position += (uint32_t)size;
    disk_size = logical_size;
    *done = size; return 0;
}
int vfs_log_reserve(struct vfs_table *t, int h, uint32_t capacity)
{
    (void)t; storage_call(); CHECK(h == 1 && capacity == BOOTLOG_LIMIT);
    if (fail_truncate) return -FS_EIO;
    memset(disk, 0, sizeof(disk)); disk_size = disk_position = published_size = 0;
    truncations++; return 0;
}
int vfs_read(struct vfs_table *t, int h, void *data, size_t size, size_t *done)
{
    (void)t; storage_call(); CHECK(h == 1 && disk_position + size <= disk_size);
    memcpy(data, disk + disk_position, size); disk_position += (uint32_t)size; *done = size; return 0;
}
int vfs_log_publish(struct vfs_table *t, int h, bool finish)
{
    (void)t; (void)finish; storage_call(); CHECK(h == 1);
    publications++; published_size = disk_size; return fail_commit ? -FS_EIO : 0;
}
int vfs_commit(struct vfs_table *t, int h) { (void)t; storage_call(); CHECK(h == 1); commits++; return fail_commit ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *t, int h) { (void)t; storage_call(); CHECK(h == 1); return 0; }
static struct bootlog_stats snapshot(void) { struct bootlog_stats s; bootlog_snapshot(&s); return s; }
static void drain(void)
{
    while (snapshot().queued) {
        uint64_t writes = snapshot().writes;
        model_now += BOOTLOG_DRAIN_MS;
        CHECK(!bootlog_drain()); CHECK(snapshot().writes == writes + 1);
        turns++;
    }
}
static void reset_model(void)
{
    bootlog_reset(); old_length=0; memset(&fs, 0, sizeof(fs)); memset(&volume, 0, sizeof(volume));
    fs.volumes[2] = &volume;
    calls = turns = commits = truncations = disk_position = publications = model_now = 0;
    fail_seek = fail_write = fail_truncate = fail_commit = 0; short_write = false;
    sync_waiting=storage_turn=service_sync=produce_during_sync=false;
}
static void retention(void)
{
    reset_model(); disk_size = 9; memcpy(disk, "OLD BOOT\n", 9);
    static char text[4096]; memset(text, 'B', sizeof(text));
    /* Periodic traffic may wrap in RAM before mount; boot bytes survive. */
    for (unsigned i = 0; i < BOOTLOG_RAM / sizeof(text) + 3; i++) bootlog_capture_ring(text, sizeof(text));
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
    for (unsigned i = BOOTLOG_RAM / sizeof(text); i < BOOTLOG_RING / sizeof(text); i++) { bootlog_capture_ring(text, sizeof(text)); drain(); }
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
    puts("bootlog: protected pre-mount bytes, periodic routing, 320KiB boundary/two wraps, zero padding, fresh qualified boot/shutdown PASS");
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
    service_sync=true;sync_pauses=0;CHECK(!sync_records() && sync_pauses==1 && commits==1);
    /* Already-written bytes still need a commit and a visible storage turn. */
    bootlog_capture("written",7);model_now += BOOTLOG_DRAIN_MS; CHECK(!bootlog_drain());
    CHECK(!snapshot().queued);sync_pauses=0;CHECK(!sync_records() && sync_pauses==1 && commits==2);
    bootlog_capture("next",4);service_sync=false;sync_pauses=0;
    unsigned before=calls;
    CHECK(bootlog_sync(never_cancel,pause_sync,NULL)==-FS_EAGAIN);
    CHECK(sync_pauses==BOOTLOG_SYNC_TIMEOUT_MS && calls==before && snapshot().active && !snapshot().error);
    service_sync=true;sync_pauses=0;CHECK(!sync_records());
    bootlog_capture("bad commit",10);fail_commit=1;sync_pauses=0;
    CHECK(sync_records()==-FS_EIO && sync_pauses==1 && snapshot().error==-FS_EIO);
    CHECK(bootlog_shutdown()==-FS_EIO);
    puts("bootlog: aligned 32KiB turns, commit-only watermark, independent deadline, recoverable wait timeout and sticky commit error PASS");
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
    CHECK(commits==overlap_turns);
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
    CHECK(commits==3 && overlap_turns==3 && !snapshot().queued);
    for(unsigned i=0;i<SYNC_WAITERS;i++)CHECK(!sync_waiters[i]);
    CHECK(!bootlog_shutdown());
    puts("bootlog: three overlapping bind watermarks commit at each captured byte target; later binders cannot extend earlier waits PASS");
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
    bootlog_capture("written",7);before=calls;
    CHECK(!bootlog_due(model_now) && !bootlog_drain() && calls==before);
    /* Ordinary drains obey f0-22's one-second cadence. */
    model_now+=BOOTLOG_DRAIN_MS;
    CHECK(bootlog_due(model_now) && !bootlog_drain());
    CHECK(!snapshot().queued && commits==1);before=calls;
    uint64_t writes=snapshot().writes;
    CHECK(bootlog_try_sync()==-FS_EAGAIN && snapshot().queued==1 && calls==before);
    CHECK(!snapshot().boot_queued && !snapshot().ring_queued && bootlog_due(model_now));
    /* The explicit request bypasses cadence without another data write. */
    CHECK(!bootlog_drain() && commits==2 && snapshot().writes==writes && !snapshot().queued);
    before=calls;CHECK(!bootlog_try_sync() && calls==before && !bootlog_due(model_now));
    bootlog_capture("bad sink",8);CHECK(bootlog_try_sync()==-FS_EAGAIN);
    fail_commit=1;CHECK(bootlog_drain()==-FS_EIO);before=calls;
    CHECK(bootlog_try_sync()==-FS_EIO && calls==before);
    CHECK(bootlog_shutdown()==-FS_EIO);
    puts("bootlog: nonblocking stage watermark, no producer I/O, commit-only turn, inactive/failed sink status PASS");
}
static void batching_and_cadence(void)
{
    reset_model(); CHECK(!bootlog_activate(&fs, true, 1));
    bootlog_capture("first\n", 6); CHECK(bootlog_due(model_now)); CHECK(!bootlog_drain());
    CHECK(snapshot().writes == 1 && snapshot().bytes_written == BOOTLOG_BATCH);
    CHECK(!publications && !published_size && truncations == 1);
    bootlog_capture("second\n", 7);
    model_now = 999; unsigned before = calls;
    CHECK(!bootlog_due(model_now) && !bootlog_drain() && calls == before);
    model_now = 1000; CHECK(bootlog_due(model_now) && !bootlog_drain());
    CHECK(snapshot().writes == 2 && !publications && truncations == 1);
    CHECK(!memcmp(disk, "first\nsecond\n", 13) && disk[13] == 0);
    model_now = 4999; CHECK(!bootlog_due(model_now));
    model_now = 5000; CHECK(bootlog_due(model_now) && !bootlog_drain());
    CHECK(publications == 1 && published_size == 13 && snapshot().writes == 2);
    static char flood[BOOTLOG_HIGH_WATER]; memset(flood, 'H', sizeof(flood));
    bootlog_capture_ring(flood, sizeof(flood));
    CHECK(bootlog_due(model_now) && !bootlog_drain()); /* high water bypasses rate */
    CHECK(snapshot().ring_queued == sizeof(flood) - BOOTLOG_BATCH && publications == 1);
    CHECK(!bootlog_shutdown() && published_size == snapshot().size);
    CHECK(snapshot().bytes_written == snapshot().writes * BOOTLOG_BATCH);
    CHECK(truncations == 1);
    reset_model(); model_now = UINT32_MAX - 500;
    CHECK(!bootlog_activate(&fs, true, 2)); bootlog_capture("a", 1); CHECK(!bootlog_drain());
    bootlog_capture("b", 1); model_now = 498; CHECK(!bootlog_due(model_now));
    model_now = 499; CHECK(bootlog_due(model_now) && !bootlog_drain());
    CHECK(!bootlog_shutdown());
    puts("bootlog: 32KiB physical blocks, one-second rate/high water, five-second metadata, shutdown EOF and clock wrap PASS");
}
static void runtime_summaries(void)
{
    reset_model();
    static const char prefix[]="boot prefix\n";
    static const char health[]="[kernel] irqoff_section=serial_tx_byte site=c011e445 irqoff_max_us=130 violations=0\n"
        "[kernel] stretch_max_ms=1 irqoff_max_us=130 timer_gaps=0/0\n"
        "[kernel] long_paths serial_drop=4 bootlog_drain_ms=3\n";
    bootlog_capture(prefix,sizeof(prefix)-1);
    bootlog_capture_ring(health,sizeof(health)-1);
    CHECK(!bootlog_activate(&fs,true,1));drain();
    CHECK(!bootlog_shutdown());
    CHECK(published_size==BOOTLOG_PROTECTED+sizeof(health)-1);
    CHECK(!memcmp(disk,prefix,sizeof(prefix)-1) && disk[sizeof(prefix)-1]==0);
    CHECK(!strstr((const char *)disk,"[kernel]")); /* a C string sees only the prefix */
    CHECK(!memcmp(disk+BOOTLOG_PROTECTED,health,sizeof(health)-1));
    puts("bootlog: kernel attribution/stretch/long_paths persist at 128KiB past the NUL-padded prefix; full directory length required PASS");
}
static void recovered_prefix(void)
{
    reset_model();
    char block[4096];memset(block,'N',sizeof(block));
    for (unsigned i=0;i<BOOTLOG_PROTECTED/sizeof(block);i++) bootlog_capture(block,sizeof(block));
    memset(old_records,'O',sizeof(old_records));memcpy(old_records,"[blackbox] previous boot\n",25);old_length=sizeof(old_records);
    CHECK(!bootlog_activate(&fs,true,1));CHECK(snapshot().dropped==BOOTLOG_PROTECTED/2);
    service_sync=true;CHECK(!bootlog_sync(never_cancel,pause_sync,NULL));
    CHECK(published_size==BOOTLOG_PROTECTED && !memcmp(disk,old_records,sizeof(old_records)));
    CHECK(disk[sizeof(old_records)]=='N');CHECK(!bootlog_shutdown());
    puts("PASS replay prefix: immutable blackbox head, bounded current-boot prefix, dropped-byte watermark retires, sync completes");
}
int main(void) { recovered_prefix(); retention(); errors(); synchronous_records();sync_bounds();overlapping_watermarks();nonblocking_records();batching_and_cadence();runtime_summaries(); return 0; }
