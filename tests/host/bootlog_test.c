/* Production logger on a bounded byte-addressed VFS model. Real FAT reopen
 * and qualification checks remain in fs/test_storage.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
static void (*device_write)(const char *,unsigned);
static void (*commit_hook)(void);
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
    if (device_write) {
        uint32_t position=ring_position==BOOTLOG_LIMIT ? BOOTLOG_PROTECTED : ring_position;
        CHECK(position>=disk_position && logical_size>=position);
        device_write((const char *)data+position-disk_position,logical_size-position);
    }
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
int vfs_commit(struct vfs_table *t, int h) { (void)t; storage_call(); CHECK(h == 1); commits++;if (commit_hook) commit_hook();return fail_commit ? -FS_EIO : 0; }
int vfs_close(struct vfs_table *t, int h) { (void)t; storage_call(); CHECK(h == 1); return 0; }
static struct bootlog_stats snapshot(void) { struct bootlog_stats s; bootlog_snapshot(&s); return s; }
static void drain(void)
{
    while (snapshot().queued) {
        uint64_t writes = snapshot().writes;
        model_now += BOOTLOG_DRAIN_MS;
        CHECK(!bootlog_drain()); CHECK(snapshot().writes > writes && snapshot().writes <= writes + BOOTLOG_TURN_BLOCKS);
        turns++;
    }
}
static void reset_model(void)
{
    bootlog_reset(); old_length=0; memset(&fs, 0, sizeof(fs)); memset(&volume, 0, sizeof(volume));
    fs.volumes[2] = &volume;
    calls = turns = commits = truncations = disk_position = publications = model_now = 0;
    fail_seek = fail_write = fail_truncate = fail_commit = 0; short_write = false;
    sync_waiting=storage_turn=service_sync=produce_during_sync=false;device_write=NULL;commit_hook=NULL;
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
    CHECK(!snapshot().ring_queued && publications == 1);
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
static void stalled_metadata(void)
{
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    bootlog_capture("durable before stall\n",21);CHECK(!bootlog_drain());
    CHECK(!snapshot().metadata_writes && !published_size);
    fs_lock_take(&fs.lock);
    unsigned before=calls;
    for (model_now=5000;model_now<=45000;model_now+=5000) {
        CHECK(bootlog_due(model_now));
        CHECK(bootlog_drain()==-FS_EBUSY && calls==before);
        CHECK(!snapshot().metadata_writes && snapshot().writes==1 && !snapshot().error);
    }
    fs_lock_drop(&fs.lock);
    CHECK(bootlog_due(model_now) && !bootlog_drain());
    CHECK(snapshot().metadata_writes==1 && published_size==21);
    CHECK(!bootlog_shutdown());
    puts("PASS F0-36 metadata stall: deadline due 5..45s, namespace try-lock skips publication with metadata=0, unlock publishes without new data");
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
/* 1,024,000 B/s diagnostic traffic plus four protected 128-byte rows each 10 ms.
 * Device time advances during the production writer, so producers continue
 * through an in-flight slow write. Verify the actual logical disk stream,
 * including records crossing aligned batches and on-disk rotations. */
