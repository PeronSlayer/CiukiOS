/* SPDX-License-Identifier: MIT
 * Protected storage binding of acct_io over the native file API (newlib on
 * the target, POSIX on the host harness). Names are the service's constants:
 * a plain name below the SYSTEM-only base directory, or an absolute path
 * strictly below it (the OWNER staging tree /System/Security/Staging/...),
 * whose missing directories are created on write. Every component is
 * printable, separator-free and never starts with a dot. write may leave a
 * partial file; flush commits one file's data; barrier commits the base
 * directory and every directory changed since the last barrier; rename
 * never replaces an existing destination.
 */
#define _POSIX_C_SOURCE 200809L
#include "production.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int component_valid(const char *p, size_t n)
{
    if (!n || p[0]=='.') return 0;
    for (size_t i=0;i<n;i++)
        if ((unsigned char)p[i]<0x20 || (unsigned char)p[i]>0x7e || p[i]=='\\' || p[i]==':') return 0;
    return 1;
}
static int path_of(const struct acct_posix_io *s, const char *name, char out[ACCT_IO_PATH])
{
    if (!name || strlen(name)>=ACCT_IO_PATH) return -EINVAL;
    const char *rest=name;
    if (name[0]=='/') {
        size_t base=strlen(s->base);
        if (strncmp(name,s->base,base) || name[base]!='/') return -EINVAL;
        rest=name+base+1;
    } else if (strchr(name,'/')) return -EINVAL;
    for (const char *p=rest;;) {
        const char *slash=strchr(p,'/');
        size_t n=slash ? (size_t)(slash-p) : strlen(p);
        if (!component_valid(p,n)) return -EINVAL;
        if (!slash) break;
        p=slash+1;
    }
    int n=name[0]=='/' ? snprintf(out,ACCT_IO_PATH,"%s",name) : snprintf(out,ACCT_IO_PATH,"%s/%s",s->base,name);
    return n<0 || n>=ACCT_IO_PATH ? -ENAMETOOLONG : 0;
}
/* Records the directory holding path for the next barrier. */
static void mark_dirty(struct acct_posix_io *s, const char *path)
{
    const char *slash=strrchr(path,'/');
    size_t n=slash ? (size_t)(slash-path) : 0;
    if (n==strlen(s->base) && !strncmp(path,s->base,n)) return; /* base: always committed */
    for (unsigned i=0;i<s->dirty_count;i++)
        if (strlen(s->dirty[i])==n && !strncmp(s->dirty[i],path,n)) return;
    if (s->dirty_count==ACCT_IO_DIRTY) { s->overflow=1; return; }
    memcpy(s->dirty[s->dirty_count],path,n); s->dirty[s->dirty_count][n]=0; s->dirty_count++;
}
static int sync_directory(const char *path)
{
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd<0) return -errno;
    int e=fsync(fd) ? -errno : 0;
    if (close(fd) && !e) e=-errno;
    return e;
}
/* Creates the missing directories between the base and path's file. */
static int make_parents(struct acct_posix_io *s, const char *path)
{
    char dir[ACCT_IO_PATH]; size_t base=strlen(s->base);
    strcpy(dir,path);
    for (char *p=dir+base+1;(p=strchr(p,'/'));p++) {
        *p=0;
        if (mkdir(dir,0700)) { if (errno!=EEXIST) return -errno; }
        else mark_dirty(s,dir);
        *p='/';
    }
    return 0;
}
static int read_file(void *ctx, const char *name, void *buffer, size_t capacity, size_t *bytes)
{
    char path[ACCT_IO_PATH]; int e=path_of(ctx,name,path); if (e) return e;
    int fd=open(path,O_RDONLY|O_CLOEXEC);
    if (fd<0) return -errno;
    size_t total=0;
    for (;;) {
        if (total==capacity) {
            char probe; ssize_t extra=read(fd,&probe,1);
            if (extra>0) e=-EIO; /* larger than any valid record: corrupt */
            else if (extra<0) e=-errno;
            break;
        }
        ssize_t n=read(fd,(char *)buffer+total,capacity-total);
        if (n<0) { e=-errno; break; }
        if (!n) break;
        total+=(size_t)n;
    }
    close(fd);
    if (!e) *bytes=total;
    return e;
}
static int write_file(void *ctx, const char *name, const void *data, size_t bytes)
{
    struct acct_posix_io *s=ctx;
    char path[ACCT_IO_PATH]; int e=path_of(s,name,path); if (e) return e;
    if (name[0]=='/' && (e=make_parents(s,path))) return e;
    int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
    if (fd<0) return -errno;
    mark_dirty(s,path);
    size_t done=0;
    while (done<bytes) {
        ssize_t n=write(fd,(const char *)data+done,bytes-done);
        if (n<0) { e=-errno; break; }
        if (!n) { e=-ENOSPC; break; }
        done+=(size_t)n;
    }
    if (close(fd) && !e) e=-errno;
    return e;
}
static int flush_file(void *ctx, const char *name)
{
    char path[ACCT_IO_PATH]; int e=path_of(ctx,name,path); if (e) return e;
    int fd=open(path,O_RDWR|O_CLOEXEC);
    if (fd<0) return -errno;
    if (fsync(fd)) e=-errno;
    if (close(fd) && !e) e=-errno;
    return e;
}
static int barrier(void *ctx)
{
    struct acct_posix_io *s=ctx;
    int e=0;
    for (unsigned i=0;i<s->dirty_count && !e;i++) e=sync_directory(s->dirty[i]);
    if (!e) e=sync_directory(s->base);
    if (e) return e;
    /* More changed directories than slots: report it as a fault rather than
     * claim a barrier that did not cover them (never expected). */
    e=s->overflow ? -EIO : 0;
    s->dirty_count=0; s->overflow=0;
    return e;
}
static int rename_file(void *ctx, const char *from, const char *to)
{
    struct acct_posix_io *s=ctx;
    char a[ACCT_IO_PATH], b[ACCT_IO_PATH]; struct stat st;
    int e=path_of(s,from,a); if (e) return e;
    e=path_of(s,to,b); if (e) return e;
    /* FAT has no link(); this single-writer SYSTEM-only directory makes the
     * existence check and the rename one serialized step. */
    if (!stat(b,&st)) return -EEXIST;
    if (errno!=ENOENT) return -errno;
    if (stat(a,&st)) return -errno;
    if (rename(a,b)) return -errno;
    mark_dirty(s,a); mark_dirty(s,b);
    return 0;
}
static int remove_file(void *ctx, const char *name)
{
    struct acct_posix_io *s=ctx;
    char path[ACCT_IO_PATH]; int e=path_of(s,name,path); if (e) return e;
    if (unlink(path)) return -errno;
    mark_dirty(s,path);
    return 0;
}
int acct_io_posix(struct acct_posix_io *s, const char *base, struct acct_io *out)
{
    if (!s || !base || !out || base[0]!='/' || strlen(base)>=sizeof(s->base) || base[strlen(base)-1]=='/') return -EINVAL;
    memset(s,0,sizeof(*s)); strcpy(s->base,base);
    *out=(struct acct_io){ .ctx=s, .real=1, .read=read_file, .write=write_file, .flush=flush_file,
        .barrier=barrier, .rename=rename_file, .remove=remove_file };
    return 0;
}
