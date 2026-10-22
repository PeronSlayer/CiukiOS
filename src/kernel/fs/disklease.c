/* f9-02 raw whole-disk access and the destination-disk lease, the kernel
 * half of docs/design/cd-setup-plan.md 5.3 (lead decisions D5, D7). The
 * device nodes /dev/ata0..3 (drivers/disk_dev.c) only copy records: every
 * decision is here, so the host suite runs it against file-backed disks.
 *
 * Authority: reads, DISK_GET_INFO and DISK_LEASE_STATE are open. Every
 * mutating request (write, flush, acquire, release, publish, phase, recheck)
 * needs the CALLER's Setup capability (EPERM) and, except the acquire, the
 * lease held by that caller (EACCES), checked on each call, never at open.
 * State lives in struct storage under the VFS namespace lock; device I/O
 * runs without it. Errors are negative ABI errno values; no panic paths.
 *
 * Durability: a flush is blkdev.flush of the disk (ATA FLUSH CACHE / FLUSH
 * CACHE EXT, ATA/ATAPI-6 d1410r3a 8.12-8.13: success means every earlier
 * write reached the medium), refused when the device has no qualified path.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/storage.h>
#include <ciuki/kernel.h>
#include <ciuki/vfs_hooks.h>
#ifdef FS_HOST
/* The host suite links the hosted C library, whose <stdlib.h>/<time.h>
 * already define the wait-status and clock macros the ABI header repeats. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmacro-redefined"
#include <ciuki/abi.h>
#pragma clang diagnostic pop
#else
#include <ciuki/abi.h>
#endif
#include <ciuki/bootlog.h>
#include <ciuki/blackbox.h>
#pragma weak bootlog_snapshot
#pragma weak blackbox_available

_Static_assert(CIUKI_FAT_RO_REQUEST == FAT_RO_REQUEST && CIUKI_FAT_RO_DURABILITY == FAT_RO_DURABILITY &&
               CIUKI_FAT_RO_DIRTY == FAT_RO_DIRTY && CIUKI_FAT_RO_IO_FLAG == FAT_RO_IO_FLAG &&
               CIUKI_FAT_RO_COPIES == FAT_RO_COPIES && CIUKI_FAT_RO_CORRUPT == FAT_RO_CORRUPT &&
               CIUKI_FAT_RO_LOST == FAT_RO_LOST && CIUKI_FAT_RO_WRITE_ERROR == FAT_RO_WRITE_ERROR &&
               CIUKI_FAT_RO_ACTIVE_ONLY == FAT_RO_ACTIVE_ONLY, "FAT read-only reasons ABI");
_Static_assert(STORAGE_DISKS == CIUKI_DISK_SLOTS, "raw disk slots");
_Static_assert(STORAGE_LEASE_PHASE == sizeof(((struct ciuki_disk_lease *)0)->phase), "lease phase text");
_Static_assert(STORAGE_LEASE_REASON == sizeof(((struct ciuki_disk_lease *)0)->reason), "lease reason text");
_Static_assert(EPERM == 1 && ENXIO == 6 && EACCES == FS_EACCES && EBUSY == FS_EBUSY && EINVAL == FS_EINVAL &&
               EIO == FS_EIO && ENOMEM == FS_ENOMEM && EROFS == FS_EROFS && ENOSPC == FS_ENOSPC &&
               EOPNOTSUPP == FS_EOPNOTSUPP, "fs errors are ABI errno values");

static int abi_error(int e)
{
    if (e >= 0) return e;
    if (e == -FS_EQUARANTINED || e == -FS_EUCLEAN || e == -FS_ELOOP || e == -VFS_ETIMEOUT || e == -VFS_ECANCELLED) return -EIO;
    return e;
}
static bool leased(const struct storage_lease *l)
{
    return l->state == CIUKI_DISK_LEASE_HELD || l->state == CIUKI_DISK_LEASE_PUBLISHED;
}
static void text(char *out, unsigned bytes, const char *in)
{
    unsigned i = 0;
    for (; in && in[i] && i + 1 < bytes; i++) out[i] = in[i] >= 32 && in[i] <= 126 ? in[i] : '_';
    for (; i < bytes; i++) out[i] = 0;
}
static void record(unsigned disk, uint32_t pid, const char *state, const char *reason)
{
    klog("[disk] lease disk=%u pid=%u state=%s reason=%s", disk, pid, state, reason);
}
static int authorize(struct storage *s, unsigned disk, const struct storage_caller *c)
{
    if (!c || !c->setup_capability) return -EPERM;
    const struct storage_lease *l = &s->raw[disk].lease;
    return leased(l) && l->holder == c->pid ? 0 : -EACCES;
}
static bool on_disk(const struct storage *s, unsigned disk, const struct storage_volume *v)
{
    return v->present && v->disk == disk && v->part.parent == s->raw[disk].dev;
}
static struct storage_volume *published(struct storage *s, unsigned disk)
{
    const struct storage_lease *l = &s->raw[disk].lease;
    if (l->drive < 3 || l->drive >= 26) return 0;
    struct storage_volume *v = &s->volumes[l->drive];
    return on_disk(s, disk, v) && s->vfs.volumes[l->drive] == &v->fat ? v : 0;
}
static unsigned fs_kind(const struct fat_volume *fat) { return fat->type; }

int storage_disk_open(struct storage *s, unsigned disk)
{
    if (!s || disk >= STORAGE_DISKS || !s->ready) return -ENXIO;
    vfs_lock(&s->vfs, 0);
    struct blkdev *dev = s->raw[disk].dev;
    vfs_unlock(&s->vfs);
    return dev ? 0 : -ENXIO;
}

static void info_locked(struct storage *s, unsigned disk, struct ciuki_disk_info *out)
{
    struct storage_disk *r = &s->raw[disk];
    struct blkdev *dev = r->dev;
    memset(out, 0, sizeof(*out));
    out->version = CIUKI_DISK_VERSION; out->size = sizeof(*out); out->disk = disk;
    out->lba_limit = CIUKI_DISK_LBA28;
    if (!dev) return;
    out->flags = CIUKI_DISK_PRESENT | (r->lba48 ? CIUKI_DISK_LBA48 : 0) | (dev->flush ? CIUKI_DISK_FLUSH_CACHE : 0) |
        (leased(&r->lease) ? CIUKI_DISK_LEASED : 0) | (r->lease.state == CIUKI_DISK_LEASE_PUBLISHED ? CIUKI_DISK_PUBLISHED : 0);
    out->sectors = dev->capacity; out->sector_size = dev->sector_size;
    out->write_cache = dev->write_cache_state == BLKDEV_CACHE_DISABLED ? CIUKI_DISK_CACHE_DISABLED :
                       dev->write_cache_state == BLKDEV_CACHE_ENABLED ? CIUKI_DISK_CACHE_ENABLED : CIUKI_DISK_CACHE_UNKNOWN;
    memcpy(out->model, r->model, sizeof(out->model)); out->model[40] = 0;
    memcpy(out->serial, r->serial, sizeof(out->serial)); out->serial[20] = 0;
    memcpy(out->firmware, r->firmware, sizeof(out->firmware)); out->firmware[8] = 0;
    for (unsigned d = 2; d < 26 && out->volume_count < CIUKI_DISK_VOLUMES_MAX; d++) {
        struct storage_volume *v = &s->volumes[d];
        if (!on_disk(s, disk, v) || s->vfs.volumes[d] != &v->fat) continue;
        struct ciuki_disk_volume *o = &out->volumes[out->volume_count++];
        o->drive = d; o->partition = v->partition; o->fs = fs_kind(&v->fat);
        o->flags = v->fat.readonly ? CIUKI_DISK_VOLUME_READONLY : 0;
        memcpy(o->label, v->fat.volume_label, sizeof(o->label)); o->label[11] = 0;
    }
    for (unsigned i = 0; i < STORAGE_NTFS && out->volume_count < CIUKI_DISK_VOLUMES_MAX; i++) {
        struct storage_ntfs *n = &s->ntfs[i];
        if (!n->present || n->disk != disk || !n->ro.mounted || n->drive < 3 || n->drive >= 26 ||
            s->vfs.ro_volumes[n->drive] != &n->ro) continue;
        struct ciuki_disk_volume *o = &out->volumes[out->volume_count++];
        o->drive = n->drive; o->partition = n->partition; o->fs = CIUKI_DISK_FS_NTFS;
        o->flags = CIUKI_DISK_VOLUME_READONLY;
    }
}
static void lease_locked(struct storage *s, unsigned disk, struct ciuki_disk_lease *out)
{
    const struct storage_lease *l = &s->raw[disk].lease;
    memset(out, 0, sizeof(*out));
    out->version = CIUKI_DISK_VERSION; out->size = sizeof(*out); out->disk = disk;
    out->state = l->state; out->holder = l->holder; out->generation = l->generation;
    out->drive = l->drive; out->reasons = l->reasons;
    out->reads = l->reads; out->writes = l->writes; out->flushes = l->flushes;
    memcpy(out->phase, l->phase, sizeof(out->phase));
    memcpy(out->reason, l->reason, sizeof(out->reason));
}

/* Plan 5.3: refuse while any volume of the disk is writable, open, dirty,
 * backs the pagefile or a disk log, or while the disk is leased. Returns
 * the reason text, NULL when the disk may be leased. */