#define MODEL_END 2000u
static unsigned device_bps, generated_ms, protected_count, observed_count;
static unsigned seen[1024], line_length, max_blocks;
static char line[256];
static bool arrivals;
static void produce_until(uint32_t until)
{
    static char diagnostic[256];
    memset(diagnostic,'D',sizeof(diagnostic));diagnostic[255]='\n';
    while (arrivals && generated_ms<until && generated_ms<MODEL_END) {
        if (!(generated_ms%10)) for (unsigned row=0;row<4;row++) {
            char record[128];memset(record,'P',sizeof(record));
            int n=snprintf(record,sizeof(record),"P%06u:",protected_count++);
            CHECK(n==8);record[n]='P';record[127]='\n';
            bootlog_capture_ring(record,sizeof(record));
        }
        for (unsigned i=0;i<4;i++) bootlog_capture_verbose(diagnostic,sizeof(diagnostic));
        generated_ms++;
    }
}
static void paced_device(const char *bytes,unsigned n)
{
    for (unsigned i=0;i<n;i++) {
        CHECK(line_length<sizeof(line));line[line_length++]=bytes[i];
        if (bytes[i]!='\n') continue;
        if (line[0]=='P') {
            unsigned id;CHECK(line_length==128 && sscanf(line,"P%6u:",&id)==1 && id<1024);
            for (unsigned k=8;k<127;k++) CHECK(line[k]=='P');
            CHECK(!seen[id]++);observed_count++;
        } else {
            CHECK(line_length==256);
            for (unsigned k=0;k<255;k++) CHECK(line[k]=='D');
        }
        line_length=0;
    }
    model_now+=(BOOTLOG_BATCH*1000u+device_bps-1)/device_bps;
    produce_until(model_now);
}
static void throughput(unsigned bps)
{
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    device_bps=bps;generated_ms=protected_count=observed_count=line_length=max_blocks=0;
    memset(seen,0,sizeof(seen));arrivals=true;device_write=paced_device;
    unsigned pressure_turns=0;
    while (generated_ms<MODEL_END) {
        model_now+=10;produce_until(model_now);
        if (bootlog_due(model_now)) {
            uint64_t before=snapshot().writes;uint32_t start=model_now;
            CHECK(!bootlog_drain());
            unsigned blocks=(unsigned)(snapshot().writes-before);
            CHECK(blocks<=BOOTLOG_TURN_BLOCKS);
            if (blocks>max_blocks) max_blocks=blocks;
            if (blocks>1) CHECK((model_now-start)<BOOTLOG_TURN_MS+(BOOTLOG_BATCH*1000u+bps-1)/bps);
            if (bootlog_due(model_now)) pressure_turns++;
        }
    }
    arrivals=false;CHECK(!bootlog_shutdown());device_write=NULL;
    struct bootlog_stats st=snapshot();
    CHECK(protected_count==800 && observed_count==protected_count && !line_length);
    for (unsigned i=0;i<protected_count;i++) CHECK(seen[i]==1);
    CHECK(!st.dropped_protected && !st.records_dropped_protected);
    CHECK(st.dropped==st.dropped_verbose && st.records_dropped_verbose*256u==st.dropped_verbose);
    if (bps==300000) CHECK(st.dropped_verbose && pressure_turns && max_blocks==1);
    else CHECK(!st.dropped_verbose && max_blocks>1);
    printf("PASS bootlog paced device=%u B/s input=1075200 B/s duration_ms=%u writes=%llu bytes=%llu max_blocks=%u protected=%u/%u dropped_protected=%u dropped_verbose=%u verbose_records=%u\n",
        bps,model_now,(unsigned long long)st.writes,(unsigned long long)st.bytes_written,max_blocks,
        observed_count,protected_count,st.dropped_protected,st.dropped_verbose,st.records_dropped_verbose);
}
static void class_pressure(void)
{
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    char protected[256],verbose[256];memset(protected,'P',256);memset(verbose,'D',256);
    bootlog_capture_ring(protected,256);
    for (unsigned i=1;i<BOOTLOG_RECORDS;i++) bootlog_capture_verbose(verbose,256);
    CHECK(!snapshot().dropped);
    for (unsigned i=0;i<BOOTLOG_RECORDS-1;i++) bootlog_capture_ring(protected,256);
    CHECK(snapshot().dropped_verbose==(BOOTLOG_RECORDS-1)*256 && !snapshot().dropped_protected);
    bootlog_capture_verbose(verbose,256); /* cannot evict a protected head */
    bootlog_capture_ring(protected,256);   /* protected-only exhaustion */
    CHECK(snapshot().records_dropped_protected==1 && snapshot().dropped_protected==256);
    CHECK(snapshot().records_dropped_verbose==BOOTLOG_RECORDS);
    CHECK(!bootlog_shutdown());
    for (unsigned i=0;i<BOOTLOG_RAM;i++) CHECK(disk[BOOTLOG_PROTECTED+i]=='P');
    puts("PASS bootlog classes: oldest verbose removed behind protected head, incoming verbose rejected at protected-only saturation, explicit per-class byte/record losses");
    /* A full-block write boundary splits a droppable record. Pin its
     * remainder while concurrent overflow removes later whole records. */
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    bootlog_capture_verbose("x",1);
    for (unsigned i=0;i<BOOTLOG_RECORDS-1;i++) bootlog_capture_verbose(verbose,256);
    /* A 256-slot burst forces pressure; restrict this turn to one block. */
    model_now=0;
    CHECK(!drain_owned());
    CHECK(head!=NONE && records[head].offset==255 && diag_head!=head);
    unsigned pinned=head;
    for (unsigned i=0;i<BOOTLOG_RECORDS*2;i++) bootlog_capture_verbose(verbose,256);
    CHECK(head==pinned && records[head].offset==255);
    CHECK(!bootlog_shutdown());
    CHECK(disk[BOOTLOG_PROTECTED]=='x');
    for (unsigned i=1;i<=256;i++) CHECK(disk[BOOTLOG_PROTECTED+i]=='D');
    puts("PASS bootlog whole records: partially drained verbose remainder pinned through concurrent overflow");
}
static void turn_budget(void)
{
    reset_model();
    static char prefix[BOOTLOG_PROTECTED];memset(prefix,'B',sizeof(prefix));
    static char runtime[BOOTLOG_RAM];memset(runtime,'R',sizeof(runtime));
    bootlog_capture(prefix,sizeof(prefix));bootlog_capture_ring(runtime,sizeof(runtime));
    CHECK(!bootlog_activate(&fs,true,1));
    CHECK(!bootlog_drain() && snapshot().writes==BOOTLOG_TURN_BLOCKS);
    CHECK(bootlog_due(model_now)); /* still due without advancing the clock */
    CHECK(!bootlog_drain() && !snapshot().queued);
    CHECK(!bootlog_shutdown());
    puts("PASS bootlog budget: fixed four-block cap with frozen clock, pressure remains due on next turn below high water");
}
static void request_during_commit(void)
{
    commit_hook=NULL;
    bootlog_capture_ring("new",3);
    CHECK(bootlog_try_sync()==-FS_EAGAIN);
}
static void commit_race(void)
{
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    bootlog_capture_ring("old",3);CHECK(bootlog_try_sync()==-FS_EAGAIN);
    commit_hook=request_during_commit;
    CHECK(!bootlog_drain() && commits==1 && snapshot().ring_queued==3);
    CHECK(bootlog_try_sync()==-FS_EAGAIN); /* new bytes were not committed */
    CHECK(!bootlog_drain() && commits==2 && !snapshot().queued && !bootlog_try_sync());
    CHECK(!memcmp(disk+BOOTLOG_PROTECTED,"oldnew",6));
    CHECK(!bootlog_shutdown());
    puts("PASS bootlog sync: request published during device commit cannot advance durability beyond the captured commit target");
}
static void pressure_watermarks(void)
{
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    char record[256];memset(record,'P',sizeof(record));
    /* Prefix and ring have independent captured targets; eviction of newer
     * diagnostics cannot make the old protected head appear durable. */
    bootlog_capture_ring(record,sizeof(record));CHECK(bootlog_try_sync()==-FS_EAGAIN);
    memset(record,'D',sizeof(record));
    for (unsigned i=0;i<BOOTLOG_RECORDS*3;i++) bootlog_capture_verbose(record,sizeof(record));
    CHECK(!commits && !snapshot().dropped_protected);
    CHECK(!bootlog_drain() && commits==1 && snapshot().ring_queued);
    for (unsigned i=0;i<256;i++) CHECK(disk[BOOTLOG_PROTECTED+i]=='P');
    /* Shutdown must drain bytes newer than a still-pending watermark too. */
    CHECK(bootlog_try_sync()==-FS_EAGAIN);bootlog_capture("new prefix",10);
    bootlog_capture_ring("new ring",8);
    CHECK(!bootlog_shutdown() && !snapshot().queued && !memcmp(disk,"new prefix",10));
    puts("PASS bootlog watermarks: out-of-order verbose evictions cannot retire older protected bytes; shutdown drains beyond pending sync target");
}

