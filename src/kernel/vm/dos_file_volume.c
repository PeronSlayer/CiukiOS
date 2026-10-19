/* SPDX-License-Identifier: GPL-2.0-only */
#include "dos_file_volume.h"
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
int dos_file_volume_raw(struct dos_file_volume *v,uint32_t lba,void *sector,bool write,const struct vfs_request *request) {
    if (!v->mounted || blkdev_range(v->provider,lba,1)) return -FS_EINVAL;
    int e=vfs_admit(&v->space,1u<<2,request); if (e) return e;
    if (write && v->fat.readonly) e=-FS_EROFS;
    else e=write ? VFS_IO(&v->space,v->provider->write(v->provider,lba,1,sector)) :
                   VFS_IO(&v->space,v->provider->read(v->provider,lba,1,sector));
    if (!e && write) {
        /* FAT has no private chain cache. Refresh live metadata with its own
         * parser after raw writes; no stale first-cluster/size/write cursor.
         * Directory identity changes invalidate the whole mount lifetime. */
        for (unsigned i=0;i<VFS_NODES;i++) {
            struct vfs_node *n=&v->space.nodes[i];
            if (!n->refs || n->volume!=&v->fat) continue;
            struct fat_entry fresh;
            int found=VFS_IO(&v->space,fat_lookup(&v->fat,n->entry.parent,n->entry.alias[0] ? n->entry.alias : n->entry.name,&fresh));
            if (found || fresh.index!=n->entry.index) { v->space.generation[2]++; break; }
            n->entry=fresh;
        }
    }
    vfs_unlock(&v->space); return e;
}