static const char *busy_locked(struct storage *s, unsigned disk)
{
    struct storage_disk *r = &s->raw[disk];
    if (leased(&r->lease)) return "leased";
    if (r->lease.io_active) return "io";
    if (disk == 0 && s == storage_get() && blackbox_available && blackbox_available()) return "disk_log";
    /* The most specific dependency is reported; any of them is EBUSY. */
    for (unsigned d = 2; d < 26; d++) {
        struct storage_volume *v = &s->volumes[d];
        if (!on_disk(s, disk, v)) continue;
        if (storage_volume_pagefile(s, &v->fat)) return "pagefile";
        if (d == 2 && s == storage_get() && bootlog_snapshot) {
            struct bootlog_stats log; bootlog_snapshot(&log);
            if (log.active) return "disk_log";
        }
        for (unsigned i = 0; i < VFS_NODES; i++)
            if (s->vfs.nodes[i].refs && s->vfs.nodes[i].volume == &v->fat) return "open";
        if (px_volume_busy && px_volume_busy(&s->vfs, &v->fat)) return "open";
        if (vfs_identity_busy(&s->vfs, &v->fat)) return "open";
        for (unsigned i = 0; i < VFS_TABLES; i++)
            if (s->vfs.tables[i] && s->vfs.tables[i]->cwd_cluster[d]) return "open";
        if (cache_dirty_bytes(&s->cache, &v->io)) return "dirty";
        if (v->fat.mounted && !v->fat.readonly) return "writable";
    }
    for (unsigned i = 0; i < STORAGE_NTFS; i++) {
        struct storage_ntfs *n = &s->ntfs[i];
        if (!n->present || n->disk != disk || !n->ro.mounted) continue;
        for (unsigned j = 0; j < VFS_NODES; j++)
            if (s->vfs.nodes[j].refs && s->vfs.nodes[j].ro == &n->ro) return "open";
        if (px_ro_busy && px_ro_busy(&s->vfs, &n->ro)) return "open";
    }
    return 0;
}
static uint32_t disk_drives(struct storage *s, unsigned disk)
{
    uint32_t mask = 0;
    for (unsigned d = 2; d < 26; d++) if (on_disk(s, disk, &s->volumes[d])) mask |= vfs_drive_mask(d);
    for (unsigned i = 0; i < STORAGE_NTFS; i++)
        if (s->ntfs[i].present && s->ntfs[i].disk == disk && s->ntfs[i].drive < 26) mask |= vfs_drive_mask(s->ntfs[i].drive);
    return mask;
}
/* Every volume of the disk was read-only and idle (busy_locked): unmounting
 * issues no media write. Their letters are free until DISK_PUBLISH. */
