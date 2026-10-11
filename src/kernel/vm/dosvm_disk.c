/* File-backed bounded private disk overlay; the base is never opened writable.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include "dosvm_internal.h"

/* f3-06: base names a read-only image of the same private geometry (a
 * local workload disk built by scripts/test/dos_fixtures.py); the canonical
 * C: payload otherwise. Only the overlay below is ever opened writable. */
#define DOSVM_DEFAULT_BASE "C:\\SYSTEM\\DOS\\DOSBOOT.IMG"
int dosvm_workload_base(char path[24])
{
    struct storage *s = storage_get();
    if (!s || !s->ready || s->stopped) return -V86_EIO;
    struct vfs_table *t = kmalloc(sizeof(*t));      /* ~7 KiB: not on the stack */
    if (!t) return -ENOMEM;
    int e = vfs_table_init(&s->vfs, t, 2);
    if (!e) {
        e = -ENOENT;
        for (unsigned d = 3; d < 26 && e == -ENOENT; d++) {
            struct fat_entry entry;
            ksnprintf(path, 24, "%c:\\CIUKIGAM.IMG", 'A' + d);
            if (!vfs_stat(t, path, &entry) && entry.size == DOSVM_SECTORS * 512u && !(entry.attr & 0x10)) e = 0;
        }
        vfs_table_destroy(t);
    }
    kfree(t);
    if (e) path[0] = 0;
    return e;
}
int dosvm_disk_open(struct dosvm *v, const char *base)
{
    struct storage *s = storage_get();
    if (!s || !s->ready || s->stopped) return -V86_EIO;
    if (!base) base = DOSVM_DEFAULT_BASE;
    unsigned length = 0;
    while (length < 64 && base[length]) length++;
    if (!length || length == 64) return -EINVAL;
    int e = vfs_table_init(&s->vfs, &v->disk_table, 2);
    if (e) return e;
    v->table_ready = true;
    struct fat_entry entry;
    e = vfs_stat(&v->disk_table, base, &entry);
    if (e) return e;
    if (entry.size != DOSVM_SECTORS * 512u) return -EINVAL;
    v->base = vfs_open(&v->disk_table, base, VFS_READ, VFS_DENY_WRITE, 0);
    if (v->base < 0) return v->base;
    v->base_open = true;
    ksnprintf(v->overlay_path, sizeof(v->overlay_path), "C:\\SYSTEM\\DOS\\VM%08x.COW",
              v->info.key.generation);
    v->overlay = vfs_open(&v->disk_table, v->overlay_path,
        VFS_READ | VFS_WRITE | VFS_CREATE | VFS_EXCLUSIVE, VFS_DENY_ALL, 0);
    if (v->overlay < 0) return v->overlay;
    v->overlay_open = v->overlay_created = true;
    return 0;
}


static bool disk_gate_free(void *arg) { return !*(bool *)arg; }
static void disk_gate_take(struct dosvm *v)
{
    /* Test-and-set under a critical section: the worker and the controller
     * task may both reach the gate, and the kernel preempts at trap exits. */
    for (;;) {
        uint32_t flags = irq_save();
        if (!v->disk_busy) { v->disk_busy = true; irq_restore(flags); return; }
        irq_restore(flags);
        kwait_wait_until(&v->disk_wait, disk_gate_free, &v->disk_busy, deadline_after_ms(1000));
    }
}
static void disk_gate_drop(struct dosvm *v) { v->disk_busy = false; kwait_wake_all(&v->disk_wait); }
static int sector_io(struct dosvm *v, int handle, uint32_t offset, void *data, bool write)
{
    uint64_t position;
    size_t done = 0;
    int e = vfs_seek(&v->disk_table, handle, offset, VFS_SEEK_SET, &position);
    if (e) return e;
    if (position != offset) return -V86_EIO;
    e = write ? vfs_write(&v->disk_table, handle, data, 512, &done) :
                vfs_read(&v->disk_table, handle, data, 512, &done);
    return e ? e : done == 512 ? 0 : -V86_EIO;
}

static int disk_read_locked(struct dosvm *v, uint32_t lba, void *sector)
{
    if (!v->base_open || !v->overlay_open || lba >= DOSVM_SECTORS) return -EINVAL;
    for (unsigned i = 0; i < v->sector_count; i++)
        if (v->sectors[i].lba == lba)
            return sector_io(v, v->overlay, v->sectors[i].offset, sector, false);
    return sector_io(v, v->base, lba * 512u, sector, false);
}

static int disk_write_locked(struct dosvm *v, uint32_t lba, const void *sector)
{
    if (!v->base_open || !v->overlay_open || lba >= DOSVM_SECTORS) return -EINVAL;
    unsigned i;
    for (i = 0; i < v->sector_count; i++) if (v->sectors[i].lba == lba) break;
    if (i == DOSVM_OVERLAY_SECTORS) return -ENOSPC;
    uint32_t offset = i == v->sector_count ? i * 512u : v->sectors[i].offset;
    /* A failed issued write may have changed private data; report its loss
     * even when no new sector can be published into the index. */
    v->info.dirty = true;
    int e = sector_io(v, v->overlay, offset, (void *)sector, true);
    if (e) return e;
    if (i == v->sector_count) {
        v->sectors[i].lba = lba;
        v->sectors[i].offset = offset;
        v->sector_count++;
    }
    return 0;
}

int dosvm_disk_read(struct dosvm *v, uint32_t lba, void *sector)
{
    disk_gate_take(v);
    int e = disk_read_locked(v, lba, sector);
    disk_gate_drop(v);
    return e;
}
int dosvm_disk_write(struct dosvm *v, uint32_t lba, const void *sector)
{
    disk_gate_take(v);
    int e = disk_write_locked(v, lba, sector);
    disk_gate_drop(v);
    return e;
}

int dosvm_disk_close(struct dosvm *v, bool forced)
{
    disk_gate_take(v);
    int error = 0, e;
    if (v->overlay_open) {
        if (!forced && v->info.dirty) error = vfs_commit(&v->disk_table, v->overlay);
        e = vfs_close(&v->disk_table, v->overlay);
        if (!error) error = e;
        v->overlay_open = false;
    }
    if (v->base_open) {
        e = vfs_close(&v->disk_table, v->base);
        if (!error) error = e;
        v->base_open = false;
    }
    if (v->overlay_created) {
        e = vfs_delete(&v->disk_table, v->overlay_path);
        if (e == -FS_ENOENT) e = 0;
        if (!error) error = e;
        if (!e) v->overlay_created = false;
    }
    if (v->table_ready && !v->overlay_created) {
        vfs_table_destroy(&v->disk_table);
        v->table_ready = false;
    }
    v->sector_count = 0;
    disk_gate_drop(v);
    return error;
}
