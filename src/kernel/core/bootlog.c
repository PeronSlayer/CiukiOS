/* Three boots within 1 MiB, each with a protected prefix and truncation ring.
 * Deliberately no output calls: a disk failure must never recurse into logs.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/bootlog.h>
#include <ciuki/blackbox.h>
#pragma weak blackbox_replay
#include "../fs/vfs.h"
#ifndef FS_HOST
#include <ciuki/cpustat.h>
#endif

static struct bootlog_stats stats;
static struct vfs_table table;
static char boot[BOOTLOG_PROTECTED], ram[BOOTLOG_RECORDS][BOOTLOG_RECORD_BYTES], batch[BOOTLOG_BATCH];
#define NONE BOOTLOG_RECORDS
struct log_record {
    uint64_t begin;
    unsigned next, prev, diag_next, diag_prev;
    unsigned length, offset;
    bool verbose;
};
static struct log_record records[BOOTLOG_RECORDS];
static unsigned boot_used, boot_sent, used, record_count, free_count;
static unsigned head=NONE, tail=NONE, diag_head=NONE, diag_tail=NONE;
static unsigned free_slots[BOOTLOG_RECORDS];
static bool slots_ready, pressure;
static uint64_t ring_captured;
static const char *history;
static unsigned history_size,history_sent,boot_capacity=BOOTLOG_PROTECTED;
static uint32_t ring_position = BOOTLOG_PROTECTED;
static int handle = -1;
static bool draining, attached, disabled;
static bool stopping, metadata_dirty, drain_started;
static uint32_t last_drain, last_metadata;
static bool drain_paused;
static uint32_t pause_start, pause_timeout;
static uint64_t sync_target, sync_done, sync_commit_target;
static unsigned sync_boot, sync_commit_boot;
static uint64_t sync_ring, sync_commit_ring;
#define SYNC_WAITERS 16u /* room for every controller worker and overlapping callers */
static uint64_t sync_waiters[SYNC_WAITERS], waiter_ring[SYNC_WAITERS];
static unsigned waiter_boot[SYNC_WAITERS];
/* Called under the RAM lock. A newer binder cannot move an older binder's
 * completion point. Timed-out requests still receive an eventual commit. */
static void sync_next_commit(void)
{
    sync_commit_target=0;
    for (unsigned i=0;i<SYNC_WAITERS;i++) {
        uint64_t target=sync_waiters[i];
        if (target>sync_done && (!sync_commit_target || target<sync_commit_target))
            { sync_commit_target=target;sync_commit_boot=waiter_boot[i];sync_commit_ring=waiter_ring[i]; }
    }
    if (!sync_commit_target && sync_target>sync_done) {
        sync_commit_target=sync_target;sync_commit_boot=sync_boot;sync_commit_ring=sync_ring;
    }
}
#ifdef FS_HOST
static pthread_mutex_t ram_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t lock_ram(void) { pthread_mutex_lock(&ram_lock); return 0; }
static void unlock_ram(uint32_t f) { (void)f; pthread_mutex_unlock(&ram_lock); }
#else
static uint32_t lock_ram(void) { return irq_save(); }
static void unlock_ram(uint32_t f) { irq_restore(f); }
#endif

