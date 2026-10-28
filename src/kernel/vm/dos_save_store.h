/* Persistent title namespace on the native system volume.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_DOS_SAVE_STORE_H
#define CIUKI_DOS_SAVE_STORE_H
#include "dos_save.h"
#include "../fs/vfs.h"
/* Legacy machine namespace: kernel-internal VMs and the interim bootstrap
 * session only. sec-k05: every other store root is the owner's profile
 * (dos_identity_root DOS_ROOT_STORE); one user's store is never opened for
 * another user's VM, and archives/exports stay inside the same owner's tree. */
#define DOS_SAVE_ROOT "C:\\SYSTEM\\DOS\\SAVES"
#define DOS_SAVE_EXPORT_ROOT "C:\\Documents\\Saves"
#define DOS_SAVE_PATH 192u
struct dos_save_file { struct vfs_table *table; int fd; bool writable, absent; };
void dos_save_file_io(struct dos_save_file *,struct dos_save_io *);
/* Archive stale namespaces before returning the new/current file. root is a
 * DOS-spelled directory on the table's volume, at most 96 bytes. */
int dos_save_store_open(struct dos_save_file *,struct dos_save *,
                        const struct dos_save_identity *,const char *root,
                        char path[DOS_SAVE_PATH],unsigned *archived);
int dos_save_store_query(struct vfs_table *,const char *root,unsigned game,uint32_t *bytes);
int dos_save_store_reset(struct vfs_table *,const char *root,unsigned game);
/* Export current namespace's changed files to a newly created folder under
 * destination (which must exist and pass the caller's path policy).
 * Requires an idle title and an already validated matching base. folder
 * receives the created folder's full DOS path. */
int dos_save_store_export(struct vfs_table *,struct blkdev *merged,struct blkdev *base,
                          struct block_cache *,unsigned game,const char *destination,
                          char folder[DOS_SAVE_PATH],uint32_t *files);
#endif
