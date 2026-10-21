/* Native management of the fixed bundled-game directories. The FAT bytes are
 * modified only through the outer VFS C: write gate, never a VM's private COW.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/cpu.h>
#include <ciuki/storage.h>
#include <ciuki/task.h>
#include "../fs/program_tree.h"
#define GAME_SECTORS 262144u
void dosvm_game_digest_invalidate(void);
/* One serialized owner-side mount, not one resident cache per operation. */
static struct {
    struct vfs_table table;
    struct fat_volume fat;
    struct blkdev device;
    int fd;
    uint32_t sectors;
    bool busy;
} owner;
static int image_read(struct blkdev *d,uint64_t lba,uint32_t count,void *out)
{
    int e=blkdev_range(d,lba,count);if(e)return e;
    memset(out,0,(size_t)count*512u);
    if(lba>=owner.sectors)return 0;
    if(count>owner.sectors-lba)count=owner.sectors-(uint32_t)lba;
    size_t got=0; e=vfs_seek(&owner.table,owner.fd,(int64_t)(lba*512u),0,0);
    if(!e)e=vfs_read(&owner.table,owner.fd,out,(size_t)count*512u,&got);
    return e ? e : got==(size_t)count*512u ? 0 : -FS_EIO;
}
static int image_write(struct blkdev *d,uint64_t lba,uint32_t count,const void *in)
{
    int e=blkdev_range(d,lba,count);if(e)return e;
    /* Removal cannot allocate beyond the installed image's nonzero prefix. */
    if(lba>=owner.sectors || count>owner.sectors-lba)return -FS_ENOSPC;
    size_t done=0;e=vfs_seek(&owner.table,owner.fd,(int64_t)(lba*512u),0,0);
    if(!e)e=vfs_write(&owner.table,owner.fd,in,(size_t)count*512u,&done);
    return e ? e : done==(size_t)count*512u ? 0 : -FS_EIO;
}
static int image_flush(struct blkdev *d)
{(void)d;return vfs_commit(&owner.table,owner.fd);}
static int remove_overlays(unsigned game)
{
    char pattern[64];ksnprintf(pattern,sizeof(pattern),"C:\\SYSTEM\\DOS\\G%uK*.COW",game);
    struct vfs_find find;struct fat_entry entry;
    /* Restart after deletion: directory indices may be reused. */
    for(unsigned budget=0;budget<4096;budget++) {
        int e=vfs_find_first(&owner.table,pattern,0,&find,&entry);
        if(e==-FS_ENOENT)return 0;
        if(e)return e;
        char path[FS_PATH_BYTES];ksnprintf(path,sizeof(path),"C:\\SYSTEM\\DOS\\%s",entry.name);
        e=vfs_delete(&owner.table,path);if(e)return e;
    }
    return -FS_ELOOP;
}
int dosvm_game_program(unsigned game,bool remove,uint32_t *size)
{
    if(!size || !game || game>3)return -FS_EINVAL;
    *size=0;
    struct storage *s=storage_get();if(!s || !s->ready || s->stopped)return -FS_EIO;
    /* Refuse competing requests; do not hold interrupt/preemption locks over
     * filesystem calls or scans. Exclusive file sharing serializes VM opens. */
    uint32_t flags=irq_save();
    if(owner.busy){irq_restore(flags);return -FS_EBUSY;}
    owner.busy=true;irq_restore(flags);
    int e=vfs_table_init(&s->vfs,&owner.table,2);
    bool table=!e;
    owner.fd=-1;
    struct fat_entry entry;
    if(!e)e=vfs_stat(&owner.table,"C:\\GAMES\\CIUKIGAM.IMG",&entry);
    if(!e && ((entry.attr&FAT_ATTR_DIR) || entry.size%512u || entry.size/512u>GAME_SECTORS))e=-FS_EINVAL;
    if(!e) {
        owner.sectors=entry.size/512u;
        owner.fd=vfs_open(&owner.table,"C:\\GAMES\\CIUKIGAM.IMG",
            remove ? VFS_READ|VFS_WRITE : VFS_READ,remove ? VFS_DENY_ALL : VFS_DENY_WRITE,0);
        if(owner.fd<0)e=owner.fd;
    }
    owner.device=(struct blkdev){.read=image_read,.write=image_write,.flush=image_flush,
        .capacity=GAME_SECTORS,.sector_size=512,.logical=true,.write_cache_state=BLKDEV_CACHE_ENABLED};
    bool mounted=false;
    if(!e) {
        e=fat_prepare_removable(&owner.fat,&s->cache,&owner.device,0,GAME_SECTORS);
        mounted=!e;
    }
    /* Prove traversal bounds and ownership before deleting any entry. */
    if(!e)e=program_tree(&owner.fat,game,false,size);
    if(!e && remove) {
        e=fat_enable_write(&owner.fat);
        if(!e)e=program_tree(&owner.fat,game,true,size);
        dosvm_game_digest_invalidate(); /* also after a partially issued write */
    }
    if(mounted){int close_error=fat_unmount(&owner.fat);if(!e)e=close_error;}
    if(!e && remove)e=remove_overlays(game);
    if(owner.fd>=0){int close_error=vfs_close(&owner.table,owner.fd);if(!e)e=close_error;}
    if(table)vfs_table_destroy(&owner.table);
    flags=irq_save();owner.busy=false;irq_restore(flags);
    return e;
}
