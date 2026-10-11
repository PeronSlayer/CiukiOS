/* Read-only filesystem interface, F4. SPDX-License-Identifier: GPL-2.0-only
 *
 * Driver calls are serialized under the VFS namespace lock, may sleep and
 * use ONLY the supplied shared block cache. No private block cache, writes,
 * durability flushes or driver-owned aliases. IDs and sizes are 64-bit and
 * contain no implied cluster/LBA encoding. metadata() validates an ID.
 * readdir's cursor is opaque to the VFS, zero starts, and success advances
 * exactly one logical entry (all sections of a multi-extent file together).
 * Driver walks and storage are bounded, exhaustion is an explicit error.
 * Driver lookup uses long/supplied names; the VFS builds the complete bounded
 * directory view before resolving aliases or returning any entries.
 *
 * generation() observes readiness/media identity; it never retries a failed
 * read. mount_generation is captured only after mount succeeds. A mismatch
 * or media loss invalidates cache, alias views, nodes/cwd/search identities,
 * removes the drive letter, and leaves every old open description failing
 * EIO. The caller must invalidate before reusing the mount/letter. unmount
 * releases parser state only; old nodes carry a VFS mount generation, never
 * a pointer into driver workspace. No public native ABI is defined here.
 *
 * f4-04 additions. A node may carry status: a negative FS_E* error (for
 * example EOPNOTSUPP for compressed, encrypted or reparse content, EIO for a
 * damaged record) that every lookup of that name returns. The entry is still
 * listed by readdir, so the file is visible but never opened with wrong
 * bytes. Drivers may declare FS_RO_STREAM_SYNTAX (a ':' in a lookup component
 * is alternate-data-stream syntax: EOPNOTSUPP, not ENOENT) and
 * FS_RO_DRIVER_COLLATION (the driver's lookup() implements the volume's own
 * case folding; the VFS falls back to it when its folding finds no name and
 * maps the answer back onto the view entry with the same id). Entries whose
 * names the VFS cannot represent are omitted by the driver and counted in
 * the volume's names_omitted.
 */
#ifndef CIUKI_FS_RO_H
#define CIUKI_FS_RO_H
#include "../../fs/cache.h"
#include "../../fs/path.h"
#define FS_RO_DIRECTORY 16u
#define FS_RO_READONLY 1u
#define FS_RO_ENTRIES 256u
#define FS_RO_VIEWS 32u
struct fs_ro_cursor { uint64_t opaque[4]; };
struct fs_ro_node {
    uint64_t id, parent, size;
    uint8_t attr;
    char name[FS_NAME_BYTES], alias[40];
    int32_t status; /* 0 or negative FS_E*: lookup fails with it (see above) */
};
struct fs_ro_volume;
#define FS_RO_STREAM_SYNTAX 1u
#define FS_RO_DRIVER_COLLATION 2u
struct fs_ro_ops {
    const char *name;
    int (*probe)(struct block_cache *, struct blkdev *, uint64_t start);
    int (*mount)(struct fs_ro_volume *);
    void (*unmount)(struct fs_ro_volume *);
    int (*root)(struct fs_ro_volume *, struct fs_ro_node *);
    int (*lookup)(struct fs_ro_volume *, uint64_t directory, const char *, struct fs_ro_node *);
    int (*readdir)(struct fs_ro_volume *, uint64_t directory, struct fs_ro_cursor *, struct fs_ro_node *);
    int (*read)(struct fs_ro_volume *, uint64_t id, uint64_t offset, void *, size_t, size_t *done);
    int (*metadata)(struct fs_ro_volume *, uint64_t id, struct fs_ro_node *);
    uint32_t (*generation)(struct fs_ro_volume *);
    uint32_t flags; /* FS_RO_* */
};
struct fs_ro_view;
struct fs_ro_volume {
    const struct fs_ro_ops *ops;
    struct block_cache *cache;
    struct blkdev *dev;
    uint64_t start, sectors;
    uint32_t mount_generation, view_count, names_omitted;
    bool mounted;
    void *state;
    struct fs_ro_view *views; /* host VFS names, never cached media blocks */
    const char *diagnostic;
};
extern const struct fs_ro_ops iso9660_ops;
extern const struct fs_ro_ops ntfs_ops;
/* Valid VFS 8.3 alias syntax (uppercase ASCII, one dot); out receives the
 * canonical form. Drivers use it to drop on-media aliases the view refuses. */
bool fs_ro_short_name(const char *name, char out[40]);
int fs_ro_mount(struct fs_ro_volume *, const struct fs_ro_ops *, struct block_cache *,
                struct blkdev *, uint64_t start, uint64_t sectors);
void fs_ro_unmount(struct fs_ro_volume *);
int fs_ro_check(struct fs_ro_volume *);
int fs_ro_lookup(struct fs_ro_volume *, uint64_t dir, const char *, struct fs_ro_node *);
int fs_ro_next(struct fs_ro_volume *, uint64_t dir, uint32_t *cursor, struct fs_ro_node *);
int fs_ro_read(struct fs_ro_volume *, uint64_t id, uint64_t offset, void *, size_t, size_t *);
#endif
