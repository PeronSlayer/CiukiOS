/* Kernel settings-file pattern; no filesystem work on tcpip_thread.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/net_control.h>
static struct vfs *settings_vfs;
static struct vfs_table table;
static struct kmutex lock;
static struct ciuki_net_config saved[2];
static struct net_config_identity saved_ids[2];
static char buffer[NET_SETTINGS_BYTES];
extern int net_control_driver(unsigned, struct ciuki_net_control *, bool) __attribute__((weak));
static int identity(unsigned index, struct net_config_identity *id)
{
    struct ciuki_net_control n = {.index = index};
    int e = net_control_driver ? net_control_driver(index, &n, false) : -ENODEV;
    if (!e) { memset(id, 0, sizeof(*id)); id->vendor=n.pci_vendor; id->device=n.pci_device; memcpy(id->mac,n.mac,6); }
    return e;
}
void net_settings_load(struct vfs *v)
{
    kmutex_init(&lock); settings_vfs = v;
    memset(saved_ids, 0, sizeof(saved_ids));
    for (unsigned i = 0; i < 2; i++) net_config_default(&saved[i]);
    const char *reason = "storage_absent";
    if (v && v->volumes[2] && !vfs_table_init(v, &table, 2)) {
        int fd = vfs_open(&table, "C:\\SYSTEM\\NETWORK.INI", VFS_READ, VFS_DENY_NONE, 0);
        size_t n = 0;
        reason = fd == -FS_ENOENT ? "missing" : "read_failed";
        if (fd >= 0) {
            int e = vfs_read(&table, fd, buffer, sizeof(buffer), &n);
            vfs_close(&table, fd);
            reason = e ? "read_failed" : net_config_parse(buffer, n, saved, saved_ids) ? "invalid" : "saved";
        }
        vfs_table_destroy(&table);
    }
    klog("[net] config boot=%s fallback=%s", reason, !strncmp(reason, "saved", 6) ? "none" : "dhcp");
}
void net_settings_get(unsigned index, struct ciuki_net_config *out)
{
    struct net_config_identity id;
    int e = identity(index, &id);
    kmutex_lock(&lock);
    net_config_default(out);
    if (!e) for (unsigned i=0; i<2; i++) if (!memcmp(&id, &saved_ids[i], sizeof(id))) { *out=saved[i]; break; }
    kmutex_unlock(&lock);
}
int net_settings_save(unsigned index, const struct ciuki_net_config *c)
{
    if (index >= 2 || !net_config_valid(c)) return -EINVAL;
    struct net_config_identity id;
    int error = identity(index, &id); if (error) return error;
    kmutex_lock(&lock);
    if (!settings_vfs || !settings_vfs->volumes[2]) { kmutex_unlock(&lock); return -ENODEV; }
    unsigned slot = index;
    for (unsigned i=0; i<2; i++) if (!memcmp(&id, &saved_ids[i], sizeof(id))) { slot=i; break; }
    if (memcmp(&id, &saved_ids[slot], sizeof(id)))
        for (unsigned i=0; i<2; i++) if (!saved_ids[i].vendor) { slot=i; break; }
    struct ciuki_net_config both[2]; struct net_config_identity ids[2];
    memcpy(both, saved, sizeof(both)); memcpy(ids, saved_ids, sizeof(ids)); both[slot] = *c; ids[slot] = id;
    size_t n = net_config_format(both, ids, buffer, sizeof(buffer)), done = 0;
    int e = n ? vfs_table_init(settings_vfs, &table, 2) : -FS_EINVAL;
    if (!e) {
        int fd = vfs_open(&table, "C:\\SYSTEM\\NETWORK.NEW", VFS_READ|VFS_WRITE|VFS_CREATE|VFS_TRUNCATE, VFS_DENY_ALL, 0);
        e = fd < 0 ? fd : vfs_write(&table, fd, buffer, n, &done);
        if (!e && done != n) e = -FS_EIO;
        if (!e) e = vfs_commit(&table, fd);
        if (fd >= 0) { int close = vfs_close(&table, fd); if (!e) e = close; }
        if (!e) {
            struct px_namespace *space;
            error = files_attach(settings_vfs, &space);
            if (!error) {
                vfs_lock(settings_vfs, 0);
                error = px_rename_locked(space, space->root, "/system/NETWORK.NEW", "/system/NETWORK.INI");
                vfs_unlock(settings_vfs);
            }
            if (error) e = -FS_EIO;
        }
        if (!e) e = vfs_flush_drive(&table, 2);
        vfs_table_destroy(&table);
    }
    if (!e) { saved[slot] = *c; saved_ids[slot] = id; }
    kmutex_unlock(&lock); return px_error(e);
}
