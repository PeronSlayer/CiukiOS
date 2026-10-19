/* Private logical image mount; source owns the f3-47 cache and overlay.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_FILE_VOLUME_H
#define CIUKI_DOS_FILE_VOLUME_H
#include "dos_file.h"
struct dos_file_volume {
    struct vfs space;
    struct vfs_table table;
    struct fat_volume fat;
    struct blkdev *provider;
    bool mounted,table_ready;
};
int dos_file_volume_open(struct dos_file_volume *,struct block_cache *workspace,struct blkdev *logical_provider,bool writable);
int dos_file_volume_close(struct dos_file_volume *);
/* The BIOS path enters this same reservation; provider callbacks used by FAT
 * must call the source directly, rather than recursively entering this gate. */
int dos_file_volume_raw(struct dos_file_volume *,uint32_t lba,void *sector,bool write,const struct vfs_request *);
#endif
