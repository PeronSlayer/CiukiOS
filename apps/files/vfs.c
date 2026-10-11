/* SPDX-License-Identifier: MIT */
#include "model.h"
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static int canonical(void *ctx,const char *path,char *out)
{ (void)ctx; char old[FM_PATH]; if (!getcwd(old,sizeof(old))) return -errno; if (chdir(path)) return -errno;
    int r=getcwd(out,FM_PATH)?0:-errno; int saved=chdir(old); if (!r && saved) r=-errno; return r; }
static int metadata_stat(void *ctx,const char *path,struct fm_meta *m)
{
    (void)ctx; struct stat s; if (stat(path,&s)) return -errno;
    *m=(struct fm_meta){S_ISDIR(s.st_mode)?FM_DIR:S_ISREG(s.st_mode)?FM_FILE:FM_OTHER,(uint64_t)s.st_size,(uint64_t)s.st_ino,(uint32_t)s.st_dev,(uint32_t)s.st_mode,(int64_t)s.st_mtime}; return 0;
}
static int list(void *ctx,const char *path,fm_emit emit,void *arg)
{
    DIR *dir=opendir(path); if (!dir) return -errno;
    int r=0; struct dirent *entry;
    for (;;) {
        errno=0; entry=readdir(dir); if (!entry) { if (errno) r=-errno; break; }
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        char full[FM_PATH]; struct fm_meta meta;
        r=fm_join(full,path,entry->d_name); if (!r) r=metadata_stat(ctx,full,&meta); if (!r) r=emit(entry->d_name,&meta,arg); if (r) break;
    }
    if (closedir(dir) && !r) r=-errno; return r;
}
static int make_dir(void *ctx,const char *p) { (void)ctx; return mkdir(p,0777)?-errno:0; }
static int move(void *ctx,const char *a,const char *b) { (void)ctx; return rename(a,b)?-errno:0; }
static int remove_file(void *ctx,const char *p) { (void)ctx; return unlink(p)?-errno:0; }
static int remove_dir(void *ctx,const char *p) { (void)ctx; return rmdir(p)?-errno:0; }
static int open_file(void *ctx,const char *p,int create) { (void)ctx; int fd=open(p,create?O_WRONLY|O_CREAT|O_EXCL:O_RDONLY,0666); return fd<0?-errno:fd; }
static int read_file(void *ctx,int fd,void *p,unsigned n) { (void)ctx; int r=(int)read(fd,p,n); return r<0?-errno:r; }
static int write_file(void *ctx,int fd,const void *p,unsigned n) { (void)ctx; int r=(int)write(fd,p,n); return r<0?-errno:r; }
static int sync_file(void *ctx,int fd) { (void)ctx; return fsync(fd)?-errno:0; }
static int close_file(void *ctx,int fd) { (void)ctx; return close(fd)?-errno:0; }
const struct fm_vfs *fm_native_vfs(void)
{
    static const struct fm_vfs fs={NULL,canonical,list,metadata_stat,make_dir,move,remove_file,remove_dir,open_file,read_file,write_file,sync_file,close_file,NULL}; return &fs;
}
