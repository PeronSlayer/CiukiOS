/* Registry-owned durable display attempts. Independent implementation;
 * preserves the VFS commit/close authorization boundary of f1-27.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/display_driver.h>
#include "../../fs/vfs.h"

/* One persistence worker/diagnostic at a time; never put a VFS table on the
 * kernel stack. A yielding filesystem call cannot re-enter this table. */
static struct vfs_table attempt_table;
static bool attempt_busy;

static int marker_write(const char *path, const char *data, size_t size, bool clear)
{
    int fd=vfs_open(&attempt_table,path,VFS_WRITE | (clear ? 0 : VFS_CREATE),VFS_DENY_ALL,0);
    if (fd<0) return fd;
    int err;
    if (clear) err=vfs_truncate(&attempt_table,fd,0);
    else {
        size_t done=0;
        err=vfs_write(&attempt_table,fd,data,size,&done);
        if (!err && done!=size) err=-FS_EIO;
    }
    if (!err) err=vfs_commit(&attempt_table,fd);
    int closed=vfs_close(&attempt_table,fd);
    return err ? err : closed;
}

int display_attempt_marker(struct vfs *vfs, const char *id, uint16_t vendor,
    uint16_t device, const char *legacy_path, bool clear, bool *previous,
    const char *diagnostic)
{
    if (previous) *previous=false;
    if (!id || !*id || (clear && diagnostic) || (!clear && !diagnostic && !previous)) return -EINVAL;
    unsigned n=0;
    for (; id[n]; n++) {
        char c=id[n];
        if (n>=15 || !((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-')) return -EINVAL;
    }
    char path[80];
    ksnprintf(path,sizeof(path),"C:/system/settings/accel-%s-%04x-%04x.cfg",id,vendor,device);
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    if (attempt_busy) return -EBUSY;
    attempt_busy=true;
    int err=vfs_table_init(vfs,&attempt_table,2);
    if (err) goto done;
    if (!clear && !diagnostic) {
        struct fat_entry entry;
        err=vfs_stat(&attempt_table,path,&entry);
        if (!err && entry.size) { *previous=true; goto out; }
        if (err && err!=-FS_ENOENT) goto out;
        if (legacy_path) {
            err=vfs_stat(&attempt_table,legacy_path,&entry);
            if (!err && entry.size) {
                /* Even an unreadable, oversized or read-only legacy record
                 * remains a retained attempt. Never guess its contents away. */
                *previous=true;
                if (vfs->volumes[2]->readonly) { err=-FS_EROFS; goto out; }
                char bytes[256];
                static const char note[]="migrated=legacy\n";
                if (entry.size>sizeof(bytes)-(sizeof(note)-1)) { err=-FS_EIO; goto out; }
                int fd=vfs_open(&attempt_table,legacy_path,VFS_READ,VFS_DENY_ALL,0);
                if (fd<0) { err=fd; goto out; }
                size_t count=0;
                err=vfs_read(&attempt_table,fd,bytes,entry.size,&count);
                if (!err && count!=entry.size) err=-FS_EIO;
                int closed=vfs_close(&attempt_table,fd);
                if (!err) err=closed;
                if (err) goto out;
                memcpy(bytes+count,note,sizeof(note)-1);
                err=marker_write(path,bytes,count+sizeof(note)-1,false);
                if (err) goto out;
                /* Commit AND close the replacement before touching old data.
                 * A cut here leaves both guards; either one prevents retry. */
                klog("[accel] marker migration driver=%s pci=%04x:%04x legacy=%s retained=1",id,vendor,device,legacy_path);
                err=marker_write(legacy_path,0,0,true);
                goto out;
            }
            if (err && err!=-FS_ENOENT) goto out;
        }
    }
    if (vfs->volumes[2]->readonly) { err=-FS_EROFS; goto out; }
    const char *data=diagnostic ? diagnostic : "accel=attempt\n";
    err=marker_write(path,data,strlen(data),clear);
out:
    vfs_table_destroy(&attempt_table);
done:
    attempt_busy=false;
    return err;
}
