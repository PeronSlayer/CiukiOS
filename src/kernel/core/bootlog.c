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
    bool droppable; /* verbose or budgeted statistic: evictable by any other class */
    bool statistic; /* budgeted periodic statistic (counted apart from verbose) */
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
    if (!r->droppable || r->offset) return;
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
static void record_loss(unsigned bytes, bool verbose, bool statistic)
{
    stats.dropped+=bytes;
    if (verbose) { stats.dropped_verbose+=bytes;stats.records_dropped_verbose++; }
    else if (statistic) { stats.dropped_statistic+=bytes;stats.records_dropped_statistic++; }
    else { stats.dropped_protected+=bytes;stats.records_dropped_protected++; }
}
/* f0-56 record classes. Protected: every klog/record line and every periodic
 * line named below (application and [user N] output, DOS VM lifecycle,
 * accelerator, ATA, storage read-only/failure, entropy, interrupt-off
 * blackouts, the log's own counters); they are only lost when the RAM queue
 * is full of other protected lines. Statistic: any other klog_periodic line;
 * it gets a per-window line budget, is refused when the queue is nearly full
 * and is the first thing evicted. Suppression is reported by one summary. */
static const char *const protected_periodic[]={
    "[kernel] irqoff","[kernel] bootlog","[dosvm] lifecycle","[accel]","[ata]",
    "[storage] readonly","[storage] failure","[entropy]","[user ","[bootlog]",0};
/* Only recognised statistics are budgeted (the dosvm perf and audio transport
 * rows stay protected: low volume, rate-limited by their emitters); any other
 * periodic line stays protected, so an unknown emitter can never be suppressed by mistake. */
static const char *const statistic_periodic[]={
    "[cpu] ","[sched] ","[kernel] ","[mem] ","[storage] queue","[pager] stats",0};
