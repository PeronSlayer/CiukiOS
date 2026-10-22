/* Persistent title namespace on the native system volume.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_SAVE_STORE_H
#define CIUKI_DOS_SAVE_STORE_H
#include "dos_save.h"
#include "../fs/vfs.h"
#define DOS_SAVE_ROOT "C:\\SYSTEM\\DOS\\SAVES"
struct dos_save_file { struct vfs_table *table; int fd; bool writable, absent; };
void dos_save_file_io(struct dos_save_file *,struct dos_save_io *);
/* Archive stale namespaces before returning the new/current file. */
int dos_save_store_open(struct dos_save_file *,struct dos_save *,
                        const struct dos_save_identity *,char path[128],unsigned *archived);
int dos_save_store_query(struct vfs_table *,unsigned game,uint32_t *bytes);
int dos_save_store_reset(struct vfs_table *,unsigned game);
/* Export current namespace's changed files to a newly created destination.
 * Requires an idle title and an already validated matching base. */
int dos_save_store_export(struct vfs_table *,struct blkdev *merged,struct blkdev *base,
                          struct block_cache *,unsigned game,char path[32],uint32_t *files);
#endif
