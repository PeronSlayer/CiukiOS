#include <ciuki/system_settings.h>
/* Read first, then enable writing explicitly. FAT mount/ownership rules:
 * https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf
 * See tests/host/fs/STORAGE-VALIDATION.md for integration decisions.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/storage.h>
#ifdef FS_HOST
/* Hosted stdlib/time also define ABI clock/wait macros (disklease.c). */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <ciuki/abi.h>
#pragma clang diagnostic pop
#else
#include <ciuki/abi.h>
#endif
#include <ciuki/kernel.h>
#include <ciuki/bootlog.h>
#include <ciuki/blackbox.h>
#pragma weak blackbox_bind
#pragma weak blackbox_flush
#pragma weak blackbox_checkpoint
#pragma weak blackbox_reset
#pragma weak blackbox_capture
#pragma weak blackbox_serial_replay
/* G14 boot status (core/bootstat.c): sector 9 of the same partition. */
#include <ciuki/bootstat.h>
#ifndef FS_HOST
#include "scandisk.h"
#endif
#pragma weak bootstat_bind
#pragma weak bootstat_service
#pragma weak bootstat_reset
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
#include <ciuki/task.h>
#else
#include <time.h>
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
        unsigned d = boot ? 2 : s->next_drive++;
        if (d >= 26) return s->disk_errors[disk] = -FS_ENOSPC;
        struct storage_volume *v = &s->volumes[d];
        v->present = true; v->drive = d; v->disk = disk; v->partition = superfloppy ? 0 : i + 1;
        e = blkpart_init(&v->part, dev, part);
        if (e) { v->error = e; continue; }
        v->io = (struct blkdev){ .read = volume_read, .write = volume_write,
            .capacity = part->count, .sector_size = 512, .ctx = v };
        refresh_volume(v);
        /* G29b: a committed conversion locator blocks ordinary FAT parsing.
         * Replay is idempotent and must precede dirty-volume recovery. */
        e = !superfloppy ? fat_convert_recover(dev,part->start,part->count) : 0;
        bool pending=false;
        if (!e && boot) e=fat_convert_boot_pending(dev,part->start,part->count,&pending);
        if (!e) e = fat_mount(&v->fat, &s->cache, &v->io, 0, part->count, false);
        if (!e && pending) {
            int conversion=fat_convert_boot(&v->fat,dev,part->start);
            fat_census_end(&v->fat);
            cache_invalidate(&s->cache,&v->io,true);
            e=conversion;
            if (conversion && !fat_convert_boot_cancel(dev,part->start)) e=0;
            if (!e) e=fat_mount(&v->fat,&s->cache,&v->io,0,part->count,false);
            klog("[storage] boot FAT32 conversion result=%d mount=%d",conversion,e);
        }
        if (!e && boot && s==&system_storage && blackbox_bind)
            blackbox_bind(dev,part->start,part->count,blackbox_serial_replay);
        if (!e && boot && s==&system_storage && bootstat_bind && v->fat.type==32)
            (void)bootstat_bind(dev,part->start,part->count);
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
int storage_add_blank_floppy(struct storage *s,struct blkdev *dev,unsigned drive)
{
    if (!s || !s->ready || s->stopped || !dev || drive>=2 || dev->sector_size!=512 ||
        !dev->capacity || dev->capacity>5760 || !dev->media_generation || dev->quarantined) return -FS_EINVAL;
    vfs_lock(&s->vfs,0);
    struct storage_volume *v=&s->volumes[drive];
    int e=v->present || v->formatting || v->fat.preparing || vfs_drive_present(&s->vfs,drive) ? -FS_EBUSY : 0;
    if (!e) {
        vfs_reserve_locked(&s->vfs,vfs_drive_mask(drive));
        if (v->present || v->formatting || v->fat.preparing || vfs_drive_present(&s->vfs,drive)) e=-FS_EBUSY;
    }
    if (!e) {
        cache_invalidate(&s->cache,&v->io,true);
        struct partition part={.count=dev->capacity}; e=blkpart_init(&v->part,dev,&part);
        if (!e) {
            v->io=(struct blkdev){.read=volume_read,.write=volume_write,.capacity=dev->capacity,.sector_size=512,.ctx=v};
            v->drive=drive; v->disk=UINT32_MAX; v->partition=0;
            v->present=v->removable=v->unformatted=true; v->safely_removed=false;
            v->fat.mounted=false; v->fat.preparing=false; v->fat.type=0; v->fat.spc=0; v->fat.media=0xf0; v->fat.volume_serial=0;
            memset(v->fat.volume_label,0,sizeof(v->fat.volume_label)); refresh_volume(v); namespace_changed();
        }
    }
    vfs_unlock(&s->vfs); return e;
}
int storage_add_floppy(struct storage *s, struct blkdev *dev, unsigned drive)
{ return drive<2 ? add_removable(s,dev,0,0,0,drive) : -FS_EINVAL; }
static int add_removable(struct storage *s, struct blkdev *dev,
                         bool (*cancel)(void *), void (*pause)(void *), void *ctx, unsigned fixed)
{
    if (!s || !s->ready || s->stopped || !dev || dev->sector_size!=512 || (cancel && !pause)) return -FS_EINVAL;
    int e=removable_lock(s,cancel,pause,ctx); if (e) return e;
    if (fixed<2 && (vfs_drive_present(&s->vfs,fixed) || s->volumes[fixed].fat.preparing || s->volumes[fixed].formatting)) {
        vfs_unlock(&s->vfs); return -FS_EBUSY;
    }
    for (unsigned d=0;d<26;d++) if ((s->volumes[d].present || s->volumes[d].fat.preparing || s->volumes[d].formatting) &&
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
    /* Blank superfloppies retain a letter without inventing a FAT mount.
     * A signed but malformed table is refused, never erased as blank media. */
    bool blank=!table.count && !table.ebr_reads && fs_rd16(b+510)!=0xaa55;
    if (blank) { table.count=1; table.entries[0]=(struct partition){.count=dev->capacity}; whole=true; e=0; }
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
        } else if (vfs_drive_present(&s->vfs,d) || s->volumes[d].fat.preparing || s->volumes[d].formatting) {
            vfs_unlock(&s->vfs); return -FS_EBUSY;
        }
        if (d>=26) { vfs_unlock(&s->vfs); return -FS_ENOSPC; }
        struct storage_volume *v=&s->volumes[d];
        attempted[attempts++]=d;
        v->safely_removed=false; v->unformatted=blank;
        if (blank) { v->fat.type=0; v->fat.spc=0; v->fat.media=0xf0; v->fat.volume_serial=0; memset(v->fat.volume_label,0,sizeof(v->fat.volume_label)); }
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
            r=blank ? 0 : fat_prepare_removable(&v->fat,&s->cache,&v->io,0,p->count);
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
            e=v->unformatted ? 0 : vfs_attach_locked(&s->vfs,d,&v->fat);
            if (!e) { v->present=true;s->vfs.drive_flags[d]|=VFS_DRIVE_REMOVABLE; if (v->unformatted) namespace_changed(); }
        }
        /* All letters appear together. An attach refusal rolls back namespace
         * pointers without issuing a flush or presenting an uncertain medium. */
        if (e) for (unsigned i=0;i<count;i++) {
            unsigned d=prepared[i];struct storage_volume *v=&s->volumes[d];
            if (s->vfs.volumes[d]==&v->fat) {s->vfs.volumes[d]=0;s->vfs.generation[d]++;s->vfs.drive_flags[d]=0;namespace_changed();}
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
                s->vfs.volumes[d]=0; s->vfs.generation[d]++; namespace_changed();
                v->fat.mounted=false; v->fat.readonly=true; v->io.quarantined=true;
                for (unsigned i=0;i<VFS_TABLES;i++) if (s->vfs.tables[i]) {
                    memcpy(s->vfs.tables[i]->cwd[d],"/",2); s->vfs.tables[i]->cwd_cluster[d]=0;
                }
            } else { int e=vfs_detach_locked(&s->vfs,d); if (e) return e; }
        }
        if (!surprise) cache_invalidate(&s->cache,&v->io,true);
        s->vfs.drive_flags[d]=0;
        fat_census_end(&v->fat); /* fx-17b: a removed medium cancels its count */
        v->fat.mounted=false; v->present=false; v->unformatted=false;
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

