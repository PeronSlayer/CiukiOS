/* Real native INI adapter, injected VFS faults and resource ledger.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "../desktop/replay_base.h"
#include <ciuki/files.h>
#include <ciuki/joystick_settings.h>
static struct vfs fs;
static struct fat_volume volume;
static struct joystick_settings_io io;
static unsigned tables, handles, commits, assertions;
static int table_error, open_error, mkdir_error, read_error, write_error, commit_error, close_error;
static uint8_t bytes[600];
static size_t length, position;
static bool partial;
#define CHECK(c) do { assertions++; if (!(c)) { fprintf(stderr,"FAIL %u: %s\n",__LINE__,#c); abort(); } } while (0)
int px_error(int e) { return e; }
void joystick_settings_start(const struct joystick_settings_io *p) { io=*p; }
int vfs_table_init(struct vfs *v,struct vfs_table *t,unsigned drive)
{ (void)t;CHECK(v==&fs && drive==2 && !tables);if (table_error) return table_error;tables++;return 0; }
void vfs_table_destroy(struct vfs_table *t) { (void)t;CHECK(tables==1 && !handles);tables--; }
int vfs_open(struct vfs_table *t,const char *path,unsigned flags,enum vfs_share share,uint8_t attr)
{
    (void)t;CHECK(tables==1 && !handles && !attr && !strcmp(path,"C:\\SYSTEM\\SETTINGS\\example.ini"));
    if (open_error) return open_error;
    if (flags&VFS_WRITE) { CHECK(flags==(VFS_WRITE|VFS_CREATE|VFS_TRUNCATE) && share==VFS_DENY_ALL);length=0; }
    else CHECK(flags==VFS_READ && share==VFS_DENY_WRITE);
    position=0;handles++;return 7;
}
int vfs_close(struct vfs_table *t,int fd) { (void)t;CHECK(fd==7 && handles==1);handles--;return close_error; }
int vfs_read(struct vfs_table *t,int fd,void *out,size_t n,size_t *done)
{
    (void)t;CHECK(fd==7 && handles==1);if (read_error) return read_error;
    if (n>length-position) n=length-position;
    memcpy(out,bytes+position,n);position+=n;*done=n;return 0;
}
int vfs_write(struct vfs_table *t,int fd,const void *in,size_t n,size_t *done)
{
    (void)t;CHECK(fd==7 && handles==1 && n<=sizeof(bytes));if (write_error) return write_error;
    if (partial && n) n--;
    memcpy(bytes,in,n);length=n;*done=n;return 0;
}
int vfs_commit(struct vfs_table *t,int fd) { (void)t;CHECK(fd==7 && handles==1);commits++;return commit_error; }
int vfs_mkdir(struct vfs_table *t,const char *path,uint8_t attr)
{ (void)t;CHECK(tables==1 && !handles && !attr && !strcmp(path,"C:\\SYSTEM\\SETTINGS"));return mkdir_error; }
int main(void)
{
    fs.volumes[2]=&volume;joystick_storage_init(&fs);joystick_storage_start();
    const char text[]="[Joystick]\nversion=1\n";char out[512];size_t n=0;
    mkdir_error=-FS_EEXIST;
    CHECK(!io.write(io.context,"example.ini",text,sizeof(text)-1) && commits==1);
    CHECK(!io.read(io.context,"example.ini",out,sizeof(out),&n) && n==sizeof(text)-1 && !memcmp(text,out,n));
    length=sizeof(out);CHECK(!io.read(io.context,"example.ini",out,sizeof(out),&n) && n==sizeof(out));
    length=sizeof(out)+1;CHECK(io.read(io.context,"example.ini",out,sizeof(out),&n)==-FS_EIO);
    open_error=-FS_ENOENT;CHECK(io.read(io.context,"example.ini",out,sizeof(out),&n)==-FS_ENOENT);open_error=0;
    read_error=-FS_EIO;CHECK(io.read(io.context,"example.ini",out,sizeof(out),&n)==-FS_EIO);read_error=0;
    close_error=-FS_EIO;CHECK(io.read(io.context,"example.ini",out,sizeof(out),&n)==-FS_EIO);close_error=0;
    mkdir_error=-FS_EROFS;CHECK(io.write(io.context,"example.ini",text,sizeof(text)-1)==-FS_EROFS);mkdir_error=0;
    unsigned old=commits;partial=true;CHECK(io.write(io.context,"example.ini",text,sizeof(text)-1)==-FS_EIO && commits==old);partial=false;
    write_error=-FS_EIO;CHECK(io.write(io.context,"example.ini",text,sizeof(text)-1)==-FS_EIO && commits==old);write_error=0;
    commit_error=-FS_EIO;CHECK(io.write(io.context,"example.ini",text,sizeof(text)-1)==-FS_EIO);commit_error=0;
    close_error=-FS_EIO;CHECK(io.write(io.context,"example.ini",text,sizeof(text)-1)==-FS_EIO);close_error=0;
    open_error=-FS_EACCES;CHECK(io.write(io.context,"example.ini",text,sizeof(text)-1)==-FS_EACCES);open_error=0;
    table_error=-FS_ENOSPC;CHECK(io.read(io.context,"example.ini",out,sizeof(out),&n)==-FS_ENOSPC);table_error=0;
    fs.volumes[2]=0;CHECK(io.write(io.context,"example.ini",text,sizeof(text)-1)==-ENODEV);
    CHECK(!tables && !handles);
    printf("PASS joystick native INI storage: %u assertions; commit, short/oversized I/O, faults and zero handle/table leaks\n",assertions);
    fflush(stdout);return 0;
}