static int detach_locked(struct storage *s, unsigned disk)
{
    for (unsigned d = 2; d < 26; d++) {
        struct storage_volume *v = &s->volumes[d];
        if (!on_disk(s, disk, v)) continue;
        if (s->vfs.volumes[d] == &v->fat) { int e = vfs_detach_locked(&s->vfs, d); if (e) return e; }
        cache_invalidate(&s->cache, &v->io, true);
        s->vfs.drive_flags[d] = 0;
        v->fat.mounted = false; v->present = false;
    }
    for (unsigned i = 0; i < STORAGE_NTFS; i++) {
        struct storage_ntfs *n = &s->ntfs[i];
        if (!n->present || n->disk != disk) continue;
        bool attached = false;
        for (unsigned d = 3; d < 26; d++) if (s->vfs.ro_volumes[d] == &n->ro) {
            int e = vfs_detach_locked(&s->vfs, d); if (e) return e;
            attached = true;
        }
        if (!attached && n->ro.mounted) VFS_IO_VOID(&s->vfs, fs_ro_unmount(&n->ro));
        cache_invalidate(&s->cache, &n->io, true);
        memset(n, 0, sizeof(*n));
    }
    cache_invalidate(&s->cache, s->raw[disk].dev, true);
    return 0;
}

static int acquire_locked(struct storage *s, unsigned disk, const struct storage_caller *c, struct ciuki_disk_info *out)
{
    struct storage_disk *r = &s->raw[disk];
    if (!c || !c->setup_capability) return -EPERM;
    uint32_t mask = disk_drives(s, disk);
    if (mask) { vfs_reserve_locked(&s->vfs, mask); vfs_tag_locked(&s->vfs, "lease"); }
    const char *busy = busy_locked(s, disk);
    if (busy) {
        text(r->lease.reason, sizeof(r->lease.reason), busy);
        record(disk, c->pid, "refused", busy);
        return -EBUSY;
    }
    int e = detach_locked(s, disk);
    if (e) {
        text(r->lease.reason, sizeof(r->lease.reason), "detach");
        record(disk, c->pid, "refused", "detach");
        return abi_error(e);
    }
    r->lease.state = CIUKI_DISK_LEASE_HELD; r->lease.holder = c->pid; r->lease.generation++;
    r->lease.drive = 0; r->lease.reasons = 0;
    text(r->lease.phase, sizeof(r->lease.phase), "");
    text(r->lease.reason, sizeof(r->lease.reason), "acquire");
    record(disk, c->pid, "held", "acquire");
    info_locked(s, disk, out);
    return 0;
}
static int commit_published(struct storage *s, unsigned disk)
{
    struct storage_volume *v = published(s, disk);
    if (!v || v->fat.readonly) return 0;
    vfs_reserve_locked(&s->vfs, vfs_drive_mask(v->drive)); vfs_tag_locked(&s->vfs, "commit");
    return VFS_IO(&s->vfs, fat_commit(&v->fat));
}
static int release_locked(struct storage *s, unsigned disk, const struct storage_caller *c)
{
    struct storage_disk *r = &s->raw[disk];
    int e = authorize(s, disk, c); if (e) return e;
    if (r->lease.io_active) return -EBUSY;
    /* The published volume stays mounted; nobody may write it afterwards
     * (storage_disk_write_denied). Its accepted writes become durable now. */
    e = commit_published(s, disk);
    r->lease.state = CIUKI_DISK_LEASE_RELEASED;
    text(r->lease.reason, sizeof(r->lease.reason), e ? "release_commit_error" : "release");
    record(disk, c->pid, "released", e ? "release_commit_error" : "release");
    return abi_error(e);
}
/* Partition 1 of the table read by the caller after the raw phases; the
 * lease and the drive letter are revalidated under the namespace lock. */
