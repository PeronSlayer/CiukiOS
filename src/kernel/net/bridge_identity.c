/* Installation identity, persisted before a guest MAC can be published.
 * VFS rename refuses an existing destination; no truncate/replace is used.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/net_bridge_port.h>
#include "../fs/vfs.h"

static const char identity_path[] = "C:\\System\\NETID.BIN";
static const char pending_path[] = "C:\\System\\NETID.TMP";

static int read_record(struct vfs_table *t, const char *path, uint8_t id[16])
{
    int h = vfs_open(t, path, VFS_READ, VFS_DENY_WRITE, 0);
    if (h < 0) return h;
    uint8_t record[17]; size_t done = 0;
    int error = vfs_read(t, h, record, sizeof(record), &done);
    uint8_t nonzero = 0;
    if (!error && done == 16)
        for (unsigned i = 0; i < 16; i++) nonzero |= record[i];
    if (!error && (done != 16 || !nonzero)) error = -FS_EINVAL;
    /* Also retries a failed final barrier from an earlier publication. */
    if (!error) error = vfs_commit(t, h);
    int closed = vfs_close(t, h);
    if (!error) error = closed;
    if (!error) memcpy(id, record, 16);
    return error;
}

int net_bridge_identity_read_file(void *arg, uint8_t id[16])
{
    if (!arg || !id) return -FS_EINVAL;
    struct vfs_table *t = arg;
    memset(id, 0, 16);
    int error = read_record(t, identity_path, id);
    if (error != -FS_ENOENT) return error;
    uint8_t pending[16];
    error = read_record(t, pending_path, pending);
    if (error) return error;
    error = vfs_rename(t, pending_path, identity_path);
    if (!error) error = vfs_flush_drive(t, 2);
    if (!error) memcpy(id, pending, 16);
    return error;
}

int net_bridge_identity_write_file(void *arg, const uint8_t id[16])
{
    if (!arg || !id) return -FS_EINVAL;
    struct vfs_table *t = arg;
    uint8_t nonzero = 0;
    for (unsigned i = 0; i < 16; i++) nonzero |= id[i];
    if (!nonzero) return -FS_EINVAL;
    /* The canonical installation already contains System. Do not create a
     * missing installation directory or open the storage write gate here. */
    int h = vfs_open(t, pending_path, VFS_WRITE | VFS_CREATE | VFS_EXCLUSIVE,
                     VFS_DENY_ALL, FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM);
    if (h < 0) return h;
    size_t done = 0;
    int error = vfs_write(t, h, id, 16, &done);
    if (!error && done != 16) error = -FS_EIO;
    if (!error) error = vfs_commit(t, h);
    int closed = vfs_close(t, h);
    if (!error) error = closed;
    /* Leave a failed pending record intact: a later read can recover a valid
     * committed candidate, but cannot silently replace partial/corrupt data. */
    if (!error) error = vfs_rename(t, pending_path, identity_path);
    if (!error) error = vfs_flush_drive(t, 2);
    return error;
}
