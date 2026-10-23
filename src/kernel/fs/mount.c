/* Read first, then enable writing explicitly. FAT mount/ownership rules:
 * https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf
 * See tests/host/fs/STORAGE-VALIDATION.md for integration decisions.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/storage.h>
#include <ciuki/kernel.h>
#include <ciuki/bootlog.h>
#include <ciuki/blackbox.h>
#pragma weak blackbox_bind
#pragma weak blackbox_flush
#pragma weak blackbox_checkpoint
#pragma weak blackbox_reset
#pragma weak blackbox_capture
#pragma weak blackbox_serial_replay
#include <ciuki/init.h>
#include <ciuki/vfs_hooks.h>
#include <ciuki/reboot.h>
#include <ciuki/pager.h>
#include "pagefile.h"
/* Fixtures that link mount.c without the adapter keep RAM-only commit. */
#pragma weak pagefile_init
#pragma weak pagefile_backend
#pragma weak pagefile_detach
#pragma weak pagefile_reattach
#pragma weak pagefile_close
/* f9-01: fs/cdview.c; harnesses without it never enter Setup mode. */
#pragma weak cdview_probe
#pragma weak cdview_bind
#pragma weak cdview_release
#pragma weak cdview_verify_volume
#ifndef FS_HOST
#include <ciuki/ata.h>
#include <ciuki/sync.h>
#endif

static struct storage system_storage;
/* The canonical storage instance owns the hardware sweep. Keep its cursor
 * ledger private here, without changing the frozen storage/VFS interfaces. */
static uint64_t system_sweep_writes;
static struct storage_volume *cursor_volume;
static int volume_flush(struct blkdev *);
static int volume_write(struct blkdev *, uint64_t, uint32_t, const void *);
/* f0-49: one pagefile per boot, on the system volume (host fixtures may
 * bind another storage instance). It is an active volume dependency. */
static struct pagefile system_pagefile;
struct pagefile *storage_pagefile(void) { return system_pagefile.vfs ? &system_pagefile : 0; }
static bool pagefile_on(const struct storage *s, const struct fat_volume *fat)
{ return system_pagefile.vfs==&s->vfs && (!fat || system_pagefile.volume==fat); }
int storage_pagefile_bind(struct storage *s, unsigned d, const char *path)
{
    struct storage_volume *v=storage_volume(s,d);
    /* Writable fixed FAT only: never removable, optical or NTFS media. */
    const char *reason=!v ? "absent" : v->error ? "error" : v->removable ? "removable" :
        !v->fat.mounted ? "unmounted" : v->fat.readonly ? "readonly" : !v->part.parent ? "unbound" : 0;
    if (reason) {
        klog("[pager] volume drive=%c usable=0 reason=%s commit=ram",'A'+(d<26?d:0),reason);
        return -FS_EROFS;
    }
    if (system_pagefile.vfs) return -FS_EBUSY;
    if (!pagefile_init || !&pagefile_backend) return -FS_EOPNOTSUPP;
    int e=pagefile_init(&system_pagefile,&s->vfs,d,path,blkpart_device(&v->part));
    if (!e && pager_attach) e=pager_attach(&pagefile_backend,&system_pagefile);
    if (e) memset(&system_pagefile,0,sizeof(system_pagefile));
    return e;
}

bool storage_volume_pagefile(const struct storage *s, const struct fat_volume *fat)
{ return pagefile_on(s,fat); }

uint64_t storage_sweep_writes(const struct storage_volume *v)
{
    return v == &system_storage.volumes[2] ? system_sweep_writes : 0;
}

static struct blkdev *refresh_volume(struct storage_volume *v)
{
    /* Setup C: may have cached FAT/data blocks when the optical service
     * observes a replacement. Check its view before any cache hit. This
     * callback is generation-only and never performs device I/O. */
    if (v->optical && v->part.parent && v->part.parent->media_check)
        (void)v->part.parent->media_check(v->part.parent);
    struct blkdev *p = blkpart_device(&v->part);
    if (!p) return 0;
    v->io.quarantined = p->quarantined;
    v->io.media_generation = p->media_generation;
    v->io.write_cache_state = p->write_cache_state;
    v->io.write = p->write ? volume_write : 0;
    v->io.flush = p->flush ? volume_flush : 0;
    return p;
}

static struct blkdev *refresh_ntfs(struct storage_ntfs *n)
{
    struct blkdev *p = blkpart_device(&n->part);
    if (p) n->io.quarantined = p->quarantined;
    return p;
}

static void refresh_cache(void *ctx, struct blkdev *dev)
{
    struct storage *s = ctx;
    for (unsigned d = 0; d < 26; d++)
        if (dev == &s->volumes[d].io) { refresh_volume(&s->volumes[d]); return; }
    for (unsigned i = 0; i < STORAGE_NTFS; i++)
        if (dev == &s->ntfs[i].io) { refresh_ntfs(&s->ntfs[i]); return; }
}

static int volume_read(struct blkdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct storage_volume *v = dev->ctx;
    struct blkdev *p = refresh_volume(v);
    int e = blkdev_range(p, lba, count);
    if (!e) { v->reads += count; e = p->read(p, lba, count, buf); }
    refresh_volume(v);
    return e;
}
static int volume_write(struct blkdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct storage_volume *v = dev->ctx;
    struct blkdev *p = refresh_volume(v);
    int e = blkdev_range(p, lba, count);
    if (!e && (!v->read_gate || !blkdev_durable(p))) e = -FS_EROFS;
    if (!e) {
        v->writes += count;
        if (v == cursor_volume && v == &system_storage.volumes[2])
            system_sweep_writes += count;
        e = p->write(p, lba, count, buf);
    } else v->refused++;
    refresh_volume(v);
    if (v->trace) v->trace(v, 'W', lba, e);
    return e;
}
static int volume_flush(struct blkdev *dev)
{
    struct storage_volume *v = dev->ctx;
    struct blkdev *p = refresh_volume(v);
    if (p->quarantined) return -FS_EQUARANTINED;
    if (!p->flush) return -FS_EOPNOTSUPP;
    v->flushes++;
    int e = p->flush(p);
    refresh_volume(v);
    if (v->trace) v->trace(v, 'B', 0, e);
    return e;
}

static int ntfs_read(struct blkdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct storage_ntfs *n = dev->ctx;
    struct blkdev *p = refresh_ntfs(n);
    int e = blkdev_range(p, lba, count);
    if (!e) { n->reads += count; e = p->read(p, lba, count, buf); }
    refresh_ntfs(n);
    return e;
}
/* Read-only instrumentation: nothing is ever forwarded to the disk. */
static int ntfs_write(struct blkdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct storage_ntfs *n = dev->ctx; (void)lba; (void)buf; n->writes += count; return -FS_EROFS;
}
static int ntfs_flush(struct blkdev *dev)
{
    struct storage_ntfs *n = dev->ctx; n->flushes++; return -FS_EROFS;
}
static int ntfs_media(struct blkdev *dev)
{
    struct storage_ntfs *n = dev->ctx; refresh_ntfs(n);
    return n->io.quarantined ? -FS_EQUARANTINED : 0;
}

int storage_setup(struct storage *s, size_t bytes)
{
    if (!s) return -FS_EINVAL;
    if (s == &system_storage) system_sweep_writes = 0;
    memset(s, 0, sizeof(*s));
    int e = cache_init(&s->cache, bytes);
    if (e) return e;
    s->cache.refresh = refresh_cache; s->cache.refresh_ctx = s;
    vfs_init(&s->vfs);
    s->next_drive = 3; s->ready = true;
    return 0;
}
static bool fat_partition(uint8_t type)
{
    return type == 1 || type == 4 || type == 6 || type == 0x0b || type == 0x0c || type == 0x0e;
}
/* An MBR type 07h partition that carries an NTFS boot sector gets a drive
 * letter and a read-only mount. Other type 07h contents (exFAT, HPFS) were
 * never mounted and still consume no letter. */