static int publish_locked(struct storage *s, unsigned disk, const struct storage_caller *c,
                          const struct partition_table *table, int scanned, struct ciuki_disk_lease *out)
{
    struct storage_disk *r = &s->raw[disk];
    int e = authorize(s, disk, c); if (e) return e;
    if (r->lease.state != CIUKI_DISK_LEASE_HELD || r->lease.io_active) return -EBUSY;
    const char *why = scanned ? "table" : 0;
    if (!why && (!table->count || table->entries[0].logical || table->entries[0].start != 2048 ||
        (table->entries[0].type != 0x0b && table->entries[0].type != 0x0c))) why = "partition1";
    unsigned d = 3;
    while (!why && d < 26 && (vfs_drive_present(&s->vfs, d) || s->volumes[d].present)) d++;
    if (!why && d >= 26) why = "letters";
    uint32_t reasons = 0;
    e = scanned;
    if (!why) {
        vfs_reserve_locked(&s->vfs, vfs_drive_mask(d)); vfs_tag_locked(&s->vfs, "mount");
        if (vfs_drive_present(&s->vfs, d) || s->volumes[d].present) why = "letters";
        else if ((e = storage_publish_locked(s, disk, d, &table->entries[0], &reasons))) why = "mount";
    }
    if (why) {
        text(r->lease.reason, sizeof(r->lease.reason), why);
        klog("[disk] publish disk=%u pid=%u result=%d reason=%s", disk, c->pid, e ? e : -EINVAL, why);
        return e ? abi_error(e) : -EINVAL;
    }
    r->lease.state = CIUKI_DISK_LEASE_PUBLISHED; r->lease.drive = d; r->lease.reasons = reasons;
    text(r->lease.reason, sizeof(r->lease.reason), "publish");
    klog("[disk] publish disk=%u pid=%u drive=%c reasons=0x%x clusters=%u", disk, c->pid, 'A' + d,
         reasons, s->volumes[d].fat.clusters);
    lease_locked(s, disk, out);
    return 0;
}
/* DISK_PUBLISH: forget every cached view of the disk and re-read its table
 * from the medium without the namespace lock, then mount under it. */