/* G29/G29c: no partition changes or raw fixed-disk access. Full floppy
 * format optionally uses the existing FDC FORMAT TRACK service.
 * BPB/FAT/FSInfo: Microsoft FAT specification, sections 3-6; see
 * docs/design/removable-format.md. All buffers are private to this call. */
struct removable_geometry {
    uint32_t sectors, fat, clusters, root, data;
    unsigned type, spc, reserved, entries;
};
static uint32_t format_family(unsigned type)
{ return type==12 ? CIUKI_FORMAT_FAT12 : type==16 ? CIUKI_FORMAT_FAT16 : type==32 ? CIUKI_FORMAT_FAT32 : 0; }
static bool format_geometry(uint64_t sectors, unsigned type, bool floppy, struct removable_geometry *g)
{
    if (!format_family(type) || sectors>UINT32_MAX || sectors<64) return false;
    unsigned reserved=type==32 ? 32 : 1, entries=type==32 ? 0 : floppy ? 224 : 512;
    uint32_t root=(entries*32u+511)/512;
    if (sectors<=reserved+root+2u) return false;
    for (unsigned spc=1;spc<=64;spc*=2) {
        /* Start with the FAT large enough for all data before subtracting
         * either FAT. The final cluster count can only be smaller. */
        uint64_t max=(sectors-reserved-root)/spc;
        uint64_t fat=((max+2)*type+4095)/4096;
        if (!fat || (type!=32 && fat>65535) || sectors<=reserved+root+2*fat) continue;
        uint64_t clusters=(sectors-reserved-root-2*fat)/spc;
        /* Keep the specification's recommended 16-cluster margin. */
        if ((type==12 && (!clusters || clusters>4068)) ||
            (type==16 && (clusters<4101 || clusters>65508)) ||
            (type==32 && (clusters<65541 || clusters>0x0fffffedu))) continue;
        *g=(struct removable_geometry){(uint32_t)sectors,(uint32_t)fat,(uint32_t)clusters,
            root,(uint32_t)(reserved+2*fat+root),type,spc,reserved,entries};
        return true;
    }
    return false;
}
static int format_label(const char label[12], char padded[11])
{
    unsigned n=0;
    for (;n<12 && label[n];n++) {
        unsigned char c=(unsigned char)label[n];
        if (n==11 || c<32 || c>126) return -FS_EINVAL;
        for (const char *bad="*?.,;:/\\|+=<>[]\"";*bad;bad++) if (c==(unsigned char)*bad) return -FS_EINVAL;
        padded[n]=(char)(c>='a' && c<='z' ? c-32 : c);
    }
    if (n==12) return -FS_EINVAL;
    for (;n<11;n++) padded[n]=' ';
    return 0;
}
static int format_live(struct storage_volume *v, uint32_t generation)
{
    struct blkdev *dev=v->part.parent;
    int e=fs_request_check();
    if (e) return e;
    if (!dev || dev->quarantined || dev->media_generation!=generation) return -FS_EIO;
    return 0;
}
static int format_transfer(struct storage_volume *v, uint32_t generation, uint32_t lba,
                           unsigned n, void *buf, bool write)
{
    int e=format_live(v,generation); if (e) return e;
    struct blkdev *dev=blkpart_device(&v->part);
    e=write ? dev->write(dev,lba,n,buf) : dev->read(dev,lba,n,buf);
    return e ? e : format_live(v,generation);
}
static int format_flush(struct storage_volume *v, uint32_t generation)
{
    int e=format_live(v,generation); if (e) return e;
    struct blkdev *dev=blkpart_device(&v->part);
    e=dev->flush ? dev->flush(dev) : blkdev_durable(dev) ? 0 : -FS_EROFS;
    return e ? e : format_live(v,generation);
}
static void format_boot(uint8_t b[512], const struct removable_geometry *g,
                        const struct storage_volume *v, const char label[11], uint32_t serial,
                        uint16_t sectors_per_track, uint16_t heads)
{
    memset(b,0,512);
    b[0]=0xeb; b[1]=0xfe; b[2]=0x90; /* deliberately nonbootable */
    memcpy(b+3,"CIUKIOS ",8); fs_wr16(b+11,512); b[13]=(uint8_t)g->spc;
    fs_wr16(b+14,(uint16_t)g->reserved); b[16]=2; fs_wr16(b+17,(uint16_t)g->entries);
    if (g->sectors<=65535) fs_wr16(b+19,(uint16_t)g->sectors);
    else fs_wr32(b+32,g->sectors);
    b[21]=v->drive<2 ? (v->fat.media ? v->fat.media : 0xf0) : 0xf8;
    if (g->type!=32) fs_wr16(b+22,(uint16_t)g->fat);
    /* Retain the mounted floppy's actual geometry (including 720K/1.2M).
     * USB CHS fields are advisory; LBA fields delimit all transfers. */
    fs_wr16(b+24,sectors_per_track);
    fs_wr16(b+26,heads);
    fs_wr32(b+28,(uint32_t)v->part.start);
    unsigned ext=36;
    if (g->type==32) {
        fs_wr32(b+36,g->fat); fs_wr32(b+44,2); fs_wr16(b+48,1); fs_wr16(b+50,6); ext=64;
    }
    b[ext]=v->drive<2 ? (uint8_t)v->drive : 0x80; b[ext+2]=0x29;
    fs_wr32(b+ext+3,serial);
    memcpy(b+ext+7,label,11);
    if (!memcmp(label,"           ",11)) memcpy(b+ext+7,"NO NAME    ",11);
    memcpy(b+ext+18,g->type==12 ? "FAT12   " : g->type==16 ? "FAT16   " : "FAT32   ",8);
    b[510]=0x55; b[511]=0xaa;
}
/* Platform optional in hosted/storage-only links. */
int floppy_format_track(struct blkdev *, unsigned, unsigned) __attribute__((weak));
static int format_bad_fat(struct storage_volume *v, const struct ciuki_media_format *r,
                          const struct removable_geometry *g, const uint8_t bad[512], uint8_t *b)
{
    for (uint32_t cluster=2;cluster<g->clusters+2;cluster++) {
        if (!(bad[cluster/8]&(1u<<(cluster%8)))) continue;
        uint32_t offset=cluster+cluster/2, sector=offset/512;
        for (unsigned copy=0;copy<2;copy++) {
            uint32_t lba=g->reserved+copy*g->fat+sector;
            unsigned n=offset%512==511 ? 2 : 1;
            int e=format_transfer(v,r->generation,lba,n,b,false); if (e) return e;
            unsigned at=offset%512; uint16_t value=fs_rd16(b+at);
            value=cluster&1 ? (uint16_t)((value&15)|0xff70) : (uint16_t)((value&0xf000)|0xff7);
            fs_wr16(b+at,value);
            e=format_transfer(v,r->generation,lba,n,b,true); if (e) return e;
        }
    }
    return 0;
}
static int format_write(struct storage_volume *v, const struct ciuki_media_format *r,
                         const struct removable_geometry *g, const char label[11], uint8_t *b)
{
    uint8_t bad[512]={0};
    /* Standard FDC media capacities: 360K/720K, 1.2M, 1.44M and 2.88M. */
    uint16_t sectors_per_track=g->sectors==720 || g->sectors==1440 ? 9 :
        g->sectors==2400 ? 15 : g->sectors==5760 ? 36 : 18, heads=2;
    int e=0;
    if (r->flags&CIUKI_FORMAT_LOW_LEVEL) {
        if (!floppy_format_track || g->type!=12) return -FS_EOPNOTSUPP;
        for (unsigned track=0;track<g->sectors/sectors_per_track;track++) {
            e=format_live(v,r->generation); if (e) return e;
            e=floppy_format_track(v->part.parent,track/heads,track%heads);
            /* A failed track may contain usable sectors. The following
             * read pass diagnoses metadata and marks only bad data. */
            if (e && e!=-FS_EIO) return e;
            if ((e=format_live(v,r->generation))) return e;
            fs_service();
        }
    }
    e=format_transfer(v,r->generation,0,1,b,false);
    if (e) return e;
    if (v->drive>=2) { sectors_per_track=fs_rd16(b+24); heads=fs_rd16(b+26); }
    else if (!(r->flags&CIUKI_FORMAT_LOW_LEVEL) && fs_rd16(b+11)==512 && fs_rd16(b+26)==2 &&
             (fs_rd16(b+24)==9 || fs_rd16(b+24)==15 || fs_rd16(b+24)==18 || fs_rd16(b+24)==36))
        sectors_per_track=fs_rd16(b+24);
    if (!(r->flags&CIUKI_FORMAT_QUICK)) {
        for (uint32_t lba=0;lba<g->sectors;) {
            unsigned n=v->drive<2 ? 1 : g->sectors-lba<8 ? g->sectors-lba : 8;
            e=format_transfer(v,r->generation,lba,n,b,false);
            if (e) {
                if (e!=-FS_EIO || v->drive>=2 || g->type!=12 || lba<g->data ||
                    lba>=g->data+g->clusters*g->spc || format_live(v,r->generation)) return e;
                unsigned cluster=2+(lba-g->data)/g->spc;
                bad[cluster/8]|=(uint8_t)(1u<<(cluster%8));
            }
            lba+=n; fs_service();
        }
    }
    memset(b,0,512);
    e=format_transfer(v,r->generation,0,1,b,true);
    if (!e) e=format_flush(v,r->generation);
    if (e) return e;
    /* Reserved sectors, both FATs and the root directory/cluster. */
    uint32_t end=g->data+(g->type==32 ? g->spc : 0);
    memset(b,0,4096);
    for (uint32_t lba=1;lba<end;) {
        unsigned n=end-lba<8 ? end-lba : 8;
        e=format_transfer(v,r->generation,lba,n,b,true); if (e) return e;
        lba+=n; fs_service();
    }
    for (unsigned copy=0;copy<2;copy++) {
        memset(b,0,512);
        unsigned media=v->drive<2 ? (v->fat.media ? v->fat.media : 0xf0) : 0xf8;
        if (g->type==12) { b[0]=(uint8_t)media; b[1]=0xff; b[2]=0xff; }
        else if (g->type==16) { fs_wr16(b,(uint16_t)(0xff00u|media)); fs_wr16(b+2,0xffff); }
        else { fs_wr32(b,0x0fffff00u|media); fs_wr32(b+4,0x0fffffff); fs_wr32(b+8,0x0fffffff); }
        e=format_transfer(v,r->generation,g->reserved+copy*g->fat,1,b,true); if (e) return e;
    }
    if (g->type==12) { e=format_bad_fat(v,r,g,bad,b); if (e) return e; }
    if (memcmp(label,"           ",11)) {
        memset(b,0,512); memcpy(b,label,11); b[11]=8;
        uint32_t root=g->type==32 ? g->data : g->reserved+2*g->fat;
        e=format_transfer(v,r->generation,root,1,b,true); if (e) return e;
    }
    if (g->type==32) {
        memset(b,0,512); fs_wr32(b,0x41615252); fs_wr32(b+484,0x61417272);
        fs_wr32(b+488,g->clusters-1); fs_wr32(b+492,3); fs_wr32(b+508,0xaa550000);
        e=format_transfer(v,r->generation,1,1,b,true);
        if (!e) e=format_transfer(v,r->generation,7,1,b,true);
        if (e) return e;
    }
    uint32_t serial=v->fat.volume_serial+1;
    if (!serial) serial=1;
    format_boot(b,g,v,label,serial,sectors_per_track,heads);
    if (g->type==32) e=format_transfer(v,r->generation,6,1,b,true);
    if (!e) e=format_flush(v,r->generation);
    if (!e) e=format_transfer(v,r->generation,0,1,b,true);
    if (!e) e=format_flush(v,r->generation);
    return e;
}
int storage_format_removable(struct storage *s, struct ciuki_media_format *r, bool execute)
{
    if (!r || r->version!=CIUKI_MEDIA_VERSION || r->size!=sizeof(*r) ||
        r->drive>=26 || !r->generation || r->reserved) return -FS_EINVAL;
    if (!s || !s->ready || s->stopped) return -FS_ENOENT;
    if (s->setup_mode || r->drive==2 || r->drive==3) return -FS_EROFS;
    unsigned d=r->drive;
    struct removable_geometry g;
    char label[11];
    if (execute && ((r->flags&~(CIUKI_FORMAT_QUICK|CIUKI_FORMAT_LOW_LEVEL)) || !format_family(r->filesystem) ||
        format_label(r->label,label))) return -FS_EINVAL;
    vfs_lock(&s->vfs,0);
    struct storage_volume *v=&s->volumes[d];
    struct blkdev *dev=v->part.parent;
    int e=!v->present || (!v->unformatted && (s->vfs.volumes[d]!=&v->fat || !v->fat.mounted)) ? -FS_ENOENT :
        !v->removable ? -FS_EROFS : !dev || dev->quarantined || dev->media_generation!=r->generation ? -FS_EIO : 0;
    if (!e && execute && (r->flags&CIUKI_FORMAT_LOW_LEVEL) && (d>=2 || (r->flags&CIUKI_FORMAT_QUICK))) e=-FS_EINVAL;
    if (!e && (!dev->read || !blkdev_durable(dev))) e=-FS_EROFS;
    uint32_t mask=0;
    for (unsigned i=0;!e && i<26;i++) if (s->volumes[i].removable && s->volumes[i].part.parent==dev) {
        if (i!=d && (s->volumes[i].present || s->volumes[i].formatting || s->volumes[i].fat.preparing)) e=-FS_EBUSY;
        /* Include retained letters too: remove_removable reserves this
         * same union, so no late extension can enter a waiting path. */
        mask|=vfs_drive_mask(i);
    }
    if (!e && v->part.block.capacity>UINT32_MAX) e=-FS_EOPNOTSUPP;
    uint32_t supported=0;
    static const unsigned families[]={12,16,32};
    for (unsigned i=0;!e && i<3;i++) {
        unsigned type=families[i];
        if (v->partition && type!=v->fat.type) continue;
        if (format_geometry(v->part.block.capacity,type,d<2,&g)) supported|=format_family(type);
    }
    if (!e && execute && !(supported&format_family(r->filesystem))) e=-FS_EOPNOTSUPP;
    if (!e && !execute) {
        r->filesystem=v->unformatted ? (d<2 ? 12 : (supported&CIUKI_FORMAT_FAT32) ? 32 : (supported&CIUKI_FORMAT_FAT16) ? 16 : 12) : v->fat.type; r->flags=v->unformatted ? CIUKI_FORMAT_UNFORMATTED : 0; r->sectors=(uint32_t)v->part.block.capacity;
        r->cluster_bytes=(uint32_t)v->fat.spc*512; r->supported_filesystems=supported;
        memset(r->label,0,sizeof(r->label)); memcpy(r->label,v->fat.volume_label,11);
    }
    if (!e && execute) {
        vfs_reserve_locked(&s->vfs,mask);
        /* Recheck after reservation waits: safe/surprise removal may have
         * changed publication while we yielded the namespace. */
        if (!v->present || (!v->unformatted && s->vfs.volumes[d]!=&v->fat) || dev!=v->part.parent ||
            dev->quarantined || dev->media_generation!=r->generation) e=-FS_EIO;
        uint8_t *buffer=0;
        if (!e) { buffer=fs_page_alloc(); if (!buffer) e=-FS_ENOMEM; }
        if (!e && !format_geometry(v->part.block.capacity,r->filesystem,d<2,&g)) e=-FS_EINVAL;
        if (!e) {
            v->formatting=true;
            e=storage_remove_removable_locked(s,dev,false);
        }
        if (!e) {
            v->safely_removed=false;
            e=VFS_IO(&s->vfs,format_write(v,r,&g,label,buffer));
            if (!e) {
                refresh_volume(v);
                e=VFS_IO(&s->vfs,fat_prepare_removable(&v->fat,&s->cache,&v->io,0,v->io.capacity));
                v->fat.preparing=false;
                v->fat.write_through=true;
            }
            if (!e) { v->read_gate=true; e=VFS_IO(&s->vfs,fat_enable_write_removable(&v->fat)); }
            if (!e) e=format_live(v,r->generation);
            if (!e) e=vfs_attach_locked(&s->vfs,d,&v->fat);
            if (!e) {
                v->present=true; v->unformatted=false; s->vfs.drive_flags[d]|=VFS_DRIVE_REMOVABLE;
                r->sectors=g.sectors; r->cluster_bytes=g.spc*512; r->supported_filesystems=supported;
                memset(r->label,0,sizeof(r->label)); memcpy(r->label,v->fat.volume_label,11);
            } else {
                v->error=e; v->fat.mounted=false; v->read_gate=false;
                cache_invalidate(&s->cache,&v->io,true);
            }
        }
        v->formatting=false;
        if (buffer) fs_page_free(buffer);
    }
    vfs_unlock(&s->vfs);
    return e;
}
/* Whole-diskette operations: retained physical view, exclusive namespace,
 * source snapshot before any destination write, exact capacity and read-back. */