static int ntfs_add(struct storage *s, unsigned disk, unsigned partition, struct blkdev *dev, const struct partition *part)
{
    unsigned slot = 0;
    while (slot < STORAGE_NTFS && s->ntfs[slot].present) slot++;
    if (slot == STORAGE_NTFS) return -FS_ENOSPC;
    struct storage_ntfs *n = &s->ntfs[slot];
    memset(n, 0, sizeof(*n));
    int e = blkpart_init(&n->part, dev, part);
    if (e) { memset(n, 0, sizeof(*n)); return e; }
    n->io = (struct blkdev){ .read = ntfs_read, .write = ntfs_write, .flush = ntfs_flush, .media_check = ntfs_media,
        .capacity = part->count, .sector_size = 512, .write_cache_state = BLKDEV_CACHE_UNKNOWN, .ctx = n };
    refresh_ntfs(n);
    e = ntfs_ops.probe(&s->cache, &n->io, 0);
    if (e == -FS_EINVAL) { cache_invalidate(&s->cache, &n->io, true); memset(n, 0, sizeof(*n)); return 0; }
    if (s->next_drive >= 26) { cache_invalidate(&s->cache, &n->io, true); memset(n, 0, sizeof(*n)); return -FS_ENOSPC; }
    n->present = true; n->disk = disk; n->partition = partition; n->drive = s->next_drive++;
    n->error = e;
    if (!e) storage_ntfs_mount(s, slot);
    return 0;
}
int storage_ntfs_mount(struct storage *s, unsigned slot)
{
    if (!s || !s->ready || s->stopped || slot >= STORAGE_NTFS || !s->ntfs[slot].present) return -FS_EINVAL;
    struct storage_ntfs *n = &s->ntfs[slot];
    vfs_lock(&s->vfs,0);
    for (unsigned d=3;d<26;d++) if (s->vfs.ro_volumes[d]==&n->ro) { vfs_unlock(&s->vfs); return -FS_EBUSY; }
    if (n->ro.mounted) { vfs_unlock(&s->vfs); return -FS_EBUSY; }
    unsigned d = n->drive;
    if (d < 3 || d >= 26 || vfs_drive_present(&s->vfs, d)) {
        d = 3; while (d < 26 && vfs_drive_present(&s->vfs, d)) d++;
        if (d >= 26) { n->error=-FS_ENOSPC; vfs_unlock(&s->vfs); return n->error; }
    }
    vfs_reserve_locked(&s->vfs,vfs_drive_mask(d)); vfs_tag_locked(&s->vfs,"mount"); vfs_site_locked(&s->vfs,"mount");
    if (s->stopped || n->ro.mounted || vfs_drive_present(&s->vfs,d)) { vfs_unlock(&s->vfs); return -FS_EBUSY; }
    n->drive=d; /* also serializes concurrent preparation of this slot */
    refresh_ntfs(n);
    int e = VFS_IO(&s->vfs,fs_ro_mount(&n->ro, &ntfs_ops, &s->cache, &n->io, 0, n->io.capacity));
    if (!e) e = vfs_attach_ro_locked(&s->vfs, d, &n->ro);
    if (e && n->ro.mounted) VFS_IO_VOID(&s->vfs,fs_ro_unmount(&n->ro));
    n->error = e;
    vfs_unlock(&s->vfs); return e ? e : (int)d;
}
int storage_ntfs_unmount(struct storage *s, unsigned slot)
{
    if (!s || !s->ready || slot >= STORAGE_NTFS || !s->ntfs[slot].present) return -FS_EINVAL;
    struct storage_ntfs *n = &s->ntfs[slot];
    for (;;) {
        vfs_lock(&s->vfs,0); unsigned d=n->drive;
        vfs_reserve_locked(&s->vfs,vfs_drive_mask(d)); vfs_tag_locked(&s->vfs,"mount"); vfs_site_locked(&s->vfs,"mount");
        if (n->drive==d) break;
        vfs_unlock(&s->vfs);
    }
    if (!n->ro.mounted) { vfs_unlock(&s->vfs); return 0; }
    for (unsigned d = 3; d < 26; d++) if (s->vfs.ro_volumes[d] == &n->ro) {
        int e=vfs_detach_locked(&s->vfs,d); vfs_unlock(&s->vfs); return e;
    }
    VFS_IO_VOID(&s->vfs,fs_ro_unmount(&n->ro)); vfs_unlock(&s->vfs);
    return 0;
}
int storage_add_disk(struct storage *s, unsigned disk, struct blkdev *dev)
{
    if (!s || !s->ready || s->stopped || disk >= STORAGE_DISKS || !dev || s->disks[disk]) return -FS_EINVAL;
    s->disks[disk] = true;
    s->raw[disk].dev = dev; /* f9-02: the raw node of this slot */
    struct partition_table table;
    int e = partition_scan(dev, &table);
    if (e && (!disk || e != -FS_EINVAL)) return s->disk_errors[disk] = e;
    uint8_t mbr[512];
    int read = dev->read(dev, 0, 1, mbr);
    if (read) return s->disk_errors[disk] = read;
    /* Nonboot whole-disk FAT volumes have no partition table. The header
     * only selects the candidate; fat_mount performs every BPB/scan check.
     * Never hide GPT, EBR limits or issued I/O errors behind this fallback.
     * Microsoft FAT spec: Boot Sector/BPB and FAT Type Determination. */
    bool superfloppy = disk && !table.count && !table.ebr_reads && fs_rd16(mbr + 510) == 0xaa55 &&
        fs_rd16(mbr + 11) == 512 && (mbr[0] == 0xe9 || (mbr[0] == 0xeb && mbr[2] == 0x90));
    if (superfloppy) {
        table.count = 1;
        table.entries[0] = (struct partition){ .start = 0, .count = dev->capacity };
    } else if (e) return s->disk_errors[disk] = e;
    uint32_t boot_start = fs_rd32(mbr + 454), boot_count = fs_rd32(mbr + 458);
    for (unsigned i = 0; i < table.count; i++) {
        struct partition *part = &table.entries[i];
        if (!superfloppy && part->type == 0x07) {
            e = ntfs_add(s, disk, i + 1, dev, part);
            if (e) return s->disk_errors[disk] = e;
            continue;
        }
        if (!superfloppy && !fat_partition(part->type)) continue;
        /* f9-01: in Setup mode C: is the boot CD's image; disk 0 is data. */
        bool boot = !s->setup_mode && !disk && !part->logical && part->start == boot_start && part->count == boot_count;
        /* Raw replay precedes FAT scanning and every writable activation. */
        if (boot && s==&system_storage && blackbox_bind)
            blackbox_bind(dev,part->start,part->count,blackbox_serial_replay);
        unsigned d = boot ? 2 : s->next_drive++;
        if (d >= 26) return s->disk_errors[disk] = -FS_ENOSPC;
        struct storage_volume *v = &s->volumes[d];
        v->present = true; v->drive = d; v->disk = disk; v->partition = superfloppy ? 0 : i + 1;
        e = blkpart_init(&v->part, dev, part);
        if (e) { v->error = e; continue; }
        v->io = (struct blkdev){ .read = volume_read, .write = volume_write,
            .capacity = part->count, .sector_size = 512, .ctx = v };
        refresh_volume(v);
        e = fat_mount(&v->fat, &s->cache, &v->io, 0, part->count, false);
        v->writes_before_gate = v->writes;
        if (!e) {
            v->read_gate = !(v->fat.ro_reasons & ~(FAT_RO_REQUEST | FAT_RO_DURABILITY | FAT_RO_DIRTY | FAT_RO_LOST | FAT_RO_IO_FLAG));
            if (v->read_gate) v->read_sequence = ++s->sequence;
            e = vfs_attach(&s->vfs, d, &v->fat);
        }
        v->error = e;
    }
    return 0;
}
/* f9-02 DISK_PUBLISH: mount primary partition 1 of a leased disk at the
 * reserved free letter d with the same read gate as storage_add_disk, then
 * open its write session (a dirty or lost-cluster volume is repaired by
 * fat_enable_write as at boot). Nothing is published on failure. Only the
 * lease path calls this; namespace lock held and d reserved by the caller. */