static int publish(struct storage *s, unsigned disk, const struct storage_caller *c, struct ciuki_disk_lease *out)
{
    struct partition_table *table = fs_alloc(sizeof(*table));
    if (!table) return -ENOMEM;
    vfs_lock(&s->vfs, 0);
    struct storage_disk *r = &s->raw[disk];
    int e = !r->dev ? -ENXIO : authorize(s, disk, c);
    if (!e && (r->lease.state != CIUKI_DISK_LEASE_HELD || r->lease.io_active)) e = -EBUSY;
    if (!e) { r->lease.io_active++; cache_invalidate(&s->cache, r->dev, true); }
    vfs_unlock(&s->vfs);
    if (e) { fs_free(table); return e; }
    int scanned = partition_scan(r->dev, table);
    vfs_lock(&s->vfs, 0);
    r->lease.io_active--;
    e = publish_locked(s, disk, c, table, scanned, out);
    vfs_unlock(&s->vfs);
    fs_free(table);
    /* Setup mode starts storage without the writer task (f9-01, D2); the
     * published volume needs it. A failure leaves writes to fsync barriers. */
    if (!e) {
        int w = storage_writer_start(s);
        if (w) klog("[disk] publish disk=%u writer=unavailable error=%d", disk, w);
    }
    return e;
}
/* The kernel's own consistency check (plan 6.3): commit, clean unmount
 * (FAT[1] clean bit, copy 1 -> barrier -> copy 2, f1-26), forget the cache,
 * read-only remount and the explicit full ownership scan. SEAL leaves the
 * volume clean and read-only; otherwise a new write session reopens it. */
static int recheck_locked(struct storage *s, unsigned disk, const struct storage_caller *c, struct ciuki_disk_recheck *out)
{
    struct storage_disk *r = &s->raw[disk];
    int e = authorize(s, disk, c); if (e) return e;
    struct storage_volume *v = published(s, disk);
    if (r->lease.state != CIUKI_DISK_LEASE_PUBLISHED || !v) return -EINVAL;
    if (r->lease.io_active) return -EBUSY;
    unsigned d = v->drive;
    bool seal = out->flags & CIUKI_DISK_RECHECK_SEAL;
    vfs_reserve_locked(&s->vfs, vfs_drive_mask(d)); vfs_tag_locked(&s->vfs, "commit");
    bool sealed = v->fat.readonly && !v->fat.writable_session;
    e = vfs_detach_locked(&s->vfs, d);
    if (e) return abi_error(e);
    cache_invalidate(&s->cache, &v->io, true);
    e = VFS_IO(&s->vfs, fat_mount(&v->fat, &s->cache, &v->io, 0, v->io.capacity, false));
    if (!e) {
        /* EUCLEAN: the scan completed and found corruption (reasons set),
         * exactly as mount_volume accepts it for a read-only mount. */
        int scan = VFS_IO(&s->vfs, fat_scan(&v->fat));
        if (scan && scan != -FS_EUCLEAN) e = scan;
    }
    if (!e) {
        out->drive = d; out->reasons = v->fat.ro_reasons & ~(uint32_t)FAT_RO_REQUEST;
        out->fat_type = v->fat.type; out->clusters = v->fat.clusters; out->free_clusters = v->fat.free_clusters;
        out->lost_clusters = v->fat.lost_clusters; out->scan_passes = v->fat.scan_passes; out->scan_ms = v->fat.scan_ms;
        out->lfn_orphans = v->fat.lfn_orphans; out->lfn_invalid = v->fat.lfn_invalid;
        out->lfn_bad_checksum = v->fat.lfn_bad_checksum;
    }
    /* Reopen for the holder only when the read gate allows it; a corrupt
     * volume stays published read-only and the caller sees its reasons. */
    bool reopen = !e && !seal && !sealed &&
        !(v->fat.ro_reasons & ~(FAT_RO_REQUEST | FAT_RO_DURABILITY | FAT_RO_DIRTY | FAT_RO_LOST | FAT_RO_IO_FLAG));
    if (reopen) e = VFS_IO(&s->vfs, fat_enable_write(&v->fat));
    if (!e) e = vfs_attach_locked(&s->vfs, d, &v->fat);
    if (e) {
        /* Nothing stays published after a failed remount; the lease holds. */
        v->fat.mounted = false; v->present = false;
        cache_invalidate(&s->cache, &v->io, true);
        r->lease.state = CIUKI_DISK_LEASE_HELD; r->lease.drive = 0;
        text(r->lease.reason, sizeof(r->lease.reason), "recheck_mount");
        klog("[disk] recheck disk=%u drive=%c result=%d reason=recheck_mount", disk, 'A' + d, e);
        return abi_error(e);
    }
    klog("[disk] recheck disk=%u drive=%c seal=%u reasons=0x%x lost=%u passes=%u", disk, 'A' + d,
         seal || sealed, out->reasons, out->lost_clusters, out->scan_passes);
    return 0;
}

