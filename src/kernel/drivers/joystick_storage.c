/* Native adapter for the existing Control Panel INI directory. Thread only.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/files.h>
#include <ciuki/joystick_settings.h>
static struct vfs *settings_vfs;
static struct vfs_table table;
static int open_table(void)
{
    if (!settings_vfs || !settings_vfs->volumes[2]) return -ENODEV;
    return px_error(vfs_table_init(settings_vfs,&table,2));
}
static int read_ini(void *context, const char *name, void *data, size_t capacity, size_t *done)
{
    (void)context;
    int e=open_table(); if (e) return e;
    char path[160]; ksnprintf(path,sizeof(path),"C:\\SYSTEM\\SETTINGS\\%s",name);
    int fd=vfs_open(&table,path,VFS_READ,VFS_DENY_WRITE,0);
    if (fd<0) { vfs_table_destroy(&table); return px_error(fd); }
    e=vfs_read(&table,fd,data,capacity,done);
    if (!e) {
        uint8_t extra; size_t n=0;
        e=vfs_read(&table,fd,&extra,1,&n); if (!e && n) e=-FS_EIO;
    }
    int closed=vfs_close(&table,fd); vfs_table_destroy(&table); return px_error(e ? e : closed);
}
static int write_ini(void *context, const char *name, const void *data, size_t size)
{
    (void)context;
    int e=open_table(); if (e) return e;
    e=vfs_mkdir(&table,"C:\\SYSTEM\\SETTINGS",0);
    if (e && e!=-FS_EEXIST) { vfs_table_destroy(&table); return px_error(e); }
    char path[160]; ksnprintf(path,sizeof(path),"C:\\SYSTEM\\SETTINGS\\%s",name);
    int fd=vfs_open(&table,path,VFS_WRITE|VFS_CREATE|VFS_TRUNCATE,VFS_DENY_ALL,0);
    if (fd<0) { vfs_table_destroy(&table); return px_error(fd); }
    size_t n=0; e=vfs_write(&table,fd,data,size,&n);
    if (!e && n!=size) e=-FS_EIO;
    if (!e) e=vfs_commit(&table,fd);
    int closed=vfs_close(&table,fd); vfs_table_destroy(&table); return px_error(e ? e : closed);
}
void joystick_storage_init(struct vfs *vfs)
{ settings_vfs=vfs; }
void joystick_storage_start(void)
{
    const struct joystick_settings_io io={read_ini,write_ini,0};
    joystick_settings_start(&io);
}