int storage_publish_locked(struct storage *s, unsigned disk, unsigned d,
                           const struct partition *part, uint32_t *reasons)
{
    if (!s || disk >= STORAGE_DISKS || !s->raw[disk].dev || d < 3 || d >= 26 || !part) return -FS_EINVAL;
    struct storage_volume *v = &s->volumes[d];
    if (s->stopped || v->present || vfs_drive_present(&s->vfs, d)) return -FS_EBUSY;
    memset(v, 0, sizeof(*v));
    v->drive = d; v->disk = disk; v->partition = 1;
    int e = blkpart_init(&v->part, s->raw[disk].dev, part);
    if (e) { memset(v, 0, sizeof(*v)); return e; }
    v->io = (struct blkdev){ .read = volume_read, .write = volume_write,
        .capacity = part->count, .sector_size = 512, .ctx = v };
    refresh_volume(v);
    cache_invalidate(&s->cache, &v->io, true);
    e = VFS_IO(&s->vfs, fat_mount(&v->fat, &s->cache, &v->io, 0, part->count, false));
    if (!e) {
        if (reasons) *reasons = v->fat.ro_reasons;
        v->writes_before_gate = v->writes;
        v->read_gate = !(v->fat.ro_reasons & ~(FAT_RO_REQUEST | FAT_RO_DURABILITY | FAT_RO_DIRTY | FAT_RO_LOST | FAT_RO_IO_FLAG));
        if (!v->read_gate || v->writes_before_gate) e = -FS_EROFS;
    }
    if (!e) {
        v->read_sequence = ++s->sequence; v->present = true;
        refresh_volume(v);
        e = VFS_IO(&s->vfs, fat_enable_write(&v->fat));
    }
    if (!e) { v->write_sequence = ++s->sequence; e = vfs_attach_locked(&s->vfs, d, &v->fat); }
    if (e) {
        v->fat.mounted = false;
        cache_invalidate(&s->cache, &v->io, true);
        memset(v, 0, sizeof(*v));
    }
    return e;
}
/* Preparation does not expose a FAT volume or hold shared storage locks
 * across device I/O. Only reservation/publication take the namespace lock. */
