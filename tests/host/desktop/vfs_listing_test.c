/* SPDX-License-Identifier: MIT
 * Production POSIX adapter with Ciuki dirent metadata and counted syscalls. */
#define _DEFAULT_SOURCE 1
#include "../../../apps/files/model.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static DIR stream;
static struct dirent record;
static unsigned stats, opens, reads, closes, emitted, classified, directories;
DIR *test_opendir(const char *path) { assert(!strcmp(path,"/media")); stream.at=0; return &stream; }
struct dirent *test_readdir(DIR *dir)
{
    assert(dir==&stream);
    if (dir->at==132) return NULL;
    unsigned i=dir->at++; memset(&record,0,sizeof(record));
    if (i) snprintf(record.d_name,sizeof(record.d_name),"file%u",i);
    else strcpy(record.d_name,"Folder");
    record.d_type=i ? DT_REG : DT_DIR; record.d_ino=i+1;
    record.d_size=i ? 100 : 0; record.d_mtime=1792100000;
    return &record;
}
int test_closedir(DIR *dir) { assert(dir==&stream); return 0; }
static int listing_stat(const char *path,struct stat *out)
{
    stats++; memset(out,0,sizeof(*out));
    out->st_mode=!strcmp(path,"/media/Folder") ? S_IFDIR|0755 : S_IFREG|0644;
    out->st_size=S_ISDIR(out->st_mode) ? 0 : 100;
    out->st_mtime=1792100000; return 0;
}
static int listing_open(const char *path,int flags,...)
{ assert(!strncmp(path,"/media/file",11) && flags==O_RDONLY); opens++; return 7; }
static ssize_t listing_read(int fd,void *out,size_t bytes)
{
    assert(fd==7 && bytes==20); reads++;
    unsigned char h[20]={0x7f,'E','L','F',1,1,1}; h[16]=2; h[18]=3;
    memcpy(out,h,sizeof(h)); return sizeof(h);
}
static int listing_close(int fd) { assert(fd==7); closes++; return 0; }
#define stat(path, out) listing_stat(path, out)
#define open listing_open
#define read listing_read
#define close listing_close
#include "../../../apps/files/vfs.c"
#undef stat
#undef open
#undef read
#undef close
static int collect_metadata(const char *name,const struct fm_meta *meta,void *ctx)
{
    (void)ctx; emitted++;
    assert(meta->modified==1792100000);
    if (!strcmp(name,"Folder")) { assert(meta->type==FM_DIR && !meta->size); directories++; }
    else assert(meta->type==FM_FILE && meta->size==100);
    classified+=!!meta->exec; return 0;
}
int main(void)
{
    const struct fm_vfs *fs=fm_native_vfs();
    assert(!fs->list(fs->ctx,"/media",FM_LIST_NAMES_ONLY,collect_metadata,NULL));
    assert(emitted==132 && directories==1 && !stats && !opens && !reads && !closes && !classified);
    emitted=directories=0;
    assert(!fs->list(fs->ctx,"/media",FM_LIST_SNIFF,collect_metadata,NULL));
    assert(emitted==132 && directories==1 && stats==132 && opens==128 && reads==128 && closes==128 && classified==128);
    puts("PASS F2-79 native VFS: dirent type/size/mtime, cheap listing zero stat/open/read, explicit SNIFF mode retains stat and 128 ELF sniffs");
    return 0;
}
