/* SPDX-License-Identifier: MIT
 * sec-own: native-file binding of struct acct_tree_io (production.h) for the
 * sealed ACCOUNT image: absolute paths on the system volume, opened as the
 * service's own SYSTEM credential. rename never replaces (FAT has no link();
 * ACCOUNT is the single writer of every tree it renames).
 */
#define _POSIX_C_SOURCE 200809L
#include "production.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int valid(const char *path)
{ return path && path[0]=='/' && strlen(path)<520 && !strstr(path,"/../") && !strstr(path,"/./"); }
static int t_stat(void *ctx, const char *path, int *directory, uint64_t *size)
{
    (void)ctx; struct stat st;
    if (!valid(path)) return -EINVAL;
    if (stat(path,&st)) return errno==ENOTDIR?-ENOENT:-errno;
    *directory=S_ISDIR(st.st_mode)?1:0; *size=S_ISDIR(st.st_mode)?0:(uint64_t)st.st_size;
    return 0;
}
static int t_mkdir(void *ctx, const char *path)
{
    (void)ctx;
    if (!valid(path)) return -EINVAL;
    return mkdir(path,0700)?-errno:0;
}
static int t_list(void *ctx, const char *path, int (*each)(void *, const struct acct_tree_entry *), void *arg)
{
    (void)ctx;
    if (!valid(path)) return -EINVAL;
    DIR *d=opendir(path); if (!d) return -errno;
    int e=0; struct dirent *entry;
    struct acct_tree_entry out;
    errno=0;
    while (!e && (entry=readdir(d))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        size_t n=strlen(entry->d_name), base=strlen(path);
        if (n>=sizeof(out.name) || base+n+2>520) { e=-ENAMETOOLONG; break; }
        char full[520]; memcpy(full,path,base); full[base]='/'; memcpy(full+base+1,entry->d_name,n+1);
        struct stat st; if (stat(full,&st)) { e=-errno; break; }
        memset(&out,0,sizeof(out)); memcpy(out.name,entry->d_name,n+1);
        out.directory=S_ISDIR(st.st_mode)?1:0; out.size=out.directory?0:(uint64_t)st.st_size;
        e=each(arg,&out);
        errno=0;
    }
    if (!e && errno) e=-errno;
    closedir(d);
    return e;
}
static int t_read(void *ctx, const char *path, uint64_t offset, void *buffer, size_t bytes, size_t *done)
{
    (void)ctx; *done=0;
    if (!valid(path)) return -EINVAL;
    int fd=open(path,O_RDONLY|O_CLOEXEC); if (fd<0) return -errno;
    int e=0;
    if (lseek(fd,(off_t)offset,SEEK_SET)<0) e=-errno;
    while (!e && *done<bytes) {
        ssize_t n=read(fd,(char *)buffer+*done,bytes-*done);
        if (n<0) { if (errno==EINTR) continue; e=-errno; break; }
        if (!n) break;
        *done+=(size_t)n;
    }
    if (close(fd) && !e) e=-errno;
    return e;
}
static int t_write(void *ctx, const char *path, uint64_t offset, const void *data, size_t bytes)
{
    (void)ctx;
    if (!valid(path)) return -EINVAL;
    int fd=open(path,offset?O_WRONLY|O_CLOEXEC:O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
    if (fd<0) return -errno;
    int e=0;
    if (offset && lseek(fd,(off_t)offset,SEEK_SET)<0) e=-errno;
    for (size_t at=0;!e && at<bytes;) {
        ssize_t n=write(fd,(const char *)data+at,bytes-at);
        if (n<0) { if (errno==EINTR) continue; e=-errno; break; }
        if (!n) { e=-ENOSPC; break; }
        at+=(size_t)n;
    }
    if (close(fd) && !e) e=-errno;
    return e;
}
static int t_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx; struct stat st;
    if (!valid(from) || !valid(to)) return -EINVAL;
    if (!stat(to,&st)) return -EEXIST;
    if (errno!=ENOENT) return -errno;
    return rename(from,to)?-errno:0;
}
static int t_remove(void *ctx, const char *path)
{ (void)ctx; if (!valid(path)) return -EINVAL; return unlink(path)?-errno:0; }
static int t_rmdir(void *ctx, const char *path)
{ (void)ctx; if (!valid(path)) return -EINVAL; return rmdir(path)?-errno:0; }
static int t_sync(void *ctx, const char *path)
{
    (void)ctx; struct stat st;
    if (!valid(path)) return -EINVAL;
    if (stat(path,&st)) return -errno;
    int fd=open(path,S_ISDIR(st.st_mode)?O_RDONLY|O_DIRECTORY|O_CLOEXEC:O_RDWR|O_CLOEXEC);
    if (fd<0) return -errno;
    int e=fsync(fd)?-errno:0;
    if (close(fd) && !e) e=-errno;
    return e;
}
int acct_tree_posix(struct acct_tree_io *out)
{
    if (!out) return -EINVAL;
    *out=(struct acct_tree_io){ .ctx=NULL, .inspect=t_stat, .mkdir=t_mkdir, .list=t_list, .read=t_read,
        .write=t_write, .rename=t_rename, .remove=t_remove, .rmdir=t_rmdir, .sync=t_sync };
    return 0;
}