static int removable_lock(struct storage *s, bool (*cancel)(void *), void (*pause)(void *), void *ctx)
{
    if (!cancel) { vfs_lock(&s->vfs,0); return 0; }
    while (!cancel(ctx)) {
        if (fs_lock_try_take(&s->vfs.lock)) return 0;
        pause(ctx);
    }
    return -FS_EAGAIN;
}
int storage_add_removable(struct storage *s, struct blkdev *dev)
{ return storage_add_removable_bounded(s,dev,0,0,0); }
static int add_removable(struct storage *,struct blkdev *,bool (*)(void *),void (*)(void *),void *,unsigned);
int storage_add_removable_bounded(struct storage *s, struct blkdev *dev,
                                  bool (*cancel)(void *), void (*pause)(void *), void *ctx)
{ return add_removable(s,dev,cancel,pause,ctx,UINT32_MAX); }
int storage_add_floppy(struct storage *s, struct blkdev *dev, unsigned drive)
{ return drive<2 ? add_removable(s,dev,0,0,0,drive) : -FS_EINVAL; }
static int add_removable(struct storage *s, struct blkdev *dev,
                         bool (*cancel)(void *), void (*pause)(void *), void *ctx, unsigned fixed)
{
    if (!s || !s->ready || s->stopped || !dev || dev->sector_size!=512 || (cancel && !pause)) return -FS_EINVAL;
    int e=removable_lock(s,cancel,pause,ctx); if (e) return e;
    if (fixed<2 && (vfs_drive_present(&s->vfs,fixed) || s->volumes[fixed].fat.preparing)) {
        vfs_unlock(&s->vfs); return -FS_EBUSY;
    }
    for (unsigned d=0;d<26;d++) if ((s->volumes[d].present || s->volumes[d].fat.preparing) &&
        s->volumes[d].removable && s->volumes[d].part.parent==dev) {
        vfs_unlock(&s->vfs); return -FS_EBUSY;
    }
    vfs_unlock(&s->vfs);
    struct partition_table table;
    e=partition_scan(dev,&table);
    uint8_t b[512];
    if (e && e!=-FS_EINVAL) return e;
    int r=dev->read(dev,0,1,b); if (r) return r;
    bool whole=!table.count && !table.ebr_reads && fs_rd16(b+510)==0xaa55 &&
        fs_rd16(b+11)==512 && (b[0]==0xe9 || (b[0]==0xeb && b[2]==0x90));
    if (whole) { table.count=1; table.entries[0]=(struct partition){.count=dev->capacity}; e=0; }
    if (fixed<2 && !whole) return -FS_EINVAL;
    if (e) return e;
    unsigned prepared[26], count=0, attempted[26], attempts=0;
    for (unsigned i=0;i<table.count;i++) {
        struct partition *p=&table.entries[i];
        if (!whole && !fat_partition(p->type)) continue;
        r=removable_lock(s,cancel,pause,ctx); if (r) return r;
        if (s->stopped || dev->quarantined) { vfs_unlock(&s->vfs); return -FS_EIO; }
        unsigned d=fixed;
        if (fixed==UINT32_MAX) {
            if (s->next_drive<4) s->next_drive=4;
            while (s->next_drive<26 && vfs_drive_present(&s->vfs,s->next_drive)) s->next_drive++;
            d=s->next_drive++;
        } else if (vfs_drive_present(&s->vfs,d) || s->volumes[d].fat.preparing) {
            vfs_unlock(&s->vfs); return -FS_EBUSY;
        }
        if (d>=26) { vfs_unlock(&s->vfs); return -FS_ENOSPC; }
        struct storage_volume *v=&s->volumes[d];
        attempted[attempts++]=d;
        v->safely_removed=false;
        v->removable=true; v->drive=d; v->disk=UINT32_MAX; v->partition=whole ? 0 : i+1;
        r=blkpart_init(&v->part,dev,p);
        if (!r && fixed<2) v->fat.preparing=true; /* reserve A:/B: before the first unlocked wait */
        vfs_unlock(&s->vfs);
        if (!r && fixed<2 && v->io.ctx) {
            /* A:/B: reuse their retained view, unlike USB's never-reused
             * letters. Forget a lost cache epoch only for a proved new
             * media generation; same-medium errors remain sticky. Old
             * handles retain their expired VFS/identity generation. */
            struct cache_completion previous={0};
            int status=cache_completion(&s->cache,&v->io,&previous);
            if (previous.generation && previous.generation!=dev->media_generation)
                cache_invalidate(&s->cache,&v->io,true);
            else if (status || previous.error) r=status ? status : previous.error;
        }
        if (!r) {
            v->io=(struct blkdev){.read=volume_read,.write=volume_write,.capacity=p->count,.sector_size=512,.ctx=v};
            refresh_volume(v);
            r=fat_prepare_removable(&v->fat,&s->cache,&v->io,0,p->count);
            v->fat.write_through=true;
        }
        v->writes_before_gate=v->writes;
        if (!r && (s->stopped || dev->quarantined || (cancel && cancel(ctx)))) r=-FS_EIO;
        if (!r) {
            v->read_gate=!(v->fat.ro_reasons & ~(FAT_RO_REQUEST|FAT_RO_DURABILITY|FAT_RO_DIRTY|FAT_RO_LOST|FAT_RO_IO_FLAG));
            if (v->read_gate) v->read_sequence=++s->sequence;
        }
        /* No parser I/O remains: publish using the one shared cache. Failure
         * leaves an unpublished retained record, never an uncertain mount. */
        v->fat.preparing=false;
        v->error=r;
        if (!r) prepared[count++]=d;
        if (r) v->fat.mounted=false;
        if (r && !e) e=r;
        if (cancel && cancel(ctx)) { e=-FS_EAGAIN; break; }
        if (dev->quarantined) { e=-FS_EIO; break; }
    }
    if (!count && !e) e=-FS_ENOENT;
    if (!e) e=removable_lock(s,cancel,pause,ctx);
    if (!e) {
        if (s->stopped || dev->quarantined) e=-FS_EIO;
        for (unsigned i=0;!e && i<count;i++) {
            unsigned d=prepared[i];struct storage_volume *v=&s->volumes[d];
            e=vfs_attach_locked(&s->vfs,d,&v->fat);
            if (!e) { v->present=true;s->vfs.drive_flags[d]|=VFS_DRIVE_REMOVABLE; }
        }
        /* All letters appear together. An attach refusal rolls back namespace
         * pointers without issuing a flush or presenting an uncertain medium. */
        if (e) for (unsigned i=0;i<count;i++) {
            unsigned d=prepared[i];struct storage_volume *v=&s->volumes[d];
            if (s->vfs.volumes[d]==&v->fat) {s->vfs.volumes[d]=0;s->vfs.generation[d]++;s->vfs.drive_flags[d]=0;}
            v->present=false;
        }
        vfs_unlock(&s->vfs);
    }
    if (e) for (unsigned i=0;i<count;i++) {
        struct storage_volume *v=&s->volumes[prepared[i]];v->error=e;v->fat.mounted=false;
    }
    /* Successful volume records belong to the caller, after it ends binding.
     * Failed candidates stay visible as diagnostics without a namespace. */
    for (unsigned i=0;e && i<attempts;i++) {
        struct storage_volume *v=&s->volumes[attempted[i]];
        klog("[usb-storage] volume drive=%c part=%u fat=%u mode=%s dirty=%u error=%d mount_ms=%u repair_ms=%u clusters=%u spc=%u",
             'A'+v->drive,v->partition,v->fat.type,v->fat.readonly ? "ro" : "rw",
             !!(v->fat.ro_reasons & FAT_RO_DIRTY),v->error,v->fat.mount_ms,v->fat.repair_ms,v->fat.clusters,v->fat.spc);
    }
    return e;
}
int storage_remove_removable_locked(struct storage *s, struct blkdev *dev, bool surprise)
{
    if (!s || !s->ready || !dev) return -FS_EINVAL;
    if (surprise) {
        dev->quarantined=true;
        blk_queue_cancel_device(dev->queue,dev); /* leaves issued pins with the controller */
        /* Publish loss before waiting for active volume operations. Their
         * barriers must wake with EIO, never flush a vanished medium. */
        for (unsigned d=0;d<26;d++) {
            struct storage_volume *v=&s->volumes[d];
            if (!v->present || !v->removable || v->part.parent!=dev) continue;
            v->io.quarantined=true;
            if (pagefile_on(s,&v->fat)) { system_pagefile.lost=true; if (pager_media_lost) pager_media_lost(); }
            cache_completion(&s->cache,&v->io,&v->fat.completion);
            v->fat.completion.error=-FS_EIO; v->fat.completion.incomplete=true;
            cache_media_lost(&s->cache,&v->io);
            vfs_identity_expire(&s->vfs,&v->fat);
            klog("[storage] completion drive=%c kind=durable result=%d incomplete=1 generation=%u accepted=%llu durable=%llu",
                 'A'+d,-FS_EIO,v->fat.completion.generation,
                 (unsigned long long)v->fat.completion.accepted,(unsigned long long)v->fat.completion.durable);
        }
    }
    uint32_t mask=0;
    for (unsigned d=0;d<26;d++) if (s->volumes[d].removable && s->volumes[d].part.parent==dev) mask|=1u<<d;
    vfs_reserve_locked(&s->vfs,mask); vfs_tag_locked(&s->vfs,"mount"); vfs_site_locked(&s->vfs,"mount");
    /* Preflight the entire physical device before flushing/cleaning ANY FAT. */
    if (!surprise) for (unsigned d=0;d<26;d++) {
        struct storage_volume *v=&s->volumes[d];
        if (!v->present || !v->removable || v->part.parent!=dev) continue;
        if (pagefile_on(s,&v->fat)) return -FS_EBUSY; /* live slots or I/O depend on it */
        for (unsigned i=0;i<VFS_NODES;i++) if (s->vfs.nodes[i].refs && s->vfs.nodes[i].volume==&v->fat) return -FS_EBUSY;
        if (px_volume_busy && px_volume_busy(&s->vfs,&v->fat)) return -FS_EBUSY;
        if (vfs_identity_busy(&s->vfs,&v->fat)) return -FS_EBUSY;
        for (unsigned i=0;i<VFS_TABLES;i++) if (s->vfs.tables[i] && s->vfs.tables[i]->cwd_cluster[d]) return -FS_EBUSY;
        refresh_volume(v);
        int e=cache_error(&s->cache,&v->io); if (e) return e;
    }
    if (!surprise) for (unsigned d=0;d<26;d++) {
        struct storage_volume *v=&s->volumes[d];
        if (v->present && v->removable && v->part.parent==dev && v->fat.mounted) {
            int e=VFS_IO(&s->vfs,fat_commit(&v->fat)); if (e) return e;
        }
    }
    if (!surprise && dev->flush) {
        struct vfs_io_state io=vfs_io_begin_locked(&s->vfs);
        int e=dev->flush(dev); vfs_io_end_locked(&s->vfs,io); if (e) return e;
    }
    for (unsigned d=0;d<26;d++) {
        struct storage_volume *v=&s->volumes[d];
        if (!v->present || !v->removable || v->part.parent!=dev) continue;
        if (s->vfs.volumes[d]) {
            if (surprise) {
                if (px_volume_detached) px_volume_detached(&s->vfs,d);
                s->vfs.volumes[d]=0; s->vfs.generation[d]++;
                v->fat.mounted=false; v->fat.readonly=true; v->io.quarantined=true;
                for (unsigned i=0;i<VFS_TABLES;i++) if (s->vfs.tables[i]) {
                    memcpy(s->vfs.tables[i]->cwd[d],"/",2); s->vfs.tables[i]->cwd_cluster[d]=0;
                }
            } else { int e=vfs_detach_locked(&s->vfs,d); if (e) return e; }
        }
        if (!surprise) cache_invalidate(&s->cache,&v->io,true);
        s->vfs.drive_flags[d]=0;
        v->fat.mounted=false; v->present=false;
    }
    if (!surprise) for (unsigned d=0;d<26;d++) {
        struct storage_volume *v=&s->volumes[d];
        if (v->removable && v->part.parent==dev) v->safely_removed=true;
    }
    return 0;
}
int storage_remove_removable(struct storage *s, struct blkdev *dev, bool surprise)
{
    if (!s) return -FS_EINVAL;
    vfs_lock(&s->vfs,0);
    int e=storage_remove_removable_locked(s,dev,surprise);
    vfs_unlock(&s->vfs); return e;
}
int storage_add_ro_locked(struct storage *s, unsigned slot, struct blkdev *dev, uint64_t session)
{
    if (!s || !s->ready || s->stopped || slot>=STORAGE_DISKS || !dev) return -FS_EINVAL;
    if (session >= dev->capacity) return -FS_EINVAL;
    struct fs_ro_volume *ro=&s->optical[slot];
    for (unsigned d=3;d<26;d++) if (s->vfs.ro_volumes[d]==ro) return -FS_EBUSY;
    if (ro->mounted) return -FS_EBUSY;
    unsigned d=s->next_drive;
    while (d<26 && vfs_drive_present(&s->vfs,d)) d++;
    if (d>=26) return -FS_ENOSPC;
    vfs_reserve_locked(&s->vfs,vfs_drive_mask(d)); vfs_tag_locked(&s->vfs,"mount"); vfs_site_locked(&s->vfs,"mount");
    if (s->stopped || vfs_drive_present(&s->vfs,d) || ro->mounted) return -FS_EBUSY;
    int e=VFS_IO(&s->vfs,fs_ro_mount(ro,&iso9660_ops,&s->cache,dev,session,dev->capacity-session));
    if (!e) e=vfs_attach_ro_locked(&s->vfs,d,ro);
    if (e && ro->mounted) VFS_IO_VOID(&s->vfs,fs_ro_unmount(ro));
    return e ? e : (int)d;
}
int storage_add_ro(struct storage *s, unsigned slot, struct blkdev *dev, uint64_t session)
{
    if (!s) return -FS_EINVAL;
    vfs_lock(&s->vfs,0);
    int e=storage_add_ro_locked(s,slot,dev,session);
    vfs_unlock(&s->vfs); return e;
}
void storage_refresh_ro(struct storage *s)
{
    if (!s || !s->ready || s->stopped) return;
    for (unsigned d=3; d<26; d++) {
        vfs_lock(&s->vfs,vfs_drive_mask(d));
        if (s->vfs.ro_volumes[d]) vfs_ro_check_locked(&s->vfs,d);
        vfs_unlock(&s->vfs);
    }
}
struct storage_volume *storage_volume(struct storage *s, unsigned d)
{
    if (!s || !s->ready || d >= 26 || !s->volumes[d].present) return 0;
    struct storage_volume *v = &s->volumes[d];
    refresh_volume(v);
    return v;
}
static int log_directory(struct fat_volume *fat)
{
    /* The image supplies SYSTEM. Create LOGS only after the write gate,
     * under the namespace lock; never put a full VFS table on the stack. */
    struct fat_entry directory;
    int e = fat_lookup(fat, fat->type == 32 ? fat->root : 0, "SYSTEM", &directory);
    if (e) return e;
    if (!(directory.attr & FAT_ATTR_DIR)) return -FS_ENOTDIR;
    uint32_t parent = directory.first;
    e = fat_lookup(fat, parent, "LOGS", &directory);
    if (e == -FS_ENOENT) return fat_create(fat, parent, "LOGS", FAT_ATTR_DIR, &directory);
    if (!e && !(directory.attr & FAT_ATTR_DIR)) e = -FS_ENOTDIR;
    return e;
}
int storage_enable_write(struct storage *s, unsigned d)
{
    struct storage_volume *v = storage_volume(s, d);
    if (!v || s->stopped) return -FS_ENOENT;
    /* f9-01 (D2): no fixed volume becomes writable in Setup mode; f9-02's
     * destination publication is the only later exception. */
    if (s->setup_mode && !v->removable) { v->refused++; return -FS_EROFS; }
    if (s->writer_error) return s->writer_error;
    if (v->error) return v->error;
    if (!v->read_gate || v->writes_before_gate) { v->refused++; return -FS_EROFS; }
    vfs_lock(&s->vfs,vfs_drive_mask(d)); vfs_tag_locked(&s->vfs,"mount"); vfs_site_locked(&s->vfs,"mount");
    refresh_volume(v);
    int e = VFS_IO(&s->vfs,fat_enable_write(&v->fat));
    if (!e && !v->write_sequence) v->write_sequence = ++s->sequence;
    if (!e && d == 2) e = VFS_IO(&s->vfs,log_directory(&v->fat));
    vfs_unlock(&s->vfs);
    /* FAT census/repair already services every 128 entries. This outer
     * checkpoint must follow release of namespace and volume locks. */
    fs_service();
    return e;
}
int storage_writeback(struct storage *s, uint32_t now)
{
    if (!s || !s->ready || s->stopped) return -FS_EINVAL;
    vfs_lock(&s->vfs,0);
    if (s->stopped || s->writer_running || s->writer_quiescing) {
        vfs_unlock(&s->vfs); return -FS_EBUSY;
    }
    s->writer_running=true;
    /* Cache writeback has its own block reservations and may span devices;
     * it never manipulates FAT metadata. Its refresh callback runs under cache. */
    struct vfs_io_state io_mask=vfs_io_begin_locked(&s->vfs);
#ifndef FS_HOST
    struct mm_ledger memory; ledger_snapshot(&memory);
    cache_adaptive_tick(&s->cache,now,memory.zone[ZONE_NORMAL].free,memory.high,ledger_pressure());
#endif
    int e = cache_writeback_tick(&s->cache, now);
    struct cache_stats stats;
    if (cache_writeback_report(&s->cache,now,&stats))
        klog("[storage] writeback dirty_bytes=%u budget_bytes=%u throttle_bytes=%u flushed_bytes=%u max_flush_ms=%u cache_sync_evictions=%llu writer_waits=%llu writer_wait_ms=%llu",
            stats.dirty_bytes,stats.dirty_budget,stats.throttle_bytes,stats.tick_flushed,stats.max_flush_ms,
            (unsigned long long)stats.cache_sync_evictions,(unsigned long long)stats.writer_waits,
            (unsigned long long)stats.writer_wait_ms);
#ifndef FS_HOST
    if (!ledger_pressure()) cache_prefetch_tick(&s->cache);
#endif
    vfs_io_end_locked(&s->vfs,io_mask);
    s->writer_ticks++;
    /* cache_error is sticky per device. Do not turn one removable-media
     * failure into a global writer-service failure on every volume. */
    vfs_unlock(&s->vfs);
    /* Delayed failures revoke the mount immediately, not just next commit. */
    for (unsigned d = 0; d < 26; d++) {
        vfs_lock(&s->vfs,0);
        struct storage_volume *v = &s->volumes[d];
        if (!v->present) { vfs_unlock(&s->vfs); continue; }
        int error = cache_error(&s->cache, &v->io);
        /* Healthy volume reservations may be held by a throttled writer
         * waiting for this service. Reserve only to publish an actual error. */
        if (!error) { vfs_unlock(&s->vfs); continue; }
        vfs_reserve_locked(&s->vfs,vfs_drive_mask(d));
        if (!v->present || !v->fat.mounted) { vfs_unlock(&s->vfs); continue; }
        error=cache_error(&s->cache,&v->io);
        if (error) {
            if (!(v->fat.ro_reasons & FAT_RO_WRITE_ERROR))
                klog("[storage] failure stage=writeback drive=%c disk=%u result=%d scope=volume fallback=readonly",
                     'A'+d,v->disk,error);
            v->fat.ro_reasons |= FAT_RO_WRITE_ERROR;
            v->fat.operation_file[0]=0; /* delayed cache error has no file owner */
            VFS_IO_VOID(&s->vfs,fat_revoke_write(&v->fat,"writeback_error",0));
        }
        vfs_unlock(&s->vfs);
    }
    vfs_lock(&s->vfs,0); s->writer_running=false; vfs_unlock(&s->vfs);
    fs_event_wake(&s->vfs.volume_changed);
    return e;
}
static int shutdown_volumes(struct storage *s);
int storage_shutdown(struct storage *s)
{
    if (!s || !s->ready) return -FS_EINVAL;
    /* The pagefile handle is the volume dependency: page-out and growth
     * stop and in-flight page I/O retires before detach may proceed. Live
     * slots stay readable through their runs; a refused shutdown resumes. */
    bool paged=pagefile_on(s,0) && !system_pagefile.detached;
    if (paged) {
        int e=pager_shutdown ? pager_shutdown() : pagefile_detach ? pagefile_detach(&system_pagefile) : 0;
        if (e) return e;
    }
    int e=shutdown_volumes(s);
    if (e && paged) { if (pager_resume) pager_resume(); else if (pagefile_reattach) pagefile_reattach(&system_pagefile); }
    return e;
}
static int shutdown_volumes(struct storage *s)
{
    /* Reserve all letters before preflight, excluding opens through durable
     * detach while allowing media snapshots during commit/device waits. */
    vfs_lock(&s->vfs,0);
    if (s->writer_quiescing) { vfs_unlock(&s->vfs); return -FS_EBUSY; }
    s->writer_quiescing=true;
    while (s->writer_running) fs_event_wait(&s->vfs.volume_changed,&s->vfs.lock);
    vfs_reserve_locked(&s->vfs,VFS_ALL_DRIVES); vfs_tag_locked(&s->vfs,"commit"); vfs_site_locked(&s->vfs,"mount");
    /* fx-5: the first holder is recorded; the refusal changes nothing. */
    const char *busy = 0; unsigned busy_drive = 0;
    for (unsigned i = 0; i < VFS_NODES && !busy; i++) if (s->vfs.nodes[i].refs && (!s->vfs.nodes[i].ro || s->vfs.nodes[i].generation==s->vfs.generation[s->vfs.nodes[i].drive]))
        { busy = "open"; busy_drive = s->vfs.nodes[i].drive; }
    for (unsigned d=0;d<26 && !busy;d++) if (s->vfs.volumes[d] && vfs_identity_busy(&s->vfs,s->vfs.volumes[d])) { busy="identity"; busy_drive=d; }
    for (unsigned d = 0; d < 26 && !busy; d++)
        if (s->vfs.volumes[d] && px_volume_busy && px_volume_busy(&s->vfs, s->vfs.volumes[d])) { busy = "posix"; busy_drive = d; }
    for (unsigned d=3; d<26 && !busy; d++)
        if (s->vfs.ro_volumes[d] && px_ro_busy && px_ro_busy(&s->vfs,s->vfs.ro_volumes[d])) { busy="posix_ro"; busy_drive=d; }
    if (busy) {
        klog("[storage] shutdown busy drive=%c holder=%s result=%d", 'A'+(busy_drive<26 ? busy_drive : 25), busy, -FS_EBUSY);
        s->writer_quiescing=false; vfs_unlock(&s->vfs); return -FS_EBUSY;
    }
    s->stopped = true;
    s->writer_quiescing=false;
    int result = s->writer_error;
    /* Preflight all volumes before setting ANY clean flag. */
    for (unsigned d = 0; d < 26; d++) {
        struct storage_volume *v = &s->volumes[d];
        if (!v->present || !v->fat.mounted) continue;
        refresh_volume(v);
        int e = VFS_IO(&s->vfs,fat_commit(&v->fat));
        klog("[storage] completion drive=%c kind=durable result=%d incomplete=%u generation=%u group=%llu accepted=%llu durable=%llu buffered_completions=%llu durable_completions=%llu",
             'A'+d,e,v->fat.completion.incomplete,v->fat.completion.generation,
             (unsigned long long)v->fat.completion.group,(unsigned long long)v->fat.completion.accepted,
             (unsigned long long)v->fat.completion.durable,(unsigned long long)v->fat.completion.buffered_completions,
             (unsigned long long)v->fat.completion.durable_completions);
        if (e && !result) result = e;
    }
    if (!result) for (unsigned d = 0; d < 26; d++) if (vfs_drive_present(&s->vfs,d)) {
        result = vfs_detach_locked(&s->vfs, d);
        if (result) break;
    }
    vfs_unlock(&s->vfs);
    return result;
}
void storage_destroy(struct storage *s)
{
    if (s==&system_storage && blackbox_reset) blackbox_reset();
    if (!s || !s->ready) return;
    if (pagefile_on(s,0) && !pager_attach && pagefile_close) { pagefile_close(&system_pagefile); memset(&system_pagefile,0,sizeof(system_pagefile)); }
    for (unsigned i=0; i<STORAGE_DISKS; i++) if (s->optical[i].mounted) fs_ro_unmount(&s->optical[i]);
    for (unsigned i=0; i<STORAGE_NTFS; i++) if (s->ntfs[i].ro.mounted) fs_ro_unmount(&s->ntfs[i].ro);
    vfs_destroy(&s->vfs); cache_destroy(&s->cache); s->ready = false;
    if (cdview_release) cdview_release(&s->boot_cd);
}
struct storage *storage_get(void) { return &system_storage; }
bool storage_readonly_info(unsigned drive, char *text, unsigned bytes)
{
    if (!text || !bytes || drive>=26 || !system_storage.ready) return false;
    const struct storage_volume *v=&system_storage.volumes[drive];
    if (!v->present || !v->fat.readonly) return false;
    /* An intentional read-only probe/reopen is not a failed write gate. */
    if (v->fat.ro_reasons==FAT_RO_REQUEST && !v->fat.readonly_reason) return false;
    ksnprintf(text,bytes,"%s; cluster=%u; file=%s",fat_readonly_reason(&v->fat),
        v->fat.readonly_cluster,v->fat.readonly_file[0] ? v->fat.readonly_file : "volume");
    return true;
}

