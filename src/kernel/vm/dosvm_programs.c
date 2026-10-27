/* Native management of the fixed bundled-game directories. The FAT bytes are
 * modified only through the outer VFS C: write gate, never a VM's private COW.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <ciuki/kernel.h>
#include <ciuki/abi.h>
#include <ciuki/cpu.h>
#include <ciuki/storage.h>
#include <ciuki/task.h>
#include <ciuki/sha256.h>
#include "../fs/program_tree.h"
#include "../fs/partition.h"
int dosvm_game_remove_reserve(unsigned);
void dosvm_game_remove_release(unsigned);
#define GAME_SECTORS 262144u
/* fx-4b: removal is the kernel's only writer of UNIFIED.IMG. It stops a
 * background verification that holds the image open, deletes the verified
 * stamp before the first destructive write and re-arms verification. */
extern void dosvm_image_hold(void) __attribute__((weak));
extern void dosvm_image_release(void) __attribute__((weak));
#define IMAGE_STAMP "C:\\SYSTEM\\DOS\\UNIFIED.VFY"
void dosvm_game_digest_invalidate(void);
/* One serialized owner-side mount, not one resident cache per operation. */
static struct {
    struct vfs_table table;
    struct fat_volume fat;
    struct blkdev device;
    struct partition_table parts;
    int fd;
    uint32_t sectors;
    uint8_t hash_buffer[4096];
    bool busy;
} owner;
static int image_read(struct blkdev *d,uint64_t lba,uint32_t count,void *out)
{
    int e=blkdev_range(d,lba,count);if(e)return e;
    memset(out,0,(size_t)count*512u);
    if(lba>=owner.sectors)return 0;
    if(count>owner.sectors-lba)count=owner.sectors-(uint32_t)lba;
    uint64_t position;
    size_t got=0; e=vfs_seek(&owner.table,owner.fd,(int64_t)(lba*512u),0,&position);
    if(!e)e=vfs_read(&owner.table,owner.fd,out,(size_t)count*512u,&got);
    return e ? e : got==(size_t)count*512u ? 0 : -FS_EIO;
}
static int image_write(struct blkdev *d,uint64_t lba,uint32_t count,const void *in)
{
    int e=blkdev_range(d,lba,count);if(e)return e;
    /* Removal cannot allocate beyond the installed image's nonzero prefix. */
    if(lba>=owner.sectors || count>owner.sectors-lba)return -FS_ENOSPC;
    uint64_t position;
    size_t done=0;e=vfs_seek(&owner.table,owner.fd,(int64_t)(lba*512u),0,&position);
    if(!e)e=vfs_write(&owner.table,owner.fd,in,(size_t)count*512u,&done);
    return e ? e : done==(size_t)count*512u ? 0 : -FS_EIO;
}
static int image_flush(struct blkdev *d)
{(void)d;return vfs_commit(&owner.table,owner.fd);}
static int remove_overlays(unsigned game)
{
    char pattern[80];
    struct vfs_find find;struct fat_entry entry;
    /* Restart after deletion: directory indices may be reused. */
    for (unsigned pass=0;pass<2;pass++) {
        const char *directory=pass ? "C:\\SYSTEM\\DOS\\SAVES" : "C:\\SYSTEM\\DOS";
        ksnprintf(pattern,sizeof(pattern),"%s\\G%uK*.COW",directory,game);
        for(unsigned budget=0;;budget++) {
            int e=vfs_find_first(&owner.table,pattern,0,&find,&entry);
            if(e==-FS_ENOENT)break;
            if(e)return e;
            if(budget==4096)return -FS_ELOOP;
            char path[FS_PATH_BYTES];ksnprintf(path,sizeof(path),"%s\\%s",directory,entry.name);
            e=vfs_delete(&owner.table,path);if(e)return e;
        }
    }
    return 0;
}
/* Authorized base mutation publishes a fresh full-file digest before another
 * launch. A failed update leaves the old sidecar, so launch fails closed. */
static int update_identity(void)
{
    struct sha256_ctx ctx; uint8_t raw[32]; char text[65];
    uint64_t pos=0; int e=vfs_seek(&owner.table,owner.fd,0,VFS_SEEK_SET,&pos);
    sha256_init(&ctx);
    for (uint32_t at=0;!e && at<owner.sectors*512u;) {
        size_t n=owner.sectors*512u-at, got=0;
        if (n>sizeof(owner.hash_buffer)) n=sizeof(owner.hash_buffer);
        e=vfs_read(&owner.table,owner.fd,owner.hash_buffer,n,&got);
        if (!e && got!=n) e=-FS_EIO;
        if (!e) { sha256_update(&ctx,owner.hash_buffer,n); at+=(uint32_t)n; kpreempt_point(); }
    }
    if (e) return e;
    sha256_final(&ctx,raw); sha256_hex(raw,text); text[64]='\n';
    int h=vfs_open(&owner.table,"C:\\SYSTEM\\DOS\\UNIFIED.SHA",VFS_WRITE|VFS_TRUNCATE,VFS_DENY_ALL,0);
    if (h<0) return h;
    size_t done=0; e=vfs_write(&owner.table,h,text,sizeof(text),&done);
    if (!e && done!=sizeof(text)) e=-FS_EIO;
    if (!e) e=vfs_commit(&owner.table,h);
    int closed=vfs_close(&owner.table,h); return e ? e : closed;
}
int dosvm_game_program_checked(unsigned game,bool remove,uint32_t *size,const char **step,const char **detail);
int dosvm_game_program(unsigned game,bool remove,uint32_t *size)
{ const char *step,*detail; return dosvm_game_program_checked(game,remove,size,&step,&detail); }
/* step names the first operation that failed and detail the FAT mount
 * diagnostic, both static strings, for the launch refusal record (fx-4). */