static void pause_during_commit(void)
{
    CHECK(bootlog_pause_drain(100)==-FS_EBUSY);
    commit_hook=NULL;
}
static void bounded_pause(void)
{
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    unsigned before=calls;
    CHECK(bootlog_pause_drain(0)==-FS_EINVAL);
    CHECK(bootlog_pause_drain(BOOTLOG_PAUSE_MAX_MS+1)==-FS_EINVAL);
    CHECK(bootlog_pause_drain(UINT32_MAX)==-FS_EINVAL);
    CHECK(!bootlog_pause_drain(100));
    bootlog_capture_ring("ARM\n",4);bootlog_capture_ring("cut\n",4);
    CHECK(snapshot().ring_queued==8 && !snapshot().dropped_protected);
    CHECK(bootlog_try_sync()==-FS_EAGAIN);
    CHECK(!bootlog_due(model_now) && bootlog_drain()==-FS_EBUSY);
    CHECK(bootlog_shutdown()==-FS_EBUSY && calls==before);
    model_now=99;
    CHECK(bootlog_pause_drain(100)==-FS_EBUSY); /* no deadline extension */
    CHECK(!bootlog_due(model_now) && bootlog_drain()==-FS_EBUSY && calls==before);
    model_now=100;CHECK(bootlog_due(model_now));
    commit_hook=pause_during_commit;
    CHECK(!bootlog_drain() && commits==1 && !snapshot().queued);
    CHECK(!memcmp(disk+BOOTLOG_PROTECTED,"ARM\ncut\n",8));
    CHECK(!bootlog_pause_drain(BOOTLOG_PAUSE_MAX_MS));
    bootlog_capture_ring("resumed",7);CHECK(bootlog_try_sync()==-FS_EAGAIN);
    before=calls;CHECK(!bootlog_due(model_now) && bootlog_drain()==-FS_EBUSY && calls==before);
    bootlog_resume_drain();bootlog_resume_drain();
    CHECK(bootlog_due(model_now) && !bootlog_drain() && commits==2);
    model_now=UINT32_MAX-50;
    CHECK(!bootlog_pause_drain(100));bootlog_capture_ring("wrap",4);
    CHECK(bootlog_try_sync()==-FS_EAGAIN);
    CHECK(!bootlog_due(model_now-1)); /* worker sampled before pause began */
    model_now=48;CHECK(!bootlog_due(model_now) && bootlog_drain()==-FS_EBUSY);
    model_now=49;CHECK(bootlog_due(model_now) && !bootlog_drain());
    CHECK(!bootlog_pause_drain(100));bootlog_capture_ring("shutdown",8);
    model_now+=100;CHECK(!bootlog_shutdown()); /* expiry also releases shutdown */
    reset_model();CHECK(!bootlog_pause_drain(100));bootlog_reset();
    CHECK(!bootlog_activate(&fs,true,1));bootlog_capture_ring("reset",5);
    CHECK(bootlog_due(model_now) && !bootlog_drain());CHECK(!bootlog_shutdown());
    puts("PASS bootlog pause: zero I/O with protected records/sync pending, in-flight exclusion, non-nesting, explicit resume, deadline/clock wrap, shutdown and reset");
}

