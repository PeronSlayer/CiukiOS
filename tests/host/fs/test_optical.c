/* Real ISO/VFS parser on independently generated media, zero-write counters.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/fs_ro.h>
#include <ciuki/sha256.h>
#include "vfs.h"
#include <stdio.h>
static unsigned failures, writes, flushes, reads;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); failures++; } } while (0)
static int disk_read(struct blkdev *d,uint64_t lba,uint32_t n,void *b)
{
    int e=blkdev_range(d,lba,n); if (e) return e;
    FILE *f=d->ctx; reads++;
    return fseek(f,(long)(lba*d->sector_size),SEEK_SET) || fread(b,d->sector_size,n,f)!=n ? -FS_EIO : 0;
}
static int disk_write(struct blkdev *d,uint64_t lba,uint32_t n,const void *b) { (void)d; (void)lba; (void)n; (void)b; writes++; return -FS_EROFS; }
static int disk_flush(struct blkdev *d) { (void)d; flushes++; return -FS_EROFS; }
static struct vfs vfs;
static struct vfs_table table;
static struct fs_ro_volume volume;
static struct block_cache cache;
static char first[FS_PATH_BYTES];
static int walk(uint64_t dir,const char *prefix,unsigned depth)
{
    CHECK(depth<16); if (depth>=16) return -FS_EIO;
    uint32_t cursor=0; struct fs_ro_node n; int e;
    while (!(e=fs_ro_next(&volume,dir,&cursor,&n))) {
        char path[FS_PATH_BYTES]; snprintf(path,sizeof(path),"%s/%s",prefix,n.name);
        if (n.attr&16) { printf("D\t%s\n",path); e=walk(n.id,path,depth+1); if (e) return e; }
        else {
            char full[FS_PATH_BYTES]; snprintf(full,sizeof(full),"D:%s",path);
            if (!first[0]) memcpy(first,full,strlen(full)+1);
            int h=vfs_open(&table,full,VFS_READ,VFS_DENY_NONE,0); CHECK(h>=0); if (h<0) return h;
            uint8_t data[517],hash[32]; char hex[65]; size_t count; uint64_t size=0;
            struct sha256_ctx ctx; sha256_init(&ctx);
            for (;;) { e=vfs_read(&table,h,data,sizeof(data),&count); if (e || !count) break; sha256_update(&ctx,data,count); size+=count; }
            CHECK(!e && size==n.size); CHECK(!vfs_commit(&table,h)); CHECK(!vfs_close(&table,h));
            sha256_final(&ctx,hash); sha256_hex(hash,hex); printf("F\t%s\t%llu\t%s\n",path,(unsigned long long)size,hex);
            struct fs_ro_node alias; CHECK(!fs_ro_lookup(&volume,dir,n.alias,&alias) && alias.id==n.id);
        }
    }
    return e==-FS_ENOENT ? 0 : e;
}
int main(int argc,char **argv)
{
    CHECK(argc==2); if (argc!=2) return 2;
    FILE *f=fopen(argv[1],"rb"); CHECK(f); if (!f) return 2;
    fseek(f,0,SEEK_END); long size=ftell(f);
    struct blkdev dev={.read=disk_read,.write=disk_write,.flush=disk_flush,.sector_size=2048,.capacity=(uint64_t)size/2048,.media_generation=1,.ctx=f};
    CHECK(!cache_init(&cache,128*1024)); vfs_init(&vfs);
    int e=fs_ro_mount(&volume,&iso9660_ops,&cache,&dev,0,dev.capacity);
    if (e) { printf("REFUSED\t%d\n",e); CHECK(!writes && !flushes); cache_destroy(&cache); vfs_destroy(&vfs); fclose(f); return failures ? 1 : 0; }
    CHECK(!vfs_attach_ro(&vfs,3,&volume)); CHECK(!vfs_table_init(&vfs,&table,2));
    struct fs_ro_node root; CHECK(!volume.ops->root(&volume,&root)); e=walk(root.id,"",0);
    if (e) printf("REFUSED\t%d\n",e);
    else {
        CHECK(first[0]); int h=vfs_open(&table,first,VFS_READ,VFS_DENY_NONE,0); CHECK(h>=0);
        struct fat_times times={0}; size_t n=0; uint8_t b[1]; uint64_t pos;
        CHECK(vfs_open(&table,first,VFS_WRITE,VFS_DENY_NONE,0)==-FS_EROFS);
        CHECK(vfs_open(&table,"D:/missing",VFS_READ|VFS_CREATE,VFS_DENY_NONE,0)==-FS_EROFS);
        CHECK(vfs_open(&table,first,VFS_WRITE|VFS_TRUNCATE,VFS_DENY_NONE,0)==-FS_EROFS);
        CHECK(vfs_write(&table,h,b,0,&n)==-FS_EROFS && !n);
        CHECK(vfs_truncate(&table,h,0)==-FS_EROFS);
        CHECK(vfs_attrib(&table,first,0)==-FS_EROFS); CHECK(vfs_times(&table,first,&times)==-FS_EROFS);
        CHECK(vfs_delete(&table,first)==-FS_EROFS); CHECK(vfs_rmdir(&table,"D:/")==-FS_EROFS);
        CHECK(vfs_rename(&table,first,first)==-FS_EROFS); CHECK(vfs_mkdir(&table,"D:/missing",0)==-FS_EROFS);
        uint64_t free; uint32_t block; CHECK(!vfs_free_space(&table,3,&free,&block) && !free && block==2048);
        struct vfs_find find; struct fat_entry ent;
        CHECK(!vfs_find_first(&table,"D:/*",16,&find,&ent));
        CHECK(!vfs_chdir(&table,"D:/"));
        dev.media_generation++;
        CHECK(vfs_read(&table,h,b,1,&n)==-FS_EIO && !n && !vfs_drive_present(&vfs,3));
        CHECK(vfs_seek(&table,h,0,VFS_SEEK_SET,&pos)==-FS_EIO);
        CHECK(vfs_commit(&table,h)==-FS_EIO); CHECK(vfs_find_next(&table,&find,&ent)==-FS_EIO);
        CHECK(!strcmp(table.cwd[3],"/"));
        CHECK(!fs_ro_mount(&volume,&iso9660_ops,&cache,&dev,0,dev.capacity)); CHECK(!vfs_attach_ro(&vfs,3,&volume));
        CHECK(vfs_read(&table,h,b,1,&n)==-FS_EIO); CHECK(!vfs_close(&table,h));
    }
    vfs_table_destroy(&table); CHECK(!vfs_detach(&vfs,3)); vfs_destroy(&vfs); cache_destroy(&cache);
    CHECK(!writes && !flushes); printf("COUNTERS\twrites=%u\tflushes=%u\treads=%u\n",writes,flushes,reads); fclose(f);
    return failures ? 1 : 0;
}
