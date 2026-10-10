/* Optional native namespace observers. Called with the F1 namespace lock.
 * Standalone F1 builds intentionally need no native process implementation.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VFS_HOOKS_H
#define CIUKI_VFS_HOOKS_H
#include <stdbool.h>
struct vfs;
struct vfs_node;
struct fat_volume;
struct fat_entry;
/* Internal storage/VFS seam: caller holds the namespace lock continuously
 * from all-volume open-description preflight through durable detach. */
int vfs_detach_locked(struct vfs *, unsigned);
extern void files_detach(struct vfs *) __attribute__((weak));
extern int px_legacy_share(struct vfs *, struct fat_volume *, const struct fat_entry *, unsigned, unsigned) __attribute__((weak));
extern bool px_legacy_busy(struct vfs *, struct fat_volume *, const struct fat_entry *) __attribute__((weak));
extern void px_legacy_changed(struct vfs *, struct fat_volume *, const struct fat_entry *, const struct fat_entry *) __attribute__((weak));
extern int px_legacy_closed(struct vfs *, struct vfs_node *) __attribute__((weak));
/* Only open descriptions on this volume block detach, never cwd pins or
 * synthetic device/namespace nodes. Mutation pin checks remain separate. */
extern bool px_volume_busy(struct vfs *, struct fat_volume *) __attribute__((weak));
extern void px_volume_detached(struct vfs *, unsigned) __attribute__((weak));
#endif
