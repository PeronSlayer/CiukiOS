/* Internal app/libdisk interface; not an installed public SDK header.
 * SPDX-License-Identifier: MIT */
#ifndef CIUKI_PARTITION_TOOL_H
#define CIUKI_PARTITION_TOOL_H
#include "include/libdisk.h"
#define PT_SECTORS 65u
struct pt_table {
    unsigned disk, count;
    struct disk_identity id;
    uint32_t lba[PT_SECTORS];
    uint8_t original[PT_SECTORS][512];
    uint8_t sector[PT_SECTORS][512];
    bool large;
};
enum pt_kind { PT_PRIMARY, PT_EXTENDED, PT_LOGICAL };
/* Numbering: primary slots 1..4; logical drives 5... (EBR order). */
int pt_read(unsigned disk, struct pt_table *);
int pt_create(struct pt_table *, enum pt_kind, uint32_t start, uint32_t sectors);
int pt_delete(struct pt_table *, unsigned number);
int pt_active(struct pt_table *, unsigned primary);
/* Revalidates every table sector after taking the exclusive kernel lease.
 * EBR children are flushed before links, one changed existing sector last.
 * Refuses multi-parent edits in a single commit (apply operations one by one). */
int pt_commit(struct pt_table *);
#endif