/* f0-56: record classes, statistic budget, summary, capacity, producer bound. */
static unsigned disk_count(const char *needle)
{
    size_t l=strlen(needle);unsigned c=0;
    for (size_t i=0;i+l<=sizeof(disk);i++) if (disk[i]==(unsigned char)needle[0] && !memcmp(disk+i,needle,l)) c++;
    return c;
}
static void statistic_budget(void)
{
    char line[256];
    /* Capacity: three logs keep at least 8 MiB, batch-aligned, within budget. */
    CHECK((uint64_t)BOOTLOG_LIMIT*3>=8u*1024u*1024u && BOOTLOG_LIMIT%BOOTLOG_BATCH==0);
    CHECK((uint64_t)BOOTLOG_LIMIT*3+3*65536u<=BOOTLOG_BUDGET && BOOTLOG_RING>BOOTLOG_RAM);
    /* Summary format and the overall/per-kind limits. */
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    for (unsigned k=0;k<10;k++) for (unsigned i=0;i<8+(k==9?2:0);i++) {
        int n=snprintf(line,sizeof(line),"[cpu] k%u=%u\n",k,i);bootlog_capture_ring(line,(size_t)n);
    }
    struct bootlog_stats st=snapshot();
    CHECK(st.budget_dropped_lines==42 && !st.dropped && !st.budget_windows);
    model_now=2000;
    bootlog_capture_ring("[cpu] task=next\n",16);   /* closes the window, opens the next */
    st=snapshot();CHECK(st.budget_windows==1 && st.budget_dropped_lines==42);
    CHECK(!bootlog_shutdown());
    unsigned bytes=0;
    for (unsigned k=5;k<10;k++) bytes+=(unsigned)snprintf(line,sizeof(line),"[cpu] k%u=0\n",k)*8+(k==9?2*(unsigned)strlen("[cpu] k9=0\n"):0);
    char expect[256];
    snprintf(expect,sizeof(expect),"[bootlog] budget_dropped window_ms=2000 limit=40/8 kept=40 dropped=42 bytes=%u "
        "kinds=cpu/k5:8,cpu/k6:8,cpu/k7:8,cpu/k8:8,cpu/k9:10\n",bytes);
    CHECK(disk_count(expect)==1);
    CHECK(disk_count("[cpu] k0=7\n")==1 && disk_count("[cpu] k4=7\n")==1 && !disk_count("[cpu] k5=0\n"));
    CHECK(disk_count("[cpu] task=next\n")==1);
    /* A flood of statistics across many windows never drops protected lines. */
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    for (unsigned w=0;w<300;w++) {
        for (unsigned i=0;i<100;i++) {
            int n=snprintf(line,sizeof(line),"[cpu] task=t%u#%u class=1 flood=%u\n",i,w,i);bootlog_capture_ring(line,(size_t)n);
            n=snprintf(line,sizeof(line),"[mem] activity w=%u i=%u\n",w,i);bootlog_capture_ring(line,(size_t)n);
        }
        int n=snprintf(line,sizeof(line),"[user 46] stderr w=%04u\n",w);bootlog_capture_ring(line,(size_t)n);
        n=snprintf(line,sizeof(line),"[dosvm] lifecycle vm=1 w=%04u\n",w);bootlog_capture_ring(line,(size_t)n);
        n=snprintf(line,sizeof(line),"[accel] savage w=%04u\n",w);bootlog_capture_ring(line,(size_t)n);
        n=snprintf(line,sizeof(line),"[kernel] irqoff_long w=%04u\n",w);bootlog_capture_ring(line,(size_t)n);
        n=snprintf(line,sizeof(line),"[ata] w=%04u\n",w);bootlog_capture(line,(size_t)n);
        model_now+=500;
        if (w%4==3) CHECK(!bootlog_drain());
    }
    drain();CHECK(!bootlog_shutdown());
    st=snapshot();
    CHECK(!st.dropped_protected && !st.records_dropped_protected && st.budget_windows==75 && st.budget_dropped_lines>50000);
    /* The ring wrapped (2.75 MiB log): the last protected lines are present. */
    for (unsigned w=299;w>=290;w--) {
        snprintf(line,sizeof(line),"[user 46] stderr w=%04u\n",w);CHECK(disk_count(line)==1);
        snprintf(line,sizeof(line),"[dosvm] lifecycle vm=1 w=%04u\n",w);CHECK(disk_count(line)==1);
        snprintf(line,sizeof(line),"[kernel] irqoff_long w=%04u\n",w);CHECK(disk_count(line)==1);
    }
    CHECK(disk_count("[bootlog] budget_dropped window_ms=")>=20);
    /* Queue saturated by statistics (no drain): protected lines evict them. */
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    CHECK(!bootlog_pause_drain(BOOTLOG_PAUSE_MAX_MS));
    for (unsigned w=0;w<40;w++) {
        for (unsigned i=0;i<BOOTLOG_BUDGET_KIND_LINES;i++) {
            int n=snprintf(line,sizeof(line),"[cpu] task=%u#%u\n",w,i);bootlog_capture_ring(line,(size_t)n);
        }
        model_now+=BOOTLOG_BUDGET_WINDOW_MS;
    }
    st=snapshot();CHECK(st.ring_queued>0 && st.dropped_statistic>0 && !st.dropped_protected);
    for (unsigned i=0;i<150;i++) {
        int n=snprintf(line,sizeof(line),"[user 7] app line %04u\n",i);bootlog_capture_ring(line,(size_t)n);
    }
    st=snapshot();CHECK(!st.dropped_protected && st.dropped_statistic>0 && st.records_dropped_statistic>0);
    bootlog_resume_drain();CHECK(!bootlog_shutdown());
    for (unsigned i=0;i<150;i++) {
        snprintf(line,sizeof(line),"[user 7] app line %04u\n",i);CHECK(disk_count(line)==1);
    }
    /* Producers never wait on the disk: saturated queue, no drain, bounded time. */
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    CHECK(!bootlog_pause_drain(BOOTLOG_PAUSE_MAX_MS));
    struct timespec a,b;unsigned io_before=calls;clock_gettime(CLOCK_MONOTONIC,&a);
    for (unsigned i=0;i<200000;i++) {
        int n=snprintf(line,sizeof(line),"[user 1] flood %u\n",i);bootlog_capture_ring(line,(size_t)n);
        n=snprintf(line,sizeof(line),"[cpu] task=x#%u\n",i);bootlog_capture_ring(line,(size_t)n);
    }
    clock_gettime(CLOCK_MONOTONIC,&b);
    long us=(b.tv_sec-a.tv_sec)*1000000L+(b.tv_nsec-a.tv_nsec)/1000;
    CHECK(us/400000<50 && calls==io_before); /* mean < 50 us per call (sanitizers), zero storage I/O */
    bootlog_resume_drain();CHECK(!bootlog_shutdown());
    puts("PASS bootlog f0-56: statistic budget 40 lines/2 s (8 per kind) with one summary line, protected classes survive a flood and a saturated queue, 3 x 2.75 MiB retention, producers never block on disk");
}