int dosvm_game_program_checked(unsigned game,bool remove,uint32_t *size,const char **step,const char **detail)
{
    *step="request";*detail="none";
    if(!size || !game || game>CIUKI_DOSVM_GAME_COUNT)return -FS_EINVAL;
    *size=0;
    *step="reservation";
    int reservation=remove ? dosvm_game_remove_reserve(game) : 0;
    if(reservation)return reservation;
    *step="storage";
    struct storage *s=storage_get();if(!s || !s->ready || s->stopped){if(remove)dosvm_game_remove_release(game);return -FS_EIO;}
    /* Refuse competing requests; do not hold interrupt/preemption locks over
     * filesystem calls or scans. Exclusive file sharing serializes VM opens. */
    *step="owner-busy";
    uint32_t flags=irq_save();
    if(owner.busy){irq_restore(flags);if(remove)dosvm_game_remove_release(game);return -FS_EBUSY;}
    owner.busy=true;irq_restore(flags);
    if(remove && dosvm_image_hold)dosvm_image_hold();
    *step="table";
    int e=vfs_table_init(&s->vfs,&owner.table,2);
    bool table=!e;
    owner.fd=-1;
    char pending_path[64];ksnprintf(pending_path,sizeof(pending_path),"C:\\SYSTEM\\REMOVE%u.PND",game);
    struct fat_entry entry;
    bool pending=false;
    if(!e){
        *step="pending-marker";
        int pe=vfs_stat(&owner.table,pending_path,&entry);
        if(!pe)pending=true;else if(pe!=-FS_ENOENT)e=pe;
    }
    if(!e){*step="image-stat";e=vfs_stat(&owner.table,"C:\\SYSTEM\\DOS\\UNIFIED.IMG",&entry);}
    if(!e && ((entry.attr&FAT_ATTR_DIR) || entry.size%512u || entry.size/512u>GAME_SECTORS)){*step="image-size";e=-FS_EINVAL;}
    if(!e) {
        *step="image-open";
        owner.sectors=entry.size/512u;
        owner.fd=vfs_open(&owner.table,"C:\\SYSTEM\\DOS\\UNIFIED.IMG",
            remove ? VFS_READ|VFS_WRITE : VFS_READ,remove ? VFS_DENY_ALL : VFS_DENY_WRITE,0);
        if(owner.fd<0)e=owner.fd;
    }
    owner.device=(struct blkdev){.read=image_read,.write=image_write,.flush=image_flush,
        .capacity=GAME_SECTORS,.sector_size=512,.logical=true,.write_cache_state=BLKDEV_CACHE_ENABLED};
    bool mounted=false;
    if(!e) {
        /* fx-4: the unified base is partitioned (reference MBR: one active
         * 0Eh FAT16 partition at LBA 63, scripts/dos_conformance.py). The
         * former test accepted only type 06h, mounted the MBR itself as a
         * BPB and failed every game launch with EINVAL, reported as EIO
         * (T23 trial 9a60215f). Address the volume DOS mounts as C:, as the
         * VM's own view does; a superfloppy keeps the whole-image view. */
        uint64_t start=0,count=GAME_SECTORS;
        *step="partition";
        if(!partition_scan(&owner.device,&owner.parts)) {
            const struct partition *p=partition_dos_primary(&owner.parts);
            if(p){start=p->start;count=p->count;}
        }
        *step="mount";
        e=fat_prepare_removable(&owner.fat,&s->cache,&owner.device,start,count);
        if(e && owner.fat.diagnostic)*detail=owner.fat.diagnostic;
        mounted=!e;
    }
    /* Prove traversal bounds and ownership before deleting any entry. */
    if(!e){*step="program-tree";e=program_tree(&owner.fat,game,false,size);}
    /* An absent directory after a committed deletion still owns cleanup. */
    bool absent=e==-FS_ENOENT && mounted;
    if(absent && (remove || pending))e=0;
    if(!e && remove) {
        /* Commit identity before the first destructive write. Existence is
         * the record; fixed filename supplies the only permitted game ID. */
        *step="pending-write";
        int marker=vfs_open(&owner.table,pending_path,VFS_WRITE|VFS_CREATE,VFS_DENY_ALL,0);
        if(marker<0)e=marker;
        else {e=vfs_commit(&owner.table,marker);int ce=vfs_close(&owner.table,marker);if(!e)e=ce;}
        /* The committed marker already voids the stamp; delete it too. */
        if(!e)(void)vfs_delete(&owner.table,IMAGE_STAMP);
    }
    if(!e && remove && !absent) {
        *step="remove";
        e=fat_enable_write(&owner.fat);
        if(!e)e=program_tree(&owner.fat,game,true,size);
        dosvm_game_digest_invalidate(); /* also after a partially issued write */
    }
    if(mounted){int close_error=fat_unmount(&owner.fat);if(!e && close_error){*step="unmount";e=close_error;}}
    if(!e && remove){*step="identity";e=update_identity();}
    if(!e && remove){*step="overlays";e=remove_overlays(game);}
    if(owner.fd>=0){int close_error=vfs_close(&owner.table,owner.fd);if(!e && close_error){*step="image-close";e=close_error;}}
    if(!e)*step="none";
    if(table)vfs_table_destroy(&owner.table);
    flags=irq_save();owner.busy=false;irq_restore(flags);
    if(remove && dosvm_image_release)dosvm_image_release();
    if(remove)dosvm_game_remove_release(game);
    return !e && !remove && pending ? 1 : e;
}