int storage_disk_control(struct storage *s, unsigned disk, const struct storage_caller *c,
                         uint32_t op, void *record_in, uint32_t size)
{
    if (!s || !s->ready || disk >= STORAGE_DISKS) return -ENXIO;
    if (op < DISK_GET_INFO || op > DISK_RECHECK) return -ENOTTY;
    if (!record_in) return -EINVAL;
    uint32_t want = op == DISK_GET_INFO || op == DISK_LEASE_ACQUIRE ? sizeof(struct ciuki_disk_info) :
        op == DISK_LEASE_RELEASE || op == DISK_FLUSH ? sizeof(struct ciuki_disk_request) :
        op == DISK_RECHECK ? sizeof(struct ciuki_disk_recheck) : sizeof(struct ciuki_disk_lease);
    const uint32_t *head = record_in;
    if (size != want || head[1] != want) return -EINVAL;
    if (head[0] != CIUKI_DISK_VERSION) return -EOPNOTSUPP;
    if (op == DISK_FLUSH) {
        const struct ciuki_disk_request *q = record_in;
        if (q->flags || q->reserved) return -EINVAL;
        return storage_disk_flush(s, disk, c);
    }
    if (op == DISK_PUBLISH) return publish(s, disk, c, record_in);
    vfs_lock(&s->vfs, 0);
    struct storage_disk *r = &s->raw[disk];
    int e = r->dev ? 0 : -ENXIO;
    if (!e) switch (op) {
    case DISK_GET_INFO: info_locked(s, disk, record_in); break;
    case DISK_LEASE_STATE: lease_locked(s, disk, record_in); break;
    case DISK_LEASE_ACQUIRE: e = acquire_locked(s, disk, c, record_in); break;
    case DISK_LEASE_RELEASE: {
        const struct ciuki_disk_request *q = record_in;
        e = q->flags || q->reserved ? -EINVAL : release_locked(s, disk, c);
        break;
    }

    case DISK_SET_PHASE: {
        struct ciuki_disk_lease *l = record_in;
        e = authorize(s, disk, c);
        if (!e) { l->phase[sizeof(l->phase) - 1] = 0; text(r->lease.phase, sizeof(r->lease.phase), l->phase); }
        break;
    }
    case DISK_RECHECK: {
        struct ciuki_disk_recheck *q = record_in;
        e = q->flags & ~CIUKI_DISK_RECHECK_SEAL || q->drive || q->reserved[0] || q->reserved[1] ? -EINVAL :
            recheck_locked(s, disk, c, q);
        break;
    }
    default: e = -ENOTTY; break;
    }
    vfs_unlock(&s->vfs);
    return e;
}

