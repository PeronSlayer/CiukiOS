/* Read first, then enable writing explicitly. FAT mount/ownership rules:
 * https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf
 * See tests/host/fs/STORAGE-VALIDATION.md for integration decisions.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/storage.h>
#include <ciuki/bootlog.h>
#ifndef FS_HOST
#include <ciuki/ata.h>
#include <ciuki/probe.h>
#include <ciuki/work.h>
#include <ciuki/sync.h>
#endif

static struct storage system_storage;
static int volume_flush(struct blkdev *);

static struct blkdev *refresh_volume(struct storage_volume *v)
{
    struct blkdev *p = blkpart_device(&v->part);
    if (!p) return 0;
    v->io.quarantined = p->quarantined;
    v->io.write_cache_state = p->write_cache_state;
    v->io.flush = p->flush ? volume_flush : 0;
    return p;
}

static void refresh_cache(void *ctx, struct blkdev *dev)
{
    struct storage *s = ctx;
    for (unsigned d = 2; d < 26; d++)
        if (dev == &s->volumes[d].io) { refresh_volume(&s->volumes[d]); return; }
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

int storage_setup(struct storage *s, size_t bytes)
{
    if (!s) return -FS_EINVAL;
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
int storage_add_disk(struct storage *s, unsigned disk, struct blkdev *dev)
{
    if (!s || !s->ready || s->stopped || disk >= STORAGE_DISKS || !dev || s->disks[disk]) return -FS_EINVAL;
    s->disks[disk] = true;
    struct partition_table table;
    int e = partition_scan(dev, &table);
    if (e) return s->disk_errors[disk] = e;
    uint8_t mbr[512];
    if ((e = dev->read(dev, 0, 1, mbr))) return s->disk_errors[disk] = e;
    uint32_t boot_start = fs_rd32(mbr + 454), boot_count = fs_rd32(mbr + 458);
    for (unsigned i = 0; i < table.count; i++) {
        struct partition *part = &table.entries[i];
        if (!fat_partition(part->type)) continue;
        bool boot = !disk && !part->logical && part->start == boot_start && part->count == boot_count;
        unsigned d = boot ? 2 : s->next_drive++;
        if (d >= 26) return s->disk_errors[disk] = -FS_ENOSPC;
        struct storage_volume *v = &s->volumes[d];
        v->present = true; v->drive = d; v->disk = disk; v->partition = i + 1;
        e = blkpart_init(&v->part, dev, part);
        if (e) { v->error = e; continue; }
        v->io = (struct blkdev){ .read = volume_read, .write = volume_write,
            .capacity = part->count, .sector_size = 512, .ctx = v };
        refresh_volume(v);
        e = fat_mount(&v->fat, &s->cache, &v->io, 0, part->count, false);
        v->writes_before_gate = v->writes;
        if (!e) {
            v->read_gate = !(v->fat.ro_reasons & ~(FAT_RO_REQUEST | FAT_RO_DURABILITY));
            if (v->read_gate) v->read_sequence = ++s->sequence;
            e = vfs_attach(&s->vfs, d, &v->fat);
        }
        v->error = e;
    }
    return 0;
}
struct storage_volume *storage_volume(struct storage *s, unsigned d)
{
    if (!s || !s->ready || d >= 26 || !s->volumes[d].present) return 0;
    struct storage_volume *v = &s->volumes[d];
    refresh_volume(v);
    return v;
}
int storage_enable_write(struct storage *s, unsigned d)
{
    struct storage_volume *v = storage_volume(s, d);
    if (!v || s->stopped) return -FS_ENOENT;
    if (s->writer_error) return s->writer_error;
    if (v->error) return v->error;
    if (!v->read_gate || v->writes_before_gate) { v->refused++; return -FS_EROFS; }
    fs_lock_take(&s->vfs.lock);
    refresh_volume(v);
    int e = fat_enable_write(&v->fat);
    if (!e && !v->write_sequence) v->write_sequence = ++s->sequence;
    fs_lock_drop(&s->vfs.lock);
    return e;
}
int storage_writeback(struct storage *s, uint32_t now)
{
    if (!s || !s->ready || s->stopped) return -FS_EINVAL;
    fs_lock_take(&s->vfs.lock);
    for (unsigned d = 2; d < 26; d++) if (s->volumes[d].present) refresh_volume(&s->volumes[d]);
    int e = cache_writeback_tick(&s->cache, now);
    s->writer_ticks++;
    if (e && !s->writer_error) s->writer_error = e;
    /* Delayed failures revoke the mount immediately, not just next commit. */
    for (unsigned d = 2; d < 26; d++) {
        struct storage_volume *v = &s->volumes[d];
        if (!v->present || !v->fat.mounted) continue;
        int error = cache_error(&s->cache, &v->io);
        if (error) { v->fat.readonly = true; v->fat.ro_reasons |= FAT_RO_WRITE_ERROR; }
    }
    fs_lock_drop(&s->vfs.lock);
    return e;
}
int storage_shutdown(struct storage *s)
{
    if (!s || !s->ready) return -FS_EINVAL;
    s->stopped = true;
    int result = s->writer_error;
    /* Preflight all volumes before setting ANY clean flag. */
    fs_lock_take(&s->vfs.lock);
    for (unsigned i = 0; i < VFS_NODES; i++) if (s->vfs.nodes[i].refs && !result) result = -FS_EBUSY;
    for (unsigned d = 2; d < 26; d++) {
        struct storage_volume *v = &s->volumes[d];
        if (!v->present || !v->fat.mounted) continue;
        refresh_volume(v);
        int e = fat_commit(&v->fat);
        if (e && !result) result = e;
    }
    fs_lock_drop(&s->vfs.lock);
    if (result) return result;
    for (unsigned d = 2; d < 26; d++) if (s->vfs.volumes[d]) {
        int e = vfs_detach(&s->vfs, d);
        if (e) return e;
    }
    return 0;
}
void storage_destroy(struct storage *s)
{
    if (!s || !s->ready) return;
    vfs_destroy(&s->vfs); cache_destroy(&s->cache); s->ready = false;
}
struct storage *storage_get(void) { return &system_storage; }

