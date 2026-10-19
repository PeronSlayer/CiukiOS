/* f0-49 pagefile adapter: the pager backend on a writable fixed FAT volume.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_FS_PAGEFILE_H
#define CIUKI_FS_PAGEFILE_H
#include "vfs.h"
#include "blkdev.h"
#include <ciuki/pager.h>

struct pagefile {
    struct vfs *vfs;
    struct vfs_table table;
    struct fat_volume *volume;
    struct blkdev *target;            /* partition view or device: request routing */
    struct blk_reservation in, out;   /* f1-83 records for page I/O only */
    struct vfs_pagefile_cursor cursor;
    const char *path;
    unsigned drive;
    int handle;
    uint32_t cluster_bytes;
    bool table_live, open, detached, lost;
    uint64_t reads, writes, cancels;
};
/* No I/O: binds the adapter; the pager manager opens it (backend->open). */
int pagefile_init(struct pagefile *, struct vfs *, unsigned drive, const char *path, struct blkdev *target);
extern const struct pager_backend pagefile_backend;
/* Storage shutdown without the pager core (host fixtures): close/reopen. */
int pagefile_detach(void *ctx);
int pagefile_reattach(void *ctx);
void pagefile_close(void *ctx);
#endif
