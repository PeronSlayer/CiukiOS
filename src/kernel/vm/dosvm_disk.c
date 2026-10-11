/* File-backed bounded private disk overlay; the base is never opened writable.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include "dosvm_internal.h"

int dosvm_disk_open(struct dosvm *v)
{
    struct storage *s = storage_get();
    if (!s || !s->ready || s->stopped) return -V86_EIO;
    int e = vfs_table_init(&s->vfs, &v->disk_table, 2);
    if (e) return e;
    v->table_ready = true;
    struct fat_entry entry;
    e = vfs_stat(&v->disk_table, "C:\\SYSTEM\\DOS\\DOSBOOT.IMG", &entry);
    if (e) return e;
    if (entry.size != DOSVM_SECTORS * 512u) return -EINVAL;
    v->base = vfs_open(&v->disk_table, "C:\\SYSTEM\\DOS\\DOSBOOT.IMG",
                       VFS_READ, VFS_DENY_WRITE, 0);
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
    kmutex_lock(&v->disk_lock);
    int e = disk_read_locked(v, lba, sector);
    kmutex_unlock(&v->disk_lock);
    return e;
}
int dosvm_disk_write(struct dosvm *v, uint32_t lba, const void *sector)
{
    kmutex_lock(&v->disk_lock);
    int e = disk_write_locked(v, lba, sector);
    kmutex_unlock(&v->disk_lock);
    return e;
}

int dosvm_disk_close(struct dosvm *v, bool forced)
{
    kmutex_lock(&v->disk_lock);
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
    kmutex_unlock(&v->disk_lock);
    return error;
}
