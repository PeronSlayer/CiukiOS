/* Private file-backed FAT32 power-cut fixture. The live system mount is
 * never unmounted, dirtied by hand, or repaired by this diagnostic.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/storage.h>
#include <ciuki/hardware_check.h>
#define SCRATCH_PATH "C:/SYSTEM/LOGS/HWCHK.TMP"
#define REPAIR_SECTORS 70000u
#define REPAIR_FAT 540u
#define REPAIR_DATA (32u+2u*REPAIR_FAT)
#define REPAIR_BACKING ((REPAIR_DATA+4u)*512u)
static struct vfs_table scratch_table;
static struct block_cache repair_cache;
static struct fat_volume repair_volume;
static struct fat_entry repair_entry;
static uint8_t scratch_sector[512], check_sector[512];
struct check_file { struct vfs_table *table; int fd; uint32_t bytes; };
static int scratch_begin(struct storage *s, struct check_file *f, unsigned bytes)
{
    if (!s || !s->ready || !s->vfs.volumes[2]) return -FS_ENOENT;
    int e=vfs_table_init(&s->vfs,&scratch_table,2);
    if (e) return e;
    int fd=vfs_open(&scratch_table,SCRATCH_PATH,VFS_READ|VFS_WRITE|VFS_CREATE|VFS_EXCLUSIVE,VFS_DENY_ALL,0);
    if (fd<0) { vfs_table_destroy(&scratch_table); return fd; }
    *f=(struct check_file){&scratch_table,fd,bytes}; return 0;
}
static int scratch_end(struct check_file *f)
{
    int e=vfs_close(f->table,f->fd);
    int d=vfs_delete(f->table,SCRATCH_PATH);
    vfs_table_destroy(f->table);
    return e ? e : d;
}
/* The outer file is preallocated and exclusively open. Reserved data writes
 * avoid COW of a half-megabyte container for each inner metadata sector. */
