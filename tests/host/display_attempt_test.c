/* Production keyed persistence against committed/cache VFS boundaries.
 * Each injected cut discards cache state, exactly like a reboot.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ciuki/kernel.h>
#include <ciuki/display_driver.h>
#include "../../src/kernel/fs/vfs.h"

#define LEGACY "C:/system/settings/accel.cfg"
#define KEY "C:/system/settings/accel-s3savage-5333-8c2e.cfg"
static struct vfs storage;
static struct fat_volume volume;
static struct {
    char path[80], disk[512], cache[512];
    size_t size, cached;
} files[8];
static unsigned count, step, fail_step, commits, migrations, writes, truncates;
static bool opened, table_open, reenter, short_read, short_write;
static int active;
static unsigned access_flags;
static bool fault(void) { return ++step==fail_step; }

void klog(const char *fmt, ...)
{
    assert(strstr(fmt,"marker migration"));
    va_list ap; va_start(ap,fmt);
    assert(!strcmp(va_arg(ap,const char *),"s3savage"));
    assert(va_arg(ap,int)==0x5333 && va_arg(ap,int)==0x8c2e);
    assert(!strcmp(va_arg(ap,const char *),LEGACY));
    va_end(ap); migrations++;
}
static int file(const char *path)
{
    for (unsigned i=0;i<count;i++) if (!strcmp(files[i].path,path)) return (int)i;
    return -1;
}
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned drive)
{
    assert(v==&storage && drive==2 && !table_open);
    if (fault()) return -FS_EIO;
    table_open=true; t->vfs=v; return 0;
}
void vfs_table_destroy(struct vfs_table *t)
{ assert(t->vfs==&storage && table_open && !opened); table_open=false; }
int vfs_stat(struct vfs_table *t,const char *path,struct fat_entry *entry)
{
    assert(t->vfs==&storage && table_open && !opened);
    if (fault()) return -FS_EIO;
    int i=file(path);
    if (i<0) return -FS_ENOENT;
    entry->size=files[i].size; return 0;
}
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share deny,uint8_t attr)
{
    assert(t->vfs==&storage && table_open && !opened && deny==VFS_DENY_ALL && !attr);
    if (fault()) return -FS_EIO;
    active=file(path);
    if (active<0) {
        if (!(flags&VFS_CREATE)) return -FS_ENOENT;
        assert(count<ARRAY_SIZE(files)); active=(int)count++;
        snprintf(files[active].path,sizeof(files[active].path),"%s",path);
    }
    memcpy(files[active].cache,files[active].disk,sizeof(files[active].cache));
    files[active].cached=files[active].size; opened=true; access_flags=flags; return 0;
}
int vfs_read(struct vfs_table *t,int fd,void *buf,size_t size,size_t *done)
{
    assert(t->vfs==&storage && fd==0 && opened && access_flags==VFS_READ && size<=files[active].size);
    if (fault()) return -FS_EIO;
    *done=short_read ? size-1 : size; memcpy(buf,files[active].disk,*done); return 0;
}
int vfs_write(struct vfs_table *t,int fd,const void *buf,size_t size,size_t *done)
{
    assert(t->vfs==&storage && fd==0 && opened && (access_flags&VFS_WRITE));
    assert(!volume.readonly && size<sizeof(files[active].cache));
    if (fault()) return -FS_EIO;
    writes++; *done=short_write ? size-1 : size;
    memcpy(files[active].cache,buf,*done);
    if (*done>files[active].cached) files[active].cached=*done;
    return 0;
}
int vfs_truncate(struct vfs_table *t,int fd,uint64_t size)
{
    assert(t->vfs==&storage && fd==0 && opened && !size && (access_flags&VFS_WRITE));
    assert(!volume.readonly);
    if (fault()) return -FS_EIO;
    /* Migration may remove legacy data only after the complete replacement
     * reached disk. A begin/clear of other keys never touches the legacy. */
    if (!strcmp(files[active].path,LEGACY)) {
        int i=file(KEY); assert(i>=0 && files[i].size>=files[active].size);
        assert(!memcmp(files[i].disk,files[active].disk,files[active].size));
    }
    truncates++; files[active].cached=0; return 0;
}
int vfs_commit(struct vfs_table *t,int fd)
{
    assert(t->vfs==&storage && fd==0 && opened && (access_flags&VFS_WRITE));
    if (reenter) {
        bool previous=true;
        assert(display_attempt_marker(&storage,"other",1,2,NULL,false,&previous,NULL)==-EBUSY && !previous);
    }
    if (fault()) return -FS_EIO;
    memcpy(files[active].disk,files[active].cache,sizeof(files[active].disk));
    files[active].size=files[active].cached; commits++; return 0;
}
int vfs_close(struct vfs_table *t,int fd)
{
    assert(t->vfs==&storage && fd==0 && opened);
    opened=false; return fault() ? -FS_EIO : 0;
}
static void reset(void)
{
    assert(!opened && !table_open);
    memset(files,0,sizeof(files)); count=step=fail_step=commits=migrations=writes=truncates=0;
    reenter=short_read=short_write=false;
    volume=(struct fat_volume){.mounted=true}; memset(&storage,0,sizeof(storage)); storage.volumes[2]=&volume;
}
static void legacy(const char *text)
{
    snprintf(files[0].path,sizeof(files[0].path),"%s",LEGACY);
    snprintf(files[0].disk,sizeof(files[0].disk),"%s",text);
    files[0].size=strlen(text); count=1;
}
static int begin(bool *previous)
{ return display_attempt_marker(&storage,"s3savage",0x5333,0x8c2e,LEGACY,false,previous,NULL); }
static void reboot(void)
{
    assert(!opened && !table_open);
    for (unsigned i=0;i<count;i++) memset(files[i].cache,0,sizeof(files[i].cache));
    fail_step=step=0; short_read=short_write=false;
}
int main(void)
{
    bool previous;
    reset(); reenter=true;
    assert(!begin(&previous) && !previous && commits==1);
    int key=file(KEY); assert(key>=0 && files[key].size==14);
    reboot(); volume.readonly=true;
    assert(!begin(&previous) && previous && commits==1); /* interrupted fresh attempt */
    volume.readonly=false;
    assert(!display_attempt_marker(&storage,"other",0x5333,0x8c2e,NULL,false,&previous,NULL) && !previous);
    assert(!display_attempt_marker(&storage,"s3savage",0x1234,0x8c2e,NULL,false,&previous,NULL) && !previous);
    assert(!display_attempt_marker(&storage,"s3savage",0x5333,0x8a22,NULL,false,&previous,NULL) && !previous);
    assert(count==4 && files[key].size==14);
    assert(!display_attempt_marker(&storage,"s3savage",0x5333,0x8a22,NULL,true,NULL,NULL));
    assert(files[key].size==14); /* clearing another key cannot clear ours */
    const char status[]="accel=attempt\nstatus=deadbeef\n";
    assert(!display_attempt_marker(&storage,"s3savage",0x5333,0x8c2e,NULL,false,NULL,status));
    assert(files[key].size==sizeof(status)-1 && !memcmp(files[key].disk,status,sizeof(status)-1));
    reboot(); assert(!begin(&previous) && previous);
    assert(!display_attempt_marker(&storage,"s3savage",0x5333,0x8c2e,NULL,true,NULL,NULL));
    assert(!begin(&previous) && !previous);

    reset(); legacy(status);
    assert(!begin(&previous) && previous && migrations==1 && commits==2);
    key=file(KEY); assert(key>=0 && !files[0].size);
    assert(!memcmp(files[key].disk,status,sizeof(status)-1));
    assert(strstr(files[key].disk,"migrated=legacy\n"));
    unsigned migration_steps=step;
    reboot(); assert(!begin(&previous) && previous); /* migration never authorizes a retry */
    /* Fail every storage boundary, reboot, and check at least one durable
     * guard remains. In particular, key commit/close precede legacy truncate. */
    for (unsigned cut=1;cut<=migration_steps;cut++) {
        reset(); legacy(status); fail_step=cut;
        assert(begin(&previous)<0);
        key=file(KEY); assert(files[0].size || (key>=0 && files[key].size));
        reboot(); assert(!begin(&previous) && previous);
        assert(!table_open && !opened);
    }
    for (unsigned kind=0;kind<5;kind++) {
        reset(); legacy(status);
        if (kind==0) volume.readonly=true;
        if (kind==1) files[0].size=300; /* bounded migration refuses oversized data */
        if (kind==2) short_read=true;
        if (kind==3) short_write=true;
        if (kind==4) { /* malformed nonempty legacy content still blocks */
            legacy("unknown legacy bytes\n");
            assert(!begin(&previous) && previous && migrations==1); continue;
        }
        assert(begin(&previous)<0 && previous && files[0].size && !migrations && !truncates);
    }
    reset(); legacy(""); assert(!begin(&previous) && !previous && !migrations && files[0].size==0);
    for (unsigned size=240;size<=241;size++) {
        reset(); legacy("interrupted");
        memset(files[0].disk,'x',size); files[0].size=size;
        int err=begin(&previous);
        assert(previous && (size==240 ? !err && !files[0].size : err<0 && files[0].size==241));
    }
    reset(); legacy(status);
    /* Unrelated adapters do not claim the private legacy mapping. */
    assert(!display_attempt_marker(&storage,"other",1,2,NULL,false,&previous,NULL) && !previous && files[0].size);
    assert(!begin(&previous) && previous);
    reset(); volume.readonly=true; assert(begin(&previous)==-FS_EROFS && !previous && !commits);
    reset(); storage.volumes[2]=NULL; assert(begin(&previous)==-FS_ENOENT);
    reset(); volume.mounted=false; assert(begin(&previous)==-FS_ENOENT);
    reset();
    const char *invalid[]={"","S3savage","../x","a/b","0123456789abcdef","bad.id","a b"};
    for (unsigned i=0;i<ARRAY_SIZE(invalid);i++)
        assert(display_attempt_marker(&storage,invalid[i],1,2,NULL,false,&previous,NULL)==-EINVAL);
    assert(!count && !step);
    puts("keyed display markers: driver/vendor/device isolation, interrupted boot, annotation, legacy migration, all migration storage cuts, read-only/oversized/short I/O, re-entry and invalid IDs PASS");
    return 0;
}
