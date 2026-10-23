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

/* The DM8 key: 1..15 lowercase ASCII letters/digits/underscore/hyphen. */
static int key_path(char path[80], const char *id, uint16_t vendor, uint16_t device)
{
    if (!id || !*id) return -EINVAL;
    for (unsigned n=0; id[n]; n++) {
        char c=id[n];
        if (n>=15 || !((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-')) return -EINVAL;
    }
    ksnprintf(path,80,"C:/system/settings/accel-%s-%04x-%04x.cfg",id,vendor,device);
    return 0;
}

int display_attempt_marker(struct vfs *vfs, const char *id, uint16_t vendor,
    uint16_t device, const char *legacy_path, bool clear, bool *previous,
    const char *diagnostic)
{
    if (previous) *previous=false;
    if (!id || !*id || (clear && diagnostic) || (!clear && !diagnostic && !previous)) return -EINVAL;
    char path[80];
    if (key_path(path,id,vendor,device)) return -EINVAL;
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

/* A declared attempt file: a literal settings path with at most one "%04x",
 * which expands to the PCI device id. Anything else is refused. */
static int declared_path(char *out, size_t size, const char *pattern, uint16_t device)
{
    static const char prefix[]="C:/system/settings/";
    if (strncmp(pattern,prefix,sizeof(prefix)-1)) return -EINVAL;
    size_t o=0;
    bool expanded=false;
    for (const char *s=pattern; *s;) {
        if (*s=='%') {
            if (expanded || strncmp(s,"%04x",4) || o+4>=size) return -EINVAL;
            ksnprintf(out+o,5,"%04x",device);
            o+=4; s+=4; expanded=true;
            continue;
        }
        if (o+1>=size) return -EINVAL;
        out[o++]=*s++;
    }
    out[o]=0;
    return 0;
}
static int attempt_size(const char *path, uint32_t *size)
{
    struct fat_entry entry;
    int err=vfs_stat(&attempt_table,path,&entry);
    *size=err ? 0 : entry.size;
    return err==-FS_ENOENT ? 0 : err;
}
/* An explicit user retry (Control Panel) for exactly this key: truncate the
 * nonempty attempt files, then consume the request. A cut between the two
 * repeats an idempotent clear on the next boot. Another key is consumed. */
static int retry_request(struct vfs *vfs, const struct display_driver *d,
    const struct pci_func *p, char paths[][80], unsigned count)
{
    uint32_t size;
    int err=attempt_size(DISPLAY_RETRY_PATH,&size);
    if (err || !size) return err;
    char expected[48], request[48];
    int length=ksnprintf(expected,sizeof(expected),"driver=%s\npci=%04x:%04x\n",d->id,p->vendor,p->device);
    bool match=false;
    if (size<sizeof(request)) {
        int fd=vfs_open(&attempt_table,DISPLAY_RETRY_PATH,VFS_READ,VFS_DENY_ALL,0);
        if (fd<0) return fd;
        size_t done=0;
        err=vfs_read(&attempt_table,fd,request,size,&done);
        if (!err && done!=size) err=-FS_EIO;
        int closed=vfs_close(&attempt_table,fd);
        if (!err) err=closed;
        if (err) return err;
        match=length>0 && size==(uint32_t)length && !memcmp(request,expected,size);
    }
    if (vfs->volumes[2]->readonly) return -FS_EROFS;
    unsigned cleared=0;
    for (unsigned i=0; match && i<count; i++) {
        err=attempt_size(paths[i],&size);
        if (!err && size) { err=marker_write(paths[i],0,0,true); cleared++; }
        if (err) return err;
    }
    err=marker_write(DISPLAY_RETRY_PATH,0,0,true);
    if (!err) klog("[accel] retry %s driver=%s pci=%04x:%04x cleared=%u",
        match ? "re-armed" : "ignored",d->id,p->vendor,p->device,cleared);
    return err;
}
int display_attempt_prepare(struct vfs *vfs, const struct display_driver *d,
    const struct pci_func *p, bool *retained)
{
    if (!retained) return -EINVAL;
    *retained=false;
    if (!d || !p) return -EINVAL;
    char paths[1+DISPLAY_ATTEMPT_FILES][80];
    unsigned count=0;
    if (key_path(paths[count++],d->id,p->vendor,p->device)) return -EINVAL;
    for (unsigned i=0; i<DISPLAY_ATTEMPT_FILES; i++) {
        if (!d->attempt_files[i]) continue;
        if (declared_path(paths[count],sizeof(paths[count]),d->attempt_files[i],p->device)) return -EINVAL;
        count++;
    }
    if (!vfs || !vfs->volumes[2] || !vfs->volumes[2]->mounted) return -FS_ENOENT;
    if (attempt_busy) return -EBUSY;
    attempt_busy=true;
    int err=vfs_table_init(vfs,&attempt_table,2);
    if (err) goto done;
    err=retry_request(vfs,d,p,paths,count);
    for (unsigned i=0; !err && i<count; i++) {
        uint32_t size;
        err=attempt_size(paths[i],&size);
        if (!err && size) {
            *retained=true;
            klog("[accel] attempt retained driver=%s pci=%04x:%04x file=%s",d->id,p->vendor,p->device,paths[i]);
            break;
        }
    }
    vfs_table_destroy(&attempt_table);
done:
    attempt_busy=false;
    return err;
}
