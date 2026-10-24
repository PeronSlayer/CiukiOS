/* G14 ScanDisk engine (fs/scandisk.c, docs/design/startup-recovery.md 2.4).
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_SCANDISK_H
#define CIUKI_SCANDISK_H
/* abi.h first: host builds then take the C library's own macro definitions. */
#include <ciuki/abi.h>
#include "blkdev.h"

/* Check the FAT volume on `dev` (partition-relative sectors). r->flags and
 * r->drive are the input (CIUKI_SCANDISK_FIX, THOROUGH, FREE_LOST, NO_COPY,
 * AT_START kept); the rest is the report. Without FIX nothing is written.
 * The caller owns exclusion: nothing else may use the volume meanwhile.
 * Returns r->result. */
int scandisk_run(struct blkdev *dev, struct ciuki_scandisk *r);
#endif