int storage_boot_cfg(struct storage *s, const char *request)
{
    /* Quiescent sweep task only. No general write gate, log directory or sink.
     * Use the same volume/cache/barrier, including after a probe's shutdown. */
    unsigned request_len = 0;
    if (request) while (request_len <= 64 && request[request_len]) request_len++;
    if (request_len > 64) return -FS_EINVAL;
    for (unsigned i = 0; i < request_len; i++)
        if ((uint8_t)request[i] < 32 || (uint8_t)request[i] > 126) return -FS_EINVAL;
    struct storage_volume *v = storage_volume(s, 2);
    if (!v || v->error || !v->read_gate || s->writer_error ||
        !blkdev_durable(blkpart_device(&v->part))) return -FS_EROFS;
    vfs_lock(&s->vfs,vfs_drive_mask(2)); vfs_tag_locked(&s->vfs,"write"); vfs_site_locked(&s->vfs,"mount");
    bool mounted = v->fat.mounted, readonly = v->fat.readonly;
    int e = 0;
    if (!mounted) e = VFS_IO(&s->vfs,fat_mount(&v->fat, &s->cache, &v->io, 0, v->io.capacity, false));
    struct fat_entry directory, file;
    char original[128], output[128]; size_t done = 0;
    unsigned used = 0, lines = 0;
    if (!e) e = VFS_IO(&s->vfs,fat_lookup(&v->fat, v->fat.type == 32 ? v->fat.root : 0, "SYSTEM", &directory));
    if (!e && !(directory.attr & FAT_ATTR_DIR)) e = -FS_ENOTDIR;
    if (!e) e = VFS_IO(&s->vfs,fat_lookup(&v->fat, directory.first, "BOOT.CFG", &file));
    if (!e && (file.attr & (FAT_ATTR_RO | FAT_ATTR_DIR) || file.size > 127)) e = -FS_EROFS;
    if (!e && file.size) e = VFS_IO(&s->vfs,fat_read(&v->fat, &file, 0, original, file.size, &done));
    if (!e && done != file.size) e = -FS_EIO;
    if (!e) for (unsigned i = 0; i < file.size;) {
        unsigned begin = i;
        while (i < file.size && original[i] != '\n' && original[i] != '\r') i++;
        unsigned end = i;
        while (i < file.size && (original[i] == '\n' || original[i] == '\r')) i++;
        unsigned token = begin;
        while (token < end && original[token] == ' ') token++;
        if (end - token >= 6 && !memcmp(original + token, "probe=", 6)) { lines++; continue; }
        if (used + i - begin > 127) { e = -FS_EFBIG; break; }
        memcpy(output + used, original + begin, i - begin); used += i - begin;
    }
    if (!e && lines > 1) e = -FS_EINVAL;
    if (!e && request) {
        if (used && output[used - 1] != '\n' && output[used - 1] != '\r') output[used++] = '\n';
        if (used + request_len + 7 > 127) e = -FS_EFBIG;
        else {
            memcpy(output + used, "probe=", 6); used += 6;
            memcpy(output + used, request, request_len); used += request_len;
            output[used++] = '\n';
        }
    }
    /* Drain a prior writable workload before attributing any writes to the
     * cursor. This barrier is outside the cursor ledger, even on failure. */
    if (!e && mounted && !readonly) e = VFS_IO(&s->vfs,fat_commit(&v->fat));
    if (!e) cursor_volume = v;
    if (!e) e = VFS_IO(&s->vfs,fat_enable_write(&v->fat));
    bool recovered = v->fat.dirty_recovered;
    if (!e && used) {
        e = VFS_IO(&s->vfs,fat_write(&v->fat, &file, 0, output, used, &done));
        if (!e && done != used) e = -FS_EIO;
    }
    if (!e) e = VFS_IO(&s->vfs,fat_truncate(&v->fat, &file, used));
    if (!e) e = VFS_IO(&s->vfs,fat_commit(&v->fat));
    /* Restore a closed gate. Never set a clean flag following an I/O error. */
    if (!e && (!mounted || readonly)) {
        e = VFS_IO(&s->vfs,fat_unmount(&v->fat));
        if (!e && mounted) {
            /* Closing the write gate reloads FAT geometry, not the VFS mount.
             * Keep its identity and group epochs across fat_mount's reset. */
            uint32_t identity_epoch=v->fat.identity_mount_generation, generation=v->io.media_generation;
            uint64_t groups=v->fat.publication_count, frontier=v->fat.publication_frontier;
            struct cache_completion completion=v->fat.completion;
            void (*changed)(void *,struct fat_volume *,const struct fat_entry *,const struct fat_entry *,enum fat_identity_change)=v->fat.identity_changed;
            void *identity_ctx=v->fat.identity_ctx;
            void (*observer)(void *,uint64_t,enum fat_publication_stage,char,int)=v->fat.publication_observer;
            void *observer_ctx=v->fat.publication_ctx;
            e=VFS_IO(&s->vfs,fat_mount(&v->fat,&s->cache,&v->io,0,v->io.capacity,false));
            if (!e && generation!=v->io.media_generation) e=-FS_EIO;
            if (!e) {
                v->fat.identity_mount_generation=identity_epoch;
                v->fat.identity_changed=changed; v->fat.identity_ctx=identity_ctx;
                v->fat.publication_count=v->fat.publication_group=v->fat.publication_durable=groups;
                v->fat.publication_frontier=frontier; v->fat.publication_generation=generation;
                v->fat.completion=completion;
                v->fat.publication_observer=observer; v->fat.publication_ctx=observer_ctx;
            } else vfs_identity_expire(&s->vfs,&v->fat);
        }
        if (!e && mounted) v->fat.dirty_recovered = recovered;
    } else if (e && readonly) {
        v->fat.readonly = true;
    }
    cursor_volume = 0;
    vfs_unlock(&s->vfs);
    return e;
}