static bool starts_with(const char *s, unsigned n, const char *p)
{
    unsigned i=0;
    for (;p[i];i++) if (i>=n || s[i]!=p[i]) return false;
    return true;
}
static bool protected_line(const char *s, unsigned n)
{
    for (unsigned i=0;protected_periodic[i];i++)
        if (starts_with(s,n,protected_periodic[i])) return true;
    for (unsigned i=0;statistic_periodic[i];i++)
        if (starts_with(s,n,statistic_periodic[i])) return false;
    return true;
}
#define BUDGET_KINDS 16u
#define BUDGET_NAME 24u
static struct {
    bool open;
    uint32_t start;
    unsigned lines, dropped_lines, dropped_bytes, kinds;
    struct { char name[BUDGET_NAME]; unsigned seen, dropped; } kind[BUDGET_KINDS];
} budget;
/* "[cpu] task=x" -> "cpu/task"; unknown shapes share the last slot. */
static void kind_name(const char *s, unsigned n, char *out)
{
    unsigned o=0,i=0;
    if (n && s[0]=='[') {
        for (i=1;i<n && s[i]!=']' && o<12;i++) out[o++]=s[i];
        while (i<n && s[i]!=']') i++;
        if (i<n) i++;
        while (i<n && s[i]==' ') i++;
        out[o++]='/';
        for (;i<n && s[i]!=' ' && s[i]!='=' && s[i]!='\n' && o<BUDGET_NAME-1;i++) out[o++]=s[i];
    } else out[o++]='?';
    out[o]=0;
}
static unsigned kind_find(const char *name)
{
    for (unsigned k=0;k<budget.kinds;k++) {
        unsigned i=0;
        while (name[i] && name[i]==budget.kind[k].name[i]) i++;
        if (name[i]==budget.kind[k].name[i]) return k;
    }
    if (budget.kinds<BUDGET_KINDS-1) {
        unsigned k=budget.kinds++,i=0;
        for (;name[i];i++) budget.kind[k].name[i]=name[i];
        budget.kind[k].name[i]=0;
        return k;
    }
    if (budget.kinds==BUDGET_KINDS-1) {
        const char *other="other";
        budget.kinds=BUDGET_KINDS;
        for (unsigned i=0;i<6;i++) budget.kind[BUDGET_KINDS-1].name[i]=other[i];
    }
    return BUDGET_KINDS-1;
}
static void put_str(char *b, unsigned *o, const char *t)
{ while (*t && *o<BOOTLOG_RECORD_BYTES-2) b[(*o)++]=*t++; }
static void put_num(char *b, unsigned *o, unsigned v)
{
    char t[10];unsigned n=0;
    do { t[n++]=(char)('0'+v%10);v/=10; } while (v);
    while (n && *o<BOOTLOG_RECORD_BYTES-2) b[(*o)++]=t[--n];
}
static void admit_locked(const char *s, unsigned take, bool periodic, bool verbose, bool statistic);
/* RAM lock held. Emits the summary of a closed window, then resets it. */
static void budget_close_locked(uint32_t now)
{
    if (budget.dropped_lines) {
        char b[BOOTLOG_RECORD_BYTES];unsigned o=0;
        put_str(b,&o,"[bootlog] budget_dropped window_ms=");put_num(b,&o,now-budget.start);
        put_str(b,&o," limit=");put_num(b,&o,BOOTLOG_BUDGET_LINES);
        put_str(b,&o,"/");put_num(b,&o,BOOTLOG_BUDGET_KIND_LINES);
        put_str(b,&o," kept=");put_num(b,&o,budget.lines);
        put_str(b,&o," dropped=");put_num(b,&o,budget.dropped_lines);
        put_str(b,&o," bytes=");put_num(b,&o,budget.dropped_bytes);
        put_str(b,&o," kinds=");
        bool first=true;
        for (unsigned k=0;k<budget.kinds;k++) if (budget.kind[k].dropped) {
            if (!first) put_str(b,&o,",");
            first=false;put_str(b,&o,budget.kind[k].name);put_str(b,&o,":");put_num(b,&o,budget.kind[k].dropped);
        }
        b[o++]='\n';
        stats.budget_windows++;
        admit_locked(b,o,true,false,false);
        stats.captured+=o;
    }
    budget.open=false;budget.lines=budget.dropped_lines=budget.dropped_bytes=budget.kinds=0;
    for (unsigned k=0;k<BUDGET_KINDS;k++) budget.kind[k].name[0]=0,budget.kind[k].seen=budget.kind[k].dropped=0;
}
/* RAM lock held. True when this statistic line is within its window budget. */
static bool budget_admit(const char *s, unsigned take)
{
    uint32_t now=fs_now_ms();
    if (budget.open && (uint32_t)(now-budget.start)>=BOOTLOG_BUDGET_WINDOW_MS) budget_close_locked(now);
    if (!budget.open) { budget.open=true;budget.start=now; }
    char name[BUDGET_NAME];kind_name(s,take,name);
    unsigned k=kind_find(name);
    if (budget.lines>=BOOTLOG_BUDGET_LINES || budget.kind[k].seen>=BOOTLOG_BUDGET_KIND_LINES) {
        budget.kind[k].dropped++;budget.dropped_lines++;budget.dropped_bytes+=take;
        stats.budget_dropped_lines++;
        return false;
    }
    budget.lines++;budget.kind[k].seen++;
    return true;
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
/* RAM lock held. One record slice (<=256 bytes), admitted or lost as a whole. */
static void admit_locked(const char *s, unsigned take, bool periodic, bool verbose, bool statistic)
{
    bool droppable=verbose || statistic;
    unsigned prefix=!periodic && boot_used<boot_capacity ? boot_capacity-boot_used : 0;
    if (prefix>=take) prefix=take;else prefix=0;
    if (prefix) { memcpy(boot+boot_used,s,prefix);boot_used+=prefix; }
    unsigned bytes=take-prefix;
    if (bytes) {
        uint64_t begin=ring_captured;ring_captured+=bytes;
        /* A statistic never takes the last slots: they stay for evidence. */
        if (statistic && free_count<=BOOTLOG_STATISTIC_RESERVE) {
            record_loss(bytes,false,true);
            return;
        }
        if (!free_count && diag_head!=NONE) {
            unsigned slot=diag_head;
            record_loss(records[slot].length,records[slot].verbose,records[slot].statistic);
            remove_record(slot);
        }
        if (!free_count) record_loss(bytes,verbose,statistic);
        else {
            unsigned slot=free_slots[--free_count];
            struct log_record *r=&records[slot];
            *r=(struct log_record){.begin=begin,.length=bytes,.next=NONE,.prev=tail,
                .diag_next=NONE,.diag_prev=NONE,.verbose=verbose,.droppable=droppable,.statistic=statistic};
            memcpy(ram[slot],s+prefix,bytes);
            if (tail!=NONE) records[tail].next=slot;else head=slot;
            tail=slot;record_count++;used+=bytes;
            if (droppable) {
                r->diag_prev=diag_tail;
                if (diag_tail!=NONE) records[diag_tail].diag_next=slot;else diag_head=slot;
                diag_tail=slot;
            }
        }
    }
}
static void capture(const char *s, size_t n, bool periodic, bool verbose)
{
    if (!s) return;
    /* Formatted output is <=255 bytes and is admitted/evicted as a whole.
     * Bulk callers use bounded slices; producers never perform disk I/O.
     * Each slice holds the RAM lock for one memcpy of <=256 bytes. */
    bool statistic=periodic && !verbose && n<=BOOTLOG_RECORD_BYTES && !protected_line(s,(unsigned)n);
    while (n) {
        unsigned take=n>BOOTLOG_RECORD_BYTES ? BOOTLOG_RECORD_BYTES : (unsigned)n;
        uint32_t flags=lock_ram();
        if (disabled) { unlock_ram(flags);return; }
        init_slots();
        if (statistic && !budget_admit(s,take)) { unlock_ram(flags);return; }
        admit_locked(s,take,periodic,verbose,statistic);
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
     * without monopolizing that task; its next turn retries. This also delays
     * a due metadata publication. The zero-initialized reserved chain must
     * remain recoverable at mount without relying on that cadence (f0-36). */
    if (stats.active && !fs_lock_try_take(&table.vfs->lock)) {
        drop_drain(); return -FS_EBUSY;
    }
    if (stats.active) fs_lock_drop(&table.vfs->lock);
#ifndef FS_HOST
    uint64_t start = cpu_path_clock ? cpu_path_clock() : 0;
#endif
    uint32_t f=lock_ram();
    if (budget.open && (uint32_t)(fs_now_ms()-budget.start)>=BOOTLOG_BUDGET_WINDOW_MS)
        budget_close_locked(fs_now_ms());
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
#ifdef FS_HOST
void bootlog_host_pause(void) __attribute__((weak)); /* host fixture clock/owner step */
static void shutdown_pause(void)
{
    if (bootlog_host_pause) { bootlog_host_pause(); return; }
    const struct timespec pause={0,1000000}; nanosleep(&pause,0);
}
#else
static void shutdown_pause(void) { task_sleep_ms(1); }
#endif
int bootlog_shutdown(void)
{
#ifndef FS_HOST
    if (!(read_eflags() & 0x200)) return -FS_EBUSY;
#endif
    /* fx-5: the storage task's drain turn is transient (bounded batches; at
     * most 39 ms on the T23 trial). Refusing it with EBUSY failed the first
     * call-73 flush of image 9a60215f. Wait for the owner, as
     * blackbox_checkpoint waits for its flusher, with a finite bound; only
     * an owner that outlives it (a probe pause) still refuses, unchanged. */
    uint32_t start=fs_now_ms();
    for (unsigned turn=0;!take_drain();turn++) {
        if (turn>=BOOTLOG_SYNC_TIMEOUT_MS || (uint32_t)(fs_now_ms()-start)>=BOOTLOG_SYNC_TIMEOUT_MS)
            return -FS_EBUSY;
        shutdown_pause();
    }
    stopping = true;
    { uint32_t f=lock_ram();budget_close_locked(fs_now_ms());unlock_ram(f); }
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
bool bootlog_report_snapshot(uint32_t *position, uint32_t *dropped,
                             char *pending, size_t capacity, size_t *length)
{
    if (!position || !dropped || !pending || !length) return false;
    if (capacity>4096) capacity=4096;
    uint32_t f=lock_ram();
    *position=boot_sent<boot_used || history_sent<history_size ? history_sent+boot_sent : ring_position;
    *dropped=stats.records_dropped_protected+stats.records_dropped_verbose+stats.records_dropped_statistic;
    *length=0;
    /* Select the newest whole retained slices, then copy in publication
     * order. Do not consume slots, change watermarks or touch disk state. */
    unsigned first=NONE;
    size_t bytes=0;
    for (unsigned i=tail, visited=0;i!=NONE && visited<BOOTLOG_RECORDS;visited++) {
        if (records[i].length>capacity-bytes) break;
        bytes+=records[i].length; first=i; i=records[i].prev;
    }
    for (unsigned i=first, visited=0;i!=NONE && visited<BOOTLOG_RECORDS;visited++) {
        memcpy(pending+*length,ram[i],records[i].length);
        *length+=records[i].length; i=records[i].next;
    }
    if (!*length && boot_used>boot_sent) {
        size_t n=boot_used-boot_sent; if (n>capacity) n=capacity;
        memcpy(pending,boot+boot_used-n,n); *length=n;
    }
    bool active=stats.active;
    unlock_ram(f); return active;
}
void bootlog_reset(void)
{
    if (attached || draining) return;
    memset(&stats, 0, sizeof(stats)); used=boot_used=boot_sent=record_count=free_count=0;
    head=tail=diag_head=diag_tail=NONE;slots_ready=pressure=false;ring_captured=0;
    memset(&budget,0,sizeof(budget));
    history=0;history_size=history_sent=0;boot_capacity=BOOTLOG_PROTECTED;
    ring_position = BOOTLOG_PROTECTED; handle = -1; disabled = false;
    stopping=metadata_dirty=drain_started=false; last_drain=last_metadata=0;
    drain_paused=false;pause_start=pause_timeout=0;
    sync_target=sync_done=sync_commit_target=sync_ring=sync_commit_ring=0;
    sync_boot=sync_commit_boot=0;
    memset(sync_waiters,0,sizeof(sync_waiters));
}
