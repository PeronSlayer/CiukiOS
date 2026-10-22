/* SPDX-License-Identifier: GPL-2.0-only */
#include "dos_file_volume.h"
#include <ciuki/vfs_hooks.h>
int dos_file_volume_open(struct dos_file_volume *v,struct block_cache *cache,struct blkdev *provider,bool writable) {
    if (!provider || !provider->logical || provider->sector_size!=512) return -FS_EINVAL;
    memset(v,0,sizeof(*v)); v->provider=provider;
    int e=fat_mount(&v->fat,cache,provider,0,provider->capacity,writable);
    if (e) return e;
    vfs_init(&v->space); v->mounted=true;
    e=vfs_attach(&v->space,2,&v->fat);
    if (!e) e=vfs_table_init(&v->space,&v->table,2);
    if (e) { (void)dos_file_volume_close(v); return e; }
    v->table_ready=true; return 0;
}
int dos_file_volume_close(struct dos_file_volume *v) {
    if (!v->mounted) return 0;
    if (v->table_ready) { vfs_table_destroy(&v->table); v->table_ready=false; }
    int e=vfs_detach(&v->space,2);
    if (e) return e;
    vfs_destroy(&v->space); v->mounted=false; return 0;
}
int dos_file_volume_discard(struct dos_file_volume *v) {
    if (!v->mounted) return 0;
    if (v->table_ready || !v->provider->logical || !v->fat.readonly) return -FS_EBUSY;
    struct vfs *space=&v->space; vfs_lock(space,0);
    int e=0;
    if (px_volume_busy && px_volume_busy(space,&v->fat)) e=-FS_EBUSY;
    if (space->reserved || space->mutating || space->waiters || space->transaction_users[2]) e=-FS_EBUSY;
    for (unsigned i=0;i<26;i++)
        if (space->readers[i] || (i!=2 && (space->volumes[i] || space->ro_volumes[i]))) e=-FS_EBUSY;
    for (unsigned i=0;i<VFS_TABLES;i++) if (space->tables[i]) e=-FS_EBUSY;
    for (unsigned i=0;i<VFS_NODES;i++) if (space->nodes[i].refs) e=-FS_EBUSY;
    for (unsigned i=0;i<VFS_DESCRIPTIONS;i++) if (space->descriptions[i].refs) e=-FS_EBUSY;
    for (unsigned i=0;i<VFS_RANGE_LOCKS;i++) if (space->range_locks[i].description) e=-FS_EBUSY;
    if (vfs_identity_busy(space,&v->fat)) e=-FS_EBUSY;
    if (!e) {
        /* Only this VM's logical cache identity can be forgotten. Its outer
         * physical volumes/cache errors remain owned by native storage. */
        cache_invalidate(v->fat.cache,v->provider,true);
        vfs_identity_expire(space,&v->fat);
        v->fat.identity_changed=0; v->fat.identity_ctx=0;
        v->fat.io_window=0; v->fat.io_window_ctx=0;
        space->volumes[2]=0; space->generation[2]++;
        v->fat.mounted=false;
    }
    vfs_unlock(space);
    if (!e) { vfs_destroy(space); v->mounted=false; }
    return e;
}
struct raw_sector { struct blkdev *provider; uint32_t lba; void *sector; bool write; };
static int raw_sector(void *arg) {
    struct raw_sector *r=arg;
    return r->write ? blkdev_write(r->provider,r->lba,1,r->sector) : blkdev_read(r->provider,r->lba,1,r->sector);
}
/* f1-93: the provider call is a device window of the logical volume: the
 * VM's volume guard is not held across the outer image I/O. A raw write
 * changes FAT state underneath the parser, so it takes the writer turn
 * like a FAT mutation and is announced to concurrent readers. */
static int volume_raw(struct dos_file_volume *v,uint32_t lba,void *sector,bool write,const struct vfs_request *request) {
    if (!v->mounted || blkdev_range(v->provider,lba,1)) return -FS_EINVAL;
    int e=vfs_admit(&v->space,1u<<2,request); if (e) return e;
    uint32_t turn=0;
    if (write && v->fat.readonly) e=-FS_EROFS;
    else if (write && (e=vfs_turn_take_locked(&v->space,1u<<2,&turn))) { }
    else if (!(v->space.active&(1u<<2)) || v->space.volumes[2]!=&v->fat) e=-FS_EIO;
    else {
        struct raw_sector call={v->provider,lba,sector,write};
        e=VFS_IO_MODE(&v->space,VFS_WINDOW_FREE,vfs_io_window_call(&v->space,&v->fat,write,raw_sector,&call));
    }
    if (!e && write) {
        /* FAT has no private chain cache. Refresh live metadata with its own
         * parser after raw writes; no stale first-cluster/size/write cursor.
         * Directory identity changes invalidate the whole mount lifetime. */
        for (unsigned i=0;i<VFS_NODES;i++) {
            struct vfs_node *n=&v->space.nodes[i];
            if (!n->refs || n->volume!=&v->fat) continue;
            struct fat_entry fresh;
            int found=vfs_dir_step_locked(&v->space,&v->fat,n->entry.parent,n->entry.alias[0] ? n->entry.alias : n->entry.name,0,&fresh,false);
            if (found || fresh.index!=n->entry.index) { v->space.generation[2]++; break; }
            n->entry=fresh;
        }
    }
    vfs_turn_drop_locked(&v->space,turn);
    vfs_unlock(&v->space); return e;
}

int dos_file_volume_raw(struct dos_file_volume *v,uint32_t lba,void *sector,bool write,const struct vfs_request *request) {
    struct fs_request_scope scope;
    fs_request_enter(&scope,request ? request : fs_request_current());
    int e=volume_raw(v,lba,sector,write,request);
    fs_request_leave(&scope); return e;
}
