/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_PARTITION_H
#define CIUKI_PARTITION_H
#include "blkdev.h"
#define PARTITION_MAX 64u
struct partition { uint64_t start, count; uint8_t type; bool bootable, logical; };
struct partition_table { struct partition entries[PARTITION_MAX]; unsigned count, ebr_reads; };
/* Atomic result: on any error count is zero. LBA28 and 64 EBRs maximum. */
int partition_scan(struct blkdev *, struct partition_table *);
/* MBR system IDs of FAT volumes: 01h FAT12, 04h FAT16 below 32 MiB,
 * 06h FAT16 (MS-DOS 4 "huge"), 0Bh FAT32, 0Ch FAT32 and 0Eh FAT16 through
 * INT 13h extensions (Microsoft "Disk Partition Types" and ntdddisk.h
 * PARTITION_INFORMATION; FreeDOS kernel initdisk.c IsFATPartition). */
static inline bool partition_fat_type(uint8_t type)
{ return type==0x01 || type==0x04 || type==0x06 || type==0x0b || type==0x0c || type==0x0e; }
/* The volume DOS mounts as C: (FreeDOS kernel 2043 initdisk.c
 * ScanForPrimaryPartitions: SCAN_PRIMARYBOOT, then SCAN_PRIMARY): the first
 * active primary FAT partition in table order, else the first primary FAT
 * partition; 0 when there is none. The VM's C: view, game inventory/removal
 * and save export share it so they always address the same volume (fx-4:
 * the unified base is one active 0Eh partition at LBA 63). */
static inline const struct partition *partition_dos_primary(const struct partition_table *t)
{
    for (unsigned pass=0;pass<2;pass++)
        for (unsigned i=0;i<t->count;i++) {
            const struct partition *p=&t->entries[i];
            if (!p->logical && partition_fat_type(p->type) && (pass || p->bootable)) return p;
        }
    return 0;
}
#endif
