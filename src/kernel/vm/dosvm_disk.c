/* File-backed bounded private disk overlay; the base is never opened writable.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/abi.h>
#include "dosvm_internal.h"

/* f3-06: base names a read-only image of the same private geometry (a
 * local workload disk built by scripts/test/dos_fixtures.py, or the bundled
 * shareware games disk); the canonical C: payload otherwise. Only the
 * overlay below is ever opened writable. */
#define DOSVM_DEFAULT_BASE "C:\\SYSTEM\\DOS\\DOSBOOT.IMG"
/* Owner decision 2026-10-12: Wolfenstein 3-D and DOOM shareware are
 * pre-installed. scripts/build_games_disk.py builds this image (the DOS
 * system plus \WOLF3D and \DOOM, trailing zero sectors cut) and
 * scripts/build_image.py installs it on the boot volume. */
#define DOSVM_BUNDLED_BASE "C:\\GAMES\\CIUKIGAM.IMG"
/* A full image, or one cut after its last used sector (zeros follow). */
static bool base_size_valid(uint32_t size)
{
    return !(size % 512u) && size >= DOSVM_BASE_MIN_SECTORS * 512u && size <= DOSVM_SECTORS * 512u;
}
/* The lookup table (20596 bytes on i686) is larger than the kernel heap's
 * biggest class (2040 bytes, core/kheap.c), so a kmalloc'd table failed with
 * ENOMEM on every call: /Programs/dosbox create was refused ("Not enough space",
 * vm-window QEMU 2026-10-12). One static table, never on a kernel stack,
 * behind a sleeping test-and-set gate (no mutex: the VFS calls below take
 * the storage lock). */
static struct vfs_table workload_table;
static bool workload_busy;
static int base_lookup(char path[24], bool bundled)
{
    struct storage *s = storage_get();
    if (!s || !s->ready || s->stopped) return -V86_EIO;
    for (;;) {
        uint32_t flags = irq_save();
        if (!workload_busy) { workload_busy = true; irq_restore(flags); break; }
        irq_restore(flags);
        task_sleep_ms(1);
    }
    struct vfs_table *t = &workload_table;
    int e = vfs_table_init(&s->vfs, t, 2);
    if (!e) {
        e = -ENOENT;
        struct fat_entry entry;
        /* The owner's local fixture disk first: exactly the full geometry
         * (dos_fixtures.py never cuts it) on the first drive after C:. */
        for (unsigned d = 3; d < 26 && e == -ENOENT; d++) {
            ksnprintf(path, 24, "%c:\\CIUKIGAM.IMG", 'A' + d);
            if (!vfs_stat(t, path, &entry) && entry.size == DOSVM_SECTORS * 512u && !(entry.attr & 0x10)) e = 0;
        }
        /* Then the bundled games disk; like a drive without the fixture, a
         * missing or unusable C:\GAMES\CIUKIGAM.IMG is simply absent. */
        if (e == -ENOENT && bundled) {
            ksnprintf(path, 24, "%s", DOSVM_BUNDLED_BASE);
            if (!vfs_stat(t, path, &entry) && base_size_valid(entry.size) && !(entry.attr & 0x10)) e = 0;
        }
        vfs_table_destroy(t);
    }
    workload_busy = false;
    if (e) path[0] = 0;
    return e;
}
int dosvm_workload_base(char path[24]) { return base_lookup(path, false); }
int dosvm_window_base(char path[24]) { return base_lookup(path, true); }
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
    if (!base_size_valid(entry.size)) return -EINVAL;
    v->base_sectors = entry.size / 512u;
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
    /* Past the end of a cut base image: the free FAT16 area, all zeros. */
    if (lba >= v->base_sectors) { memset(sector, 0, 512); return 0; }
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
