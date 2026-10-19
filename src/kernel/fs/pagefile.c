/* f0-49 pagefile adapter (M4). The pager's backing file lives on the
 * writable fixed FAT system volume. open/grow/shrink/map run on the pager
 * manager through the VFS, one bounded volume reservation per step: growth
 * uses the append publication order (zero data, detached chain, tail link,
 * length), shrink publishes the shorter length before releasing the tail,
 * and live clusters never move. Page I/O bypasses FAT and the block cache:
 * prevalidated slot runs, reserved f1-83 request records, the physical
 * controller queue. No namespace, FAT or cache lock on that path.
 * Swap data is never durable state: no flush, no trust across reboot.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "pagefile.h"
#include "fat.h"
#include "cache.h"

#define MAP_TEMP 16u

int pagefile_init(struct pagefile *pf, struct vfs *vfs, unsigned drive, const char *path, struct blkdev *target)
{
    if (!pf || !vfs || drive >= 26 || !path || !target || !vfs->volumes[drive]) return -FS_EINVAL;
    memset(pf, 0, sizeof(*pf));
    pf->vfs = vfs; pf->drive = drive; pf->path = path; pf->target = target;
    pf->volume = vfs->volumes[drive]; pf->handle = -1;
    return 0;
}
static void release(struct pagefile *pf, bool reservations)
{
    if (pf->open) { (void)vfs_close(&pf->table, pf->handle); pf->open = false; pf->handle = -1; }
    if (pf->table_live) { vfs_table_destroy(&pf->table); pf->table_live = false; }
    if (reservations) {
        if (pf->in.live) (void)blk_request_unreserve(&pf->in);
        if (pf->out.live) (void)blk_request_unreserve(&pf->out);
    }
}
static int reopen(struct pagefile *pf, bool create)
{
    int e = vfs_table_init(pf->vfs, &pf->table, pf->drive);
    if (e) return e;
    pf->table_live = true;
    /* Exclusive: no other description can read swap bytes or move the file. */
    int h = vfs_open(&pf->table, pf->path, VFS_READ | VFS_WRITE | (create ? VFS_CREATE : 0),
                     VFS_DENY_ALL, FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM);
    if (h < 0) { release(pf, false); return h; }
    pf->handle = h; pf->open = true;
    memset(&pf->cursor, 0, sizeof(pf->cursor));
    return 0;
}
static int file_size(struct pagefile *pf, uint32_t *size)
{
    struct fat_entry entry;
    int e = vfs_handle_stat(&pf->table, pf->handle, &entry, 0);
    if (!e) *size = entry.size;
    return e;
}
static int pf_open(void *ctx, uint32_t *existing)
{
    struct pagefile *pf = ctx;
    *existing = 0;
    if (pf->open) return -FS_EBUSY;
    int e = reopen(pf, true);
    uint64_t free = 0; uint32_t size = 0;
    if (!e) e = vfs_free_space(&pf->table, pf->drive, &free, &pf->cluster_bytes);
    if (!e && (!pf->cluster_bytes || pf->cluster_bytes > PAGER_EXTENT_BYTES ||
               PAGER_EXTENT_BYTES % pf->cluster_bytes)) e = -FS_EINVAL;
    if (!e) e = file_size(pf, &size);
    /* A partial tail (cut growth or foreign size) is released in place;
     * owned excess past the length is reused by the next extension. */
    if (!e && size % PAGER_EXTENT_BYTES)
        e = vfs_pagefile_trim(&pf->table, pf->handle, size - size % PAGER_EXTENT_BYTES);
    if (!e) e = blk_request_reserve(&pf->in, PAGER_IN_REQUESTS);
    if (!e) e = blk_request_reserve(&pf->out, PAGER_OUT_REQUESTS);
    if (e) { release(pf, true); return e; }
    *existing = size / PAGER_EXTENT_BYTES;
    return 0;
}
/* Sector runs of one extent -> page slots fully inside a contiguous run.
 * Slots straddling discontiguous clusters (<4 KiB clusters) stay unmapped
 * and therefore failed capacity; runs beyond PAGER_RUNS likewise. */
