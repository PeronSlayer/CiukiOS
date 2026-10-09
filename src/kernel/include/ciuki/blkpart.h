/* MBR partition block views. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_BLKPART_H
#define CIUKI_BLKPART_H
#include "../../fs/partition.h"

struct blkpart {
    struct blkdev block;
    struct blkdev *parent;
    uint64_t start;
};
int blkpart_init(struct blkpart *view, struct blkdev *parent, const struct partition *partition);
/* Refresh parent properties before inspecting the frozen interface's scalar
 * quarantine/cache fields (also done before and after every operation). */
struct blkdev *blkpart_device(struct blkpart *view);
int blkpart_scan(struct blkdev *parent, struct partition_table *table,
                 struct blkpart *views, unsigned capacity);
#endif