int storage_disk_io(struct storage *s, unsigned disk, const struct storage_caller *c, bool write,
                    uint64_t offset, void *buffer, uint32_t bytes)
{
    if (!s || !s->ready || disk >= STORAGE_DISKS) return -ENXIO;
    if (!buffer || !bytes || bytes > CIUKI_DISK_IO_MAX || bytes % CIUKI_DISK_SECTOR || offset % CIUKI_DISK_SECTOR)
        return -EINVAL;
    uint64_t lba = offset / CIUKI_DISK_SECTOR; uint32_t count = bytes / CIUKI_DISK_SECTOR;
    vfs_lock(&s->vfs, 0);
    struct storage_disk *r = &s->raw[disk];
    struct blkdev *dev = r->dev;
    int e = !dev ? -ENXIO : dev->sector_size != CIUKI_DISK_SECTOR ? -EINVAL : abi_error(blkdev_range(dev, lba, count));
    if (!e && write) e = authorize(s, disk, c);
    if (!e && write) {
        /* The published volume owns its sectors and its cache. */
        struct storage_volume *v = published(s, disk);
        if (v && lba < v->part.start + v->io.capacity && v->part.start < lba + count) e = -EBUSY;
        else if (!blkdev_durable(dev)) e = -EROFS;
    }
    if (!e) r->lease.io_active++;
    vfs_unlock(&s->vfs);
    if (e) return e;
    e = write ? dev->write(dev, lba, count, buffer) : dev->read(dev, lba, count, buffer);
    vfs_lock(&s->vfs, 0);
    r->lease.io_active--;
    if (!e) { if (write) r->lease.writes += count; else r->lease.reads += count; }
    vfs_unlock(&s->vfs);
    if (e && write) klog("[disk] write disk=%u lba=%llu count=%u result=%d", disk, (unsigned long long)lba, count, e);
    /* D7: every device error is the refusal EIO for libdisk, with its LBA. */
    return e ? -EIO : (int)bytes;
}

int storage_disk_flush(struct storage *s, unsigned disk, const struct storage_caller *c)
{
    if (!s || !s->ready || disk >= STORAGE_DISKS) return -ENXIO;
    vfs_lock(&s->vfs, 0);
    struct storage_disk *r = &s->raw[disk];
    struct blkdev *dev = r->dev;
    int e = dev ? authorize(s, disk, c) : -ENXIO;
    if (!e && !dev->flush) e = -EOPNOTSUPP;
    if (!e) e = abi_error(commit_published(s, disk));
    if (!e) r->lease.io_active++;
    vfs_unlock(&s->vfs);
    if (e) return e;
    e = dev->flush(dev);
    vfs_lock(&s->vfs, 0);
    r->lease.io_active--;
    if (!e) r->lease.flushes++;
    vfs_unlock(&s->vfs);
    if (e) klog("[disk] flush disk=%u result=%d", disk, e);
    return e ? -EIO : 0;
}

void storage_disk_exit(struct storage *s, uint32_t pid)
{
    if (!s || !s->ready || !pid) return;
    vfs_lock(&s->vfs, 0);
    for (unsigned disk = 0; disk < STORAGE_DISKS; disk++) {
        struct storage_lease *l = &s->raw[disk].lease;
        if (!leased(l) || l->holder != pid) continue;
        /* No I/O here: the published volume keeps its accepted writes for
         * the writer task and shutdown; no process may write it again. */
        l->state = CIUKI_DISK_LEASE_REVOKED;
        text(l->reason, sizeof(l->reason), "process_exit");
        record(disk, pid, "revoked", "process_exit");
    }
    vfs_unlock(&s->vfs);
}

bool storage_disk_write_denied(struct storage *s, unsigned drive, uint32_t pid)
{
    if (!s || !s->ready || drive < 3 || drive >= 26) return false;
    for (unsigned disk = 0; disk < STORAGE_DISKS; disk++) {
        const struct storage_lease *l = &s->raw[disk].lease;
        if (l->drive != drive || l->state == CIUKI_DISK_LEASE_NONE) continue;
        if (!on_disk(s, disk, &s->volumes[drive])) continue;
        return !(l->state == CIUKI_DISK_LEASE_PUBLISHED && l->holder == pid);
    }
    return false;
}

uint64_t storage_disk_writes(struct storage *s, unsigned disk)
{
    if (!s || !s->ready || disk >= STORAGE_DISKS) return 0;
    vfs_lock(&s->vfs, 0);
    uint64_t total = s->raw[disk].lease.writes;
    for (unsigned d = 2; d < 26; d++) if (on_disk(s, disk, &s->volumes[d])) total += s->volumes[d].writes;
    vfs_unlock(&s->vfs);
    return total;
}