static void map_add(struct pager_map *map, uint64_t offset, const struct vfs_pagefile_run *run)
{
    uint64_t bytes = (uint64_t)run->sectors * 512, first = (offset + PAGER_PAGE - 1) / PAGER_PAGE;
    uint64_t end = (offset + bytes) / PAGER_PAGE;
    if (end <= first) return;
    uint64_t lba = run->lba + (first * PAGER_PAGE - offset) / 512;
    struct pager_run *last = map->runs ? &map->run[map->runs - 1] : 0;
    if (last && last->first + last->count == first && last->lba + (uint64_t)last->count * (PAGER_PAGE / 512) == lba) {
        last->count += (uint32_t)(end - first); return;
    }
    if (map->runs == PAGER_RUNS) return;
    map->run[map->runs++] = (struct pager_run){ .first = (uint32_t)first, .count = (uint32_t)(end - first), .lba = lba };
}
static int pf_grow(void *ctx, uint32_t extent, struct pager_map *map)
{
    struct pagefile *pf = ctx;
    if (!pf->open || extent >= PAGER_MAX_EXTENTS) return -FS_EINVAL;
    uint32_t target = (extent + 1) * PAGER_EXTENT_BYTES, size;
    int e = file_size(pf, &size);
    /* Allocate and flush before any slot is published: each step is one
     * durable publication group under one volume reservation. */
    while (!e && size < target) {
        uint32_t step = target - size > PAGER_GROW_STEP ? size + PAGER_GROW_STEP : target;
        e = vfs_pagefile_extend(&pf->table, pf->handle, step);
        if (!e) size = step;
        fs_service();
    }
    if (e) return e;
    memset(map, 0, sizeof(*map));
    uint32_t per = PAGER_EXTENT_BYTES / pf->cluster_bytes, index = extent * per, done = 0;
    while (!e && done < per) {
        struct vfs_pagefile_run runs[MAP_TEMP];
        unsigned used = 0; uint32_t mapped = 0;
        uint64_t offset = (uint64_t)done * pf->cluster_bytes;
        e = vfs_pagefile_map(&pf->table, pf->handle, &pf->cursor, index + done, per - done,
                             runs, MAP_TEMP, &used, &mapped, 0);
        for (unsigned r = 0; !e && r < used; r++) {
            map_add(map, offset, &runs[r]);
            offset += (uint64_t)runs[r].sectors * 512;
        }
        done += mapped;
        fs_service();
    }
    return e;
}
static int pf_shrink(void *ctx, uint32_t extents)
{
    struct pagefile *pf = ctx;
    if (!pf->open) return -FS_EBADF;
    uint32_t target = extents * PAGER_EXTENT_BYTES, size;
    int e = file_size(pf, &size);
    /* Each step publishes a shorter length, then releases that tail: one
     * bounded volume reservation; a cut leaves a sub-extent tail, released
     * on the next open, never a live slot. */
    while (!e && size > target) {
        uint32_t step = size - target > PAGER_SHRINK_STEP ? size - PAGER_SHRINK_STEP : target;
        e = vfs_pagefile_trim(&pf->table, pf->handle, step);
        if (!e) size = step;
        fs_service();
    }
    return e;
}
static int pf_space(void *ctx, uint64_t *free, uint64_t *capacity)
{
    struct pagefile *pf = ctx;
    if (!pf->open) return -FS_EBADF;
    uint32_t cb;
    int e = vfs_free_space(&pf->table, pf->drive, free, &cb);
    if (!e) *capacity = (uint64_t)pf->volume->clusters * cb;
    return e;
}
static struct blkdev *physical(struct pagefile *pf, uint64_t *lba)
{
    struct blkdev *d = pf->target;
    for (unsigned depth = 0; d && d->request_parent; depth++) {
        struct blkdev *parent = d->request_parent;
        if (depth >= 4 || d->media_generation != parent->media_generation || parent->quarantined ||
            *lba > UINT64_MAX - d->request_offset) return 0;
        *lba += d->request_offset; d = parent;
    }
    return d && !d->quarantined ? d : 0;
}
static int pf_submit(void *ctx, struct pager_io *io)
{
    struct pagefile *pf = ctx;
    io->token = 0;
    if (pf->lost || (io->write && (pf->detached || !pf->open))) return -PAGER_EIO;
    uint64_t lba = io->lba;
    struct blkdev *d = physical(pf, &lba);
    if (!d || !io->bytes || io->bytes % 512 || blkdev_range(d, lba, io->bytes / 512)) return -PAGER_EIO;
    enum blk_priority priority = io->cls == PAGER_DEMAND ? BLK_DEMAND : BLK_BACKGROUND;
    uint32_t deadline = priority == BLK_DEMAND ? fs_now_ms() + BLKREQ_READER_MS : 0;
    struct blk_request *r = blk_request_prepare_reserved(io->write ? &pf->out : &pf->in, d,
        io->write ? BLK_WRITE : BLK_READ, lba, io->bytes, io->buffer, priority, 0, deadline, 30000);
    if (!r) return -PAGER_EAGAIN;
    if (blk_request_submit(r)) {
        (void)blk_request_cancel(r); (void)blk_request_release(r);
        return -PAGER_EIO;
    }
    io->token = r;
    if (io->write) pf->writes++; else pf->reads++;
    return 0;
}
static int pf_wait(void *ctx, struct pager_io *io)
{
    struct pagefile *pf = ctx;
    struct blk_request *r = io->token;
    if (!r) return -PAGER_EIO;
    int e = blk_request_wait(r);
    bool cancelled = r->state == BLK_CANCELLED || (e == -FS_EAGAIN && r->cancel);
    (void)blk_request_release(r);
    io->token = 0;
    if (cancelled) pf->cancels++;
    return !e ? 0 : cancelled ? -PAGER_ECANCELED : -PAGER_EIO;
}
static bool pf_cancel(void *ctx, struct pager_io *io)
{
    (void)ctx;
    return io->token && blk_request_cancel(io->token);
}
static bool pf_idle(void *ctx)
{
    struct pagefile *pf = ctx;
    struct cache_stats stats;
    cache_stats(pf->volume->cache, &stats);
    if (stats.dirty_bytes) return false; /* prewrite only after pending VFAT writes */
    uint64_t lba = 0;
    struct blkdev *d = physical(pf, &lba);
    if (!d) return false;
    if (d->queue) {
        struct blk_queue_stats q;
        blk_queue_snapshot(d->queue, &q);
        for (unsigned p = 0; p < BLK_PRIORITIES; p++) if (q.depth[p]) return false;
    }
    return true;
}
static bool pf_writable(void *ctx)
{
    struct pagefile *pf = ctx;
    return pf->open && !pf->detached && !pf->lost && pf->volume->mounted && !pf->volume->readonly;
}
void pagefile_close(void *ctx) { release(ctx, true); }
int pagefile_detach(void *ctx)
{
    struct pagefile *pf = ctx;
    /* The volume dependency ends; reserved records and runs stay, so data
     * clusters still owned by the file remain readable for live pages. */
    if (pf->open) release(pf, false);
    pf->detached = true;
    return 0;
}
int pagefile_reattach(void *ctx)
{
    struct pagefile *pf = ctx;
    if (!pf->detached) return 0;
    int e = reopen(pf, false);
    if (!e) pf->detached = false;
    return e;
}
const struct pager_backend pagefile_backend = {
    .open = pf_open, .grow = pf_grow, .shrink = pf_shrink, .space = pf_space,
    .submit = pf_submit, .wait = pf_wait, .cancel = pf_cancel, .idle = pf_idle,
    .writable = pf_writable, .close = pagefile_close, .detach = pagefile_detach,
    .reattach = pagefile_reattach,
};