static void report_snapshot(void)
{
    reset_model();
    char out[4104]; memset(out,'X',sizeof(out));
    uint32_t position,dropped; size_t length;
    for (unsigned i=1;i<=20;i++) {
        char line[64]; int n=snprintf(line,sizeof(line),"[ata] count=%u\n",i);
        bootlog_capture_ring(line,(size_t)n);
    }
    unsigned before_calls=calls, before_head=head, before_tail=tail, before_used=used;
    CHECK(!bootlog_report_snapshot(&position,&dropped,out,4096,&length));
    CHECK(length && length<=4096 && !memcmp(out,"[ata] count=1\n",14));
    out[length]=0; CHECK(strstr(out,"[ata] count=20\n"));
    CHECK(calls==before_calls && head==before_head && tail==before_tail && used==before_used);
    for (unsigned i=4096;i<sizeof(out);i++) CHECK(out[i]=='X');
    CHECK(!bootlog_report_snapshot(&position,&dropped,out,16,&length) && length==15);
    CHECK(!memcmp(out,"[ata] count=20\n",15));
    CHECK(!bootlog_report_snapshot(&position,&dropped,out,1,&length) && !length);
    stats.active=true; ring_position=BOOTLOG_PROTECTED+8192;
    CHECK(bootlog_report_snapshot(&position,&dropped,out,4096,&length) && position==BOOTLOG_PROTECTED+8192);
    stats.active=false; reset_model();
    puts("PASS bootlog report: newest whole pending slices, publication order, fixed cap, wrap position, no consume/watermark/storage mutation");
}
/* fx-5: call 73 met the storage task's drain turn on the T23 (image
 * 9a60215f) and bootlog_shutdown refused at once with EBUSY. It now waits
 * for the owner within BOOTLOG_SYNC_TIMEOUT_MS; this fixture's pause hook
 * models one millisecond per turn and ends the owner's turn on cue. */