/* All queue helpers run under the bounded RAM lock. No payload compaction. */
static void init_slots(void)
{
    if (slots_ready) return;
    for (unsigned i=0;i<BOOTLOG_RECORDS;i++) free_slots[i]=i;
    free_count=BOOTLOG_RECORDS;slots_ready=true;
}
static void unlink_diag(unsigned slot)
{
    struct log_record *r=&records[slot];
    if (!r->verbose || r->offset) return;
    if (r->diag_prev!=NONE) records[r->diag_prev].diag_next=r->diag_next;
    else diag_head=r->diag_next;
    if (r->diag_next!=NONE) records[r->diag_next].diag_prev=r->diag_prev;
    else diag_tail=r->diag_prev;
}
static void remove_record(unsigned slot)
{
    struct log_record *r=&records[slot];
    unlink_diag(slot);
    if (r->prev!=NONE) records[r->prev].next=r->next;
    else head=r->next;
    if (r->next!=NONE) records[r->next].prev=r->prev;
    else tail=r->prev;
    used-=r->length-r->offset;record_count--;
    free_slots[free_count++]=slot;
}
static void record_loss(unsigned bytes, bool verbose)
{
    stats.dropped+=bytes;
    if (verbose) { stats.dropped_verbose+=bytes;stats.records_dropped_verbose++; }
    else { stats.dropped_protected+=bytes;stats.records_dropped_protected++; }
}
static bool high_water(void)
{
    return history_size-history_sent+boot_used-boot_sent+used>=BOOTLOG_HIGH_WATER ||
           record_count>=BOOTLOG_RECORDS*3/4;
}
static uint64_t ring_done(void)
{
    return head==NONE ? ring_captured : records[head].begin+records[head].offset;
}
static bool sync_ready(void)
{
    return sync_commit_target && history_sent==history_size &&
           boot_sent>=sync_commit_boot && ring_done()>=sync_commit_ring;
}
static void capture(const char *s, size_t n, bool periodic, bool verbose)
{
    if (!s) return;
    /* Formatted output is <=255 bytes and is admitted/evicted as a whole.
     * Bulk callers use bounded slices; producers never perform disk I/O. */
    while (n) {
        unsigned take=n>BOOTLOG_RECORD_BYTES ? BOOTLOG_RECORD_BYTES : (unsigned)n;
        uint32_t flags=lock_ram();
        if (disabled) { unlock_ram(flags);return; }
        init_slots();
        unsigned prefix=!periodic && boot_used<boot_capacity ? boot_capacity-boot_used : 0;
        if (prefix>=take) prefix=take;else prefix=0;
        if (prefix) { memcpy(boot+boot_used,s,prefix);boot_used+=prefix; }
        unsigned bytes=take-prefix;
        if (bytes) {
            uint64_t begin=ring_captured;ring_captured+=bytes;
            if (!free_count && diag_head!=NONE) {
                unsigned slot=diag_head;
                record_loss(records[slot].length,true);remove_record(slot);
            }
            if (!free_count) record_loss(bytes,verbose);
            else {
                unsigned slot=free_slots[--free_count];
                struct log_record *r=&records[slot];
                *r=(struct log_record){.begin=begin,.length=bytes,.next=NONE,.prev=tail,
                    .diag_next=NONE,.diag_prev=NONE,.verbose=verbose};
                memcpy(ram[slot],s+prefix,bytes);
                if (tail!=NONE) records[tail].next=slot;else head=slot;
                tail=slot;record_count++;used+=bytes;
                if (verbose) {
                    r->diag_prev=diag_tail;
                    if (diag_tail!=NONE) records[diag_tail].diag_next=slot;else diag_head=slot;
                    diag_tail=slot;
                }
            }
        }
        stats.captured+=take;
        if (high_water()) pressure=true;
        unlock_ram(flags);s+=take;n-=take;
    }
}
void bootlog_capture(const char *s, size_t n) { capture(s,n,false,false); }
void bootlog_capture_ring(const char *s, size_t n) { capture(s,n,true,false); }
void bootlog_capture_verbose(const char *s, size_t n) { capture(s,n,true,true); }
static int failed(int e)
{
    if (e && !stats.error) stats.error = e;
    if (e) { stats.active = false; disabled = true; }
    return e;
}
int bootlog_activate(struct vfs *vfs, bool qualified, uint32_t sequence)
{
#ifndef FS_HOST
    if (!(read_eflags() & 0x200)) return -FS_EBUSY;
#endif
    if (stats.active) return 0;
    if (!qualified || !vfs || !vfs->volumes[2] || vfs->volumes[2]->readonly) return -FS_EROFS;
    if (disabled || attached) return stats.error ? stats.error : -FS_EBUSY;
    stats.qualification_sequence = sequence;
    int e = vfs_table_init(vfs, &table, 2);
    if (e) return failed(e);
    attached = true;
    stats.storage_calls++;
    e = vfs_log_rotate(&table, BOOTLOG_PATH, BOOTLOG_PREVIOUS, BOOTLOG_OLDEST, BOOTLOG_LIMIT);
    if (e) return failed(e);
    stats.storage_calls++;
    handle = vfs_open(&table, BOOTLOG_PATH, VFS_READ | VFS_WRITE | VFS_CREATE, VFS_DENY_WRITE, 0);
    if (handle < 0) { e = handle; handle = -1; return failed(e); }
    /* Previous boots now have separate names and finalized reservations. */
    stats.storage_calls++;
    e = vfs_log_reserve(&table, handle, BOOTLOG_LIMIT);
    if (e) return failed(e);
    last_metadata = fs_now_ms();
    /* Replay is immutable until storage teardown. Keep it as a separate
     * prefix stream so activation never moves 128KiB with interrupts off. */
    if (blackbox_replay) {
        size_t length;history=blackbox_replay(&length);
        if (length>BOOTLOG_PROTECTED/2) {
            size_t drop=length-BOOTLOG_PROTECTED/2;
            while (drop<length && history[drop-1]!='\n') drop++;
            history+=drop;length-=drop;
        }
        uint32_t f=lock_ram();
        history_size=(unsigned)length;boot_capacity=BOOTLOG_PROTECTED-history_size;
        if (boot_used>boot_capacity) {
            unsigned lost=boot_used-boot_capacity;
            boot_used=boot_capacity;
            /* Legacy replay-prefix trimming has no record descriptors. */
            stats.dropped+=lost;stats.dropped_protected+=lost;
            /* Pending prefix targets refer to bytes retained after replay. */
            if (sync_boot>boot_used) sync_boot=boot_used;
            for (unsigned i=0;i<SYNC_WAITERS;i++)
                if (waiter_boot[i]>boot_used) waiter_boot[i]=boot_used;
            sync_next_commit();
        }
        stats.captured+=length;unlock_ram(f);
    }
    stats.size = 0; stats.active = true;
    return 0;
}
static int drain_owned(void)
{
    if (stats.error) return stats.error;
    if (!stats.active) return 0;
    /* Keep blocks aligned and >=32 KiB even for a sparse one-second tail.
     * Preserve the preceding bytes in a partly written block, then zero its
     * suffix. The published logical length excludes all padding. */
    uint32_t flags = lock_ram();
    unsigned prefix_end=!stopping && sync_commit_target && sync_commit_boot<boot_used ? sync_commit_boot : boot_used;
    bool prefix = history_sent<history_size || boot_sent < prefix_end;
    uint32_t position = prefix ? history_sent+boot_sent : ring_position;
    bool syncing=!stopping && sync_commit_target!=0;
    unsigned boot_target=sync_commit_boot;
    uint64_t ring_target=sync_commit_ring;
    unlock_ram(flags);
    bool rotate = !prefix && position == BOOTLOG_LIMIT;
    if (rotate) position = BOOTLOG_PROTECTED;
    unsigned offset = position % BOOTLOG_BATCH;
    uint32_t base = position - offset;
    unsigned limit = sizeof(batch) - offset;

    memset(batch, 0, sizeof(batch));
    int e = 0;
    if (offset) {
        uint64_t at; size_t got;
        stats.storage_calls++;
        e = vfs_seek(&table, handle, base, VFS_SEEK_SET, &at);
        if (!e) {
            stats.storage_calls++;
            e = vfs_read(&table, handle, batch, offset, &got);
            if (!e && got != offset) e = -FS_EIO;
        }
        if (e) return failed(e);
    }
    unsigned n = 0;
    while (n < limit) {
        flags = lock_ram();
        bool replaying=prefix && history_sent<history_size;
        unsigned pending;
        if (prefix) {
            unsigned end=syncing && boot_target<boot_used ? boot_target : boot_used;
            pending=replaying ? history_size-history_sent : end-boot_sent;
        } else {
            pending=head==NONE ? 0 : records[head].length-records[head].offset;
            uint64_t done=ring_done();
            if (syncing && done>=ring_target) pending=0;
            else if (syncing && ring_target-done<pending) pending=(unsigned)(ring_target-done);
        }
        unsigned take=pending<128 ? pending : 128;
        if (take>limit-n) take=limit-n;
        if (take) {
            if (prefix) {
                memcpy(batch+offset+n,replaying ? history+history_sent : boot+boot_sent,take);
                if (replaying) history_sent+=take;else boot_sent+=take;
            } else {
                struct log_record *r=&records[head];
                /* Once any bytes enter a batch, pin the remainder. */
                unlink_diag(head);
                memcpy(batch+offset+n,ram[head]+r->offset,take);
                r->offset+=take;used-=take;
                if (r->offset==r->length) remove_record(head);
            }
            n+=take;
        }
        unlock_ram(flags);
        if (!take) break;
    }
    if (n && !e) {
        uint64_t at;
        stats.storage_calls++;
        e = vfs_seek(&table, handle, base, VFS_SEEK_SET, &at);
        if (!e) {
            size_t done;
            stats.storage_calls++; stats.writes++;
            if (!stats.first_write_sequence) stats.first_write_sequence = stats.qualification_sequence + 1;
            uint32_t size = rotate ? BOOTLOG_PROTECTED : stats.size;
            if (position + n > size) size = position + n;
            e = vfs_log_write(&table, handle, batch, sizeof(batch), size, &done);
            if (!e && done != sizeof(batch)) e = -FS_EIO;
            if (!e) {
                position += n;
                stats.size = size; stats.bytes_written += done;
                if (rotate) stats.rotations++;
                metadata_dirty = true;
                if (prefix) stats.boot_size = position;
                else ring_position = position;
            }
        }
    }
    return failed(e);
}
/* RAM lock held. Expiry never changes capture, pacing or sync watermarks. */
static bool paused(uint32_t now)
{
    if (drain_paused && (int32_t)(now-(pause_start+pause_timeout))>=0)
        drain_paused=false;
    return drain_paused;
}
int bootlog_pause_drain(uint32_t timeout_ms)
{
    if (!timeout_ms || timeout_ms>BOOTLOG_PAUSE_MAX_MS) return -FS_EINVAL;
    uint32_t now=fs_now_ms(), f=lock_ram();
    int e=draining || paused(now) ? -FS_EBUSY : 0;
    if (!e) { drain_paused=true;pause_start=now;pause_timeout=timeout_ms; }
    unlock_ram(f);return e;
}
void bootlog_resume_drain(void)
{
    uint32_t f=lock_ram();drain_paused=false;unlock_ram(f);
}
static bool take_drain(void)
{
    uint32_t now=fs_now_ms(), f=lock_ram(); bool free=!draining && !paused(now);
    if (free) draining=true;
    unlock_ram(f); return free;
}
static void drop_drain(void)
{ uint32_t f=lock_ram(); draining=false; unlock_ram(f); }
bool bootlog_due(uint32_t now)
{
    uint32_t f = lock_ram();
    uint32_t pending = history_size-history_sent + boot_used - boot_sent + used;
    bool due = !paused(now) && stats.active && (stopping || sync_target > sync_done ||
        (pending && (!drain_started || pressure || high_water() ||
                     (uint32_t)(now - last_drain) >= BOOTLOG_DRAIN_MS)) ||
        (metadata_dirty && (uint32_t)(now - last_metadata) >= BOOTLOG_METADATA_MS));
    unlock_ram(f); return due;
}
int bootlog_drain(void)
{
#ifndef FS_HOST
    /* Never inherit an IRQ lock into FAT, cache barriers or ATA waits. */
    if (!(read_eflags() & 0x200)) return -FS_EBUSY;
#endif
    if (!take_drain()) return -FS_EBUSY;
    uint32_t now = fs_now_ms();
    if (!bootlog_due(now)) {
        int e = stats.error; drop_drain(); return e;
    }
    /* The storage task alone does runtime VFS work. Skip a contended client
     * without monopolizing that task; its next turn retries. */
    if (stats.active && !fs_lock_try_take(&table.vfs->lock)) {
        drop_drain(); return -FS_EBUSY;
    }
    if (stats.active) fs_lock_drop(&table.vfs->lock);
#ifndef FS_HOST
    uint64_t start = cpu_path_clock ? cpu_path_clock() : 0;
#endif
    uint32_t f=lock_ram();
    if (high_water()) pressure=true;
    unlock_ram(f);
    uint32_t turn_start=fs_now_ms();
    int e=0;
    bool commit=false;
    uint64_t committed_target=0;
    for (unsigned block=0;block<BOOTLOG_TURN_BLOCKS;block++) {
        e=drain_owned();
        uint32_t f=lock_ram();
        commit=!e && stats.active && sync_ready();
        if (commit) committed_target=sync_commit_target;
        bool pending=history_sent<history_size || boot_sent<boot_used || used;
        if (!pending) pressure=false;
        bool again=!e && !commit && pressure && pending;
        unlock_ram(f);
        if (!again || (uint32_t)(fs_now_ms()-turn_start)>=BOOTLOG_TURN_MS) break;
#ifndef FS_HOST
        kpreempt_point();
#endif
    }
    last_drain = fs_now_ms(); drain_started = true;
    bool publish = !e && stats.active && metadata_dirty &&
        (commit || (uint32_t)(fs_now_ms()-last_metadata) >= BOOTLOG_METADATA_MS);
    if (publish) {
        stats.storage_calls++; stats.metadata_writes++;
        e=vfs_log_publish(&table,handle,false);
        if (!e) { metadata_dirty=false; last_metadata=fs_now_ms(); }
    }
    if (commit) {
        if (!e) { stats.storage_calls++; e=vfs_commit(&table,handle); }
        f=lock_ram();
        if (!e) {
            sync_done=committed_target;
            sync_next_commit();
        }
        unlock_ram(f);
        if (e) failed(e);
    }
    if (e) failed(e);
    drop_drain();
#ifndef FS_HOST
    if (cpu_path_add && cpu_path_clock) cpu_path_add(CPU_BOOTLOG, cpu_path_clock() - start);
    kpreempt_point();
#endif
    return e;
}
int bootlog_try_sync(void)
{
    uint32_t f=lock_ram();
    int result=stats.error;
    if (!result && !stats.active) result=-FS_EAGAIN;
    if (!result && sync_done<stats.captured) {
        if (stats.captured>sync_target) {
            sync_target=stats.captured;sync_boot=boot_used;sync_ring=ring_captured;
        }
        sync_next_commit();
        result=-FS_EAGAIN;
    }
    unlock_ram(f); return result;
}
int bootlog_sync(bool (*cancel)(void *), void (*pause)(void *), void *ctx)
{
    if (!cancel || !pause) return -FS_EINVAL;
    uint32_t f=lock_ram();
    if (stats.error || !stats.active || sync_done>=stats.captured) {
        int error=stats.error;unlock_ram(f);return error;
    }
    unsigned slot=0;
    while (slot<SYNC_WAITERS && sync_waiters[slot]) slot++;
    if (slot==SYNC_WAITERS) { unlock_ram(f);return -FS_EBUSY; }
    uint64_t target=stats.captured;
    sync_waiters[slot]=target;waiter_boot[slot]=boot_used;waiter_ring[slot]=ring_captured;
    if (target>sync_target) { sync_target=target;sync_boot=boot_used;sync_ring=ring_captured; }
    sync_next_commit();
    unlock_ram(f);
    uint32_t start=fs_now_ms();
    int result=-FS_EAGAIN;
    /* No VFS calls, drain ownership or reservations on a binder's stack.
     * An independent deadline also bounds a broken/non-cancelling caller. */
    for (unsigned turn=0;turn<BOOTLOG_SYNC_TIMEOUT_MS;turn++) {
        f=lock_ram();
        int error=stats.error;
        bool done=!stats.active || sync_done>=target;
        unlock_ram(f);
        if (error || done) { result=error;break; }
        if (cancel(ctx) || (uint32_t)(fs_now_ms()-start)>=BOOTLOG_SYNC_TIMEOUT_MS)
            break;
        pause(ctx);
    }
    f=lock_ram();sync_waiters[slot]=0;sync_next_commit();unlock_ram(f);
    return result;
}
int bootlog_shutdown(void)
{
#ifndef FS_HOST
    if (!(read_eflags() & 0x200)) return -FS_EBUSY;
#endif
    if (!take_drain()) return -FS_EBUSY;
    stopping = true;
    int result = stats.error;
    for (;;) {
        uint32_t f = lock_ram();
        bool pending = stats.active && (used || history_sent<history_size || boot_sent < boot_used);
        if (!pending) disabled = true;
        unlock_ram(f);
        if (!pending) break;
        int e = drain_owned(); if (e) { result = e; break; }
    }
    if (handle >= 0) {
        if (!result) {
            stats.storage_calls++; stats.metadata_writes++;
            result = vfs_log_publish(&table, handle, true);
        }
        stats.storage_calls++;
        int e = vfs_commit(&table, handle);
        if (e && !result) result = e;
        stats.storage_calls++;
        e = vfs_close(&table, handle);
        if (e && !result) result = e;
        handle = -1;
    }
    if (attached) { vfs_table_destroy(&table); attached = false; }
    stats.active = false;
    stopping = metadata_dirty = false;
    drop_drain();
    return failed(result);
}
void bootlog_snapshot(struct bootlog_stats *out)
{
    if (!out) return;
    uint32_t f = lock_ram(); *out = stats;
    out->boot_queued = history_size-history_sent+boot_used - boot_sent; out->ring_queued = used;
    out->queued = out->boot_queued + used;
    /* mount.c's existing storage task polls active && queued. A request to
     * commit bytes already drained must still cause one storage turn. */
    if (stats.active && !out->queued && sync_target>sync_done) out->queued=1;
    unlock_ram(f);
}
void bootlog_reset(void)
{
    if (attached || draining) return;
    memset(&stats, 0, sizeof(stats)); used=boot_used=boot_sent=record_count=free_count=0;
    head=tail=diag_head=diag_tail=NONE;slots_ready=pressure=false;ring_captured=0;
    history=0;history_size=history_sent=0;boot_capacity=BOOTLOG_PROTECTED;
    ring_position = BOOTLOG_PROTECTED; handle = -1; disabled = false;
    stopping=metadata_dirty=drain_started=false; last_drain=last_metadata=0;
    drain_paused=false;pause_start=pause_timeout=0;
    sync_target=sync_done=sync_commit_target=sync_ring=sync_commit_ring=0;
    sync_boot=sync_commit_boot=0;
    memset(sync_waiters,0,sizeof(sync_waiters));
}
