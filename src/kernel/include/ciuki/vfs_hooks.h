/* Optional native namespace observers. Called with the F1 namespace lock.
 * Standalone F1 builds intentionally need no native process implementation.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_VFS_HOOKS_H
#define CIUKI_VFS_HOOKS_H
#include <stdbool.h>
#include <stdint.h>
struct vfs;
struct vfs_node;
struct fat_volume;
struct fat_entry;
struct fs_ro_volume;
/* Internal storage/VFS seam: caller holds the namespace lock continuously
 * from all-volume open-description preflight through durable detach. */
int vfs_detach_locked(struct vfs *, unsigned);
extern void files_detach(struct vfs *) __attribute__((weak));
extern int px_legacy_share(struct vfs *, struct fat_volume *, const struct fat_entry *, unsigned, unsigned, unsigned, uint64_t) __attribute__((weak));
extern bool px_legacy_busy(struct vfs *, struct fat_volume *, const struct fat_entry *) __attribute__((weak));
extern void px_legacy_changed(struct vfs *, struct fat_volume *, const struct fat_entry *, const struct fat_entry *, bool committed_write) __attribute__((weak));
extern int px_legacy_closed(struct vfs *, struct vfs_node *) __attribute__((weak));
/* Only open descriptions on this volume block detach, never cwd pins or
 * synthetic device/namespace nodes. Mutation pin checks remain separate. */
extern bool px_volume_busy(struct vfs *, struct fat_volume *) __attribute__((weak));
extern int px_ro_share(struct vfs *, unsigned, uint64_t, unsigned, unsigned, unsigned, uint64_t) __attribute__((weak));
extern bool px_ro_busy(struct vfs *, struct fs_ro_volume *) __attribute__((weak));
extern void px_volume_detached(struct vfs *, unsigned) __attribute__((weak));
/* fx-26: a drive was published or withdrawn (generation[d] changed);
 * media_dev.c wakes /dev/media pollers (the tray's media notification). */
extern void vfs_namespace_changed(void) __attribute__((weak));
static inline void namespace_changed(void) { if (vfs_namespace_changed) vfs_namespace_changed(); }
/* ---- sec-k03: legacy-table authorization (one block) -----------------
 * Implemented by the native namespace (proc/posixpath.c) with the same
 * resolver and decision as native/Win32 calls. The target names the
 * authorized identity so the legacy frontend confirms that its own lookup
 * reached that object; a mismatch is retried, never authorized. Absent
 * hooks deny every subject table (kernel tables carry no subject). */
struct vfs_table;
struct security_subject;
struct vfs_policy_target; /* vfs.h */
extern int px_table_authorize_locked(struct vfs_table *, const char *, unsigned flags, unsigned access,
                                     struct vfs_policy_target *) __attribute__((weak));
extern int px_table_rename_locked(struct vfs_table *, const char *, const char *,
                                  struct vfs_policy_target *, struct vfs_policy_target *) __attribute__((weak));
/* ---- end sec-k03 ----------------------------------------------------- */
#endif
