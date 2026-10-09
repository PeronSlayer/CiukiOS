/* Bounded, offset-only views; MBR/EBR policy belongs to partition.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/blkpart.h>

static int part_flush(struct blkdev *d);

struct blkdev *blkpart_device(struct blkpart *v)
{
    if (!v || !v->parent)
        return 0;
    v->block.quarantined = v->parent->quarantined;
    v->block.write_cache_state = v->parent->write_cache_state;
    v->block.flush = v->parent->flush ? part_flush : 0;
    return &v->block;
}

static int transfer(struct blkdev *d, uint64_t lba, uint32_t n, void *buf, bool write)
{
    struct blkpart *v = d->ctx;
    blkpart_device(v);
    int e = blkdev_range(d, lba, n);
    if (e || !buf)
        return e ? e : -FS_EINVAL;
    if (lba > UINT64_MAX - v->start)
        return -FS_EINVAL;
    lba += v->start;
    e = blkdev_range(v->parent, lba, n);
    if (!e) {
        if (write)
            e = v->parent->write ? v->parent->write(v->parent, lba, n, buf) : -FS_EROFS;
        else
            e = v->parent->read ? v->parent->read(v->parent, lba, n, buf) : -FS_EINVAL;
    }
    blkpart_device(v);
    return e;
}

static int part_read(struct blkdev *d, uint64_t lba, uint32_t n, void *buf)
{
    return transfer(d, lba, n, buf, false);
}

static int part_write(struct blkdev *d, uint64_t lba, uint32_t n, const void *buf)
{
    return transfer(d, lba, n, (void *)buf, true);
}

static int part_flush(struct blkdev *d)
{
    struct blkpart *v = d->ctx;
    blkpart_device(v);
    if (d->quarantined)
        return -FS_EQUARANTINED;
    int e = v->parent->flush ? v->parent->flush(v->parent) : -FS_EOPNOTSUPP;
    blkpart_device(v);
    return e;
}

int blkpart_init(struct blkpart *v, struct blkdev *p, const struct partition *part)
{
    if (!v || !part || !p || p->sector_size != BLKDEV_SECTOR_SIZE || !part->count ||
        part->start >= p->capacity || part->count > p->capacity - part->start || !p->read)
        return -FS_EINVAL;
    memset(v, 0, sizeof(*v));
    v->parent = p;
    v->start = part->start;
    v->block = (struct blkdev){ .read = part_read, .write = p->write ? part_write : 0,
        .capacity = part->count, .sector_size = p->sector_size, .ctx = v };
    blkpart_device(v);
    return 0;
}

int blkpart_scan(struct blkdev *p, struct partition_table *t, struct blkpart *views, unsigned n)
{
    if (!t || (!views && n))
        return -FS_EINVAL;
    int e = partition_scan(p, t);
    if (e)
        return e;
    if (t->count > n)
        return -FS_ENOSPC;
    for (unsigned i = 0; i < t->count; i++) {
        e = blkpart_init(&views[i], p, &t->entries[i]);
        if (e)
            return e;
    }
    return 0;
}
