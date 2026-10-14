/* F4-16: 1 MiB log, protected 128 KiB prefix plus a truncation ring.
 * Deliberately no output calls: a disk failure must never recurse into logs.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/bootlog.h>
#include "../fs/vfs.h"
#ifndef FS_HOST
#include <ciuki/cpustat.h>
#endif

static struct bootlog_stats stats;
static struct vfs_table table;
static char boot[BOOTLOG_PROTECTED], ram[BOOTLOG_RAM], batch[4096];
static unsigned boot_used, boot_sent, head, used;
static uint32_t ring_position = BOOTLOG_PROTECTED;
static int handle = -1;
static bool draining, attached, disabled;
#ifdef FS_HOST
static pthread_mutex_t ram_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t lock_ram(void) { pthread_mutex_lock(&ram_lock); return 0; }
static void unlock_ram(uint32_t f) { (void)f; pthread_mutex_unlock(&ram_lock); }
#else
static uint32_t lock_ram(void) { return irq_save(); }
static void unlock_ram(uint32_t f) { irq_restore(f); }
#endif

static void capture(const char *s, size_t n, bool periodic)
{
    if (!s) return;
    /* A formatted kernel record is <=255 bytes: copy it under one lock.
     * Bulk callers use bounded 256-byte slices; producers do no disk I/O. */
    while (n) {
        size_t take = n > 256 ? 256 : n;
        uint32_t flags = lock_ram();
        if (disabled) { unlock_ram(flags); return; }
        for (size_t i = 0; i < take; i++) {
            if (!periodic && boot_used < BOOTLOG_PROTECTED)
                boot[boot_used++] = s[i];
            else {
                if (used == BOOTLOG_RAM) { head = (head + 1) % BOOTLOG_RAM; used--; stats.dropped++; }
                ram[(head + used) % BOOTLOG_RAM] = s[i]; used++;
            }
        }
        stats.captured += take;
        unlock_ram(flags); s += take; n -= take;
    }
}
void bootlog_capture(const char *s, size_t n) { capture(s, n, false); }
void bootlog_capture_ring(const char *s, size_t n) { capture(s, n, true); }
static int failed(int e)
{
    if (e && !stats.error) stats.error = e;
    if (e) { stats.active = false; disabled = true; }
    return e;
}
int bootlog_activate(struct vfs *vfs, bool qualified, uint32_t sequence)
{
    if (stats.active) return 0;
    if (!qualified || !vfs || !vfs->volumes[2] || vfs->volumes[2]->readonly) return -FS_EROFS;
    if (disabled || attached) return stats.error ? stats.error : -FS_EBUSY;
    stats.qualification_sequence = sequence;
    int e = vfs_table_init(vfs, &table, 2);
    if (e) return failed(e);
    attached = true;
    stats.storage_calls++;
    handle = vfs_open(&table, BOOTLOG_PATH, VFS_READ | VFS_WRITE | VFS_CREATE, VFS_DENY_WRITE, 0);
    if (handle < 0) { e = handle; handle = -1; return failed(e); }
    /* A qualified boot owns a fresh file. Read-only/probe reopens never
     * activate this sink, so they can still recover the previous boot. */
    stats.storage_calls++;
    e = vfs_truncate(&table, handle, 0);
    if (e) return failed(e);
    stats.size = 0; stats.active = true;
    return 0;
}
static int drain_owned(void)
{
    if (stats.error) return stats.error;
    if (!stats.active) return 0;
    /* Boot bytes cannot be displaced even before mount. Flush that prefix
     * first, then one runtime batch per turn. Copies keep IF-off work at
     * <=128 bytes. Periodic producers never consume boot capacity. */
    uint32_t flags = lock_ram();
    bool prefix = boot_sent < boot_used;
    uint32_t position = prefix ? boot_sent : ring_position;
    unlock_ram(flags);
    unsigned n = 0;
    while (n < sizeof(batch)) {
        flags = lock_ram();
        unsigned pending = prefix ? boot_used - boot_sent : used;
        unsigned take = pending < 128 ? pending : 128;
        if (take > sizeof(batch) - n) take = sizeof(batch) - n;
        for (unsigned i = 0; i < take; i++)
            batch[n++] = prefix ? boot[boot_sent + i] : ram[(head + i) % BOOTLOG_RAM];
        if (prefix) boot_sent += take;
        else { head = (head + take) % BOOTLOG_RAM; used -= take; }
        unlock_ram(flags);
        if (!take) break;
    }
    int e = 0;
    if (n && !prefix && position + n > BOOTLOG_LIMIT) {
        stats.storage_calls++;
        e = vfs_truncate(&table, handle, BOOTLOG_PROTECTED);
        if (!e) { stats.size = BOOTLOG_PROTECTED; position = BOOTLOG_PROTECTED; stats.rotations++; }
    }
    if (n && !e) {
        uint64_t at;
        stats.storage_calls++;
        e = vfs_seek(&table, handle, position, VFS_SEEK_SET, &at);
        if (!e) {
            size_t done;
            stats.storage_calls++; stats.writes++;
            if (!stats.first_write_sequence) stats.first_write_sequence = stats.qualification_sequence + 1;
            e = vfs_write(&table, handle, batch, n, &done);
            if (!e && done != n) e = -FS_EIO;
            if (!e) {
                position += (uint32_t)done;
                if (position > stats.size) stats.size = position;
                if (prefix) stats.boot_size = position;
                else ring_position = position;
            }
        }
    }
    return failed(e);
}
static bool take_drain(void)
{
    uint32_t f=lock_ram(); bool free=!draining;
    if (free) draining=true;
    unlock_ram(f); return free;
}
static void drop_drain(void)
{ uint32_t f=lock_ram(); draining=false; unlock_ram(f); }
int bootlog_drain(void)
{
    if (!take_drain()) return -FS_EBUSY;
    #ifndef FS_HOST
    uint64_t start = cpu_path_clock ? cpu_path_clock() : 0;
#endif
    int e=drain_owned(); drop_drain();
#ifndef FS_HOST
    if (cpu_path_add && cpu_path_clock) cpu_path_add(CPU_BOOTLOG, cpu_path_clock() - start);
    kpreempt_point();
#endif
    return e;
}
int bootlog_sync(bool (*cancel)(void *), void (*pause)(void *), void *ctx)
{
    if (!cancel || !pause) return -FS_EINVAL;
    while (!take_drain()) {
        if (cancel(ctx)) return -FS_EAGAIN;
        pause(ctx);
    }
    int e=0;
    /* Snapshot the producer watermark. New periodic output cannot extend the
     * synchronous operation forever. The protected prefix is drained first. */
    struct bootlog_stats before; bootlog_snapshot(&before);
    unsigned batches=(before.queued+4095)/4096+1;
    for (unsigned i=0;i<batches && stats.active && !e;i++) {
        if (cancel(ctx)) { e=-FS_EAGAIN; break; }
        struct bootlog_stats pending; bootlog_snapshot(&pending);
        if (!pending.queued) break;
        /* Avoid waiting indefinitely for a desktop/storage client. Once free,
         * the non-preemptible kernel enters the next VFS call without yielding. */
        while (!fs_lock_try_take(&table.vfs->lock)) {
            if (cancel(ctx)) { e=-FS_EAGAIN; break; }
            pause(ctx);
        }
        if (e) break;
        fs_lock_drop(&table.vfs->lock);
        e=drain_owned();
#ifndef FS_HOST
        kpreempt_point();
#endif
    }
    if (!e && stats.active && handle>=0) {
        while (!fs_lock_try_take(&table.vfs->lock)) {
            if (cancel(ctx)) { e=-FS_EAGAIN; break; }
            pause(ctx);
        }
        if (!e) {
            fs_lock_drop(&table.vfs->lock);
            stats.storage_calls++; e=vfs_commit(&table,handle);
            if (e) failed(e);
        }
    }
    if (!e && stats.error) e=stats.error;
    drop_drain(); return e;
}
int bootlog_shutdown(void)
{
    if (draining) return -FS_EBUSY;
    int result = stats.error;
    for (;;) {
        uint32_t f = lock_ram();
        bool pending = stats.active && (used || boot_sent < boot_used);
        if (!pending) disabled = true;
        unlock_ram(f);
        if (!pending) break;
        int e = bootlog_drain(); if (e) { result = e; break; }
    }
    if (handle >= 0) {
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
    return failed(result);
}
void bootlog_snapshot(struct bootlog_stats *out)
{
    if (!out) return;
    uint32_t f = lock_ram(); *out = stats;
    out->boot_queued = boot_used - boot_sent; out->ring_queued = used;
    out->queued = out->boot_queued + used; unlock_ram(f);
}
void bootlog_reset(void)
{
    if (attached || draining) return;
    memset(&stats, 0, sizeof(stats)); head = used = boot_used = boot_sent = 0;
    ring_position = BOOTLOG_PROTECTED; handle = -1; disabled = false;
}