bool storage_setup_media_ready(void) { return system_storage.boot_cd.bound; }
int storage_setup_identify(struct storage *s, unsigned slot, struct blkdev *optical, uint64_t session,
                           const char *options, size_t options_size, const struct cdview_identity *kid)
{
    if (!s || !s->ready || s->stopped || !s->setup_mode || slot >= STORAGE_DISKS || !optical) return -FS_EINVAL;
    if (!cdview_probe || !cdview_bind || !cdview_release || !cdview_verify_volume) return -FS_EOPNOTSUPP;
    if (s->boot_cd.bound || s->volumes[2].present || s->next_drive != 3 ||
        vfs_drive_present(&s->vfs, 3)) return -FS_EBUSY;
    const char *reason = "memory";
    struct cdview_boot_record record = { 0 };
    int iso_drive = -1;
    struct storage_volume *v = &s->volumes[2];
    void *block = fs_page_alloc();
    int e = block ? cdview_probe(optical, session, block, &record, &reason) : -FS_ENOMEM;
    if (block) fs_page_free(block);
    if (!e) { reason = "view"; e = cdview_bind(&s->boot_cd, optical, record.load_rba, record.image_sectors); }
    if (!e) {
        memset(v, 0, sizeof(*v));
        struct partition part = { .start = record.part_start, .count = record.part_count, .type = CDVIEW_PART_TYPE };
        reason = "partition";
        e = blkpart_init(&v->part, &s->boot_cd.dev, &part);
        if (!e) {
            v->io = (struct blkdev){ .read = volume_read, .write = volume_write,
                .capacity = part.count, .sector_size = 512, .ctx = v };
            refresh_volume(v);
            reason = "fat";
            e = fat_mount(&v->fat, &s->cache, &v->io, 0, part.count, false);
        }
        if (!e) e = cdview_verify_volume(&v->fat, options, options_size, kid, &reason);
        /* Qualify both namespaces before publishing C:. Without the ISO
         * tree's licences and sources the disc is not a usable Setup CD. */
        if (!e) {
            reason = "iso";
            iso_drive = storage_add_ro(s, slot, optical, session);
            if (iso_drive < 0) e = iso_drive;
        }
        if (!e) {
            v->present = true; v->drive = 2; v->disk = slot; v->partition = 1; v->optical = true;
            v->writes_before_gate = v->writes;
            v->read_gate = !(v->fat.ro_reasons & ~(FAT_RO_REQUEST | FAT_RO_DURABILITY | FAT_RO_DIRTY | FAT_RO_LOST | FAT_RO_IO_FLAG));
            if (v->read_gate) v->read_sequence = ++s->sequence;
            e = vfs_attach(&s->vfs, 2, &v->fat);
            reason = e ? "attach" : "ok";
        }
        if (e) {
            if (iso_drive >= 0) (void)vfs_detach(&s->vfs, (unsigned)iso_drive);
            if (v->fat.mounted) fat_unmount(&v->fat);
            cache_invalidate(&s->cache, &v->io, true);
            memset(v, 0, sizeof(*v));
            cdview_release(&s->boot_cd);
        }
    }
    klog("[setup] candidate slot=%u session=%llu catalog=%u image_block=%u image_sectors=%u result=%d reason=%s",
         slot, (unsigned long long)session, record.catalog_block, record.load_rba, record.image_sectors, e, reason);
    if (e) return e;
    s->boot_cd_slot = slot;
    klog("[setup] media bound slot=%u system=C iso=%c result=0", slot, 'A' + iso_drive);
    return 2;
}
void storage_setup_finish(struct storage *s)
{
    if (!s || !s->setup_mode) return;
    if (!s->boot_cd.bound) klog("[setup] media missing");
    /* Plan 3.3: C: image, D: ISO tree, E: onward the read-only ATA volumes. */
    if (s->next_drive < 4) s->next_drive = 4;
}

