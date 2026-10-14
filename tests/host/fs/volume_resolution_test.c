/* Production mount/POSIX namespace and desktop libreg transactions. Only the
 * host stdio/syscall boundary is adapted; no path-selection model is faked.
 * SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE 1
#define STORAGE_NAMESPACE_LIBRARY
#include "test_storage_namespace.c"
#include <sys/stat.h>
#undef _SC_ARG_MAX
#undef _SC_OPEN_MAX
#undef _SC_PAGESIZE
#undef _SC_THREAD_KEYS_MAX
#undef _SC_THREAD_STACK_MIN
#include <unistd.h>
#include <errno.h>

int master_preference_save(const char *, unsigned, unsigned, unsigned);
bool supervisor_output(struct task *task,unsigned stream,const void *bytes,uint32_t length)
{ (void)task;(void)stream;(void)bytes;(void)length;CHECK(false);return false; }
void proc_launch_volume_read(uint32_t requested,size_t done) { (void)requested;(void)done; }
void serial_write(const char *s,size_t n) { (void)s;(void)n;CHECK(false); }
void console_write(const char *s,size_t n) { (void)s;(void)n;CHECK(false); }
static struct px_namespace *test_space;
#define NATIVE_FD 10000
static bool native_path(const char *p)
{ return !strncmp(p,"/System",7) || !strncmp(p,"/mnt/",5); }
static int host_result(int r) { if (r<0) { errno=-r; return -1; } return r; }
int __real_open(const char *,int,...);
int __real_close(int);
int __real_stat(const char *,struct stat *);
int __real_mkdir(const char *,mode_t);
int __real_unlink(const char *);
int __real_rename(const char *,const char *);
int __real_fsync(int);
FILE *__real_fopen(const char *,const char *);
int __real_fileno(FILE *);

int __wrap_open(const char *p,int flags,...)
{
    unsigned mode=0;
    if (flags&O_CREAT) { va_list ap;va_start(ap,flags);mode=va_arg(ap,unsigned);va_end(ap); }
    if (!native_path(p)) return __real_open(p,flags,mode);
    int fd=host_result(file_open(&supervisor,p,(uint32_t)flags,mode));
    return fd<0 ? -1 : NATIVE_FD+fd;
}
int __wrap_close(int fd)
{ return fd<NATIVE_FD ? __real_close(fd) : host_result(file_close(&supervisor,fd-NATIVE_FD)); }
int __wrap_fsync(int fd)
{
    if (fd<NATIVE_FD) return __real_fsync(fd);
    struct file_description *d=file_fd(&supervisor,fd-NATIVE_FD); CHECK(d);
    CHECK(d->node->drive==2 && d->node->volume==&storage_get()->volumes[2].fat);
    fs_lock_take(&test_space->vfs->lock); int e=file_sync_locked(d);
    fs_lock_drop(&test_space->vfs->lock); return host_result(e);
}
int __wrap_stat(const char *p,struct stat *out)
{
    if (!native_path(p)) return __real_stat(p,out);
    struct px_path path;struct ciuki_stat st;
    fs_lock_take(&test_space->vfs->lock);
    int e=px_resolve_locked(test_space,supervisor.cwd,p,false,&path);
    if (!e) e=px_stat_locked(path.node,&st);
    fs_lock_drop(&test_space->vfs->lock);
    if (e) return host_result(e);
    memset(out,0,sizeof(*out));out->st_dev=st.st_dev;out->st_mode=st.st_mode;out->st_size=st.st_size;
    return 0;
}
int __wrap_unlink(const char *p)
{
    if (!native_path(p)) return __real_unlink(p);
    fs_lock_take(&test_space->vfs->lock);
    int e=px_remove_locked(test_space,supervisor.cwd,p,false);
    fs_lock_drop(&test_space->vfs->lock);return host_result(e);
}
int __wrap_mkdir(const char *p,mode_t mode)
{
    if (!native_path(p)) return __real_mkdir(p,mode);
    fs_lock_take(&test_space->vfs->lock);
    int e=px_mkdir_locked(test_space,supervisor.cwd,p,(uint32_t)mode);
    fs_lock_drop(&test_space->vfs->lock);return host_result(e);
}
int __wrap_rename(const char *a,const char *b)
{
    if (!native_path(a) && !native_path(b)) return __real_rename(a,b);
    fs_lock_take(&test_space->vfs->lock);
    int e=px_rename_locked(test_space,supervisor.cwd,a,b);
    fs_lock_drop(&test_space->vfs->lock);return host_result(e);
}
static struct stream { FILE *file;int fd; } streams[16];
static ssize_t stream_io(void *cookie,char *buf,size_t n,bool write)
{
    struct stream *s=cookie;size_t done=0;
    struct file_description *d=file_fd(&supervisor,s->fd);CHECK(d);
    CHECK(d->node->drive==2 && d->node->volume==&storage_get()->volumes[2].fat);
    fs_lock_take(&test_space->vfs->lock);
    int e=file_io_locked(d,buf,n,write,false,0,&done);
    fs_lock_drop(&test_space->vfs->lock);return e ? host_result(e) : (ssize_t)done;
}
static ssize_t stream_read(void *s,char *buf,size_t n) { return stream_io(s,buf,n,false); }
static ssize_t stream_write(void *s,const char *buf,size_t n) { return stream_io(s,(char *)buf,n,true); }
static int stream_seek(void *cookie,off64_t *off,int whence)
{
    struct stream *s=cookie;int64_t result=0;
    fs_lock_take(&test_space->vfs->lock);
    int e=file_seek_locked(file_fd(&supervisor,s->fd),*off,(uint32_t)whence,&result);
    fs_lock_drop(&test_space->vfs->lock);*off=result;return host_result(e);
}
static int stream_close(void *cookie)
{
    struct stream *s=cookie;int e=file_close(&supervisor,s->fd);s->file=NULL;return host_result(e);
}
FILE *__wrap_fopen(const char *p,const char *mode)
{
    if (!native_path(p)) return __real_fopen(p,mode);
    CHECK(!strcmp(mode,"rb") || !strcmp(mode,"wb"));
    int flags=mode[0]=='r' ? O_RDONLY : O_WRONLY|O_CREAT|O_TRUNC;
    int fd=host_result(file_open(&supervisor,p,(uint32_t)flags,0666));
    if (fd<0) return NULL;
    for (unsigned i=0;i<ARRAY_SIZE(streams);i++) if (!streams[i].file) {
        struct stream *s=&streams[i];s->fd=fd;
        s->file=fopencookie(s,mode,(cookie_io_functions_t){stream_read,stream_write,stream_seek,stream_close});
        CHECK(s->file);return s->file;
    }
    CHECK(false);return NULL;
}
int __wrap_fileno(FILE *f)
{
    for (unsigned i=0;i<ARRAY_SIZE(streams);i++) if (streams[i].file==f) return NATIVE_FD+streams[i].fd;
    return __real_fileno(f);
}
static unsigned cd_reads,cd_writes,cd_flushes;
static int cd_read(struct blkdev *d,uint64_t lba,uint32_t n,void *buf)
{
    int e=blkdev_range(d,lba,n);if (e) return e;cd_reads+=n;
    return fseek(d->ctx,(long)(lba*2048),SEEK_SET) || fread(buf,2048,n,d->ctx)!=n ? -FS_EIO : 0;
}
static int cd_write(struct blkdev *d,uint64_t lba,uint32_t n,const void *buf)
{ (void)d;(void)lba;(void)n;(void)buf;cd_writes++;return -FS_EROFS; }
static int cd_flush(struct blkdev *d) { (void)d;cd_flushes++;return -FS_EROFS; }
static void system_file(const char *path,const char *text)
{
    int fd=file_open(&supervisor,path,O_CREAT|O_WRONLY,0666);CHECK(fd>=3);
    struct file_description *d=file_fd(&supervisor,fd);size_t done;
    fs_lock_take(&test_space->vfs->lock);
    OK(file_io_locked(d,(void *)text,strlen(text),true,false,0,&done));CHECK(done==strlen(text));
    OK(file_sync_locked(d));fs_lock_drop(&test_space->vfs->lock);OK(file_close(&supervisor,fd));
}
static void system_opens(void)
{
    const char *paths[]={"/System/Registry/System.reg","/Programs/dosbox","/Games/CIUKIGAM.IMG",
                         "/System/cache/wallpaper/frame.bin"};
    unsigned usb_reads=media[2].reads,usb_writes=media[2].writes,usb_flushes=media[2].flushes,optical_reads=cd_reads;
    unsigned writes=media[0].writes;
    int saved=master_preference_save("/System/Registry",37,41,0);
    if (saved) fprintf(stderr,"F1-63 master save: errno=%d system_ro=%u reasons=%u\n",errno,storage_get()->volumes[2].fat.readonly,storage_get()->volumes[2].fat.ro_reasons);
    OK(saved);
    CHECK(media[0].writes>writes);
    for (unsigned i=0;i<ARRAY_SIZE(paths);i++) {
        int fd=file_open(&supervisor,paths[i],O_RDONLY,0);CHECK(fd>=3);
        struct file_description *d=file_fd(&supervisor,fd);
        CHECK(d->node->drive==2 && d->node->volume==&storage_get()->volumes[2].fat && !d->node->ro);
        struct ciuki_stat st;size_t done;char bytes[1024];
        fs_lock_take(&test_space->vfs->lock);OK(px_stat_locked(d->node,&st));CHECK(st.st_dev==3);
        OK(file_io_locked(d,bytes,sizeof(bytes)-1,false,false,0,&done));CHECK(done>0);bytes[done]=0;
        if (!i) CHECK(strstr(bytes,"\"Master\" = 10533")); /* 37 | 41<<8 */
        fs_lock_drop(&test_space->vfs->lock);OK(file_close(&supervisor,fd));
    }
    CHECK(media[2].reads==usb_reads && media[2].writes==usb_writes && media[2].flushes==usb_flushes);
    CHECK(cd_reads==optical_reads && !cd_writes && !cd_flushes);
}
int main(int argc,char **argv)
{
    CHECK(argc==5);for (unsigned i=0;i<3;i++) OK(fake_open(&media[i],argv[i+1]));
    baseline();media[2].undo_enabled=true;
    struct storage *s=namespace_start(&test_space);OK(storage_enable_write(s,2));
    const char *dirs[]={"C:/System/Registry","C:/Programs","C:/Games","C:/System/cache","C:/System/cache/wallpaper"};
    for (unsigned i=0;i<ARRAY_SIZE(dirs);i++) OK(vfs_mkdir(&table,dirs[i],0));
    system_file("/Programs/dosbox","system executable");system_file("/Games/CIUKIGAM.IMG","system games");
    system_file("/System/cache/wallpaper/frame.bin","system wallpaper");
    FILE *iso=fopen(argv[4],"rb");CHECK(iso);CHECK(!fseek(iso,0,SEEK_END));long size=ftell(iso);CHECK(size>0);
    struct blkdev cd={.read=cd_read,.write=cd_write,.flush=cd_flush,.ctx=iso,.capacity=(uint64_t)size/2048,.sector_size=2048,.media_generation=1};
    CHECK(storage_add_ro(s,2,&cd,0)==3);CHECK(s->vfs.ro_volumes[3]==&s->optical[2]);
    OK(vfs_set_drive(&table,3));OK(px_chdir(&supervisor,"/mnt/d"));
    system_opens();
    /* Recognized partition, terminal BPB read fault: retained E: diagnostic,
     * no E: mount. A quarantined device must not redirect system operations. */
    disk_init(2);media[2].fail_read=0;
    CHECK(storage_add_removable(s,&disks[2].dev)==-FS_EIO);disks[2].dev.quarantined=true;
    CHECK(s->volumes[4].drive==4 && s->volumes[4].error==-FS_EIO && !s->volumes[4].present);
    CHECK(!vfs_drive_present(&s->vfs,4) && !s->vfs.drive_flags[4]);
    CHECK(file_open(&supervisor,"/mnt/e/SHORT.BIN",O_RDONLY,0)==-ENOENT);
    CHECK(vfs_open(&table,"E:/SHORT.BIN",VFS_READ,VFS_DENY_NONE,0)==-FS_ENOENT);
    system_opens();
    puts("PASS F1-63 C: registry save/dosbox/games/wallpaper with D: ISO cwd and failed/quarantined E:; no CD/USB I/O");
    /* The next attachment gets F:, and surprise removal expires old handles
     * without publishing a quarantined filesystem for fresh opens. */
    disks[2].dev.quarantined=false;media[2].fail_read=UINT64_MAX;
    OK(storage_add_removable(s,&disks[2].dev));CHECK(s->vfs.volumes[5]);
    int stale=file_open(&supervisor,"/mnt/f/SHORT.BIN",O_RDONLY,0);CHECK(stale>=3);
    OK(px_chdir(&supervisor,"/mnt/f"));system_opens();
    OK(storage_remove_removable(s,&disks[2].dev,true));CHECK(!vfs_drive_present(&s->vfs,5));
    CHECK(file_open(&supervisor,"/mnt/f/SHORT.BIN",O_RDONLY,0)==-ENOENT);
    struct ciuki_stat st;CHECK(px_stat_locked(file_fd(&supervisor,stale)->node,&st)==-EIO);
    system_opens();OK(file_close(&supervisor,stale));OK(px_chdir(&supervisor,"/"));
    puts("PASS F1-63 absolute C: paths with live and expired removable cwd; stale handle EIO, fresh path ENOENT, letters do not change current drive");
    CHECK(table.drive==3 && s->vfs.volumes[2]==&s->volumes[2].fat && s->vfs.ro_volumes[3]==&s->optical[2]);
    for (unsigned i=0;i<ARRAY_SIZE(streams);i++) CHECK(!streams[i].file);
    namespace_stop(s,true);fake_reset(&media[2]);CHECK(!fclose(iso));
    for (unsigned i=0;i<3;i++) fake_close(&media[i]);
    printf("F1-63 VOLUME RESULT checks=%u failures=0 ASan/UBSan=enabled\n",checks);return 0;
}