int storage_floppy_copy(struct storage *s, struct ciuki_floppy_image *r, storage_floppy_copy_fn copy, void *ctx, bool write)
{
    if (!s || !s->ready || s->stopped || !r || !copy || r->version!=CIUKI_MEDIA_VERSION ||
        r->size!=sizeof(*r) || r->drive>=2 || !r->generation || r->flags || r->reserved ||
        !r->sectors || r->sectors>5760) return -FS_EINVAL;
    unsigned d=r->drive;
    vfs_lock(&s->vfs,0);
    vfs_reserve_locked(&s->vfs,vfs_drive_mask(d));
    struct storage_volume *v=&s->volumes[d]; struct blkdev *dev=v->part.parent;
    int e=!v->present || !v->removable || v->formatting ? -FS_EBUSY :
        !dev || dev->capacity!=r->sectors || v->partition ? -FS_EINVAL : format_live(v,r->generation);
    if (!e && write && !blkdev_durable(dev)) e=-FS_EROFS;
    uint8_t *check=0,*buffer=0;
    if (!e) { check=fs_page_alloc(); buffer=fs_page_alloc(); if (!check || !buffer) e=-FS_ENOMEM; }
    bool held=false,claimed=false,blank=v->unformatted,readonly=v->fat.readonly;
    if (!e) {
        v->formatting=true; claimed=true;
        e=storage_remove_removable_locked(s,dev,false);
        if (!e) { held=true; v->safely_removed=false; }
    }
    if (!e && write && blank && floppy_format_track) {
        unsigned sectors=r->sectors==720 || r->sectors==1440 ? 9 : r->sectors==2400 ? 15 : r->sectors==5760 ? 36 : 18;
        for (unsigned track=0;!e && track<r->sectors/sectors;track++) {
            e=VFS_IO(&s->vfs,format_live(v,r->generation));
            if (!e) e=VFS_IO(&s->vfs,floppy_format_track(dev,track/2,track%2));
        }
    }
    for (uint32_t lba=0;!e && lba<r->sectors;) {
        unsigned n=r->sectors-lba<8 ? r->sectors-lba : 8;
        if (write) e=copy(ctx,lba*512,buffer,n*512,false);
        if (!e) e=VFS_IO(&s->vfs,format_transfer(v,r->generation,lba,n,buffer,write));
        if (!e && !write) e=copy(ctx,lba*512,buffer,n*512,true);
        if (!e && write) e=VFS_IO(&s->vfs,format_flush(v,r->generation));
        if (!e && write) e=VFS_IO(&s->vfs,format_transfer(v,r->generation,lba,n,check,false));
        if (!e && write && memcmp(check,buffer,n*512)) e=-FS_EIO;
        lba+=n;
    }
    /* Failed writes leave an unpublished raw identity for an explicit
     * retry/format; never remount a partially copied FAT as usable. */
    if (held && !e) {
        refresh_volume(v);
        e=VFS_IO(&s->vfs,fat_prepare_removable(&v->fat,&s->cache,&v->io,0,v->io.capacity));
        v->fat.preparing=false; v->fat.write_through=true;
        /* Reading a write-protected source must not require a new write
         * session. Preserve its original read-only mount on return. */
        if (!e && (write || !readonly)) e=VFS_IO(&s->vfs,fat_enable_write_removable(&v->fat));
        if (!e) e=format_live(v,r->generation);
        if (!e) e=vfs_attach_locked(&s->vfs,d,&v->fat);
        if (!e) { v->present=true; v->unformatted=false; s->vfs.drive_flags[d]|=VFS_DRIVE_REMOVABLE; }
    }
    if (held && e) { v->present=true; v->unformatted=true; v->fat.mounted=false; cache_invalidate(&s->cache,&v->io,true); }
    if (claimed) v->formatting=false;
    if (check) fs_page_free(check);
    if (buffer) fs_page_free(buffer);
    vfs_unlock(&s->vfs);
    return e;
}
static int floppy_memory_copy(void *ctx,uint32_t offset,void *buffer,uint32_t bytes,bool output)
{
    if (output) memcpy((uint8_t *)ctx+offset,buffer,bytes);
    else memcpy(buffer,(uint8_t *)ctx+offset,bytes);
    return 0;
}
int storage_floppy_image(struct storage *s,struct ciuki_floppy_image *r,void *buffer,bool write)
{
    return buffer ? storage_floppy_copy(s,r,floppy_memory_copy,buffer,write) : -FS_EINVAL;
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
#ifndef FS_HOST
/* G14 ScanDisk at start (docs/design/startup-recovery.md 2.4): the reports
 * of this start, one per drive, for system call 99's REPORT. */
static struct ciuki_scandisk scandisk_reports[26];
static bool scandisk_ran[26], scandisk_busy;
static bool boot_option(const char *token)
{
    size_t n = strlen(token);
    for (unsigned i = 0; i < sizeof(g_boot.options) && g_boot.options[i];) {
        unsigned first = i;
        while (i < sizeof(g_boot.options) && g_boot.options[i] && g_boot.options[i] != ' ' &&
               g_boot.options[i] != '\r' && g_boot.options[i] != '\n') i++;
        if (i - first == n && !memcmp(g_boot.options + first, token, n)) return true;
        if (i < sizeof(g_boot.options) && g_boot.options[i]) i++;
    }
    return false;
}
/* Reload a volume from disk after the engine wrote it (storage_boot_cfg's
 * pattern): the VFS keeps its mount, identity and publication epochs. */
static int remount_from_disk(struct storage *s, struct storage_volume *v)
{
    uint32_t identity_epoch=v->fat.identity_mount_generation, generation=v->io.media_generation;
    uint64_t groups=v->fat.publication_count, frontier=v->fat.publication_frontier;
    struct cache_completion completion=v->fat.completion;
    void (*changed)(void *,struct fat_volume *,const struct fat_entry *,const struct fat_entry *,enum fat_identity_change)=v->fat.identity_changed;
    void *identity_ctx=v->fat.identity_ctx;
    void (*observer)(void *,uint64_t,enum fat_publication_stage,char,int)=v->fat.publication_observer;
    void *observer_ctx=v->fat.publication_ctx;
    cache_invalidate(&s->cache, &v->io, true);
    int e=VFS_IO(&s->vfs,fat_mount(&v->fat,&s->cache,&v->io,0,v->io.capacity,false));
    if (!e && generation!=v->io.media_generation) e=-FS_EIO;
    if (e) { vfs_identity_expire(&s->vfs,&v->fat); return e; }
    v->fat.identity_mount_generation=identity_epoch;
    v->fat.identity_changed=changed; v->fat.identity_ctx=identity_ctx;
    v->fat.publication_count=v->fat.publication_group=v->fat.publication_durable=groups;
    v->fat.publication_frontier=frontier; v->fat.publication_generation=generation;
    v->fat.completion=completion;
    v->fat.publication_observer=observer; v->fat.publication_ctx=observer_ctx;
    v->read_gate = !(v->fat.ro_reasons & ~(FAT_RO_REQUEST | FAT_RO_DURABILITY | FAT_RO_DIRTY | FAT_RO_LOST | FAT_RO_IO_FLAG));
    if (v->read_gate && !v->read_sequence) v->read_sequence = ++s->sequence;
    return 0;
}
/* Namespace and volume lock held, before the FAT write session opens. A
 * fixed volume marked unclean or with a hard error, or every fixed volume
 * when the check was scheduled, unless Esc, autoscan=0, a test request. */
static void boot_scandisk(struct storage *s, struct storage_volume *v, unsigned d)
{
    if (s != &system_storage || d >= 26 || scandisk_ran[d] || v->removable || v->optical || s->setup_mode) return;
    uint32_t flags = g_boot.flags;
    if ((flags & (CBI_F_TEST_REQUEST | CBI_F_SCANDISK_SKIP)) || boot_option("autoscan=0")) return;
    bool unclean = (v->fat.ro_reasons & (FAT_RO_DIRTY | FAT_RO_IO_FLAG | FAT_RO_CORRUPT | FAT_RO_COPIES | FAT_RO_LOST)) != 0;
    if (!unclean && !(flags & CBI_F_SCANDISK)) return;
    if (!blkdev_durable(blkpart_device(&v->part))) return;
    scandisk_ran[d] = true;
    struct ciuki_scandisk *r = &scandisk_reports[d];
    memset(r, 0, sizeof(*r));
    r->drive = d;
    r->flags = CIUKI_SCANDISK_FIX | CIUKI_SCANDISK_AT_START |
        ((flags & CBI_F_SCANDISK_THOROUGH) || (v->fat.ro_reasons & FAT_RO_IO_FLAG) ? CIUKI_SCANDISK_THOROUGH : 0);
    klog("[scandisk] start drive=%c reasons=0x%x scheduled=%u thorough=%u", 'A' + d, v->fat.ro_reasons,
         !!(flags & CBI_F_SCANDISK), !!(r->flags & CIUKI_SCANDISK_THOROUGH));
    refresh_volume(v);
    cache_invalidate(&s->cache, &v->io, true);
    (void)scandisk_run(&v->io, r);
    int e = remount_from_disk(s, v);
    if (e) { v->error = e; klog("[scandisk] remount drive=%c error=%d", 'A' + d, e); }
}
int storage_scandisk_report(unsigned d, struct ciuki_scandisk *out)
{
    if (d >= 26) return -FS_EINVAL;
    vfs_lock(&system_storage.vfs,0);
    if (scandisk_ran[d]) *out = scandisk_reports[d];
    else { memset(out, 0, sizeof(*out)); out->version = CIUKI_SCANDISK_VERSION; out->size = sizeof(*out); out->drive = d; }
    vfs_unlock(&system_storage.vfs);
    return 0;
}
/* On demand (ScanDisk, Check Now): a read-only check of a mounted FAT drive
 * after its writes are durable, the drive locked for the duration. A fix a
 * fixed volume in use needs is scheduled for the next start. */
int storage_scandisk(unsigned d, struct ciuki_scandisk *r, bool *schedule)
{
    struct storage *s = &system_storage;
    *schedule = false;
    if (d >= 26) return -FS_EINVAL;
    vfs_lock(&s->vfs,0);
    struct storage_volume *v = storage_volume(s, d);
    bool ok = v && !v->error && v->fat.mounted && !v->optical && !s->stopped;
    bool busy = scandisk_busy;
    if (ok && !busy) scandisk_busy = true;
    vfs_unlock(&s->vfs);
    if (!ok) return -FS_ENOENT; /* not a mounted FAT drive */
    if (busy) return -FS_EBUSY;
    uint32_t wanted = r->flags;
    vfs_lock(&s->vfs,vfs_drive_mask(d)); vfs_tag_locked(&s->vfs,"scandisk"); vfs_site_locked(&s->vfs,"mount");
    int e = v->fat.readonly ? 0 : VFS_IO(&s->vfs,fat_commit(&v->fat));
    if (!e) {
        refresh_volume(v);
        r->drive = d;
        r->flags = wanted & ~(uint32_t)CIUKI_SCANDISK_FIX;
        e = scandisk_run(&v->io, r);
    }
    vfs_unlock(&s->vfs);
    vfs_lock(&s->vfs,0); scandisk_busy = false; vfs_unlock(&s->vfs);
    fs_service();
    if (!e && (wanted & CIUKI_SCANDISK_FIX) && (r->flags & CIUKI_SCANDISK_ERRORS) && !v->removable) *schedule = true;
    r->flags |= wanted & CIUKI_SCANDISK_FIX;
    return e;
}
#endif

int storage_enable_write(struct storage *s, unsigned d)
{
    struct storage_volume *v = storage_volume(s, d);
    if (!v || s->stopped) return -FS_ENOENT;
    /* f9-01 (D2): no fixed volume becomes writable in Setup mode; f9-02's
     * destination publication is the only later exception. */
    if (s->setup_mode && !v->removable) { v->refused++; return -FS_EROFS; }
    if (s->writer_error) return s->writer_error;
    if (v->error) return v->error;
    if (v->writes_before_gate) { v->refused++; return -FS_EROFS; }
#ifndef FS_HOST
    /* G14: ScanDisk repairs what the read gate would refuse (cross-links,
     * FAT copies) and the dirty volume, before any write session. */
    vfs_lock(&s->vfs,vfs_drive_mask(d)); vfs_tag_locked(&s->vfs,"mount"); vfs_site_locked(&s->vfs,"mount");
    boot_scandisk(s, v, d);
    vfs_unlock(&s->vfs);
    if (v->error) return v->error;
#endif
    if (!v->read_gate) { v->refused++; return -FS_EROFS; }
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
int storage_activate_removable(struct storage *s, struct blkdev *dev)
{
    if (!s || !s->ready || s->stopped || !dev) return 0;
    int writable=0;
    for (unsigned d=0;d<26;d++) {
        vfs_lock(&s->vfs,0);
        struct storage_volume *v=&s->volumes[d];
        bool candidate=v->present && v->removable && v->part.parent==dev && v->fat.mounted && v->fat.readonly &&
            !v->error && v->read_gate && !v->writes_before_gate && !s->setup_mode && !s->writer_error;
        vfs_unlock(&s->vfs);
        if (!candidate) continue;
        vfs_lock(&s->vfs,vfs_drive_mask(d)); vfs_tag_locked(&s->vfs,"mount"); vfs_site_locked(&s->vfs,"mount");
        int e=-FS_ENOENT; bool dirty=false;
        if (v->present && v->part.parent==dev && v->fat.mounted) {
            dirty=(v->fat.ro_reasons&(FAT_RO_DIRTY|FAT_RO_IO_FLAG))!=0;
            refresh_volume(v);
            e=VFS_IO(&s->vfs,fat_enable_write_removable(&v->fat));
            if (!e && !v->write_sequence) v->write_sequence=++s->sequence;
            if (e) v->refused++;
            if (v->fat.free_unknown && !v->fat.census) (void)VFS_IO(&s->vfs,fat_census_start(&v->fat));
        }
        if (v->fat.census) klog("[storage] removable drive=%c mode=%s dirty=%u reason=%s free=counting census_ms=%u result=%d",
             'A'+d,v->fat.readonly ? "ro" : "rw",dirty,v->fat.readonly ? fat_readonly_reason(&v->fat) : "writable",v->fat.repair_ms,e);
        else klog("[storage] removable drive=%c mode=%s dirty=%u reason=%s free=%u census_ms=%u result=%d",
             'A'+d,v->fat.readonly ? "ro" : "rw",dirty,v->fat.readonly ? fat_readonly_reason(&v->fat) : "writable",
             v->fat.free_clusters,v->fat.repair_ms,e);
        vfs_unlock(&s->vfs);
        fs_service();
        if (!e) writable++;
    }
    if (writable) fs_event_wake(&s->vfs.volume_changed);
    return writable;
}
/* fx-23: background free-count steps are maintenance I/O: their FAT reads
 * queue behind foreground reads and stay at the cold end of the cache, and
 * a step is not taken while a foreground caller waits for the drive (the
 * next turn retries). Windows 98 runs such work "while the computer is
 * idle" (Resource Kit ch. 10, Task Scheduler and Compression Agent). */
static uint64_t census_now(void *arg) { (void)arg; return (uint64_t)fs_now_ms()*1000u; } /* microseconds */
static void census_pause(void *arg)
{
    (void)arg;
#ifdef FS_HOST
    struct timespec t={0,1000000}; nanosleep(&t,0);
#else
    task_sleep_ms(1);
#endif
}
static const struct vfs_request census_request={.deadline=UINT64_MAX/2,.now=census_now,.pause=census_pause,
    .operation="census",.site="mount",.background=true};
#define CENSUS_YIELD_MS 20u
unsigned storage_census_service(struct storage *s, uint32_t now)
{
    if (!s || !s->ready || s->stopped) return 0;
    unsigned running=0;
    for (unsigned d=0;d<26;d++) {
        vfs_lock(&s->vfs,0);
        struct storage_volume *v=&s->volumes[d];
        bool due=v->present && v->removable && v->fat.census && v->fat.mounted &&
            (!v->census_next_ms || (int32_t)(now-v->census_next_ms)>=0);
        bool counting=v->present && v->fat.census;
        bool contended=due && s->vfs.diagnostic_waiters[d];
        if (contended) { v->census_next_ms=now+CENSUS_YIELD_MS; v->census_yields++; }
        vfs_unlock(&s->vfs);
        if (!due || contended) { running+=counting; continue; }
        vfs_lock(&s->vfs,vfs_drive_mask(d)); vfs_tag_locked(&s->vfs,"census"); vfs_site_locked(&s->vfs,"mount");
        int e=1; uint32_t began=fs_now_ms();
        if (v->present && v->fat.census && v->fat.mounted) {
            refresh_volume(v);
            struct fs_request_scope scope; fs_request_enter(&scope,&census_request);
            e=VFS_IO(&s->vfs,fat_census_step(&v->fat,128));
            fs_request_leave(&scope);
            if (e<0) {
                klog("[storage] free_count drive=%c result=%d state=abandoned",'A'+d,e);
                fat_census_end(&v->fat);
            }
        }
        uint32_t spent=fs_now_ms()-began;
        /* At most a quarter of the medium's time: the reservation is held
         * for one FAT read-ahead window, then readers and writers own it. */
        v->census_next_ms=fs_now_ms()+3*spent+20;
        if (!e) running++;
        vfs_unlock(&s->vfs);
        fs_service();
    }
    return running;
}
/* fx-23: a drive reservation still held after STORAGE_HOLD_REPORT_MS is
 * named while it is held (the release-time vfs_hold record is budgeted and
 * never appears for a hold that outlives the session), once per hold. */
#define STORAGE_HOLD_REPORT_MS 2000u
unsigned storage_hold_watch(struct storage *s, uint32_t now)
{
    if (!s || !s->ready) return 0;
    struct hold_report { struct vfs_hold_info owner; uint32_t ms, waiters; char drive; } found[4];
    unsigned n=0;
    vfs_lock(&s->vfs,0);
    for (unsigned d=0;d<26 && n<4;d++) {
        if (!(s->vfs.reserved&(1u<<d))) { s->hold_reported[d]=0; continue; }
        struct vfs_hold_info *h=&s->vfs.holds[d];
        uint32_t ms=now-h->started;
        if (ms<STORAGE_HOLD_REPORT_MS || s->hold_reported[d]==h->started+1u) continue;
        s->hold_reported[d]=h->started+1u;
        found[n++]=(struct hold_report){*h,ms,s->vfs.diagnostic_waiters[d],(char)('A'+d)};
    }
    vfs_unlock(&s->vfs);
    for (unsigned i=0;i<n;i++)
        klog("[storage] vfs_hold_long drive=%c ms=%u task=%s#%u op=%s site=%s waiters=%u",
             found[i].drive,found[i].ms,found[i].owner.task.name[0] ? found[i].owner.task.name : "none",
             found[i].owner.task.id,found[i].owner.op[0] ? found[i].owner.op : "metadata",
             found[i].owner.site[0] ? found[i].owner.site : "vfs",found[i].waiters);
    return n;
}
#ifdef FS_HOST
#include <stdio.h>
#define stall_format snprintf
#else
#define stall_format ksnprintf
/* T23 trial 2ca3b0b6: storage writer progress (storage.h). The stamp and
 * stage are written by the system writer only and read by cpustat. */
static uint32_t writer_beat, writer_passes;
static const char *writer_stage = "start";
static struct task *writer_task;
void storage_writer_progress(struct storage *s, const char *stage)
{
    if (s != &system_storage) return;
    if (!strncmp(stage, "start", 6)) __atomic_store_n(&writer_task, g_current, __ATOMIC_RELEASE);
    else if (!strncmp(stage, "stop", 5)) __atomic_store_n(&writer_task, (struct task *)0, __ATOMIC_RELEASE);
    else if (!strncmp(stage, "idle", 5)) __atomic_add_fetch(&writer_passes, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&writer_stage, stage, __ATOMIC_RELEASE);
    __atomic_store_n(&writer_beat, (uint32_t)fs_now_ms(), __ATOMIC_RELEASE);
}
#endif
bool storage_stall_due(uint32_t now, uint32_t beat, uint32_t *reported)
{
    if (now - beat < STORAGE_STALL_MS) { *reported = 0; return false; }
    if (*reported && now - *reported < STORAGE_STALL_REPEAT_MS) return false;
    *reported = now ? now : 1u; /* zero means nothing reported */
    return true;
}
static void stall_name(char out[20], const struct fs_task_identity *t)
{
    if (!t->name[0]) { memcpy(out, "none", 5); return; }
    stall_format(out, 20, "%s#%u", t->name, t->id);
}
int storage_stall_describe(const struct storage *s, uint32_t now, const char *stage, uint32_t age,
                           unsigned passes, const char *writer_wait, unsigned part, char *line, size_t size)
{
    const struct vfs *v = &s->vfs;
    char owner[20]; int n;
    if (!part) {
        const struct vfs_hold_info *o = &v->namespace_owner;
        stall_name(owner, &o->task);
        n = stall_format(line, size, "[storage] writer_stall ms=%u stage=%s passes=%u wait=%s namespace=%s ns_held=%u op=%s site=%s",
                      age, stage ? stage : "none", passes, writer_wait ? writer_wait : "unknown", owner,
                      fs_lock_holder((fs_lock *)&v->lock) != 0, o->op[0] ? o->op : "-", o->site[0] ? o->site : "-");
    } else {
        const unsigned d = 2; const struct vfs_hold_info *h = &v->holds[d];
        stall_name(owner, &h->task);
        const char *state = (v->mutating & (1u << d)) ? "mutating" : (v->reserved & (1u << d)) ? "reserved" : "free";
        n = stall_format(line, size, "[storage] writer_stall drive=C state=%s hold=%s op=%s hold_ms=%u turn=%u/%u gate=%u queued=%u readers=%u cache_held=%u dirty=%u batch=%u",
                      state, owner, h->op[0] ? h->op : "-", h->task.name[0] ? now - h->started : 0u,
                      v->turn_depth[d], v->turn_waiters[d], v->publish_waiting[d], v->next[d] - v->serving[d],
                      v->readers[d], fs_lock_holder((fs_lock *)&s->cache.lock) != 0, s->cache.dirty_bytes,
                      (unsigned)s->cache.batch_busy);
    }
    return n > 0 && (size_t)n < size ? n : 0;
}
#ifndef FS_HOST
unsigned storage_stall_poll(uint32_t now)
{
    static uint32_t reported;
    struct storage *s = &system_storage;
    struct task *w = __atomic_load_n(&writer_task, __ATOMIC_ACQUIRE);
    if (!s->ready || s->stopped || !w) return 0;
    uint32_t beat = __atomic_load_n(&writer_beat, __ATOMIC_ACQUIRE);
    if (!storage_stall_due(now, beat, &reported)) return 0;
    /* One consistent uniprocessor snapshot; nothing is taken or waited for. */
    char wait[48], a[240], b[240];
    uint32_t f = irq_save();
    const struct task *owner = w->waiting_on ? w->waiting_on->owner : 0;
    if (owner) ksnprintf(wait, sizeof(wait), "lock:%s#%u", owner->name, owner->id);
    else if (w->depends_on) ksnprintf(wait, sizeof(wait), "%s:%s#%u", w->depend_reason == FS_DEPEND_CACHE ? "cache" :
                                      w->depend_reason == FS_DEPEND_IO ? "io" : "drive", w->depends_on->name, w->depends_on->id);
    else ksnprintf(wait, sizeof(wait), "%s", w->state == T_BLOCKED ? "blocked" : w->state == T_READY ? "ready" : "running");
    int na = storage_stall_describe(s, now, __atomic_load_n(&writer_stage, __ATOMIC_ACQUIRE), now - beat,
                                    __atomic_load_n(&writer_passes, __ATOMIC_RELAXED), wait, 0, a, sizeof(a));
    int nb = storage_stall_describe(s, now, 0, now - beat, 0, 0, 1, b, sizeof(b));
    irq_restore(f);
    if (na) klog("%s", a);
    if (nb) klog("%s", b);
    /* The records above are in the black box's RAM ring; its sectors are
     * written here, not by the stalled writer (bounded: 2 s, 512 sectors). */
    if (blackbox_checkpoint) (void)blackbox_checkpoint();
    return 1;
}
#endif
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
    if (s==&system_storage && bootstat_reset) bootstat_reset();
    if (!s || !s->ready) return;
    if (pagefile_on(s,0) && !pager_attach && pagefile_close) { pagefile_close(&system_pagefile); memset(&system_pagefile,0,sizeof(system_pagefile)); }
    for (unsigned i=0; i<STORAGE_DISKS; i++) if (s->optical[i].mounted) fs_ro_unmount(&s->optical[i]);
    for (unsigned i=0; i<STORAGE_NTFS; i++) if (s->ntfs[i].ro.mounted) fs_ro_unmount(&s->ntfs[i].ro);
    for (unsigned d=0; d<26; d++) fat_census_end(&s->volumes[d].fat);
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

/* G29d at shutdown: the starts still queued for CIUKIST.LOG are written
 * (best effort, advisory). It never waits for a flush in progress: that
 * flush holds the log open, or is about to open it, and may be stalled in a
 * C: commit. The shutdown is then refused with EBUSY before any change, as
 * for the boot log, and the desktop retries (250 ms, up to 5 s). */
int storage_startup_log_shutdown(struct storage *s)
{
    if (!s || !s->ready || s->stopped) return 0;
    int e=vfs_startup_flush(&s->vfs,false);
    return e==-FS_EBUSY ? e : 0;
}
int storage_sync(void)
{
    static const char marker[]="[storage] shutdown requested\n";
    if (blackbox_capture) blackbox_capture(marker,sizeof(marker)-1);
    int e=blackbox_checkpoint ? blackbox_checkpoint() : 0;
    if (e) { klog("[storage] shutdown refused stage=blackbox result=%d",e); return e; }
    e=storage_startup_log_shutdown(&system_storage);
    if (e) { klog("[storage] shutdown refused stage=startup-log result=%d",e); return e; }
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
    uint32_t last_raw=0, window=(uint32_t)fs_now_ms(), passes=0;
    uint32_t kicks=__atomic_load_n(&s->cache.writer_kicks,__ATOMIC_RELAXED);
    storage_writer_progress(s,"start");
    while (!s->stopped) {
        storage_writer_progress(s,"idle");
        cache_writer_wait(&s->cache,10);
        uint64_t now=fs_now_ms();
        storage_writer_progress(s,"window");
        /* fx-23: the writer polls every 10 ms (about 100 passes a second);
         * a window far above that means something keeps kicking it. */
        passes++;
        if ((uint32_t)now-window>=2000) {
            uint32_t k=__atomic_load_n(&s->cache.writer_kicks,__ATOMIC_RELAXED);
            if (passes>600) klog("[storage] writer_busy passes=%u kicks=%u window_ms=%u",
                                 passes,k-kicks,(uint32_t)now-window);
            (void)storage_hold_watch(s,(uint32_t)now);
            window=(uint32_t)now; passes=0; kicks=k;
        }
        bool log_due=bootlog_due((uint32_t)now);
        bool due = cache_writeback_due(&s->cache, now) || log_due || (uint32_t)(now-last_raw)>=1000;
        if (due && !s->stopped) {
            /* Disk waits/writeback run on this P_DEVICE task, never on the
             * shared bounded work queue. fs_service/ATA yield within work. */
            storage_writer_progress(s,"blackbox");
            if (blackbox_flush) blackbox_flush();
            last_raw=(uint32_t)now;
            storage_writer_progress(s,"bootstat");
            if (bootstat_service) (void)bootstat_service();
            storage_writer_progress(s,"bootlog");
            if (log_due) bootlog_drain();
        }
        /* The writer_running reservation covers resize/prefetch too, so
         * shutdown quiesces every cache user before destroying its pages. */
        storage_writer_progress(s,"writeback");
        if (!s->stopped) storage_writeback(s,fs_now_ms());
        storage_writer_progress(s,"census");
        if (!s->stopped) (void)storage_census_service(s,(uint32_t)fs_now_ms());
    }
    storage_writer_progress(s,"stop");
    task_exit(0);
}
/* G29d: program starts reach CIUKIST.LOG on this task, never inside exec
 * (Windows 98's Task Monitor records application starts asynchronously).
 * A start is written about a second after it was queued, so a burst of
 * starts costs one log write and commit; a queue 3/4 full is written at
 * once. A failed flush drops its starts (no retry loop); an error is logged
 * when it first appears, not again until a flush succeeds. */
#define STARTUP_LOG_DELAY_MS 1000u
static void startup_log_main(void *arg)
{
    struct storage *s = arg;
    int reported = 0;
    while (!s->stopped) {
        if (!vfs_startup_wait(&s->vfs) || s->stopped) continue;
        vfs_startup_settle(&s->vfs, STARTUP_LOG_DELAY_MS);
        if (s->stopped) break;
        int e = vfs_startup_flush(&s->vfs, true);
        if (e < 0 && e != reported) klog("[storage] startup_log error=%d dropped=%u failed=%u", e,
                                         s->vfs.startup_dropped, s->vfs.startup_failed);
        reported = e < 0 ? e : 0;
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
    struct task *timer = 0, *starts = 0;
    if (policy.writer) {
        timer = task_create_kernel("storage-writer", storage_timer, s, P_DEVICE);
        if (!timer) s->writer_error = -FS_ENOMEM;
        else s->cache.writer_online=true;
        starts = timer ? task_create_kernel("startup-log", startup_log_main, s, P_NORMAL) : 0;
        if (timer && !starts) klog("[storage] startup_log unavailable error=%d", -FS_ENOMEM);
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
    if (system_settings_load) system_settings_load(&s->vfs);
    if (policy.pagefile && (!system_performance || system_performance()->vm != 2)) {
        /* M4: the pager manager opens and grows the file asynchronously;
         * without it commit stays RAM-only and the system remains usable. */
        e = storage_pagefile_bind(s, 2, "C:/SYSTEM/PAGEFILE.SWP");
        klog("[storage] pagefile drive=C result=%d", e);
    }
    if (policy.writer) {
        if (timer) task_start(timer);
        else klog("[storage] writeback unavailable; volumes read-only error=%d", s->writer_error);
        if (starts) task_start(starts);
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

/* G29b: per-file exclusive maintenance; requests never borrow userspace
 * cluster addresses. The sequence joins successive reservations and detects
 * intervening publishers. Reads and writes outside the reservation wait. */
int storage_defrag(struct storage *s, struct ciuki_media_defrag *r, bool step)
{
    if (!r || r->version!=CIUKI_MEDIA_VERSION || r->size!=sizeof(*r) ||
        r->drive>=26 || (r->flags&~(CIUKI_DEFRAG_CHECK|CIUKI_DEFRAG_STARTUP))) return -FS_EINVAL;
    if (!s || !s->ready || s->stopped) return -FS_ENOENT;
    if (s->setup_mode) return -FS_EBUSY;
    unsigned d=r->drive;
    /* G29d: the analysis of "rearrange program files" sees every start so
     * far, also those still queued for the start-log task. A flush already
     * in progress holds the log open (and may be stalled in a commit): the
     * analysis is refused with EBUSY instead of waiting for it. */
    if (!step && (r->flags&CIUKI_DEFRAG_STARTUP) && vfs_startup_flush(&s->vfs,false)==-FS_EBUSY) return -FS_EBUSY;
    vfs_lock(&s->vfs,0); vfs_reserve_locked(&s->vfs,vfs_drive_mask(d));
    struct storage_volume *v=&s->volumes[d];
    struct blkdev *dev=v->part.parent;
    int e=!v->present || s->vfs.volumes[d]!=&v->fat || !v->fat.mounted ? -FS_ENOENT :
        !dev || dev->quarantined || ((step || r->generation) && dev->media_generation!=r->generation) ? -FS_EIO : 0;
    if (!e && step) {
        if (pagefile_on(s,&v->fat) || vfs_identity_busy(&s->vfs,&v->fat)) e=-FS_EBUSY;
        for (unsigned i=0;!e && i<VFS_NODES;i++)
            if (s->vfs.nodes[i].refs && s->vfs.nodes[i].volume==&v->fat) e=-FS_EBUSY;
        if (!e && px_volume_busy && px_volume_busy(&s->vfs,&v->fat)) e=-FS_EBUSY;
        for (unsigned i=0;!e && i<VFS_TABLES;i++)
            if (s->vfs.tables[i] && s->vfs.tables[i]->cwd_cluster[d]) e=-FS_EBUSY;
        if (!e && v->fat.readonly) e=-FS_EROFS;
        bool moved=false;
        if (!e) {
            if (r->flags&CIUKI_DEFRAG_STARTUP) {
                uint32_t previous=r->placement;
                e=VFS_IO(&s->vfs,fat_defrag_startup_step(&v->fat,r->ordinal,r->sequence,&r->placement,&moved));
                if (!e && moved && previous!=r->placement) r->profiled++;
            } else e=VFS_IO(&s->vfs,fat_defrag_step(&v->fat,r->ordinal,r->sequence,&moved));
            r->finished=e==1; if (e==1) e=0;
            r->moved=moved;
            if (!e && !r->finished) r->ordinal++;
        }
    } else if (!e) {
        struct fat_defrag_info info;
        if (r->flags&CIUKI_DEFRAG_CHECK) e=VFS_IO(&s->vfs,fat_scan(&v->fat));
        if (!e && (v->fat.ro_reasons&(FAT_RO_CORRUPT|FAT_RO_COPIES|FAT_RO_LOST))) e=-FS_EIO;
        if (!e) e=VFS_IO(&s->vfs,fat_defrag_analyze(&v->fat,&info));
        if (!e) {
            r->files=info.files; r->fragmented=info.fragmented;
            r->clusters=info.clusters; r->free_clusters=info.free_clusters;
            memcpy(r->map,info.map,sizeof(r->map)); r->ordinal=0; r->moved=0; r->finished=0; r->placement=2; r->profiled=0;
        }
    }
    if (!e) { r->sequence=v->fat.mutation_sequence; r->generation=dev->media_generation; }
    vfs_unlock(&s->vfs);
    return e;
}

int storage_convert(struct storage *s, struct ciuki_media_convert *r, bool execute)
{
    if (!r || r->version!=CIUKI_MEDIA_VERSION || r->size!=sizeof(*r) ||
        r->drive>=26 || (r->reserved&~CIUKI_CONVERT_NEXT_BOOT)) return -FS_EINVAL;
    if (!s || !s->ready || s->stopped) return -FS_ENOENT;
    unsigned d=r->drive;
    if (s->setup_mode) return -FS_EROFS;
    if (d==2 && execute && !(r->reserved&CIUKI_CONVERT_NEXT_BOOT)) return -FS_EROFS;
    if (d!=2 && r->reserved) return -FS_EINVAL;
    vfs_lock(&s->vfs,0); vfs_reserve_locked(&s->vfs,vfs_drive_mask(d));
    struct storage_volume *v=&s->volumes[d]; struct blkdev *dev=v->part.parent;
    int e=!v->present || !v->fat.mounted || s->vfs.volumes[d]!=&v->fat ? -FS_ENOENT :
        v->removable || !v->partition ? -FS_EROFS :
        !dev || dev->quarantined || ((execute || r->generation) && dev->media_generation!=r->generation) ? -FS_EIO : 0;
    if (!e && (!dev->write || !blkdev_durable(dev))) e=-FS_EROFS;
    if (!e && v->disk<STORAGE_DISKS && s->raw[v->disk].lease.holder) e=-FS_EBUSY;
    uint32_t bytes=0;
    if (!e) e=fat_convert_plan(&v->fat,&bytes);
    if (!e && d==2) {
        if (v->fat.spc>64) e=-FS_EOPNOTSUPP;
        if (!e) e=VFS_IO(&s->vfs,fat_convert_boot_loader(dev,v->part.start,v->part.block.capacity));
        if (!e && execute) {
            e=VFS_IO(&s->vfs,fat_convert_boot_schedule(&v->fat,dev,v->part.start));
            cache_invalidate(&s->cache,&v->io,true);
        }
        if (!e) { r->reserved=CIUKI_CONVERT_NEXT_BOOT; r->generation=dev->media_generation;
            r->filesystem=16; r->cluster_bytes=bytes; r->sectors=(uint32_t)v->part.block.capacity; }
        vfs_unlock(&s->vfs); return e;
    }
    if (!e && execute) {
        for (unsigned i=0;!e && i<26;i++)
            if (s->volumes[i].part.parent==dev && s->volumes[i].formatting) e=-FS_EBUSY;
        if (pagefile_on(s,&v->fat) || vfs_identity_busy(&s->vfs,&v->fat)) e=-FS_EBUSY;
        for (unsigned i=0;!e && i<VFS_TABLES;i++)
            if (s->vfs.tables[i] && s->vfs.tables[i]->cwd_cluster[d]) e=-FS_EBUSY;
        if (!e) { v->formatting=true; e=vfs_detach_locked(&s->vfs,d); } /* refuses open native/legacy files */
        if (!e) {
            bool was_writable=!v->fat.readonly;
            cache_invalidate(&s->cache,&v->io,true); v->formatting=true;
            e=VFS_IO(&s->vfs,fat_prepare(&v->fat,&s->cache,&v->io,0,v->io.capacity));
            if (!e) {
                v->fat.preparing=false; /* old read-only FAT may use the shared read cache */
                e=VFS_IO(&s->vfs,fat_convert(&v->fat,dev,v->part.start));
            }
            cache_invalidate(&s->cache,&v->io,true);
            if (!e) {
                v->fat.mounted=false;
                e=VFS_IO(&s->vfs,fat_prepare(&v->fat,&s->cache,&v->io,0,v->io.capacity));
                if (!e && was_writable) e=VFS_IO(&s->vfs,fat_enable_write(&v->fat));
                v->fat.preparing=false;
                if (!e) e=vfs_attach_locked(&s->vfs,d,&v->fat);
            }
            /* Failure deliberately stays detached. A locator-bearing volume
             * must be replayed on reboot before any filesystem access. */
            if (e) { v->present=false; v->fat.mounted=false; v->read_gate=false; v->error=e; }
        }
        v->formatting=false;
    }
    if (!e) { r->generation=dev->media_generation; r->filesystem=execute ? 32 : 16; r->cluster_bytes=bytes; r->sectors=(uint32_t)v->part.block.capacity; }
    vfs_unlock(&s->vfs); return e;
}