bool storage_probe_readonly(const char *selector, unsigned length)
{
    /* Every test starts without a disk sink or a writable mount. Destructive
     * probes explicitly open their gates, F0 panic never starts a writer. */
    return selector && length != 0;
}
int storage_sync_until(struct storage *s, const struct vfs_request *request)
{
    if (!s || !s->ready) return 0;
    if (!request || !request->now || !request->pause) return -FS_EINVAL;
    struct fs_request_scope scope;
    fs_request_enter(&scope, request);
    int result = fs_request_check();
    /* Do not close open files, detach a pagefile, mark FAT clean, or drain
     * raw logs through their unbounded shutdown paths on suspend. Every
     * mounted writable drive gets its normal durable publication barrier. */
    for (unsigned d = 0; !result && d < 26; d++) {
        result = fs_request_check();
        if (result) break;
        result = vfs_flush_volume_until(&s->vfs, d, request);
        if (result == -FS_ENOENT) result = 0;
    }
    if (!result) result = fs_request_check();
    fs_request_leave(&scope);
    return result;
}

int storage_sync(void)
{
    static const char marker[]="[storage] shutdown requested\n";
    if (blackbox_capture) blackbox_capture(marker,sizeof(marker)-1);
    int e=blackbox_checkpoint ? blackbox_checkpoint() : 0;
    if (e) { klog("[storage] shutdown refused stage=blackbox result=%d",e); return e; }
    e = bootlog_shutdown();
    /* fx-5: EBUSY is a refusal before any change (bootlog_shutdown waited
     * for the drain owner first): the writer, cache and mounts continue and
     * call 73 may retry. Latching `stopped` here ended the storage writer
     * after the first, refused, T23 power-off (image 9a60215f). A real log
     * failure still stops storage without a clean mark. */
    if (e) {
        klog("[storage] shutdown refused stage=bootlog result=%d",e);
        if (e != -FS_EBUSY) system_storage.stopped = true;
        return e;
    }
    e=storage_shutdown(&system_storage);
    if (e) klog("[storage] shutdown refused stage=volumes result=%d",e);
    if (!e) {
        static const char complete[]="[storage] shutdown complete\n";
        if (blackbox_capture) blackbox_capture(complete,sizeof(complete)-1);
        if (blackbox_checkpoint) e=blackbox_checkpoint();
    }
    return e;
}
#ifndef FS_HOST
static bool initialized;
static void storage_timer(void *arg)
{
    struct storage *s = arg;
    uint32_t last_raw=0;
    while (!s->stopped) {
        cache_writer_wait(&s->cache,10);
        uint64_t now=fs_now_ms();
        bool log_due=bootlog_due((uint32_t)now);
        bool due = cache_writeback_due(&s->cache, now) || log_due || (uint32_t)(now-last_raw)>=1000;
        if (due && !s->stopped) {
            /* Disk waits/writeback run on this P_DEVICE task, never on the
             * shared bounded work queue. fs_service/ATA yield within work. */
            if (blackbox_flush) blackbox_flush();
            last_raw=(uint32_t)now;
            if (log_due) bootlog_drain();
        }
        /* The writer_running reservation covers resize/prefetch too, so
         * shutdown quiesces every cache user before destroying its pages. */
        if (!s->stopped) storage_writeback(s,fs_now_ms());
    }
    task_exit(0);
}
/* f9-02: a published destination needs the writer even when the boot
 * started storage read-only (Setup mode). Idempotent; never in ordinary
 * boots, whose writer is created by storage_init. */
