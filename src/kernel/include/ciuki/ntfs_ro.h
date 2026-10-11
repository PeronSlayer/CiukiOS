/* Independent NTFS read subset. SPDX-License-Identifier: GPL-2.0-only */
#ifndef CIUKI_NTFS_RO_H
#define CIUKI_NTFS_RO_H
#include <stddef.h>
#include <stdint.h>

/* Negative errors use the filesystem/Linux numbers, independent of libc. */
enum ntfs_ro_error {
    NTFS_RO_ENOENT=2, NTFS_RO_EIO=5, NTFS_RO_ENOMEM=12,
    NTFS_RO_ENOTDIR=20, NTFS_RO_EISDIR=21, NTFS_RO_EINVAL=22,
    NTFS_RO_ENAMETOOLONG=36, NTFS_RO_EILSEQ=84, NTFS_RO_EOPNOTSUPP=95
};
enum ntfs_ro_reason {
    NTFS_RO_OK, NTFS_RO_INVALID, NTFS_RO_UNSUPPORTED,
    NTFS_RO_DIRTY, NTFS_RO_RECOVERY_REQUIRED, NTFS_RO_HIBERNATED
};
/* No write or flush operation exists. Success means every requested byte
 * was read. LBAs are relative to the containing volume, not the disk. */
typedef int (*ntfs_ro_read_blocks)(void *, uint64_t lba, uint32_t count, void *);
struct ntfs_ro;
struct ntfs_ro_node { uint64_t reference, size; uint32_t directory; };
struct ntfs_ro_entry {
    uint64_t reference, size;
    uint32_t directory, name_space; /* 2 = DOS alias of the same reference */
    char name[1021];               /* at most 255 UTF-16 units, UTF-8 */
    uint32_t file_attributes;      /* FILE_NAME flags (f4-04): 1 RO, 2 hidden, 4 system, 0x20 archive */
};
typedef int (*ntfs_ro_emit)(void *, const struct ntfs_ro_entry *);

/* Caller reserves/charges this workspace (alignment of uint64_t) before
 * mount. No allocation, private block cache or global state. Serialize calls
 * on a context; callbacks must not reenter it. Entries expire on return.
 * Invalidate handles/context on media replacement before any further call. */
size_t ntfs_ro_workspace_size(void);
int ntfs_ro_mount(void *workspace, size_t bytes, ntfs_ro_read_blocks, void *opaque,
                  uint32_t sector_size, uint64_t volume_sectors,
                  struct ntfs_ro **out, enum ntfs_ro_reason *reason);
void ntfs_ro_unmount(struct ntfs_ro *);
int ntfs_ro_root(struct ntfs_ro *, struct ntfs_ro_node *);
int ntfs_ro_open(struct ntfs_ro *, uint64_t reference, struct ntfs_ro_node *);
/* Lookup accepts one UTF-8 component (up to 255 UTF-16 units). No ADS or
 * separators. Long names and DOS aliases identify the same MFT reference. */
int ntfs_ro_lookup(struct ntfs_ro *, const struct ntfs_ro_node *directory,
                   const char *component, struct ntfs_ro_node *);
/* Emits on-disk names, including explicitly marked aliases. Return 0 from
 * emit to continue, a negative error to abort. A partial listing on failure
 * must be discarded. Directory traversal validates the entire index. */
int ntfs_ro_list(struct ntfs_ro *, const struct ntfs_ro_node *, ntfs_ro_emit, void *);
/* Offset stays 64-bit; n is bounded to 1 MiB per operation. On error *read
 * is zero and the caller must discard buffer contents. No close I/O needed. */
int ntfs_ro_pread(struct ntfs_ro *, const struct ntfs_ro_node *, uint64_t offset,
                  void *buffer, size_t n, size_t *read);
#endif
