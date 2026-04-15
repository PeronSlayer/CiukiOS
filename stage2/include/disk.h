#ifndef STAGE2_DISK_H
#define STAGE2_DISK_H

#include "types.h"
#include "handoff.h"

void stage2_disk_init(const handoff_v0_t *handoff);
int stage2_disk_ready(void);
u32 stage2_disk_block_size(void);
u64 stage2_disk_block_count(void);
const u8 *stage2_disk_lba_ptr(u64 lba);
int stage2_disk_read_blocks(u64 lba, u32 count, void *out);

#endif
