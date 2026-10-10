/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_PARTITION_H
#define CIUKI_PARTITION_H
#include "blkdev.h"
#define PARTITION_MAX 64u
struct partition { uint64_t start, count; uint8_t type; bool bootable, logical; };
struct partition_table { struct partition entries[PARTITION_MAX]; unsigned count, ebr_reads; };
/* Atomic result: on any error count is zero. LBA28 and 64 EBRs maximum. */
int partition_scan(struct blkdev *, struct partition_table *);
#endif