static unsigned shutdown_pauses, release_after;
static bool advance_clock;
void bootlog_host_pause(void)
{
    shutdown_pauses++;
    if (advance_clock) model_now++;
    if (release_after && shutdown_pauses==release_after) draining=false; /* drain turn ends */
}
static void shutdown_drain_owner(void)
{
    static const char line[]="[power] power off requested by pid 2\n";
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    bootlog_capture(line,sizeof(line)-1);
    draining=true;shutdown_pauses=0;release_after=39;advance_clock=true; /* 39 ms: T23 bootlog_drain_ms */
    CHECK(!bootlog_shutdown() && shutdown_pauses==39 && !snapshot().active && !draining);
    CHECK(disk_size==sizeof(line)-1 && !memcmp(disk,line,sizeof(line)-1));
    /* An owner outliving the bound: refusal without any change, then retry. */
    reset_model();CHECK(!bootlog_activate(&fs,true,1));
    bootlog_capture(line,sizeof(line)-1);
    unsigned before=calls;
    draining=true;shutdown_pauses=0;release_after=0;
    CHECK(bootlog_shutdown()==-FS_EBUSY && shutdown_pauses==BOOTLOG_SYNC_TIMEOUT_MS && calls==before);
    CHECK(snapshot().active && snapshot().queued && !stopping && !disabled && draining && !snapshot().error);
    /* A stalled clock is bounded by turns alone. */
    model_now=0;advance_clock=false;shutdown_pauses=0;
    CHECK(bootlog_shutdown()==-FS_EBUSY && shutdown_pauses==BOOTLOG_SYNC_TIMEOUT_MS && calls==before);
    draining=false;shutdown_pauses=0;
    CHECK(!bootlog_shutdown() && !shutdown_pauses && !snapshot().active && !memcmp(disk,line,sizeof(line)-1));
    puts("bootlog: shutdown waits for the storage task's drain turn (39 ms T23 case), bounded refusal changes nothing, retry seals the log PASS");
}
int main(void) { report_snapshot();statistic_budget(); stalled_metadata();bounded_pause();class_pressure();turn_budget();commit_race();pressure_watermarks();throughput(4000000);throughput(300000);recovered_prefix(); retention(); errors(); shutdown_drain_owner(); synchronous_records();sync_bounds();overlapping_watermarks();nonblocking_records();batching_and_cadence();runtime_summaries(); return 0; }
