/* F1: 128 KiB log at SYSTEM/LOGS/BOOT.LOG, ring by durable truncation.
 * Deliberately no output calls: a disk failure must never recurse into logs.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/bootlog.h>
#include "../fs/vfs.h"

static struct bootlog_stats stats;
static struct vfs_table table;
static char ram[BOOTLOG_RAM], batch[4096];
static unsigned head, used;
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

void bootlog_capture(const char *s, size_t n)
{
    if (!s || disabled) return;
    /* Each <=128-byte IF-off copy is bounded; producers do not do disk I/O. */
    while (n) {
        size_t take = n > 128 ? 128 : n;
        uint32_t flags = lock_ram();
        for (size_t i = 0; i < take; i++) {
            if (used == BOOTLOG_RAM) { head = (head + 1) % BOOTLOG_RAM; used--; stats.dropped++; }
            ram[(head + used) % BOOTLOG_RAM] = s[i]; used++;
        }
        stats.captured += take;
        unlock_ram(flags); s += take; n -= take;
    }
}
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
    uint64_t position;
    stats.storage_calls++;
    e = vfs_seek(&table, handle, 0, VFS_SEEK_END, &position);
    if (e) return failed(e);
    if (position > BOOTLOG_LIMIT) {
        stats.storage_calls++;
        e = vfs_truncate(&table, handle, 0);
        if (e) return failed(e);
        stats.storage_calls++;
        e = vfs_seek(&table, handle, 0, VFS_SEEK_SET, &position);
        if (e) return failed(e);
        stats.rotations++;
    }
    stats.size = (uint32_t)position; stats.active = true;
    return 0;
}
int bootlog_drain(void)
{
    if (stats.error) return stats.error;
    if (!stats.active) return 0;
    if (draining) return -FS_EBUSY;
    draining = true;
    /* One bounded batch per worker turn, including logs emitted during I/O. */
    unsigned n = 0;
    while (n < sizeof(batch)) {
        uint32_t flags = lock_ram();
        unsigned take = used < 128 ? used : 128;
        if (take > sizeof(batch) - n) take = sizeof(batch) - n;
        for (unsigned i = 0; i < take; i++) batch[n++] = ram[(head + i) % BOOTLOG_RAM];
        head = (head + take) % BOOTLOG_RAM; used -= take;
        unlock_ram(flags);
        if (!take) break;
    }
    int e = 0;
    if (n && stats.size + n > BOOTLOG_LIMIT) {
        stats.storage_calls++;
        e = vfs_truncate(&table, handle, 0);
        uint64_t position;
        if (!e) { stats.storage_calls++; e = vfs_seek(&table, handle, 0, VFS_SEEK_SET, &position); }
        if (!e) { stats.size = 0; stats.rotations++; }
    }
    if (n && !e) {
        size_t done;
        stats.storage_calls++; stats.writes++;
        if (!stats.first_write_sequence) stats.first_write_sequence = stats.qualification_sequence + 1;
        e = vfs_write(&table, handle, batch, n, &done);
        if (!e && done != n) e = -FS_EIO;
        if (!e) stats.size += (uint32_t)done;
    }
    draining = false;
    return failed(e);
}
int bootlog_shutdown(void)
{
    if (draining) return -FS_EBUSY;
    int result = stats.error;
    while (stats.active && used) { int e = bootlog_drain(); if (e) { result = e; break; } }
    disabled = true;
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
    uint32_t f = lock_ram(); *out = stats; out->queued = used; unlock_ram(f);
}
void bootlog_reset(void)
{
    if (attached || draining) return;
    memset(&stats, 0, sizeof(stats)); head = used = 0; handle = -1; disabled = false;
}