static int backing_write(struct blkdev *b,uint64_t lba,uint32_t n,const void *buf)
{
    struct check_file *f=b->ctx;
    if (lba>=f->bytes/512 || n>f->bytes/512-lba) return -FS_EIO;
    struct vfs *v=f->table->vfs;
    fs_lock_take(&v->lock);
    struct vfs_node *node=f->table->handles[f->fd]->node;
    size_t done=0;
    int e=fat_write_reserved(node->volume,&node->entry,f->bytes,lba*512,buf,n*512,&done);
    fs_lock_drop(&v->lock);
    return e ? e : done==n*512 ? 0 : -FS_EIO;
}
static int backing_read(struct blkdev *b,uint64_t lba,uint32_t n,void *buf)
{
    struct check_file *f=b->ctx;
    if (blkdev_range(b,lba,n)) return -FS_EINVAL;
    if (lba>=f->bytes/512) { memset(buf,0,n*512); return 0; }
    if (n>f->bytes/512-lba) return -FS_EIO;
    uint64_t position;
    int e=vfs_seek(f->table,f->fd,lba*512,VFS_SEEK_SET,&position);
    size_t done=0;
    if (!e) e=vfs_read(f->table,f->fd,buf,n*512,&done);
    return e ? e : done==n*512 ? 0 : -FS_EIO;
}
static int backing_flush(struct blkdev *b)
{ struct check_file *f=b->ctx; return vfs_commit(f->table,f->fd); }
int hardware_check_ata(struct storage *s, struct hw_check_result *out)
{
    struct check_file file;
    int e=scratch_begin(s,&file,512);
    if (e) { out->status=e==-FS_ENOENT ? HW_SKIP : HW_FAIL; goto result; }
    for (unsigned i=0;i<512;i++) scratch_sector[i]=(uint8_t)(i*37u+11u);
    size_t done=0;
    e=vfs_write(file.table,file.fd,scratch_sector,512,&done);
    if (!e && done!=512) e=-FS_EIO;
    if (!e) e=vfs_commit(file.table,file.fd);
    if (!e) {
        /* A physical read, bypassing the block cache used for the write. */
        struct vfs *v=file.table->vfs;
        fs_lock_take(&v->lock);
        struct vfs_node *node=file.table->handles[file.fd]->node;
        struct fat_volume *vol=node->volume;
        uint64_t lba=vol->start+vol->data_sector+(uint64_t)(node->entry.first-2)*vol->spc;
        e=vol->dev->read(vol->dev,lba,1,check_sector);
        fs_lock_drop(&v->lock);
        if (!e && memcmp(scratch_sector,check_sector,512)) e=-FS_EIO;
    }
    { int clean=scratch_end(&file); if (!e) e=clean; }
    out->status=e ? HW_FAIL : HW_PASS;
result:
    ksnprintf(out->detail,sizeof(out->detail),"bytes=512 physical_readback error=%d",e);
    return e;
}
int hardware_check_repair(struct storage *s, struct hw_check_result *out)
{
    struct check_file file;
    int e=scratch_begin(s,&file,REPAIR_BACKING);
    bool recovered=false, writable=false; uint32_t reasons=0, mount_ms=0, repair_ms=0;
    if (e) { out->status=e==-FS_ENOENT ? HW_SKIP : HW_FAIL; goto result; }
    uint8_t *image=fs_contig_alloc(REPAIR_BACKING);
    if (!image) { e=-FS_ENOMEM; goto close_file; }
    uint8_t *b=image;
    b[0]=0xeb; b[1]=0x58; b[2]=0x90; memcpy(b+3,"CIUKICHK",8);
    fs_wr16(b+11,512); b[13]=1; fs_wr16(b+14,32); b[16]=2; b[21]=0xf8;
    fs_wr32(b+32,REPAIR_SECTORS); fs_wr32(b+36,REPAIR_FAT); fs_wr32(b+44,2);
    fs_wr16(b+48,1); fs_wr16(b+50,6); fs_wr16(b+510,0xaa55);
    memcpy(image+6*512,b,512);
    fs_wr32(image+512,0x41615252); fs_wr32(image+512+484,0x61417272);
    fs_wr32(image+512+488,0xffffffff); fs_wr32(image+512+492,0xffffffff);
    fs_wr32(image+512+508,0xaa550000); memcpy(image+7*512,image+512,512);
    for (unsigned copy=0;copy<2;copy++) {
        uint8_t *fat=image+(32+copy*REPAIR_FAT)*512;
        fs_wr32(fat,0x0ffffff8); fs_wr32(fat+4,0x0fffffff); fs_wr32(fat+8,0x0fffffff);
    }
    size_t done=0;
    e=vfs_write(file.table,file.fd,image,REPAIR_BACKING,&done);
    if (!e && done!=REPAIR_BACKING) e=-FS_EIO;
    fs_contig_free(image,REPAIR_BACKING);
    if (!e) e=vfs_commit(file.table,file.fd);
    if (e) goto close_file;
    struct blkdev disk={.read=backing_read,.write=backing_write,.flush=backing_flush,
        .capacity=REPAIR_SECTORS,.sector_size=512,.write_cache_state=BLKDEV_CACHE_ENABLED,.ctx=&file};
    e=cache_init(&repair_cache,2*1024*1024);
    if (e) goto close_file;
    e=fat_mount(&repair_volume,&repair_cache,&disk,0,disk.capacity,true);
    if (!e) e=fat_create(&repair_volume,repair_volume.root,"CRASH.DAT",0,&repair_entry);
    memset(scratch_sector,0x35,512);
    if (!e) e=fat_write(&repair_volume,&repair_entry,0,scratch_sector,128,&done);
    if (!e) e=fat_commit(&repair_volume);
    /* Stable data, unpublished extension/EOF, no unmount/clean mark. A
     * discarded cache models power loss without touching the live C: FAT. */
    memset(scratch_sector+128,0x6a,384);
    if (!e) e=fat_write_reserved(&repair_volume,&repair_entry,512,0,scratch_sector,512,&done);
    cache_destroy(&repair_cache);
    if (!e) e=cache_init(&repair_cache,2*1024*1024);
    else goto close_file;
    if (e) goto close_file;
    e=fat_mount(&repair_volume,&repair_cache,&disk,0,disk.capacity,true);
    recovered=repair_volume.dirty_recovered; writable=repair_volume.mounted&&!repair_volume.readonly;
    mount_ms=repair_volume.mount_ms; repair_ms=repair_volume.repair_ms;
    reasons=repair_volume.ro_reasons;
    if (!e) e=fat_lookup(&repair_volume,repair_volume.root,"CRASH.DAT",&repair_entry);
    if (!e) e=fat_read(&repair_volume,&repair_entry,0,check_sector,512,&done);
    if (!e && (!recovered || !writable || repair_entry.size!=128 || done!=128 ||
        memcmp(check_sector,scratch_sector,128))) e=-FS_EUCLEAN;
    if (!e) e=fat_unmount(&repair_volume);
    cache_destroy(&repair_cache);
close_file:
    { int clean=scratch_end(&file); if (!e) e=clean; }
    out->status=e ? HW_FAIL : HW_PASS;
result:
    ksnprintf(out->detail,sizeof(out->detail),"FAT32 no_publish repaired=%u writable=%u reasons=%x mount_ms=%u repair_ms=%u error=%d",
        recovered,writable,reasons,mount_ms,repair_ms,e);
    return e;
}