bool storage_probe_readonly(const char *selector, unsigned length)
{
    /* Every test starts without a disk sink or a writable mount. Destructive
     * probes explicitly open their gates, F0 panic never starts a writer. */
    return selector && length != 0;
}
int storage_sync(void)
{
    int e = bootlog_shutdown();
    if (e) { system_storage.stopped = true; return e; }
    return storage_shutdown(&system_storage);
}
#ifndef FS_HOST
static bool initialized, work_pending;
static void storage_work(void *arg)
{
    struct storage *s = arg;
    if (!s->stopped) {
        fs_worker_enter();
        bootlog_drain();
        storage_writeback(s, fs_now_ms());
        fs_worker_leave();
    }
    work_pending = false;
}
static void storage_timer(void *arg)
{
    struct storage *s = arg;
    while (!s->stopped) {
        task_sleep_ms(1);
        struct bootlog_stats log;
        bootlog_snapshot(&log);
        bool due = cache_writeback_due(&s->cache, fs_now_ms()) || (log.active && log.queued);
        if (due && !work_pending && !s->stopped) {
            work_pending = true;
            if (!kwork_queue(storage_work, s)) work_pending = false;
        }
    }
    task_exit(0);
}
void storage_init(void)
{
    if (initialized) return;
    initialized = true;
    fs_calendar_init();
    struct storage *s = &system_storage;
    int e = storage_setup(s, CACHE_DEFAULT_BYTES);
    if (e) { klog("[storage] unavailable error=%d", e); return; }
    bool probe = (g_boot.flags & CBI_F_TEST_REQUEST) && storage_probe_readonly(g_boot.test_request, g_boot.test_request_len);
    struct task *timer = 0;
    if (!probe) {
        e = kwork_init();
        if (!e) timer = task_create_kernel("storage-timer", storage_timer, s, P_DEVICE);
        if (e || !timer) s->writer_error = e ? e : -FS_ENOMEM;
    }
    for (unsigned slot = 0; slot < STORAGE_DISKS; slot++) {
        struct ata_device *dev = ata_device_get(slot / 2, slot % 2);
        if (!dev) continue;
        e = storage_add_disk(s, slot, &dev->block);
        rec_emit("boot", "DATA", "group=storage disk=%u result=%d gate=read writes=0", slot, e);
    }
    for (unsigned d = 2; d < 26; d++) {
        struct storage_volume *v = storage_volume(s, d);
        if (!v) continue;
        if (!probe && !v->error) storage_enable_write(s, d);
        rec_emit("boot", "DATA", "group=mount drive=%c disk=%u part=%u type=%u mode=%s reasons=%u error=%d read_seq=%u writes_before_gate=%llu",
                 'A' + d, v->disk, v->partition, v->fat.type, v->fat.readonly ? "ro" : "rw",
                 v->fat.ro_reasons, v->error, v->read_sequence, v->writes_before_gate);
    }
    /* CBI1 has no loader sector fingerprints. Fixed slot mapping above is
     * f1-09's rule; identity qualification remains explicitly unproven. */
    rec_emit("boot", "DATA", "group=storage_identity binding=disk0_partition1 qualified=0 reason=loader_fingerprints_absent");
    if (!probe) {
        struct storage_volume *boot = storage_volume(s, 2);
        e = bootlog_activate(&s->vfs, boot && !boot->error && !boot->fat.readonly, s->sequence);
        klog("[storage] disk_log=%s error=%d", e ? "unavailable" : "available", e);
        if (timer) task_start(timer);
        else klog("[storage] writeback unavailable; volumes read-only error=%d", s->writer_error);
    }
}
#else
void storage_init(void) { }
#endif