int storage_writer_start(struct storage *s)
{
    if (!s || s != &system_storage) return 0;
    vfs_lock(&s->vfs,0);
    bool start = s->ready && !s->stopped && !s->cache.writer_online;
    if (start) s->cache.writer_online = true;
    vfs_unlock(&s->vfs);
    if (!start) return 0;
    struct task *timer = task_create_kernel("storage-writer", storage_timer, s, P_DEVICE);
    if (!timer) {
        vfs_lock(&s->vfs,0); s->cache.writer_online = false; vfs_unlock(&s->vfs);
        return -FS_ENOMEM;
    }
    task_start(timer);
    return 0;
}
void storage_init(void)
{
    if (initialized) return;
    initialized = true;
    fs_calendar_init();
    struct storage *s = &system_storage;
    struct mm_ledger memory; ledger_snapshot(&memory);
    uint32_t cache_bytes=cache_initial_bytes(memory.zone[ZONE_NORMAL].usable,
        memory.zone[ZONE_NORMAL].free,memory.high);
    int e=cache_bytes ? storage_setup(s,cache_bytes) : -FS_ENOMEM;
    if (!e) e=cache_adaptive_enable(&s->cache,memory.zone[ZONE_NORMAL].usable);
    if (e) { klog("[storage] unavailable error=%d", e); return; }
    bool probe = (g_boot.flags & CBI_F_TEST_REQUEST) && storage_probe_readonly(g_boot.test_request, g_boot.test_request_len);
    s->setup_mode = storage_setup_requested();
    struct storage_start_policy policy = storage_start_policy(probe, s->setup_mode);
    struct task *timer = 0;
    if (policy.writer) {
        timer = task_create_kernel("storage-writer", storage_timer, s, P_DEVICE);
        if (!timer) s->writer_error = -FS_ENOMEM;
        else s->cache.writer_online=true;
    }
    struct ata_device *devices[STORAGE_DISKS];
    for (unsigned slot = 0; slot < STORAGE_DISKS; slot++)
        devices[slot] = ata_device_get(slot / 2, slot % 2);
    if (s->setup_mode) storage_setup_media(s, devices);
    for (unsigned slot = 0; slot < STORAGE_DISKS; slot++) {
        struct ata_device *dev = devices[slot];
        if (!dev || dev->atapi) continue;
        e = storage_add_disk(s, slot, &dev->block);
        /* f9-02 DISK_GET_INFO identity, copied once (data only). */
        memcpy(s->raw[slot].model, dev->model, sizeof(s->raw[slot].model));
        memcpy(s->raw[slot].serial, dev->serial, sizeof(s->raw[slot].serial));
        memcpy(s->raw[slot].firmware, dev->firmware, sizeof(s->raw[slot].firmware));
        s->raw[slot].lba48 = dev->lba48;
        drivers_storage_add((struct activation_entry){ .kind = ACTIVATION_STORAGE,
            .disk = slot, .error = e, .readonly = true, .reason = "disk_scan" });
    }
    for (unsigned d = 2; d < 26; d++) {
        struct storage_volume *v = storage_volume(s, d);
        if (!v) continue;
        if (policy.enable_write && !v->error) storage_enable_write(s, d);
        drivers_storage_add((struct activation_entry){ .kind = ACTIVATION_MOUNT,
            .drive = d, .disk = v->disk, .partition = v->partition, .type = v->fat.type,
            .readonly = v->fat.readonly, .reasons = v->fat.ro_reasons, .error = v->error,
            .read_gate = v->read_gate, .read_sequence = v->read_sequence,
            .writes = v->writes, .writes_before_gate = v->writes_before_gate,
            .reason = v->fat.dirty_recovered && !v->fat.readonly ? "dirty_recovered" :
                (v->fat.ro_reasons & FAT_RO_IO_FLAG) ? "error-flag" : "mount", .qualified = false });
        if (!v->partition)
            klog("[storage] kind=%u disk=%u part=0 layout=superfloppy drive=%c mode=%s error=%d",
                 ACTIVATION_MOUNT, v->disk, 'A' + d, v->fat.readonly ? "ro" : "rw", v->error);
    }
    for (unsigned i=0; i<STORAGE_NTFS; i++) {
        struct storage_ntfs *n=&s->ntfs[i];
        if (!n->present) continue;
        klog("[storage] ntfs slot=%u disk=%u part=%u drive=%c mode=ro result=%d reason=%s",
             i,n->disk,n->partition,n->error ? '-' : 'A'+n->drive,n->error,n->ro.diagnostic ? n->ro.diagnostic : "unavailable");
    }
    for (unsigned slot=0; slot<STORAGE_DISKS; slot++) {
        struct ata_device *dev=devices[slot];
        if (!dev || !dev->atapi || s->optical_bindings[slot].device) continue;
        optical_bind(s,slot,dev);
    }
    optical_service(s,fs_now_ms());
    /* The F1 boot-identity amendment accepts disk 0, primary partition 1
     * for CBI1. Loader fingerprints remain a boot-info v2 requirement. */
    drivers_storage_add((struct activation_entry){ .kind = ACTIVATION_STORAGE_IDENTITY,
        .disk = 0, .partition = 1, .readonly = true, .reason = "loader_fingerprints_absent" });
    if (s->setup_mode) storage_setup_records(devices);
    if (policy.optical_poller) optical_start_service(s);
    if (policy.bootlog) {
        struct storage_volume *boot = storage_volume(s, 2);
        e = bootlog_activate(&s->vfs, boot && !boot->error && !boot->fat.readonly, s->sequence);
        klog("[storage] disk_log=%s error=%d", e ? "unavailable" : "available", e);
    }
    if (policy.pagefile) {
        /* M4: the pager manager opens and grows the file asynchronously;
         * without it commit stays RAM-only and the system remains usable. */
        e = storage_pagefile_bind(s, 2, "C:/SYSTEM/PAGEFILE.SWP");
        klog("[storage] pagefile drive=C result=%d", e);
    }
    if (policy.writer) {
        if (timer) task_start(timer);
        else klog("[storage] writeback unavailable; volumes read-only error=%d", s->writer_error);
    }
}
#else
void storage_init(void) { }
/* Host builds have no ATA/ATAPI driver: Setup mode is never requested here
 * (the kernel's definitions are in core/setup_mode.c). */
bool storage_setup_requested(void) { return false; }
void storage_setup_media(struct storage *s, struct ata_device *devices[STORAGE_DISKS]) { (void)s; (void)devices; }
void storage_setup_records(struct ata_device *devices[STORAGE_DISKS]) { (void)devices; }
int storage_writer_start(struct storage *s) { (void)s; return 0; }
#endif
